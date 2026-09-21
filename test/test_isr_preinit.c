/**
 * @file    test_isr_preinit.c
 * @brief   ISR 在 initcall 跑之前必须也是安全的
 *
 * 为什么需要这个测试：**上机时 3833024 卡死在 TIM7 中断里**，根因就是 ISR 读了
 * initcall 期才填充的数据 ——
 *
 *     if (g_tim_handle[PL_TIM7])         // 此刻还是 NULL
 *         HAL_TIM_IRQHandler(...);       // 于是标志永不清除
 *
 * （上面用 // 而非块注释：C 的块注释不嵌套，内层的结束符会把外层提前终止，
 *   本文件第一版就是这么被编译器和 clangd 同时报错的。）
 *
 * g_tim_handle[] 在 pl_tim_init（hw_pl_initcall）里填，而 **TIM7 是 HAL 时基，
 * 由 HAL_Init() → HAL_InitTick() 启动，早于 initcall_run()**。第一次触发时句柄
 * 是 NULL，ISR 什么都不做 → 更新标志不清 → 立刻重入 → CPU 再也出不来，
 * pl_tim_init 根本没机会跑。
 *
 * 这个顺序在 host 上不会自然出现（不跑 initcall），所以这里**手工构造**它：
 * 板级表 g_pl_tim_board[] 是 const、编译期初始化，测试直接调用各 ISR，
 * **全程不调 pl_tim_init()**。断言的是"ISR 确实去清了中断源"，
 * 也就是 HAL_xxx_IRQHandler 真的被调用到了 —— 在真机上，没被调用就是风暴。
 *
 * 反向验证：把 pl_tim.c 改回"ISR 读一个 initcall 期填充的运行时数组"，本用例立刻红。
 *
 * 同一个类在 pl_uart.c 也修过（MX_USARTx_UART_Init 自己就会 HAL_NVIC_EnableIRQ，
 * 而 ctx 里的 huart 在那之后才赋值），一并守着。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pl_tim.h"
#include "pl_uart.h"
/* 解析到 test/stubs/stm32f4xx_hal.h（-I test/stubs 排在最前）—— 上面两个头不拉 HAL，
   但本文件的替身实现要用到 HAL 的类型形状。 */
#include "stm32f4xx_hal.h"

/* ISR 与 HAL 回调是链接期符号，没有头文件声明它们（真机上由向量表引用），
   这里显式声明以便直接调用。 */
void TIM2_IRQHandler(void);
void TIM3_IRQHandler(void);
void TIM4_IRQHandler(void);
void TIM7_IRQHandler(void);
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim);

/* ---- HAL 替身的实现（stub 头只给形状） ---- */

TIM_TypeDef   s_fake_tim2, s_fake_tim3, s_fake_tim4, s_fake_tim5, s_fake_tim6, s_fake_tim7;
USART_TypeDef s_fake_usart1;
uint32_t      s_fake_reg;

static int              s_tim_irq_calls;
static TIM_HandleTypeDef *s_last_tim;
static int              s_uart_irq_calls;
static UART_HandleTypeDef *s_last_uart;
static int              s_inc_tick_calls;

/* pl_mem_is_dma_capable 要读的段边界，由链接脚本在真机上定义（见 Compiler/sections.ld）。
   host 上没有链接脚本，给它一对空区间：任何缓冲都不落在 CCMRAM 里，
   于是 DMA 可达性判断恒为真 —— 本套件不测那段。 */
uint8_t _sccmram[1];
uint8_t _eccmram[1];

void HAL_TIM_IRQHandler(TIM_HandleTypeDef *htim)
{
    s_tim_irq_calls++;
    s_last_tim = htim;
}
void HAL_TIM_Base_Start_IT(TIM_HandleTypeDef *htim) { (void)htim; }
void HAL_IncTick(void) { s_inc_tick_calls++; }

void HAL_UART_IRQHandler(UART_HandleTypeDef *huart)
{
    s_uart_irq_calls++;
    s_last_uart = huart;
}
/* 记下调用方传进来的超时，供"发送超时下限"用例断言 */
static uint32_t s_last_tx_timeout;
static uint16_t s_last_tx_len;

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *d, uint16_t l, uint32_t t)
{
    (void)h;
    (void)d;
    s_last_tx_timeout = t;
    s_last_tx_len     = l;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_Receive_DMA(UART_HandleTypeDef *h, uint8_t *d, uint16_t l)
{
    (void)h; (void)d; (void)l;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *h, uint8_t *d, uint16_t l)
{
    (void)h; (void)d; (void)l;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_DMAStop(UART_HandleTypeDef *h)
{
    (void)h;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *h)
{
    (void)h;
    return HAL_OK;
}
void HAL_DMA_IRQHandler(DMA_HandleTypeDef *hdma) { (void)hdma; }
void NVIC_DisableIRQ(IRQn_Type irq) { (void)irq; }
void NVIC_EnableIRQ(IRQn_Type irq) { (void)irq; }

/* ---- 板级表：测试提供。
 *      注意这**不是**在测板级文件，是给它一份最小可用的表让被测机制能跑。
 *      init 全为空 —— 我们刻意不跑任何初始化。 ---- */

static TIM_HandleTypeDef  s_htim2 = {.Instance = TIM2};
static TIM_HandleTypeDef  s_htim3 = {.Instance = TIM3};
static TIM_HandleTypeDef  s_htim4 = {.Instance = TIM4};
static TIM_HandleTypeDef  s_htim7 = {.Instance = TIM7};
/* BaudRate 要给真值：pl_uart 的发送超时下限按它算 */
static UART_HandleTypeDef s_huart1 = {.Instance = &s_fake_usart1, .Init = {.BaudRate = 115200}};

/* 角色统一后两块板都不再有 TIM2 条目，这里照实反映：PL_TIM2 留空，
   它的 ISR 在没有句柄时什么都不做（NVIC 也不会使能，不会真的进来）。 */
const pl_tim_board_entry_t g_pl_tim_board[PL_TIM_MAX] = {
    [PL_TIM3] = {.init = NULL, .handle = &s_htim3, .irq = 0},
    [PL_TIM4] = {.init = NULL, .handle = &s_htim4, .irq = 0},
    [PL_TIM7] = {.init = NULL, .handle = &s_htim7, .irq = 0},
};

const pl_uart_board_entry_t g_pl_uart_board[PL_UART_MAX] = {
    [PL_UART1] = {.init = NULL, .huart = &s_huart1, .dma_rx = NULL, .irq = 0, .dma_irq = 0},
};

/* ---- 断言 ---- */

static int g_pass;
static int g_fail;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("      \033[31m✘\033[0m %s:%d  %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                                          \
    } while (0)

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            printf("      \033[31m✘\033[0m %s:%d  ", __FILE__, __LINE__);                           \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

#define TEST_BEGIN(name) printf("\n\033[36m▶ %s\033[0m\n", name)

/* ================================================================
 *  用例 —— 全程不调 pl_tim_init() / pl_uart_init()
 * ================================================================ */

static void case_tim_isrs_before_init(void)
{
    TEST_BEGIN("initcall 之前：各 TIM ISR 都要真的去清中断源");

    s_tim_irq_calls = 0;

    TIM3_IRQHandler();
    CHECK_MSG(s_tim_irq_calls == 1 && s_last_tim == &s_htim3, "TIM3_IRQHandler 没把中断交给 HAL");
    TIM4_IRQHandler();
    CHECK_MSG(s_tim_irq_calls == 2 && s_last_tim == &s_htim4, "TIM4_IRQHandler 没把中断交给 HAL");

    /* TIM7 是真正的受害者：它是 HAL 时基，HAL_Init 阶段就被使能了 */
    TIM7_IRQHandler();
    CHECK_MSG(s_tim_irq_calls == 3 && s_last_tim == &s_htim7,
              "TIM7_IRQHandler 没把中断交给 HAL —— 真机上就是中断风暴、CPU 卡死在这里");

    /* TIM2 统一后不再使用：没有板级条目，ISR 应安静返回（不清标志也不该被解引用崩溃） */
    TIM2_IRQHandler();
    CHECK_MSG(s_tim_irq_calls == 3, "TIM2 已不用，它的 ISR 不该去碰 HAL");
}

static void case_tim7_tick_before_init(void)
{
    TEST_BEGIN("initcall 之前：TIM7 的周期回调仍要喂 HAL tick");

    s_inc_tick_calls = 0;
    HAL_TIM_PeriodElapsedCallback(&s_htim7);
    CHECK_MSG(s_inc_tick_calls == 1, "TIM7 周期回调没喂 tick —— HAL 时基会停摆");
}

static void case_uart_isr_before_init(void)
{
    TEST_BEGIN("initcall 之前：UART ISR 也要真的去清中断源");

    s_uart_irq_calls = 0;
    pl_uart_irq_handler(PL_UART1);
    CHECK_MSG(s_uart_irq_calls == 1 && s_last_uart == &s_huart1,
              "pl_uart_irq_handler 读的是 initcall 期填充的 ctx，此窗口内不清标志 → 风暴");

    /* 越界与空槽位不能崩 */
    s_uart_irq_calls = 0;
    pl_uart_irq_handler(PL_UART_MAX);
    CHECK(s_uart_irq_calls == 0);
}

/* ================================================================
 *  发送超时下限（与 initcall 无关，但同在一个 TU 里，顺带守着）
 *
 *  原来的实现把 100ms 写死在 app_rs485.c 里，而 @115200 8N1 下 100ms 只够 1152 字节：
 *  IAP 的 1044 字节帧只剩 9ms 余量，级联的 1.4KB 位图帧必被 HAL 中途截断、且因为
 *  ccb_send 不检查返回值而**完全静默**。修法是 pl_uart 替调用方兜一个按波特率算的下限。
 *
 *  这条用例钉住的是"下限确实被抬上去了"—— 它是纯算术，真机验证只能间接看到
 *  （帧发出去了），而它被"简化"掉时不会有任何东西报警。
 * ================================================================ */

static void case_tx_timeout_floor(void)
{
    TEST_BEGIN("发送超时 < 本帧物理时间时，必须被抬到物理时间");

    pl_uart_init(); /* 只为填 ctx->huart；本套件其余用例刻意不调它 */

    static uint8_t buf[1400];
    pl_uart_handle_t h = pl_uart_get_handle(PL_UART1);

    /* 122ms 是 1400 字节 @115200 8N1（10 bit/字节）的物理时间 */
    pl_uart_send(h, buf, sizeof(buf), 50);
    CHECK_MSG(s_last_tx_len == sizeof(buf), "发出去的字节数不对：%u", s_last_tx_len);
    CHECK_MSG(s_last_tx_timeout >= 122, "50ms 的超时没被抬到物理时间（实际 %ums）—— 长帧会被截断",
              (unsigned)s_last_tx_timeout);

    /* 调用方给得足够大时不改动它 */
    pl_uart_send(h, buf, sizeof(buf), 5000);
    CHECK_MSG(s_last_tx_timeout == 5000, "足够大的超时被改动了：%ums", (unsigned)s_last_tx_timeout);

    /* 短帧不该被无谓地抬高 */
    s_last_tx_timeout = 0;
    pl_uart_send(h, buf, 10, 100);
    CHECK_MSG(s_last_tx_timeout == 100, "短帧的超时被抬了：%ums", (unsigned)s_last_tx_timeout);
}

/* ================================================================ */

int main(void)
{
    case_tim_isrs_before_init();
    case_tim7_tick_before_init();
    case_uart_isr_before_init();
    case_tx_timeout_floor();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
