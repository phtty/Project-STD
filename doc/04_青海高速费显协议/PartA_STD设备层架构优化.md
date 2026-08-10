# Part A：STD 设备能力复用与缺口分析（青海协议适配）

**状态**：`[规划中]`
**目标**：评估青海高速费显协议对 STD 设备层的能力需求，确认现有 `Device/` 驱动的覆盖范围，仅对协议中确实缺失的设备能力做新增封装（如 `232_voice`），其余直接复用 `Device/` 驱动。
**协议版本**：青海高速费显协议 2019-10-06
**参考裸机工程**：`9K1030CE00`
**协议原文**：`青海协议修改20191006.doc`
**前置条件**：`Device/` 现有外设驱动已就绪（HUB75 显示、通信灯、报警灯、光敏传感器、RS485、网口）；仅 `232_voice`**（TTS over RS232 语音板）** 需新增一个 `Device/` 级模块。

---

## A.0 架构决策记录

### 方案选型：是否保留 `dev_service` 共享层


| 方案          | 描述                                                                                                                    | 优缺点                                                                                                                    | 结论     |
| ----------- | --------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- | ------ |
| **原方案（废弃）** | 新建跨协议共享的 `dev_service` 层，新增 `dev_service_host_status_query()` / `dev_service_show_text_xy()` 等接口                      | 青海所需的外设能力（HUB75、通信灯、报警灯、光敏、485、网口）已由 `Device/` 驱动提供，再建一层只是转发，徒增冗余；且 `dev_service_host_status_query` 本质是串口回发应答帧，不属于设备能力 | **废弃** |
| **现方案（采用）** | **砍掉** `dev_service` **共享层**。QH 协议层直接复用 `Device/` 现有驱动；仅「协议无关的设备能力缺口」在 `Device/` 内新增模块（`232_voice`）；协议特有的映射/格式构造留在协议层 | 与 LDI 现有套路一致（LDI 也是直接调 `Device/` 驱动），无多余抽象层，职责清晰                                                                       | **采用** |




### 判定标准

- **直接复用** `Device/` **驱动**：参数简单、与硬件强相关、已在 `Device/` 封装好的能力（HUB75、通信灯、报警灯、光敏、485、网口、亮度）。
- `Device/` **内新增模块**：能力确实缺失且协议无关、可跨协议复用（如 `232_voice` 语音板 TTS 发送）。
- **留在协议层（ProtocolParser_QingHai）**：协议特有的映射（亮度 0~5→硬件级、颜色 0/1/2→红/绿/黄）、格式构造（`\|` 字段解析、礼貌用语索引表、费额金额→文本）、主机查询应答帧（串口回发）。



### 与 LDI 的关系

LDI 的 `Application/Src/LDI/app_ldi_device.c` 直接调用 `app_render` / `dev_display_*` / `dev_io_ctrl`（如 `dev_io_lane_light`、`dev_io_flash_light`），并不存在跨协议共享的 `dev_service` 层。QH 采用同一套路：**协议层直接调** `Device/` **驱动**，不另起共享服务层。

### 协议层内部架构：Parse/Execute + Tagged Union

「协议层直接复用 `Device/` 驱动」确定后，协议层**内部**如何组织仍需决策：是把「帧解析 + 命令分发 + 设备调用 + 回响应」一把梭在一个 cmd handler 里（LDI 现状），还是把「解析」与「执行」分离、以 **Tagged Union 结构体**作为两者边界。


| 方案                                   | 描述                                                                                           | 结论                                                                                                                                                           |
| ------------------------------------ | -------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 一把梭（LDI 现状）                          | 每个 cmd handler 内部自己强转 raw data、解析参数、调驱动、回响应                                                  | 命令少时简单，但解析/执行耦合，难单测，裸机差异（Q9/Q14/Q17/Q19）难定位                                                                                                                  |
| 事件驱动 Pub/Sub                         | 解析器 emit 事件入队，事件循环再分派到 handler                                                               | 与现有 `app_dispatch` 的「ring_buffer→probe→osMessageQueue→协议任务」**重复成双重队列**（额外 RAM + memcpy + 上下文切换），且打破 cmd `'1'`/`'B'` 「解析完立刻回响应」的同步时序；QH 命令纯请求-响应、无优先级抢占，属过度设计 |
| **Parse/Execute + Tagged Union（采用）** | 解析器把 raw bytes 解成 `qh_parsed_cmd_t`（枚举 cmd + 校验状态 + 各命令已解码参数 union）；执行器按 cmd 分派，只读 union 调驱动 | 解析与执行解耦、清单可 dump 调试/对账裸机、解析器是纯函数可单测、与 LDI `ldi_ctrl_*_t` 套路一致；在已有 frame_queue 之后、在 `handle_task` 内**同步** parse→execute，不加新队列                                 |


**为什么不用事件驱动**：现有 `app_dispatch` 已对每条协议做了异步分发（见 `app_dispatch.h` 的 `frame_msg_t` 队列）。再在协议层套一层 event queue 就是双重队列；且同步响应时序会被破坏。Tagged Union 是「在 handle_task 内同步 parse→execute」，零额外队列、不破坏响应时序。

**清单（Tagged Union）设计原则**：

- `cmd` 映射为枚举，`sta` 拆为结构/语义两级校验（`QH_PARSE_OK` / `ERR_FRAME` / `ERR_CMD` / `ERR_PARAM`）；
- union 内只放**协议域已解码**的值（颜色索引 0/1/2、亮度 0~5、bit 位、金额分），**不放设备域值**（如 `COLOR_RED`、硬件亮度级）——映射留在执行器；
- 复杂命令（如 cmd `'6'`）解析器只做结构校验 + 保留原始 `|` 字段指针，字段拆分 / 5 行格式化留给执行器（属协议语义解释，本就在协议层）；
- 解析器与执行器在**同一任务上下文同步调用**，清单里 `data` 指针指向 `frame_msg_t` 缓冲区，必须同步消费，禁止跨任务异步使用。

---



## A.1 青海协议功能全景——分类总表

> **分类原则**：
>
> - **Device/ 已有驱动**：硬件能力已在 `Device/` 封装，协议层直接调用，无需新增。
> - **需新增 Device/ 模块**：设备能力缺失且协议无关，`Device/` 内新增（仅 `232_voice`）。
> - **协议层逻辑**：解析/映射/格式构造属协议特有，留在 `ProtocolParser_QingHai`。



### A.1.1 全部功能清单


| 序号  | 功能描述                           | 分类                         | 实现位置                                                 | 说明                                       |
| --- | ------------------------------ | -------------------------- | ---------------------------------------------------- | ---------------------------------------- |
| F01 | ASCII 帧定界 `{`...`}` 扫描         | **协议层**                    | `app_qh_proto.c` probe                               | 青海特有的 `{cmd len data}` 帧格式               |
| F02 | cmd 字节查表分发 `'1'~'B'`           | **协议层**                    | `app_qh_proto_cmd.c`                                 | ASCII 命令字分发表                             |
| F03 | 主机状态查询应答 `{1 01 00}`           | **协议层**                    | `app_qh_proto_cmd.c`                                 | 串口 RS232 回发应答帧（非设备能力）                    |
| F04 | 单行显示：颜色 + 行号 + GBK 文本          | **Device/ 已有**             | `app_render` + `dev_display`                         | 协议层做颜色映射后构造 `render_cfg_t`               |
| F05 | 全屏可编辑显示：颜色 + X/Y + 文本          | **Device/ 已有**             | `app_render` + `dev_display`                         | `RENDER_TEXT` 本身支持任意 x/y                 |
| F06 | 全屏清除                           | **Device/ 已有**             | `dev_display_fill` + `commit_frame`                  | 填充黑并提交帧                                  |
| F07 | 固定格式显示（客车）5 行 + 费额语音           | **协议层 + 232_voice**        | `app_qh_proto_cmd.c` + `dev_rs232_voice_play`        | 协议层解析 `                                  |
| F08 | 固定格式显示（货车）5 行 + 费额语音           | **协议层 + 232_voice**        | `app_qh_proto_cmd.c` + `dev_rs232_voice_play`        | 同上，增加总重/超重行                              |
| F09 | 礼貌用语索引→GBK 文本→TTS              | **协议层 + 232_voice**        | `app_qh_proto_voice.c` + `dev_rs232_voice_play`      | 协议层完成索引→文本映射                             |
| F10 | 亮度设置：协议 0~~5 → 硬件 0~~7         | **Device/ 已有**             | `dev_display_set_brightness`                         | 映射在协议层，调 `dev_display`                   |
| F11 | 音量设置：协议 1~5 → 标签               | **Device/ 新增**             | `dev_rs232_voice_volume`                             | 经 `232_voice` 模块                         |
| F12 | 外设控制：bit0=绿灯, bit1=红灯, bit2=黄闪 | **Device/ 已有**             | `dev_io_lane_light` + `dev_io_flash_light`           | 协议层解析 bit 位后调用                           |
| F13 | 费额语音：数字→金额文本→TTS               | **协议层 + 232_voice**        | `app_qh_proto_voice.c` + `dev_rs232_voice_play`      | 协议层构造文本                                  |
| F14 | 自检模式：全屏黄 + 语音                  | **Device/ 已有 + 232_voice** | `app_render` + `dev_rs232_voice_play`                | 协议层编排                                    |
| F15 | 上电欢迎语："祝您一路平安" + 语音            | **协议层 + 232_voice**        | `app_qh_proto.c` (init)                              | 协议层构造文本                                  |
| F16 | 串口 RS232 收发（`CH_ID_RS232`）     | **Device/ 已有**             | 已有 `dev_rs232.c` + 通道框架                              | 通道已实现，青海绑定即可                             |
| F17 | 文字渲染（GBK→点阵→屏显）                | **Device/ 已有**             | 已有 `app_render`                                      | 16 点阵单行/多行显示                             |
| F18 | HUB75 全屏颜色填充                   | **Device/ 已有**             | 已有 `dev_display` + `app_render`                      | `RENDER_FILL`                            |
| F19 | HUB75 清屏                       | **Device/ 已有**             | 已有 `dev_display`                                     | 填充黑 + 提交                                 |
| F20 | 亮度调节（手动 0~7 / 光敏自动）            | **Device/ 已有**             | 已有 `dev_display_set_brightness` + `dev_light_sensor` | 手动经 `dev_display`，自动经 `dev_light_sensor` |
| F21 | TTS 语音播报（GBK 文本 → 语音板）         | **Device/ 新增**             | `dev_rs232_voice_play`                               | **缺失，需在** `Device/Comm/` **新增**          |
| F22 | TTS 音量设置                       | **Device/ 新增**             | `dev_rs232_voice_volume`                             | 随 `232_voice` 模块新增                       |
| F23 | 车道灯 GPIO 控制                    | **Device/ 已有**             | 已有 `dev_io_lane_light`                               | 通信灯                                      |
| F24 | 黄闪报警控制                         | **Device/ 已有**             | 已有 `dev_io_flash_light`                              | 报警灯                                      |




### A.1.2 分类统计


| 分类                 | 数量                                         | 说明                                     |
| ------------------ | ------------------------------------------ | -------------------------------------- |
| **Device/ 已有驱动**   | 14 (F04~~F06, F10, F12, F16~~F20, F23~F24) | 直接复用，零新增                               |
| **需新增 Device/ 模块** | 1 类（`232_voice`：F11, F21, F22）             | 仅在 `Device/Comm/` 新增 `dev_rs232_voice` |
| **协议层逻辑**          | 9 (F01~~F03, F07~~F09, F13~F15)            | 映射/格式构造/应答帧，留在协议层                      |


**核心结论：砍掉** `dev_service` **共享层；14 项设备能力由** `Device/` **驱动直接复用，仅缺「TTS over RS232 语音板」一项，在** `Device/` **新增** `232_voice` **模块；其余协议特有逻辑留在协议层。**

---



## A.2 设备能力调用矩阵



### A.2.1 各命令 → 协议层动作 → `Device/` 驱动调用


| cmd   | ASCII | 协议层动作                   | `Device/` 驱动调用                                                                      | 备注              |
| ----- | ----- | ----------------------- | ----------------------------------------------------------------------------------- | --------------- |
| `'1'` | 0x31  | 构造应答帧 `{1 01 00}`       | `channel_send(ch, rsp, len)`（RS232 回发）                                              | 非设备能力，走通道       |
| `'2'` | 0x32  | 全屏黄 + 语音                | `app_render(RENDER_FILL, COLOR_YELLOW)` + `dev_rs232_voice_play("系统正在加电自检")`        | 自检              |
| `'3'` | 0x33  | 颜色映射(0~2→红/绿/黄) + 行号→y  | `app_render(RENDER_TEXT, x=0, y=row*FONT_16, ...)`                                  | 单行显示            |
| `'4'` | 0x34  | 颜色映射 + 解析 X/Y + `\n` 换行 | `app_render(RENDER_TEXT, x, y, ...)`                                                | 全屏可编辑           |
| `'5'` | 0x35  | 无参数                     | `dev_display_fill(dev,0,0,rows,cols,COLOR_BLACK)` + `dev_display_commit_frame(dev)` | 清屏              |
| `'8'` | 0x38  | 映射 0~~5→硬件 0~~7         | `dev_display_set_brightness(dev, mapped_level)`                                     | 亮度（映射在协议层）      |
| `'9'` | 0x39  | 映射 1~5→音量标签             | `dev_rs232_voice_volume(mapped_level)`                                              | 音量（经 232_voice） |
| `'A'` | 0x41  | 解析 bit0/bit1/bit2       | `dev_io_lane_light(green)` + `dev_io_flash_light(yellow)`                           | 外设              |



| cmd   | ASCII | 协议层动作（业务逻辑）            | `Device/` 驱动调用               | 备注                             |
| ----- | ----- | ---------------------- | ---------------------------- | ------------------------------ |
| `'6'` | 0x36  | 解析 `                   | ` 分隔字段 + 构造 5 行              | `app_render(RENDER_TEXT)` x5 行 |
| `'6'` | 0x36  | 金额 > 阈值时               | `dev_rs232_voice_play(text)` | 语音播报                           |
| `'7'` | 0x37  | 索引→文本映射                | `dev_rs232_voice_play(text)` | 礼貌用语                           |
| `'B'` | 0x42  | 解析 type + 5 位数字 + 构造文本 | `dev_rs232_voice_play(text)` | 费额语音                           |




### A.2.2 与 LDI 的设备调用复用对比


| 设备能力              | 青海 QH 调用                       | LDI 调用                                               | 复用说明                                   |
| ----------------- | ------------------------------ | ---------------------------------------------------- | -------------------------------------- |
| HUB75 填充/清屏/文字    | `app_render` + `dev_display_*` | `app_render` + `dev_display_*`（经 `app_ldi_device.c`） | 同一套 `Device/` 驱动                       |
| 通信灯               | `dev_io_lane_light`            | `dev_io_lane_light`（经 `app_ldi_device.c`）            | 完全一致                                   |
| 报警灯/黄闪            | `dev_io_flash_light`           | `dev_io_flash_light`（经 `app_ldi_device.c`）           | 完全一致                                   |
| 亮度                | `dev_display_set_brightness`   | `dev_display_set_brightness`（经 `app_ldi_device.c`）   | 完全一致                                   |
| 光敏自动亮度            | `dev_light_sensor`             | `dev_light_sensor`                                   | 完全一致                                   |
| RS232 收发          | `dev_rs232` + 通道               | `dev_rs232` + 通道                                     | 完全一致                                   |
| **TTS 语音（缺失→新增）** | `dev_rs232_voice_play`         | 无（LDI 不碰语音）                                          | **新增** `Device/Comm/dev_rs232_voice.c` |


---



## A.3 设备能力缺口分析



### A.3.1 已确认无缺口（直接复用 `Device/` 驱动）


| 能力                   | 状态      | 调用入口                                                            |
| -------------------- | ------- | --------------------------------------------------------------- |
| HUB75 文字显示（单行/多行/XY） | **已覆盖** | `app_render`（RENDER_TEXT，支持任意 x/y）                              |
| HUB75 颜色填充 / 清屏      | **已覆盖** | `app_render(RENDER_FILL)` / `dev_display_fill` + `commit_frame` |
| 通信灯（通行灯）             | **已覆盖** | `dev_io_lane_light(bool)`                                       |
| 报警灯 / 黄闪             | **已覆盖** | `dev_io_flash_light(bool)`                                      |
| 亮度调节（手动 0~7）         | **已覆盖** | `dev_display_set_brightness(dev, level)`                        |
| 光敏自动亮度               | **已覆盖** | `dev_light_sensor`                                              |
| RS232 串口收发           | **已覆盖** | `dev_rs232.c` + `CH_ID_RS232` 通道                                |
| RS485 / 网口           | **已覆盖** | `dev_rs485.c` / `dev_eth.c`（青海未用，但能力就绪可复用）                      |




### A.3.2 需新增的 `Device/` 模块


| 序号  | 模块                              | 位置                                                               | 用途                                          |
| --- | ------------------------------- | ---------------------------------------------------------------- | ------------------------------------------- |
| N1  | `232_voice`（TTS over RS232 语音板） | `Device/Comm/dev_rs232_voice.c` + `Device/Inc/dev_rs232_voice.h` | 把 GBK 文本按语音板物理帧格式经 RS232 发送；协议无关，CQ/QH 均可复用 |


接口：

```c
/* Device/Inc/dev_rs232_voice.h */
#pragma once
#include <stdint.h>

#define DEV_RS232_VOICE_MAX_TEXT (200U)

/** TTS 合成播放（GBK 文本），经 RS232 发送至语音板 */
void dev_rs232_voice_play(const uint8_t *gbk_text, uint16_t len);

/** TTS 设置音量（协议无关的音量标签） */
void dev_rs232_voice_volume(uint8_t level);
```



### A.3.3 协议层逻辑（非设备缺口，留在协议层）


| 序号  | 项                 | 详情                                                                                  | 归属  |
| --- | ----------------- | ----------------------------------------------------------------------------------- | --- |
| M1  | 亮度映射 0~~5→硬件 0~~7 | QH 协议 0~~5 映射到~~ `dev_display` ~~的 0~~7 级（查表法）；协议层查表后调 `dev_display_set_brightness` | 协议层 |
| M2  | 颜色映射 0/1/2→红/绿/黄  | QH 协议色码 → `display_color_t`（COLOR_RED/GREEN/YELLOW）                                 | 协议层 |
| M3  | 主机查询应答帧           | cmd `'1'` 经 RS232 回发 `{1 01 00}`，属通道收发，非设备能力                                        | 协议层 |
| M4  | 礼貌用语索引表 / 费额文本构造  | 协议特有的索引→文本映射、金额→"您好请交费XX元"                                                          | 协议层 |
| M5  | 外设控制位解析           | bit0/bit1/bit2 → `dev_io_lane_light` / `dev_io_flash_light`                         | 协议层 |


---



## A.4 设备架构



### A.4.1 分层架构（无 `dev_service` 层）

```
+--------------------------------------------------+
|  Protocol Layer (LDI / CQ / RLS / QH)             |
|  内部再分（以 QH 为例）：                          |
|   解析器 qh_parse_frame()                         |
|     bytes ──▶ qh_parsed_cmd_t（Tagged Union 清单）  |
|   执行器 qh_execute_cmd()                         |
|     清单 ──▶ 协议→设备映射 + Device/ 驱动调用       |
|  职责：帧解析 + 命令分发 + 协议特有的映射/格式构造   |
|  例如：cmd '6' \| 分隔解析、cmd '7' 索引映射、      |
|       cmd 'B' 金额文本构造、cmd '8' 亮度映射、      |
|       cmd 'A' bit 位解析、cmd '1' 应答帧回发        |
|  调用：Device/ 驱动 + 232_voice 模块               |
+--------------------------------------------------+
|  Device Layer (Device/)                           |
|  app_render / dev_display   (HUB75 显示)           |
|  dev_io_ctrl             (通信灯 / 报警灯)          |
|  dev_light_sensor         (光敏)                   |
|  dev_rs232 (+ 232_voice)  (RS232 / 语音板 TTS)     |
|  dev_rs485 / dev_eth      (485 / 网口)             |
+--------------------------------------------------+
```

> **说明**：不再存在跨协议共享的 `dev_service` 层。QH 协议层与 LDI 一样，直接调用 `Device/` 驱动；仅「TTS 语音板」能力缺失，在 `Device/Comm/` 新增 `232_voice` 模块。协议层**内部**采用 Parse/Execute + Tagged Union（详见 Part B B.4）：解析器产出「清单」结构体，执行器消费清单并调设备。



### A.4.2 青海协议在分层中的位置

```
串口 RS232 (CH_ID_RS232)
  -> app_channel_dispatch
  -> ring_buffer -> frame_dispatch
  -> qh_proto_probe_frame (帧定界: '{' ... '}')
  -> g_qh_proto_frame_queue (已有队列，非新增)
  -> qh_proto_handle_task
       |
       |  // 同步、同一任务上下文，无额外队列
       |  qh_parsed_cmd_t cmd = qh_parse_frame(msg->data, msg->data_len);  // 解析器
       |  qh_execute_cmd(msg->ch, &cmd);                                   // 执行器
       |
  |--- 解析器 qh_parse_frame：bytes → 清单（协议域已解码，不碰设备）---
  |    结构校验 -> cmd 枚举 -> 各命令参数填入 union
  |
  |--- 执行器 qh_execute_cmd：清单 → Device/ 驱动（做协议→设备映射后调用）---
  |    cmd '2' -> app_render(RENDER_FILL, YELLOW) + dev_rs232_voice_play("自检")
  |    cmd '3' -> app_render(RENDER_TEXT, 颜色映射, y=row*FONT_16, ...)
  |    cmd '4' -> app_render(RENDER_TEXT, 颜色映射, x, y, ...)
  |    cmd '5' -> dev_display_fill(BLACK) + dev_display_commit_frame
  |    cmd '8' -> dev_display_set_brightness(dev, 映射后 level)
  |    cmd '9' -> dev_rs232_voice_volume(映射后 level)
  |    cmd 'A' -> dev_io_lane_light(green) + dev_io_flash_light(yellow)
  |    cmd '1' -> channel_send(ch, "{1 01 00}", ...)    <-- 应答帧，走通道
  |    cmd '6' -> 解析 \| 字段 + 构造 5 行 -> app_render x5 + dev_rs232_voice_play
  |    cmd '7' -> 索引→文本映射 -> dev_rs232_voice_play
  |    cmd 'B' -> 解析 + 构造金额文本 -> dev_rs232_voice_play
```

---



## A.5 `232_voice` 设备模块设计（唯一新增）



### A.5.1 定位

- **位置**：`Device/Comm/dev_rs232_voice.c` + `Device/Inc/dev_rs232_voice.h`（与 `dev_rs232.c` 同目录）。
- **职责**：协议无关的「GBK 文本 → 语音板物理帧 → RS232 发送」。帧格式是语音板的，不是任何协议的。
- **复用范围**：QH 与 CQ 共用（谁用语音板谁调它）。



### A.5.2 语音板帧格式

```
FD | 00 | [Len+2] | 01 | 01 | GBK text bytes...
```

由 `dev_rs232_voice_play` 内部组装，协议层只传 GBK 文本与长度。

### A.5.3 接口实现要点

```c
/* Device/Comm/dev_rs232_voice.c */
#include "dev_rs232_voice.h"
#include "dev_rs232.h"   // RS232 通道发送

void dev_rs232_voice_play(const uint8_t *gbk_text, uint16_t len)
{
    if (!gbk_text || len == 0 || len > DEV_RS232_VOICE_MAX_TEXT) return;
    // 组装 FD 00 [Len+2] 01 01 + GBK...
    // 经 RS232 通道发送至语音板
}

void dev_rs232_voice_volume(uint8_t level)
{
    // 组装音量设置帧并发送
}
```



### A.5.4 与协议层的边界


| 在哪做                             | 内容                                                    |
| ------------------------------- | ----------------------------------------------------- |
| `Device/Comm/dev_rs232_voice.c` | 语音板物理帧封装、RS232 发送、音量帧                                 |
| `app_qh_proto_voice.c`          | 礼貌用语索引表、费额金额→"您好请交费XX元"文本构造，再调 `dev_rs232_voice_play` |


---



## A.6 编译期配置

```makefile
PROTO ?= LDI
ifeq ($(PROTO),QH)
  SRC_APPLICATION += $(wildcard Application/Src/ProtocolParser_QingHai/*.c)
  SRC_DEVICE      += Device/Comm/dev_rs232_voice.c   # 新增 232_voice 模块
  CFLAGS += -DPROTO_QINGHAI
else ifeq ($(PROTO),CQ)
  SRC_APPLICATION += $(wildcard Application/Src/ProtocolParser_ChongQing/*.c)
  SRC_DEVICE      += Device/Comm/dev_rs232_voice.c   # CQ 亦复用
  CFLAGS += -DPROTO_CHONGQING
else
  SRC_APPLICATION += $(wildcard Application/Src/LDI/*.c)
endif
```

**与 LDI / CQ 编译期互斥**，运行时只执行一种协议。

---



## A.7 设备能力验收标准（Part A 范围）


| 验收项               | 标准                                                                               |
| ----------------- | -------------------------------------------------------------------------------- |
| 无 `dev_service` 层 | 全仓不存在 `Device/Service/dev_service.*` 与 `dev_service_*` 调用                        |
| HUB75/灯/光敏复用      | `app_render` / `dev_display_*` / `dev_io_ctrl` / `dev_light_sensor` 被 QH 协议层直接调用 |
| `232_voice` 新增    | `Device/Comm/dev_rs232_voice.c` 编译通过，`dev_rs232_voice_play/volume` 可被协议层调用       |
| LDI 不受影响          | `make PROTO=LDI -j8` 编译不变                                                        |
| CQ 不受影响           | `make PROTO=CQ -j8` 编译不变（共享 `232_voice`）                                         |
| 无循环依赖             | `Device/Comm/dev_rs232_voice.h` 不依赖任何协议头文件                                       |


---



## A.8 总结


| 项目                       | 结论                                                                                                                                 |
| ------------------------ | ---------------------------------------------------------------------------------------------------------------------------------- |
| 是否保留 `dev_service` 共享层   | **否** —— 已砍掉，QH 协议层直接复用 `Device/` 驱动                                                                                               |
| `Device/` 已有驱动可复用        | **14 项** —— HUB75/通信灯/报警灯/光敏/485/网口/亮度/RS232 等，零新增                                                                                 |
| 需新增的 `Device/` 模块        | **1 类** —— `232_voice`（TTS over RS232 语音板），仅此一项                                                                                    |
| 是否新增 `dev_voice` 之外的设备驱动 | **否** —— 其余硬件驱动均就绪                                                                                                                 |
| 协议层保留逻辑                  | 帧扫描、命令分发、固定格式显示(cmd `'6'`)、礼貌用语(cmd `'7'`)、费额语音(cmd `'B'`)、亮度/颜色映射、主机查询应答帧                                                         |
| **协议层内部架构**              | **Parse/Execute + Tagged Union**：解析器 `qh_parse_frame` 产出 `qh_parsed_cmd_t` 清单，执行器 `qh_execute_cmd` 消费清单调设备；同步、无额外队列（详见 Part B B.4） |
| **为何不用事件驱动**             | 现有 `app_dispatch` 已做异步分发，再套 event queue 成双重队列且破坏同步应答时序；QH 纯请求-响应，过度设计                                                              |
| Part A 工作量               | **极小** —— 新增 `232_voice` 一个模块 + 编译期切换验证                                                                                            |
| 重点                       | **复用** `Device/` **驱动；仅补** `232_voice`**；协议特有逻辑留在协议层；协议层内部按 Parse/Execute 拆分；全部工作量在 Part B（协议解析器）**                                |


