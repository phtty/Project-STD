# AH 协议族改造与 TCP 参数配置接口 —— 设计方案（v6）

> **状态**：已定稿（2026-11-17）。P0（改名/接构建/显示安全）、P1（配置层）、P2（TCP 参数口）、
> P3（MQTT 接通）已落地并双板构建/host 测试全绿；P4 的双板构建与文档已完成，**上机冒烟待硬件**。
> **协议依据**：《IP 化设备通讯协议》V1.0（2023-07-06，22 页）——
> 第二章 TCP 参数配置（0AH/0BH）、第三章各设备 MQTT 控制、第四章监控接口。
> **历史依据**：旧《费显、情报板 TCP》V1.6、《MQTT 协议_费显、情报板》V1.4（均已被新文档取代）。
> **一句话**：把 `AHMQ` 升格为 **AH 协议族**（AH/MQTT 业务 + AH/TCP 参数）；
> 参数接口严格按文档 0AH/0BH，**无私有命令、无私有配置项**；主题按文档派生；
> 参数落盘后**统一重启生效**（断电重启或 MQTT `op/restart`）。

### 变更记录

| 版本 | 依据 | 主要变化 |
|---|---|---|
| v1 | 旧 TCP V1.6 + MQTT V1.4 | 初版：0AH 202 B / 0BH 244 B、FTP、50 B 专有区、MQTTSetting；用户名密码靠私有扩展 |
| v2 | IP 化设备通讯协议 V1.0 | 0AH/0BH 结构重写（UnixTm/Cert/模块表/用户名密码）；CRC 明确 X25；仍保留 C0/C1/C2 |
| v3 | 同上 | 删除全部私有命令；主题完全派生；改为“应答后即时生效” |
| **v4** | 同上 + 评审意见 | **回到统一“重启生效”**（不做运行期热应用/热重连）；`CertificationInfo` **只存不校验**；先做 32H 但**不硬编码**（类型从模块表/板级默认取，业务按 `device_id` 分派）；`ClientPort` 确认为设备端口、重启生效；模块表语义待确认 |
| **v5** | 同上 + 评审意见 | 模块表语义定稿：**设备只有本机类型的一个模块**；0AH 的 `ModeCnt` 只接受 0 或 1，且 `DeviceId` 必须是本机类型（如 32H 设备收到 12H）→ **整帧静默丢弃**（不应答、不落盘、只记日志）；配置去掉模块数组，改为 `device_id/device_no/font_line`（80 B） |
| **v6** | 同上 + 评审意见 | 情报板清屏命令定稿：保留旧文档的 `ASK/display/clean`（0/1/2/3），新文档未列但预留无坏处；方案无阻塞项 |

---

## 0. 结论先行（TL;DR）

1. **命名**：`AHMQ` → `AH`。协议族下两条信道：
   - **AH/MQTT**：业务（情报板显示、清屏、重启、对时 + 状态、签到上报）；
   - **AH/TCP**：参数（0AH 设置 / 0BH 采集），设备作 **服务端，默认 6001**。
   新文档里 6000 的“设备作客户端”只属于**栏杆机（3.1）和车检（3.7）**，本产品不在其列。
2. **帧格式确认，CRC 不用新写**：文档写明 `CRC-16/X25`，正是工程里的 `crc16_xmodem()`
   （反射多项式 `0x8408`、初值 `0xFFFF`、结果取反；标准校验串 `"123456789"` → `0x906E`）。
3. **接口 = 文档本身**：只实现 0AH/0BH，**不增加任何私有命令**。
   MQTT 用户名/密码、QOS、ServerIP3 都在标准 0AH 里；文档没给的主题/Client ID 全部**派生**。
4. **主题按文档派生**：`收费站HEX/车道HEX/设备类型/设备序号` 取 `LaneHex`（站 4 B + 道 1 B）
   + **显示模块的** `DeviceId/DeviceNo`，再加固定后缀。`CertificationInfo` 是身份码，**不在主题里**。
   模块表只接受“本机类型的一个模块”；出现别的类型（32H 设备收到 12H）→ **整帧静默丢弃**。
5. **生效方式：统一重启**。0AH 只做“校验 → 落盘 → 应答”，**不碰运行态**（不改 IP、不重连 MQTT、不重建主题）。
   断电重启、或已连上 MQTT 时发 `op/restart`，设备从上电流程按新配置起来。
   这样没有跨任务热应用的竞态，也没有“改 IP 把应答连接拆掉”的问题。
   `UnixTm` 是例外：它不是持久化参数，0AH 成功即写 RTC（否则时间没来源）。
6. **启用前必须先修 5 个既有缺陷**（否则一开就是错的，详见 §1.1 / §5.3）：
   - 命令表里存的是主题**后缀**，探针却拿**完整主题**去 `strcmp` → 永远不匹配，订阅也订错；
   - 上报通知号的日期时间/自增计数是硬编码；
   - `_cmd_display` 的 `cvt_utf8_to_gbk` 未夹长度、`gbk_len` 未初始化（会越界写 `gbk_txt[128]`）；
   - 密码默认 `""`（非 NULL、长度 0），lwIP 直接返回 `ERR_VAL`，**一次都连不上**；
   - 连接同步失败不释放 `connect_sem`，主循环永久阻塞，**不会重连**。

---

## 1. 现状核对

### 1.1 AHMQ 的真实状态

| 项 | 现状 | 影响 |
|---|---|---|
| 编译 | `Makefile` 的 `SRC_EXCLUDE` 排除两个 `.c`；EIDE 两个 target 也 exclude | 所有板都不编，改了也测不出来 |
| 启动 | `sw_app_initcall(_ahmq_module_init)` 被注释；`app_mqtt_start()` 无调用者 | 链路完全未激活 |
| Broker | `120.46.136.199:6000`、`CD_ZTP`、`pxh` 写死在 `app_mqtt.c` | 现场无法改 |
| 设备标识 | `station_hex="11451419"`、`lane_hex="01"`、`device_type="32"`、`device_id="01"` 写死 | 主题固定，无法多站多车道 |
| 通知号 | 日期写死 `2025-03-05 14`，`send_count` 恒 `000000` | 与文档第四章不符 |
| 主题表 | `g_ahmq_cmd_table[].topic` 只有 `"ASK/board/NULL"` 这类**后缀**；探针用完整主题 `strcmp`，订阅也拿后缀去订 | **业务命令一条都收不到**（见 §1.2） |
| 显示命令 | `_cmd_display` 未夹长度、`gbk_len` 未初始化 | 文本超长越界 |
| MQTT 连接 | `client_pass = ""`（非 NULL 但长度 0）；lwIP 的 `mqtt_client_connect()` 对非 NULL 凭据要求 `strlen > 0`，否则返回 `ERR_VAL` | **一次都连不上**（即使 broker 不需要密码） |
| MQTT 重连 | `app_mqtt_connect()` 同步失败（`err != ERR_OK`）时只置 DISCONNECTED、**不释放** `connect_sem`；主循环随后永久阻塞在 `osSemaphoreAcquire(..., osWaitForever)` | 失败一次就再也不重连 |

### 1.2 为什么“改参数”之前必须先修主题表

MQTT 主题是七级：`收费站HEX/车道HEX/设备类型/设备序号/ASK|Reply|Push/业务/子业务`。
现在的代码把第 6、7 级（`ASK/board/NULL`）当完整主题：

- `app_mqtt_subscribe()` 拿着 `"ASK/board/NULL"` 去订 —— broker 上不存在这个主题，**不会投递**；
- 探针 `strcmp(g_ahmq_cmd_table[i].topic, src->topic)` —— 一边是后缀、一边是完整主题，**永不相等**，整帧 SKIP。

所以 AH/MQTT 不是“能跑但参数写死”，而是**从未真正跑通**。本次改造顺带把它修到可用。

### 1.3 两条信道的定位（新文档）

```
                ┌────────────── AH 协议族 ──────────────┐
  平台（MQTT）   │  AH/MQTT（业务）      AH/TCP（参数）    │  现场工具/平台
  ─────────────►│  broker 发布/订阅     设备:6001 服务端   │◄──────────
                │  App: app_ah_mqtt    App: app_ah_tcp   │
                └──────────────┬────────────────────────┘
                               │ 共用
                        app_ah_cfg（W25Qxx "ah_cfg"）
```

- 第三章按设备类型分：MQTT 的是通行灯/雾灯/顶棚灯/报警器/限高架/费显/情报板/语音/相机；
  TCP（设备作客户端）的只有栏杆机（3.1）和车检（3.7）。**本产品（费显/情报板）的业务全在 MQTT。**
- 第四章监控：心跳 `EDH` 仅栏杆机/车检需要；重启/对时/状态/签到走 MQTT。

---

## 2. 协议解析结论（实现依据）

### 2.1 帧格式

| 字段 | 长度 | 值 / 说明 |
|---|---|---|
| STX | 2 | `FF FF` |
| VER | 1 | `00`（其他值丢弃） |
| SEQ | 1 | 服务端 `0X`（1~9），客户端/设备 `X0`（10~90） |
| LEN | 4 | 大端；DATA 字节数（高 2 字节恒 0） |
| DATA | LEN | 见 §2.2~§2.5 |
| CRC | 2 | 大端；**CRC-16/X25**，对 **VER..DATA** 计算 |

- **CRC 就是 `Kernel/Src/crc_utils.c` 的 `crc16_xmodem()`**：反射多项式 `0x8408`、初值 `0xFFFF`、
  结果取反 —— 与 X25 一致（`"123456789"` 复算 `0x906E`，正是 X25 的 check 值）。
  直接复用；把注释补一句“即 CRC-16/X25，AH 参数帧亦用”。
- 旧文档四处样例（0AH 请求 `470A`、0AH 应答 `EC3F`、0BH 请求 `AF3B`、0BH 应答 `5987`）
  全部命中该算法，可交叉印证。

### 2.2 0AH 参数设置（请求：固定 84 B + 功能模块）

| 偏移 | 字节 | 字段 | 固件落点 / 处理 |
|---:|---:|---|---|
| 0 | 1 | CmdType = `0A` | 命令分派 |
| 1 | 4 | UnixTm | 平台时间；落盘成功后写 RTC（非持久化参数，立即生效） |
| 5 | 5 | LaneHex | `cfg.lane_hex[5]`；主题派生用 |
| 10 | 8 | CertificationInfo | `cfg.cert[8]`；身份码，**只存不校验** |
| 18 | 2 | Reserved | 保留 |
| 20 | 4 | ServerIP1 | `cfg.server1_ip`（外场控制服务；本期只存） |
| 24 | 2 | ServerPort1 | `cfg.server1_port` |
| 26 | 4 | ServerIP2 | `cfg.server2_ip`（升级服务；本期只存） |
| 30 | 2 | ServerPort2 | `cfg.server2_port` |
| 32 | 4 | ServerIP3 | `cfg.server3_ip`（预留） |
| 36 | 2 | ServerPort3 | `cfg.server3_port` |
| 38 | 4 | MQTTIP | `cfg.mqtt_ip` |
| 42 | 2 | MQTTPort | `cfg.mqtt_port` |
| 44 | 10 | MQTTUser | `cfg.mqtt_user[10]` |
| 54 | 10 | MQTTPwd | `cfg.mqtt_pwd[10]` |
| 64 | 1 | MQTTQOS | `cfg.mqtt_qos`（文档标“预留”，只存不用） |
| 65 | 4 | NtpIP | `cfg.ntp_ip`（本期只存，无 SNTP 客户端） |
| 69 | 4 | ClientIP | `cfg.client_ip`（设备 IP） |
| 73 | 2 | ClientPort | `cfg.client_port`（**设备端口 = 参数服务监听口**，默认 6001） |
| 75 | 4 | Gateway | `cfg.gateway` |
| 79 | 4 | Netmask | `cfg.netmask` |
| 83 | 1 | ModeCnt | 模块个数：本机只接受 `0`（不改）或 `1`（本机类型）；其它→静默丢弃 |
| 84 | Σ | 功能模块表 | 见 §2.5，每项 2 或 3 B |
| **合计** | **84 + Σ模块** | | |

### 2.3 0AH 应答（21 B，状态码 + 本机身份）

| 偏移 | 字节 | 字段 | 来源 |
|---:|---:|---|---|
| 0 | 1 | CmdType = `0A` | — |
| 1 | 4 | UnixTm | **回显请求值**（便于平台配对） |
| 5 | 5 | LaneHex | **本机存储值**（成功时 = 请求值） |
| 10 | 8 | CertificationInfo | **本机存储值**（成功时 = 请求值） |
| 18 | 2 | Reserved | 0 |
| 20 | 1 | ErrCode | `00` 成功；见 §4.5 |

> 回显 `UnixTm` 让平台把应答和请求对上；`LaneHex/Cert` 返回**本机存储值**，
> 平台可核验“我下发的”和“设备存的”一致（比纯回显有用）。

### 2.4 0BH 参数采集

**请求（20 B）**

| 偏移 | 字节 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 1 | CmdType = `0B` | — |
| 1 | 4 | UnixTm | 回显 |
| 5 | 5 | LaneHex | 只记日志（不强制匹配，避免配错车道后读不回来） |
| 10 | 8 | CertificationInfo | 只记日志（**不校验**，身份码未使用） |
| 18 | 2 | Reserved | 忽略 |

**应答（84 B + 本机显示模块 2/3 B）**

| 偏移 | 字节 | 字段 | 来源 |
|---:|---:|---|---|
| 0 | 1 | CmdType = `0B` | — |
| 1 | 4 | UnixTm | 回显请求 |
| 5 | 5 | LaneHex | **本机存储值** |
| 10 | 8 | CertificationInfo | **本机存储值** |
| 18 | 2 | Reserved | 0 |
| 20 | 4 | ServerIP1 | `cfg.server1_ip` |
| 24 | 2 | ServerPort1 | `cfg.server1_port` |
| 26 | 4 | ServerIP2 | `cfg.server2_ip` |
| 30 | 2 | ServerPort2 | `cfg.server2_port` |
| 32 | 4 | ServerIP3 | `cfg.server3_ip` |
| 36 | 2 | ServerPort3 | `cfg.server3_port` |
| 38 | 4 | MQTTIP | `cfg.mqtt_ip` |
| 42 | 2 | MQTTPort | `cfg.mqtt_port` |
| 44 | 10 | MQTTUser | `cfg.mqtt_user` |
| 54 | 10 | MQTTPwd | `cfg.mqtt_pwd`（明文回读，见 §9） |
| 64 | 1 | MQTTQOS | `cfg.mqtt_qos` |
| 65 | 4 | NtpIP | `cfg.ntp_ip` |
| 69 | 4 | ClientIP | `cfg.client_ip` |
| 73 | 2 | ClientPort | `cfg.client_port` |
| 75 | 4 | Gateway | `cfg.gateway` |
| 79 | 4 | Netmask | `cfg.netmask` |
| 83 | 1 | ModeCnt | 固定 `01`（本机只有一个显示模块） |
| 84 | 2/3 | 本机显示模块 | `{cfg.device_id, cfg.device_no[, cfg.font_line]}` |
| **合计** | **84 + 2/3** | | |

> 0AH 请求与 0BH 应答的**公共块偏移完全一致**（0~83），可共用一套字段偏移常量；
> 但报文长度/尾部模块必须按各自命令分别校验。

### 2.5 功能模块表（每个模块 2~3 B）

| DeviceId | 设备 | 字节 | 字段 |
|---:|---|---:|---|
| `11H` | 自动栏杆机 | 3 | DeviceId, DeviceNo, FiterTm(过滤时间 ms) |
| `12H` | 通行信号灯 | 2 | DeviceId, DeviceNo |
| `13H` | 雾灯 | 2 | DeviceId, DeviceNo |
| `14H` | 天棚信号灯 | 2 | DeviceId, DeviceNo |
| `15H` | 报警器 | 2 | DeviceId, DeviceNo |
| `16H` | 限高架 | 2 | DeviceId, DeviceNo |
| `21H` | 车检 | 3 | DeviceId, DeviceNo, FiterTm |
| `31H` | 费额显示屏 | 3 | DeviceId, DeviceNo, FontLine(默认行数 0~6) |
| `32H` | LED 情报板 | 2 | DeviceId, DeviceNo |
| `39H` | 语音播报 | 2 | DeviceId, DeviceNo |

- 文档的模块表是给“多模块设备”用的；本机**只有一个显示模块**，类型由板级
  `BOARD_AH_DISPLAY_TYPE` 决定（两块板当前都是 `0x32`）。
- **0AH 模块表语义（定稿）**：
  - `ModeCnt = 0`：不改模块（保留现有 `device_no/font_line`）；
  - `ModeCnt = 1` 且 `DeviceId == BOARD_AH_DISPLAY_TYPE`：采纳 `DeviceNo`（31H 还采纳 `FontLine`）；
  - `ModeCnt > 1`、或 `DeviceId` 不是本机类型（如 32H 设备收到 12H/31H/33H）：
    **整帧静默丢弃**——不应答、不落盘、不改 RAM，只记一条日志。
- 因为只接受本机类型，不需要维护“所有类型的模块长度表”来跳读；长度校验只针对本机类型的 2/3 B。
  帧长与本机类型对不上同样静默丢弃。
- **不硬编码 32H**：
  - 类型用命名常量 `AH_DEV_FEE_DISPLAY = 0x31` / `AH_DEV_LED_BOARD = 0x32`，不出现裸数字；
  - 出厂默认来自板级 `BOARD_AH_DISPLAY_TYPE`，共享代码不写字面量；
  - 配置里存 `device_id/device_no`，业务按 `cfg.device_id` 分派（`switch`），
    31H 分支留“未实现”日志——不是编译期 `#if` 固定成情报板。

### 2.6 已确认口径与剩余待确认

**已确认（本版按此实现）**

| 项 | 口径 |
|---|---|
| 主题组合 | 按文档：LaneHex（站/道）+ 显示模块 `DeviceId/DeviceNo` + 固定后缀；Cert 不参与 |
| `CertificationInfo` | 身份码，**只存不校验**；0AH 存、0BH 原样返回本机值 |
| 0BH 应答的 `LaneHex/Cert` | 返回**本机存储值**（不是回显） |
| 显示类型 | 先做 32H 情报板；**不硬编码**，类型来自 `BOARD_AH_DISPLAY_TYPE`；0AH 只接受本机类型 |
| `ClientPort` | 设备端口 = 参数服务监听口；**重启生效** |
| 生效方式 | **统一重启生效**（0AH 不碰运行态）；断电重启或 MQTT `op/restart` |
| 0AH 的 `UnixTm` | 例外：立即写 RTC（不持久化，重启后靠 RTC 续走） |
| “00H 默认值” | 逐字段“全 0 = 保持原值”；`ModeCnt=0` = 不改模块 |
| 非本机模块 | `ModeCnt>1` 或 `DeviceId != BOARD_AH_DISPLAY_TYPE` → **整帧静默丢弃**（无应答、只记日志） |
| `MQTTQOS` | 文档标“预留”，只保存/回读，不参与发布订阅 |
| 字符串填充 | 发送 `0x00` 填充；解析同时接受 `0x00`/空格结尾；全 0 = 不携带凭据 |
| 应答 SEQ | 接收任意；应答 `(req & 0x0F) << 4`，低半字节 0 时用 `0x10`（单函数可切换） |
| 情报板清屏 | 保留旧文档的 `ASK/display/clean`（0 文字/1 全红/2 全绿/3 全黄）；新文档 3.9 未列，预留兼容 |
| 31H 业务载荷 | 本期不做，保留后续增量（见 §5.4） |

**剩余待确认**

| # | 问题 | 现状 |
|---|---|---|
| Q8 | 参数口是否需要来源 IP 白名单/鉴权 | 文档没有；本期不做，不阻塞 |
| Q9 | 31H 费显业务载荷什么时候要 | 本期只做 32H，代码按 `cfg.device_id` 预留分派，不阻塞 |

### 2.7 派生：身份与主题

```
收费站   = LaneHex[0..3] → 8 个 hex 大写字符
车道     = LaneHex[4]    → 2 个 hex 字符
设备类型 = cfg.device_id（= BOARD_AH_DISPLAY_TYPE）→ 2 个 hex 字符
设备序号 = cfg.device_no → 2 个 hex 字符

prefix = "3201FFFF/01/32/01"

订阅：prefix + "/ASK/board/NULL"
      prefix + "/ASK/display/clean"
      prefix + "/ASK/op/restart"
      prefix + "/ASK/op/checktime"
回复：prefix + "/Reply/...（同上四条）"
发布：prefix + "/Push/monitor/devstatus"
      prefix + "/Push/monitor/sign"

MQTT Client ID = "CD" + 站(8) + 道(2) + 型(2) + 号(2)   例 "CD3201FFFF013201"
```

> `CertificationInfo` 不在主题里（主题第 3、4 级是设备类型/序号，来自本机显示模块）。
> Client ID 文档没定义，按上面规则派生即可保证多台设备不重名，不需要配置接口。

---

## 3. 配置模型 `app_ah_cfg`

### 3.1 结构（持久化载荷，`[[gnu::packed]]`，`static_assert(sizeof == 80)`）

```c
typedef struct [[gnu::packed]] {
    /* ---- 标准 0AH 字段（顺序与协议 1~83 一致，便于整段解析/回读） ---- */
    uint8_t  lane_hex[5];
    uint8_t  cert[8];                 /* CertificationInfo（只存） */
    uint8_t  server1_ip[4];
    uint16_t server1_port;
    uint8_t  server2_ip[4];
    uint16_t server2_port;
    uint8_t  server3_ip[4];
    uint16_t server3_port;
    uint8_t  mqtt_ip[4];
    uint16_t mqtt_port;
    char     mqtt_user[10];           /* 定长；不足补 0x00 */
    char     mqtt_pwd[10];
    uint8_t  mqtt_qos;                /* 文档“预留”，只存 */
    uint8_t  ntp_ip[4];
    uint8_t  client_ip[4];
    uint16_t client_port;             /* 设备端口/参数服务监听口，默认 6001 */
    uint8_t  gateway[4];
    uint8_t  netmask[4];
    uint8_t  device_id;               /* 显示模块类型，= BOARD_AH_DISPLAY_TYPE */
    uint8_t  device_no;               /* 显示模块序号 */
    uint8_t  font_line;               /* 31H 默认行数 0~6；32H 恒 0 */
    uint8_t  reserved[1];             /* 记录版本演进用 */
} app_ah_cfg_t;
```

- 配置结构**不含**报文头部的 `CmdType`/`UnixTm`/`Reserved`（瞬态量），其余字段顺序与协议 1~83 一致；
  解析/组帧按 §2.2 的**报文偏移**读写，不依赖结构体布局
  （另用 `offsetof` 的 `static_assert` 把结构体布局钉住，防止日后插字段导致落盘格式漂移）。
- 载荷 80 B；远小于 `DEV_CFG_RECORD_MAX_IMAGE`（2600）。
- `cert` 是 8 字节原始数据（不假定字符串）；`mqtt_user/pwd` 定长 10，允许 10 字符无终止符（内存里补 NUL）。

### 3.2 字段 → 消费方 → 生效方式

| 字段 | 消费方 | 生效 |
|---|---|---|
| `lane_hex`、`device_id/device_no` | AH/MQTT 主题、Client ID、通知号 | 重启 |
| `cert` | 0BH 回读（不校验） | — |
| `mqtt_ip/port`、`mqtt_user/pwd` | `app_mqtt`（broker、凭据） | 重启 |
| `client_ip/gateway/netmask` | `pl_net_set_ip()`（上电应用） | 重启 |
| `client_port` | AH/TCP 监听口 | 重启 |
| `mqtt_qos`、`server1/2/3_*`、`ntp_ip` | 只存/回读 | — |
| `font_line` | 只存（31H 费显业务未实现） | — |

> **统一重启生效的取舍**：0AH 是整包下发，运行期热应用需要处理“改 IP 把应答拆掉”、
> “MQTT/上报任务读到半套配置”、“主题数组重建与探针/发送并发”三类竞态；
> 而这类外设本来就可以断电重启，现场也能接受。重启生效把复杂度降到最低，
> 0AH 只负责“可信地把参数写进 Flash 并回 ACK”。

### 3.3 默认值（首次上电 / 记录无效）

| 字段 | 默认 | 说明 |
|---|---|---|
| `lane_hex` | `32 01 FF FF 01` | 文档主题样例，站 `3201FFFF`、道 `01` |
| `cert` | 8 字节 0 | 未配置；只存 |
| `mqtt_ip/port` | `120.46.136.199` / `6000` | 沿用现值 |
| `mqtt_user/pwd` | `"pxh"` / 空 | 沿用现值 |
| `mqtt_qos` | 0 | |
| `client_ip/mask/gw` | 取板级 netif 默认 `pl_net_get_ip()`（dev_eth：192.168.1.100/24、gw 1.1）；IAP 记录的 192.168.114.200 是 Recovery/升级约定地址，不参与主应用默认 | 与板级默认一致 |
| `client_port` | 6001 | 参数口 |
| `device_id/device_no/font_line` | `BOARD_AH_DISPLAY_TYPE` / `0x01` / `0` | 类型来自板级宏，不是共享代码里的字面量 |

> `BOARD_AH_DISPLAY_TYPE` 放 `boards/<板>/board.h`（两块板当前都是 `0x32`），
> 共享代码直接引用、**不给兜底**：漏定义就编译失败（故意的，避免共享层偷偷写死 32H）。
> 这符合命名约定“`BOARD_*` 只能定义在 `boards/<板>/`”的约束。

### 3.4 持久化

- 记录名 **`"ah_cfg"`**、版本 **1**、载荷 `sizeof(app_ah_cfg_t)`；记录头/长度/CRC32 由 `dev_cfg_record` 负责。
- 注册在 `sw_dev`；读取由 `app_cfg_sched_load_all()` 在 `sw_app` 调 `load` 回调完成（现有机制）。
- 消费者（AH/MQTT、AH/TCP）的 initcall 放 **`sw_post`**，保证 `load_all` 已跑完
  ——**不要靠链接顺序**（AGENTS 坑 4）。
- 版本演进：载荷布局一变就 `version + 1`，旧记录判 INVALID → 回落默认。
- 加载时校验 `device_id`：若记录里的类型与 `BOARD_AH_DISPLAY_TYPE` 不符（记录来自别的板/型号），
  强制改回本机类型并记日志——避免用错载荷解析（32H 与 31H 的显示命令结构不同）。
- 写：`0x0A` → 校验通过才 `app_cfg_sched_save()`；调度器自带“读回对比去重”，重复下发不磨损 Flash。

### 3.5 通知号（20 字符）

`YYYYMMDDhh` + 设备类型 2 位 + 设备序号 2 位 + 6 位自增：

- 时间由 `pl_rtc_get_timestamp()` 经 `gmtime_r()` 生成；
- 0AH 的 `UnixTm`（立即写 RTC）与 MQTT 对时命令（`op/checktime`，type=1）都写 RTC；
- 命令里收到的 NotifyID 只用于**回复**；`Push` 上报用设备自己生成的号并自增
  （现状是把收到的号复制进上报，语义不对）。

---

## 4. AH/TCP 参数信道

### 4.1 TCP server 通道多实例化（现有通道改造）

现状：`app_tcp_server.c` 是单例——静态 port（9528）、静态 ccb、静态断开信号量，
`app_tcp_server_ccb()` 被 LDI 绑死。参数口需要**第二个监听实例**（6001）。

改造（保留现有 API，LDI 零改动）：

```c
/* app_tcp_server.h */
typedef struct app_tcp_server app_tcp_server_t;

app_tcp_server_t *app_tcp_server_open(const char *name, uint16_t port,
                                      uint32_t idle_timeout_ms); /* 建任务并监听 */
app_ccb_t        *app_tcp_server_ccb_of(const app_tcp_server_t *srv);
app_ccb_t        *app_tcp_server_ccb(void);      /* 默认实例（9528，兼容 LDI） */
uint16_t          app_tcp_server_get_port(void); /* 默认实例端口，兼容 IAP 镜像 */
```

- 实例结构：`{ app_tcp_ccb_t ccb; const char *name; uint16_t port; uint32_t idle_timeout_ms;
  struct netconn *conn; osSemaphoreId_t disconnect_sem; }`，静态池 2 个。
- 连接任务参数改为“实例指针”，conn 挂在实例上（原模型不变：同一时刻一个连接）。
- 实例 B（AH 参数）：`idle_timeout_ms = 30000`，用 `netconn_set_recvtimeout()` 回收空闲连接；
  实例 A（LDI）保持 `0`（无限等待 + keepalive），行为不变。
- 资源：多 1 个 manage 任务 + 1 个 conn 任务（各 1 KB 栈），静态池 ~100 B。

### 4.2 探针（`app_ah_tcp_probe_frame`）

```
n < 8                      → WAIT
stx != FFFF                → FAKE（前进 1 字节重扫）
len = be16(buf+6)
len > AH_TCP_DATA_MAX      → FAKE（先挡住坏长度，防止被它带走）
LEN[0..1] != 0             → SKIP（帧长仍可信，跳 8+len+2；记日志）
ver != 00                  → SKIP（同上，记日志）
n < 8 + len + 2            → WAIT
crc16_xmodem(buf+2, 6+len) != be16(buf+8+len)
                           → FAKE（前进 1 字节重同步）
else                       → READY；*total_len = 8+len+2；*aux = buf[8]（CmdType）
```

- `AH_TCP_DATA_MAX = 128`：0AH 最长 84 + 8×3 = 108；0BH 应答同理；`payload_max = 128`。
- 与 MQTT 的“按 NUL 定界”完全不同：这里**只认 LEN**，二进制载荷里的 0x00 不影响分帧。
- 探针只窥视不消费；CRC 失败返回 FAKE 让框架前进 1 字节重扫，避免一次坏长度吞掉后续好帧。

### 4.3 组帧与应答

```c
static void _ah_tcp_reply(app_ccb_t *ccb, uint8_t seq, const uint8_t *data, uint16_t len);
```

- 头部按字节拼（不用位域/联合体，避免端序歧义）；LEN 高 2 字节写 0；
- CRC 覆盖 VER..DATA，大端写入；
- 应答 SEQ：`(req & 0x0F) ? ((req & 0x0F) << 4) : 0x10`（单函数可切换成回显）；
- `app_ccb_send_to(ccb, nullptr, ...)`：TCP 通道忽略目的地，回复到本连接。

### 4.4 0AH 处理流程（原子校验 → 落盘 → 应答；不碰运行态）

```
收到 0AH
 1. 固定部分长度 ≥ 84，否则 ErrCode 05
 2. staged = 当前 cfg（copy）
 3. 解析 1~83 的固定字段：全 0 → 保持；非 0 → 写入 staged
 4. 解析 83 的 ModeCnt（长度必须与分支一致）：
      0                         → 不改 device_no/font_line（长度必须 84）
      1 且 DeviceId == 本机类型 → 采纳 DeviceNo（31H 还采纳 FontLine；长度 84+2/3）
      其它（>1、DeviceId 非本机类型、长度对不上）→ 静默丢弃整帧
 5. 整体校验（§4.5），任一失败 → 回 ErrCode 01，不落盘
 6. app_cfg_sched_save(staged)
      失败 → 回 ErrCode 02/03
 7. 提交 RAM cfg = staged（供 0BH 回读）
 8. UnixTm 合法（2020~2100）→ pl_rtc_set_timestamp()
 9. 回 {0A, UnixTm 回显, 本机 LaneHex/Cert, Reserved=0, ErrCode=00}
10. 结束。不调用 pl_net_set_ip、不重连 MQTT、不重建主题 —— 等重启
```

- **先校验后写**：不会出现“写了一半发现网关不在网段”。
- **应答在写之后**：回 00 就代表已经落盘，断电重启不会丢。
- **不碰运行态**：改 IP/broker/身份都不影响当前连接和当前业务，直到下次上电。
- 0BH：由 RAM 里的 cfg 组 `84 + 2/3` 应答（不读 Flash，快且不受写忙影响）。

### 4.5 校验规则

| 字段 | 规则 | 失败码 |
|---|---|---|
| 0AH 固定部分长度 | < 84 → 回 ErrCode 05 | 05 |
| 0AH 模块表 | `ModeCnt` 0/1；为 1 时 `DeviceId`=本机类型且长度 84+2/3；否则静默 | 静默 |
| 0BH 请求长度 | ≠ 20 → 不应答 | 静默 |
| CRC | 不匹配 → 不应答 | 静默 |
| `UnixTm` | 0（不校时）或 2020~2100 的合理区间 | 01 |
| `lane_hex` | 全 0 保持；否则任意（主题按 hex 大写输出） | — |
| `cert` | 直接保存；不校验 | — |
| IP | 全 0 保持；拒绝 `255.255.255.255`、`224.0.0.0/4`、`127.0.0.0/8`、`169.254.0.0/16` | 01 |
| 端口 | 0 保持；`1..65535`（<1024 记警告仍接受） | 01 |
| `netmask` | 全 0 保持；必须连续 1（如 `255.255.255.0`），不能全 1/全 0 | 01 |
| `gateway` | 全 0 保持；非 0 时必须与 `client_ip & netmask` 同网段 | 01 |
| `mqtt_user/pwd` | 定长 10；全 0 = 无凭据；允许 10 字符无终止符（内部补 NUL） | 01 |
| `mqtt_qos` | 0~2 之外只记日志（文档“预留”） | — |
| `DeviceNo/FontLine` | DeviceNo 1..0xFF；FontLine 0~6（仅 31H 有） | 01 |

ErrCode 枚举（文档只定义 00）：`00` 成功、`01` 参数非法、`02` 保存失败、`03` 未就绪、
`04` 命令/版本不支持、`05` 帧格式错。

### 4.6 上电应用顺序（重启生效的落点）

```
sw_dev : app_cfg_sched 初始化；ah_cfg 注册（load 回调）
sw_app : app_cfg_sched_load_all() → ah_cfg 读记录/回落默认
         LDI 初始化（若也配了 IP，会在这里 pl_net_set_ip）
sw_post: ah_cfg 应用：
           1) pl_net_set_ip(cfg.client_ip, netmask, gateway)   // 仅记录有效且与当前不同
           2) app_mqtt_set_broker(cfg.mqtt_ip, cfg.mqtt_port)
              app_mqtt_set_credentials(派生 client_id, user?, pwd?)
           3) 构建主题表（LaneHex + cfg.device_id/device_no）
           4) app_dispatch_bind + app_mqtt_subscribe
           5) 打开 AH/TCP 实例（cfg.client_port）
app_boot: app_mqtt_start()（与其它通道并列）
```

- `pl_net_set_ip()` 会触发 `app_iap_cfg.c` 的 IP 监听，自动把 IP/掩码/网关镜像到 IAP 记录（现有机制）。
- **LDI 0AH 与 AH 0AH 都能改 IP**：两者都在 `sw_post` 应用，谁后跑取决于链接顺序
  （**不可依赖**，见 AGENTS 坑 4）。部署时二选一；AH 应用时前后各打印一次 IP 与来源，现场可据此判定。
- 参数口改端口后，**下次上电**在新端口监听；改完当次仍可在旧端口读 0BH 确认。

---

## 5. AH/MQTT 业务信道改造

### 5.1 主题表

- 命令表由 `{后缀, handler}` 改为 `{层级后缀, handler}`，上电时与 prefix 拼成完整主题，
  存 `s_ah_mqtt_sub_topics[4][96]`、`s_ah_mqtt_reply_topics[4][96]`、发布主题 2 条。
- 探针改为**完整主题 strcmp**；订阅登记用完整主题。
- 主题只在**上电时**构建一次（重启生效），运行期不变，因此可以继续沿用
  `app_mqtt_subscribe()` 的“通道长期持有指针”契约，不需要给通道加锁或拷贝。
- 主题拼接、通知号、Client ID 生成放 `app_ah.c`（纯函数，可 host 测试）。

### 5.2 配置注入（上电一次）

`app_ah_mqtt` 的 `sw_post` initcall：

```
cfg = app_ah_cfg_get()
client_id = "CD" + 站(8) + 道(2) + 型(2) + 号(2)
app_mqtt_set_broker(cfg.mqtt_ip, cfg.mqtt_port)
app_mqtt_set_credentials(client_id, empty→NULL(user), empty→NULL(pwd))
构建主题表
app_dispatch_bind(pcb, app_mqtt_ccb())
app_mqtt_subscribe(完整主题, 4)
创建 AH 协议任务（等待 READY 后启动 10s 上报 / 5min 签到）
```

`app_boot.c` 增加 `app_mqtt_start()`（与其它通道并列）。

### 5.3 启用前必修

| 缺陷 | 修法 |
|---|---|
| 主题后缀 vs 完整主题 | §5.1 |
| `_cmd_display` 越界 | 先把 `strlen(buff) - sizeof(head)` 夹到 `sizeof(gbk_txt)-1`，`gbk_len` 初始化；超过 127 B 的文本截断并回 ErrCode（回复仍 0/1，按文档） |
| 回复长度 `sizeof("0")` = 2 | 改成 1 字节 `'0'` |
| 空密码导致 `ERR_VAL` | 凭据转换：空字符串一律传 `NULL`（lwIP 用 NULL 表示“不带该字段”）；`client_id` 必须非空 |
| 连接失败不重连 | `app_mqtt_connect()` 同步失败时 `osSemaphoreRelease(ctx->connect_sem)`（或主循环改“超时 + 重试”），保证 1 s 退避重连 |
| 订阅结果未检查 | `mqtt_subscribe()` 返回值/`SUBACK` 至少日志打印失败 |
| 上报/签到任务的 `s_ahmq.notify_id` 与命令收到的号混用 | 分开：`notify_in`（命令）与自生成号（上报） |

### 5.4 显示类型分派（32H 先做，但不硬编码）

```c
/* 运行时按 cfg.device_id 分派，不是编译期 #if 固定成 32H */
switch (cfg.device_id) {
case AH_DEV_LED_BOARD:   /* 32H：本期实现 */
    _cmd_display_board(...);   /* NotifyID + OpenClose + FontColor + FontSize + Text */
    break;
case AH_DEV_FEE_DISPLAY: /* 31H：本期只记日志 */
    printf("[ah] 31H 费显业务载荷未实现，忽略显示命令\n");
    _reply_error(...);
    break;
default:
    _reply_error(...);
}
```

- **32H 情报板（本期实现）**：`NotifyID(20) + OpenClose(1) + FontColor(1) + FontSize(1) + Text(N)`，
  与现有 `_cmd_display` 结构一致；`Text` 最多 6 个中文字符。
- **31H 费显（后续增量）**：`NotifyID(20) + FontColor(1) + FontSize(1) + LineCount(1) +
  KeepTime(2) + LineText(N)`，行间 `_`，每行 ≤10 汉字，`KeepTime` 秒后清屏。
- 清屏：费显 `ASK/display/clean`（0/1/2）；情报板在新文档 3.9 里**没有清屏章节**
  （旧 MQTT 文档 2.7.2 有，且带第 3 种“全黄”）。本方案对 32H **保留** `ASK/display/clean` 兼容，
  按 0/1/2/3 处理（已确认保留）。

---

## 6. 代码落点

### 6.1 目录与文件

| 旧 | 新 | 说明 |
|---|---|---|
| `Application/{Inc,Src}/AHMQ/` | `Application/{Inc,Src}/AH/` | 目录改名（2 字母缩写，登记见 §6.5） |
| `app_ahmq.h` | `app_ah_mqtt.h` | 类型 `app_ahmq_*` → `app_ah_mqtt_*` |
| `app_ahmq_cmd.h` | `app_ah_mqtt_cmd.h` | 同上 |
| `app_ahmq.c` | `app_ah_mqtt.c` | 协议任务 + 探针 |
| `app_ahmq_cmd.c` | `app_ah_mqtt_cmd.c` | 命令处理 + 显示类型分派 |
| — | `app_ah.h` | 设备类型/错误码/上限等常量 |
| — | `app_ah.c` | 纯函数：hex 格式化、主题前缀、通知号、Client ID |
| — | `app_ah_cfg.[ch]` | 配置模型 + 持久化 + 校验 |
| — | `app_ah_tcp.[ch]` | 帧编解码 + 0AH/0BH（仅此两条命令） |
| `Channel/app_tcp_server.[ch]` | 改 | 单例 → 实例池（保留兼容 API） |
| `boards/<板>/board.h` | 改 | 新增 `BOARD_AH_DISPLAY_TYPE`（两块板都是 0x32） |

预估：新增 ~800 行（含注释），改 ~200 行，测试 ~700 行。

### 6.2 构建清单（两套，缺一不可）

- `Makefile`：
  - 删 `SRC_EXCLUDE` 里的两条 AHMQ；
  - `INC_DIRS` 的 `Application/Inc/AHMQ` → `AH`；
  - `SRC_APPLICATION` 新增 3 个 `.c`（`app_ah.c` / `app_ah_cfg.c` / `app_ah_tcp.c`），另 2 个由 AHMQ 改名；
  - 测试段加 3 个套件（源 + 依赖行）。
- `.eide/eide.yml`：两个 target 的 `excludeList`/源清单/头文件路径同步（EIDE 会重写，改前关 IDE）。
- `docs/`、`AGENTS.md`：见 §6.4。

### 6.3 host 测试（19 → 22 套件）

| 套件 | 覆盖 |
|---|---|
| `test_ah` | hex/主题前缀/通知号/Client ID（纯函数）：站 8 位、道 2 位、类型/序号 2 位；自增回卷；日期边界 |
| `test_ah_cfg` | 默认值；EMPTY/INVALID/IO_ERR → 默认；roundtrip；版本不符；字段校验矩阵（IP/掩码/网关/端口/模块表/凭据）；板级默认类型 |
| `test_ah_tcp` | 探针四态；组帧 CRC 已知向量；0AH 模块表（ModeCnt=0/1、本机类型 32H、非本机类型 12H/31H **静默丢弃**、ModeCnt>1 静默、长度不符静默）；0AH 应答 21 B 逐字段；0BH 请求 20 B / 应答 86 B（`{32H,01}`）逐字段；零保持/非法零副作用/成功即 save；**0AH 不调用 pl_net_set_ip / MQTT / 重建主题**（stub 断言零副作用） |

要求：`make test` 与 `make BOARD=5006048 test` 双绿、`make lint` 全过；
协议命令按 `docs/测试约定.md` §3 的配方写（正例 + 越界矩阵两条 case）。

### 6.4 文档同步

- `docs/架构说明.md`：§9 网络（两个 TCP server）、§10 协议表（AH/MQTT + AH/TCP）、§8 配置所有者；
- `docs/命名约定.md` §10.2：目录登记 `AHMQ` → `AH`；
- `docs/测试约定.md`：套件数 19 → 22；
- `AGENTS.md`：§3 目录地图、§6 坑 10（AHMQ 排除说明作废，改为 AH 协议族说明）、§7 测试表；
- `Kernel/Inc/crc_utils.h` 注释补“CRC-16/X25，AH 参数帧亦用”。

### 6.5 命名与契约

- 类型 `app_ah_*_t`、函数 `app_ah_*`、static `_ah_<子模块>_*`、`g_ah_*`/`s_ah_*`（不出现裸 `s_ah`）；
- 宏 `AH_*`（协议族）/`AH_MQTT_*`（MQTT 子模块）；设备类型用 `AH_DEV_*` 命名常量；
- **新契约**：记录名 `"ah_cfg"`、记录版本 1、0AH/0BH 的字段偏移、模块表长度规则、
  主题派生规则——一旦上现场就进 `命名约定.md` §13 的禁改清单。

---

## 7. 分阶段实施

> 每阶段结束都可编译、可跑测试、可回退；P0/P1/P2 不改变现网行为。

### P0 —— 改名 + 接进构建（0.5 天）

- 目录/文件/符号改名；`Makefile` 与 EIDE 双清单；删除 `SRC_EXCLUDE`。
- initcall 仍保持注释（**行为零变化**），先证明两份构建都绿。
- 顺手修 `_cmd_display` 的越界（编译能过、运行时才炸，必须在启用前修）。
- 验收：两板 `make` 通过；`make test`/`make lint` 绿；`git diff` 只有改名与清单。

### P1 —— 配置层（1 天）

- `app_ah_cfg` + `app_ah` 纯函数 + 持久化注册 + 默认值 + 校验（含模块表、板级默认类型）。
- `test_ah`、`test_ah_cfg`。
- 验收：host 测试全绿；上板后串口打印加载结果（默认/记录有效）。

### P2 —— TCP 参数信道（2 天）

- `app_tcp_server` 多实例化（LDI 回归：9528 仍能连、IAP 镜像端口不变）。
- `app_ah_tcp`：探针、组帧、0AH/0BH、模块表校验（非本机类型静默丢弃）、UnixTm 校准 RTC、**不碰运行态**。
- AH 参数口 6001 起来；可用 `nc`/Python 脚本读写参数。
- 验收：`test_ah_tcp` 绿；上板 0BH 回读与默认值一致；0AH 写入 → 断电重启 → 0BH 一致；
  改 IP 后重启，设备在新 IP 上仍能应答。
- 状态：**已落地**（双板构建 + `test_ah_tcp` 80 条断言）；上机验收待硬件。

### P3 —— MQTT 业务接通（1 天）

- 修主题表/通知号/显示越界；上电配置注入（含 10 B 用户名/密码）；`app_mqtt_start()`；AHMQ 任务启用。
- 验收：设备连上 broker（MQTT 3.1.1）；平台下发 `ASK/board/NULL` 能显示、回复主题正确；
  10 s 状态、5 min 签到在 `Push/...` 上可见；对时后通知号时间正确。
- 状态：**已落地**（`test_ah_mqtt` 36 条 + `test_ah_mqtt_cmd` 54 条断言）；上机验收待硬件。

### P4 —— 双板 + 上机冒烟 + 文档（1 天）

- 两块板都烧；级联/折叠屏不受影响；文档与 AGENTS 同步。
- 状态：双板构建、`make test`/`make lint` 与文档已完成；**烧板/上机冒烟未执行**（当前无 ST-Link）。
  上机时按 §8.3 跑 `test/tools/ah_param.py get/set`，并回归 9528 的 LDI/IAP 与级联显示。

合计约 **5.5 人日**（另留 1 人日缓冲，合计 6.5，见 §11；不含甲方确认与现场联调）。

---

## 8. 验证方案

### 8.1 host

```
make test && make BOARD=5006048 test      # 24 套件双绿
make lint
make mutate MUT_ARGS="--suite test_ah_tcp --fail-on-alive"   # 可选
```

### 8.2 构建

```
make -j$(nproc) && make BOARD=5006048 -j$(nproc)
```
检查 `.map`/size：新增静态 RAM 约 1.8 KB（配置 80 B + 主题表 ~1 KB + 通知队列 +384 B + 实例池 ~100 B），
另加 1 个监听任务 + 1 个连接任务的栈（各 1 KB，堆分配）；Flash 约 6~8 KB。两块板都要看余量。

### 8.3 上机（脚本：`test/tools/ah_param.py`）

先跑 `test/tools/ah_param.py self-test` 验证组帧/CRC/解析；连设备时用 `get` / `set`。

1. **0BH 读默认**：连 6001，发 20 B 请求（UnixTm + LaneHex + Cert + Rsv），校验 CRC 与 86 B 应答字段；
2. **0AH 写**：改 LaneHex、MQTT IP/端口/用户名/密码、模块 `[{32H, 01}]`，读应答 21 B 的 ErrCode=00；
3. **断电重启** → 0BH 回读一致；MQTT 在新 broker/新主题上连接；30 s 状态、5 min 签到可见；
4. **改 IP** → 0AH 应答成功 → 断电重启 → 新 IP 上 6001 可用、旧 IP 不可用；
5. **改 ClientPort** → 应答成功，旧端口仍可读 0BH；断电重启后新端口生效；
6. **异常帧**：坏 CRC（不应答）、超长 LEN、截断帧、未知/不支持 DeviceId（ErrCode 01）、
   ModeCnt=0（模块不变）、全 0 的 0AH（配置不变）；
7. **稳定性**：参数口连续读写不重启；拔网线重插后 6001 仍可连；
8. **LDI 回归**：9528 的 LDI 命令、IAP 升级、级联显示不受影响（重点看 `app_tcp_server` 改造）。

---

## 9. 风险与回退

| 风险 | 影响 | 缓解 |
|---|---|---|
| 参数改了但没重启 | 现场以为配错 | 应答只表示“已落盘”；文档/操作流程明确“重启生效”；0BH 回读的是新值，可据此判断“已存但未生效” |
| 配置写坏（IP/端口）后失联 | 现场无法再配 | 断电重启后参数才生效，仍有旧 IP 可回退改；另建议板载按键长按清记录（可选兜底） |
| 0AH 无鉴权 | 同网段任何人可改参数 | 文档本身如此；本期只记来源 IP，后续可加白名单 |
| 密码明文回读 | 抓包可获取 MQTT 口令 | 文档设计如此；内网部署 |
| 10 B 凭据上限 | 长口令配不进去 | 文档字段限制；更长需甲方扩字段 |
| LDI 与 AH 都配 IP | 谁生效不确定（同层链接顺序） | 部署二选一；AH 应用时打印新/旧 IP |
| 主题与平台约定不一致 | 平台收不到数据 | 派生规则集中在 `app_ah.c` 一处；上机先抓包核对 |
| 客户端 ID 重名 | 两台设备互相踢下线 | 按站/道/型/号派生，默认唯一 |
| 31H 业务未实现 | 费显产品不可用 | 本期只交付 32H；31H 命令回失败并日志 |

回退：P0 是纯改名；P1/P2 新增文件可整体排除；P3 只需把 `app_mqtt_start()` 与
`sw_post_initcall` 注释回去即回到“链路未激活”。**AH 全新模块不影响现有 LDI/IAP/CDSTD/RLS/CASC 路径**。

---

## 10. 后续增量（不阻塞）

模块表语义与清屏命令均已定稿，**无阻塞项**：

- 模块表：`ModeCnt` 只接受 0/1，`DeviceId` 必须是本机类型；设备是 32H 时收到 12H/31H 等
  → **整帧静默丢弃**（不应答、不落盘、只记日志）。
- 清屏：保留旧文档的 `ASK/display/clean`（0/1/2/3）。

以下两条是后续增量，不影响 P0 开工：

- **Q8**：参数口是否需要来源 IP 白名单/鉴权（文档没有，本期不做）。
- **Q9**：31H 费显业务载荷什么时候要（本期只做 32H，代码按 `cfg.device_id` 分派预留）。

---

## 11. 工作量

| 阶段 | 内容 | 人日 |
|---|---|---:|
| P0 | 改名 + 双构建清单 + 显示越界修复 | 0.5 |
| P1 | 配置模型/持久化/纯函数 + 2 套测试 | 1.0 |
| P2 | TCP 多实例 + 参数协议（模块表校验/静默丢弃/校时/不碰运行态）+ 1 套测试 | 2.0 |
| P3 | MQTT 接通 + 既有缺陷修复 + 显示类型分派 | 1.0 |
| P4 | 双板/上机/文档 | 1.0 |
| — | 缓冲（联调、甲方确认后调整） | 1.0 |
| **合计** | | **6.5** |
