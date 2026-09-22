/**
 * @file    app_screen.h
 * @brief   整屏门面 —— 逻辑画布、渲染目标、落屏、亮度
 *
 * **它解决什么**：级联场景里主卡要画的"整屏"比它自己那块屏大（其余由从卡显示），
 * 也需要把同一份内容按卡切分下发。若不收出一个门面，协议模块就要直接摸
 * `dev_display_t`、`app_render`、切分表三样东西 —— 那是本设计明确要避免的耦合。
 *
 * 约定：**凡是影响整屏的动作都从本模块过**，`app_cascade` 只认本模块。
 *   · 渲染   —— 本模块实现 `render_target_t`（定义在 app_render.h）并注册为渲染目标，
 *               渲染引擎一行都不复制（MSL 那版复制了 200 行排版逻辑，是本设计要避开的）
 *   · 调光   —— `app_screen_set_brightness()` 一处设本地 + 置"待下发"标志
 *   · 落屏   —— `app_screen_commit_bitmap()`，**主卡本地与从卡走同一个函数**
 *   · 显存持久化 —— 通过 `render_persist_hook_t` 接管（画布比实屏大，直存实屏会错位）
 *
 * 画布是 **1bpp 位掩码**（`row_bytes = (宽+7)/8`、行优先、MSB-first、bit=1 上色），
 * 与 `dev_display_draw_bitmap` 和 `render_persist_t` 的位序约定**逐位一致** ——
 * 三处同一约定，所以"画布抽取出的位图"从卡可以直接吃，零转码。
 *
 * **每卡一个颜色**：画布只记亮/灭，每个像素最终是什么颜色由它属于哪张卡决定。
 * 代价是**卡内的多色也会塌缩成该卡那一色** —— 这是用户明确接受的取舍，不是缺陷。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "board.h" /* BOARD_SCREEN_CANVAS / BOARD_CASCADE_* —— 本文件的 API 按它们开关 */

/* ================================================================
 *  切分表 —— 整屏怎么分给各张卡
 *
 *  **为什么放在整屏门面而不是级联协议里**：切分描述的是"整屏长什么样"，那是部署
 *  事实，与用哪种总线、哪个协议下发无关。放这里，光传感器、持久化、以及将来任何
 *  "只有主卡该做"的事都能读它，不必去依赖级联协议（依赖方向：协议 → 整屏）。
 * ================================================================ */

/** @brief 切分表最多几张卡（4 卡横排是已知的最大部署规模） */
#define SCREEN_CARD_MAX (4U)

/** @brief 一张卡的**运行期**状态 —— 由枚举与轮次结果驱动
 *
 *  **MISSING 与 OFFLINE 必须分开**，因为现场处置完全不同：
 *   · MISSING = 表里有、**从未**应答过 → 多半是**配置错**（没上电 / 地址写重 / A-B 接反）
 *   · OFFLINE = 曾经在线、后连续失败被剔除 → 多半是**故障**（线松 / 供电 / 卡死）
 *  合成一个值的话，现场分不清该去查接线还是查配置 —— 而这两件事的代价差很远。 */
typedef enum {
    SCREEN_CARD_MISSING = 0, /**< 建表初值；也可能是从卡根本没起来 */
    SCREEN_CARD_ONLINE  = 1,
    SCREEN_CARD_OFFLINE = 2,
} screen_card_state_t;

/** @brief 一张卡在整屏里占的矩形 */
typedef struct {
    uint8_t  addr;  /**< 总线地址：0 = 主卡，1..0x1F = 从卡。
                     *   **与网格下标不是一回事**：主卡可以在任意一格（现场的拼法
                     *   就有"下面那块是主卡"），地址由 board.h 的
                     *   BOARD_CASCADE_MASTER_CELL 决定谁编 0。 */
    uint8_t  color; /**< 该卡的显示颜色（display_color_t）—— 卡间可不同，见 design */
    uint16_t x, y;  /**< 矩形左上角（整屏逻辑坐标，像素） */
    uint16_t w, h;  /**< 矩形尺寸。**本工程要求它 == 该卡自己的屏几何**（见下） */

    /* ---- 以下由运行期填（建表时是"表里的静态事实"，之后被枚举与轮次改写）---- */
    uint8_t state; /**< screen_card_state_t。建表初值 MISSING */
} screen_card_t;

/** @brief 切分表。
 *
 *  **矩形必须等于该卡的屏几何**（w×h == screen_rows×screen_cols）—— 本工程的每张卡
 *  各自带一整块屏，"卡要显示的那块"就是"它自己那块屏"。若将来要支持"多张卡分一块
 *  大屏的 HUB75 口"（矩形小于本卡屏），需要给落屏加一个偏移，那是另一期的事；
 *  现在不符会被 app_screen_commit_bitmap 的长度校验明确拒绝，不会静默错位。 */
typedef struct {
    const screen_card_t *cards;
    uint8_t              count; /**< 卡数 */
    uint16_t             rows;  /**< **整屏**宽（= 各卡矩形并集宽） */
    uint16_t             cols;  /**< **整屏**高 */
} screen_layout_t;

/** @brief 本卡看到的切分表。**永远非空**（没有级联时是"单卡占满整屏"）。 */
const screen_layout_t *app_screen_layout(void);

/** @brief **逻辑屏**尺寸（= 整屏；单卡时等于本卡屏）
 *
 *  渲染调用点算"全屏矩形"要用这两个，**不要用 `dev_display_get()->screen_rows/cols`**。
 *  后者是**本卡那块屏**，级联时比逻辑屏小：用它算出来的矩形只覆盖整屏的一角，
 *  内容会整块跑到别的卡那半边去，而主卡自己那块全黑 —— 现象很像"发错了卡"，
 *  其实只是矩形算小了。
 *
 *  门面停用（显示未就绪/地址不在表里）时回落到 `dev_display_get()`，与加级联之前
 *  的行为一致。 */
uint16_t app_screen_rows(void);
uint16_t app_screen_cols(void);

/* ---- 下标 与 地址 是两回事，别混 ----
 *
 * 切分表按**下标**索引（0..count-1，几何顺序），而协议按**地址**寻址。两者**顺序
 * 可以不同**：现场那个"上面一块、下面一块、下面那块是主卡"的拼法，
 * 下标 0 的 addr 是 1，下标 1 的 addr 是 0 —— 正好相反。
 *
 * 所以：**永远不要拿地址当下标**。要某张卡的矩形，先 `app_screen_index_of_addr()`
 * 换下标，或直接 `app_screen_card(下标)` 取那一项（地址与矩形在同一个结构体里，
 * 不可能对不上）。反过来，开轮时每帧的 `dst` 取的就是那一项的 `.addr`。
 *
 * 混了的表现：两块屏的内容**互换**，而 CRC / 长度 / 几何校验**全部通过** ——
 * 没有任何一处会报错。 */

/** @brief 第 card_idx 张卡（**切分表下标，不是总线地址**）；越界返回 nullptr。
 *
 *  返回的项同时带 `.addr` 与矩形，所以"发给谁"和"发哪块"从同一个来源取，不会错配。 */
const screen_card_t *app_screen_card(uint8_t card_idx);

/** @brief 总线地址 → 切分表下标；表中没有该地址返回 0xFF */
uint8_t app_screen_index_of_addr(uint8_t addr);

/* ================================================================
 *  卡片状态与整屏状态快照
 *
 *  归属在整屏门面而不是级联协议：状态描述的是"这张卡在不在"，与用哪种总线、
 *  哪个协议探活无关。协议只**驱动**它（收到应答 / 轮次成败），不拥有它。
 * ================================================================ */

/** @brief 某张卡的当前状态；下标越界返回 MISSING */
screen_card_state_t app_screen_card_state(uint8_t card_idx);

/** @brief 改写某张卡的状态；**状态真的变了才会触发告警回调**
 *
 *  由驱动方（级联协议）在"收到应答""轮次成功/失败到阈值"时调用。 */
void app_screen_card_set_state(uint8_t card_idx, screen_card_state_t st);

/** @brief 整屏状态快照 —— **轮询式，取数据不产生任何副作用**
 *
 *  给"上位机主动来问"的产品用：它自己取快照塞进自己的应答里。
 *  地址位序：位 i 对应**地址 i+1**（地址 0 是本卡，不进掩码）。 */
typedef struct {
    uint8_t  seq_lo;      /**< 最近一轮的序号低 8 位，供日志对照 */
    uint8_t  online_mask; /**< 位 i = 地址 i+1 在线 */
    uint16_t retrans_cnt; /**< 累计定向重传次数 */
    uint8_t  evict_cnt;   /**< 累计剔除次数 */
    uint8_t  last_alarm;  /**< 最近一次状态跳变的卡地址；0 = 尚未有过 */
} app_screen_status_t;

void app_screen_status(app_screen_status_t *out);

/** @brief 状态跳变时的告警回调；**不注册 = 完全静默，一个字节都不产生**
 *
 *  **故障处理与故障上报是两件事**：重传 / 本轮放弃 / 剔除是本协议必须自己做完的，
 *  不做则同步显示本身不成立；而上报是**可选的** —— 有的上位机根本不问，
 *  出问题静默即可。所以这里只暴露状态，不决定去向，也不依赖任何上层协议。 */
typedef void (*app_screen_alarm_fn_t)(uint8_t card_addr, uint8_t new_st);

void app_screen_register_alarm(app_screen_alarm_fn_t fn);

/** @brief 记一次定向重传（计数器，供状态快照用） */
void app_screen_note_retrans(void);

/** @brief 记一轮的序号（供状态快照用） */
void app_screen_note_round(uint16_t seq);

/** @brief 本卡在切分表里的下标；本卡地址不在表中返回 0xFF（配置错误） */
uint8_t app_screen_self_index(void);

/** @brief 第 card_idx 张卡（**切分表下标**）矩形的 1bpp 位图长度（字节）；越界返回 0 */
uint16_t app_screen_card_bm_len(uint8_t card_idx);

#if BOARD_SCREEN_CANVAS

/** @brief 把第 card_idx 张卡（**切分表下标，不是总线地址**）的矩形从画布抽成 1bpp 位图
 *
 *  位图格式与 `dev_display_draw_bitmap` / `render_persist_t` **逐位一致**：
 *  `(w+7)/8` 行字节、行优先、MSB-first、bit=1 为上色 —— 所以抽出来的东西从卡
 *  可以直接吃，零转码。
 *
 *  **末字节的补位一律归零**（w 不是 8 的倍数时），这样"抽出来的位图"是唯一确定的
 *  一串字节，可以直接比对、可以直接当协议载荷。
 *
 *  @return false = card_idx 越界 / buf 装不下 / 矩形超出画布（都不写 buf） */
bool app_screen_extract(uint8_t card_idx, uint8_t *buf, uint16_t cap);

/** @brief 把本卡那块画布矩形抽出来落到本地实屏
 *
 *  主卡本地提交走这条 —— 与"抽出来发给从卡"是同一个 `app_screen_extract`，
 *  所以主卡屏上的内容与从卡收到的是同一份（这正是同步显示要保证的）。
 *  @return false = 本卡不在切分表里，或抽带缓冲不够大 */
bool app_screen_commit_self(void);

/** @brief 画布有未落屏的内容、且已过静默期 → 返回 true 并清掉"待落屏"标志
 *
 *  单卡时由 app_screen 自己的任务消费；级联主卡由开轮的那个任务消费 ——
 *  **两者只能有一个**，谁消费谁负责把内容落下去。 */
bool app_screen_take_pending_settled(void);

#endif /* BOARD_SCREEN_CANVAS */

/** @brief 显式提交：立刻把画布内容落到本地屏（不必等静默期）
 *
 *  默认不需要调 —— 静默期会自动提交，现有渲染调用点一行都不用改。
 *  只在"必须立即生效"的场合用（如某条协议要求看到即时反馈）。 */
void app_screen_flush(void);

/** @brief 画布内容代数（每次写入自增）。级联用它做"本轮内容是否已过期"的复检。 */
uint32_t app_screen_generation(void);

/** @brief 本上电周期内画布**被渲染过**没有（持久化恢复不算）
 *
 *  级联用它闸开轮：上电时画布上只有本卡那一块是从记录恢复来的，这时候开轮会把
 *  一张**不全的画布**推下去、把从卡刚恢复的内容刷黑。 */
bool app_screen_canvas_touched(void);

/** @brief 把一张整屏 1bpp 位图落到本地实屏（主卡本地提交与从卡落屏共用的唯一路径）
 *
 *  `len` 必须等于 `ceil(屏宽/8) × 屏高`，不符直接返回（换模组后的旧内容不适用）。
 *
 *  内部是 `fill(BLACK)` → `dirty=false` → `draw_bitmap`：
 *  中间那段 `dirty` 为假，`scan_task` 的 prepare 不跑，屏上不会闪出全黑帧 ——
 *  这个手法在 `app_factory_test.c` 已有先例。 */
void app_screen_commit_bitmap(const uint8_t *bm, uint16_t len, uint8_t color);

/** @brief 把**所有卡**的输出颜色临时统一成这一个（`0xFF` = 取消覆盖）
 *
 *  只给工厂逐色老化用。为什么需要它：画布是 **1bpp**（只记亮/灭），而颜色在协议里
 *  是**逐卡**给的（`casc_image_t.color`，来自切分表）—— 不覆盖的话，整屏轮流点亮
 *  红/绿/蓝这种测试在级联下只能显示成"每块屏各自的颜色"（现场：十色全绿）。
 *  正常运行时不要调用它。 */
void app_screen_set_color_override(uint8_t color);

/** @brief 本卡最终输出用的颜色：有覆盖用覆盖，否则用切分表给的这一个 */
uint8_t app_screen_output_color(uint8_t card_color);

/** @brief 设置屏亮度等级（0~7）
 *
 *  级联下由主卡统一分发 —— 从卡若有自己的光传感器，两张卡会各调各的，屏上出现
 *  亮度接缝。故本函数是**唯一**的亮度入口，光传感器也走它。 */
void app_screen_set_brightness(uint8_t level);

/** @brief 当前亮度等级 */
uint8_t app_screen_get_brightness(void);

/** @brief 取走"亮度已变、待下发"的标志 —— 级联协议用它把每秒可能变多次的亮度
 *         攒成一次广播。返回 true 时 *level 给出新等级。
 *
 *  为什么要有这个：光传感器每秒都可能改，而广播要走总线。攒成一次比每次改都发
 *  省得多，且亮度是渐变量、晚几十毫秒下发不可见。 */
bool app_screen_brightness_take_pending(uint8_t *level);

/** @brief 本卡总线地址。0 = 主卡。
 *
 *  **身份属于整屏，不属于某个协议** —— 所以放在这里而不是 app_cascade：否则
 *  光传感器、以及将来任何"只有主卡该做"的事，都得去依赖级联协议。
 *
 *  本期取 board.h 的编译期常量（主卡/从卡烧不同固件）；后续期由 W25Qxx 的切分表
 *  记录覆盖 —— 同型号板子可以是 2/3/4 卡部署，那是**部署期事实**，不可能写死。 */
uint8_t app_screen_self_addr(void);

/** @brief 本卡是否主卡（地址 0） */
bool app_screen_is_master(void);

/** @brief 改写本机地址：**只改值，不落盘、不重应用**
 *
 *  持久化与重应用是调用方的事（见 app_cascade.c 的身份解析与 SET_ADDR 处理）。
 *  改完必须调一次 `app_screen_reinit_identity()`，否则门面还按旧身份装着。 */
void app_screen_set_addr(uint8_t addr);

/** @brief 按当前地址**重装**整屏门面：重算几何、定位本卡、清画布、
 *         按主/从注册或撤销渲染目标与持久化钩子
 *
 *  上电（`_screen_init`）与运行期换身份（按键认领 / 收到识别帧）走**同一条路**，
 *  避免"上电装对了、运行期漏装一样"的漂移。必须在**任务上下文**调用。 */
void app_screen_reinit_identity(void);

/** @brief 应用身份（地址 + **主卡格**）：一个都没变就什么都不做
 *
 *  **主卡格是运行期事实**（出厂默认来自 `BOARD_CASCADE_MASTER_CELL`）：它 + 网格形状
 *  决定整张切分表（主卡格编 addr 0，其余按格序编 1..N）。编译期钉死会怎样：
 *  "谁被按谁主卡"改成的是**地址**，而"我在哪一格"没跟着走 —— 被按的那张卡于是
 *  按老规矩渲染**另一块屏**那一格，两块屏的上下半幅当场对调。
 *
 *  两件事一次做完（而不是分开的 set_addr + reinit）：Caller 很容易只做一半，
 *  而"地址变了、格没变"在屏上没有任何报错。 */
void app_screen_apply_identity(uint8_t addr, uint8_t master_cell);

/** @brief 当前认定的主卡格（哪一格编 addr 0） */
uint8_t app_screen_master_cell(void);

/** @brief 格号 ↔ 地址：整张切分表就这一条规则
 *
 *  纯函数、不依赖当前表 —— 认领时要同时算"旧表里你在哪"与"新表里你去哪"，
 *  而表在那一刻只能有一份。 */
uint8_t app_screen_addr_of_cell(uint8_t cell, uint8_t master_cell);
uint8_t app_screen_cell_of_addr(uint8_t addr, uint8_t master_cell);
