# Part B：ProtocolParser_ChongQing 接入

**状态**：`[已落地]`（2026-08-20；Makefile `PROTO=CQ` 构建 + 全协议 dev 构建双通道验证）
**协议版本**：重庆高速二代费显协议 2.0.1
**架构决策**：Part A 服务层方案已废弃（用户决策），CQ 模块按 doc/08 惯例**直调** STD 设备接口。

> 已落地实现要点（与早期规划的差异，详见各节修订）：
> - 无 `dev_service.*` / `dev_voice.*` 服务层，执行层直调 `app_render` / `dev_display` /
>   `dev_io_ctrl` / `dev_rs232_voice` / `app_board_net_cfg` / `pl_net`；
> - 语音板走 USART6（`dev_rs232_voice` 旁路 TX），非 `CH_ID_RS232`（修正 Q13）；
> - 文件划分为 3+3：`app_cq_proto.c`（注册/probe/双任务/cJSON 钩子）、
>   `app_cq_proto_parse.c`（JSON/二进制纯解析）、`app_cq_proto_cmd.c`（命令执行/位图/故障屏）；
>   **上电画面不实现**（2026-08-21 撤销 `app_cq_proto_default.c`，使用系统默认显示「欢迎行驶\n高速公路」）；
> - Makefile `PROTO ?= ALL` / `PROTO=CQ`（剔除 LDI 目录 + `-DPROTO_CHONGQING`）；
> - 队列深度 3、payload 1044、msg 1052、静态体 3156B（**置 CCMRAM**，见 doc/06-04）；
> - **端口语义（2026-08-21 方案 B 定稿：TCP/UDP 端口彻底分离）**：Sector1.net_cfg
>   新增 `udp_port`（CQ UDP 业务口专有，setip 写入，默认 20103）；业务口
>   `PROTO_CHONGQING` 构建读 `net_cfg.udp_port`（有效即用，无效/0 回退 20103）；
>   dev 共存构建**固定 20103、不读该字段**（dev 构建无写入方）。`net_cfg.port`
>   恒为 **TCP 业务口**（LDI/IAP 生态，出厂 9528），setip 不再写入该字段（历史
>   bug：setip 写 port 曾令 CQ 错绑 9528、20103 无响应，见 B.11）；搜索应答端口
>   同样改报 `app_udp_cq_get_port()`；
> - **10011 口 12B 二进制帧（dev 全协议构建，2026-08-21 二次实态修正）**：CQ 12B
>   帧 len=00 00 00 02 且 CRC16-XMODEM 恰好通过 LDI 校验——LDI probe 对
>   `data_len==2` 显式 FAKE 放行，CQ 12B 帧仍由 CQ probe 认领（修复前 LDI 沿
>   身份校验路径 SKIP 吞掉 CQ 搜索/重启帧 → CQ 12B 在 10011 无响应；
>   原「CQ 帧 len 域全 0」表述与实态不符，见 B.9.9 / Q23 / B.11）。

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

与 LDI 同级；源/头分离。**文件名统一使用 `app_cq_proto_*` 样式**（已落地实态）：

```
Application/Src/ProtocolParser_ChongQing/
├── app_cq_proto.c          # 模块 initcall、probe、处理任务、1s 定时任务、
│                           #   cJSON 钩子初始化、PROTO_CHONGQING 网络配置应用
├── app_cq_proto_parse.c    # JSON/二进制帧纯解析 → cq_parsed_cmd_t
└── app_cq_proto_cmd.c      # 命令执行（渲染/IO/语音/网络/32×32 位图/故障屏）

Application/Inc/ProtocolParser_ChongQing/
├── app_cq_proto.h          # CQ_PAYLOAD_MAX=1044 / CQ_MSG_SIZE=1052 /
│                           #   CQ_QUEUE_DEPTH=3 / 命令枚举 / 12B 二进制帧常量
├── app_cq_proto_parse.h    # cq_parse_frame 声明 + cq_parsed_cmd_t
└── app_cq_proto_cmd.h      # cq_execute_cmd / cq_render_fault_screen 声明
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

**编译期切换方案（已落地实态）**：

```makefile
PROTO ?= ALL                          # 默认全协议 dev 构建（-DSTD_ALL_PROTO）
ifeq ($(PROTO),CQ)
  SRC_APPLICATION := $(filter-out Application/Src/LDI/%,$(SRC_APPLICATION))
  DEFINES += -DPROTO_CHONGQING        # CQ 口径：TCP 通道不启动，网络应用由 app_net_boot 承担
endif
```

- CQ 四个源文件**恒定编入** `SRC_APPLICATION`（位于 LDI 源之后：probe 注册序 =
  源码收录序，CQ 必须后于 LDI）；cJSON.c 恒定编入第三方源列表。
- `PROTO=CQ` 分支仅剔除 LDI 目录文件 + 追加 `-DPROTO_CHONGQING`；`{` 帧族
  （青海/山东/贵州/四川MTC）仍编入且保留 `-DSTD_ALL_PROTO` 豁免守卫。
- 量产 EIDE 互斥配置：新增 `Application/protocol/ProtocolParser_ChongQing` 文件夹与
  incList 条目已入 `.eide/eide.yml`（Debug 目标未排除 CQ）；LDI 互斥按既有
  excludeList 纪律操作（添加 `<virtual_root>/Application/protocol/ldi` 排除项）。

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
  → g_cq_queue（静态队列，置 CCMRAM）
  → cq_proto_handle_task
       ├─ JSON → cJSON_Parse（钩子=FreeRTOS 堆）→ "cmd" 查表 → g_cq_cmd_table[]
       └─ BIN  → cq_execute_cmd 内 _cq_exec_bin_reboot / _cq_exec_bin_search
  → 执行层直调 STD 设备接口（app_render / dev_display / dev_io_ctrl /
     dev_rs232_voice / app_board_net_cfg / pl_net / app_udp_broadcast）
```

### B.3.2 UDP 双端口模型（已确认）

**业务端口可配置，默认 20103；搜索端口固定 10011。**

```
CH_ID_UDP     (index=4) → 端口 10011 → 承载搜索应答（广播）+ IAP
CH_ID_UDP_CQ  (index=7) → 端口 20103 → 承载 JSON 业务 + 重启请求
```

具体改动：

1. `app_dispatch.h`：`channel_id_t` 枚举新增 `CH_ID_UDP_CQ = 7`，`CH_ID_MAX = 8`
2. `app_udp.c`：新增 `udp_cq_task` / `udp_cq_connect_task`，绑定业务口 20103。
   端口来源 `_udp_cq_read_port()`：`PROTO_CHONGQING` 读 Sector1 `net_cfg.udp_port`
   （CQ UDP 业务口专有字段，方案 B 2026-08-21；有效 1~65535 即用，无效/0 回退
   20103）；dev 共存构建固定 20103（不读 `net_cfg.udp_port`——dev 构建该字段无
   写入方，见 B.11 端口 bug 修复记录）
3. CQ 模块 init：`app_proto_bind_channel(s_cq_mask, CH_ID_UDP_CQ)` 绑业务口
4. 搜索应答发送：通过 `CH_ID_UDP`（10011）广播
5. 业务响应发送：通过 `CH_ID_UDP_CQ`（20103）单播回源

### B.3.3 心跳

```
cq_proto_timer_task (1s)
  → 若 app_factory_test 占用显示 → 跳过计数
  → s_cq_sync_counter++（跨任务读写，临界区保护，B.7.2）
  → ≥120 → cq_render_fault_screen()（渲染后清零、置 s_cq_fault_shown）
帧结构合法的 JSON（合法命令 / 命令未知 ERR_CMD / 参数非法 ERR_PARAM）到达
  → 若 s_cq_fault_shown：先 app_default_display_show() 恢复默认画面、再执行本帧命令
  → s_cq_sync_counter = 0（宽松心跳复位，B.7.2）
仅 CQ_PARSE_ERR_FRAME 与 BIN 帧（重启/搜索）不复位
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

帧结构合法的 JSON 成功解析后（含命令未知/参数非法两档宽松复位）→
`s_cq_sync_counter = 0`（心跳复位，见 B.7.2）；仅 `CQ_PARSE_ERR_FRAME` 与
二进制帧（重启/搜索）不复位。

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

## B.6 执行层（已落地：直调设备接口，无服务层）

Part A 的 `dev_service.*` 服务层已废弃。命令执行层（`app_cq_proto_cmd.c`）直接
调用 STD 既有设备接口（doc/08 惯例）：

```c
/* tra1：右下角 32×32 红叉/绿箭位图（先清区域再 RENDER_BITMAP） */
app_render(&(render_cfg_t){ .type = RENDER_FILL, .x = x, .y = y, .w = 32, .h = 32,
                            .color = COLOR_BLACK });
app_render(&(render_cfg_t){ .type = RENDER_BITMAP, .x = x, .y = y, .w = 32, .h = 32,
                            .color = green ? COLOR_GREEN : COLOR_RED, .bitmap = bmp });

/* full：0红/1绿/2黄/3白 全屏（3=白 → 红绿双基色映射红绿全亮即黄） */
app_render(&(render_cfg_t){ .type = RENDER_FILL, .x = 0, .y = 0, .w = 0, .h = 0,
                            .color = color });
```

| 协议语义 | 设备接口（直调） |
|----------|------------------|
| text1 行渲染 / full 全屏 / 故障屏 | `app_render` + `dev_display_commit_frame` |
| tra1 红叉/绿箭 | `app_render` RENDER_BITMAP（32×32 位图在 `app_cq_proto_cmd.c` 内，MSB-first 每行 4B） |
| light1 亮度 | 光敏任务挂起/恢复（`g_light_sensor_task_handle` + `osThreadSuspend/Resume`）+ `dev_display_set_brightness`（对齐贵州 '8' 命令方式） |
| voice1 / voice_control1 | `dev_rs232_voice_play` / `dev_rs232_voice_volume`（USART6 旁路 TX） |
| warn1 黄闪 | `dev_io_flash_light`（倒计时在 `cq_proto_timer_task` 每秒递减） |
| setip 落盘 | `app_board_net_cfg_update`（Sector1 net_cfg）→ `NVIC_SystemReset()` |
| 二进制重启应答 | `channel_send` 回源通道（UDP_CQ 单播） |
| 二进制搜索应答 | `app_udp_broadcast`（10011 搜索口广播 25B） |
| CQ 构建网络启动（PROTO_CHONGQING） | `app_net_boot_apply`（中立模块统一读 Sector1 → netif，空/损坏写 CQ 默认落盘，2026-08-21 解耦） |

---

## B.7 配置与默认值

| 项 | 默认值 | 落点 |
|----|--------|------|
| IP | 192.168.1.5 | Sector1 net_cfg（`app_board_net_cfg`） |
| Mask | 255.255.255.0 | 同上 |
| Gateway | **192.168.1.1** | 同上 |
| 业务端口 | **20103** | `PROTO_CHONGQING`：Sector1 net_cfg.udp_port（可通过 `setip` 修改，方案 B 2026-08-21）；dev 共存构建：固定 20103，不读 net_cfg.udp_port（dev 构建无写入方） |
| 搜索端口 | **10011** | 固定，`CH_ID_UDP` |

配置存储：复用 `app_board_net_cfg` 的 net_cfg 字段（Sector1）。网络配置应用自
2026-08-21 起由中立模块 `app_net_boot`（`app_boot.c` init_task 调
`app_net_boot_apply`）统一承担，LDI `ldi_ctx_init` 与 CQ `cq_proto_init` 均不再
直接改 netif：读 Sector1 有效 → `pl_net_set_ip` + `app_tcp_server_set_port` 应用
（**TCP Server 口两口径统一应用 net_cfg.port，2026-08-24 修订**——TCP Server/
Client 通道两口径均启动，见下）；空/损坏/非法 → 写回本构建默认记录并应用
（PROTO_CHONGQING 口径 192.168.1.5/20103；dev 口径 192.168.114.200/9528）。

重启二进制：**软复位**（`NVIC_SystemReset`，应答后延时 100ms），**禁止照搬裸机
Sector11 写法**（STD 无该机制）。

### B.7.1 setip 双构建生效语义与测试方法（2026-08-21 定稿，2026-08-21 方案 B 修订）

setip 命令链（静态审计 2026-08-21 无断点）：`_cq_parse_frame`（parse 层校验
ip/mask/wg 点分字符串 + port JSON 数字 1~65535，非法帧按「JSON 无应答帧」约定
静默丢弃）→ `_cq_exec_setip` → `app_board_net_cfg_update(ip,mask,gw,port,udp_port)`
落盘 Sector1（同步 HAL 轮询擦写，同值跳过，升级中间态仅更新 net_cfg 不拒绝）→
`NVIC_SystemReset()` 重启生效。**方案 B（2026-08-21，TCP/UDP 端口彻底分离）**：
Sector1.net_cfg 新增 `udp_port` 字段（CQ UDP 业务口专有），setip 命令端口写入
`udp_port`、TCP 业务口 `port` **保留现值**（get 失败用出厂默认 9528）——setip
不再污染 TCP 口。**port 字段必须是 JSON 数字**：协议 2.0.1 原文
`{"cmd":"setip",...,"port":20103}`、裸机 `cmd_ip_ctrl`（`tempjson->valueint`）、
官方测试软件（`","port":` 无引号）三者均为数字，port 发成字符串 `"20103"` 会被
parse 拒绝（与裸机行为一致，不兼容字符串）。

| 构建 | setip 后重启生效语义 | 验证方法 |
|------|---------------------|----------|
| dev 共存（EIDE Debug / `make` 默认，LDI 编入） | **IP 生效**：`app_net_boot_apply` 读 Sector1 net_cfg（magic+CRC 有效、IP 非 0、port 1~65535）→ `pl_net_set_ip` + `app_tcp_server_set_port`（`ldi_ctx_init` 分支 1 只负责装载 cfg 与 W25 自愈，2026-08-21 起不再应用 netif；其分支判定中 `update_sta==updated` 不参与——升级中间态记录同样采纳 Sector1.net_cfg，防 W25 陈旧镜像回写覆盖 setip 刚写入的 net_cfg）；**端口均不生效**——setip 端口写 Sector1 `udp_port` 但 dev 构建 CQ 业务口固定 20103（`_udp_cq_read_port` `#else` 分支，防历史污染回归，勿改）；**TCP 口（port）不被 setip 改动**（保留现值），LDI 12H/1DH 仍报 TCP 口 | 改 IP 后重启，用新 IP 向 **20103** 发 text1 验证；端口修改须在 PROTO_CHONGQING 构建验证 |
| CQ 量产（`make PROTO=CQ` / EIDE 配置 `PROTO_CHONGQING` 目标，LDI 剔除） | **IP + UDP 端口均生效**：重启后 `app_net_boot_apply` 读 Sector1 → `pl_net_set_ip` + `app_tcp_server_set_port(net_cfg.port)`（**TCP 口两口径统一应用，2026-08-24 修订**——TCP Server 通道仍启动、绑定 net_cfg.port；CQ setip 只写 udp_port、port 保留现值，故 TCP 口不受 setip 影响）；`udp_cq_task` 每轮重建前 `_udp_cq_read_port` 读 Sector1 net_cfg.udp_port（有效 1~65535 即用，无效回退 20103） | 改 IP+端口后重启，用新 IP 向新端口发 text1 / 12B 搜索帧验证；搜索应答 port 字段报 `app_udp_cq_get_port()` |

> **不变量（2026-08-21 方案 B 更新，2026-08-24 端口应用两口径化修订）**：网络配置应用自 2026-08-21 起由中立模块
> `app_net_boot` 统一兜底（两口径都编入、均在 `app_board_net_cfg_fw_version_update`
> 之后执行）——「排除 LDI 而不加宏 = 无人应用网络配置」的中间态不再致命：空/损坏
> 扇区由 `app_net_boot` 写本构建默认记录落盘并应用，设备不会停留在编译期默认 IP。
> 但 **PROTO_CHONGQING 仍决定默认口径与 UDP 端口应用对象**：不定义该宏即按 dev 口径
> （192.168.114.200，TCP 口 9528），定义该宏才按 CQ 口径
> （192.168.1.5，UDP 业务口读 Sector1 udp_port）——**TCP 口（net_cfg.port）应用
> 两口径统一（2026-08-24）**：TCP Server 通道两口径均启动（app_boot.c 无口径
> 裁剪），均绑定 Sector1 net_cfg.port，与 LDI 12H / IAP 0x01 上报恒一致。排除 LDI 与
> 定义 `PROTO_CHONGQING` 仍需成对出现，否则口径错配（2026-08-24 实测：
> EIDE Debug 曾为「LDI 编入 + PROTO_CHONGQING」混合口径，修复前 TCP 口恒 9528
> 与搜索上报矛盾——代码已按「端口应用不随宏裁剪」兜底，混合口径不再导致
> TCP 口错配，但默认 IP 口径仍由宏决定，成对纪律保留）。两口径默认记录均含
> port=9528 / udp_port=20103（方案 B：TCP/UDP 双字段落盘完整记录）。EIDE Debug
> 目标口径（excludeList 与 defineList 是否成对）须在改动前核对：
> excludeList 含 `Application/protocol/ldi` 时 defineList 须含 `PROTO_CHONGQING`
> （CQ 量产口径）；反之不定义（dev 共存口径）。

边界语义（已审计）：写盘完成到复位之间无需延时（内部 Flash 擦写为 HAL 轮询同步，
同裸机 30ms 延时仅为 W25 SPI 写）；`_cq_exec_setip` 忽略落盘返回值、无条件复位
（写失败时重启后回旧配置，与裸机无条件复位行为一致）；IWDG（256 预分频/4095
重载 ≈32.8s 窗口）远大于扇区擦除 1~2s 停顿，无复位竞争风险。升级中间态
（`update_sta != updated`）记录：net_cfg 仍写入但重启后 Bootloader 条件 D 判中间
态进 Recovery（任何重启均如此，非 setip 特有）；主固件 `ldi_ctx_init` 侧 2026-08-21
起对中间态记录同样采纳 Sector1.net_cfg（分支 1 条件不再要求 `update_sta==updated`），
不再被 W25 陈旧镜像回写覆盖。

**setip 链路 RTT 观察点（2026-08-21 撤销）**：本会话调试期曾补
`[cq] rx JSON/BIN`、`[cq] setip parsed/exec/update sector1 ret/reset now`、
`[netcfg] ldi_ctx_init/branch1/2/3/branch2 write-back/boot-apply`、
`[cq] udp_cq port` 等 RTT 标签，2026-08-21 已按「代码-文档同步」全部删除
（setip 排障改用静态审计 + 双构建验证，见 B.7.1 验证方法列）。仅
`app_board_net_cfg.c` 的 `[fwver]` 日志保留（doc/CLAUDE.md 0x03 判断路径依赖）。

### B.7.2 心跳宽松语义 / 故障屏恢复 / 临界区（2026-08-21 修订）

**宽松复位语义**（`cq_proto_handle_task`）：帧结构合法的 JSON 即视为上位机
存活——`CQ_PARSE_OK` 执行后复位；`CQ_PARSE_ERR_CMD`（命令未知）与
`CQ_PARSE_ERR_PARAM`（参数非法）不执行但**亦复位**（只不计入超时）；
仅 `CQ_PARSE_ERR_FRAME`（非 `{` / 非 12B 二进制 / 截断 / JSON 语法错）静默
丢弃且继续计数。BIN 帧（重启/搜索）`sta == CQ_PARSE_OK` 但 `cmd` 为
`CQ_PCMD_BIN_REBOOT/BIN_SEARCH`——**保持不复位**（重启帧自身复位设备，搜索帧
非业务心跳）。

**故障屏恢复默认画面**：`cq_proto_timer_task` 渲染故障屏时置
`s_cq_fault_shown = true`。`cq_proto_handle_task` 在「帧到达且被判定为上位机
存活信息」的两个 `cq_proto_sync_reset()` 调用点之前检查该标志：置位则先
`app_default_display_show()` 恢复默认画面（有注册回调调回调；无注册回调
清屏后渲染系统默认欢迎画面「欢迎行驶\n高速公路」——2026-08-21 重庆已取消
专属上电画面，恢复落到系统默认显示）再清标志。执行顺序
**先恢复、再执行本帧命令**——text1/full 等随后覆盖渲染，同帧同步提交
（`app_render` 内 commit）不闪烁。

**共享计数器临界区**：`s_cq_sync_counter` / `s_cq_warn_seconds` /
`s_cq_fault_shown` 跨 timer_task/handle_task 共享。M4 对齐 u32 单指令原子，但
读写-判断序列用 `taskENTER_CRITICAL()/taskEXIT_CRITICAL()` 保护：timer_task 的
递增（心跳）/递减（warn 倒计时）与 `cq_proto_sync_reset()` /
`cq_proto_warn_set()` 的写入在临界区内；读取侧阈值判断在临界区外（值本身
volatile）。标志位 `s_cq_fault_shown` 仅单指令置/清，不加临界区（最坏重复
渲染一次默认画面，无害）。

**心跳超时故障屏编译开关（2026-08-21 新增；2026-08-24 改名单层宏）**：宏
`CQ_FAULT_SCREEN`（1 开 / 0 关，`app_cq_proto.h`）控制 `cq_proto_timer_task` 的
「心跳计数 + 120s 阈值判定 + `cq_render_fault_screen`」段——**默认跟随构建
口径**：`PROTO_CHONGQING`（重庆量产）开启；共存/其他构建（通用版多协议固件）
**关闭**（他省上位机不发重庆 syn1 心跳，120s 超时故障屏会误触发）。可显式
覆盖：`-DCQ_FAULT_SCREEN=1` 强制开 / `=0` 强制关（Makefile 命令行 `DEFINES` /
EIDE defineList）。关闭时心跳不计数、不渲染（`s_cq_sync_counter` 恒 0、
`s_cq_fault_shown` 恒 false，`cq_proto_sync_reset()` 清零无副作用）；
**warn1 倒计时不受开关影响**（保持现有行为）。旧名 `CQ_FAULT_SCREEN_ENABLED`
/ `CQ_FAULT_SCREEN_FORCE_ON` 已废弃（不保留兼容别名），开关全集见
`doc/构建开关总表.md`。

---

## B.8 已确认问题汇总

| 问题 | 确认结果（已落地修订） |
|------|----------|
| Q1. 与 LDI 的产品关系 | **编译期互斥**：`make PROTO=CQ` 剔除 LDI 目录；EIDE 按 excludeList 目录排除纪律互斥 |
| Q2. UDP 端口 | **双端口**：业务 20103（可配置，`CH_ID_UDP_CQ`）+ 搜索 10011（`CH_ID_UDP`） |
| Q3. 语音 | **USART6 语音板 TTS**：`dev_rs232_voice_play/volume` 旁路 TX（**修正早期 CH_ID_RS232 结论**） |
| Q4. pic1 / voice_play1 | **空实现**（stub，解析后忽略） |
| Q5. screen | **仅记录 led 到 static 变量**（`cq_screen_led_get` 可查）；不写 Flash、不切模组（本次不含模组排布） |
| Q6. 通行灯 | **显示器图片模式**：tra1 右下角 32×32 红叉/绿箭位图（`app_render` RENDER_BITMAP） |
| Q7. setip / 重启配置 | `app_board_net_cfg_update` 写 Sector1 net_cfg → `NVIC_SystemReset()` 重启生效（对齐 LDI 0AH 落盘语义） |
| Q8. 默认网关 | **192.168.1.1** |
| Q9. 字库芯片 | **显示层实现**（`app_render`，FONT_ENC_GBK 直通渲染） |
| Q10. JSON 库 | **cJSON**（`Middlewares/Third_Party/cJSON`，钩子换绑 pvPortMalloc/pvPortFree） |
| Q11. 上电欢迎语 | **不在协议层实现**（2026-08-21 撤销 `app_cq_proto_default.c`）：使用系统默认显示「欢迎行驶\n高速公路」（`app_default_display` 无注册回调兜底） |
| Q12. 文件名 | **`app_cq_proto_*` 样式** |
| Q13. TTS 串口 | **`dev_rs232_voice`（USART6）**，禁止协议 bind `CH_ID_RS232_1` |
| Q14. 业务端口可配性 | **支持且仅 CQ 构建可配**：`PROTO_CHONGQING` 下 `setip` port 落盘 Sector1 `udp_port`、UDP_CQ 通道每轮重建重读；dev 共存构建固定 20103（2026-08-21 方案 B：`net_cfg.udp_port` 为 CQ UDP 口专有字段、`net_cfg.port` 为 TCP 口，两端口彻底分离，见 Q24/B.11） |
| Q15. 搜索应答端口 | **从 10011 广播**（`app_udp_broadcast`，25B 应答含 CRC16-XMODEM）；应答内 **port 字段上报 CQ 实际业务口 `app_udp_cq_get_port()`**（2026-08-21 修订：原直读 Sector1 net_cfg.port，dev 构建会误报 LDI 9528） |
| Q16. warn1 -1 | **常开不自动停**（映射 0xFFFF 语义；协议文档 level 参数未实现，注释说明） |
| Q17. 工厂测试与心跳 | **工厂测试期间暂停心跳计数**（新增 `app_factory_test_active()` 查询接口） |
| Q18. RS485 串口转发 | **不迁移**（裸机 `needTxToSerial` 级联功能，STD 未使用） |
| Q19. screen 命令 | **仅记录不生效**。裸机 `cmd_BoardSet` 写 Flash 切模组的语义不迁移（STD 模组适配在 dev_display_1_2xx 中实现） |
| Q20. text1 行号限制 | **ln4 仅字号 ≤24，ln5~7 仅 ≤16**，超限跳过该行（执行层判定）；行超出屏高跳过 |
| Q21. full 颜色=3 | 协议文档说"白色"，红绿双基色屏无法物理显示纯白，**映射红绿全亮即黄** |
| Q22. 队列/内存（新增） | 深度 3 / payload 1044 / msg 1052 / 静态 3156B + 任务帧缓冲 1052B + JSON/文本缓冲 1045+1044B，**全部置 CCMRAM**（SRAM 全协议构建仅余 ~1.9KB；CPU 独占访问无 DMA，安全） |
| Q23. dev 构建 10011 二进制帧（2026-08-21 二次实态修正） | 全协议 dev 构建下 10011 口 12B 二进制帧**先经 LDI probe 探测**：CQ 12B 帧 len=00 00 00 02 且 CRC16-XMODEM 恰好通过 LDI 校验——**LDI probe 对 `data_len==2` 显式 FAKE 放行，CQ probe 认领该帧**（「LDI 先探测、FAKE 放行、CQ 收单」）。修复前 LDI 沿身份校验路径 SKIP 吞掉 CQ 搜索/重启帧（cfg_valid 下 data_len<18 → SKIP），CQ 12B 在 10011 无响应；原「CQ 帧 len 域全 0」表述与实态不符，已修订 app_cq_proto.c 头注释与 doc/05-01 §6 |
| Q24. 业务端口 bug（新增，2026-08-21 修复） | dev 共存构建（LDI 编入）下 `_udp_cq_read_port()` 无条件读 Sector1 `net_cfg.port`，而 `ldi_ctx_init` 空/损坏扇区写回 LDI 出厂记录 port=9528 → CQ 业务口错绑 9528（UDP 20103 无响应、9528 反被 CQ 认领）。修复：`#ifdef PROTO_CHONGQING` 才读 Sector1（无效/0 回退 20103），`#else` 固定 20103；搜索应答 port 字段改报 `app_udp_cq_get_port()`。**2026-08-21 方案 B 二次修复（根除）**：Sector1.net_cfg 新增 `udp_port` 字段与 TCP 口彻底分离——CQ 构建改读 `udp_port`（setip 写入），`port` 恒为 TCP 业务口（LDI/IAP 生态）不再被 setip 污染。LDI 残留记录靠「擦 Sector1 出厂化」纪律清理（flash_all.sh 默认烧后即擦）。详见 B.11 |

---

## B.9 风险与约束

1. **JSON 粘包/半包**：UDP 通常一包一帧，但仍按流式 probe 写（花括号深度扫描）。
2. **花括号定界**：文本字段含 `}` 时同裸机脆弱；GBK 尾字节 0x5C（`\`）落入 JSON 字符串会被 cJSON 当转义符——已记录为已知限制。
3. **亮度档位**：协议 0~15 vs 硬件 8 级，映射 `level/2+1`（上限 8）。
4. **内存**：cJSON 树为 FreeRTOS 堆瞬时分配（钩子 pvPortMalloc/pvPortFree），帧长 ≤ 1044；队列/缓冲静态体置 CCMRAM（见 doc/06-04）。
5. **编码**：text1/voice1 的 t 字段为 GB2312 字节直通（FONT_ENC_GBK 渲染 / 语音板 GBK 直送），不转码。
6. **TTS 串口波特率**：需与语音控制板硬件规格对齐。
7. **text1 行号限制**：ln4 仅支持字号 ≤24，ln5~7 仅支持字号 ≤16；执行层判定超限跳过该行。
8. **full 颜色=3**：协议文档说"白色"，红绿双基色屏映射红绿全亮即黄。
9. **dev 全协议构建 10011 口 12B 二进制帧**：LDI probe 先探测、对 len 域全 0 的帧
   FAKE 放行，CQ probe 认领（「LDI 先探测、FAKE 放行、CQ 收单」）——无功能受限，
   且不构成永久阻塞（LDI FAKE 不消费字节、不置 any_wait）（见 Q23 实态修正）。
10. **dev 共存构建端口语义**：业务口固定 20103；板上若残留 LDI 出厂记录
    （Sector1 port=9528），dev 构建的 LDI 自身仍正常使用 9528，CQ 不受影响
    （修复后不再读该字段）；量产前按纪律擦 Sector1 出厂化。

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

---

## B.11 端口语义修复与深度审计记录（2026-08-21）

### B.11.1 业务端口 bug（已修复）

**症状（用户实测）**：设备对 UDP 20103 无响应，对 UDP 9528 却能处理 JSON；协议要求业务口 20103。

**根因链**：
1. `Application/Src/Channel/app_udp.c` `_udp_cq_read_port()` 无条件读 Sector1
   `net_cfg.port`（有效即用，无效/0 回退 20103），`udp_cq_task` 每轮重建前重读；
2. dev 共存构建（EIDE Debug，LDI 编入）下 `ldi_ctx_init`（`Application/Src/LDI/app_ldi.c`
   :170 附近）在 Sector1 空/损坏时写回 LDI 出厂记录，`net_cfg.port` = 9528
   （`LDI_DEFAULT_CONFIG_PORT`，LDI TCP 配置口语义）；
3. CQ 通道因此绑到 9528：UDP 9528 被 CQ probe 认领处理，20103 无 netconn。

**修复**（`app_udp.c` `_udp_cq_read_port`，:283~:301；2026-08-21 方案 B 二次修订）：
- `#ifdef PROTO_CHONGQING`：读 Sector1 `net_cfg.udp_port`（方案 B 新增 CQ UDP 业务口
  专有字段，CQ setip 写入；空扇区由 `app_net_boot` 写 20103 默认——2026-08-21 起
  替代 `_cq_net_cfg_apply`），有效（1~65535）即用，无效/0 回退 20103；
- `#else`（dev 共存构建）：固定 `UDP_CQ_DEFAULT_PORT`(20103)，不读 `net_cfg.udp_port`。

**连带修复**（`app_cq_proto_cmd.c` `_cq_exec_bin_search`，:438 附近）：25B 搜索应答的
port 字段原直读 Sector1 `net_cfg.port`（dev 构建会误报 LDI 9528），改报
`app_udp_cq_get_port()`（CQ 实际业务口；PROTO_CHONGQING 下两者同源）。

**验证**：`make clean && make -j8` 与 `make clean && make PROTO=CQ -j8` 双构建 0 error；
PROTO=CQ 构建 `nm` 无 `ldi_` 符号（0 个）；PROTO=ALL 反汇编 `_udp_cq_read_port` 为
`movw r2,#20103; strh`（常量写入，无 `app_board_net_cfg_get` 调用）。

**LDI 残留记录纪律**：板上历史 LDI 记录（Sector1 port=9528）靠「擦 Sector1 出厂化」
清理——`tool/flash_all.sh` 默认烧后即擦 Sector1；dev 构建修复后 CQ 不再受其影响。

### B.11.2 深度审计结论（逐项）

| # | 检查项 | 位置 | 结论 |
|---|--------|------|------|
| 1 | probe PURE 契约 | `app_cq_proto.c` `cq_probe_frame` :159 | OK：只 rb_peek；avail==0 返 FAKE；仅 READY 写 total_len；WAIT/FAKE 零副作用 |
| 2 | 花括号深度扫描 | 同上 :174 | OK：深度>16 / scanned>1044 / 孤立 '}' → FAKE；64B 分块 peek 不放大栈。**已知限制（已补注 probe 头注释）**：GB2312 低字节域 0x40~0xFE 含 `{`(0x7B)/`}`(0x7D)/`\`(0x5C)，串内伪花括号会破坏深度定界；JSON 转义 `\"` 未按转义处理——与裸机同脆弱（B.9.2），接受 |
| 3 | 12B 二进制精确匹配 | 同上 :206 | OK：avail<12 → WAIT；memcmp 精确匹配两帧；不匹配 FAKE |
| 4 | 多帧粘包续取 | `app_dispatch.c` `frame_dispatch_task` | OK：READY 后 any_parsed → 内循环继续探下一帧；64 帧上限 + 1B 重同步防死循环 |
| 5 | LDI/CQ probe 顺序 | Makefile SRC_APPLICATION + `Compiler/STM32F407XX_FLASH.ld` | 确认：LDI 源在 CQ 之前，initcall 同层按链接序执行（SORT 对同名 section 为 no-op）→ LDI probe 先于 CQ；CQ 12B 帧 len=2 且 CRC 恰好通过 LDI 校验，2026-08-21 修复：LDI probe 对 `data_len==2` 显式 FAKE → CQ 12B 帧由 CQ 认领（**二次实态修正 Q23**） |
| 6 | 通道断链生命周期 | `app_udp.c` `udp_cq_connect_task` | OK（同 10011 通道既有模式）：deinit 置 `me.ops=nullptr` + `conn=NULL`（2026-08-21 补，广播复用路径判空依据）+ 注销；`channel_send` 双守卫（ops 空 / get 回验）拒发；`s_udp_cq_ch.conn` 不复用旧值（init 重拷模板）。**残余风险**：同优先级任务间 send 与 deinit/netconn_delete 的并发窗口（与 10011 通道同源，非本次引入） |
| 7 | disconnect_sem 逻辑 | `app_udp.c` `udp_cq_task` | OK：信号量 1 槽；bind 成功后先排空再等；link_listener 断链注入 |
| 8 | app_udp_cq_broadcast | `app_udp.c` | OK：当前无调用点（搜索应答走 10011 的 app_udp_broadcast）。2026-08-21 重构：优先复用常驻通道 conn（`udp_cq_task` 创建时已设 SOF_BROADCAST，`app_channel_get` 回验 + conn 非空），常驻未就绪回退临时 conn（netconn_new 失败即返回）；修复池满时临时 conn 必然 NULL 的静默丢包 |
| 9 | 端口读取时序 | `app_udp.c` `udp_cq_task` :307 | OK：每轮循环 bind 前 `_udp_cq_read_port()`，setip 重启后生效 |
| 10 | cJSON 树释放 | `app_cq_proto_parse.c` `cq_parse_frame` | OK：root==NULL / cmd 缺失 / 无效 cmd / default / 正常路径全部 cJSON_Delete，无泄漏路径 |
| 11 | s_cq_text_buf 单消费者 | `app_cq_proto_parse.c` :38 | OK：仅 `cq_proto_handle_task` 调 `cq_parse_frame`；text1 总量超限 `_cq_text_dup` 返 NULL → ERR_PARAM |
| 12 | _cq_parse_ip4 边界 | 同上 :77 | OK：每段 0~255 累加溢出即拒、恰好 4 段、结尾 '\0'；port 1~65535 |
| 13 | warn1 second 语义 | 同上 :283 / `app_cq_proto.c` `cq_proto_warn_set` | OK：-1 常开 / 0 关 / >0 倒计时；<-1 ERR_PARAM |
| 14 | voice1 长度截断 | `app_cq_proto_cmd.c` `_cq_exec_voice1` | OK：执行层按 DEV_RS232_VOICE_MAX_TEXT(200) 截断；解析层 1044 上限内 |
| 15 | raw_len > CQ_PAYLOAD_MAX | `app_cq_proto_parse.c` :205 | OK：拒绝（dispatch 层亦钳制 frame_len ≤ FRAME_DATA_MAX_LEN=1044） |
| 16 | text1 行渲染 | `app_cq_proto_cmd.c` `_cq_exec_text1` | OK：y=i×字号、`y+font > screen_cols`（屏高）跳过；ln4≤24/ln5~7≤16；像素宽按 GBK=字号/ASCII=字号/2 截断到 screen_rows（屏宽）；半 GBK 尾字节不渲染 |
| 17 | tra1 位图坐标 | `_cq_exec_tra1` | OK：`screen_rows<32 || screen_cols<32` 提前 return，坐标为 (rows-32, cols-32) 不为负 |
| 18 | full color 3 | `_cq_exec_full` | OK：0红/1绿/2黄/3白→红绿全亮即黄（双基色屏物理限制，注释在案） |
| 19 | light1 光敏挂起/恢复 | `_cq_exec_light1` | OK：与贵州 '8' 同模式（osThreadSuspend/Resume + dev_display_set_brightness）；level/2+1 上限 8 = DEV_DISPLAY_BRIGHTNESS_MAX |
| 20 | setip 落盘复位 | `_cq_exec_setip` | OK：app_board_net_cfg_update → NVIC_SystemReset；JSON 无应答约定，无需延时（落盘为同步完成） |
| 21 | 搜索应答 25B 帧 | `_cq_exec_bin_search` | OK：buf[23..24]=crc16_xmodem(&buf[2],21) 覆盖 [2..22]；头 9B 固定 0x0F 0xA5；**port 字段已改为 app_udp_cq_get_port()**（本次修复） |
| 22 | 重启应答回源 | `_cq_exec_bin_reboot` | OK：channel_send(msg->ch) 回源通道；ops 空守卫下应答可能丢失但复位照常（可接受） |
| 23 | s_cq_screen_led 不落盘 | `_cq_exec_screen` | OK：仅记录（Q5/Q19 定案：不写 Flash 不切模组） |
| 24 | 120s 故障屏 | `cq_render_fault_screen` | OK：渲染后 s_cq_sync_counter=0 不重复刷屏；小屏防御（cols<32/<96 跳过行） |
| 25 | 工厂测试 vs warn1 | `cq_proto_timer_task` | OK（已注释）：warn1 倒计时在工厂测试期间**不暂停**（裸机 hsCnt 同 ISR 口径）；心跳计数暂停（Q17）。裸机源码不在本仓库，逐行核对缺口已记入注释 |
| 26 | cq_proto_sync_reset 调用点 | `app_cq_proto.c` `cq_proto_handle_task` | OK（2026-08-21 宽松语义 B.7.2）：帧结构合法的 JSON（OK / ERR_CMD / ERR_PARAM）均复位；仅 ERR_FRAME 不复位；二进制帧（重启/搜索）不复位 |
| 27 | 双 mask 注册失败路径 | `cq_proto_init` | OK：第一 mask==0 即 return（不建任务）；第二 mask==0 时 bind/set_frame_queue 对称跳过，队列仍建（第一 mask 用） |
| 28 | CCM 队列越界 | `app_cq_proto.c` | OK：`_Static_assert(CQ_PAYLOAD_MAX <= RB_SIZE_RJ45)`；队列体 3×1052=3156 = mq_size；CCM 总量 ~6.3KB（64KB 池内） |
| 29 | 网络配置应用时序（原 _cq_net_cfg_apply，2026-08-21 移交 app_net_boot） | `app_net_boot.c` `app_net_boot_apply` / `app_boot.c` init_task | OK：`dev_eth_start()` 在 `sw_board_init()`（sw_initcall）之前调用，`pl_net_set_ip` 在 eth 启动后执行；有效性判定 ip 非 0 + port 1~65535，空/损坏写本构建默认落盘 |
| 30 | 协议一致性偏差 | 全部 | 记录：modify/warn1 level/pic1/voice_play1 不实现（与方案一致）；screen 仅记录；voice_control1 grade=level/2+1 上限 7 → dev_rs232_voice_volume 原样透传（驱动无值域校验，grade 域 1~7，符合语音板 0~7 约定子集） |
| 31 | CH_ID_UDP_CQ=7 边界 | `app_dispatch.h` | OK：channels[CH_ID_MAX=8]，索引 7 合法 |
| 32 | 上电画面注册冲突 | （已撤销） | **2026-08-21 已撤销**：删除 `app_cq_proto_default.c`，CQ 不注册默认画面、使用系统默认显示；故障恢复经 `app_default_display_show()` 无回调分支渲染系统欢迎画面 |
| 33 | IAP/LDI/CQ 10011 共存 | 实态 | OK：IAP 0x5A 快拒；LDI 先探测 12B CQ 帧（len=2 且 CRC 通过 LDI 校验）→ 2026-08-21 起对 `data_len==2` 显式 FAKE 放行 → CQ 认领。无已知功能冲突 |

### B.11.3 遗留风险

1. `udp_cq_ch_send` 与断链 deinit/`netconn_delete` 的同优先级并发窗口（send 中途 conn 被删）
   ——与 10011 通道同源模式，未在本次引入，若需根治可加发送锁或在 deinit 前暂停发送方。
2. GBK 尾字节伪 `{`/`}` 与 JSON 转义 `\"` 的花括号定界脆弱性（与裸机同脆弱，接受）。
3. dev 共存构建下搜索应答的 ip/mask/gw 取自 Sector1（LDI 语义），Sector1 空且 W25 有
   配置（LDI 分支 2 会回写 Sector1，一致性有保障；分支 3 双空时报告出厂默认
   192.168.1.5 而实机为 pl_net 默认 IP，偏差仅 dev 构建双空场景）。
4. `app_udp_cq_broadcast` 当前无调用点（死代码），如协议后续要求业务口广播应答，
   端口语义已随 `g_udp_cq_port` 一致。
5. **三固件同步烧录纪律（方案 B 布局变更，2026-08-21）**：方案 B 把 Sector1 记录从
   68B 扩到 72B（`NetConfig_t` 新增 `udp_port`，`config_crc` 偏移 64→68）。
   **设备上 Bootloader/Recovery 若仍是旧 68B 布局（未随主固件同步烧录）**：旧
   Bootloader 按旧偏移读 `config_crc`（读到的是新记录的 `udp_port` 字段）→ CRC 判
   失配 → 重建**旧布局**出厂记录；主固件按 72B 读又失配 → 反复自愈改写，两布局
   交替重建、Sector1 永不稳定（网络配置回出厂态；若旧记录残留升级中间态还可能被
   旧 Bootloader 条件 A/B/D 误判 App 损坏 → 设备陷 Recovery，主固件不运行 →
   **UDP 10011/20103 全部无响应，LDI 搜索/CQ 业务均失效**）。此点修不了代码，
   属烧录纪律：**方案 B 后三固件（Bootloader+Recovery+主固件）必须同步烧录
   （`tool/flash_all.sh` 一次 J-Link 会话）**；任何单固件重烧后按惯例擦 Sector1
   恢复出厂态。
