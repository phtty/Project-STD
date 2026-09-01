# Part A：STD 设备层架构优化

**状态**：`[已废弃]`
**废弃原因（用户决策）**：跳过统一服务层（`dev_service_*`），CQ 协议模块按
`doc/08_协议模块接入规则` 惯例**直调** STD 既有设备接口（`app_render` /
`dev_display` / `dev_io_ctrl` / `dev_rs232_voice` / `app_board_net_cfg` /
`pl_net`），不再新建 `dev_service.*` / `dev_voice.*` / `dev_io_lane_light_PIC.*`
中间层。本文保留存档，仅作历史设计推演记录，与已落地实现（Part B）无关。

---

## A.1 统一业务服务层（dev_service）

### A.1.1 设计动机

当前设备层已有零散的底层抽象（`app_render` / `dev_io_lane_light` / `dev_display_set_brightness` / `pl_net_set_ip` 等），但存在两个问题：

1. **协议层仍需直接调用多个分散接口**，并自行编排逻辑（如"全屏亮红→等待→复位"），同一套业务逻辑在 LDI 和 CQ 中重复实现
2. **缺少协议无关的业务能力封装**，如 IP 搜索应答（25 字节帧 + CRC + 广播）、故障屏显示、IP 修改+落盘+复位等组合操作

**结论：在 @Device 层新增统一业务服务层，将"协议无关的输出基础功能"统一封装。**

### A.1.2 分层架构

```
┌─────────────────────────────────────────────────┐
│  Protocol Layer (LDI / CQ / RLS / 未来新协议)      │
│  只做：帧解析、命令分发、状态管理                      │
│  调用：dev_service_*() 高层接口                      │
├─────────────────────────────────────────────────┤
│  Service Layer (dev_service.c / dev_service.h)   │  ← 新增
│  协议无关的业务能力封装                              │
│  内部调用底层设备驱动                                │
├─────────────────────────────────────────────────┤
│  Device Layer                                      │
│  app_render / dev_display / dev_io_ctrl /         │
│  dev_voice / pl_net / pl_system                    │
└─────────────────────────────────────────────────┘
```

### A.1.3 新增文件

```
Application/Src/Device/
└── dev_service.c           # 业务服务层实现

Application/Inc/Device/
└── dev_service.h           # 业务服务层接口
```

### A.1.4 对外接口清单

```c
/* ==================== 显示服务 ==================== */

/** 全屏填充颜色（协议无关）
 *  @param color  颜色索引：0=红, 1=绿, 2=黄, 3=灭/黑
 */
void dev_service_fullscreen(uint8_t color);

/** 显示故障屏：红色文字 "车道关闭" / "请择道行驶!" + 右下角红叉 */
void dev_service_show_fault(void);

/** 清屏 */
void dev_service_clear_screen(void);

/** 显示文字（指定行、字号、颜色、GBK文本） */
void dev_service_show_text(uint8_t line, uint8_t font_size,
                           uint8_t color, const uint8_t *gbk_text);

/** 显示通行灯图标（右下角 32x32 红叉或绿箭） */
void dev_service_show_lane_icon(bool green);

/* ==================== 通行灯服务 ==================== */

/** 设置通行灯
 *  编译期选择后端：
 *  - PROTO_CHONGQING: 调用 dev_io_lane_light_PIC()（显示器图片）
 *  - 其他协议: 调用 dev_io_lane_light()（GPIO 硬件）
 *  @param green  true=绿灯/绿箭, false=红灯/红叉
 */
void dev_service_lane_light(bool green);

/** 黄闪报警控制
 *  @param enable  true=开启, false=关闭
 *  @param seconds 持续时间（秒），0=直到手动关闭
 */
void dev_service_yellow_flash(bool enable, uint16_t seconds);

/* ==================== 亮度服务 ==================== */

/** 设置屏幕亮度
 *  @param level  协议值 0~15
 *    - level == 0：自动亮度模式（启用光敏传感器）
 *    - level 1~15：手动亮度，内部映射公式 `(level / 2) + 1`，上限 8
 */
void dev_service_set_brightness(uint8_t level);

/* ==================== 网络服务 ==================== */

/** 获取本机 IP/Mask/Gateway/Port 信息 */
void dev_service_get_net_info(uint8_t *ip, uint8_t *mask,
                               uint8_t *gw, uint16_t *port);

/** 设置本机 IP/Mask/Gateway/Port 并落盘 */
void dev_service_set_net_info(const uint8_t *ip, const uint8_t *mask,
                               const uint8_t *gw, uint16_t port,
                               bool save_and_reset);

/** IP 搜索应答：构造 25 字节帧 + CRC + 10011 广播 */
void dev_service_send_search_response(void);

/* ==================== 语音服务 ==================== */

/** TTS 合成播放（GBK 文本） */
void dev_service_voice_play(const uint8_t *gbk_text, uint16_t len);

/** TTS 设置音量（0~15） */
void dev_service_voice_volume(uint8_t level);

/* ==================== 系统服务 ==================== */

/** 系统复位（延时指定毫秒后复位） */
void dev_service_reset(uint32_t delay_ms);
```

### A.1.5 服务层 vs 设备层的职责边界

| 层级 | 职责 | 示例 |
|------|------|------|
| **Service Layer** | 业务逻辑组合：帧构造 + 多步骤编排 | IP 搜索应答（查询 IP → 构造帧 → CRC → 广播）、故障屏（显示红字 + 红叉 + 亮红灯） |
| **Device Layer** | 单一硬件操作：一个接口做一件事 | `dev_io_lane_light(true)`、`dev_io_lane_light_PIC(true)`、`app_render_fill(RED)`、`crc16_xmodem(buf, len)` |

**原则**：Service Layer 可以组合多个 Device Layer 调用，Device Layer 不感知业务含义。

### A.1.6 典型调用对比

**IP 搜索应答**：

| 方式 | 协议层代码 |
|------|-----------|
| 旧（直调设备层） | `pl_net_get_ip(buf); buf[8]=0xA5; memcpy(buf+9,ip,4); crc=crc16_xmodem(buf+2,21); channel_send(CH_ID_UDP, buf, 25);` |
| 新（调服务层） | `dev_service_send_search_response();` |

**全屏亮红 + 故障文字**：

| 方式 | 协议层代码 |
|------|-----------|
| 旧 | `app_render_fill(RED); app_render_text(0, FONT_24, WHITE, "车道关闭"); app_render_text(1, FONT_16, RED, "请择道行驶!"); app_render_bitmap(BMP_RED_X, ...);` |
| 新 | `dev_service_show_fault();` |

**IP 修改 + 落盘复位**：

| 方式 | 协议层代码 |
|------|-----------|
| 旧 | `pl_net_set_ip(ip); pl_net_set_mask(mask); pl_net_set_gw(gw); dev_flash_iap_save(); pl_system_reset();` |
| 新 | `dev_service_set_net_info(ip, mask, gw, port, true);` |

### A.1.7 所有协议共享的服务能力

| 服务接口 | 重庆 CQ | LDI | 未来新协议 | 说明 |
|----------|---------|-----|-----------|------|
| `dev_service_fullscreen` | full 命令 | 上电/故障 | 可用 | 全屏颜色 |
| `dev_service_show_fault` | 心跳超时 | 心跳超时 | 可用 | 故障屏 |
| `dev_service_show_text` | text1 命令 | 文字显示 | 可用 | 文字显示 |
| `dev_service_lane_light` | tra1 命令 | 通行灯 | 可用 | 编译期选后端 |
| `dev_service_yellow_flash` | warn1 命令 | 黄闪 | 可用 | 黄闪报警 |
| `dev_service_set_brightness` | light1 命令 | 亮度调节 | 可用 | 亮度 |
| `dev_service_send_search_response` | bin 搜索 | 搜索 | 可用 | IP 搜索应答 |
| `dev_service_set_net_info` | setip 命令 | 改 IP | 可用 | IP 修改+落盘+复位 |
| `dev_service_voice_play` | voice1 命令 | — | 可用 | TTS 播报 |
| `dev_service_voice_volume` | voice_control1 | — | 可用 | TTS 音量 |
| `dev_service_reset` | 重启应答 | — | 可用 | 系统复位 |

---

## A.2 语音 TTS 通用封装（dev_voice）

### A.2.1 背景

语音 TTS 通过 RS232 串口（`CH_ID_RS232`，USART3）与语音控制板通信。TTS 功能**下沉到 @Device 层**统一封装，使任何协议（LDI / CQ / 未来新协议）都可复用。

### A.2.2 新增文件

```
Application/Src/Device/
└── dev_voice.c              # TTS 封装（帧组装 + 串口发送 + 组合索引查表）

Application/Inc/Device/
└── dev_voice.h              # 对外接口
```

### A.2.3 对外接口设计

```c
/**
 * dev_voice.h - TTS 语音输出抽象层
 */

/* 直接文本模式：GBK 文本直接发送给 TTS 引擎合成 */
void dev_voice_play(const uint8_t *gbk_text, uint16_t len);

/* 固定索引模式：将已知短语映射为 4 位数字索引，支持 | 分隔组合 */
void dev_voice_combination(const uint8_t *gbk_text, uint16_t len);

/* 音量设置：level 0~15 映射到 TTS 音量标签 */
void dev_voice_set_volume(uint8_t level);

/* 静音/取消静音 */
void dev_voice_mute(bool enable);
```

### A.2.4 TTS 帧格式（对齐裸机）

**直接文本帧**（`dev_voice_play`）：

```
FD | 00 | [Len+2] | 01 | 01 | GBK text bytes...
│    │     │        │    │    └─ 文本内容
│    │     │        │    └─ 编码 01=GBK
│    │     │        └─ 命令码 01=合成播放
│    │     └─ 数据区长度低字节（文本长度 + 2）
│    └─ 数据区长度高字节（0x00）
└─ 帧头
```

**音量设置帧**（`dev_voice_set_volume`）：

```
5B | 76 | 30 | [31~39] | 5D    即字符串 [v01] ~ [v09]
```

裸机实现采用两步映射：先计算 `voceGrade = (level / 2) + 1`（上限 7），再查表得到 TTS 帧字节。
**注意：TTS 芯片音量标签 [v02] 和 [v07] 不可用，映射有跳档。**

| level (协议层) | voceGrade | TTS 音量标签 | 音量帧第 3 字节 |
|---------------|-----------|-------------|----------------|
| 0 ~ 1 | 1 | `[v01]` | `0x31` |
| 2 ~ 3 | 2 | `[v03]` | `0x33` |
| 4 ~ 5 | 3 | `[v04]` | `0x34` |
| 6 ~ 7 | 4 | `[v05]` | `0x35` |
| 8 ~ 9 | 5 | `[v06]` | `0x36` |
| 10 ~ 11 | 6 | `[v08]` | `0x38` |
| 12 ~ 15 | 7 | `[v09]` | `0x39` |

**实现时必须使用查表法，不可线性映射。**

### A.2.5 固定索引组合模式（`voiceCombination`）

裸机实现中，已知短语（地名、车型、费用词等）被映射为 4 位数字索引，多段用 `-` 分隔拼接后一次性发送。例如：

- "永川" → `5007`
- "成渝高速" → `4001`
- "通行费" → `7016`
- "一型车" → `8001`

**迁移策略**：
1. 将裸机 `voice.c` 中的索引映射表完整迁移到 `dev_voice.c`
2. `dev_voice_combination()` 内部实现文本 → 索引的查表逻辑
3. 数字/费用的特殊处理（"吨"、"元"、小数点）也一并迁入

---

## A.3 显示器通行灯驱动（dev_io_lane_light_PIC）

### A.3.1 背景

重庆二代费显的通行灯实现与其他费显**完全不同**：

| 类型 | 通行灯实现方式 | 驱动接口 |
|------|---------------|----------|
| LDI / 其他费显 | **IO 口驱动**通信灯控制板的 IO，控制车道灯板 | `dev_io_lane_light()` (GPIO PD14) |
| **重庆二代费显** | 在显示器**最后一行靠后位置**渲染 32x32 红叉/绿箭图片 | **`dev_io_lane_light_PIC()`** (display BITMAP) |

### A.3.2 新增文件

```
Application/Src/Device/
└── dev_io_lane_light_PIC.c    # 显示器图片式通行灯驱动

Application/Inc/Device/
└── dev_io_lane_light_PIC.h    # 对外接口
```

### A.3.3 接口与实现

```c
/**
 * dev_io_lane_light_PIC.h - 基于显示器图片的通行灯控制
 * 与 dev_io_lane_light()（GPIO 硬件）接口完全一致，可互换使用。
 */

/** 控制通行灯显示（显示器图片模式）
 *  @param green  true=绿箭, false=红叉
 */
void dev_io_lane_light_PIC(bool green);
```

```c
void dev_io_lane_light_PIC(bool green)
{
    if (green)
        app_render_bitmap(BMP_GREEN_ARROW, LANE_PIC_POS);
    else
        app_render_bitmap(BMP_RED_X, LANE_PIC_POS);
}
```

- **不触碰任何 GPIO 硬件**，完全通过 `app_render` BITMAP 实现
- 位置 `LANE_PIC_POS` 需对齐裸机实际渲染位置（显示器最后一行靠后）
- 位图资源 `BMP_GREEN_ARROW` / `BMP_RED_X` 需从裸机提取或重新制作

---

## A.4 Part A 验收标准

| 验收项 | 标准 |
|--------|------|
| `dev_service.c/h` 编译 | `make -j8` 通过（CQ 和 LDI 两种配置均通过） |
| `dev_voice.c/h` 编译 | 同上 |
| `dev_io_lane_light_PIC.c/h` 编译 | 同上 |
| LDI 不受影响 | LDI 固件编译和功能不变（新增文件，LDI 不调用即可） |
| 接口头文件无循环依赖 | `dev_service.h` 不依赖任何协议头文件 |
