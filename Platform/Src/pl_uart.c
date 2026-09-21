/**
 * @file        pl_uart.c
 * @brief       UART 平台层抽象（hw_pl_initcall 优先级 3）
 *
 * 提供不透明句柄、阻塞发送和 DMA 空闲中断接收。
 *
 * 本文件只有机制，不含"本板有哪几路 UART"——那来自板级的 g_pl_uart_board[]
 * （见 boards/<板>/Src/pl_uart_board.c）。ISR 向量也在板级文件里，因为
 * "有哪些中断向量、哪个 DMA 流配哪一路"同样是板级事实。
 */

#include "pl_uart.h"
#include "initcall.h"
#include "pl_mem.h"
#include "cmsis_os2.h"
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    UART_HandleTypeDef *huart;
    pl_uart_rx_cb_t rx_cb;
    void *rx_cb_ctx;
    pl_uart_dir_fn_t dir_cb;   /* Device 层注入的 RS485 方向控制，无则为 NULL */
    uint8_t *rx_buf;
    uint16_t rx_buf_size;
    /* ---- TX 串行化与完成通知 ----
     * 锁是**两种发送模式共用**的（见 pl_uart_send_ex 的说明），不是 DMA 专用。 */
    osMutexId_t     tx_lock;
    osSemaphoreId_t tx_done; /* DMA 完成（TC）由中断 release，发送任务 acquire */
} uart_ctx_t;

static uart_ctx_t g_uart_ctx[PL_UART_MAX];

/* ---- 发送需要在 RTOS 起来之后建锁：hw_pl_initcall 阶段建内核对象会出事
 *      （见 Kernel/Inc/initcall.h 与 pl_tim.c 的说明）。sw_pl(1) 早于任何协议任务。 */
static void pl_uart_tx_sync_init(void)
{
    for (uint8_t i = 0; i < PL_UART_MAX; i++) {
        if (!g_pl_uart_board[i].huart) continue;

        const osMutexAttr_t mattr = {.name = "uart_tx", .attr_bits = osMutexPrioInherit};
        g_uart_ctx[i].tx_lock     = osMutexNew(&mattr);

        const osSemaphoreAttr_t sattr = {.name = "uart_tc"};
        g_uart_ctx[i].tx_done         = osSemaphoreNew(1, 0, &sattr);
    }
}
sw_pl_initcall(pl_uart_tx_sync_init);

/* ---- 本帧的物理发送时间（ms）----
 * 8N1 = 10 bit/字节。取整后 +2 兜住帧首帧尾与抢占。 */
static uint32_t _wire_ms(uint32_t baud, size_t len)
{
    if (baud == 0) return 100;
    return (uint32_t)(((uint64_t)len * 10U * 1000U) / baud) + 2U;
}

/* ---- 内部：UART 空闲中断处理 ---- */
static void uart_idle_handle(uart_ctx_t *ctx)
{
    if (!(__HAL_UART_GET_FLAG(ctx->huart, UART_FLAG_IDLE))) return;
    __HAL_UART_CLEAR_IDLEFLAG(ctx->huart);

    /* **只停接收，绝不能用 HAL_UART_DMAStop()** —— 那个函数在 gState 是 BUSY_TX 时
     * 会把**发送** DMA 一并中止（见 HAL 源码里那两段对称的 dmarequest 判断）。
     *
     * 而接收侧的这个中断随时可能落在一次发送进行中：半双工收发器切回接收那一刻，
     * 线路本来就是空闲的，IDLE 会立刻置位。此时若把发送 DMA 中止掉，这一帧就永远
     * 等不到 TC —— 表现为 `[pl_uart] DMA 发送超时（N 字节）`，丢的却是**与接收毫无
     * 关系的那一帧**，且丢哪一帧取决于时序，看上去随机。
     *
     * 症状第一次出现在级联开轮时：主卡发完 PING 紧接着就发 BEGIN，两帧首尾相接，
     * 接收侧的中断正好压在 BEGIN 的发送中间。此前 PING 是孤立的一帧，撞不上。 */
    HAL_UART_AbortReceive(ctx->huart);

    uint16_t len = ctx->rx_buf_size - __HAL_DMA_GET_COUNTER(ctx->huart->hdmarx);

    if (ctx->rx_cb && len > 0)
        ctx->rx_cb(ctx->rx_buf, len, ctx->rx_cb_ctx);

    HAL_UART_Receive_DMA(ctx->huart, ctx->rx_buf, ctx->rx_buf_size);
    __HAL_UART_ENABLE_IT(ctx->huart, UART_IT_IDLE);
}

/* ---- initcall ---- */
void pl_uart_init(void)
{
    for (uint8_t i = 0; i < PL_UART_MAX; i++) {
        if (g_pl_uart_board[i].init) g_pl_uart_board[i].init();
        g_uart_ctx[i].huart = (UART_HandleTypeDef *)g_pl_uart_board[i].huart;
    }
}
hw_pl_initcall(pl_uart_init); /* 优先级 2: 在 device 驱动之前 */

/* ---- 公开 API ---- */
pl_uart_handle_t pl_uart_get_handle(uint8_t id)
{
    return (id < PL_UART_MAX) ? &g_uart_ctx[id] : NULL;
}

int32_t pl_uart_send_ex(pl_uart_handle_t h, const uint8_t *buf, size_t len, uint32_t timeout_ms,
                        pl_uart_tx_mode_t mode)
{
    uart_ctx_t *ctx = (uart_ctx_t *)h;
    if (!ctx || !ctx->huart) return -1;
    if (!buf || len == 0) return 0;

    if (mode == PL_UART_TX_DMA) {
        /* DMA 够不到 CCMRAM（见 pl_mem.h）：用了不会报错，只是发出去的数据不对。 */
        if (!pl_mem_is_dma_capable(buf, len)) {
            printf("[pl_uart] DMA 发送缓冲落在 CCMRAM，DMA 不可达（buf=%p len=%u）\n", (void *)buf,
                   (unsigned)len);
            return -1;
        }
        if (!ctx->huart->hdmatx) {
            printf("[pl_uart] 本路未配 TX DMA，无法用 DMA 模式发送\n");
            return -1;
        }
    }

    /* 超时下限 = 本帧的物理发送时间。**两种模式都需要它**，只是用途不同：
     *   POLL：它是 HAL 的轮询预算 —— 不够会在帧中间超时返回，对端收到残帧
     *   DMA ：它是等 TC 信号量的上限 —— 不够会在还没发完时就中止 DMA
     * 共同点：都必须大于物理发送时间，而调用方很难正确估算（要知道波特率、10 bit/字节、
     * 还要留余量）。原实现把 100ms 写死在 app_rs485.c 里，@115200 只够 1152 字节，
     * IAP 的 1044 字节帧只剩 9ms 余量、级联的 1.4KB 帧必被截断。这里兜住。 */
    uint32_t need = _wire_ms(ctx->huart->Init.BaudRate, len);
    if (timeout_ms < need) timeout_ms = need;

    /* 两种模式共用同一把锁：都往同一条半双工总线上写。
       锁未建（RTOS 未就绪）时不加 —— 那是单线程启动期，不存在竞争。 */
    bool locked = false;
    if (ctx->tx_lock && osMutexAcquire(ctx->tx_lock, timeout_ms) == osOK) locked = true;

    int32_t ret = -1;

    if (ctx->dir_cb) ctx->dir_cb(true);

    if (mode == PL_UART_TX_DMA) {
        /* 清掉可能残留的完成计数：上一次若超时被中止，TC 仍可能随后触发一次 release，
           那个残留会让**本次**的 acquire 立刻返回、被误判成"发完了"。
           代价是一次非阻塞的 acquire，比发出去一帧假成功便宜得多。 */
        osSemaphoreAcquire(ctx->tx_done, 0);

        if (HAL_UART_Transmit_DMA(ctx->huart, (uint8_t *)buf, len) == HAL_OK) {
            /* 等到 TC：期间本任务阻塞在信号量上，CPU 交给别人跑（长帧下这是 DMA
               相对轮询的真正收益）。超时则中止 DMA —— 不中止的话下一帧会接在
               残帧后面发出去，对端永远收不到完整帧。 */
            if (osSemaphoreAcquire(ctx->tx_done, timeout_ms) == osOK) {
                ret = (int32_t)len;
            } else {
                HAL_UART_DMAStop(ctx->huart);
                printf("[pl_uart] DMA 发送超时（%u 字节 @%u baud）\n", (unsigned)len,
                       (unsigned)ctx->huart->Init.BaudRate);
            }
        }
    } else {
        ret = (HAL_UART_Transmit(ctx->huart, (uint8_t *)buf, len, timeout_ms) == HAL_OK)
                  ? (int32_t)len
                  : -1;
    }

    /* 无论成败都把总线切回接收，不把它留在发送态。DMA 模式走到这里时 TC 已发生
       （信号量就是 TC 中断给的），所以此刻切方向是正确时刻。 */
    if (ctx->dir_cb) ctx->dir_cb(false);

    if (locked) osMutexRelease(ctx->tx_lock);
    return ret;
}

int32_t pl_uart_send(pl_uart_handle_t h, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    return pl_uart_send_ex(h, buf, len, timeout_ms, PL_UART_TX_POLL);
}

int32_t pl_uart_send_dma(pl_uart_handle_t h, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    return pl_uart_send_ex(h, buf, len, timeout_ms, PL_UART_TX_DMA);
}

/* ---- TX 完成（TC）回调：由 USARTx_IRQHandler 在 TC 中断里调用 ----
 * HAL 的 DMA 发送序列是：DMA 搬完 → 开 TC 中断 → TC 到 → 这里。
 * 所以这一步等价于"最后一个字节已经移出移位寄存器"，正是能切方向的时刻。 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    for (uint8_t i = 0; i < PL_UART_MAX; i++) {
        if (g_uart_ctx[i].huart == huart && g_uart_ctx[i].tx_done) {
            osSemaphoreRelease(g_uart_ctx[i].tx_done);
            return;
        }
    }
}

void pl_uart_set_rx_cb(pl_uart_handle_t h, pl_uart_rx_cb_t cb, void *ctx_arg)
{
    uart_ctx_t *ctx = (uart_ctx_t *)h;
    if (ctx) {
        ctx->rx_cb     = cb;
        ctx->rx_cb_ctx = ctx_arg;
    }
}

void pl_uart_set_dir_cb(pl_uart_handle_t h, pl_uart_dir_fn_t cb)
{
    uart_ctx_t *ctx = (uart_ctx_t *)h;
    if (ctx) ctx->dir_cb = cb;
}

int32_t pl_uart_start_rx(pl_uart_handle_t h, uint8_t *buf, uint16_t len)
{
    uart_ctx_t *ctx = (uart_ctx_t *)h;
    if (!ctx || !ctx->huart) return -1;

    /* DMA 够不到 CCMRAM（见 pl_mem.h）：放进去不会报错，只是收到的数据不对。
       这里当场拦下。 */
    if (!pl_mem_is_dma_capable(buf, len)) {
        printf("[pl_uart] DMA 接收缓冲落在 CCMRAM，DMA 不可达（buf=%p len=%u）\n",
               (void *)buf, (unsigned)len);
        return -1;
    }

    ctx->rx_buf      = buf;
    ctx->rx_buf_size = len;

    HAL_StatusTypeDef st = HAL_UART_Receive_DMA(ctx->huart, buf, len);
    if (st != HAL_OK) return -1;

    __HAL_UART_ENABLE_IT(ctx->huart, UART_IT_IDLE);
    return 0;
}

/* ================================================================
 *  中断入口（由 boards/<板>/Src/pl_uart_board.c 里的向量调用）
 * ================================================================ */

void pl_uart_irq_handler(uint8_t id)
{
    if (id >= PL_UART_MAX) return;

    /* 取句柄走**板级常量表**，不走 g_uart_ctx（那是 initcall 期才填的）。
     * 窗口是真实存在的：MX_USARTx_UART_Init() 自己就会 HAL_NVIC_EnableIRQ，
     * 而 g_uart_ctx[i].huart 是在那之后才赋值 —— 这中间来一条 UART 中断，
     * 读运行时 ctx 就会既不处理、也不清标志，变成中断风暴。
     * TIM7 上已经因为同一类问题卡死过一次（见 pl_tim.c 的说明）。 */
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)g_pl_uart_board[id].huart;
    if (!h) return;

    HAL_UART_IRQHandler(h); /* 无条件清标志：这一步绝不能依赖初始化期数据 */

    /* 空闲中断的后续处理要用 ctx 里的 rx_buf/rx_cb，那些确实是运行期状态；
       初始化完成前 ctx 还是空的，此时跳过即可（标志上面已经清过）。 */
    if (g_uart_ctx[id].huart) uart_idle_handle(&g_uart_ctx[id]);
}

void pl_uart_dma_irq_handler(uint8_t id)
{
    if (id >= PL_UART_MAX) return;
    DMA_HandleTypeDef *hdma = (DMA_HandleTypeDef *)g_pl_uart_board[id].dma_rx;
    if (hdma) HAL_DMA_IRQHandler(hdma);
}

void pl_uart_dma_tx_irq_handler(uint8_t id)
{
    if (id >= PL_UART_MAX) return;
    /* 同样走板级常量表而不是 g_uart_ctx（理由同 pl_uart_irq_handler） */
    DMA_HandleTypeDef *hdma = (DMA_HandleTypeDef *)g_pl_uart_board[id].dma_tx;
    if (hdma) HAL_DMA_IRQHandler(hdma);
}
