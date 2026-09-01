# 07 — LDI 与 IAP 配置耦合 / 解耦

> **范围**：EIDE/目录选编时「只编 LDI、不编 IAP」导致链接失败的根因、影响面，以及解耦方案。  
> **状态**：问题 **已确认**（2026-08-14）；三处缺陷 **已定案并先行修复**（2026-08-14，见 [02 §9](./02_decoupling_solutions.md)）；两项补充修复（守卫细化 + 4B02 结果码）**已落地**（见 [02 §10](./02_decoupling_solutions.md)）；**方案 A 主体重构（抽出板级网络配置模块 `app_board_net_cfg`）已落地（2026-08-14）**。  
> **非本目录**：多协议 RB/绑定 → [`../05_协议模块多协议兼容优化/`](../05_协议模块多协议兼容优化/README.md)；SRAM/CCM → [`../06_SRAM内部分数据迁移/`](../06_SRAM内部分数据迁移/README.md)。  
> **备注（2026-08-14）**：`doc/CLAUDE.md` 中「当前固件入口 0x08000000、无 IAP bootloader 运行」已过时——链接脚本 `Compiler/STM32F407XX_FLASH.ld:67` 现为 `ORIGIN=0x08040000, LENGTH=768K`，`Core/Src/main.c:15` 已设 `SCB->VTOR = FLASH_BASE|0x40000`（主固件已按 IAP 适配入口链接）；本目录 01/02/03 均按此现状撰写。

---

## 文档结构

| 文件 | 角色 |
|------|------|
| [01_background_and_impact.md](./01_background_and_impact.md) | **背景、根因、影响面** |
| [02_decoupling_solutions.md](./02_decoupling_solutions.md) | **解耦方案**（推荐路径 + 备选 + 验收） |
| [03_interim_build_rules.md](./03_interim_build_rules.md) | **解耦前**工程选编临时规则 |

## 快速结论

| 问 | 答 |
|----|-----|
| 只加 LDI 为何链接失败？ | LDI **硬依赖** `app_iap_cfg`（Sector1 网络配置 API），不是依赖 IAP **升级协议** |
| 是否必须加整包 IAP？ | **否**。最小要 `app_iap_cfg.c`；`app_iap.c` / `app_iap_cmd.c` 可裁 |
| 长期怎么解？ | **已落地（2026-08-14）**：抽出中立模块 `app_board_net_cfg`（`Application/Config`），LDI 与 IAP 协议都依赖它；`app_iap_cfg.{c,h}` 与死代码 `Device/Inc/config_info.h` 已删除 |
| 改码纪律 | 方案在 `02`；**审核通过后才改代码**（三处缺陷修复已定案落地，见 [02 §9](./02_decoupling_solutions.md)） |
| 出厂新机 0AH 改 IP，Boot 为何读不到？ | 现状缺陷已修复（2026-08-14）：空 Sector1 写路径不置 magic（[01 §3.4](./01_background_and_impact.md)）→ 现按 Bootloader 语义完整初始化；0AH 写失败如实应答 01H，上位机重发恢复 |
| 三固件出厂默认网络配置 | 统一为 192.168.114.200/24、网关 192.168.114.1、端口 9528；Recovery 上电读 Sector1.net_cfg 配 netif、主固件 W25 空 + Sector1 有效时同步 set_ip（见 [02 §11](./02_decoupling_solutions.md)） |
| 网络配置真源与镜像 | Sector1.net_cfg 为**真源**（三固件共享）；W25 网络字段为**恢复镜像**（擦 Sector1 后还原用户配置）；主固件上电 Sector1 优先、W25 自愈回写、皆空用出厂默认（见 [02 §12](./02_decoupling_solutions.md)） |
| W25 镜像自愈 | 主固件上电分支 1 检测 Sector1 与 W25 网络字段不一致即刷新镜像（覆盖 4B02 单写 Sector1 或单次 W25 写失败的陈旧，见 [02 §12](./02_decoupling_solutions.md)） |
| Bootloader 损坏态自愈 | magic 不匹配或 config_crc 错 → 条件 C 重建出厂记录自愈；App 损坏仍 Recovery；Recovery 判空保持两哨兵（防死循环进 Recovery 的分工，见 [02 §12](./02_decoupling_solutions.md)） |
| 搜索/上报端口字段语义 | LDI 12H 应答与 IAP 0x01 上报的 `port` 字段 = 配置功能端口（TCP 业务口，出厂默认 9528，宏 `LDI_DEFAULT_CONFIG_PORT`）；UDP 10011（宏 `LDI_DISCOVERY_PORT`）仅为搜索/上报的**传输渠道**，勿混淆（2026-08-18 修复 12H 误报 10011 污染循环，见 [02 §12 ④](./02_decoupling_solutions.md)） |
| LDI 12H 应答发送路径 | **广播为主、回源为辅**（2026-08-21）：先 `app_udp_broadcast` 再 `channel_send` 回源（失败静默）；广播复用常驻通道 conn（`lwipopts.h` `MEMP_NUM_NETCONN=8`/`MEMP_NUM_UDP_PCB=8` 扩池根治池满丢包）。**已知限制**：回源依赖通道级 src 快照，多协议交错下可能发错目标；IAP 回源同源竞态不修改（见 [02 修订 2026-08-21 五](./02_decoupling_solutions.md)） |
| 配置生效时机 | 所有改 IP 接口（LDI 0AH / IAP 4B02 / Recovery IAP）统一**重启生效**（既定策略，非缺陷；4B02 已删除即时应用段） |
| UDP 发现口 | 10011 固定（宏 `LDI_DISCOVERY_PORT`），禁止提供修改接口 |
| IAP 0x03 版本号来源 | 主固件启动时把 `PROGRAM_CODE` 落库 Sector1 `app_info.version`（`app_board_net_cfg_fw_version_update`，见 [02 §13](./02_decoupling_solutions.md)）——此前无人写入恒报全 0 |

## 与 05 的边界

| | 05 多协议 | 07 本专题 |
|--|-----------|-----------|
| 主题 | 谁绑哪个通道 / RB | **配置存储归属**与编译依赖 |
| 协议帧 | LDI / IAP 可同 RJ45 链式 | 即使不编 IAP 协议，配置层仍可能被 LDI 引用 |

阅读顺序：`01` → `03`（临时规避）→ `02`（正式解耦）。

---

## 修订

- 2026-08-14：首版，与 01/02/03 同步。
- 2026-08-14（同日修订）：补充固件入口现状备注（doc/CLAUDE.md 矛盾点）、出厂新机 0AH 缺陷入口。
- 2026-08-14（同日再修订）：状态改为「缺陷已定案并先行修复、主体重构暂缓」；快速结论同步 [02 §9](./02_decoupling_solutions.md) 三决策。
- 2026-08-14（同日三修订）：状态补两项补充修复（守卫细化 + 4B02 结果码，见 [02 §10](./02_decoupling_solutions.md)）。
- 2026-08-14（同日四修订）：**方案 A 落地**——抽出 `app_board_net_cfg`（`Application/Config`，Sector1 布局唯一归属 + static_assert 锁定），LDI/IAP 迁移到 `app_board_net_cfg_get/update/read`，删除 `app_iap_cfg.{c,h}` 与 `Device/Inc/config_info.h`；[03](./03_interim_build_rules.md) 选编临时规则废止。
- 2026-08-18：新增 [02 §11](./02_decoupling_solutions.md) 三固件出厂默认网络配置统一约定（192.168.114.200/24、网关 192.168.114.1、端口 9528）；快速结论同步三项一致性修复现状。
- 2026-08-18（同日再修订）：同步 [02 §12](./02_decoupling_solutions.md) 网络配置真源与生效语义——Sector1 真源/W25 恢复镜像、0AH/4B02 重启生效（既定策略）、UDP 发现口固定宏 `LDI_DISCOVERY_PORT`（删除 `app_udp_set_port`）。
- 2026-08-18（同日三修订）：快速结论同步 [02 §12](./02_decoupling_solutions.md) 缺陷 A/C/D/E 修复——W25 镜像自愈、Bootloader 损坏态重建（magic/CRC 错 → 条件 C 重建出厂记录；Recovery 判空保持两哨兵防死循环）、所有改 IP 接口（0AH/4B02/Recovery IAP）统一重启生效。
- 2026-08-18（12H 端口字段语义修复）：12H 应答 port 由误用发现口 10011 改为配置功能端口（device_port / 默认 9528），修复 10011 污染循环（12H 报 10011 → 上位机写回 → TCP Server 错监听 10011）；新增宏 `LDI_DEFAULT_CONFIG_PORT`；存量污染设备需 0AH 重设 9528 或擦 Sector1（见 [02 §12 ④](./02_decoupling_solutions.md)）。
- 2026-08-18（同日四修订）：新增 [02 §13](./02_decoupling_solutions.md) 固件版本落库——IAP 0x03 上报的 version 由主固件启动时从 PROGRAM_CODE 写入 Sector1（空/损坏自愈、升级中间态拒绝、同值跳过）。
- 2026-08-21：新增 [02 §14](./02_decoupling_solutions.md) 网络配置应用横切解耦——中立模块 `app_net_boot`（`app_net_boot_apply`）统一承担「Sector1 net_cfg → netif + TCP Server 口」应用，`ldi_ctx_init` 三分支与 CQ `_cq_net_cfg_apply` 不再直接改 netif；accept_write 两口径统一（空/损坏/非法写本构建默认落盘并应用：CQ 192.168.1.5/20103、dev 192.168.114.200/9528）；CQ 构建不启动 TCP Server/Client 通道。同步 [02 §12](./02_decoupling_solutions.md) 分支 1 条件修订（`update_sta` 不参与分支判定）。
- 2026-08-21（同日再修订）：**LDI 搜索应答丢包修复**——根因 netconn 池满（baseline 4 已满）+ 广播临时 conn 必然 NULL；修复：`lwipopts.h` 扩池（`MEMP_NUM_NETCONN=8`、`MEMP_NUM_UDP_PCB=8`，bss +304B）+ `app_udp_broadcast`/`app_udp_cq_broadcast` 复用常驻通道 conn + LDI 12H 应答「广播为主、回源容错」。已知限制：回源 src 快照竞态（LDI 12H 与 IAP 同源，本轮不架构改造）。
- 2026-08-24：**TCP 口应用两口径化**——修复「LDI 改端口后搜索上报新值、TCP 仍监听 9528」现场 bug：EIDE Debug 混合口径（LDI 编入 + PROTO_CHONGQING）下 `app_net_boot_apply` 的 `app_tcp_server_set_port` 被宏裁剪、TCP Server 恒绑 9528；修复后 TCP 口两口径统一应用 Sector1 `net_cfg.port`（[02 §14](./02_decoupling_solutions.md) 同步，CQ setip 语义不变）；「CQ 构建不启动 TCP Server/Client 通道」表述更正为两口径均启动。
