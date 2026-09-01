# 03 网络协议接入（UDP 与 TCP）

> 通用规则见 `01`。本文讲网口专项：RJ45 共享 RB、UDP/TCP 逻辑通道、LwIP 约束、与串口协议的混合规则。

---

## 1. RJ45 共享 RB 与逻辑通道

- 物理 RB 只有一个：`RB_SLOT_RJ45`，容量 `RB_SIZE_RJ45 (1536U)`（`app_dispatch.h:37`，注释：≥ IAP 最大帧 1044 + 余量）。
- 四个逻辑通道共享该槽（`app_dispatch.h:47-55`）：

| CH_ID | 值 | 传输 | 通道实现 |
|---|---|---|---|
| `CH_ID_TCP_SERVER` | 2 | LwIP TCP 服务器 | `app_tcp_server.c`（默认端口 9528） |
| `CH_ID_TCP_CLIENT` | 3 | LwIP TCP 客户端 | `app_tcp_client.c`（默认 192.168.2.17:9529） |
| `CH_ID_UDP` | 4 | LwIP UDP | `app_udp.c`（端口 10011，创迪发现口 + IAP 共用） |
| `CH_ID_MQTT` | 5 | LwIP MQTT | `app_mqtt.c`（AH 平台，当前未激活） |

- 协议侧提供 RB：`RB_PROVIDE_WEAK(rb_provide_rj45, RB_SIZE_RJ45)`（IAP `app_iap.c:20`、LDI `app_ldi.c:17`）。
- **每逻辑通道独立 mask + bind**（doc/05-01 §2.3）：LDI 一次 `ldi_module_init` 里 register 三次拿三个 mask，分别 bind TCP_SERVER/TCP_CLIENT/UDP（`app_ldi.c:135-157`），三 mask 共用 `g_ldi_msg_queue`。**禁止单 mask 绑多通道**。
- 风险门禁（doc/05-01 §2.4）：TCP 流 / UDP 报可能在同一 RJ45 RB 交错排列。以单网口业务为主时维持共享 1536；若 TCP 与 UDP 同时高压收发，评估拆逻辑 RB——新协议接入先按共享设计，超出预算再提案。

## 2. UDP 范例

### 2.1 IAP 升级协议（0x5A5A5A5A）

- 帧格式：`0x5A5A5A5A | seq(4B) | cmd(4B) | len(4B) | data | CRC32(4B)`（`app_iap.c:5-7` 注释；`FRAME_HEAD (0x5A5A5A5AU)` 定义于 `app_iap.h:12`；`FRAME_MIN_LEN (5U)`、`FRAME_MAX_LEN (5U+256U)` :9-10）。
- 只 bind `CH_ID_UDP`（`app_iap.c:39-53`）；与 LDI 同槽链式共存：首字节 0x5A vs 0xFF 快拒互斥。
- probe（`app_iap.c:96-155`）要点：首字节 `0x5A` 快拒 → 4B 帧头比对 `FRAME_HEAD` → len 合法性 ≤256 → 数据不足时**二次帧头扫描**（范围内出现另一 `0x5A` 立即 FAKE 重同步，防止伪头粘包）→ CRC32 校验 → READY。
- **广播应答**：`cmd01/cmd02` 回复经 `udp->src_ip` 回传，源 IP 置全 `0xFF`（255.255.255.255 广播）（`app_iap_cmd.c:60-63`）。依赖 `udp_channel_t` 在收包时记录 `src_ip[4]`/`src_port`（`app_udp.c`）。**已知限制（2026-08-21）**：src 快照是通道级单例，帧排队期间后续帧会覆盖快照——IAP 停等一问一答、低流量，竞态不修改，仅记录（见 doc/07 修订）。

### 2.2 LDI UDP 搜索（21H/12H）

- LDI 协议 bind UDP 通道用于「创迪发现口」搜索/应答（`app_udp.c` 注释：同端口 10011 承载 LDI 21H/12H 搜索与 IAP 升级，按帧头分流）。**注意区分两个端口语义**：10011 仅为搜索/上报的 UDP 传输渠道；12H 应答与 IAP 0x01 上报内容中的 `port` 字段是设备「配置功能端口」（TCP 业务口 9528，见 doc/07 §12 ④）。LDI probe 首字节 `0xFF` 快拒 + STX `0xFF 0xFF` + CRC16-XMODEM（`app_ldi.c:313-373`）。
- **12H 应答发送顺序（2026-08-21）**：广播为主（`app_udp_broadcast` 先发，复用常驻 conn 不占 netconn 池）、回源为辅（`channel_send` 后发、失败静默）——回源依赖通道级 src 快照，多协议交错下可能发错目标，广播是可靠路径。

### 2.3 UDP 通道实现要点（新协议发包侧）

- 应答直接 `channel_send(ch, data, len)`：`udp_ch_send` 用 `udp->src_ip` 重建 `ip_addr_t` 发回源地址（`app_udp.c`）——**通道层自动回源，协议层无需管理 IP**。
- 通道生命周期：`udp_task` 建 netconn/bind/派生 `udp_connect_task`；断链时 `udp_channel_deinit` 置 `ops=nullptr`、`state=DOWN`、`conn=NULL`（2026-08-21 补）、`app_channel_register(CH_ID_UDP, nullptr)`。协议层不要持有裸 `channel_t *` 跨断链使用——`channel_send` 对 `ops==nullptr` 有守卫（`app_dispatch.c`），但状态判断应像 LDI 那样经 `app_channel_get(CH_ID_*)` 每次现查。

## 3. TCP 范例（LDI 双通道）

- LDI 同时注册 TCP Server + TCP Client + UDP 三 mask（`app_ldi.c:140-156`）。
- **通道状态**：`channel_t.state`（`CH_STATE_UP/DOWN`，`app_dispatch.h:59-61`）。TCP 连接建立时 `tcp_channel_init` 置 `CH_STATE_UP` + `app_channel_register`（`app_tcp_server.c:92-98`、`app_tcp_client.c:57-64`）；断开时 conn 任务置 `ops=nullptr`、`state=DOWN`、`app_channel_register(..., nullptr)`（`app_tcp_server.c:158-161`、`app_tcp_client.c:163-166`）。
- **断链状态机复位先例**：`ldi_timer_task` 每秒检查 `ch == nullptr || ch->state != CH_STATE_UP` → `g_ldi.state = LDI_ST_UNINIT`（`app_ldi.c:406-419`），断链后认证/上报状态自动回退。新协议若有会话状态机，必须设计等价的断链复位路径。
- TCP 客户端非阻塞 connect + 4s 轮询（`app_tcp_client.c:87-112`），keepalive idle 10s / intvl 2s / cnt 3（:148-153）——新协议用 TCP Client 模式可直接复用通道，不改 `app_tcp_client.c`。
- 若新协议需要新端口/新远端：用 `app_tcp_server_set_port()` / `app_tcp_client_set_remote(ip, port)`（后者触发断链信号量重连，`app_tcp_client.c:48-53`）；两个通道实例是单例，**多远端需求需要评估扩通道，不在接入范围内**。

## 4. LwIP 约束

- **广播依赖**：`lwipopts.h` 中 `LWIP_BROADCAST` 未定义，但 `IP_SOF_BROADCAST=1` + `IP_SOF_BROADCAST_RECV=1` 已生效（IAP 升级依赖，见 doc/CLAUDE.md「LwIP 配置要点」）；`MEMP_NUM_NETCONN=8`/`MEMP_NUM_UDP_PCB=8`（2026-08-21 覆盖，修复池满广播丢包）。代码侧广播：`udp_task`/`udp_cq_task` 建常驻 conn 时 `ip_set_option(conn->pcb.udp, SOF_BROADCAST)`；`app_udp_broadcast`/`app_udp_cq_broadcast` **复用常驻通道 conn**（`app_channel_get` 回验 + conn 非空）`netconn_sendto` 255.255.255.255，常驻未就绪回退临时 conn。新协议需要广播时调用 `app_udp_broadcast`（10011 口）或 `app_udp_cq_broadcast`（20103 口），勿自行 `netconn_new` 临时 conn。
- **`pl_net_adapt.h` 禁止在任何 .h 中引用**（doc/CLAUDE.md 架构约束）：LwIP 类型（`struct netconn`、`ip_addr_t` 等）不得泄漏进协议头文件。协议 .c 需要 LwIP API 时只 include `pl_net.h`/`pl_net_adapt.h` 于 .c 内。
- **IP 类型隔离先例**：`udp_channel_t.src_ip` 用 `uint8_t[4]` 字节数组而非 `ip_addr_t`（`app_udp.c:160-164`），避免 Application 层暴露 middleware 类型。新协议存储对端地址照此办理。

## 5. 网络协议与串口协议混合规则

- **按槽绑定**：协议绑哪个 CH_ID 就吃哪个物理口的数据（RJ45 槽 / RS485 槽 / RS232 槽互不相通）。同一协议可同时绑网口和串口（如青海绑 RS485+RS232，LDI 绑三网口逻辑通道）——每通道一个 mask，probe 对 `ch` 参数通常不区分（除应答回源）。
- **同槽链式 probe**：RJ45 上 IAP/LDI 靠首字节（0x5A vs 0xFF）共存；RS485 上多地区协议靠帧头/第二字节共存（见 `02 §4`）。跨槽协议永远不存在 probe 竞争。
- **禁绑**：`CH_ID_RS232_1` 对任何协议（含网络协议）禁绑。
- 混合部署时注意 queue 深度与 payload 上限按**本协议绑定通道的最小 RB** 设计（如同时绑 RS485+RS232 时按 768 约束，绑 RJ45 时按 1536 约束）。