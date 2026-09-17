/**
 * @file    dev_w25qxx.c
 * @brief   W25Qxx SPI Flash 存储设备 — 实现 dev_storage_ops
 */

#include "dev_w25qxx.h"

#include <string.h>
#include "cmsis_os2.h"
#include "initcall.h"
#include "pl_spi.h"
#include "pl_gpio.h"
#include "pl_sys.h"

/* W25Qxx 命令 */
#define W25Q_READ_CMD      0x03
#define W25Q_RESET_ENABLE  0x66
#define W25Q_RESET_DEVICE  0x99
#define W25Q_READ_JEDEC_ID 0x9F
#define W25Q_WRITE_ENABLE  0x06
#define W25Q_PAGE_PROGRAM  0x02
#define W25Q_SECTOR_ERASE  0x20
#define W25Q_CHIP_ERASE    0xC7
#define W25Q_ENTER_4BYTE   0xB7
#define W25Q_READ_STATUS1  0x05

typedef struct {
    dev_storage_t me;
    pl_spi_handle_t spi;
    uint16_t device_id; /* JEDEC ID: Memory Type << 8 | Capacity */
    uint16_t page_size;
    uint32_t sector_size;
} dev_w25qxx_t;

/* ---- 全局实例 ---- */
static dev_w25qxx_t g_w25qxx = {.page_size = 256, .sector_size = 4096};

/* JEDEC ID → 容量
 *
 * 容量字节本身就是二进制指数：0x15→2^21=2MB, 0x18→2^24=16MB, 0x19→2^25=32MB。
 * 原实现把 default 当作 W25Q256+ 返回 32MB，于是**未响应/未识别的器件
 * （ID 读回 0x00 或 0xFF）也会被报成 32MB** —— 上层据此算出配置区地址并擦写，
 * 把"存储不可用"伪装成"存储可用"。这里只接受合法指数区间，其余返回 0。 */
static uint32_t _jedec_capacity(uint16_t id)
{
    uint8_t cap_byte = (uint8_t)(id & 0xFF);

    if (cap_byte < 0x15U || cap_byte > 0x1FU)
        return 0; /* 未识别或器件未响应 */

    return 1UL << cap_byte;
}

/* 容量字节 >= 0x19 → >128Mb → 需 4 字节地址 */
static inline uint8_t _addr_len(dev_w25qxx_t *s)
{
    return (s->device_id & 0xFF) >= 0x19 ? 4 : 3;
}

/* 写地址到 cmd buffer（cmd_cnt 模式），返回写入的字节数 */
static inline uint8_t _put_addr(uint8_t *cmd, uint32_t addr, dev_w25qxx_t *s)
{
    uint8_t n = 0;
    if (_addr_len(s) == 4) cmd[n++] = (uint8_t)(addr >> 24);
    cmd[n++] = (uint8_t)(addr >> 16);
    cmd[n++] = (uint8_t)(addr >> 8);
    cmd[n++] = (uint8_t)addr;
    return n;
}

dev_storage_t *dev_w25qxx_get(void)
{
    return &g_w25qxx.me;
}

/* ---- CS 控制 ---- */
static inline void _cs_low(void)
{
    pl_gpio_write(PL_PORT_B, 1, false);
}
static inline void _cs_high(void)
{
    pl_gpio_write(PL_PORT_B, 1, true);
}

/* ---- DMA 同步（_read 懒初始化）---- */
static osEventFlagsId_t s_evt;
static volatile bool s_ok;

static void _dma_cb(void *ctx)
{
    (void)ctx;
    s_ok = true;
    osEventFlagsSet(s_evt, 0x01);
}

/* ---- 访问串行化 ----
 *
 * 本驱动**不可重入**，三处共享状态：
 *   - s_ok 是全局的 DMA 完成标志：两个并发读会互相"吃掉"完成事件，先发起的一方
 *     会带着半满的缓冲返回（它看到的是对方置的标志）；
 *   - _write 的读-改-写用共享的 static sec[4096]；
 *   - _cs_high() 由一方调用会打断另一方正在进行的传输。
 *
 * 而调用方确实来自不同任务：app_render 读字库（LDI 任务与 RLS 任务都会调），
 * 配置落盘同样来自这两个任务。表现为偶发字模乱码 / 配置读取出错，且在台面上
 * 极难复现。
 *
 * 故在三个虚表入口统一持锁，内部实现一律走 _unlocked 版本（_write_unlocked
 * 内部要读扇区，若调加锁版会自死锁——osMutexNew 建的是非递归锁）。
 *
 * 锁在 **sw_dev_initcall** 里创建，不能在 _init（hw_dev_initcall）里创建：
 *   FreeRTOS 的内核对象要从堆上分配（pvPortMalloc），而 heap_4 的 pvPortMalloc
 *   内部用 vTaskSuspendAll/xTaskResumeAll，后者进临界区（taskENTER/EXIT_CRITICAL）。
 *   调度器启动前 uxCriticalNesting 的初值是 0xaaaaaaaa（哨兵），只在
 *   xPortStartScheduler() 里被置 0；此前进一次临界区，退出时递减成 0xaaaaaaa9
 *   ≠ 0，portENABLE_INTERRUPTS() 就永远不会被调用 —— 中断从此永久关闭，
 *   TIM7 不再产生 HAL 时基，HAL_Delay 死等。
 *   （实测症状：卡在 dev_w25qxx_init → _init → pl_delay_ms。）
 * sw_dev(2) 在 RTOS 启动之后、且早于 sw_app(3) 的配置加载遍（那才是首次访问），
 * 因此既安全又无竞态。 */
static osMutexId_t s_lock;

static void _lock(void)
{
    if (s_lock) osMutexAcquire(s_lock, osWaitForever);
}

static void _unlock(void)
{
    if (s_lock) osMutexRelease(s_lock);
}

/* ---- OPS 实现 ---- */
static int32_t _init(dev_storage_t *dev)
{
    dev_w25qxx_t *self = (dev_w25qxx_t *)dev;
    self->spi          = pl_spi_get_handle();

    /* 注意：这里**不能**创建访问锁。RTOS 尚未启动，而创建内核对象要从堆上分配，
       会把中断永久关掉（原因见文件上方 s_lock 的说明）。锁在下面的 sw_dev
       initcall 里创建。 */

    /* 复位（阻塞，无需RTOS） */
    uint8_t rst[2] = {W25Q_RESET_ENABLE, W25Q_RESET_DEVICE};
    _cs_low();
    pl_spi_transmit(self->spi, rst, 2);
    _cs_high();
    pl_delay_ms(10);

    /* JEDEC ID → 容量（阻塞全双工，无需RTOS） */
    uint8_t tx[4] = {W25Q_READ_JEDEC_ID, 0xFF, 0xFF, 0xFF}, rx[4] = {0};
    _cs_low();
    pl_spi_transmit_receive(self->spi, tx, rx, 4);
    _cs_high();

    self->device_id = (uint16_t)(rx[2] << 8) | rx[3]; /* Type|Capacity */
    dev->capacity   = _jedec_capacity(self->device_id);

    /* >128Mb → 4 字节地址模式 */
    if (_addr_len(self) == 4) {
        uint8_t c4 = W25Q_ENTER_4BYTE;
        _cs_low();
        pl_spi_transmit(self->spi, &c4, 1);
        _cs_high();
    }
    return 0;
}

/** @brief 读扇区数据（调用者已持锁；_write 的读-改-写也走这里） */
static int32_t _read_unlocked(dev_storage_t *dev, uint32_t addr, uint8_t *buf, uint32_t len)
{
    dev_w25qxx_t *self = (dev_w25qxx_t *)dev;
    if (!buf || len == 0) return -1;

    if (!s_evt) {
        s_evt = osEventFlagsNew(NULL);
        if (!s_evt) return -1;
        pl_spi_set_rx_cplt_cb(self->spi, _dma_cb, NULL);
    }

    uint8_t al = _addr_len(self);
    uint8_t cmd[5];
    cmd[0] = W25Q_READ_CMD;
    _put_addr(cmd + 1, addr, self);

    s_ok = false;
    _cs_low();
    pl_spi_transmit(self->spi, cmd, (uint16_t)(al + 1));
    pl_spi_receive_dma(self->spi, buf, (uint16_t)len);
    while (!s_ok)
        osDelay(1);
    _cs_high();
    return 0; /* 约定: 0 = 成功（不返回字节数） */
}

static int32_t _read(dev_storage_t *dev, uint32_t addr, uint8_t *buf, uint32_t len)
{
    _lock();
    int32_t r = _read_unlocked(dev, addr, buf, len);
    _unlock();
    return r;
}

static int32_t _write_enable(dev_w25qxx_t *self)
{
    uint8_t cmd = W25Q_WRITE_ENABLE;
    _cs_low();
    int32_t r = pl_spi_transmit(self->spi, &cmd, 1);
    _cs_high();
    return r;
}

static int32_t _wait_busy(dev_w25qxx_t *self, uint32_t timeout_ms)
{
    for (uint32_t t = 0; t < timeout_ms; t++) {
        _cs_low();
        uint8_t cmd = W25Q_READ_STATUS1, st;
        pl_spi_transmit(self->spi, &cmd, 1);
        pl_spi_receive(self->spi, &st, 1);
        _cs_high();
        if (!(st & 0x01)) return 0;
        osDelay(1);
    }
    return -1;
}

static int32_t _write_page(dev_w25qxx_t *self, uint32_t addr, const uint8_t *buf, uint16_t len)
{
    _write_enable(self);
    uint8_t al = _addr_len(self);
    uint8_t cmd[5];
    cmd[0] = W25Q_PAGE_PROGRAM;
    _put_addr(cmd + 1, addr, self);
    _cs_low();
    if (pl_spi_transmit(self->spi, cmd, (uint16_t)(al + 1)) != 0) {
        _cs_high();
        return -1;
    }
    int32_t r = pl_spi_transmit(self->spi, buf, len);
    _cs_high();
    return (r == 0) ? _wait_busy(self, 100) : -1;
}

static int32_t _write_no_check(dev_w25qxx_t *self, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t w = 0;
    while (w < len) {
        uint16_t pr = 256 - (addr % 256);
        uint16_t ch = (len - w <= pr) ? (uint16_t)(len - w) : pr;
        if (_write_page(self, addr, buf + w, ch) != 0) return -1;
        w += ch;
        addr += ch;
    }
    return 0;
}

/** @brief 读-改-写（调用者已持锁） */
static int32_t _write_unlocked(dev_storage_t *dev, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    dev_w25qxx_t *self = (dev_w25qxx_t *)dev;
    if (!buf || len == 0) return -1;

    static uint8_t sec[4096];
    uint32_t w = 0;

    while (w < len) {
        uint32_t sa  = addr - (addr % 4096);
        uint16_t off = addr % 4096;
        uint32_t ch  = 4096 - off;
        if (len - w < ch) ch = len - w;

        _read_unlocked(dev, sa, sec, 4096);

        bool need = false;
        for (uint16_t i = off; i < off + ch; i++)
            if (sec[i] != 0xFF) {
                need = true;
                break;
            }

        if (need) {
            _write_enable(self);
            uint8_t eal = _addr_len(self);
            uint8_t ec[5];
            ec[0] = W25Q_SECTOR_ERASE;
            _put_addr(ec + 1, sa, self);
            _cs_low();
            pl_spi_transmit(self->spi, ec, (uint16_t)(eal + 1));
            _cs_high();
            _wait_busy(self, 3000);
            memset(sec, 0xFF, 4096);
        }

        memcpy(sec + off, buf + w, ch);
        if (_write_no_check(self, sa, sec, 4096) != 0) return -1;
        w += ch;
        addr += ch;
    }
    return 0;
}

static int32_t _write(dev_storage_t *dev, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    _lock();
    int32_t r = _write_unlocked(dev, addr, buf, len);
    _unlock();
    return r;
}

/** @brief 擦除一个扇区（调用者已持锁） */
static int32_t _erase_unlocked(dev_storage_t *dev, uint32_t addr, uint32_t len)
{
    (void)len;
    dev_w25qxx_t *self = (dev_w25qxx_t *)dev;

    _write_enable(self);
    uint8_t al = _addr_len(self);
    uint8_t cmd[5];
    cmd[0] = W25Q_SECTOR_ERASE;
    _put_addr(cmd + 1, addr, self);
    _cs_low();
    int32_t r = pl_spi_transmit(self->spi, cmd, (uint16_t)(al + 1));
    _cs_high();
    return (r == 0) ? _wait_busy(self, 3000) : -1;
}

static int32_t _erase(dev_storage_t *dev, uint32_t addr, uint32_t len)
{
    _lock();
    int32_t r = _erase_unlocked(dev, addr, len);
    _unlock();
    return r;
}

static uint32_t _capacity(dev_storage_t *dev)
{
    return dev->capacity;
}

static const dev_storage_ops_t w25qxx_ops = {
    .init     = _init,
    .read     = _read,
    .write    = _write,
    .erase    = _erase,
    .capacity = _capacity,
};

/* ---- 自动初始化 ---- */
void dev_w25qxx_init(void)
{
    g_w25qxx.me.ops = &w25qxx_ops;
    _init(&g_w25qxx.me);
}
hw_dev_initcall(dev_w25qxx_init);

/**
 * @brief 创建访问锁 —— 必须在 RTOS 启动之后
 *
 * 见文件上方 s_lock 的说明：创建内核对象要从堆上分配，而 heap_4 的 pvPortMalloc
 * 会进临界区；调度器启动前 uxCriticalNesting 是哨兵值 0xaaaaaaaa，进一次临界区
 * 就会把中断永久关掉（TIM7 停摆 → HAL_Delay 死等）。
 *
 * sw_dev(2) 早于 sw_app(3) 的配置加载遍 —— 那才是本驱动的首次访问，
 * 所以到这里为止 s_lock 还是 NULL 是安全的（_lock 会跳过）。
 */
static void _w25qxx_lock_init(void)
{
    const osMutexAttr_t attr = {.name = "w25qxx", .attr_bits = osMutexPrioInherit};
    s_lock                   = osMutexNew(&attr);
}
sw_dev_initcall(_w25qxx_lock_init);
