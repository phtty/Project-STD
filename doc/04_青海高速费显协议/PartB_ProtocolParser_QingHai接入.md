# Part B：ProtocolParser_QingHai 接入

**状态**：`[规划中]`
**协议层内部架构**：**Parse/Execute + Tagged Union**（解析器 `qh_parse_frame` → 清单 `qh_parsed_cmd_t` → 执行器 `qh_execute_cmd`；同步、无额外队列，详见 B.4）
**前置条件**：`Device/` 现有驱动已就绪；仅 `232_voice`（TTS over RS232 语音板）需在 `Device/Comm/` 新增（见 Part A）。QH 协议层直接复用 `Device/` 驱动，不引入 `dev_service` 共享层。
**协议版本**：青海高速费显协议 2019-10-06
**参考裸机工程**：`/home/yystation/Desktop/9K1030CE00`
**协议原文**：`/home/yystation/Desktop/9K1030CE00/07协议/青海协议修改20191006.txt`

> QH 协议解析器**直接调用 `Device/` 驱动**完成硬件操作，不新增跨协议共享层。
> 协议层内部按 **Parse/Execute + Tagged Union** 拆分：解析器只产出协议域清单（纯函数），执行器只消费清单做映射 + 调驱动；两者在 `handle_task` 内同步调用，无事件队列。
> 所有协议特有逻辑（帧定界、命令分发、字段解析、格式编排、颜色/亮度映射）收敛在本模块内。

---

## B.1 背景与定位

### B.1.1 现有协议栈

| 协议 | 目录 | 帧形态 | 默认通道 | 编译宏 |
|------|------|--------|----------|--------|
| IAP | `Application/Src/IAP/` | 二进制 `0x5A5A5A5A` | UDP/10011 | — |
| LDI | `Application/Src/LDI/` | 二进制 `0xFFFF` + CRC16-XMODEM | TCP + UDP/10011 | `PROTO_LDI` |
| RLS | `Application/Src/RLS/` | 二进制 | RS485 | — |
| AH_MQTT | `Application/Src/AH_MQTT/` | MQTT topic | MQTT | — |
| 重庆（规划中） | `Application/Src/ProtocolParser_ChongQing/` | JSON `{...}` + 固定 12B 二进制 | UDP/20103 + UDP/10011 | `PROTO_CHONGQING` |
| **青海（新建）** | `Application/Src/ProtocolParser_QingHai/` | **ASCII `{cmd len data}`** | **RS232（串口）** | **`PROTO_QINGHAI`** |

### B.1.2 青海协议核心特征

青海协议是**基于串口的 ASCII 文本协议**，与 LDI（二进制 TCP/UDP）和 CQ（JSON UDP）有本质区别：

| 特征 | 青海协议 | LDI | CQ |
|------|----------|-----|-----|
| 传输层 | **串口 RS232** | TCP + UDP | UDP |
| 帧格式 | ASCII `{cmd len data}` | 二进制 0xFFFF + CRC16 | JSON `{...}` 或 12B 二进制 |
| 校验 | **无** | CRC-16/XMODEM | 无（JSON）/ CRC（BIN） |
| 命令编码 | ASCII 字符 `'1'~'B'` | 十六进制 0x0A~0x21 | JSON "cmd" 字段 |
| 状态机 | **无**（纯命令驱动） | UNINIT→AUTHED→READY | 无（心跳检测） |
| 显示行数 | **5 行**（16 点阵） | 8 行 | 8 行 |
| 上电默认 | "祝您一路平安" + 语音 | 网络搜索 | 网络搜索 |

### B.1.3 与裸机工程的关系

移植策略：**行为对齐裸机联调结果**（含已知与协议文档的差异），直接复用 `Device/` 驱动。**通过 `Device/` 驱动调用所有设备能力**（含新增的 `232_voice`）。

### B.1.4 与 LDI 的产品关系

**编译期互斥**：青海固件不含 LDI，LDI 固件不含青海。编译切换通过 Makefile / `.eide` 的条件编译实现（如 `PROTO=QH`）。

### B.1.5 初始化顺序与 RS232 启动（M1：明确调用位置）

QH 协议走 **RS232-0（USART3，`CH_ID_RS232=1`）**。通道实例的注册与协议的绑定是解耦的两步，必须都完成且早于首个帧到达：

1. **启动 RS232-0 通道任务**（在 `main` 早期、`app_dispatch_init()` 之后调用）：
   ```c
   app_rs232_start();   // 启动 USART3 收/发任务（见 Application/Src/Channel/app_rs232.c）
   ```
   该函数内部创建任务，任务首件事是 `app_channel_register(CH_ID_RS232, &g_rs232_0.me)`，把通道实例注册进分发框架。**只要此函数返回，通道即已就绪。**

2. **QH 协议自注册 + 绑定**（经 `sw_app_initcall(qh_proto_init)` 自动跑）：
   ```c
   static void qh_proto_init(void)
   {
       g_qh_proto_frame_queue = osMessageQueueNew(2, FRAME_DATA_MAX_LEN, &s_qh_queue_attr);
       proto_mask_t mask = app_proto_register(qh_proto_probe_frame, rb_group_rb(RB_GROUP_PROTO));
       app_proto_bind_channel(mask, CH_ID_RS232);             // 协议 mask ← RS232 通道
       app_proto_set_frame_queue(mask, g_qh_proto_frame_queue);
   }
   ```

**顺序约束**：`app_rs232_start()` 与 `qh_proto_init` 二者无强先后依赖（注册的是不同对象），但**首个帧到达前两者都必须完成**。推荐约定：`app_rs232_start()` 在 `main` 显式调用（不放在 initcall，避免与协议 initcall 顺序耦合）；`qh_proto_init` 交给 initcall 自动执行。

**易错点**：`app_rs232_1_start()` 启动的是 **RS232-1（USART6，`CH_ID_RS232_1=6`）**，与 QH 无关（除非硬件改接第二路串口），切勿误调为 QH 绑定通道。

---

## B.2 功能全景——协议特有功能清单

> 以下所有功能均属于"协议特有功能"，在 `ProtocolParser_QingHai` 层实现；设备能力直接调 `Device/` 驱动（或 `232_voice`），详见 Part A。

### B.2.1 协议特有功能矩阵

| 序号 | 功能 | 协议命令 | 实现位置 | 说明 |
|------|------|---------|---------|------|
| P01 | **ASCII 帧定界** | — | `app_qh_proto.c` probe | `'{'` 起始 + 长度字段 + `'}'` 结尾 |
| P02 | **命令字查表分发** | `'1'~'B'` | `app_qh_proto_cmd.c` | 11 条命令的分发表 + 处理函数 |
| P03 | **主机查询应答** | `'1'` | `app_qh_proto_cmd.c` | 构造 `{1 01 00}` 固定应答帧，串口回发（`channel_send`） |
| P04 | **自检模式编排** | `'2'` | `app_qh_proto_cmd.c` | 全屏黄色（`app_render`）+ 语音（`dev_rs232_voice_play`） |
| P05 | **单行显示参数解析** | `'3'` | `app_qh_proto_cmd.c` | 解析颜色(0~2) + 行号(1~5) + GBK文本，构造 `render_cfg_t` |
| P06 | **全屏可编辑显示** | `'4'` | `app_qh_proto_cmd.c` | 解析颜色 + X/Y坐标 + 文本 + 换行符，构造 `render_cfg_t` |
| P07 | **清屏** | `'5'` | `app_qh_proto_cmd.c` | `dev_display_fill(BLACK)` + `commit_frame` |
| P08 | **固定格式显示（客车）** | `'6'` | `app_qh_proto_cmd.c` | 解析 `\|` 分隔字段，构造 5 行格式化文本 + 语音 |
| P09 | **固定格式显示（货车）** | `'6'` | `app_qh_proto_cmd.c` | 同上，增加总重/超重行，0 吨不显示 |
| P10 | **礼貌用语索引映射** | `'7'` | `app_qh_proto_cmd.c` | `'0'~'3'` → 4 条固定 GBK 文本 → `dev_rs232_voice_play` |
| P11 | **亮度级别映射** | `'8'` | `app_qh_proto_cmd.c` | `'0'`=自动(光敏驱动)；`'1'~'5'` 查表(索引=level-1) → `dev_display_set_brightness` |
| P12 | **音量级别映射** | `'9'` | `app_qh_proto_cmd.c` | 协议 1~5 → 音量标签 → `dev_rs232_voice_volume` |
| P13 | **外设控制位解析** | `'A'` | `app_qh_proto_cmd.c` | bit0=绿灯, bit1=红灯, bit2=黄闪 → `dev_io_lane_light` / `dev_io_flash_light` |
| P14 | **费额语音文本构造** | `'6'/'B'` | `app_qh_proto_voice.c` | 金额数字 → "您好请交费XX.XX元" GBK 文本 → `dev_rs232_voice_play` |
| P15 | **上电欢迎语编排** | — | `app_qh_proto.c` init | "祝您一路平安" 显示（`app_render`）+ 语音（`dev_rs232_voice_play`） |
| P16 | **串口通道绑定** | — | `app_rs232_start()` + `qh_proto_init` | 绑定 `CH_ID_RS232`(index=1)，初始化顺序见 B.1.5 |
| P17 | **编译期互斥** | — | Makefile | `PROTO_QINGHAI` 宏 |

---

## B.3 目录与文件规划

与 LDI 同级；源/头分离。**文件名统一使用 `app_qh_proto_*` 样式**。采用 **Parse/Execute + Tagged Union**：

- **`app_qh_proto_parse.c`** —— 解析器：把 raw bytes 解成 `qh_parsed_cmd_t`（清单，Tagged Union）。纯函数，不碰 `Device/` 驱动、不回响应。
- **`app_qh_proto_cmd.c`** —— 执行器：消费清单，做协议→设备映射后调用 `Device/` 驱动。
- **`app_qh_proto_voice.c`** —— 语音辅助（执行器侧）：礼貌用语索引、费额金额→TTS 文本。
- **`app_qh_proto.c`** —— 核心：initcall、probe、handle_task（同步 `parse→execute`）、帧队列。
- **`app_qh_proto_cfg.c`** —— 青海侧配置（波特率等参数落盘）。

### B.3.1 文件树架构

```
Application/Src/ProtocolParser_QingHai/
  app_qh_proto.c          # 核心：initcall、probe、任务、帧分发（同步 parse→execute）
  app_qh_proto_parse.c    # 解析器：qh_parse_frame()  bytes → qh_parsed_cmd_t（清单）
  app_qh_proto_cmd.c      # 执行器：qh_execute_cmd()  清单 → Device/ 驱动调用 + 协议→设备映射
  app_qh_proto_voice.c    # 语音辅助（执行器侧）：礼貌用语索引、费额金额→TTS文本
  app_qh_proto_cfg.c      # 青海侧配置（波特率等参数落盘）

Application/Inc/ProtocolParser_QingHai/
  app_qh_proto.h          # 公共契约：qh_parsed_cmd_t（清单）+ qh_cmd_t + 解析/执行 API 声明
  app_qh_proto_parse.h    # 解析器对外 API（含 qh_parse_frame）
  app_qh_proto_cmd.h      # 执行器对外 API（含 qh_execute_cmd + 映射表 extern）
  app_qh_proto_voice.h
  app_qh_proto_cfg.h
```

> 不新增 `app_qh_device.c` 之类的设备包装层：执行器直接调用 `Device/` 驱动，与 LDI（`app_ldi_device.c` 直接调驱动）同一套路；协议特有的映射表收在 `app_qh_proto_cmd.c` / `app_qh_proto_voice.c`。
> **解析器与执行器之间是「清单」结构体，不是队列、不是事件**——两者在 `handle_task` 内同步调用，零额外 IPC。

### B.3.2 命名规范

| 项 | 规则 | 示例 |
|----|------|------|
| 目录名 | `ProtocolParser_QingHai` | — |
| 文件名 | `app_qh_proto_*.c` | `app_qh_proto_parse.c` / `app_qh_proto_cmd.c` |
| 解析器函数前缀 | `qh_parse_` | `qh_parse_frame()` |
| 执行器函数前缀 | `qh_exec_` | `qh_exec_one_line()` |
| 执行器入口 | `qh_execute_cmd()` | 由 handle_task 调用 |
| 全局变量 | `g_qh_proto_` | `g_qh_proto_ctx` |
| 头文件守卫 | `APP_QH_PROTO_*_H` | `APP_QH_PROTO_PARSE_H` |
| 静态变量 | `s_qh_` | `s_qh_color_map` |

### B.3.3 构建编入

| 文件 | 改动 |
|------|------|
| `Makefile` | `INC_DIRS` 增加 `Application/Inc/ProtocolParser_QingHai/`；`SRC_APPLICATION` 条件追加；`SRC_DEVICE` 追加 `Device/Comm/dev_rs232_voice.c` |
| `.eide/eide.yml` | 增加同组源文件（放在 QH 条件组中） |

**编译期切换方案**：

```makefile
PROTO ?= LDI
ifeq ($(PROTO),QH)
  SRC_APPLICATION += $(wildcard Application/Src/ProtocolParser_QingHai/*.c)
  SRC_DEVICE      += Device/Comm/dev_rs232_voice.c
  CFLAGS += -DPROTO_QINGHAI
else ifeq ($(PROTO),CQ)
  SRC_APPLICATION += $(wildcard Application/Src/ProtocolParser_ChongQing/*.c)
  SRC_DEVICE      += Device/Comm/dev_rs232_voice.c
  CFLAGS += -DPROTO_CHONGQING
else
  SRC_APPLICATION += $(wildcard Application/Src/LDI/*.c)
endif
```

**无需引入 cJSON 等第三方库**——QH 是纯 ASCII 协议，手动解析即可。

---

## B.4 架构设计（Parse/Execute + Tagged Union）

### B.4.1 架构蓝图

```
                          ┌──────────────────────────────────────────────────┐
                          │            ProtocolParser_QingHai                  │
   RS232 ─▶ channel ─▶ rb ─▶ frame_dispatch ─▶ qh_proto_probe_frame            │
                          │                    │  READY                       │
                          │                    ▼                              │
                          │        g_qh_proto_frame_queue (已有队列，非新增)    │
                          │                    │                              │
                          │                    ▼                              │
                          │         qh_proto_handle_task (协议任务)            │
                          │                    │                              │
                          │         ┌──────────┴───────────┐                  │
                          │         ▼                      ▼                  │
                          │   qh_parse_frame()       qh_execute_cmd()          │
                          │     （解析器）              （执行器）              │
                          │         │                      │                  │
                          │         ▼                      ▼                  │
                          │   qh_parsed_cmd_t ──▶ switch(cmd) ──▶ Device/       │
                          │   （Tagged Union            │                      │
                          │      「清单」)               ▼                    │
                          │                  app_render / dev_display_*       │
                          │                  dev_io_* / dev_rs232_voice       │
                          │                  channel_send (cmd '1' 应答)       │
                          └──────────────────────────────────────────────────┘

   边界 = 结构体 qh_parsed_cmd_t（清单）
   解析器只产出清单（纯函数，不碰设备）；执行器只消费清单（映射 + 调驱动）
   两者同步、同一任务上下文，无额外队列 / 无事件总线
```

### B.4.2 清单结构体 `qh_parsed_cmd_t`（Tagged Union）

**位置**：`app_qh_proto.h`（解析器与执行器的唯一契约；保持 `Device/` 无关，仅依赖 `stdint.h` 与 `channel_t` 前向声明）。

```c
/* app_qh_proto.h —— 解析器→执行器 的「清单」(Tagged Union) */
#pragma once
#include <stdint.h>

/* 前向声明，避免引入 app_dispatch.h 造成循环依赖 */
typedef struct channel channel_t;

/* 命令枚举（ASCII '1'~'B' 映射为枚举，消除裸字符比较） */
typedef enum {
    QH_PCMD_HOST_QUERY = 0,  /* '1' */
    QH_PCMD_SELF_CHECK,      /* '2' */
    QH_PCMD_ONE_LINE,        /* '3' */
    QH_PCMD_FULL_SCREEN,     /* '4' */
    QH_PCMD_CLEAR,           /* '5' */
    QH_PCMD_FIXED_DISPLAY,   /* '6' */
    QH_PCMD_CIVIL_VOICE,     /* '7' */
    QH_PCMD_BRIGHTNESS,      /* '8' */
    QH_PCMD_VOLUME,          /* '9' */
    QH_PCMD_PERIPHERAL,      /* 'A' */
    QH_PCMD_VOICE,           /* 'B' */
    QH_PCMD_NONE,
} qh_pcmd_t;

/* 解析状态：结构级 / 语义级 分离 */
typedef enum {
    QH_PARSE_OK = 0,
    QH_PARSE_ERR_FRAME,  /* 帧结构非法（非 {..}） */
    QH_PARSE_ERR_CMD,    /* 命令字非法 */
    QH_PARSE_ERR_PARAM,   /* 参数越界 / 不合法 */
} qh_parse_sta_t;

/* 各命令已解码的协议域参数（只放协议语义，不放设备域值） */
typedef struct { uint8_t color; uint8_t row; const uint8_t *text; uint8_t text_len; } qh_one_line_t;
typedef struct { uint8_t color; uint8_t x; uint8_t y; const uint8_t *text; uint8_t text_len; } qh_full_screen_t;
typedef struct { uint8_t type; const uint8_t *raw; uint8_t raw_len; } qh_fixed_t; /* type∈{0,1}，原始 '|' 字段留执行器拆分 */
typedef struct { uint8_t level; } qh_brightness_t;   /* 0~5 */
typedef struct { uint8_t level; } qh_volume_t;        /* 1~5 */
typedef struct { uint8_t ctrl; } qh_peripheral_t;     /* bit0/1/2 */
typedef struct { uint8_t idx; } qh_civil_t;           /* 0~3 */
typedef struct { uint8_t type; uint32_t amount_fen; } qh_fee_t; /* 5位BCD→分 */

typedef struct {
    qh_pcmd_t      cmd;        /* 已映射为枚举 */
    qh_parse_sta_t sta;        /* 结构/语义校验结果 */
    uint8_t        data_len;   /* 执行器仅用到此 */
    union {
        qh_one_line_t    one_line;
        qh_full_screen_t full_screen;
        qh_fixed_t       fixed;
        qh_brightness_t  brightness;
        qh_volume_t      volume;
        qh_peripheral_t  peripheral;
        qh_civil_t       civil;
        qh_fee_t         fee;
        /* HOST_QUERY / SELF_CHECK / CLEAR: 无参数 */
    } p;
    const uint8_t *data;       /* 逃生舱口：指向整帧 data 区起始，仅复杂命令(如 cmd '6')
                                   的执行器需按需截取子字段(如 '|' 分隔字段)时使用；
                                   简单命令的执行器只读本 union 内已解码字段，不碰此指针 */
} qh_parsed_cmd_t;

/* 边界 API（声明在此，实现分别在 parse.c / cmd.c） */
qh_parsed_cmd_t qh_parse_frame(const uint8_t *raw, uint8_t raw_len);
void qh_execute_cmd(channel_t *ch, const qh_parsed_cmd_t *cmd);
```

> **设计要点**：
> - union 内只放**协议域已解码**值（颜色索引 0/1/2、亮度 0~5、bit 位、金额分），**不放** `COLOR_RED` / 硬件亮度级——那些是设备域，留给执行器映射。
> - cmd `'6'` 解析器只做结构校验 + 保留 `raw` 指针，字段拆分 / 5 行格式化在执行器（属协议语义解释，本就在协议层）。
> - `data` 指针指向 `frame_msg_t` 缓冲，执行器必须**同步消费**，`qh_parse_frame` 返回的是栈上 `qh_parsed_cmd_t`，`handle_task` 同步调用 `qh_execute_cmd` 后销毁。

### B.4.3 各模块实现样板

#### B.4.3.1 解析器 `app_qh_proto_parse.c`

```c
/* app_qh_proto_parse.c —— 解析器：bytes → qh_parsed_cmd_t（清单） */
#include "app_qh_proto.h"
#include "app_qh_proto_parse.h"

static qh_pcmd_t qh_char_to_cmd(char c)
{
    switch (c) {
        case '1': return QH_PCMD_HOST_QUERY;
        case '2': return QH_PCMD_SELF_CHECK;
        case '3': return QH_PCMD_ONE_LINE;
        case '4': return QH_PCMD_FULL_SCREEN;
        case '5': return QH_PCMD_CLEAR;
        case '6': return QH_PCMD_FIXED_DISPLAY;
        case '7': return QH_PCMD_CIVIL_VOICE;
        case '8': return QH_PCMD_BRIGHTNESS;
        case '9': return QH_PCMD_VOLUME;
        case 'A': return QH_PCMD_PERIPHERAL;
        case 'B': return QH_PCMD_VOICE;
        default:  return QH_PCMD_NONE;
    }
}

/* 纯函数：raw bytes → 协议域清单；不碰任何 Device/ 驱动、不回响应 */
qh_parsed_cmd_t qh_parse_frame(const uint8_t *raw, uint8_t raw_len)
{
    qh_parsed_cmd_t cmd = {0};

    /* 结构校验（probe 已确保整帧 {..}，这里做最终防御） */
    if (raw_len < 4 || raw[0] != '{' || raw[raw_len - 1] != '}') {
        cmd.sta = QH_PARSE_ERR_FRAME;
        return cmd;
    }
    cmd.cmd = qh_char_to_cmd((char)raw[1]);
    if (cmd.cmd == QH_PCMD_NONE) { cmd.sta = QH_PARSE_ERR_CMD; return cmd; }

    const uint8_t *data = &raw[3];                 // 跳过 '{' + cmd + len
    /* E2：len 字段(raw[2]) 与帧实际可载数据长度(raw_len-4) 必须一致。
       probe 已保证整帧 {..} 完整，但串口干扰可能让 len 字段与实际不符 → 防御性校验 */
    uint8_t frame_data_len = (raw_len > 4) ? (uint8_t)(raw_len - 4) : 0; // 实际 payload 字节数
    uint8_t declared_len   = (raw_len > 4) ? (uint8_t)(raw[2] - '0') : 0; // 帧内 len 字段
    if (declared_len > frame_data_len) {           // len 字段越界 → 视为坏帧
        cmd.sta = QH_PARSE_ERR_FRAME;
        return cmd;
    }
    uint8_t data_len = declared_len;
    cmd.data = data;
    cmd.data_len = data_len;
    cmd.sta = QH_PARSE_OK;

    switch (cmd.cmd) {
    case QH_PCMD_ONE_LINE:
        if (data_len < 2) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
        cmd.p.one_line.color = data[0] - '0';
        cmd.p.one_line.row   = data[1] - '1';
        cmd.p.one_line.text  = &data[2];
        cmd.p.one_line.text_len = (data_len > 2) ? (data_len - 2) : 0;
        if (cmd.p.one_line.color > 2 || cmd.p.one_line.row > 4)
            cmd.sta = QH_PARSE_ERR_PARAM;
        break;
    case QH_PCMD_FULL_SCREEN:
        if (data_len < 3) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
        cmd.p.full_screen.color = data[0] - '0';
        cmd.p.full_screen.x = data[1];
        cmd.p.full_screen.y = data[2];
        cmd.p.full_screen.text = &data[3];
        cmd.p.full_screen.text_len = data_len - 3;
        if (cmd.p.full_screen.color > 2) cmd.sta = QH_PARSE_ERR_PARAM;
        break;
    case QH_PCMD_BRIGHTNESS:
        if (data_len < 1) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
        cmd.p.brightness.level = data[0] - '0';
        if (cmd.p.brightness.level > 5) cmd.sta = QH_PARSE_ERR_PARAM;
        break;
    case QH_PCMD_VOLUME:
        if (data_len < 1) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
        cmd.p.volume.level = data[0] - '0';
        if (cmd.p.volume.level < 1 || cmd.p.volume.level > 5) cmd.sta = QH_PARSE_ERR_PARAM;
        break;
    case QH_PCMD_PERIPHERAL:
        if (data_len < 1) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
        cmd.p.peripheral.ctrl = data[0];
        break;
    case QH_PCMD_CIVIL_VOICE:
        if (data_len < 1) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
        cmd.p.civil.idx = data[0] - '0';
        if (cmd.p.civil.idx > 3) cmd.sta = QH_PARSE_ERR_PARAM;
        break;
    case QH_PCMD_FIXED_DISPLAY:
        if (data_len < 1) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
        cmd.p.fixed.type = data[0] - '0';
        cmd.p.fixed.raw  = &data[1];
        cmd.p.fixed.raw_len = (data_len > 1) ? (data_len - 1) : 0;
        if (cmd.p.fixed.type > 1) cmd.sta = QH_PARSE_ERR_PARAM;   // 0=客车, 1=货车
        break;
    case QH_PCMD_VOICE:
        if (data_len < 6) { cmd.sta = QH_PARSE_ERR_PARAM; break; }
        /* E3：每位必须是 '0'~'9'，否则减去 '0' 会得到负值/乱码金额，须丢弃 */
        for (int i = 1; i <= 5; i++) {
            if (data[i] < '0' || data[i] > '9') { cmd.sta = QH_PARSE_ERR_PARAM; return cmd; }
        }
        cmd.p.fee.type = data[0] - '0';
        cmd.p.fee.amount_fen = (uint32_t)(data[1] - '0') * 10000
                             + (uint32_t)(data[2] - '0') * 1000
                             + (uint32_t)(data[3] - '0') * 100
                             + (uint32_t)(data[4] - '0') * 10
                             + (uint32_t)(data[5] - '0');
        break;
    /* HOST_QUERY / SELF_CHECK / CLEAR: 无参数，仅 cmd + sta 即可 */
    default:
        break;
    }
    return cmd;
}
```

#### B.4.3.2 执行器 `app_qh_proto_cmd.c`

```c
/* app_qh_proto_cmd.c —— 执行器：qh_parsed_cmd_t → Device/ 驱动调用 */
#include "app_qh_proto.h"
#include "app_qh_proto_cmd.h"
#include "app_qh_proto_voice.h"
#include "app_render.h"
#include "dev_display.h"
#include <string.h>             // memchr（cmd '6' 字段拆分）
#include "dev_io_ctrl.h"
#include "dev_rs232_voice.h"

/* 协议域→设备域 映射表（执行器侧；与 LDI 套路一致） */
static const display_color_t s_qh_color_map[] = { COLOR_RED, COLOR_GREEN, COLOR_YELLOW };
#define QH_MAP_COLOR(i) (((i) < 3) ? s_qh_color_map[(i)] : COLOR_GREEN)
static const uint8_t s_qh_brightness_map[] = {0, 3, 4, 5, 7, 8};
static const uint8_t s_qh_volume_map[] = {0, 0x31, 0x33, 0x35, 0x37, 0x39};

/* 执行器入口：handle_task 调用；解析失败直接丢弃（或此处回异常） */
void qh_execute_cmd(channel_t *ch, const qh_parsed_cmd_t *cmd)
{
    if (cmd->sta != QH_PARSE_OK) return;
    switch (cmd->cmd) {
    case QH_PCMD_HOST_QUERY:    qh_exec_host_query(ch); break;
    case QH_PCMD_SELF_CHECK:    qh_exec_self_check(ch); break;
    case QH_PCMD_ONE_LINE:      qh_exec_one_line(ch, &cmd->p.one_line); break;
    case QH_PCMD_FULL_SCREEN:   qh_exec_full_screen(ch, &cmd->p.full_screen); break;
    case QH_PCMD_CLEAR:         qh_exec_clear(ch); break;
    case QH_PCMD_FIXED_DISPLAY: qh_exec_fixed(ch, &cmd->p.fixed); break;
    case QH_PCMD_CIVIL_VOICE:   qh_exec_civil_voice(ch, cmd->p.civil.idx); break;
    case QH_PCMD_BRIGHTNESS:    qh_exec_brightness(ch, cmd->p.brightness.level); break;
    case QH_PCMD_VOLUME:        qh_exec_volume(ch, cmd->p.volume.level); break;
    case QH_PCMD_PERIPHERAL:    qh_exec_peripheral(ch, cmd->p.peripheral.ctrl); break;
    case QH_PCMD_VOICE:         qh_exec_voice_fee(ch, cmd->p.fee.type, cmd->p.fee.amount_fen); break;
    default: break;
    }
}

/* ---- 各命令执行器：只读清单 + 做映射 + 调 Device/ 驱动 ---- */

static void qh_exec_one_line(channel_t *ch, const qh_one_line_t *p)
{
    dev_display_t *d = dev_display_get();
    app_render(&(render_cfg_t){
        .type = RENDER_TEXT, .x = 0, .y = (uint16_t)p->row * FONT_16,
        .w = d ? d->screen_cols : 0, .h = FONT_16,
        .color = QH_MAP_COLOR(p->color),
        .text = (const char *)p->text, .len = p->text_len,
        .font_size = FONT_16, .font_type = FONT_ST, .text_enc = FONT_ENC_GBK,
    });
}

static void qh_exec_brightness(channel_t *ch, uint8_t level)
{
    dev_display_t *d = dev_display_get();
    if (!d) return;
    if (level == 0) {
        /* 自动档：不强制固定亮度，交由板级光敏自动调光（dev_light_sensor 钳位 4~7） */
        return;
    }
    /* 手动档：level∈1~5，映射表索引 = level-1 */
    dev_display_set_brightness(d, s_qh_brightness_map[level - 1]);
}

static void qh_exec_peripheral(channel_t *ch, uint8_t ctrl)
{
    bool green  = (ctrl & 0x01) && !(ctrl & 0x02);  // bit0=绿, bit1=红(优先)
    bool red    = (ctrl & 0x02);
    bool yellow = (ctrl & 0x04);
    if (red)        dev_io_lane_light(false);
    else if (green) dev_io_lane_light(true);
    dev_io_flash_light(yellow);
}

/* qh_exec_civil_voice / qh_exec_voice_fee / qh_exec_full_screen /
   qh_exec_clear / qh_exec_self_check / qh_exec_host_query 见 B.7 */

/* ---- cmd '6' 固定格式显示（核心命令）执行器 ---- */
/* 解析器只填 p.fixed{type,raw,raw_len}（保留 '|' 原文字段），此处完成字段拆分 + 5 行构造 */
static void qh_exec_fixed(channel_t *ch, const qh_fixed_t *p)
{
    /* 1. 按 '|' 拆分 raw（含 type 之后整段），最多 5 段 */
    const uint8_t *fields[6];
    uint8_t       flen[6];
    int nfields = 0;
    const uint8_t *cur = p->raw;
    const uint8_t *end = p->raw + p->raw_len;
    while (cur < end && nfields < 6) {
        const uint8_t *sep = memchr(cur, '|', (size_t)(end - cur));
        fields[nfields] = cur;
        flen[nfields]   = (uint8_t)((sep ? sep : end) - cur);
        nfields++;
        if (!sep) break;
        cur = sep + 1;
    }
    /* 2. 客车 5 行：车型/金额/余额/信息1/信息2；货车少 2 段时后两行空白 */
    dev_display_t *d = dev_display_get();
    for (int line = 0; line < 5; line++) {
        const uint8_t *txt = (line < nfields) ? fields[line] : (const uint8_t *)"";
        uint8_t tlen = (line < nfields) ? flen[line] : 0;
        app_render(&(render_cfg_t){
            .type = RENDER_TEXT, .x = 0, .y = (uint16_t)line * FONT_16,
            .w = d ? d->screen_cols : 0, .h = FONT_16,
            .color = COLOR_GREEN,
            .text = (const char *)txt, .len = tlen,
            .font_size = FONT_16, .font_type = FONT_ST, .text_enc = FONT_ENC_GBK,
        });
    }
    /* 3. 金额 > 0.5 元自动播报（需从字段提取金额，此处以第 1 段作示意；
          实际按裸机"金额: "后数字解析，对齐 B.7.5） */
    uint32_t amount_fen = 0;   // TODO: 从 fields[1] 解析 "金额: 12.50 元" → 分
    if (p->type == 0 && nfields > 1) {
        /* 客车：fields[1]=金额 ... 解析略，见 B.7.5 */
    }
    qh_voice_fee_amount(amount_fen);   // amount_fen < 50 时内部不播报
}
```

#### B.4.3.3 语音辅助 `app_qh_proto_voice.c`（执行器侧）

```c
/* app_qh_proto_voice.c —— 语音辅助（执行器侧） */
#include "app_qh_proto_voice.h"
#include "dev_rs232_voice.h"

static const struct { const uint8_t *gbk; uint8_t len; } s_qh_civil_phrases[] = {
    { (const uint8_t *)"\xC4\xFA\xBA\xC3\xA3\xA1\xBB\xB6\xD3\xAD\xD0\xD0\xCA\xBB\xB9\xF3\xD6\xDD\xB8\xDF\xCB\xD9\xB9\xAB\xC2\xB7", 27 }, /* 您好！欢迎行驶贵州高速公路 */
    { (const uint8_t *)"\xC7\xEB\xB3\xF6\xCA\xBE\xCD\xA8\xD0\xD0\xBF\xA8", 12 }, /* 请出示通行卡 */
    { (const uint8_t *)"\xD0\xBB\xD0\xBB\xBA\xCF\xD7\xF7\xA3\xA1\xD7\xA3\xC4\xFA\xD2\xBB\xC2\xB7\xC6\xBD\xB0\xB2", 22 }, /* 谢谢合作！祝您一路平安 */
    { (const uint8_t *)"\xBD\xBB\xD2\xD7\xB2\xBB\xB3\xC9\xB9\xA6\xA3\xAC\xC7\xEB\xD7\xDF\xC8\xCB\xB9\xA4\xB3\xB5\xB5\xC0", 24 }, /* 交易不成功，请走人工车道 */
};

void qh_voice_civil(uint8_t idx)
{
    if (idx >= (sizeof(s_qh_civil_phrases) / sizeof(s_qh_civil_phrases[0]))) return;
    dev_rs232_voice_play(s_qh_civil_phrases[idx].gbk, s_qh_civil_phrases[idx].len);
}

void qh_voice_fee_amount(uint32_t amount_fen)
{
    if (amount_fen < 50) return;  // 不足 0.5 元不播报（裸机对齐）
    uint32_t yuan = amount_fen / 100;
    char text[64];
    int n = snprintf(text, sizeof(text), "您好请交费%lu元", (unsigned long)yuan);
    if (n > 0 && n < (int)sizeof(text))
        dev_rs232_voice_play((const uint8_t *)text, (uint16_t)n);
}
```

#### B.4.3.4 核心 `app_qh_proto.c`（handle_task 同步 parse→execute）

```c
/* app_qh_proto.c —— 核心：initcall、probe、handle_task */
#include "app_qh_proto.h"
#include "app_qh_proto_parse.h"
#include "app_qh_proto_cmd.h"
#include "app_dispatch.h"

/* （initcall / probe / 通道绑定 见 B.1~B.5，此处仅展示任务主循环） */

void qh_proto_handle_task(void *argument)
{
    /* 缓冲复用：_buf 同时充当「队列 item 体」与「frame_msg_t + payload」两种视图。
       FRAME_DATA_MAX_LEN(=1044) 是 app_dispatch.h 定义的最大 item 尺寸，
       恰好覆盖 frame_msg_t 头(8B) + 最大 payload(1036B)，与队列 item size 严格一致。
       ——评审误提「应改为 sizeof(frame_msg_t)+QH_PAYLOAD_MAX」：该宏不存在，且
         FRAME_DATA_MAX_LEN 已含头开销，改用它反而要凭空引入未定义宏，故保持现状。 */
    static uint8_t _buf[FRAME_DATA_MAX_LEN];
    frame_msg_t *msg = (frame_msg_t *)_buf;
    g_qh_proto_frame_queue = osMessageQueueNew(2, FRAME_DATA_MAX_LEN, &s_qh_queue_attr);
    app_proto_set_frame_queue(s_qh_mask, g_qh_proto_frame_queue);

    for (;;) {
        if (osOK != osMessageQueueGet(g_qh_proto_frame_queue, msg, NULL, osWaitForever))
            continue;
        /* Parse → Execute：同步、同一任务上下文，无额外队列 / 无事件总线 */
        qh_parsed_cmd_t cmd = qh_parse_frame(msg->data, msg->data_len);
        qh_execute_cmd(msg->ch, &cmd);
    }
}
```

### B.4.4 数据流（文字版）

```
串口 RS232 收包 (CH_ID_RS232, USART3, 9600~115200bps)
  -> app_channel_dispatch
  -> ring_buffer (id=1, RB_GROUP_PROTO)
  -> frame_dispatch_task
  -> qh_proto_probe_frame
       +-- '{' -> 扫描到 '}' -> 长度校验 -> READY (ASCII 帧)
  -> g_qh_proto_frame_queue         （已有队列，非新增）
  -> qh_proto_handle_task
       +-- qh_parse_frame()  解析器：bytes → qh_parsed_cmd_t（清单）
       +-- qh_execute_cmd() 执行器：
       |     +-- 直接调用 Device/ 驱动 + 协议→设备映射:
       |     |     app_render / dev_display_*      (HUB75 显示)
       |     |     dev_io_lane_light / dev_io_flash_light  (通信灯/报警灯)
       |     |     dev_display_set_brightness      (亮度)
       |     |     dev_rs232_voice_play/volume     (TTS 语音板，新增 232_voice)
       |     +-- qh_proto_voice_*() 语音辅助（执行器侧）
       +-- channel_send(ch, rsp, len)        (cmd '1' 串口回发应答帧)
```

### B.4.5 RS232 串口模型

QH 协议**仅使用 RS232 串口**，不涉及 UDP/TCP：

```
CH_ID_RS232 (index=1) -> USART3 -> 9600~115200bps（默认9600）
  -> 承载 QH 协议帧收发
  -> 同时承担 cmd '1' 应答帧回发
  （注：枚举见 app_dispatch.h `channel_id_t`；index=5 是 CH_ID_MQTT，切勿混淆）
```

串口参数：
| 参数 | 默认值 | 可配置 |
|------|--------|--------|
| 波特率 | 9600 | 可通过 cfg 保存 |
| 数据位 | 8 | 固定 |
| 停止位 | 1 | 固定 |
| 校验位 | 无 | 固定 |

---

## B.5 Probe 与帧定界

### B.5.1 ASCII 帧格式

```
字段:  {    cmd    len    data[0..N]    }
长度:   1     1      1      N(=len)       1
示例:  7B   33    0A     xx xx xx...    7D
```

| 字段 | 长度 | 说明 |
|------|------|------|
| 帧头 | 1B | 固定 `'{'` (0x7B) |
| 命令字 | 1B | `'1'~'B'` (0x31~0x42) |
| 参数长度 | 1B | ASCII 数字 `'0'~'9'`，表示 data 字段字节数 |
| 参数数据 | 变长 | data[0]~data[N-1]，N = len 字段的 ASCII 值 |
| 帧尾 | 1B | 固定 `'}'` (0x7D) |

**总帧长 = 3 + len 字节**

### B.5.2 probe 逻辑

```c
qh_proto_probe_sta_t qh_proto_probe_frame(channel_t *ch, ring_buffer_t *rb,
                                           uint32_t *frame_len, uint8_t *aux)
{
    // 1. 读首字节，检查 '{'
    // 2. 读 cmd 字节（偏移1），检查 '1'~'B'
    // 3. 读 len 字节（偏移2），解析为数字 0~99
    // 4. frame_len = 3 + len
    // 5. 检查 rb 中是否有 frame_len 字节可用
    //    -> 有：PROTO_PROBE_READY
    //    -> 无：PROTO_PROBE_WAIT
    // 6. 首字节非 '{' -> PROTO_PROBE_FAKE（跳过1字节）
}
```

**无需花括号深度扫描**——QH 帧格式简单，`{` 和 `}` 不会在 data 区出现（data 是可显示 ASCII 文本或空格）。

### B.5.3 帧校验

QH 协议**无 CRC 校验**。probe 阶段仅校验：
1. 首字节 = `'{'`
2. 命令字 ∈ `'1'~'B'`（合法范围）
3. 长度字段为 ASCII 数字 `'0'~'9'`，且 `len <= FRAME_DATA_MAX_LEN`
4. 帧尾 = `'}'`（通过 rb_peek 检查尾字节）

---

## B.6 命令实现矩阵

### B.6.1 命令表（Parse/Execute 视角）

每条命令在「解析器」产出清单字段、在「执行器」消费清单调设备。下表给出**解析器写入的清单字段**与**执行器调用的设备能力**：

| cmd | ASCII | 清单字段（解析器写入） | 执行器入口 | 设备能力调用 | 一期 |
|-----|-------|------------------------|------------|-------------|------|
| `'1'` | 0x31 | 无（仅 `cmd`+`sta`） | `qh_exec_host_query` | `channel_send` | **必做** |
| `'2'` | 0x32 | 无 | `qh_exec_self_check` | `app_render(RENDER_FILL)` + `dev_rs232_voice_play` | **必做** |
| `'3'` | 0x33 | `p.one_line{color,row,text,text_len}` | `qh_exec_one_line` | `app_render(RENDER_TEXT)` | **必做** |
| `'4'` | 0x34 | `p.full_screen{color,x,y,text,text_len}` | `qh_exec_full_screen` | `app_render(RENDER_TEXT, x, y)` | **必做** |
| `'5'` | 0x35 | 无 | `qh_exec_clear` | `dev_display_fill` + `commit_frame` | **必做** |
| `'6'` | 0x36 | `p.fixed{type,raw,raw_len}` | `qh_exec_fixed` | `app_render` x5 + `dev_rs232_voice_play` | **必做** |
| `'7'` | 0x37 | `p.civil.idx` | `qh_exec_civil_voice` | `dev_rs232_voice_play` | **必做** |
| `'8'` | 0x38 | `p.brightness.level` | `qh_exec_brightness` | `dev_display_set_brightness` | **必做** |
| `'9'` | 0x39 | `p.volume.level` | `qh_exec_volume` | `dev_rs232_voice_volume` | **必做** |
| `'A'` | 0x41 | `p.peripheral.ctrl` | `qh_exec_peripheral` | `dev_io_lane_light` + `dev_io_flash_light` | **必做** |
| `'B'` | 0x42 | `p.fee{type,amount_fen}` | `qh_exec_voice_fee` | `dev_rs232_voice_play` | **必做** |

> 旧版「`qh_proto_cmd_*` 一把梭 handler」已废弃，拆分为 `qh_parse_frame`（解析器，产出清单）与 `qh_exec_*`（执行器，消费清单）。命令分派不再用函数指针表，而是 `qh_execute_cmd` 内 `switch(cmd)` 直接映射到执行器函数（见 B.4.3.2）。

### B.6.2 解析器→执行器分派（无函数指针表）

分派在 `qh_execute_cmd` 内由 `switch(cmd)` 完成（`cmd` 已是枚举，比函数指针表更直接、可单步）：

```c
void qh_execute_cmd(channel_t *ch, const qh_parsed_cmd_t *cmd)
{
    if (cmd->sta != QH_PARSE_OK) return;   // 解析失败：清单已带 sta
    switch (cmd->cmd) {
    case QH_PCMD_HOST_QUERY:    qh_exec_host_query(ch); break;
    case QH_PCMD_SELF_CHECK:    qh_exec_self_check(ch); break;
    case QH_PCMD_ONE_LINE:      qh_exec_one_line(ch, &cmd->p.one_line); break;
    case QH_PCMD_FULL_SCREEN:   qh_exec_full_screen(ch, &cmd->p.full_screen); break;
    case QH_PCMD_CLEAR:         qh_exec_clear(ch); break;
    case QH_PCMD_FIXED_DISPLAY: qh_exec_fixed(ch, &cmd->p.fixed); break;
    case QH_PCMD_CIVIL_VOICE:   qh_exec_civil_voice(ch, cmd->p.civil.idx); break;
    case QH_PCMD_BRIGHTNESS:    qh_exec_brightness(ch, cmd->p.brightness.level); break;
    case QH_PCMD_VOLUME:        qh_exec_volume(ch, cmd->p.volume.level); break;
    case QH_PCMD_PERIPHERAL:    qh_exec_peripheral(ch, cmd->p.peripheral.ctrl); break;
    case QH_PCMD_VOICE:         qh_exec_voice_fee(ch, cmd->p.fee.type, cmd->p.fee.amount_fen); break;
    default: break;
    }
}
```

解析器 `qh_parse_frame` 内用 `qh_char_to_cmd()` 把 ASCII 命令字映射为枚举（见 B.4.3.1），无需运行时遍历表。

---

## B.7 关键命令详细设计

> 所有设备操作均直接调用 `Device/` 驱动，不使用 `dev_service`。
> **本节每命令拆为「解析（解析器 `qh_parse_frame`，见 B.4.3.1）」+「执行（执行器 `qh_exec_*`，见 B.4.3.2）」两部分**：解析器把 raw bytes 填入清单字段，执行器只读清单字段做映射 + 调驱动。解析器代码已统一在 B.4.3.1，本节不再重复，只展示**执行器**实现与协议要点。

### B.7.0 协议层本地映射表（放 `app_qh_proto_cmd.c` 执行器侧）

```c
#include "app_render.h"
#include "dev_display.h"
#include "dev_io_ctrl.h"
#include "dev_rs232_voice.h"

/* 协议色码 0/1/2 -> display_color_t (红/绿/黄) */
static const display_color_t s_qh_color_map[] = {
    [0] = COLOR_RED, [1] = COLOR_GREEN, [2] = COLOR_YELLOW,
};
#define QH_MAP_COLOR(idx) (((idx) < 3) ? s_qh_color_map[(idx)] : COLOR_GREEN)

/* 亮度：协议 1~5 -> 硬件 0~7（dev_display 范围 0~7）；协议 0=自动(走光敏)，不入表 */
static const uint8_t s_qh_brightness_map[] = {3, 4, 5, 7, 8}; /* 索引 = level-1 */

/* 音量：协议 1~5 -> 语音板音量标签 */
static const uint8_t s_qh_volume_map[] = {0, 0x31, 0x33, 0x35, 0x37, 0x39};
```

### B.7.1 cmd '1' — 主机查询

**协议格式**：
- 发送：`{1 00}`（无参数）
- 应答：`{1 01 00}`（正常）/ `{1 01 01}`（异常）

**解析（解析器）**：无参数，清单仅含 `cmd=QH_PCMD_HOST_QUERY` + `sta=OK`。
**执行（执行器）**（应答帧走串口通道，非设备能力）：

```c
static void qh_exec_host_query(channel_t *ch)
{
    static const uint8_t rsp[] = { '{', '1', '0', '1', '0', '0', '}' };
    channel_send(ch, (uint8_t *)rsp, sizeof(rsp));
}
```

**裸机对齐**：裸机始终返回"正常"（0x00），不检查实际设备状态。STD 工程同样返回固定值。

### B.7.2 cmd '2' — 自检

**协议格式**：
- 发送：`{2 00}`（无参数）
- 行为：全屏显示固定内容 + 语音播报

**解析**：无参数，清单仅含 `cmd=QH_PCMD_SELF_CHECK`。
**执行**：

```c
static void qh_exec_self_check(channel_t *ch)
{
    app_render(&(render_cfg_t){.type = RENDER_FILL, .color = COLOR_YELLOW});

    static const uint8_t voice_text[] = "系统正在加电自检";
    dev_rs232_voice_play(voice_text, sizeof(voice_text) - 1);
}
```

### B.7.3 cmd '3' — 单行显示

**协议格式**：
- 发送：`{3 len color row text...}`
- color: `'0'`=红, `'1'`=绿, `'2'`=黄
- row: `'1'`~`'5'`（对应设备行 0~4）
- text: GBK 文本

**裸机行为（已确认差异）**：
- 裸机 `inbuf[3]` 做 `fontColor = inbuf[3] - 0x30 + 1`（协议值 0→设备值 1）
- STD 工程 `display_color_t` 对齐：`0`→COLOR_RED, `1`→COLOR_GREEN, `2`→COLOR_YELLOW

**解析（解析器写入 `p.one_line`）**：`color=data[0]-'0'`，`row=data[1]-'1'`，`text=&data[2]`，`text_len=len-2`；越界置 `ERR_PARAM`。
**执行（执行器读 `p.one_line`）**：

```c
static void qh_exec_one_line(channel_t *ch, const qh_one_line_t *p)
{
    dev_display_t *d = dev_display_get();
    app_render(&(render_cfg_t){
        .type = RENDER_TEXT, .x = 0, .y = (uint16_t)p->row * FONT_16,
        .w = d ? d->screen_cols : 0, .h = FONT_16,
        .color = QH_MAP_COLOR(p->color),   // 协议→设备映射在执行器
        .text = (const char *)p->text, .len = p->text_len,
        .font_size = FONT_16, .font_type = FONT_ST, .text_enc = FONT_ENC_GBK,
    });
}
```

### B.7.4 cmd '4' — 全屏可编辑显示

**协议格式**：
- 发送：`{4 len color X Y text...}`
- color: `'0'`=红, `'1'`=绿, `'2'`=黄
- X: 起始 X 坐标（0~屏幕宽度）
- Y: 起始 Y 坐标（0~屏幕高度）
- text: GBK 文本，`\n`(0x0A) 换行
- 空格(0x20) 填充空位

**解析（写入 `p.full_screen`）**：`color/data[0]`，`x=data[1]`，`y=data[2]`，`text=&data[3]`，`text_len=len-3`。
**执行**：逐行拆分 text（按 `\n`），每行调用 `app_render(RENDER_TEXT, x, y + line*row_height, ...)`。

```c
static void qh_exec_full_screen(channel_t *ch, const qh_full_screen_t *p)
{
    app_render(&(render_cfg_t){
        .type = RENDER_TEXT, .x = p->x, .y = p->y,
        .color = QH_MAP_COLOR(p->color),
        .text = (const char *)p->text, .len = p->text_len,
        .font_size = FONT_16, .font_type = FONT_ST, .text_enc = FONT_ENC_GBK,
    });
}
```

### B.7.5 cmd '6' — 固定格式显示（核心命令）

**协议格式**：
- 发送：`{6 len type|车型|金额|余额|信息1|信息2}`
- type: `'0'`=客车, `'1'`=货车
- 字段以 `'|'`(0x7C) 分隔

**解析（解析器）**：只做结构校验 + 填 `p.fixed{type, raw, raw_len}`（保留原始 `|` 字段指针），**不**在此拆分字段。
**执行（执行器）**：在 `qh_exec_fixed` 内完成 `|` 字段拆分与 5 行格式化（属协议语义解释，本就在协议层）。

**客车模式（type='0'）— 5 行**：

| 行号 | 内容 | 格式示例 |
|------|------|---------|
| 1 | 车型 | `车型:   1    型` |
| 2 | 金额 | `金额: 12.50 元` |
| 3 | 余额 | `余额: 88.00 元` |
| 4 | 信息1 | 协议传入的文本 |
| 5 | 信息2 | 协议传入的文本 |

**货车模式（type='1'）— 5 行**：

| 行号 | 内容 | 格式示例 | 条件 |
|------|------|---------|------|
| 1 | 车型 | `车型:   3    型` | 始终显示 |
| 2 | 金额 | `金额: 25.80 元` | 始终显示 |
| 3 | 余额 | `余额: 50.00 元` | 始终显示 |
| 4 | 总重 | `总重: 12.50 吨` | > 0.1 吨才显示，否则空白行 |
| 5 | 超重 | `超重:  2.30 吨` | > 0.1 吨才显示，否则空白行 |

**金额格式化**：`tmp_amount + 0.001f`（裸机对齐：补偿浮点精度误差）

**语音播报**：金额 > 0.5 元时自动播报"您好请交费XX.XX元"（通过 `dev_rs232_voice_play`，调 `qh_voice_fee_amount`）。

**执行要点**：协议层完成 `\|` 字段解析与 5 行构造，每行用 `app_render(RENDER_TEXT, y=line*FONT_16, ...)` 显示；金额超阈值时调 `dev_rs232_voice_play`。完整执行器实现见 B.4.3.2 的 `qh_exec_fixed`（含 `memchr` 按 `|` 拆分、客车/货车 5 行构造、金额阈值播报骨架）。

### B.7.6 cmd '7' — 礼貌用语

**协议格式**：
- 发送：`{7 01 index}`
- index: `'0'`~`'3'`

**解析（写入 `p.civil.idx`）**：`idx=data[0]-'0'`，越界置 `ERR_PARAM`。
**执行（读 `p.civil.idx`）**：

索引映射表（对齐裸机 `cmd_civil_voice_play`，**协议层持有**）：

| index | GBK 文本 |
|-------|---------|
| `'0'` | `您好！欢迎行驶贵州高速公路` |
| `'1'` | `请出示通行卡` |
| `'2'` | `谢谢合作！祝您一路平安` |
| `'3'` | `交易不成功，请走人工车道` |

**注意**：协议文档写的是"欢迎行驶高速公路"，但裸机实际使用的是"贵州高速公路"。**STD 工程对齐裸机**。

```c
static void qh_exec_civil_voice(channel_t *ch, uint8_t idx)
{
    qh_voice_civil(idx);   // 见 app_qh_proto_voice.c（B.4.3.3）
}
```

### B.7.7 cmd '8' — 亮度设置

**协议格式**：
- 发送：`{8 01 level}`
- level: `'0'`~`'5'`

**解析（写入 `p.brightness.level`）**：`level=data[0]-'0'`，越界置 `ERR_PARAM`。
**执行（读 `p.brightness.level`）**：`level==0` 为自动档——不写固定亮度，交由板级光敏自动调光（`app_light_sensor` 任务按 4~7 钳位驱动）；`level∈1~5` 为手动档，查表映射到硬件 0~7。

**映射表**（对齐裸机 `cmd_screen_bright_set`，**协议层查表**；仅手动档，索引 = level-1）：

| 协议值 | 含义 | 硬件级（dev_display 0~7） | 表索引 |
|--------|------|--------------------------|--------|
| `'0'` | 自动 | 不走此表（光敏驱动） | — |
| `'1'` | 手动 1 | 3 | 0 |
| `'2'` | 手动 2 | 4 | 1 |
| `'3'` | 手动 3 | 5 | 2 |
| `'4'` | 手动 4 | 7 | 3 |
| `'5'` | 手动 5 | 8 | 4 |

```c
static void qh_exec_brightness(channel_t *ch, uint8_t level)
{
    dev_display_t *d = dev_display_get();
    if (!d) return;
    if (level == 0) {
        /* 自动档：不强制固定亮度，交由板级光敏自动调光（dev_light_sensor 钳位 4~7） */
        return;
    }
    /* 手动档：level∈1~5，映射表索引 = level-1 */
    dev_display_set_brightness(d, s_qh_brightness_map[level - 1]);
}
```

### B.7.8 cmd '9' — 音量设置

**协议格式**：
- 发送：`{9 01 level}`
- level: `'1'`~`'5'`

**解析（写入 `p.volume.level`）**：`level=data[0]-'0'`，越界置 `ERR_PARAM`。
**执行（读 `p.volume.level`）**：协议层做 1~5 → 音量标签映射，调用 `dev_rs232_voice_volume(mapped_level)`。

**映射表**（对齐裸机 `cmd_voice_volume_set`，**协议层查表**）：

| 协议值 | 音量标签 |
|--------|---------|
| `'1'` | 0x31 |
| `'2'` | 0x33 |
| `'3'` | 0x35 |
| `'4'` | 0x37 |
| `'5'` | 0x39 |

```c
static void qh_exec_volume(channel_t *ch, uint8_t level)
{
    dev_rs232_voice_volume(s_qh_volume_map[level]);
}
```

### B.7.9 cmd 'A' — 外设控制

**协议格式**：
- 发送：`{A 01 ctrl}`
- ctrl: 位域控制字节

**位域定义**（对齐裸机 `cmd_ws`）：

| bit | 设备 | 1=开 | 0=关 |
|-----|------|------|------|
| bit0 | 绿灯 | 绿灯亮 | 绿灯灭 |
| bit1 | 红灯 | 红灯亮 | 红灯灭 |
| bit2 | 黄闪 | 黄闪开 | 黄闪关 |

**裸机行为**：绿灯和红灯可同时亮（位独立控制）。STD 工程中 `dev_io_lane_light(green)` 为互斥接口（绿灯优先）。对齐裸机：bit0=1 且 bit1=0 -> 绿灯；bit1=1 -> 红灯（优先）。

**解析（写入 `p.peripheral.ctrl`）**：`ctrl=data[0]`。
**执行（读 `p.peripheral.ctrl`）**：

```c
static void qh_exec_peripheral(channel_t *ch, uint8_t ctrl)
{
    bool green  = (ctrl & 0x01) && !(ctrl & 0x02); // bit0=绿, bit1=红(优先)
    bool red    = (ctrl & 0x02);
    bool yellow = (ctrl & 0x04);

    if (red)        dev_io_lane_light(false); // 红灯
    else if (green) dev_io_lane_light(true);  // 绿灯
    dev_io_flash_light(yellow);
}
```

### B.7.10 cmd 'B' — 费额语音播报

**协议格式**：
- 发送：`{B 06 type d0 d1 d2 d3 d4}`
- type: `'0'`=播报金额, `'1'`=播报剩余次数
- d0~d4: 5 位数字（万/千/百/十/个）

**解析（写入 `p.fee`）**：`type=data[0]-'0'`，`amount_fen` = 5 位 BCD 合成（见 B.4.3.1）。
**执行（读 `p.fee`）**：

```c
static void qh_exec_voice_fee(channel_t *ch, uint8_t type, uint32_t amount_fen)
{
    (void)type;  // '1'=剩余次数；本期对齐裸机仅实现金额播报
    qh_voice_fee_amount(amount_fen);   // 见 app_qh_proto_voice.c（B.4.3.3 / B.8.2）
}
```

---

## B.8 语音子系统

### B.8.1 青海语音模式

青海协议使用**纯文本 TTS 模式**（与 CQ 的索引组合模式不同）：

| 特征 | 青海 | 重庆 |
|------|------|------|
| TTS 方式 | **GBK 文本直接合成** | 索引组合 + 分隔符 |
| 费用播报 | "您好请交费XX.XX元" | 索引组合 |
| 预置短语 | 4 条（cmd '7'） | 数十条 |

### B.8.2 费额语音文本构造（执行器侧，`app_qh_proto_voice.c`）

**裸机对齐**：金额 + 0.001f（浮点精度补偿），整数和小数部分分别提取。

```c
void qh_voice_fee_amount(uint32_t amount_fen)
{
    if (amount_fen < 50) return; // 不足 0.5 元不播报
    uint32_t yuan = amount_fen / 100;
    char text[64];
    int n = snprintf(text, sizeof(text), "您好请交费%lu元", (unsigned long)yuan);
    if (n > 0 && n < (int)sizeof(text))
        dev_rs232_voice_play((const uint8_t *)text, (uint16_t)n);
}
```

### B.8.3 TTS 帧格式（新增 `232_voice` 模块负责）

TTS 语音通过 RS232 发送给语音控制板。帧格式：

```
FD | 00 | [Len+2] | 01 | 01 | GBK text bytes...
```

由 `Device/Comm/dev_rs232_voice.c` 内部的 `dev_rs232_voice_play` 组装并发送，协议层只传 GBK 文本与长度（详见 Part A A.5）。

---

## B.9 设备能力桥接

`app_qh_proto_cmd.c`（**执行器**）的职责是**将 QH 协议语义（来自清单）直接转发到 `Device/` 驱动**，而非新增共享服务层。

**原则**：执行器**直接调用** `app_render` / `dev_display_*` / `dev_io_ctrl` / `dev_rs232_voice_*`，不引入 `dev_service_*` 中间层。协议特有的映射（颜色、亮度、音量）以本地查表方式放在执行器。清单 `qh_parsed_cmd_t` 是解析器与执行器之间**唯一的耦合点**。

**桥接示例**（执行器读清单，做映射 + 调驱动）：

```c
static void qh_exec_one_line(channel_t *ch, const qh_one_line_t *p)
{
    dev_display_t *d = dev_display_get();
    app_render(&(render_cfg_t){
        .type = RENDER_TEXT, .x = 0, .y = (uint16_t)p->row * FONT_16,
        .color = QH_MAP_COLOR(p->color),   // 协议→设备映射在执行器
        .text = (const char *)p->text, .len = p->text_len,
        .font_size = FONT_16, .font_type = FONT_ST, .text_enc = FONT_ENC_GBK,
    });
}

static void qh_exec_peripheral(channel_t *ch, uint8_t ctrl)
{
    bool green  = (ctrl & 0x01) && !(ctrl & 0x02);
    bool red    = (ctrl & 0x02);
    bool yellow = (ctrl & 0x04);
    if (red)         dev_io_lane_light(false);
    else if (green)  dev_io_lane_light(true);
    dev_io_flash_light(yellow);
}
```

---

## B.10 配置与默认值

| 项 | 默认值 | 落点 |
|----|--------|------|
| 串口波特率 | 9600 | `app_qh_proto_cfg` |
| 显示行数上限 | 5 | 硬编码 |
| 字号 | 16 点阵 | 硬编码（裸机固定 16 点阵） |
| 字体 | 宋体 | 硬编码（裸机默认宋体） |
| 上电欢迎语 | "祝您一路平安" | 硬编码 GBK |

---

## B.11 已确认问题汇总

| 问题 | 确认结果 |
|------|----------|
| Q1. 与 LDI 的产品关系 | **编译期互斥**，运行时只执行一种 |
| Q2. 通信通道 | **RS232 串口**（CH_ID_RS232, USART3） |
| Q3. 语音通道 | **RS232 串口**（`232_voice` 模块封装，经语音板物理帧） |
| Q4. 字号 | **固定 16 点阵**，协议不支持字号选择 |
| Q5. 字体 | **固定宋体**，协议不支持字体选择 |
| Q6. 行号 | **1~5**（对应设备行 0~4） |
| Q7. cmd '1' 应答 | **固定返回正常**，不检查实际设备状态（走 `channel_send`） |
| Q8. cmd '2' 自检 | 全屏黄色 + 语音"系统正在加电自检" |
| Q9. cmd '7' 礼貌用语 | **对齐裸机**，使用"贵州高速公路"而非协议文档的"高速公路" |
| Q10. 亮度映射 | **查表法**（协议层），裸机 `cmd_screen_bright_set` 的 switch-case |
| Q11. 音量映射 | **查表法**（协议层），裸机 `cmd_voice_volume_set` 的 switch-case |
| Q12. 外设控制 | **位独立控制**，红灯优先于绿灯（对齐裸机） |
| Q13. 费额语音 | **纯文本 TTS**，"您好请交费XX.XX元" |
| Q14. 金额精度 | **+0.001f 补偿**（裸机对齐） |
| Q15. 上电欢迎语 | **"祝您一路平安"** 显示 + 语音 |
| Q16. 帧无校验 | QH 协议**无 CRC**，probe 仅校验帧头帧尾 |
| Q17. cmd '4' X/Y 坐标 | 裸机忽略 X/Y（硬编码 0,0），STD 工程**应透传到渲染层** |
| Q18. RS232 波特率 | **9600~115200 可调**，默认 9600 |
| Q19. cmd '6' 货车总重/超重 | **0 吨不显示**（显示空白行），对齐裸机 |
| Q20. 文件名 | **`app_qh_proto_*` 样式** |
| Q21. 设备层 | **不引入 `dev_service` 共享层**；直接复用 `Device/` 驱动，仅新增 `232_voice` |

---

## B.12 风险与约束

1. **串口粘包**：RS232 是流式传输，`{` 和 `}` 可能跨多个收包周期。probe 必须支持跨包扫描。
2. **帧无校验**：QH 协议无 CRC，串口干扰可能导致误解析。考虑在 probe 中增加 cmd 字节合法性检查（`'1'~'B'`）作为弱校验。
3. **浮点运算**：cmd '6'/'B' 的金额解析使用 `strtod` / `sprintf`，需要 `-lm` 链接。FreeRTOS 堆 32KB，限制帧长。
4. **编码**：源文件中文故障串必须按 GB2312 字节写入或用 `\x` 转义。
5. **内存**：cmd '6' 的 5 行缓冲区需要 5x64=320 字节栈空间。FreeRTOS 任务栈需 >= 512 字节。
6. **裸机差异**：cmd '7' 的"贵州高速公路"与协议文档不一致，需对齐裸机行为。
7. **无 `dev_service` 层**：设备调用分散在协议命令处理函数中，映射表以本地 `static const` 形式存在于 `app_qh_proto_cmd.c`，注意保持单一来源。

---

## B.13 实施阶段与验收

### 阶段 0 — 设备缺口补充

- 新增 `Device/Comm/dev_rs232_voice.c` + `Device/Inc/dev_rs232_voice.h`
- 实现 `dev_rs232_voice_play` / `dev_rs232_voice_volume`
- Makefile 将 `dev_rs232_voice.c` 编入 QH（及 CQ）条件组

**验收**：`make PROTO=QH -j8` 通过，`dev_rs232_voice_play` 可被调用；LDI/CQ 不受影响。

### 阶段 1 — 骨架

- 目录 + Makefile 条件编译
- `app_qh_proto.c`：initcall、probe、handle_task（同步 `parse→execute`）
- `app_qh_proto_parse.c`：清单结构体 `qh_parsed_cmd_t` + `qh_parse_frame`（先只覆盖 cmd `'1'`/`'3'`/`'8'` 验证骨架）
- `app_qh_proto_cmd.c`：`qh_execute_cmd` + `qh_exec_*` 执行器骨架
- RS232 通道绑定（`CH_ID_RS232`）

**验收**：`make PROTO=QH -j8` 通过；发送 cmd `'1'`/`'3'` 验证 parse→execute→device 全链路。

### 阶段 2 — 基础命令

- cmd '1' 查询应答（`channel_send`）
- cmd '3' 单行显示（`app_render`）
- cmd '5' 清屏（`dev_display_fill` + `commit_frame`）
- cmd '8' 亮度（`dev_display_set_brightness`）
- cmd '9' 音量（`dev_rs232_voice_volume`）

**验收**：串口工具发送对应帧，屏显与裸机对照。

### 阶段 3 — 核心显示 + 语音

- cmd '6' 固定格式显示（客车/货车）
- cmd '7' 礼貌用语
- cmd 'B' 费额语音
- cmd '4' 全屏可编辑显示

**验收**：上位机发送完整费额数据，屏显 + 语音与裸机对照。

### 阶段 4 — 外设 + 收尾

- cmd 'A' 外设控制（`dev_io_lane_light` / `dev_io_flash_light`）
- cmd '2' 自检
- 上电欢迎语
- LDI 不受影响回归
- 文档归档

**验收**：全部 11 条命令逐项验证通过。

---

## B.14 验收标准

| 验收项 | 标准 |
|--------|------|
| `app_qh_proto.c/h` 编译 | `make PROTO=QH -j8` 通过 |
| Parse/Execute 分离 | 解析逻辑仅在 `app_qh_proto_parse.c`（`qh_parse_frame` 不调用任何 `Device/` 驱动）；设备调用仅在 `app_qh_proto_cmd.c`（执行器） |
| 清单为唯一边界 | `handle_task` 仅 `qh_parse_frame()` + `qh_execute_cmd()`；无函数指针命令表、无事件队列 |
| 无 `dev_service` 层 | 全仓无 `dev_service_*` 调用 |
| `232_voice` 模块 | `dev_rs232_voice_play` / `dev_rs232_voice_volume` 编译可用 |
| LDI 不受影响 | `make PROTO=LDI -j8` 编译不变 |
| CQ 不受影响 | `make PROTO=CQ -j8` 编译不变 |
| cmd '1' 查询 | 串口发送 `{1 00}`，收到 `{1 01 00}` |
| cmd '3' 单行显示 | 发送文本，对应行正确显示 |
| cmd '5' 清屏 | 发送后全屏清除 |
| cmd '6' 客车 | 费额信息正确显示 + 语音播报 |
| cmd '6' 货车 | 费额/总重/超重正确显示 + 语音播报 |
| cmd 'A' 外设 | 绿灯/红灯/黄闪正确响应 |
| 亮度 0~5 | 各级别亮度正确切换 |
| 音量 1~5 | 各级别音量正确切换 |
| 接口头文件无循环依赖 | `app_qh_proto.h` 不依赖任何设备/协议私有头（仅依赖 `stdint.h` + `channel_t` 前向声明） |
