/**
 * @file    app_cascade.c
 * @brief   多控制卡级联同步显示协议 —— 探针、任务、枚举、整屏调光、一帧一轮的图传
 *
 * **一轮 = 一条 IMAGE 帧**：主卡把每张从卡那一块整块位图（一帧装得下，不再分片）
 * 单播过去，从卡收下即落屏并回 ACK。逐卡走完再下一张，最后主卡本地提交。
 *
 * 骨架照 `Application/Src/RLS/app_rls.c`（收-only 最干净的那个）：
 * RB_DEFINE_ATTR → 静态 pcb_t → sw_post_initcall 里 rb_init / 建队列 / 绑通道 / 起任务。
 *
 * **绑 RS485**，且地址必须写在帧里 —— `ccb_dst_t` 只有 broadcast/topic 两个字段、
 * 没有地址，而 RS485 通道本来就忽略 dst（总线共享）。各从卡按帧里的 dst 过滤，
 * 与 LDI 用 lane_code 判"不属于本设备"→ SKIP 是同一做法。
 */

#include "app_cascade.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h" /* StaticQueue_t（静态队列控制块）*/
#include "app_cfg_sched.h" /* 身份记录 */
#include "app_rs485.h"
#include "app_render.h" /* 从卡落盘走 app_render_save；开轮 peek 持久化请求位 */
#include "app_screen.h"
#include "dev_key.h" /* 有拨码的板子按 DIP1/DIP2 定址 */
#include "dev_display.h" /* PRESENT 要报本卡几何 */
#include "pl_crc.h"
#include "initcall.h"
#include "pl_mem.h"
#include "pl_task.h"

/* ---- 诊断输出 ----
 *
 * **两侧都要有**：只有一侧打日志时，"帧没到"与"帧到了但没处理"在现场分不开，
 * 而这两种情况的排查方向完全相反（查线 vs 查任务）。
 * 默认开；现场嫌吵可 -DCASC_DIAG=0 关掉，与 app_screen_status() 那条"上报是可选的"
 * 是两回事 —— 这是**本地**调试输出，不出设备。 */
#ifndef CASC_DIAG
#define CASC_DIAG (1)
#endif
/* **每行带内核 tick**：probe-rs 打的是"它读到这一行的时刻"，不是打印时刻 ——
 * 一次 attach 会把缓冲区里积压的多行打上同一个时间戳，跨两次 attach 的日志更是
 * 没法对照。今天已经因此误判过好几次（"PING 到 PRESENT 只隔了 1.4 秒"之类）。
 * 带上 tick，日志就自己说明先后与间隔，不必再反推。 */
#if CASC_DIAG
#define CASC_LOG(fmt, ...) printf("[%8u] " fmt, (unsigned)osKernelGetTickCount(), ##__VA_ARGS__)
#else
#define CASC_LOG(fmt, ...) ((void)0)
#endif

/* ================================================================
 *  单卡板：本文件编译成空
 *
 *  见 app_cascade.h 的 `BOARD_CASCADE_ENABLED`。守卫放在 include 之后、所有
 *  静态量与函数之前 —— 于是单卡板上这个翻译单元**一个字节都不产生**
 *  （Flash、CCMRAM、以及"每 10 秒一次 PING"全都省掉），而两块板共用同一份
 *  构建清单（Makefile / EIDE 都不用改）。
 * ================================================================ */
#if !BOARD_CASCADE_ENABLED
/* 故意留空：本板不跑级联 */
#else

/* ---- 队列与缓冲区（容量取法见 app_iap.c / app_rls.c 的同名注释）---- */

/* **本协议会发出的最长帧** = 一帧装下整块本卡位图（见 app_cascade.h 的 CASC_FRAME_MAX）。
 * 它同时是 `pcb.payload_max`：框架据此拒收超长的帧，队列元素也因此有硬上界。 */
#define CASC_MSG_MAX CASC_FRAME_MAX

_Static_assert(CASC_MSG_MAX >= CASC_OVERHEAD + sizeof(casc_present_t), "PRESENT 装不下");
_Static_assert(CASC_MSG_MAX >= CASC_OVERHEAD + sizeof(casc_nack_t), "NACK 装不下");

#define CASC_MSG_SIZE (sizeof(frame_msg_t) + CASC_MSG_MAX)

/* **深度 2**。P2 时期是 2（帧稀疏，够用），P3 分片时被抬到 8 —— 因为一轮连发 4~5 帧，
   而 ISR → rs485_task → frame_dispatch_task → 本协议任务四跳都是 Normal 优先级，
   队列一满框架就**静默丢帧**（Put 超时为 0），现场表现为"总缺中间那几片"。

   一帧一轮之后连发没了：主卡发一条、从卡回一条，**任何时刻队列里最多一条**，
   2 就是留一格的余量。而深度现在也不是能随便加的 —— 队列元素 = 8 + 1427 = 1435 字节
   （位图整块进队列），深 8 要 11.5KB CCMRAM，而 CCMRAM 总共只剩 2.2KB 空闲。 */
#define CASC_QUEUE_DEPTH (2U)

/* **发送缓冲**：组帧与 DMA 发送共用这一块。
 *
 * 必须在 **.bss（SRAM）**：RS485 通道按"指针是否落 CCMRAM"在 DMA 与轮询之间二选一
 * （app_rs485.c 的 rs485_send），挪进 CCMRAM 不会报错，只会**静默退化成轮询发送** ——
 * 一帧 1427 字节 @115200 就是 124ms 的 CPU 被烧在这条任务里。`_cascade_init` 里有
 * 一次性校验，真被挪了会打出来。
 *
 * 一块够所有卡轮着用：`ccb_send` 是**阻塞**语义（DMA 等 TC、轮询等发完），返回即发完。
 *
 * **接收侧一律不碰它**（`_cmd_ack`/`_cmd_nack`/`_cmd_present` 只读 `msg->data`）——
 * 这是"位图能一直躺在 s_tx 里等应答"的前提：主卡把整块位图抽进 s_tx 之后要等最多
 * 200ms 的应答，这期间收进来的帧若也往 s_tx 写，等到的应答会把还没发出去的位图冲掉，
 * 或者重发的帧发出去的是应答的字节。**加新的接收处理函数时别用 s_tx。** */
static uint8_t s_tx[CASC_FRAME_MAX];

static StaticQueue_t s_casc_queue_cb;
static uint8_t       s_casc_queue_buf[CASC_QUEUE_DEPTH * CASC_MSG_SIZE] PL_CCMRAM;
static const osMessageQueueAttr_t s_casc_queue_attr = {
    .name    = "proto_casc_queue",
    .cb_mem  = &s_casc_queue_cb,
    .cb_size = sizeof(s_casc_queue_cb),
    .mq_mem  = s_casc_queue_buf,
    .mq_size = sizeof(s_casc_queue_buf),
};

/* 必须**严格大于**传输层单次最大写入（RS485 的 DMA 缓冲 2048）；
   相等或更小会静默丢掉最后一字节 —— 见 app_iap.c 的说明。
 *
 * **取 4096 而不是够用就行的 2112**：`app_ccb_dispatch` 在"装不下"时的策略是
 * **丢旧留新**（整段 flush 再写），而它分不清"旧数据是半截帧"与"旧数据是一条完整
 * 但还没轮到解析的帧"。实测就撞上了：帧分发任务被同一条总线上别的协议的探针拖住
 * 几十毫秒（见 casc_probe_frame 里那段 O(n²) 的说明），这期间**下一条帧的第一段**
 * 就到了 —— 2112 装不下 1427 + 1211，于是那条完整帧被 flush 掉，主卡只能重发。
 * 留够"两帧"的余量，这种抢占就不会发生（仍留有 flush 兜底，只是不再误伤整帧）。 */
RB_DEFINE_ATTR(s_casc_rb, 4096, PL_CCMRAM);

static osMessageQueueId_t s_casc_queue;

/* 上电时要先等从卡起来，故延后一点再发 PING */
#define CASC_BOOT_PING_DELAY_MS (3000U)
/* 亮度待下发的轮询周期 */
#define CASC_BRIGHT_POLL_MS (100U)

/* 等一条 ACK 的上限。
 *
 * 原定 40ms，是按"一帧 ACK 只有 17 字节 + 转发与调度延迟"估的 —— **估小了**。
 * 实测从卡从"收到 COMMIT"到"应答上总线"的时延在 3ms 到 87ms 之间波动：87ms 那次
 * 主卡必然超时，而从卡那边 `sta=0`、它认为自己一切正常 —— 于是两块屏不同步，
 * 且从卡那一侧看不出任何异常。这是这套协议里最坏的失配形态。
 *
 * 200ms 留了足够余量。代价：卡真的掉线时每轮要等 3×200ms，配合 CASC_FAIL_RUN_MAX
 * 的剔除，最坏 3 秒把它摘掉 —— 可接受。
 *
 * 那段波动的来源还没查清（`prepare` 只要 ~2ms，不是它）。主卡的日志会把每次
 * 实际等待时长打出来，够了就能看出分布。 */
#define CASC_ACK_TIMEOUT_MS (200U)

/* 等 ACK 期间的轮询周期。只在一轮进行中才跑这么密，空闲时任务阻塞在队列上。 */
#define CASC_ACK_POLL_MS (1U)

/* **整帧重传**次数上限（一封帧最多发 1 + 本值 次）。
 *
 * 一帧就是一幅完整画面，所以"重传"没有"只补缺的那几片"这种粒度可言 —— 整帧重发。
 * 一条 1427 字节的重发（124ms）换掉整套缺片追踪，值。
 * 但不能无限试：卡真的掉线时，每轮都卡在这儿会拖长整轮时间（其余卡与主卡本来不用等它），
 * 所以上限 2 次 ≈ 3×(124ms 发送 + 200ms 等应答)，之后交 `CASC_FAIL_RUN_MAX` 去剔除。 */
#define CASC_RETRY_MAX (2U)

/* 上电枚举：发 PING 等 PRESENT，**没等到就再发**，最多这么多轮、每轮隔这么久。
 *
 * **不能只发一次就下结论**：从卡可能比主卡晚就绪，或者那一帧正好赶上总线冲突。
 * 只发一次的话"链路其实没问题"会被报成"整个不通"—— 实测就撞上过：枚举说没人应答，
 * 紧接着的对齐轮次却全部成功，把人往接线/上电的方向引了半天。
 * （这也正是 P4 要做的"运行期重新枚举"的雏形。） */
#define CASC_PING_TRIES     (5U)
#define CASC_PING_RETRY_MS  (250U)

/* 枚举收尾后、第一轮对齐之前的间隔。**两者不能同刻** —— 半双工总线上一次只能有
   一个节点驱动，从卡回 PRESENT 时主卡若正在发分片，两边都成乱码（P3 的现场根因）。 */
#define CASC_BOOT_ALIGN_PING_GAP_MS (300U)

/* （这里原来是"上电对齐轮"的一整套常量与状态机；2026-09-22 撤掉 —— 各卡现在
   各存各的那一块、上电各自恢复，对齐轮反而会把主卡那张不全的画布推下去刷黑从卡。
   见 `_enum_finish` 的说明。） */

/* **连续**几轮未完成才剔除。不能一失败就剔 —— 单轮失败多半是总线上的偶发冲突，
   下一轮自己就好了；剔了反而要等下一次枚举才叫得回来（更慢）。 */
#define CASC_FAIL_RUN_MAX (5U)

/* 运行期重新枚举的周期。**这是发现"卡回来了"的唯一途径** —— 从卡不会主动说话，
   掉线后重新上电，只有靠主卡去问才知道。代价是一帧 15 字节的 PING；从卡在线时
   第一帧 PRESENT 就结束枚举，不会把重试次数耗完。 */
#define CASC_REENUM_MS (10000U)

/* 上电枚举的收卷时刻（0 = 不判定）与"是否见到过应答"。
 *
 * **为什么要专门报一句**：主卡收不到任何应答时，日志里只是"静悄悄地没有 PRESENT"，
 * 而那与"缓冲区被冲掉了""还没到点"分不开。而"从卡整个不答"与"答了但内容不对"
 * 是**完全相反**的两个排查方向（查线 vs 查协议），必须一刀切开。
 * 枚举与画布无关，所以这两个变量放在守卫之外。 */
static uint32_t s_enum_deadline;
static bool     s_enum_seen;
/** 本次枚举是不是**上电那一次** —— 只有它对"有没有应答"下判据。
 *  周期性重新枚举也判的话，每分钟会多出 6 行"有卡应答"，把 1KB 的 RTT 缓冲冲掉，
 *  而那个信息本来就没用（PRESENT 那行已经说明了）。 */
static bool s_enum_verdict;

/** 上电那一次枚举是否已经收尾 —— 只有它才对"有没有卡应答"下判据 */
static bool s_first_enum_done;

/* 上电枚举的剩余次数与下次发包时刻（0 = 枚举已收尾） */
static uint8_t  s_ping_left;
static uint32_t s_ping_next;

/* 下一次重新枚举的时刻 —— 见 CASC_REENUM_MS 的说明 */
static uint32_t s_reenum_at;

/* 每张从卡的**连续失败轮数**：成功一轮即清零。放协议侧而不是切分表里 ——
   "失败几轮算掉线"是本协议自己的策略，不是"这张卡长什么样"的部署事实。 */
static uint8_t s_fail_run[SCREEN_CARD_MAX];

/* 需要立刻开一轮：**刚上线的卡要马上拿到当前内容**。它可能刚插回来，
   屏上还是掉线前那幅旧的，而等下一次内容更新可能要几分钟。 */
static bool s_force_round;

/* 发完 PING 之后的"总线安静期"截止时刻 —— 半双工，从卡回 PRESENT 时主卡不能
   同时在发分片，否则两个节点同时驱动总线，两边都成乱码。 */
static uint32_t s_bus_quiet_until;

/** 轮次序号源。**放在守卫之外**：识别流程（认领时逐卡单播 `SET_ADDR` 并等 ACK）
 *  也用同一个序号源 —— ACK 一律按 (seq, src) 匹配，序号源分开会让匹配更难看出规律。 */
static uint16_t s_round_seq;

/** 本轮开始时**本卡的身份** —— 身份可以在一轮中途被改（按键认领 / 收到识别帧），
 *  而一轮里的 dst、矩形、颜色全是按开轮那一刻的身份算的：继续发下去就是把画面
 *  发错卡、或把别的格子的矩形塞给对端（对端会 NACK，但那一轮的失败计数已经记上，
 *  几轮下来会把一张好卡剔掉）。所以身份一变就立刻给这一轮收尾。
 *  只在有画布那半边用（轮次本身就在守卫内）。 */
#if BOARD_SCREEN_CANVAS
static uint8_t s_round_me;
#endif

/** @brief 起一轮枚举；已在枚举中则不动 */
static void _enum_start(uint32_t now)
{
    s_ping_left    = CASC_PING_TRIES;
    s_ping_next    = now;
    s_enum_seen    = false; /* 只认这一轮发出去之后的应答 */
    s_enum_verdict = !s_first_enum_done; /* 上电那一次才判"有没有卡应答" */
}

/** @brief 枚举收尾：不再发 PING，等最后一帧 PRESENT 走完
 *
 *  **这里不再起"上电对齐轮"**（2026-09-22 撤掉）：那个机制原本是为了
 *  "上电时画布内容是从持久化恢复来的、不算新内容 → 那一轮永远不开 → 从卡一直停在
 *  上电前它自己那幅旧画面上"。而现在**每张卡各存各的那一块**、上电各自恢复，
 *  "各自显示掉电前那一幅"正是我们要的行为 ✓ 反过来，对齐轮会把主卡那张**不全的**
 *  画布（本上电周期里它只恢复了自己那块，其余区域是空的）推下去，把从卡刚恢复的
 *  内容当场刷黑 —— 所以它必须撤掉。
 *
 *  改为由"画布本上电周期内被写过"（`app_screen_canvas_touched()`）闸住轮次：
 *  上位机一下发新内容就恢复整屏同步；某张卡掉线回来时画布早已被写过，`s_force_round`
 *  照常给它补内容。 */
static void _enum_finish(uint32_t now)
{
    s_ping_left        = 0;
    s_enum_deadline    = now + CASC_BOOT_ALIGN_PING_GAP_MS;
    s_reenum_at        = now + CASC_REENUM_MS; /* 到点再问一次：卡回来了只有靠问才知道 */
    s_first_enum_done  = true;                 /* 之后都是运行期的重新枚举 */
}

/* pcb 的字段在 _cascade_init 里填（探针定义在本文件后部，ops 要指向它） */
static pcb_t s_casc_pcb;

pcb_t *app_cascade_pcb(void)
{
    return &s_casc_pcb;
}

/* ================================================================
 *  探针
 * ================================================================ */

/** @brief 判断本帧是否该由本卡处理。
 *
 *  广播帧所有卡都收；其余只收自己地址的。**发送方自己也要按这条过滤** ——
 *  半双工总线上主卡可能收到自己发出去的回声（取决于 RE 是否与 DE 联动），
 *  而主卡地址是 0，单播给从卡的帧天然被它自己滤掉。
 *
 *  主卡的 PING/PRESENT 例外见各自的分派处：主卡发 PING 但不应答自己的 PING。 */
static bool _is_for_me(uint8_t dst)
{
    return dst == CASC_ADDR_BCAST || dst == app_screen_self_addr();
}

/** @brief 探测一帧
 *
 *  四态与边界处理的依据见 app_dispatch.h 的 probe 契约：
 *   · 调用者**已持 rb 锁**，故所有 rb API 传 nullptr 跳过二次加锁
 *   · 只窥视不消费
 *   · READY 必须写 *total_len；SKIP 也要写（框架按它跳过整帧）
 *   · 帧长超过 scratch_size 必须 SKIP，不得越界写
 *
 *  **校验顺序：长度域夹紧 → 等整帧 → CRC → 地址过滤。**
 *  CRC 放在地址过滤之前是刻意的：地址过滤会 SKIP 掉整个 len，若 len 被干扰破坏，
 *  就会连带吞掉后面一帧真帧；而 CRC 失败走 FAKE（只跳 1 字节重试），能在下一个
 *  真 SOF 处重新对上。代价是给别人的帧也要算一次 CRC（约 60µs @1KB），
 *  换回来的是可靠的再同步。 */
static pcb_probe_sta_t casc_probe_frame(pcb_t *self, const ccb_t *ccb, const ccb_src_t *src,
                                        uint8_t *scratch, uint16_t scratch_size,
                                        uint32_t *total_len, uint8_t *aux)
{
    (void)ccb;
    (void)src; /* RS485 无来源概念 */

    /* ---- **先只窥视帧头，别一上来就拷整帧** ----
     *
     * 伪帧时框架会"跳 1 字节再探"，于是"每次探都把整帧拷进暂存区"就成了 O(n²)：
     * 实测一条 1427 字节的杂物要拷 ~2MB（1427 次 × 每次 ~1428 字节），×3 个协议 = ~6MB，
     * 把帧分发任务拖住**四十多毫秒** —— 而这段时间里同一条总线上后到的帧会把前一条
     * **完整但还没轮到解析的**帧从协议缓冲里挤掉（丢旧留新），现场表现就是
     * "跨回绕拆成两段的帧总是丢、一次发完的就成"。所以头没对上就直接 FAKE，
     * 一个字节都不多拷；只有确认是真帧、且整帧到齐，才拷进来算 CRC。
     *
     * 顺带：`avail` 用 rb_avail 问，不靠窥视的返回值（那次窥视只拷了 11 字节）。 */
    const uint16_t head_cap = (scratch_size < (uint16_t)sizeof(casc_hdr_t))
                                  ? scratch_size
                                  : (uint16_t)sizeof(casc_hdr_t);
    if (rb_peek_capped(self->rb, 0, scratch, head_cap, nullptr) < sizeof(casc_hdr_t))
        return PCB_PROBE_WAIT;

    casc_hdr_t *h = (casc_hdr_t *)scratch;
    if (h->sof[0] != CASC_SOF0 || h->sof[1] != CASC_SOF1) return PCB_PROBE_FAKE;

    uint16_t len = casc_get_u16(h->len);

    /* 长度域先夹：合法级联帧永远在 [MIN, MAX] 内。越界说明这是伪同步，
       逐字节重跳能最快找回真帧；若返回 SKIP 会按伪长度把后面的真帧一起吞掉。 */
    if (len < CASC_FRAME_MIN || len > CASC_FRAME_MAX) return PCB_PROBE_FAKE;

    /* 整帧到齐前不碰帧尾/CRC —— 否则读到的是后续字节 */
    if (rb_avail(self->rb, nullptr) < len) return PCB_PROBE_WAIT;

    if (len > scratch_size) {
        *total_len = len;
        return PCB_PROBE_SKIP; /* 本设计下不可达（static_assert 保证），按契约保留 */
    }

    /* 到这里才需要整帧（CRC 要覆盖到帧尾） */
    if (rb_peek_capped(self->rb, 0, scratch, len, nullptr) < len) return PCB_PROBE_WAIT;

    /* CRC32 覆盖 [2, len-4)：不含 SOF（恒定值不增加信息量）、不含 CRC 自身。
       走硬件单元（1KB 约 9µs；软件 CRC16 要 71µs）。
       注意区段起点 scratch+2 **不对齐** —— pl_crc32_calc 原先对非对齐输入只算前
       256 字节，正是为此先修了它（见该提交）。 */
    uint32_t crc_calc = pl_crc32_calc(pl_crc_get_handle(), scratch + 2, (size_t)(len - 6U));
    uint32_t crc_recv = casc_get_u32(scratch + len - 4U);
    if (crc_calc != crc_recv) return PCB_PROBE_FAKE;

    if (!_is_for_me(h->dst)) {
        *total_len = len;
        return PCB_PROBE_SKIP; /* 帧合法，只是不是给我这张卡的 */
    }

    *total_len = len;
    *aux       = CASC_TYPE_OF(h->ver_type);
    return PCB_PROBE_READY;
}

/* ================================================================
 *  发送
 * ================================================================ */

/** @brief 给**载荷已在位**的缓冲补上帧头与 CRC；返回整帧长度，0 = 放不下
 *
 *  之所以要"载荷已在位"这条路径：位图是**直接抽进发送帧的载荷位置**的（见
 *  `_round_one_card`），不能先抽到别处再 memcpy 一次 —— 那是每卡每轮白搬 1400 字节。 */
static uint16_t _finish_frame(uint8_t *buf, uint16_t buf_cap, uint8_t type, uint8_t dst,
                              uint16_t seq, uint16_t payload_len)
{
    const uint16_t len = (uint16_t)(CASC_OVERHEAD + payload_len);
    if (len > buf_cap || len > CASC_FRAME_MAX) return 0;

    casc_hdr_t *h = (casc_hdr_t *)buf;
    h->sof[0]     = CASC_SOF0;
    h->sof[1]     = CASC_SOF1;
    h->ver_type   = (uint8_t)((CASC_PROTO_VER << 6) | CASC_TYPE_OF(type));
    h->dst        = dst;
    h->src        = app_screen_self_addr();
    casc_put_u16(h->seq, seq);
    h->idx        = 0; /* 保留字段：一帧一轮，不再有分片号 */
    h->frag_n     = 0; /* 保留字段：同上 */
    casc_put_u16(h->len, len);

    const uint32_t crc = pl_crc32_calc(pl_crc_get_handle(), buf + 2, (size_t)(len - 6U));
    casc_put_u32(buf + len - 4U, crc);
    return len;
}

/** @brief 组一帧到 buf（把 payload 拷进载荷区）；返回整帧长度，0 = 放不下 */
static uint16_t _build(uint8_t *buf, uint16_t buf_cap, uint8_t type, uint8_t dst, uint16_t seq,
                       const void *payload, uint16_t payload_len)
{
    const uint16_t len = (uint16_t)(CASC_OVERHEAD + payload_len);
    if (len > buf_cap || len > CASC_FRAME_MAX) return 0; /* 先判再拷，不能让 memcpy 越界 */

    if (payload_len) memcpy(buf + sizeof(casc_hdr_t), payload, payload_len);
    return _finish_frame(buf, buf_cap, type, dst, seq, payload_len);
}

/** @brief 发一条**已组好**的帧（内容在 s_tx 里就位）；返回整帧长度，组帧失败为 -1
 *
 *  `ccb_send` 是阻塞的（DMA 等 TC / 轮询等发完），返回即代表已发完 —— 所以 s_tx
 *  在返回后可以立刻被下一次组帧覆盖。 */
static int32_t _send_tx(uint16_t len, uint8_t type)
{
    if (!len) return -1;

    const int32_t r = ccb_send(app_rs485_ccb(), s_tx, len);
    if (r < 0) {
        /* 通道没 UP / 平台层拒收。**只报前几次**：真出问题时每轮都会失败，
           不限量会把 RTT 冲干净，反而看不到别的。 */
        static uint8_t s_fail_logged;
        if (s_fail_logged < 4U) {
            s_fail_logged++;
            CASC_LOG("[casc] 发送**没出去**（%u 字节，类型 %02X）—— 通道未 UP 或平台层拒绝\n",
                     (unsigned)len, (unsigned)type);
        }
    }
    return (int32_t)len;
}

/** @brief 组一帧（载荷拷进 s_tx）并发出；**序号由调用方给**。
 *
 *  轮次类命令（IMAGE）与应答（ACK/NACK）必须用**本轮那个 seq**，不能用发送方自己的
 *  计数器：主卡靠 seq 认"这条 ACK 是答哪一轮的"，从卡回的 ACK 若带自己的序号，
 *  主卡永远匹配不上，表现是"每张卡都超时"。 */
static int32_t _send_seq(uint8_t type, uint8_t dst, uint16_t seq, const void *payload,
                         uint16_t payload_len)
{
    return _send_tx(_build(s_tx, sizeof(s_tx), type, dst, seq, payload, payload_len), type);
}

/** @brief 发一帧不需要轮次序号的（PING/PRESENT/SET_BRIGHT），序号取自增计数器 */
static int32_t _send(uint8_t type, uint8_t dst, const void *payload, uint16_t payload_len)
{
    static uint16_t s_seq;
    return _send_seq(type, dst, s_seq++, payload, payload_len);
}

int32_t app_cascade_ping(void)
{
    return _send(CASC_T_PING, CASC_ADDR_BCAST, nullptr, 0);
}

int32_t app_cascade_broadcast_bright(uint8_t level)
{
    casc_set_bright_t p = {.level = (uint8_t)(level > 7 ? 7 : level)};
    return _send(CASC_T_SET_BRIGHT, CASC_ADDR_BCAST, &p, sizeof(p));
}

/* ================================================================
 *  命令处理
 *
 *  P2 只三条：PING / PRESENT / SET_BRIGHT。分派按 `msg->aux` 查表 ——
 *  与 app_iap.c 同一做法，**且必须先做范围检查再索引**（那里写着不检查就等于
 *  给出一条可控的越界函数调用）。
 * ================================================================ */

static void _cmd_ping(frame_msg_t *msg)
{
    (void)msg;
    /* 主卡发 PING，从卡应答。主卡收到自己的 PING（或回声）时什么都不做 ——
       否则主卡会把自己当从卡应答一次，总线上出现两条 PRESENT。 */
    if (app_screen_is_master()) return;

    /* 按地址错开应答，避免多张从卡同时抢半双工总线。
       延时放在任务里（不在探针里）—— 探针在分发任务上下文中，堵它等于堵所有协议。 */
    osDelay((uint32_t)(app_screen_self_addr() - 1U) * 3U);

    dev_display_t *d = dev_display_get();
    casc_present_t p = {
        .addr      = app_screen_self_addr(),
        .bright    = app_screen_get_brightness(),
        .proto_ver = CASC_PROTO_VER,
    };
    uint16_t w = d ? d->screen_rows : 0;
    uint16_t h = d ? d->screen_cols : 0;
    casc_put_u16(p.w, w);
    casc_put_u16(p.h, h);

    (void)_send(CASC_T_PRESENT, CASC_ADDR_MASTER, &p, sizeof(p));
}

static void _cmd_present(frame_msg_t *msg)
{
    /* 主卡收：枚举结果先打出来。P4 建卡表时这里改成填表 + 校验几何。 */
    if (!app_screen_is_master()) return;
    if (msg->data_len != (uint16_t)(CASC_OVERHEAD + sizeof(casc_present_t))) return;

    const casc_present_t *p = (const casc_present_t *)(msg->data + sizeof(casc_hdr_t));

    s_enum_seen = true; /* 上电枚举据此判"有没有卡应答" */

    /* **建表**：这张卡在线了。刚上线要**立刻**给它一轮 —— 它可能刚插回来，
       屏上还是掉线前那幅旧内容，而等下一次内容更新可能要几分钟。
       这也是"拔线→插回"能自愈的关键一步：枚举把状态改回 ONLINE，这一轮把内容补齐。 */
    const uint8_t i = app_screen_index_of_addr(p->addr);
    if (i != 0xFF && app_screen_card_state(i) != SCREEN_CARD_ONLINE) {
        app_screen_card_set_state(i, SCREEN_CARD_ONLINE);
        s_fail_run[i] = 0;
        s_force_round = true;
    }

    printf("[casc] PRESENT addr=%u %ux%u bright=%u ver=%u\n", (unsigned)p->addr,
           (unsigned)casc_get_u16(p->w), (unsigned)casc_get_u16(p->h), (unsigned)p->bright,
           (unsigned)p->proto_ver);
}

static void _cmd_set_bright(frame_msg_t *msg)
{
    if (msg->data_len != (uint16_t)(CASC_OVERHEAD + sizeof(casc_set_bright_t))) return;

    const casc_set_bright_t *p = (const casc_set_bright_t *)(msg->data + sizeof(casc_hdr_t));

    /* 广播的接收方一律套用（包括主卡自己）—— 幂等，且省掉"谁是发出者"的特判。
       主卡本机亮度在生成广播时已经设过，这里再设一次无害。 */
    app_screen_set_brightness(p->level);
}

/* ================================================================
 *  图传（从卡侧）—— 收一条 IMAGE：校验、落屏、应答
 *
 *  **没有暂存、没有状态机**：一帧就是一幅完整画面，收下即落屏。
 *  于是"先攒后换"那一套（分片暂存 + have_mask + 收齐才换帧）整个不需要了 ——
 *  半幅画面的窗口本来只在"边收边画"时才存在。
 *
 *  **幂等**：主卡没收到 ACK 会把同一条帧重发，本函数把同一份内容再落一次，无副作用。
 *  所以从卡不必记住"这一轮处理过没有"，也不必靠 seq 拦陈旧帧 ——
 *  主卡对同一张卡是**等到应答才发下一轮**的，帧不会乱序。
 * ================================================================ */

/** @brief 从卡：拒收本轮，并明确回绝（配置错，重发也没用 → 主卡据此放弃本卡本轮）
 *
 *  与"超时"分开是**有意的**：超时的排查方向是链路，拒绝的排查方向是配置
 *  （格号写重、切分表不一致），而把两者混成一个静默超时，现场只能靠猜。 */
static void _nack(uint16_t seq, uint8_t err, const char *why)
{
    const casc_nack_t nack = {.err = err};

    CASC_LOG("[casc·从] 拒绝 IMAGE seq=%u：%s\n", (unsigned)seq, why);
    (void)_send_seq(CASC_T_NACK, CASC_ADDR_MASTER, seq, &nack, sizeof(nack));
}

static void _cmd_image(frame_msg_t *msg)
{
    /* 主卡不该收到发给从卡的帧（半双工回声、或两张卡地址配重时会）。
       不应答、不落屏 —— 否则总线上会多出一个应答源，主卡把自己当从卡。 */
    if (app_screen_is_master()) return;

    /* **长度先判，再碰任何内容**：这样"两块板烧了不同版本固件"退化成一条 NACK，
       而不是按旧帧的偏移去读位图越界。（帧长上界由框架的 payload_max 兜着。） */
    if (msg->data_len < (uint16_t)(CASC_OVERHEAD + sizeof(casc_image_t))) {
        /* **静默返回是最坏的一种**：主卡只会超时重发，而两侧日志都看不出哪里不对。
           真出现这行说明"探针给的 aux 是 IMAGE，但帧短得装不下头" —— 多半是版本或
           长度域的错，所以必须报出来（限次，免得刷屏）。 */
        static uint8_t s_short_logged;
        if (s_short_logged < 4U) {
            s_short_logged++;
            CASC_LOG("[casc·从] IMAGE 帧太短（%u 字节 < %u）—— 长度域或版本不符？\n",
                     (unsigned)msg->data_len,
                     (unsigned)(CASC_OVERHEAD + sizeof(casc_image_t)));
        }
        return;
    }

    const casc_hdr_t   *h       = (const casc_hdr_t *)msg->data;
    const casc_image_t *p       = (const casc_image_t *)(msg->data + sizeof(casc_hdr_t));
    const uint16_t      seq     = casc_get_u16(h->seq);
    const uint16_t      bmp_len = casc_get_u16(p->bmp_len);

    if (msg->data_len != (uint16_t)(CASC_OVERHEAD + sizeof(casc_image_t) + bmp_len)) {
        _nack(seq, CASC_NACK_LEN, "帧长与 bmp_len 不符");
        return;
    }

    /* 矩形必须与**本卡在本地切分表里那一项**逐字段相符（含 x/y）。核对的是
       "主卡与我对整屏切分的认识一致"，而不只是"这张图装得进我的屏"。
       两块板若烧了不同的 BOARD_CASCADE_MASTER_CELL，同一地址对应的格子就相反 ——
       旧的校验只比 w/h，那种情形下两块屏内容会**悄悄互换而所有检查通过**。 */
    const uint8_t        self = app_screen_self_index();
    const screen_card_t *me   = app_screen_card(self);

    if (!me) {
        _nack(seq, CASC_NACK_GEOM, "本卡地址不在切分表里");
        return;
    }
    if (casc_get_u16(p->x) != me->x || casc_get_u16(p->y) != me->y || casc_get_u16(p->w) != me->w ||
        casc_get_u16(p->h) != me->h) {
        _nack(seq, CASC_NACK_GEOM, "矩形与本卡切分表不符");
        return;
    }
    if (!bmp_len || bmp_len != app_screen_card_bm_len(self)) {
        _nack(seq, CASC_NACK_GEOM, "位图长度与本卡矩形不符");
        return;
    }

    /* **每一轮都重新断言亮度**。SET_BRIGHT 是单次广播、没有重传，而本帧是每轮必发、
       丢一轮就自己自愈的一字节 —— 这是"从卡永久停在旧亮度上"的唯一防线。 */
    app_screen_set_brightness(p->bright);

    /* 位图**直接从队列元素里落屏**：它的载荷区就是线上那串字节，与 draw_bitmap 的
       入参格式逐位一致，零转码、零拷贝。落屏走的是与主卡本地提交完全相同的那个函数。 */
    const uint32_t c0 = osKernelGetTickCount();
    app_screen_commit_bitmap(p->bitmap, bmp_len, p->color);

    CASC_LOG("[casc·从] IMAGE seq=%u %ux%u@(%u,%u) 位图%u bright=%u 落屏 %u ms\n", (unsigned)seq,
             (unsigned)me->w, (unsigned)me->h, (unsigned)me->x, (unsigned)me->y, (unsigned)bmp_len,
             (unsigned)p->bright, (unsigned)(osKernelGetTickCount() - c0));

    /* 回带**本轮序号**（不是自己的发送计数器）—— 主卡的等待循环按 (seq, src) 匹配。
       ACK 无载荷："收下并落屏"就是它唯一的意思，拒绝走 NACK 那条类型。 */
    (void)_send_seq(CASC_T_ACK, CASC_ADDR_MASTER, seq, nullptr, 0);

    /* ---- 落盘：**必须在 ACK 之后** ----
     *
     * 写一条记录是整扇区读-改-写 + 擦除，几十~几百 ms，而主卡等 ACK 的上限是
     * `CASC_ACK_TIMEOUT_MS`(200ms) —— 先写就成了"每轮都超时、每轮都整帧重发"。
     * 先 ACK 则主卡立刻走下一张卡；本卡写盘期间后到的帧进协议 RB（4096B 装得下
     * 一整帧）与队列（深 2），不会丢。
     *
     * 存的是**本卡这一块**（各管各的）：从卡没注册画布钩子，`app_render_save()`
     * 走"直存实屏"那条路，而它的实屏就是它那一块 —— 与主卡本地的持久化同一条路。
     * 内容与上次一致时 `cfg_record_save` 会读回比对后跳过擦写，所以重发无副作用。 */
    if (p->persist) app_render_save();
}

/* ================================================================
 *  图传（主卡侧）—— 结算用收到的应答
 *
 *  **收在这儿、由开轮的那段读**：两者跑在同一个任务里先后发生，不需要锁。
 *  等待循环按 (seq, src) 双重匹配 —— 总线是共享的，迟到的、别张卡的应答都会
 *  落进这里，只按 sta 判会把它们当成当前这张卡的答复。
 * ================================================================ */

#define CASC_STA_ACK  (0x00U) /**< 从卡收下并落屏了 */
#define CASC_STA_NACK (0xFFU) /**< 从卡明确回绝（配置错，重发没用）—— 不在线上出现，只在 s_ack 里 */

static struct {
    uint16_t seq;   /**< 应答回带的轮次序号 */
    uint8_t  src;   /**< 谁答的（总线地址） */
    uint8_t  sta;   /**< CASC_STA_ACK / CASC_STA_NACK */
    uint8_t  err;   /**< sta == CASC_STA_NACK 时是 casc_nack_err_t，否则 0 */
    bool     valid;
} s_ack;

static void _cmd_ack(frame_msg_t *msg)
{
    /* **不判主从**：识别流程里"被降级的那张卡"也要能收到对方的 ACK
       （它就是主动认领/让位的一方）。`s_ack` 只被等待方读，谁收到都无害。 */
    if (msg->data_len != CASC_OVERHEAD) return; /* ACK 无载荷 */

    const casc_hdr_t *h = (const casc_hdr_t *)msg->data;

    /* **收到就打**，不等匹配：只有这一行能把"字节根本没到"与"到了但没对上"分开，
       而这两者排查方向相反（查接收链路 vs 查匹配条件）。 */
    CASC_LOG("[casc·主] ← 收到 ACK seq=%u src=%u\n", (unsigned)casc_get_u16(h->seq),
             (unsigned)h->src);

    s_ack.seq   = casc_get_u16(h->seq);
    s_ack.src   = h->src;
    s_ack.sta   = CASC_STA_ACK;
    s_ack.err   = 0;
    s_ack.valid = true;
}

static void _cmd_nack(frame_msg_t *msg)
{
    if (msg->data_len != (uint16_t)(CASC_OVERHEAD + sizeof(casc_nack_t))) return; /* 同上：不判主从 */

    const casc_hdr_t *h   = (const casc_hdr_t *)msg->data;
    const uint8_t     err = msg->data[sizeof(casc_hdr_t)];

    CASC_LOG("[casc·主] ← 收到 NACK seq=%u src=%u err=%u\n", (unsigned)casc_get_u16(h->seq),
             (unsigned)h->src, (unsigned)err);

    s_ack.seq   = casc_get_u16(h->seq);
    s_ack.src   = h->src;
    s_ack.sta   = CASC_STA_NACK;
    s_ack.err   = err;
    s_ack.valid = true;
}

/* 前置声明：定义在文件后部的"主从识别"一节（识别帧与认领流程共处一段） */
static void _cmd_set_addr(frame_msg_t *msg);

typedef void (*casc_cmd_fn_t)(frame_msg_t *msg);

/* 按帧类型索引。0 项留空 = 未实现或不支持（SET_COLOR/SET_LAYOUT/BLANK 归后续期）。 */
static const casc_cmd_fn_t g_casc_cmd[CASC_TYPE_MASK + 1U] = {
    [CASC_T_IMAGE]      = _cmd_image,
    [CASC_T_SET_ADDR]   = _cmd_set_addr,
    [CASC_T_PING]       = _cmd_ping,
    [CASC_T_PRESENT]    = _cmd_present,
    [CASC_T_SET_BRIGHT] = _cmd_set_bright,
    [CASC_T_ACK]        = _cmd_ack,
    [CASC_T_NACK]       = _cmd_nack,
};

/* ================================================================
 *  取帧与排空
 *
 *  **等待期间必须主动排空**：队列深度 2，而框架在队列满时**静默丢帧**
 *  （app_dispatch.c 的 osMessageQueuePut 超时为 0）—— 丢掉的若是 ACK，
 *  现场表现与"从卡没应答"完全一样，只能白等一轮重传。
 *
 *  一帧一轮之后实际压力很小（主卡那边任何时刻最多一条 ACK 在队列里），
 *  所以排空只是"等应答时顺手做掉的事"，不再是分片时期那种必须连轴转的节奏。
 * ================================================================ */

/** @brief 取一条帧（最多等 timeout_ms）并按类型分派；返回是否有帧被处理 */
static bool _casc_pump(uint32_t timeout_ms)
{
    static uint8_t     msg_buf[CASC_MSG_SIZE] __attribute__((aligned(4)));
    frame_msg_t *const msg = (frame_msg_t *)msg_buf;

    if (osMessageQueueGet(s_casc_queue, msg, nullptr, timeout_ms) != osOK) return false;

    const uint8_t type = msg->aux;
    if (type <= CASC_TYPE_MASK && g_casc_cmd[type]) g_casc_cmd[type](msg);
    return true;
}

static void _casc_drain(void)
{
    while (_casc_pump(0)) { }
}

/** @brief 等 `from` 那张卡对本轮 `seq` 的应答；返回 false = 超时
 *
 *  **在画布守卫之外**：识别流程（`SET_ADDR` 等 ACK）也要用它，而无画布的板子
 *  （BOARD_SCREEN_CANVAS=0）同样会参与识别。
 *
 *  **等待期间持续分派**：队列深度只有 2，不排空的话后到的 ACK 会被丢掉，而
 *  "ACK 丢了"与"卡没应答"在现场分不开 —— 两者都表现为这张卡超时。
 *  按 (seq, src) 双重匹配：总线上会有别张卡的应答与上一轮的迟到应答。 */
static bool _wait_ack(uint8_t from, uint16_t seq, uint32_t deadline)
{
    const uint32_t t0 = osKernelGetTickCount();

    for (;;) {
        _casc_drain();
        if (s_ack.valid && s_ack.seq == seq && s_ack.src == from) {
            /* **把等了多少打出来**：这是"超时该定多少"的唯一依据。40ms 那个数当初是
               估的，结果实测有 87ms 的回合 —— 以后照这行调，不再拍脑袋。 */
            CASC_LOG("[casc·主] ← 等应答 %u ms\n", (unsigned)(osKernelGetTickCount() - t0));
            return true;
        }
        if ((int32_t)(osKernelGetTickCount() - deadline) >= 0) return false;
        osDelay(CASC_ACK_POLL_MS);
    }
}

#if BOARD_SCREEN_CANVAS

/* ================================================================
 *  开轮（主卡侧）
 *
 *  一轮 = 把**整屏**的新内容分发给每张从卡（各发它那一块，一帧装下），全部结算完后
 *  主卡自己也换帧。刷新需求是指令式的（几秒~几分钟一次），所以一轮 ~135ms/卡
 *  （1427 字节 @115200 的 124ms + 应答往返）完全够用 —— 这也是不做差分模式的前提。
 * ================================================================ */

/** @brief 单张从卡的一轮：发一条 IMAGE（含整块位图）→ 等应答 → 失败则整帧重发
 *
 *  **逐卡走完再下一张**：位图要占住发送帧里那块 1400 字节，逐卡走完就只需一块缓冲。
 *
 *  **失败是怎么被看见的**：ACK 到不了、NACK 被明确回绝、超时 —— 三种都归结为
 *  "这一轮这张卡没对齐"，由 `_round_run` 记失败轮数、到阈值剔除。 */
static bool _round_one_card(uint8_t idx, uint16_t seq, uint8_t bright, bool persist)
{
    const screen_card_t *c       = app_screen_card(idx);
    const uint16_t       bmp_len = app_screen_card_bm_len(idx);
    if (!c || !bmp_len) return false;

    /* 身份在本轮开始后换了：本项的 dst/矩形是按**旧的**本卡身份定位的，发出去就是错的 */
    if (s_round_me != app_screen_self_addr()) return false;

    /* **位图直接抽进发送帧的载荷位置**：抽出来的格式（1bpp、行优先、MSB-first、
       末字节补位归零）与线上格式逐位一致，所以既不需要中间缓冲，也不用再 memcpy 一次。 */
    casc_image_t  *p   = (casc_image_t *)(s_tx + sizeof(casc_hdr_t));
    const uint16_t cap = (uint16_t)(sizeof(s_tx) - sizeof(casc_hdr_t) - sizeof(casc_image_t));

    if (cap < bmp_len || !app_screen_extract(idx, p->bitmap, cap)) return false;

    casc_put_u16(p->x, c->x);
    casc_put_u16(p->y, c->y);
    casc_put_u16(p->w, c->w);
    casc_put_u16(p->h, c->h);
    casc_put_u16(p->bmp_len, bmp_len);
    p->bright  = bright;
    p->color   = app_screen_output_color(c->color); /* 工厂逐色老化时会临时统一 */
    /* 上位机"这次内容要长期保留"的意图原样传到从卡：各卡各存自己那一块 */
    p->persist = persist ? 1U : 0U;

    const uint16_t len = _finish_frame(s_tx, sizeof(s_tx), CASC_T_IMAGE, c->addr, seq,
                                       (uint16_t)(sizeof(casc_image_t) + bmp_len));
    if (!len) return false;

    for (uint8_t attempt = 0; attempt <= CASC_RETRY_MAX; attempt++) {
        if (attempt) {
            /* **整帧重传**：一帧就是一幅完整画面，没有"只补缺的那几片"这种粒度。
               代价 124ms 的总线时间，换掉整套缺片追踪与缺片位图。 */
            app_screen_note_retrans();
            CASC_LOG("[casc·主] seq=%u 卡%u 第%u次整帧重发\n", (unsigned)seq, (unsigned)c->addr,
                     (unsigned)(attempt + 1));
        }

        /* 应答槽要在**发之前**清：ACK 可能在这条帧还没收完时就回来了，而它是被
           `_wait_ack` 取走的 —— 若在发之后清，清掉的正是刚收到的那一条。 */
        s_ack.valid = false;
        (void)_send_tx(len, CASC_T_IMAGE);

        const bool got_ack = _wait_ack(c->addr, seq, osKernelGetTickCount() + CASC_ACK_TIMEOUT_MS);

        /* 等应答期间身份可能被改了（收到识别帧 / 按键认领）：**立刻收尾**，
           不再重发、也不按旧身份结算。这里放在超时判断之前 —— 两条路都要拦。 */
        if (s_round_me != app_screen_self_addr()) {
            CASC_LOG("[casc·主] 轮次中途本卡身份变了（addr=%u），本轮作废\n",
                     (unsigned)app_screen_self_addr());
            return false;
        }

        if (!got_ack) {
            /* 超时 = 这张卡没应答，或者 ACK 丢了 —— 现场分不开，只能整帧重发 */
            CASC_LOG("[casc·主] seq=%u 卡%u ← 等应答超时（%ums）\n", (unsigned)seq,
                     (unsigned)c->addr, (unsigned)CASC_ACK_TIMEOUT_MS);
            continue;
        }

        if (s_ack.sta == CASC_STA_NACK) {
            /* 从卡明确回绝（矩形与本卡切分表不符 / 本卡地址不在表里）—— 重发没有意义。
               它每回绝一次，`_round_run` 的失败计数照常累加，所以配置错的卡最终会被剔除；
               而在此之前，这行日志是唯一能指出"是配置问题不是链路问题"的东西。 */
            CASC_LOG("[casc·主] seq=%u 卡%u 拒绝本轮（err=%u）—— 多半是两块板的切分表/地址不一致\n",
                     (unsigned)seq, (unsigned)c->addr, (unsigned)s_ack.err);
            return false;
        }
        return true; /* ACK：这一轮完成 */
    }

    printf("[casc] 卡 %u 本轮未完成（重发 %u 次仍无应答）—— 其余卡与主卡照常更新\n",
           (unsigned)c->addr, (unsigned)CASC_RETRY_MAX);
    return false;
}

/** @brief 开一轮：逐张从卡下发它那一块，全部结算完后主卡自己也换帧 */
/** @brief 现在该开一轮吗？（`casc_task` 周期活里的那一格，单独成函数便于 host 测）
 *
 *  三个闸，各自对应一种"这一眼的内容不对"：
 *   · **画布没被写过就不开**（`canvas_touched`）：上电时画布上只有本卡那一块是从
 *     记录恢复来的，推下去会把从卡刚恢复的内容刷黑。
 *   · **正在渲染就不开**（`app_render_busy`）：渲染是"测量趟 + 渲染趟"，文字还要
 *     逐字读字库（SPI，几十毫秒）。中途看一眼，画布上只有一半 —— 或者刚好是
 *     "已经清屏、文字还没画"的那一瞬间，推下去就是现场看到的"闪一下"。
 *   · **静默期**：内容连改几次（老化轮播就是）时，攒到不再变再推一轮。
 *
 *  `s_force_round`（有卡刚上线）**不跳过前两个闸**：它要的正是"一张完整的画布"。 */
static bool _round_ready(void)
{
    if (!app_screen_canvas_touched()) return false;
    if (app_render_busy()) return false;

    if (s_force_round) {
        s_force_round = false; /* 有卡刚上线：立刻给它当前内容 */
        return true;
    }
    return app_screen_take_pending_settled();
}

static bool _round_run(void)
{
    const screen_layout_t *L      = app_screen_layout();
    const uint8_t          me     = app_screen_self_addr();
    const uint16_t         seq    = ++s_round_seq;

    /* 记下本轮的身份：中途别人改了它，这一轮就作废（见 s_round_me 的说明） */
    s_round_me = me;
    /* **peek 而不是 take**：本轮的帧在落屏**之前**发出去，取走要等到最后的
       `app_screen_commit_self()`（它才是那个请求位的消费者）。 */
    const bool             persist = app_render_peek_persist_req();
    const uint8_t          bright = app_screen_get_brightness();
    bool                   all_ok = true;

    CASC_LOG("[casc·主] 开轮 seq=%u（共 %u 卡，本卡 addr=%u）\n", (unsigned)seq,
             (unsigned)L->count, (unsigned)me);
    app_screen_note_round(seq);

    for (uint8_t i = 0; i < L->count; i++) {
        /* 身份在本轮中途换了：后面那些卡一项都不许发（它们的 dst 也是按旧身份定的） */
        if (s_round_me != app_screen_self_addr()) {
            all_ok = false;
            break;
        }

        const screen_card_t *c = app_screen_card(i);
        /* **按下标遍历，地址与矩形都从同一项里取** —— 两者顺序可以不同
           （主卡在下时 addr 与下标相反），拿地址当下标会把两块屏的内容对调。
           本卡那块不下发，由下面的本地提交处理。 */
        if (!c || c->addr == me) continue;

        /* **不在线的卡直接跳过**：发过去也要等满重试与超时（一轮从 ~130ms 涨到
           ~250ms），而它那块屏本来就不会更新 —— 各卡各带一整块屏，主卡既改不到
           别人的屏，也没法命令一张掉线的卡清屏。"替它涂黑"在这里是做不到的事，
           做了只会把内容毁掉（卡回来时拿到的是黑的）。
           让它回来的是枚举，不是往黑洞里发数据。 */
        if (app_screen_card_state(i) != SCREEN_CARD_ONLINE) {
            /* **没上线的卡也算"这轮没对上"**：否则从卡晚几秒起来时，对齐全在
               "没人应答"的状态下判成成功、只走一轮就收尾 —— 上电同步就**永远不会发生**，
               从卡要靠之后某次 force_round 碰巧补上（实测就是这样）。
               代价是配置错（地址写重/卡没上电）时要白等满 5 次重试，5 秒，可接受。 */
            all_ok = false;
            continue;
        }

        if (_round_one_card(i, seq, bright, persist)) {
            s_fail_run[i] = 0;
        } else {
            all_ok = false; /* 上电对齐据此重试；剔除与否是另一件事 */
            /* 身份变了的那次失败**不算这张卡的**：它不是没应答，是这一轮作废了。
               记进去的话，几轮认领之后会把一张好卡剔掉，然后要等枚举才叫得回来。 */
            if (s_round_me != app_screen_self_addr()) break;
            if (++s_fail_run[i] >= CASC_FAIL_RUN_MAX) {
                CASC_LOG("[casc·主] 卡 %u 连续 %u 轮未完成 → 剔除（不再发数据，等枚举找回来）\n",
                         (unsigned)c->addr, (unsigned)s_fail_run[i]);
                app_screen_card_set_state(i, SCREEN_CARD_OFFLINE);
                s_fail_run[i] = 0;
            }
        }
    }

    /* **本地提交放在最后**：主卡自己那块也换到新画面。走的是与从卡完全相同的
       "抽本卡矩形 → commit_bitmap"那条路（app_screen_commit_self），所以主卡屏上的
       内容与从卡收到的出自同一个 app_screen_extract。直接交整块画布不行：多卡时
       画布比本卡屏大，长度对不上，commit_bitmap 会拒绝。

       代价是主卡的换帧时机被总线节奏绑住（一轮 ~130ms/卡）。本项目是交通屏、
       只显示静态文字与标识，指令式刷新，所以不为此另开"只更新主卡本地"的快路径。

       **身份在本轮中途变了就不提交**：那时门面已被重装（画布清空、本卡那一块的
       矩形也换了），提交上去是把一张空画面推到屏上。 */
    if (s_round_me == app_screen_self_addr()) (void)app_screen_commit_self();

    return all_ok; /* 只要有一张从卡没完成，上电对齐就还要再试 */
}

#endif /* BOARD_SCREEN_CANVAS */

/* ================================================================
 *  身份：从哪读
 *
 *  **状态在 app_screen、策略在这里**：`app_screen_self_addr()` 存值并按新值重装门面，
 *  而"该取哪个值、记在哪"是部署事实（拨码/记录/默认），属于本模块。
 *
 *  优先级 **拨码（仅 3833024）> W25Qxx 记录 > 板级默认（= 主卡）**：
 *  · 有拨码的板子每次现读 —— 现场拨一下、按一下键就生效，不落盘；
 *  · 没有拨码的（5006048）按键认领后写记录，掉电不忘；
 *  · 记录也没有就是出厂状态，做**主卡** —— 一块板单独上电必须是一台可用设备
 *    （默认从卡会让它"地址不在切分表里"→ 门面直接停用，等于砖）。
 *
 *  **跑在 `sw_app(3)`（`_screen_init` 是 `sw_dev(2)` 之后）**：那时切分表已按默认身份
 *  建过一遍，这里解析出真身份后再 `reinit_identity()` 重装一次 —— 两条路走的是
 *  同一个 `_apply_identity()`，不会漂。 */
#define CASC_ID_REC_VERSION (2U)

typedef struct [[gnu::packed]] {
    uint8_t addr;        /**< 0 = 主卡，1..0x1F = 从卡 */
    uint8_t src;         /**< 谁定的：0=默认 1=记录 2=拨码 3=识别帧（只为排障打印） */
    uint8_t master_cell; /**< 本机认定的**主卡格**（哪一格编 addr 0）—— 见 app_screen.h */
    uint8_t rsv;
} casc_id_rec_t;

_Static_assert(sizeof(casc_id_rec_t) == 4, "身份记录载荷必须是 4 字节");

static uint8_t s_id_cfg = 0xFF; /* 记录句柄（注册失败即 0xFF，本次上电不落盘） */

/** @brief 本板有没有拨码（**不是**"拨码读到了几"） */
static bool _id_dip_present(void)
{
#if BOARD_HAS_ADDR_DIP
    return dev_key_get(DEV_KEY_DIP1) != nullptr;
#else
    return false;
#endif
}

/** @brief 拨码读数：DIP1 = bit0，ON = 低电平 = 1（极性见 dev_key 的 _dip_get_state） */
static uint8_t _id_dip(void)
{
#if BOARD_HAS_ADDR_DIP
    return (uint8_t)((dev_key_get_state(DEV_KEY_DIP1) ? 1U : 0U) |
                     (dev_key_get_state(DEV_KEY_DIP2) ? 2U : 0U));
#else
    return 0;
#endif
}

/** @brief 写身份记录（地址 + 主卡格）：**同址确认也要写**
 *
 *  以前只有"真改了地址"才写，于是从卡（被通知的地址与它自己一样）永远没有记录，
 *  它的身份一直靠出厂默认值撑着 —— 默认值一改、或换一块板，它就跑到别的格上去了。 */
static void _id_save(uint8_t addr, uint8_t master_cell, uint8_t src)
{
    if (s_id_cfg == 0xFF) return;
    const casc_id_rec_t r = {
        .addr        = addr,
        .src         = src,
        .master_cell = master_cell,
    };
    if (app_cfg_sched_save(s_id_cfg, (const uint8_t *)&r, sizeof(r)) != 0)
        printf("[casc·id] 身份记录写入失败（本次上电仍按 %u 跑）\n", (unsigned)addr);
}

/** @brief 记下身份（地址 + 主卡格）并立刻生效（不重启）。拨码优先 → 冲突时拒绝 */
static bool _id_set_local(uint8_t addr, uint8_t master_cell, uint8_t src, bool persist)
{
    if (_id_dip_present() && addr != _id_dip()) {
        printf("[casc·id] 本板有拨码（=%u），拒绝把地址改成 %u（拨码优先）\n",
               (unsigned)_id_dip(), (unsigned)addr);
        return false;
    }
    /* 记录必须在 apply 之前写：apply 会把主卡格换成新的，而记录要记的是"新的那一份" */
    if (persist) _id_save(addr, master_cell, src); /* 主卡格用**参数**：此刻 app_screen 里还是旧的 */


    /* **一个都没变就什么都不做**（`app_screen_apply_identity` 内部的判断）：
       重装门面会按新身份重建表、清画布与闩 —— 上电那一次与"同址确认"都是这种
       情况，白清一次内容（工厂测试第一次按键要显示的编码就是这么丢的）。 */
    app_screen_apply_identity(addr, master_cell);
    return true;
}

/** @brief 解析身份：地址（`*mc` 带回记录里的主卡格） */
static uint8_t _id_resolve(uint8_t *src, uint8_t *mc)
{
    *mc = (uint8_t)BOARD_CASCADE_MASTER_CELL; /* 默认：板级配置里那一格 */

    if (_id_dip_present()) {
        *src = 2;
        return _id_dip(); /* 每次现读：现场拨一下即生效 */
    }

    casc_id_rec_t r;
    uint16_t      n = 0;
    if (s_id_cfg != 0xFF &&
        app_cfg_sched_load(s_id_cfg, (uint8_t *)&r, sizeof(r), &n) == CFG_REC_OK &&
        n == sizeof(r) && r.addr <= 0x1FU) {
        /* 标签只能报"**记录**"：`r.src` 是这条记录**当初被写下来时**的来源
           （认领写的是 3=识别帧），拿它当"这次从哪读的"会打出一句
           `本机地址=0（来源 识别帧）` —— 看起来像开机时跑了一次识别，其实是读记录。 */
        *src = 1;
        *mc  = r.master_cell;
        return r.addr;
    }

    *src = 0;
    return (uint8_t)BOARD_CASCADE_ADDR; /* 板级默认（= 从卡） */
}

/** @brief 上电：解析身份 → 应用 → 打印 */
static void _casc_id_boot(void)
{
    uint8_t       src  = 0;
    uint8_t       mc   = 0;
    const uint8_t addr = _id_resolve(&src, &mc);

    app_screen_apply_identity(addr, mc);

    static const char *const k_src_name[] = {"板级默认", "记录", "拨码", "识别帧"};
    printf("[casc·id] 本机地址=%u（来源 %s）→ %s\n", (unsigned)addr,
           k_src_name[src < 4U ? src : 0U], app_screen_is_master() ? "主卡" : "从卡");
}
sw_app_initcall(_casc_id_boot);

/** @brief 注册身份记录（`sw_dev(2)`：只为拿句柄；真正的读在 `_casc_id_boot` 惰性做） */
static const cfg_sched_desc_t s_id_desc = {
    .name    = "casc_id",
    .version = CASC_ID_REC_VERSION,
    .load    = NULL, /* 不在加载遍里应用身份：那条路早于 _screen_init，装不了门面 */
};

static void _casc_id_register(void)
{
    s_id_cfg = app_cfg_sched_register(&s_id_desc);
    if (s_id_cfg == 0xFF) printf("[casc·id] 身份记录注册失败，本次上电身份不持久化\n");
}
sw_dev_initcall(_casc_id_register);

/* ================================================================
 *  主从识别（识别帧的收发与认领）
 *
 *  **谁能改身份**：只有"按下按键的那张卡"是发起者（`mine=0`），接收方从不自封主卡
 *  （`yours` 恒非 0）→ 重放/迟到/回声都造不出第二张主卡。两张卡都自称主卡时由
 *  `claim` 时刻裁决（更早者胜）—— 两卡做同一个比较，结论一致。
 * ================================================================ */

/** 本卡最近一次认领主卡的时刻；`0xFFFFFFFF` = 从未认领（最晚，别人优先） */
static uint32_t s_my_claim = 0xFFFFFFFFU;

/** @brief 身份变了之后把协议侧的陈旧状态全部作废
 *
 *  **必须做**：切分表里的"谁在线"是按**旧地址**建立的，一轮的 dst 也是按旧身份算的。
 *  不 poke 的话新主卡要等下一个 10 秒枚举周期才被发现，而新从卡还在用旧地址发 ACK
 *  （源地址对不上，主卡永远超时）。 */
static void _identity_poke(void)
{
    const screen_layout_t *L = app_screen_layout();

    for (uint8_t i = 0; i < L->count; i++) {
        app_screen_card_set_state(i, SCREEN_CARD_MISSING); /* 重新枚举才知道谁在 */
        s_fail_run[i] = 0;                                 /* 旧的失败计数同样作废 */
    }

    s_ping_left       = 0;
    s_enum_deadline   = 0;
    s_enum_verdict    = false;                  /* 运行期枚举，不必下"有没有卡"的判据 */
    s_reenum_at       = osKernelGetTickCount(); /* 下一圈立刻开始枚举 */
    s_force_round     = false;
    s_ack.valid       = false;
    s_bus_quiet_until = 0; /* 认领期间可能刚发过帧，别让自己再等 300ms */
}

/** @brief 收到识别帧：按 `mine`/`yours`/`master_cell`/`claim` 决定改不改自己的身份 */
static void _cmd_set_addr(frame_msg_t *msg)
{
    if (msg->data_len != (uint16_t)(CASC_OVERHEAD + sizeof(casc_set_addr_t))) return;

    const casc_hdr_t      *h = (const casc_hdr_t *)msg->data;
    const casc_set_addr_t *p = (const casc_set_addr_t *)(msg->data + sizeof(casc_hdr_t));
    const uint16_t         seq = casc_get_u16(h->seq);

    const uint32_t claim = casc_get_u32(p->claim);
    const uint8_t  mine  = p->mine;
    const uint8_t  yours = p->yours;
    const uint8_t  mc    = p->master_cell;

    /* **自己的回声**（半双工）：丢弃，且不能回 ACK —— 否则会把发起方的等待槽当成
       "对方的答复"。判据是"地址 + claim 都是我自己"：两张卡都自称主卡时两个地址
       都是 0，光看 src 分不开，而 claim 是各按各的时刻。 */
    if (h->src == app_screen_self_addr() && claim == s_my_claim) return;

    /* ① 对方不是主卡 → 不动作。**这一条守住"整个装置不会没有主卡"**：
       一张从卡（例如拨码≠0 的板子）按了键，不能把唯一的主卡降级。 */
    if (mine != CASC_ADDR_MASTER) {
        CASC_LOG("[casc] ← 识别帧来自非主卡（mine=%u），忽略\n", (unsigned)mine);
        _nack(seq, CASC_NACK_ADDR, "对方不是主卡（mine!=0）");
        return;
    }

    const uint8_t old_mc   = app_screen_master_cell();
    const uint8_t self     = app_screen_self_addr();
    /* 我这一格（**按旧表算**：新主卡格一应用，地址与格的对应就变了） */
    const uint8_t my_cell  = app_screen_cell_of_addr(self, old_mc);

    uint8_t       new_addr;
    if (yours == CASC_ADDR_SELF_CALC) {
        if (my_cell == mc) {
            /* **我也认为自己在主卡格，但发起方不是我** —— 两张卡都自称主卡的那种局面。
               让不让由 claim 裁决：**更晚按下者作数**（两卡对同一对数做同一个比较，
               结论一致，不会两张都让）。从没按过（0xFFFFFFFF）的一律让。 */
            if ((int32_t)(claim - s_my_claim) < 0) {
                CASC_LOG("[casc] ← 对方的 claim=%ums 早于本卡，回绝（本卡留主卡）\n",
                         (unsigned)claim);
                _nack(seq, CASC_NACK_ADDR, "本卡按得更晚，主卡位归本卡");
                return;
            }
            /* 该让到哪一格：**第一个不是主卡格的格**。两卡设备上这就是另一块屏 ✓；
               四卡时是个猜测（打印出来，别静默）。 */
            const uint8_t spare_cell = (mc == 0U) ? 1U : 0U;
            new_addr                 = app_screen_addr_of_cell(spare_cell, mc);
            printf("[casc] ← 两张卡都自称主卡；本卡让位到 addr=%u（按第 %u 格算）\n",
                   (unsigned)new_addr, (unsigned)spare_cell);
        } else {
            new_addr = app_screen_addr_of_cell(my_cell, mc); /* 格不动，只按新表重算地址 */
        }
    } else {
        /* 明确的地址（逐卡单播）。`yours == 0` 只在"本卡就是主卡格"时才对得上，
           那时下面 apply 的 addr==0 与 mc 一致，等于只更新主卡格。 */
        if (yours != CASC_ADDR_MASTER && yours != self && app_screen_is_master() &&
            (int32_t)(claim - s_my_claim) < 0) {
            /* 我也自称主卡、而且**我按得更晚**（claim 更大 ⇒ 差为负）→ 我不让位，
               只回绝。两卡对同一对数做同一个比较，结论一致，不会两张都让。 */
            CASC_LOG("[casc] ← 对方的 claim=%ums 早于本卡，回绝（本卡留主卡）\n",
                     (unsigned)claim);
            _nack(seq, CASC_NACK_ADDR, "本卡按得更晚，主卡位归本卡");
            return;
        }
        new_addr = yours;
    }

    /* ② 改成算出来的地址 + 新的主卡格并持久化。**哪怕原来就是主卡，收到也照改**
       （用户定的规则："谁被按谁主卡"）。有拨码的板子由 _id_set_local 拒绝 → NACK。 */
    if (!_id_set_local(new_addr, mc, 3U, true)) {
        _nack(seq, CASC_NACK_ADDR, "本板有拨码，地址以拨码为准");
        return;
    }
    _identity_poke();
    (void)_send_seq(CASC_T_ACK, CASC_ADDR_MASTER, seq, nullptr, 0);
}

/** @brief 认领之后：把"我是主卡、主卡格是 M"告诉其余每一张卡；返回收到 ACK 的卡数
 *
 *  **先广播再逐卡单播**，两件事各有不可替代的用处：
 *   · **广播**（`yours = 0xFF`，让各卡按自己的格自算）：发起方**未必知道总线上有谁** ——
 *     两张卡都自称主卡时，谁也枚举不到谁（主卡不应答 PING），逐卡单播无从下手；
 *     而总线是共享的，一条广播一定到得了。这也是"整个装置不会卡在两张主卡"的关键。
 *   · **逐卡单播**（具体地址）：表里认得的卡要一个个等 ACK —— 那才是"确实通知到了"
 *     的证据，广播没法等（N 张卡回 N 条，`_wait_ack` 按 (seq,src) 匹配无从匹配）。
 *     dst 用**旧表**里的地址（对端此刻还在旧地址上听着），`yours` 给**新表**里那个。 */
static uint8_t _claim_notify_peers(const uint8_t *addr_old, uint8_t count)
{
    const uint8_t me = app_screen_self_addr();
    const uint8_t mc = app_screen_master_cell();
    uint8_t       ok = 0;

    /* ---- ① 广播：谁在听谁自算（含"另一张自称主卡"的那张） ---- */
    {
        casc_set_addr_t p = {.mine = CASC_ADDR_MASTER, .yours = CASC_ADDR_SELF_CALC,
                             .master_cell = mc};
        casc_put_u32(p.claim, s_my_claim);
        if (_send_seq(CASC_T_SET_ADDR, CASC_ADDR_BCAST, ++s_round_seq, &p, sizeof(p)) >= 0)
            ++ok; /* 广播只要发出去就算一份：它到得了谁，对端自己会 ACK/让位 */
    }

    /* ---- ② 逐卡单播：按旧地址点名叫，等它的 ACK ---- */
    uint8_t unicast_ok = 0;
    for (uint8_t i = 0; i < count; i++) {
        /* **按下标（格）跳过自己**，不是按地址比 —— 此刻自己的地址已经是新的 0，
           而 `addr_old[]` 里存的是旧地址，两者不可比（比错的后果：该通知的旧主卡
           被当成"自己"跳过，而不该通知的旧地址收到一条发给别人的帧）。 */
        if (i == mc) continue; /* mc == 本卡那一格（认领之后就是主卡格） */

        const uint8_t dst = addr_old[i];

        casc_set_addr_t p = {.mine = CASC_ADDR_MASTER,
                             .yours = app_screen_addr_of_cell(i, mc), /* 新表里它在几号 */
                             .master_cell = mc};
        casc_put_u32(p.claim, s_my_claim);

        for (uint8_t attempt = 0; attempt <= CASC_RETRY_MAX; attempt++) {
            const uint16_t seq = ++s_round_seq; /* 借用同一个序号源：ACK 按 (seq,src) 匹配 */
            s_ack.valid        = false;
            if (_send_seq(CASC_T_SET_ADDR, dst, seq, &p, sizeof(p)) < 0) break;

            /* 对端改完地址后用**新地址**回 ACK（它就是从这个 seq 上答的） */
            if (!_wait_ack(p.yours == CASC_ADDR_MASTER ? dst : p.yours, seq,
                           osKernelGetTickCount() + CASC_ACK_TIMEOUT_MS))
                continue;

            if (s_ack.sta == CASC_STA_NACK) {
                printf("[casc·主] 卡 %u 拒绝识别帧（err=%u）—— 要么它那块的拨码与切分表"
                       "对不上，要么它按得比本卡更晚\n",
                       (unsigned)dst, (unsigned)s_ack.err);
            } else {
                unicast_ok++;
            }
            break;
        }
    }

    printf("[casc·主] 认领完成：广播 1 条 + %u/%u 张卡单播确认（本卡 addr=%u 主卡格=%u）\n",
           (unsigned)unicast_ok, (unsigned)(count > 0 ? count - 1U : 0U), (unsigned)me,
           (unsigned)mc);
    return (uint8_t)(ok + unicast_ok);
}

/** @brief 认领的实际动作：自己变主卡（写记录）→ 作废陈旧状态 → 通知其余卡 */
static void _claim_run(void)
{
    s_my_claim = osKernelGetTickCount();

    /* ---- 先把旧表抄下来：应用新身份之后，旧地址就查不到了 ---- */
    uint8_t       addr_old[SCREEN_CARD_MAX] = {0};
    const uint8_t count                     = app_screen_layout()->count;
    for (uint8_t i = 0; i < count && i < SCREEN_CARD_MAX; i++)
        addr_old[i] = app_screen_card(i)->addr;

    /* ---- 我这一格 = 新主卡格（**"谁被按谁主卡"的落点**） ---- */
    const uint8_t my_cell = app_screen_cell_of_addr(app_screen_self_addr(),
                                                    app_screen_master_cell());
    if (my_cell >= count) {
        printf("[casc·id] 本卡地址 %u 不在切分表里，按键不改变身份\n",
               (unsigned)app_screen_self_addr());
        return;
    }

    if (!_id_set_local(CASC_ADDR_MASTER, my_cell, 3U, true)) {
        /* 有拨码的板子按键不能选主卡（拨码优先的必然推论）：身份保持拨码给的值，
           这一下**到此为止** —— 继续往下发的话，帧里的 mine 是拨码值（≠0），
           接收侧一律按"对方不是主卡"回绝，白占总线还打出一句方向错的日志。 */
        printf("[casc·id] 本板有拨码（=%u），地址由拨码定，按键不改变身份；"
               "要选主卡请把拨码拨到 0\n",
               (unsigned)_id_dip());
        return;
    }
    printf("[casc·id] 认领主卡：本卡成为 addr=0，主卡格=%u\n", (unsigned)my_cell);

    _identity_poke(); /* 先作废陈旧状态，再通知（通知要等 ACK，占着总线） */
    (void)_claim_notify_peers(addr_old, count);
}

/** @brief 按键请求位（由 `app_cascade_claim_master` 置、`casc_task` 取） */
static volatile bool s_claim_req;

void app_cascade_claim_master(void)
{
    s_claim_req = true;
}

/** @brief 取走按键请求并执行认领 —— `casc_task` 里那一格
 *
 *  **单独成函数**是为了能在 host 上测：`casc_task` 是死循环，用例没法进去看认领
 *  到底做了什么（写记录、重装门面、逐卡通知）。 */
static void _claim_poll(void)
{
    if (!s_claim_req) return;
    s_claim_req = false;
    _claim_run();
}

/* ================================================================
 *  任务
 * ================================================================ */

static void casc_task(void *argument)
{
    (void)argument;

    /* 上电先等 3 秒（从卡通常比主卡晚就绪），之后由"到点重新枚举"那条驱动 */
    s_reenum_at = osKernelGetTickCount() + CASC_BOOT_PING_DELAY_MS;

    for (;;) {
        /* 阻塞等第一条（超时取轮询周期而不是 osWaitForever：本任务还兼着周期活，
           等不到帧也要醒过来），取到就把队列**一次排空** —— 见 _casc_pump 的说明。 */
        if (_casc_pump(CASC_BRIGHT_POLL_MS)) _casc_drain();

        uint32_t now = osKernelGetTickCount();

        /* 上电枚举的收卷：到点报一句"有没有卡应答" —— 没有应答时后面那些
           "本轮未完成 / 等应答超时"是必然的，这里先把话说在前面，免得白查协议。 */
        if (s_enum_deadline && (int32_t)(now - s_enum_deadline) >= 0) {
            if (s_enum_verdict)
            CASC_LOG("[casc·主] 上电枚举：%s\n",
                     s_enum_seen ? "有卡应答（从卡在，链路通）"
                                 : "连发多次 PING 都没有应答 —— 从卡没上电/没接 485/没烧这份"
                                   "固件，或总线方向与接线不对；后面那些「本轮未完成」都是必然的");
            s_enum_deadline = 0;
        }

        /* 上电枚举：从卡通常比主卡晚就绪（等待各自的初始化），故延后 3 秒起发，
           没等到应答就再发 —— 见 CASC_PING_TRIES 的说明。
           运行期的重新枚举（拔插、掉线恢复）留到 P4。 */
        if (app_screen_is_master() && !s_ping_left && s_enum_deadline == 0 &&
            (int32_t)(now - s_reenum_at) >= 0) {
            _enum_start(now);
        }
        if (app_screen_is_master() && s_ping_left && (int32_t)(now - s_ping_next) >= 0) {
            s_ping_left--;
            s_enum_seen = false; /* 只认**这一轮**发出去之后的应答 */
            /* 只在上电那次报：周期性重新枚举每 10 秒一次，报出来会把 RTT 缓冲冲掉，
               而那个信息本来也没用（PRESENT 那行说明了） */
            if (s_enum_verdict) CASC_LOG("[casc·主] 枚举 PING（还剩 %u 次）\n", (unsigned)s_ping_left);
            (void)app_cascade_ping();
            /* 从卡马上会回 PRESENT —— 这段时间主卡不能再发别的（半双工） */
            s_bus_quiet_until = now + CASC_BOOT_ALIGN_PING_GAP_MS;
            s_ping_next       = now + CASC_PING_RETRY_MS;
            if (!s_ping_left) _enum_finish(now); /* 次数用尽：不再等 */
        }
        /* 收到应答就提前收尾，不必把重试次数耗完 */
        if (app_screen_is_master() && s_ping_left && s_enum_seen) _enum_finish(now);

        /* 亮度待下发：光传感器每秒钟都可能改，这里把"变了"攒成一次广播。
           广播本身**不要求应答** —— 亮度差一帧不可见，且它天然是渐变量。
           "从卡永久停在旧亮度"的防线是 P3 起每一轮 SYNC_BEGIN 都带 bright 重新断言。 */
        uint8_t lv;
        if (app_screen_is_master() && app_screen_brightness_take_pending(&lv)) {
            (void)app_cascade_broadcast_bright(lv);
        }

        /* ---- 按键认领主卡 ----
         * 请求由 `app_cascade_claim_master()`（按键所在的任务）投递，实际动作在这里做：
         * 写身份记录、重装门面、逐卡通知。**不判主从** —— "谁被按谁主卡"里就包含
         * "本来是从卡、按一下变成主卡"这一半。 */
        if (s_claim_req) {
            _claim_poll();
            now = osKernelGetTickCount(); /* 认领最长约 1s，后面按新时刻算 */
        }

#if BOARD_SCREEN_CANVAS
        /* 多卡主卡：一轮 = 一次整屏更新，**落屏由它统一做**（app_screen 自己的
           静默期任务在多卡时不启动，见 app_screen.c 的 _screen_init）。
           单卡时整屏就是本卡自己，走那个任务更直接，这儿不成立。
           放在周期活之后：一轮约 130ms/卡，跑起来本任务就顾不上别的了。 */
        /* 总线安静期内不开轮：见 s_bus_quiet_until 的说明。

           **`app_screen_canvas_touched()` 是本轮的关键闸**：上电时画布上只有本卡
           那一块是从记录恢复来的、其余区域是空的 —— 这时候开轮就是把一张不全的
           画布推下去，把从卡刚恢复的内容刷黑。闸住之后：整机掉电重启 → 一轮都不开、
           各卡显示各自记录的内容；上位机一下发内容 → 画布被重画 → 正常同步整屏；
           某张卡掉线回来时画布早被写过，`s_force_round` 照常补内容。 */
        if (app_screen_is_master() && app_screen_layout()->count > 1 &&
            (int32_t)(now - s_bus_quiet_until) >= 0) {
            if (_round_ready()) (void)_round_run();
        }
#endif
    }
}


/* ================================================================
 *  注册
 * ================================================================ */

static const pcb_ops_t s_casc_ops = {.probe = casc_probe_frame};

static void _cascade_init(void)
{
    pcb_t *p = &s_casc_pcb;
    p->name        = "cascade";
    p->ops         = &s_casc_ops;
    p->rb          = &s_casc_rb;
    /* 本协议最长的一帧 = 整块位图那一帧。队列元素按它定，见 CASC_QUEUE_DEPTH 的说明。 */
    p->payload_max = CASC_MSG_MAX;

    /* **设备内部总线**：本协议的流量全在机箱里（板间 485），不是上位机下发的。
       置了这一位，它的有效帧就不触发接收事件监听 —— 否则老化测试跑起来之后，
       每轮的 ACK/PRESENT 都会触发一次"收到上位机数据"→ 工厂模式当场退出
       （现场表现：按第一下之后测试再也推不动）。 */
    p->internal_bus = true;

    /* 发送缓冲必须 DMA 可达（RS485 按这个在 DMA 与轮询之间二选一，见 app_rs485.c）。
       真被挪进 CCMRAM 也不会报错，只会静默退化成 124ms 的轮询发送 —— 所以在这里
       一次性地把它喊出来。 */
    if (!pl_mem_is_dma_capable(s_tx, sizeof(s_tx))) {
        printf("[casc] 发送缓冲不在 DMA 可达区（SRAM）—— 长帧会退化成轮询发送\n");
    }

    rb_init(&s_casc_rb, "cascade");

    /* 队列必须在 initcall 内建好，不能等任务启动 —— 否则有"向空队列投递"的窗口 */
    s_casc_queue = osMessageQueueNew(CASC_QUEUE_DEPTH, CASC_MSG_SIZE, &s_casc_queue_attr);
    if (s_casc_queue == nullptr) {
        printf("[casc] 队列创建失败，级联协议停用\n");
        return;
    }
    p->queue = s_casc_queue;

    app_proto_bind(p, app_rs485_ccb());

    const osThreadAttr_t attr = {
        .name       = "cascade",
        .stack_size = 384 * 4,
        .priority   = osPriorityNormal,
    };
    pl_task_new(casc_task, nullptr, &attr);
}
sw_post_initcall(_cascade_init);

#endif /* BOARD_CASCADE_ENABLED */
