# Part B：ProtocolParser_ChongQing 接入

**状态**：`[计划中]`
**前置条件**：Part A 完成（`dev_service.*` / `dev_voice.*` / `dev_io_lane_light_PIC.*` 已就绪）
**协议版本**：重庆高速二代费显协议 2.0.1
**参考裸机工程**：`/home/yystation/Desktop/9K10810A80`
**协议原文**：`/home/yystation/Desktop/9K10810A80/06协议/重庆高速二代费显协议2.0.1.txt`

> CQ 协议解析器通过 `dev_service.*` / `dev_voice.*` / `dev_io_lane_light_PIC.*` 调用设备能力，不直接碰底层硬件。

---

## B.1 背景与定位

### B.1.1 现有协议栈

| 协议 | 目录 | 帧形态 | 默认通道/端口 |
|------|------|--------|---------------|
| IAP | `Application/Src/IAP/` | 二进制 `0x5A5A5A5A` | UDP/10011 |
| LDI | `Application/Src/LDI/` | 二进制 `0xFFFF` + CRC16-XMODEM | TCP + UDP/10011 |
| RLS | `Application/Src/RLS/` | 二进制 | （另绑） |
| AH_MQTT | `Application/Src/AH_MQTT/` | MQTT topic | （initcall 已注释） |
| **重庆（新建）** | `Application/Src/ProtocolParser_ChongQing/` | **JSON `{...}` + 固定 12B 二进制** | **UDP/20103（业务）+ UDP/10011（搜索）** |

分发框架：`app_dispatch` 按 `proto_probe` 竞争识别帧，协议模块通过 `sw_app_initcall` 自注册。

### B.1.2 与 LDI 的产品关系（已确认）

**编译期互斥**：重庆固件不含 LDI，LDI 固件不含重庆。编译切换通过 Makefile / `.eide` 的条件编译实现（如 `PROTO=CQ`）。

### B.1.3 与裸机工程的关系

移植策略：**行为对齐裸机联调结果**（含已知与协议文档的差异），API/分层对齐本工程。**通过 Part A 的 `dev_service_*` 调用所有设备能力**。

---

## B.2 目录与文件规划

与 LDI 同级；源/头分离。**文件名统一使用 `app_cq_proto_*` 样式**：

```
Application/Src/ProtocolParser_ChongQing/
├── app_cq_proto.c          # 模块 initcall、probe、任务、心跳定时
├── app_cq_proto_cmd.c      # JSON cmd 分发表 + 各命令处理
├── app_cq_proto_bin.c      # 二进制重启 / IP 搜索
├── app_cq_proto_device.c   # 设备能力桥接（转发 dev_service_* 调用）
└── app_cq_proto_cfg.c      # 重庆侧网络/端口等配置落盘

Application/Inc/ProtocolParser_ChongQing/
├── app_cq_proto.h
├── app_cq_proto_cmd.h
├── app_cq_proto_bin.h
├── app_cq_proto_device.h
└── app_cq_proto_cfg.h
```

命名规范：

| 项 | 规则 | 示例 |
|----|------|------|
| 目录名 | `ProtocolParser_ChongQing` | — |
| 文件名 | `app_cq_proto_*.c` | `app_cq_proto_cmd.c` |
| 函数前缀 | `cq_proto_` | `cq_proto_cmd_text1()` |
| 全局变量 | `g_cq_proto_` | `g_cq_proto_ctx` |
| 头文件守卫 | `APP_CQ_PROTO_*_H` | `APP_CQ_PROTO_CMD_H` |
| 静态变量 | `s_cq_` | `s_cq_sync_counter` |

### B.2.1 构建编入

| 文件 | 改动 |
|------|------|
| `Makefile` | `INC_DIRS` 增加 `Application/Inc/ProtocolParser_ChongQing/`；`SRC_APPLICATION` 条件追加 |
| `.eide/eide.yml` | 增加同组源文件（放在 CQ 条件组中） |
| ThirdParty | 引入 **cJSON**：`Middlewares/Third_Party/cJSON/` |

**编译期切换方案**：

```makefile
PROTO ?= LDI
ifeq ($(PROTO),CQ)
  SRC_APPLICATION += $(wildcard Application/Src/ProtocolParser_ChongQing/*.c)
  CFLAGS += -DPROTO_CHONGQING
else
  SRC_APPLICATION += $(wildcard Application/Src/LDI/*.c)
endif
```

---

## B.3 架构设计

### B.3.1 数据流

```
UDP 收包 (业务口 20103 / 搜索口 10011)
  → app_channel_dispatch
  → ring_buffer (id=1, RB_GROUP_PROTO)
  → frame_dispatch_task
  → cq_proto_probe_frame
       ├─ '{' → 花括号深度定界 → READY (JSON 帧)
       └─ FF FF + 固定 12B 特征匹配 → READY (二进制帧)
  → g_cq_proto_frame_queue
  → cq_proto_handle_task
       ├─ JSON → cJSON_Parse → "cmd" 查表 → g_cq_proto_cmd_table[]
       └─ BIN  → cq_proto_bin_reboot / cq_proto_bin_ip_search
  → dev_service_*()（通过 Part A 统一服务层调用设备能力）
```

### B.3.2 UDP 双端口模型（已确认）

**业务端口可配置，默认 20103；搜索端口固定 10011。**

```
CH_ID_UDP     (index=4) → 端口 10011 → 承载搜索应答（广播）+ IAP
CH_ID_UDP_CQ  (index=7) → 端口 20103 → 承载 JSON 业务 + 重启请求
```

具体改动：

1. `app_dispatch.h`：`channel_id_t` 枚举新增 `CH_ID_UDP_CQ = 7`，`CH_ID_MAX = 8`
2. `app_udp.c`：新增 `udp_cq_task` / `udp_cq_connect_task`，绑定端口 20103
3. CQ 模块 init：`app_proto_bind_channel(s_cq_mask, CH_ID_UDP_CQ)` 绑业务口
4. 搜索应答发送：通过 `CH_ID_UDP`（10011）广播
5. 业务响应发送：通过 `CH_ID_UDP_CQ`（20103）单播回源

### B.3.3 心跳

```
cq_proto_timer_task (1s)
  → 若 app_factory_test 占用显示 → 跳过计数
  → s_cq_sync_counter++
  → ≥120 → dev_service_show_fault()
任意合法 JSON cmd 处理成功 → s_cq_sync_counter = 0
```

---

## B.4 Probe 与帧定界

### B.4.1 JSON 帧

- 首字节 `{`
- probe 阶段做花括号深度扫描（`{` +1, `}` -1，跳过字符串内的 `}`）
- 深度归零 → `PROTO_PROBE_READY`；深度超过 16 或总长超过 `FRAME_DATA_MAX_LEN`(1044) → `PROTO_PROBE_FAKE`

### B.4.2 二进制帧（精确匹配 12 字节）

重启请求：`FF FF 00 00 00 00 00 02 07 31 5B C9`
重启应答：`FF FF 00 00 00 00 00 02 A7 31 F4 36` → 延时 100ms → `dev_service_reset(100)`

搜索请求：`FF FF 00 00 00 00 00 02 05 00 48 73`
搜索应答：`dev_service_send_search_response()`

CRC：多项式 `0x8408`，初值 `0xFFFF`，结果取反 —— 与 `crc16_xmodem()` 一致。

### B.4.3 混收处理

同 RB 内 JSON 与二进制帧可能交错。probe 按字节尝试：
1. `{` → JSON 路径
2. `FF FF` + 后续 10 字节精确匹配 → BIN 路径
3. 否则 `FAKE` 跳过 1 字节

---

## B.5 JSON 命令实现矩阵

### B.5.1 命令表

| cmd | 处理函数 | 行为要点 | 设备层映射（通过 dev_service） | 一期 |
|-----|----------|----------|-------------------------------|------|
| `text1` | `cq_proto_cmd_text1` | `scr==0` 清屏；`ln0`~`ln7` 的 `s/c/t`；字号 16/24/32 | `dev_service_show_text` / `dev_service_clear_screen` | **必做** |
| `tra1` | `cq_proto_cmd_tra1` | `turn`=r/g → 右下角 32x32 红叉/绿箭 | `dev_service_lane_light` → `dev_io_lane_light_PIC` | **必做** |
| `pic1` | `cq_proto_cmd_pic1` | 空实现（stub），返回成功 | — | **stub** |
| `light1` | `cq_proto_cmd_light1` | `level` 0~15 → 亮度映射 | `dev_service_set_brightness` | **必做** |
| `voice_control1` | `cq_proto_cmd_voice_vol` | `level` → TTS 音量标签 | `dev_service_voice_volume` → `dev_voice_set_volume` | **必做** |
| `voice1` | `cq_proto_cmd_voice` | `t` 文本；`|` 组合 | `dev_service_voice_play` → `dev_voice_play` | **必做** |
| `warn1` | `cq_proto_cmd_warn` | `second`：0 关 / -1 常开 / >0 倒计时；**注意：协议含 `level` 参数（音量），裸机未处理，本实现同样忽略** | `dev_service_yellow_flash` | **必做** |
| `syn1` | `cq_proto_cmd_syn` | 心跳，清计数器 | 无 | **必做** |
| `setip` | `cq_proto_cmd_setip` | `ip/mask/wg/port` 落盘复位 | `dev_service_set_net_info` | **必做** |
| `full` | `cq_proto_cmd_full` | `color` 0~3 满屏 | `dev_service_fullscreen` | **必做** |
| `screen` | 不做 | 单元板切换通过 @Device/Display 实现 | — | **不做** |
| `modify` | 不做 | 老格式，裸机未实现 | — | **不做** |

任意合法 JSON cmd 成功解析后 → `s_cq_sync_counter = 0`（心跳复位）。

### B.5.2 JSON 解析架构

**cJSON + 静态命令分发表 + 类型安全提取宏**：

```c
typedef void (*cq_proto_cmd_fn_t)(const cJSON *root, channel_t *ch);

typedef struct {
    const char        *cmd_name;
    cq_proto_cmd_fn_t  handler;
} cq_proto_cmd_entry_t;

static const cq_proto_cmd_entry_t g_cq_proto_cmd_table[] = {
    { "text1",           cq_proto_cmd_text1 },
    { "tra1",            cq_proto_cmd_tra1 },
    { "pic1",            cq_proto_cmd_pic1 },
    { "light1",          cq_proto_cmd_light1 },
    { "voice_control1",  cq_proto_cmd_voice_vol },
    { "voice1",          cq_proto_cmd_voice },
    { "warn1",           cq_proto_cmd_warn },
    { "syn1",            cq_proto_cmd_syn },
    { "setip",           cq_proto_cmd_setip },
    { "full",            cq_proto_cmd_full },
};
```

类型安全提取宏：

```c
#define CQ_JSON_GET_STR(root, key) \
    ({ cJSON *_item = cJSON_GetObjectItem(root, (key)); \
       (_item && cJSON_IsString(_item)) ? _item->valuestring : NULL; })

#define CQ_JSON_GET_INT(root, key, default_val) \
    ({ cJSON *_item = cJSON_GetObjectItem(root, (key)); \
       (_item && cJSON_IsNumber(_item)) ? _item->valueint : (default_val); })

#define CQ_JSON_GET_OBJ(root, key) \
    ({ cJSON *_item = cJSON_GetObjectItem(root, (key)); \
       (_item && cJSON_IsObject(_item)) ? _item : NULL; })
```

---

## B.6 设备层桥接

`app_cq_proto_device.c` 的职责是**将 CQ 协议语义转发到 `dev_service_*`**，而非直接调用底层设备驱动。

```c
void cq_proto_cmd_tra1(const cJSON *root, channel_t *ch)
{
    const char *turn = CQ_JSON_GET_STR(root, "turn");
    if (!turn) return;
    bool green = (turn[0] == 'g');
    dev_service_lane_light(green);
}

void cq_proto_cmd_full(const cJSON *root, channel_t *ch)
{
    int color = CQ_JSON_GET_INT(root, "color", 0);
    dev_service_fullscreen((uint8_t)color);
}
```

**原则**：CQ 协议层**不直接碰** `app_render` / `dev_io_lane_light` / `pl_net_set_ip` 等底层接口，全部通过 `dev_service_*` 调用。

---

## B.7 配置与默认值

| 项 | 默认值 | 落点 |
|----|--------|------|
| IP | 192.168.1.5 | `app_cq_proto_cfg` / IAP NetConfig |
| Mask | 255.255.255.0 | 同上 |
| Gateway | **192.168.1.1** | 同上（裸机 PowerOn 对齐） |
| 业务端口 | **20103** | `app_cq_proto_cfg`（可通过 `setip` 修改） |
| 搜索端口 | **10011** | 固定，`CH_ID_UDP` |

配置存储：复用 `dev_flash_iap` 的 NetConfig 字段（Sector1）。

重启二进制写 IAP 标记：对接本工程 IAP 状态机（`app_iap_cfg.h` 中的 `g_config->iap_flag`），**禁止照搬裸机 Sector11 写法**。

---

## B.8 已确认问题汇总

| 问题 | 确认结果 |
|------|----------|
| Q1. 与 LDI 的产品关系 | **编译期互斥**，运行时只执行一种 |
| Q2. UDP 端口 | **双端口**：业务 20103（可配置）+ 搜索 10011 |
| Q3. 语音 | **RS232 串口**，通过 `dev_voice.*` |
| Q4. pic1 | **空实现**（stub） |
| Q5. screen | **不做**，通过 @Device/Display 处理 |
| Q6. 通行灯 | **显示器图片模式**：通过 `dev_io_lane_light_PIC()` |
| Q7. setip / 重启配置 | 复用 `dev_flash_iap` NetConfig |
| Q8. 默认网关 | **192.168.1.1** |
| Q9. 字库芯片 | **显示层实现** |
| Q10. JSON 库 | **cJSON** |
| Q11. 上电欢迎语 | **不在协议层实现** |
| Q12. 文件名 | **`app_cq_proto_*` 样式** |
| Q13. TTS 串口 | **`CH_ID_RS232`**（USART3） |
| Q14. 业务端口可配性 | **支持** |
| Q15. 搜索应答端口 | **从 10011 广播** |
| Q16. warn1 -1 | **常开不自动停** |
| Q17. 工厂测试与心跳 | **工厂测试期间暂停心跳计数** |
| Q18. RS485 串口转发 | **不迁移**。裸机有 `needTxToSerial` 级联功能（拨码开关控制），STD 工程未使用，不纳入本次实现。如后续需要可通过 `dev_service` 新增接口 |
| Q19. screen 命令 | **空实现（stub）**。裸机 `cmd_BoardSet` 会写 Flash 切换单元板扫描，STD 工程的模组适配已在 `dev_display_1_260.c` / `dev_display_1_577.c` 中实现。收到 `screen` 命令时解析并忽略，不影响通信 |
| Q20. text1 行号限制 | **裸机限制：ln4 仅支持字号 ≤24，ln5~7 仅支持字号 ≤16**。`dev_service_show_text` 内部应做限制，字号超出时跳过该行 |
| Q21. full 颜色=3 | **协议文档说"白色"，裸机实现 `WHITE`**。STD 工程红绿双基色显示器无法物理显示纯白，映射为红绿全亮（视觉近似白） |

---

## B.9 风险与约束

1. **JSON 粘包/半包**：UDP 通常一包一帧，但仍应按流式 probe 写。
2. **花括号定界**：文本字段含 `}` 时裸机也脆弱；可记录为已知限制。
3. **亮度档位**：协议 0~15 vs 硬件 8 级，必须固定映射表。
4. **内存**：cJSON 堆分配；FreeRTOS heap 32KB，限制帧长 ≤ 1044。
5. **编码**：源文件中文故障串必须按 GB2312 字节写入或用 `\x` 转义。
6. **TTS 串口波特率**：需与语音控制板硬件规格对齐。
7. **text1 行号限制**：ln4 仅支持字号 ≤24，ln5~7 仅支持字号 ≤16；`dev_service_show_text` 内部需做限制，字号超出时跳过该行。
8. **full 颜色=3**：协议文档说"白色"，裸机 `WHITE`。STD 工程红绿双基色显示器无法物理显示纯白，映射为红绿全亮（视觉近似白）。

---

## B.10 实施阶段与验收

### 阶段 1 — 骨架

- 目录 + cJSON 引入 + Makefile 条件编译
- `app_cq_proto.c`：initcall、probe、handle_task、timer_task
- 双 UDP 通道（`CH_ID_UDP_CQ` + `CH_ID_UDP`）
- 空命令表可编译

**验收**：`make PROTO=CQ -j8` 通过。

### 阶段 2 — 显示类 cmd + 心跳故障

- `text1` / `tra1` / `full` / `light1` / `syn1`
- 心跳 120s 故障屏
- `warn1` 黄闪倒计时

**验收**：上位机按协议发包，屏显与裸机对照。

### 阶段 3 — 网络类 + 语音

- `setip` 落盘复位
- bin 重启应答 + 复位
- bin IP 搜索广播应答
- `voice1` / `voice_control1`

**验收**：搜索工具能发现；改 IP 重启后生效。

### 阶段 4 — 回归与收尾

- `pic1` stub / `screen` 忽略
- 与 IAP（10011）互不误伤
- 亮度 / 光敏 / 工厂测试互斥
- 文档归档
