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
    uint16_t rx_pos; /* 循环 DMA 里"已经交出去到哪了"（相对缓冲起点） */
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

/* ---- 接收侧诊断开关（限次）----
 * "HAL 之后补武装"那一行：HAL 的溢出路径会中止接收 DMA 且不重新武装，
 * 补上它才不会让接收永久停摆。查完把这个开关连同那几行一起删掉。 */
#define PL_UART_RX_DIAG 1
#if PL_UART_RX_DIAG
#define PLU_LOG(...) printf(__VA_ARGS__)
#else
#define PLU_LOG(...) ((void)0)
#endif

/* ---- 重新武装接收 DMA ----
 *
 * **返回值必须看**：重启失败 = 这一路**从此再也收不到任何东西**，而且一声不响。
 * 现象是"刚才还好好的，忽然就什么都收不到了"，然后重启才好 —— 与"线松了"
 * 在现场完全分不开。
 *
 * 传输层的接收是"空闲中断 → 停接收 DMA → 取走这一段 → 重新武装"，所以每收一段
 * 就走一次这个函数。任何一次没武装上，后面就全哑了。 */
static bool _rx_rearm(uart_ctx_t *ctx)
{
    /* 重新武装 = DMA 从缓冲起点重新转，已交付位置随之归零（错误路径里
       丢掉的字节只能丢，不能重放） */
    ctx->rx_pos = 0;

    if (HAL_UART_Receive_DMA(ctx->huart, ctx->rx_buf, ctx->rx_buf_size) == HAL_OK) return true;

    /* 兜一次：把接收 DMA 流与 UART 的接收状态都拽回可用再试。
       HAL_UART_AbortReceive 正常会把两者都复位；走到这儿说明有一步没成。 */
    HAL_DMA_Abort(ctx->huart->hdmarx);
    /* HAL_DMA_Abort 对**非 BUSY** 的流只返回错误、不改状态（中止路径留下的
       HAL_DMA_STATE_ABORT 正是这种），所以这里显式拽回 READY —— 否则永远武装不上。 */
    ctx->huart->hdmarx->State = HAL_DMA_STATE_READY;
    ctx->huart->RxState       = HAL_UART_STATE_READY;
    if (HAL_UART_Receive_DMA(ctx->huart, ctx->rx_buf, ctx->rx_buf_size) == HAL_OK) return true;

    printf("[pl_uart] **接收重启失败**（uart RxState=%u，RX DMA State=%u）—— "
           "本路从此收不到任何数据，直到复位\n",
           (unsigned)ctx->huart->RxState,
           (unsigned)(ctx->huart->hdmarx ? ctx->huart->hdmarx->State : 0xFFFFU));
    return false;
}

/* ---- 内部：UART 空闲中断处理 ----
 *
 * **接收 DMA 跑循环模式**，这里只做一件事：算出"从上次交出去到现在又收了多少"。
 *
 * 为什么不用"中止 → 取长度 → 重开"那个经典写法：中止窗口里到达的字节必然丢失，
 * 而且**一旦发生溢出（ORE），IDLE 位就不再置位** —— 空闲交付这条路彻底断掉，
 * 中断从此只被 EIE 驱动、空转，接收再也起不来。实测就是这样：突发帧期间一溢出，
 * 从卡收不到任何东西，直到十几秒后偶然恢复；全程零报错、CR3 也一切正常
 * （DMAR/EIE 都在），从寄存器上完全看不出问题。
 *
 * 循环模式没有中止动作，也就没有那个窗口。 */
static void uart_idle_handle(uart_ctx_t *ctx)
{
    if (!(__HAL_UART_GET_FLAG(ctx->huart, UART_FLAG_IDLE))) return;
    __HAL_UART_CLEAR_IDLEFLAG(ctx->huart);

    const uint16_t pos =
        (uint16_t)(ctx->rx_buf_size - __HAL_DMA_GET_COUNTER(ctx->huart->hdmarx));
    if (pos == ctx->rx_pos) return; /* 没有新数据 */

    if (pos > ctx->rx_pos) {
        const uint16_t n = (uint16_t)(pos - ctx->rx_pos);
        if (ctx->rx_cb) ctx->rx_cb(&ctx->rx_buf[ctx->rx_pos], n, ctx->rx_cb_ctx);
    } else {
        /* 绕过缓冲末尾：**分两段交**。协议侧的环形缓冲区本来就是流式的，
           一帧被拆成两次交付无妨（它按字节累积）。 */
        const uint16_t n1 = (uint16_t)(ctx->rx_buf_size - ctx->rx_pos);
        if (n1 && ctx->rx_cb) ctx->rx_cb(&ctx->rx_buf[ctx->rx_pos], n1, ctx->rx_cb_ctx);
        if (pos && ctx->rx_cb) ctx->rx_cb(ctx->rx_buf, pos, ctx->rx_cb_ctx);
    }
    ctx->rx_pos = pos;
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
        } else {
            /* **这一条必须报**：DMA 起不来时（gState 不是 READY、或 DMA 流被占）本函数
               原地返回 -1，而整条 ccb_send 链**没人看返回值**。表现是"帧根本没发出去"
               却一声不响 —— 与"发出去了但对端没收到"在现场完全分不开，而两者的排查
               方向相反（查本机状态机 vs 查线）。 */
            printf("[pl_uart] DMA 发送**没起来**（%u 字节）：uart gState=%u，TX DMA State=%u\n",
                   (unsigned)len, (unsigned)ctx->huart->gState,
                   (unsigned)(ctx->huart->hdmatx ? ctx->huart->hdmatx->State : 0xFFFFU));
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
    ctx->rx_pos      = 0;

    /* **循环模式**：DMA 一直转，空闲中断只用来算长度（见 uart_idle_handle 的说明）。
       在这里改而不是在板级 usart.c 里改 —— 那是 CubeMX 产物、用户手工维护，
       而"这一路接收用循环 DMA"是平台层的机制选择。 */
    ctx->huart->hdmarx->Init.Mode = DMA_CIRCULAR;

    if (!_rx_rearm(ctx)) return -1;

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

    /* ---- **空闲必须先于 HAL 处理** ----
     *
     * `HAL_UART_IRQHandler` 处理溢出等错误时走的是 `__HAL_UART_CLEAR_PEFLAG`，
     * 而那个宏是"**读 SR、再读 DR**"——按 RM0090，**该序列同时会清掉 IDLE 位**。
     * 于是顺序一反，`uart_idle_handle` 查不到 IDLE、静默返回，这一段的字节永不交付。
     *
     * 更糟的是 HAL 的 ORE 处理还会**中止接收 DMA**，且不会重新武装。两者叠加：
     * 突发帧期间一溢出，接收就死掉，直到某次空闲恰好没有伴随错误才活过来 ——
     * 实测表现为 247ms 到 21 秒不等、毫无规律的"处理慢"，而 P2 时期帧稀疏撞不上。
     *
     * 这一条不改"无条件清标志"的性质：HAL 照样无条件执行，只是挪到后面。
     * 初始化期 ctx 还是空的，这里照旧跳过（HAL 那一步会清掉标志，不会成风暴）。 */
    /* ---- 诊断：ISR 入口的寄存器快照（限次）----
     * 只在"看起来不对"时打：有错误位、或接收 DMA 请求不在了。
     * SR 位（F4）：PE(0) FE(1) NE(2) ORE(3) IDLE(4) RXNE(5) TC(6) TXE(7)
     * CR3 位：EIE(0) DMAR(6)  CR1 位：RE(2) RXNEIE(5) IDLEIE(4) */
    static uint8_t s_isr_diag;
    if (s_isr_diag < 16U) {
        const uint32_t sr = h->Instance->SR;
        if ((sr & 0x0FU) || !(h->Instance->CR3 & USART_CR3_DMAR)) {
            s_isr_diag++;
            printf("[%8u] [pl_uart] ISR SR=%04X CR3=%04X CR1=%04X RxState=%u DMAState=%u\n",
                   (unsigned)osKernelGetTickCount(), (unsigned)(sr & 0xFFFFU),
                   (unsigned)(h->Instance->CR3 & 0xFFFFU), (unsigned)(h->Instance->CR1 & 0xFFFFU),
                   (unsigned)(g_uart_ctx[id].huart ? h->RxState : 0xEEU),
                   (unsigned)(h->hdmarx ? h->hdmarx->State : 0xFFU));
        }
    }

    if (g_uart_ctx[id].huart) uart_idle_handle(&g_uart_ctx[id]);

    /* ---- **自己清掉错误标志，不让 HAL 的错误路径跑起来** ----
     *
     * HAL 处理溢出等错误时会**中止接收 DMA**，并把它留在 `HAL_DMA_STATE_ABORT` 上；
     * 而 `HAL_DMA_Abort` 对非 BUSY 的流只返回错误、**不改状态** —— 于是这一路
     * **再也武装不回来**，永久哑掉。实测原话：
     *     **接收重启失败**（uart RxState=32，RX DMA State=5）—— 本路从此收不到任何数据
     *
     * 而溢出本身根本不该是致命的：丢一个字节，帧的 CRC 会拒掉它，协议层重新同步；
     * 整路停摆才是灾难。所以在这里把错误标志清掉（读 SR + 读 DR），HAL 就看不到错误，
     * 也就不会去动 DMA。
     *
     * **只在真的出错时才清** —— 那个序列会读走 DR，正常情况下会从 DMA 嘴里抢走一个字节。 */
    if (__HAL_UART_GET_FLAG(h, UART_FLAG_ORE) || __HAL_UART_GET_FLAG(h, UART_FLAG_FE) ||
        __HAL_UART_GET_FLAG(h, UART_FLAG_NE) || __HAL_UART_GET_FLAG(h, UART_FLAG_PE)) {
        __HAL_UART_CLEAR_PEFLAG(h);
    }

    HAL_UART_IRQHandler(h);

    /* HAL 可能刚把接收 DMA 中止掉（错误路径）。**DMA 的接收请求还在不在**是判断依据；
     * 不在就武装回去 —— 否则接收就此停摆，而这一点不会有任何报错。 */
    if (g_uart_ctx[id].huart && !(h->Instance->CR3 & USART_CR3_DMAR)) {
        static uint8_t n;
        if (n < 8) {
            n++;
            PLU_LOG("[pl_uart] HAL 之后接收 DMA 已不在，补武装（第 %u 次）\n", (unsigned)n);
        }
        (void)_rx_rearm(&g_uart_ctx[id]);
    }
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
