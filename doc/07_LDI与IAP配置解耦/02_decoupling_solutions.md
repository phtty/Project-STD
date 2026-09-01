# 02 解耦方案

> **状态**：三处缺陷**已定案并先行修复**（2026-08-14，见 §9）；**方案 A 主体重构已落地（2026-08-14）**——抽出板级网络配置模块 `app_board_net_cfg`（`Application/Config`），LDI/IAP 迁移至 `app_board_net_cfg_*` API，`app_iap_cfg.{c,h}` 与 `Device/Inc/config_info.h` 已删除　|　2026-08-14  
> **前提**：已读 [01_background_and_impact.md](./01_background_and_impact.md)  
> **纪律**：本文件仅方案；**未经授权不改代码**（§9 已获授权的先行修复除外）。

---

## §0 目标与非目标

### 目标

| ID | 目标 |
|----|------|
| D-1 | EIDE/Makefile **只编 LDI、不编 IAP 升级协议** 时链接成功 |
| D-2 | 仍可在需要时编入完整 IAP；Sector1 网络配置语义可保留或可显式关闭 |
| D-3 | LDI 源文件 **不再** `#include "app_iap_cfg.h"` / 直调 `app_flash_iap_*` |
| D-4 | Boot/Recovery 与主固件对 `0x08004000` 布局的兼容策略有文档与代码单一归属 |

### 非目标

- 不改 LDI 帧格式、probe、RB 绑定（属 doc/05）  
- 不强制取消 Sector1 存 IP（产品可保留，只改归属模块名）  
- 不在本方案内重做整板配置中心（可分阶段）

---

## §1 方案对比

| 方案 | 摘要 | 满足 D-1 | 工作量 | 推荐 |
|------|------|----------|--------|------|
| **A. 抽出板级网络配置模块** | `app_iap_cfg` → `app_board_net_cfg`（或 `Device/Config`），LDI/IAP 协议都依赖它 | ✅ | 中 | **首选** |
| **B. 弱符号 + 空实现** | `app_flash_iap_*` 改为 weak；无 IAP 配置 TU 时空操作 | ✅ | 小 | 过渡可接受 |
| **C. 接口注入 / 函数指针** | LDI 只依赖 `ldi_net_persist_ops`；默认挂 IAP cfg 或 stub | ✅ | 中 | 与 A 可合并 |
| **D. LDI 彻底不碰 Sector1** | 删同步调用；IP 只活在 W25 + 运行时 LwIP | ✅ | 小 | 需产品确认 Boot 行为 |
| **E. 仅文档规定必带 cfg** | 不改代码，选编规则强制带 `app_iap_cfg.c` | ⚠️ 治标 | 极小 | 见 [03](./03_interim_build_rules.md)，非终态 |

---

## §2 方案 A（推荐）：抽出「板级网络配置」

### 2.1 目标结构

```
Application/Src/Config/   或  Device/Config/
  app_board_net_cfg.c/.h     ← 原 app_iap_cfg 中与 Sector1/net 相关的 API
Application/Src/IAP/
  app_iap.c / app_iap_cmd.c  ← 只依赖 board_net_cfg + 升级逻辑
Application/Src/LDI/
  app_ldi.c / app_ldi_cmd.c  ← 只依赖 board_net_cfg（或经 ops）
```

命名示例（落地时可调整，但勿再叫「iap」若模块已中立）：

```c
/* app_board_net_cfg.h */
bool board_net_cfg_valid(void);
bool board_net_cfg_get(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4]);
int  board_net_cfg_set(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4]);
```

内部仍可读写 `0x08004000` 的既有布局（**二进制兼容 Boot/Recovery**），或在同文件内保留 `app_flash_iap_sys_info_t` 别名。

### 2.2 机械步骤（审核通过后执行）

0. **改造前盘点（完整调用点清单）**：
   - 全仓 `ADDR_CONFIG_SECTOR` 使用点 **7 处**：`app_iap_cmd.c:94/110/143`（裸转换 `(app_flash_iap_sys_info_t *)ADDR_CONFIG_SECTOR` 直接读配置）+ `app_iap_cfg.c:23/39/111`（+ 第 10 行注释）。另有两处 `#define`（`app_iap_cfg.h:8` 与死代码 `config_info.h:6`）。
   - LDI 侧 `app_flash_iap_*` 调用 **4 处**：`app_ldi.c:100/104/108`（上电同步与回退读）+ `app_ldi_cmd.c:431`（0AH）。
   - Sector1 结构体 **4 处独立定义**：主固件 `Application/Inc/IAP/app_iap_cfg.h`、`Device/Inc/config_info.h`（死代码，建议删除）、Bootloader 与 Recovery 各自的 `Drivers/BSP/Config/config_info.h`。无共享头、无布局锁定。
1. 新建 `app_board_net_cfg.{c,h}`，从 `app_iap_cfg` **迁出**（或整体改名 + 更新引用）。  
2. `app_iap_cfg.h` 可薄包装 `#include "app_board_net_cfg.h"` + deprecated 别名（过渡一期）。  
3. LDI：`#include "app_board_net_cfg.h"`，替换 **4 处**调用（`app_ldi.c:100/104/108`、`app_ldi_cmd.c:431`）为 `board_net_cfg_*`。  
4. IAP 协议：`app_iap_cmd.c:94/110/143` 三处裸读改经 `board_net_cfg` API，消除裸转换；升级状态 / FWInfo 继续用同一头文件（若 FWInfo 仍在同结构，可同模块或再拆 `app_board_fw_meta`）。  
5. **布局锁定**：在 `app_board_net_cfg.h`（唯一归属）用 `static_assert` 锁死 `sizeof` / `offsetof` 与 magic 值，保证 0x08004000 布局与 Boot/Recovery **二进制兼容**；同步收敛/删除其余 3 处独立定义（`Device/Inc/config_info.h` 死代码直接删）。  
6. **顺带修复（已定案并先行落地，见 §9）**：空 Sector1 写路径补 magic 初始化（[01 §3.4](./01_background_and_impact.md)）；写/擦除返回值上抛与失败处理（§8）。  
7. EIDE/Makefile：`Config` 或 `board_net_cfg` 列为 **公共必选**；`IAP/` 协议目录可选。  
8. 更新本目录 README 状态 → 已落地；补回归清单。

### 2.3 验收（DoD）

- [ ] 工程 **exclude 全部** `app_iap.c` / `app_iap_cmd.c`，保留 `app_board_net_cfg.c` + LDI → **链接成功**  
- [ ] 工程 **exclude LDI**，保留 IAP 协议 + board_net_cfg → 链接成功  
- [ ] LDI 目录内 `rg app_iap_cfg` / `rg app_flash_iap` 无匹配（或仅测试桩）  
- [ ] **全仓** `rg ADDR_CONFIG_SECTOR`：使用点由现状 7 处收敛到唯一归属模块内；`app_iap_cmd.c` 无 `(app_flash_iap_sys_info_t *)ADDR_CONFIG_SECTOR` 裸转换  
- [ ] `rg config_info.h`：主固件死代码（`Device/Inc/config_info.h`）已删除；Sector1 布局定义收敛为 1 处 + static_assert 锁定  
- [ ] `0AH` 改 IP 后 Sector1 `net_cfg` 与 W25 一致（若保留同步语义）；**出厂新机（空 Sector1）0AH 后 Sector1 记录有效**（magic 修复生效）  
- [ ] 上电：W25 空 / Sector1 有值 / 皆空 三条路径行为与现网一致或有产品签字差异说明  
- [ ] 失败注入：擦除/写入失败或中途断电后上电，行为符合 §8 声明的掉电语义  

---

## §3 方案 B：弱符号空实现（低成本过渡）

在公共层提供：

```c
__attribute__((weak)) bool app_flash_iap_is_config_valid(...) { return false; }
__attribute__((weak)) void app_flash_iap_update_net_cfg(...) { (void)ip;(void)mask;(void)gw; }
app_flash_iap_sys_info_t *g_config __attribute__((weak));
```

强符号仍由 `app_iap_cfg.c` 提供。

| 优点 | 缺点 |
|------|------|
| 改动小，立刻可「只编 LDI」 | LDI 仍 `#include app_iap_cfg.h`，目录边界未清 |
| | 无 cfg 时静默放弃 Sector1 同步，易忽略产品风险 |
| | `g_config` weak 空指针需所有路径判空 |

建议：仅作 **过渡**，终态仍走方案 A。

---

## §4 方案 C：LDI 持久化 ops 注入

```c
typedef struct {
    bool (*net_valid)(void);
    void (*net_load)(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4]);
    void (*net_store)(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4]);
} ldi_net_bridge_ops_t;

void ldi_set_net_bridge(const ldi_net_bridge_ops_t *ops); /* nullptr = 不桥接 Sector1 */
```

- 默认产品固件：在 `app_boot` 或 IAP cfg initcall 里注入真实 ops。  
- 纯 LDI 演示固件：不注入 → 只走 W25 / `pl_net_get_ip`。  

可与方案 A 组合：ops 的默认实现放在 `app_board_net_cfg`。

---

## §5 方案 D：取消 Sector1 同步（产品决策）

删除 LDI 中所有 `app_flash_iap_*` 调用，IP 仅：

- W25 LDI 记录  
- 运行时 `pl_net_*`  

| 条件 | 说明 |
|------|------|
| 必须产品确认 | Boot/Recovery **不再**依赖 Sector1 的 net_cfg，或另有配置源 |
| 风险 | 出厂只配 LDI、未写 Sector1 时，升级/恢复流程 IP 不一致 |

若产品确认「主业务只认 W25」，D 最干净；否则不要单独选 D。

---

## §6 推荐落地路径

```
短期（不改架构，历史） → 执行 03 选编规则（必带 app_iap_cfg.c，已废止）
中期（推荐）         → 方案 A 抽 board_net_cfg；可选加方案 C 注入
可选加速过渡         → 方案 B weak stub（同时排期 A）
产品放弃 Sector1 IP  → 方案 D + 更新 Boot/Recovery 文档
```

> **2026-08-14 现状**：三处缺陷已按 §9 定案**先行修复**；**方案 A 主体重构已落地**（`app_board_net_cfg` @ `Application/Config`，DoD §2.3 全项通过），选编规则 [03](./03_interim_build_rules.md) 已废止、仅存历史。

---

## §7 风险与回滚

| 风险 | 缓解 |
|------|------|
| 搬文件漏改 EIDE/Makefile | 以「只 LDI / 只 IAP」两套链接为 CI 门禁 |
| 结构体搬迁破坏 Boot | **保持** `0x08004000` 布局与 CRC 算法不变；只改源码归属 |
| 擦写 Flash 时机 | 维持现逻辑：避免无配置上电路径 erase |
| 0AH 写 Sector1 断电 | 记录 CRC 失效 → Boot/Recovery 回退默认 IP；掉电语义见 §8，解耦不改变既有回退顺序 |
| Sector1 擦写寿命 | 每次更新均为 16KB 全扇区擦除（约 10k 次寿命），运行中停顿 1~2s；0AH 属出厂低频操作，产品确认见 §8 |
| Bootloader 条件 D `app_info` CRC 校验 × 烧录器重烧主固件 → 永远进 Recovery | Recovery 升级过一次后（`app_info` 为旧固件真实值），用 J-Link/EIDE 直接重烧主固件而不更新 Sector1 → 条件 D 校验失配 → 判 App 损坏 → 设备永远进 Recovery。`tool/flash_all.sh` 默认烧后擦 Sector1 恢复出厂态；手动烧录/单固件烧录遵守同纪律；net_cfg 由主固件上电从 W25 同步回写 |

回滚：恢复 `app_iap_cfg` 路径与 LDI include 即可。

---

## §8 失败场景与掉电语义

### 8.1 现状（解耦前）

| 环节 | 现状行为 |
|------|----------|
| `app_flash_iap_update_net_cfg`（`app_iap_cfg.c:108-122`） | 擦除/写入返回码**均被忽略**（`app_flash_iap_edit_config` 内忽略 erase/write 返回），无重试、无回读校验、无上报 |
| LDI save（`app_flash_ldi_save_config`，`app_ldi_cfg.c`） | 同样忽略 W25 写入返回；SPI busy / 擦除失败静默 |
| 0AH 命令（`app_ldi_cmd.c:430-431`） | 先写 W25 再写 Sector1；任一步失败仍回 0x00 成功响应 |
| Sector1 擦写 | 每次更新都是 **16KB 全扇区擦除**：运行中（0AH 处理）停总线 **1~2s**；擦写寿命约 **10k 次**。0AH 属出厂低频操作风险可控，但高频改 IP 场景需产品论证 |

### 8.2 掉电语义（现状声明）

- **W25 写入中断电**：W25 CRC 失效 → 主固件回退 Sector1；Sector1 无有效记录（含出厂新机）→ 回退默认 IP。
- **Sector1 写入中断电**：Sector1 CRC 失效 → Bootloader/Recovery 回退默认 IP；主固件走 W25（W25 先写且已成功）。**自愈**（§10 方案 1 落地后）：下次 0AH / 上电同步时，损坏记录按「完整初始化」分支覆盖为有效记录，无需返修。
- **出厂新机（Sector1 空）**：受 [01 §3.4](./01_background_and_impact.md) 缺陷影响，0AH 后 Sector1 仍无有效记录 → Bootloader/Recovery 恒回退默认 IP。
- 归纳：**Sector1 无有效记录时，三固件各自回退默认 IP**；这是现网行为，解耦不得静默改变。

### 8.3 方案 A 要求

- `board_net_cfg_set` 必须返回错误码；LDI 0AH 判错并向上位机返回失败/告警（不再静默回 0x00）。
- 写后回读校验；失败时保留内存中的新配置、记录错误状态。
- 失败后是否重试：**已定案（§9 Q2）**——固件不自动重试，0AH 应答 ErrCode=01H，由上位机重发 0AH 恢复。
- 掉电语义显式写入本目录 01/03 及 Boot/Recovery 侧文档，保持「W25 → Sector1 → 默认」回退顺序不变。
- 空 Sector1 写路径的 magic 初始化修复（[01 §3.4](./01_background_and_impact.md)）**已按 §9 Q1 落地**。

---

## §9 先行修复定案（2026-08-14，已落地）

> **范围声明**：本次只做下述三个缺陷修复 + 文档落实；**07 解耦主体重构（方案 A 抽板级网络配置模块）暂缓**。
> 涉及文件：`Application/Src/IAP/app_iap_cfg.c`、`Application/Src/IAP/app_iap_cmd.c`、
> `Application/Src/LDI/app_ldi_cmd.c`、`Application/Src/LDI/app_ldi.c`（+ `Application/Inc/IAP/app_iap_cfg.h` 接口声明）。

### Q1（定案：修，镜像 Bootloader 语义）

`app_flash_iap_update_net_cfg` 空配置分支改为**完整初始化**，使写出的记录与老 Bootloader
`Init_Config_Info`（`STM32F407-Bootloader-master/Drivers/BSP/Config/config_info.c`）首次上电
自写内容**逐字节同构**：

| 字段 | 值 |
|------|-----|
| magic | `0x0d000721`（CONFIG_MAGIC） |
| update_sta | `updated = 0` |
| app_info | 全 0，其中 `crc32 = 0xFFFFFFFF`（size=0、version 全 0；version 由启动落库 API 填写，见 §13） |
| net_cfg | 调用方传入（ip/mask/gw + **port**，0AH 把 device_port 传入） |
| config_crc | 硬件 CRC32 覆盖 `sizeof - 4`（前 64B） |

- 4B02「强制修改IP」处理器改走同一修复后的函数。
- **禁止复用** `app_flash_iap_init_config`（其 update_sta=FAILED，会被 Bootloader 判 App 损坏）。

### Q2（定案：最小修复，如实应答）

- `cmd_set_ip`（0AH）捕获 **W25 写入**与 **Sector1 同步**两个返回值，任一失败则应答
  ErrCode=01H（00H=成功，01H=失败，与 ldi_status_rsp_t / 0BH 既有约定一致）；上位机重发 0AH 恢复。
  **固件侧不做自动重试。**
- 上电同步钩子（`ldi_ctx_init`）扩展：Sector1 **空/无效**时也走 `app_flash_iap_update_net_cfg`
  用 W25 配置初始化 Sector1（空扇区走 Q1 完整初始化；损坏记录仍被守卫拒绝覆盖，保护升级中间态）。
  修复 Q1 后该钩子自然覆盖「0AH 写中断电 → Sector1 空/擦除残留」场景。
  **（2026-08-14 细化：损坏记录改为自愈覆盖，见 §10 方案 1，本条「损坏拒绝覆盖」表述已被取代）**

### Q3（定案：接受现状 + 跳过同值写）

`app_flash_iap_update_net_cfg` 写前比较 net_cfg（**含 port**）与现扇区内容，一致则跳过擦写返回成功。
**不做磨损均衡**。16KB 全扇区擦除的现状（单次 1~2s 停顿、约 10k 次寿命）维持不变。

### 修复后的全流程走查（空扇区 + 0AH）

1. 出厂新机：Sector1 空、W25 空 → Bootloader 条件 C：`Init_Config_Info` 写默认 IP，Main App 存在则跳 Main。
2. 上位机下发 0AH：主固件先写 W25，再经 `app_flash_iap_update_net_cfg` 空分支完整初始化 Sector1
   （magic + updated + app_info 全 0/crc32=0xFFFFFFFF + 新 net_cfg + CRC32）→ 应答 00H。
3. 复位：Bootloader 条件 D：magic/CRC 通过、update_sta=updated、app CRC 对 0 字长结果为 0xFFFFFFFF
   与 app_info.crc32 相等 → `Is_App_Exist(ADDR_MAIN_APP)` 通过则跳 Main App（与 Bootloader 自己初始化的记录行为一致）。
4. Main App 上电：W25 有效 → `ldi_ctx_init` 调 `app_flash_iap_update_net_cfg`，net_cfg 已一致 → 跳过擦写。
5. 若 Main App 不存在：Bootloader 跳 Recovery；Recovery 的 `Is_Config_Empty`（magic 有效）为假，
   不再重置配置，读到的 net_cfg 即 0AH 下发值。
6. 0AH 任一步失败：应答 01H，上位机重发 0AH 恢复。升级中间态记录（valid 且 update_sta≠updated）
   不被覆盖；损坏记录在成功路径由「完整初始化」分支自愈覆盖（见 §10 方案 1）。

---

## §10 补充修复定案（2026-08-14，已落地）

> **范围声明**：§9 三缺陷修复后追加的两项修正——细化守卫语义 + 4B02 应答扩展结果码。
> 涉及文件：`Application/Src/IAP/app_iap_cfg.c`、`Application/Src/IAP/app_iap_cmd.c`
> （+ `Application/Inc/IAP/app_iap_cfg.h`、`Application/Src/LDI/app_ldi.c` 注释同步）。

### 10.1 方案 1（细化守卫：升级中间态仅放行 net_cfg 更新 vs 损坏可覆盖）

`app_flash_iap_update_net_cfg` 原守卫 `!empty && !valid → return -1` 把「损坏」与「升级中间态」一起拒绝；
细化后按状态分四支：

| Sector1 状态 | 判定 | 处理 |
|--------------|------|------|
| 空 | magic==0xFFFFFFFF 且 config_crc==0xFFFFFFFF | 完整初始化（镜像 Bootloader 语义）后写 |
| 损坏（magic 无效或 CRC 错） | !empty 且 !valid | **完整初始化后写（覆盖自愈）** |
| 有效 + update_sta==updated | valid 且 updated | 仅更新 net_cfg；同值跳过擦写 |
| 升级中间态 | valid 且 update_sta≠updated | **仅更新 net_cfg；update_sta/app_info 原样保留**（2026-08-18 修订） |

- **升级中间态判定独立于损坏判定**：Bootloader 条件 A/B（强制升级/崩溃过频）写入的记录
  magic/CRC 均正确，属「valid 且 update_sta=failed」；中间态仅放行 net_cfg 字段更新、
  update_sta/app_info 原样保留（2026-08-18 修订）；损坏记录（magic 无效/CRC 错）不可能是
  升级中间态——Bootloader 写记录永远是完整结构（整扇区擦→完整写）。
- **损坏覆盖不会误伤升级流程**：结论为否。损坏判定与中间态判定互斥（一个看 magic/CRC，
  一个看 valid 后的 update_sta），覆盖损坏不会触碰任何有效中间态记录。
- **需重新评估的窗口**：若未来 Bootloader 引入「写一半中断」的新写入模式（中断恢复后不
  保证完成记录），出现 magic 有效但 CRC 错的半成品时，损坏覆盖语义需重新评估——届时须把
  该状态识别为中间态并保护。当前 Bootloader 无此模式，评估结论成立。
- **同值跳过条件 `!empty` → `valid` 的原因**：空/损坏记录现已进入完整初始化分支且必须写；
  若仍按 `!empty` 判定，损坏记录快照（`old_cfg` 为垃圾）碰巧与新 net_cfg 一致时会错误跳过
  写入，导致损坏记录永不自愈。`valid` 下 `old_cfg` 才有比较意义。
- **附带价值**：0AH 自愈——Sector1 写中断电/历史损坏后，下一次 0AH 或上电同步即恢复有效记录
  （§8.2 已同步）。

### 10.2 方案 2（4B02 应答扩展结果码）

`cmd_ForceModifyIP_02` 捕获 `app_flash_iap_update_net_cfg` 返回值，应答帧由 **len 0 → 1**：

| 载荷（1 word） | 语义 | 来源 |
|----------------|------|------|
| 0x00000000 | 成功（含同值跳过） | ret ≥ 0 |
| 0x00000001 | 失败（擦写错误） | ret < 0 |

参照 4B04「准备升级」应答 0/1 结果码先例。**具体可能导致的问题**：

1. **老上位机按「B402 无载荷」解析**：多读到的 1 个 word 是被忽略还是报错（长度/CRC 校验），
   取决于上位机解析实现——若按 len 字段解析则兼容，若按固定长度或严格 CRC 覆盖校验则可能
   报错。**上线前须与上位机联调验证**。
2. **新固件 + 老上位机混合部署期**：升级窗口期内老上位机可能解析异常；建议上位机按 len 字段
   容忍解析，或统一固件/上位机同批升级。
3. **结果码的实际价值**：4B02 失败率本身极低（擦写错误罕见；升级中间态自 2026-08-18 修订
   起仅放行 net_cfg 更新、不再返回失败）；结果码的主要价值在于**显式失败反馈**，避免
   「应答成功但配置未落盘」的静默错误。

### 10.3 行为矩阵（四状态 × 两入口，自查基线）

| Sector1 状态 | 4B02 / 0AH 处理 | 4B02 应答 | 0AH 应答 |
|--------------|-----------------|-----------|----------|
| 空 | 完整初始化后写 | result=0 | 00H |
| 损坏 | 完整初始化后写（自愈） | result=0 | 00H |
| 有效 + updated | 同值跳过 / 更新 net_cfg | result=0 | 00H |
| 升级中间态 | 仅更新 net_cfg（update_sta/app_info 保留） | result=0 | 00H |

（0AH 应答还需 W25 写入成功；W25 与 Sector1 任一步失败即 01H。）

---

## §11 三固件出厂默认网络配置统一约定（2026-08-18，已落地）

**三固件（主固件/Bootloader/Recovery 工程）出厂默认网络配置统一为：**

| 项 | 值 |
|----|----|
| IP | 192.168.114.200 |
| 掩码 | 255.255.255.0（/24） |
| 网关 | 192.168.114.1 |
| 端口 | 9528（0x2538） |

同日落地的一致性修复：
- **Recovery 上电读 Sector1.net_cfg 配置 netif**：`MX_LWIP_Init` 内自行直读 Sector1（该函数早于 main 中 config 判空/初始化代码），magic + CRC 校验通过用 net_cfg.ip/mask/gw，空/无效回退统一出厂默认；Recovery 判空逻辑与 Bootloader 对齐（只看 magic/config_crc 两哨兵，不再把 app_info.crc32==0xFFFFFFFF 当空）。
- **主固件 W25 空 + Sector1 有效时同步 set_ip**：`ldi_ctx_init` 该分支拷贝 cfg 后补 `pl_net_set_ip`，使搜索应答地址与网口实际地址一致（新板首启 114.200 可达）；仍不写 W25/Sector1（上电不写 Flash 纪律保留）。
- **主固件上电默认 IP 统一**：`dev_eth.c` 出厂默认改为 114.200/114.1（与三固件统一值一致）。
- Bootloader 无网络栈，其 `config_info.c` 出厂默认与判空逻辑已满足本约定，无需改动。

---

## §12 网络配置真源与生效语义（2026-08-18，已落地）

**方案 X 定案**：Sector1.net_cfg 为网络配置**唯一真源**（三工程共享），W25 中的网络字段降级为**恢复镜像**。

| 角色 | 存储 | 职责 |
|------|------|------|
| 真源 | Sector1 `net_cfg`（ip/mask/gw 共享 + `port`/`udp_port` 双端口，方案 B 2026-08-21） | Bootloader/Recovery/主固件共享；0AH/4B02 写 `port`（TCP 口），CQ setip 写 `udp_port`（UDP 口） |
| 恢复镜像 | W25 LDI 配置记录（含网络字段） | 擦 Sector1 恢复出厂后用于还原用户配置；主固件上电若 W25 空/无效则回写镜像 |

**主固件上电同步（`ldi_ctx_init` 三分支，`Application/Src/LDI/app_ldi.c`）**：

| 分支 | 条件 | 行为 |
|------|------|------|
| 1 Sector1 优先 | Sector1 有效（magic+CRC）且 net_cfg 合法（IP 非 0、port 1~65535） | 网络字段取 Sector1（**含 port，缺陷 B 修复**）；W25 有效沿用其非网络字段（host/lane/cert/modules）、W25 空/无效回写镜像；netif/TCP Server 口应用移交 `app_net_boot`（§14）。**（2026-08-21 修订：`update_sta==APP_BOARD_UPDATED` 不再参与分支判定——有效记录含升级中间态一律采纳 Sector1.net_cfg；`app_board_net_cfg_update` 自 2026-08-18 起在中间态也放行 net_cfg 更新（0AH/4B02/CQ setip 改 IP 中间态同样生效），若中间态仍落分支 2，W25 回写会用陈旧镜像覆盖刚写入的 net_cfg 导致 setip 失效；Bootloader 条件 D 对中间态记录本就在主固件启动前进 Recovery，主固件运行态下该放宽无副作用）** |
| 2 W25 自愈 | Sector1 空/损坏/记录无效或 net_cfg 非法 + W25 有效 | cfg 全字段取 W25；`app_tcp_client_set_remote` 应用（netif/TCP Server 口应用移交 `app_net_boot`，§14）；`app_board_net_cfg_update` 回写 Sector1（空/损坏完整初始化自愈；有效但 net_cfg 非法仅放行 net_cfg 更新、update_sta/app_info 原样保留）。**（2026-08-21 方案 B：回写以 W25 device_port 为 TCP 口，udp_port 保留现值（get 失败用 20103））** |
| 3 出厂默认 | 两者皆空/无效 | `pl_net_get_ip` 统一出厂默认（§11）填 cfg；不写任何 Flash（netif 应用由 `app_net_boot` 执行，§14） |

**生效语义**：所有改 IP 接口（LDI 0AH / IAP 4B02 / Recovery IAP）统一**重启生效**（运行时网口不即时变更，属既定策略，非缺陷）。

**2026-08-18 缺陷 A/C/D/E 修复落地**：

① **镜像自愈（缺陷 C/A）**：`ldi_ctx_init` 分支 1（Sector1 有效）检测 Sector1.net_cfg 与 W25 网络字段（ip/mask/gw/port）逐项不一致即刷新 W25 镜像——覆盖 4B02 单写 Sector1 不写 W25、或单次 W25 写失败导致的永久陈旧（陈旧镜像会在擦 Sector1 恢复出厂时回滚旧 IP）；SPI 4KB 擦写无内部 Flash 总线停顿，上电执行安全。4B02 写入路径不改（保持 IAP 只写 Sector1 + 启动自愈，解耦）。

② **Bootloader 判空与损坏态重建（缺陷 D）**：`Is_Config_Empty` 改为 `magic != CONFIG_MAGIC` 即视为空（覆盖 magic 半写损坏但非全 FF 态，合法记录 magic 恒为 CONFIG_MAGIC）；条件 D 中 magic 匹配但 config_crc 校验失败（Sector1 擦写中途掉电：magic 已写、CRC 未写完）→ 走条件 C 重建出厂记录（update_sta=updated、app_info.size=0、crc32=0xFFFFFFFF，0 长度 CRC 恒匹配仍能正确跳主固件）；config_crc 对但 app CRC 不匹配（App 损坏）仍跳 Recovery 由升级流程修复。Recovery 判空保持两哨兵不动：Recovery `Init_Config_Info` 写 update_sta=failed，若由 Recovery 重建半写态会造成 Bootloader 条件 D 永远判 failed → 死循环进 Recovery；半写态自愈完全交由 Bootloader（启动顺序 Bootloader 先行）承担，分工已写入 Recovery config_info.c 注释。

③ **统一重启生效（缺陷 E）**：所有改 IP 接口（LDI 0AH / IAP 4B02 / Recovery IAP）统一重启生效；4B02 删除 `IP4_ADDR` + `tcpip_callback` 即时应用段（及仅其使用的 `iap_update_ip` / `ipconfig`），改为纯持久化；Recovery 运行期无即时改 netif 调用（netif 在 `MX_LWIP_Init` 读 Sector1，运行期改配置自然重启生效）。

**UDP 发现口**：10011 为固定设计原则（宏 `LDI_DISCOVERY_PORT`，定义于 `Application/Inc/LDI/app_ldi.h`），禁止提供修改接口（`app_udp_set_port` 已删除，`app_udp.c` 端口取宏引用）。

④ **搜索/上报的端口字段语义（2026-08-18 修复）**：LDI 12H 搜索应答的 `port` 字段与 IAP 0x01 上报 IP 的端口字段语义均为**设备「配置功能端口」= TCP 业务端口（出厂默认 9528，`LDI_DEFAULT_CONFIG_PORT` 宏）**，而非 UDP 发现口 10011——搜索/上报的**传输渠道**才是 UDP 10011。此前 `cmd_search` 误把 `LDI_DISCOVERY_PORT`(10011) 填入 12H 应答 port 字段，导致上位机把 10011 当配置端口读回并写回，污染 Sector1/W25 后 `ldi_ctx_init` 读得 device_port=10011 → TCP Server 错误改监听 10011（9528 连不上）。修复：12H port 取 `g_ldi.cfg.device_port`（未配置回退 9528）；已污染存量设备需 0AH 重设端口 9528 或擦 Sector1 恢复出厂默认。

---

## §13 固件版本落库（app_board_net_cfg_fw_version_update，2026-08-18，已落地）

**定案**：IAP 0x03「上报固件状态」应答的 `app_info.version` 改由主固件**启动时**从
`PROGRAM_CODE`（`app_boot.h` 常量）落库到 Sector1——此前该字段无人写入（出厂空扇区与
初始化路径均为全 0，0x03 恒报全 0）。

**API**：`Application/Src/Config/app_board_net_cfg.c` 新增
`app_board_net_cfg_fw_version_update(const char *version)`；调用点 `app_boot.c init_task()`，
位于 `sw_board_init()` 之后、`app_splash_display()` 之前。

**红线**：只允许改 `app_info.version[32]` 并重算 `config_crc`，**绝不动**
`app_info.size / app_info.crc32 / net_cfg / update_sta` 既有值（size/crc32 由 Recovery
升级流程写入，改动会触发 Bootloader 条件 D 判 App 损坏）。

**四分支语义（空/损坏自愈与 net_cfg_update 同构；升级中间态仅本函数拒绝——net_cfg_update 自 2026-08-18 起仅放行 net_cfg 更新）**：

| Sector1 状态 | 判定 | 处理 |
|--------------|------|------|
| 空 | magic==0xFFFFFFFF 且 config_crc==0xFFFFFFFF | 完整初始化（magic + update_sta=updated + app_info 全 0 且 crc32=0xFFFFFFFF + net_cfg 全 0）+ 填 version |
| 损坏（magic 无效或 CRC 错） | !empty 且 !valid | 完整初始化后填 version（覆盖自愈） |
| 有效 + update_sta==updated | valid 且 updated | 仅更新 version；同值跳过擦写 |
| 升级中间态 | valid 且 update_sta≠updated | **返回 -1 拒绝覆盖**（中间态拒绝仅本函数保留） |

- **边界（如实声明）**：空/损坏分支把 net_cfg 写成 0.0.0.0/port0——调用点安排在
  `sw_board_init()`（含 `ldi_ctx_init` 从 W25Qxx 回写 net_cfg）之后，正常路径不会走到该
  分支；若走到（如 `ldi_ctx_init` 未回写），net_cfg 以 0 落库，由后续 0AH / 上电同步恢复。
- **错误码**（仅限本函数）：0 成功（含同值跳过）；-1 升级中间态拒绝覆盖（中间态拒绝仅本函数保留——net_cfg 函数 `app_board_net_cfg_update` 自 2026-08-18 起仅放行 net_cfg 更新、只返回擦/写错误码）；-2 version 过长
  （`strlen(version) >= 32`，不做截断，显式拒绝）；其余为擦/写错误码。
- **同值跳过**：version 与现扇区一致则跳过擦写返回 0（Q3 同款，不做磨损均衡）——保证每次
  版本发布只擦一次 16KB 扇区，日常重启零 Flash 磨损；写前 `pl_iwdg_refresh` 喂一次狗
  （16KB 擦除耗时数百 ms）。
- **失败不阻断启动**：调用点忽略返回值（升级中间态拒绝 / 擦写错误静默降级，0x03 报旧值
  或全 0）。
- **RTT 诊断日志（2026-08-18 补）**：各分支打印 `[fwver] ...`（仅失败/首写/跳过路径，低
  开销）：`reject: bad version arg`（-2）/ `refuse: upgrade mid-state`（-1）/ `sector
  empty/damaged -> full init`（空/损坏自愈首写）/ `same ver=... , skip`（同值跳过）/
  `write ok ver=...`（成功）/ `erase|write sector1 failed`（擦写失败）。现场复现「0x03
  版本为空」时先看 RTT：**完全无 `[fwver]` 日志 = 主固件根本没运行（设备落在 Recovery
  态，查 Bootloader 条件 D）；有日志但写失败 = 擦写路径问题**。
- **0x03 应答字节序（2026-08-18 补）**：0x03 应答 version 按**大端 word 构造**（对齐 0x01
  IP 约定），存储侧保持纯 ASCII——`cmd_ReportFirmwareStatus_03` 逐 word
  `v[4i]<<24|v[4i+1]<<16|v[4i+2]<<8|v[4i+3]` 构造，禁 memcpy 裸拷（否则上位机按 4 字节
  一组反转显示）。主固件与 Recovery `cmd.c` 同构修改。

---

## §14 网络配置应用移交中立模块 app_net_boot（2026-08-21，已落地）

**背景**：「应用 netif + TCP Server 口」此前分散在 `ldi_ctx_init` 三分支末尾与 CQ
`_cq_net_cfg_apply`（PROTO_CHONGQING），协议模块既管配置真源裁定又管网络应用，
横切职责耦合。2026-08-21 抽离为中立模块 `Application/Src/app_net_boot.c`
（`app_net_boot_apply`，两口径都编入）。

**职责**：`app_boot.c init_task` 在 `app_board_net_cfg_fw_version_update` 之后调
`app_net_boot_apply()`——读 Sector1 net_cfg（magic+CRC 有效、IP 非 0、port
1~65535）→ `pl_net_set_ip` 应用 netif + `app_tcp_server_set_port` 应用 TCP
Server 口（**两口径统一，2026-08-24 修订**：TCP 口应用不再随 PROTO_CHONGQING
裁剪——TCP Server/Client 通道两口径均启动，见下「TCP 通道裁剪」实态更正）。

**accept_write（两口径统一）**：Sector1 空/损坏/非法 → 写本构建默认记录落盘并应用：

| 口径 | ip / mask / gw | port | 端口应用对象 |
|------|----------------|------|-------------|
| PROTO_CHONGQING | 192.168.1.5 / 255.255.255.0 / 192.168.1.1 | 20103 | TCP Server 口（net_cfg.port，两口径统一 2026-08-24）；UDP 业务口由 `_udp_cq_read_port` 直读 Sector1 |
| 其余（dev/LDI） | 192.168.114.200 / 255.255.255.0 / 192.168.114.1 | 9528 | TCP Server 口 |

**顺序约束**：`sw_board_init` 内 `ldi_module_init` → `ldi_ctx_init`（Sector1/W25
自愈 + LDI 设备配置装载，**不再调用 pl_net_set_ip / app_tcp_server_set_port**）先
执行；其后 `fw_version_update`（空/损坏扇区初始化时 net_cfg 置 0）→
`app_net_boot_apply` 判无效写默认——顺序不可颠倒。

**LDI 瘦身**：`ldi_ctx_init` 三分支末尾的 `pl_net_set_ip` /
`app_tcp_server_set_port` 与末尾 `[netcfg] apply` 日志删除；分支 2 保留
`app_tcp_client_set_remote`；分支判定/自愈日志保留。

**CQ 瘦身**：`app_cq_proto.c` 的 `_cq_net_cfg_apply` 整块删除（含 `s_cq_def_ip/mask/gw`
常量与 `[cq] net_cfg_apply` 日志），`cq_proto_init` 不再承担网络启动职责；
`_udp_cq_read_port`（app_udp.c，PROTO_CHONGQING 分支读 Sector1）保持不动。

**TCP 通道裁剪（2026-08-24 实态更正）**：`app_boot.c` 实际**无**口径裁剪——
TCP Server/Client 通道两口径均启动（`app_tcp_server_start()` /
`app_tcp_client_start()` 无条件调用；此前本节与 doc/CLAUDE.md「CQ 构建不启动
TCP 通道」表述与代码不符）。2026-08-24 起 TCP 口应用同步两口径化：
`app_net_boot_apply` 不再按 PROTO_CHONGQING 裁剪 `app_tcp_server_set_port`，
修复「LDI 编入 + PROTO_CHONGQING」混合构建（如 EIDE Debug 口径错配）下 TCP 口
恒 9528、与 LDI 12H / IAP 0x01 上报的 net_cfg.port 矛盾的现场 bug。CQ setip
端口语义不受影响（仍只写 udp_port、port 保留现值）。

**RTT 观察点（2026-08-21 撤销）**：本节落地时曾补 `[netcfg] boot-apply`、
`[netcfg] ldi_ctx_init/branch1/2/3/branch2 write-back`、`[cq] net_cfg_apply` 等
诊断标签，2026-08-21 已全部删除（源码不再有任何 `[netcfg]`/`[cq]` RTT 日志）；
网络配置应用正确性以「擦 Sector1 出厂化 → 上电三分支落盘」静态审计与双构建
验证为准。仅 `app_board_net_cfg.c` 的 `[fwver]` 日志保留（0x03 版本判断路径
依赖，见 doc/CLAUDE.md 烧录小节）。

---

## §15 方案 B：Sector1 记录新增 udp_port，TCP/UDP 端口彻底分离（2026-08-21，已落地）

**定案**：`Sector1.net_cfg.port`（u32）历史语义完全不变 = **TCP 业务口**（LDI 0AH
写 / IAP 0x01 报 / 0x02 写 / TCP Server 监听，出厂默认 9528）；新增
`Sector1.net_cfg.udp_port`（u32）= **CQ UDP 业务口**（CQ setip 写 / CQ UDP 通道
读，出厂默认 20103）。共存构建下两套口并存；ip/mask/gw 共享（setip 与 0AH 都写，
现状不变）。

**记录布局（三固件二进制兼容，各自 static_assert 锁定）**：

| 字段 | 偏移 | 大小 | 语义 |
|------|------|------|------|
| magic | 0 | 4B | 魔数 0x0d000721 |
| update_sta | 4 | 4B | 升级状态机 |
| app_info (size/crc32/version[32]) | 8 | 40B | 固件信息 |
| net_cfg.ip/mask/gw | 48 | 12B | 两套口共享 IP/掩码/网关 |
| net_cfg.port | 60 | 4B | **TCP 业务口**（LDI/IAP 生态，默认 9528） |
| net_cfg.udp_port | 64 | 4B | **CQ UDP 业务口**（CQ 生态，默认 20103） |
| config_crc | 68 | 4B | 覆盖 magic~udp_port（68B）的 CRC32 |

NetConfig 16B → 20B；SysInfo 68B → 72B（18 words）。config_crc 覆盖范围随结构体
扩展（仍为「整结构体去掉 crc 字段自身」），与 Bootloader/Recovery 的
`HAL_CRC_Calculate` 覆盖一致。

**各写入点端口语义（调用 `app_board_net_cfg_update(ip,mask,gw,port,udp_port)`）**：

| 写入点 | port（TCP 口） | udp_port（CQ UDP 口） |
|--------|----------------|------------------------|
| LDI 0AH（app_ldi_cmd.c） | 命令下发 device_port | 保留现值（get 失败 20103） |
| IAP 0x02/4B02（app_iap_cmd.c） | 包内 port | 保留现值（同上） |
| ldi_ctx_init 分支 2 回写（app_ldi.c） | W25 device_port | 保留现值（同上） |
| CQ setip（app_cq_proto_cmd.c） | **保留现值（get 失败 9528）** | 命令下发端口 |
| app_net_boot invalid 写默认（dev/CQ 两口径） | 9528 | 20103 |

**读取点**：`_udp_cq_read_port`（app_udp.c）`PROTO_CHONGQING` 分支读 `udp_port`
（1~65535 有效即用，无效/0 回退 20103），`#else` 固定 20103；CQ 12B 搜索应答报
`app_udp_cq_get_port()`；LDI 12H/1DH 报 `g_ldi.cfg.device_port`（TCP 口）；Bootloader/
Recovery 的 0x01/0x02 继续使用 `net_cfg.port`（TCP 语义不变）。

**升级兼容（一次配置丢失，已接受）**：旧 68B 记录升级到新固件后，主固件按 72B
布局重算 config_crc 必然失配 → 判损坏 → 走空/损坏自愈路径重写记录（网段配置
丢失一次：dev 由 app_net_boot 写 114.200/9528/20103 出厂默认或由 ldi_ctx_init
从 W25 镜像回写恢复用户配置；CQ 由 app_net_boot 写 192.168.1.5/9528/20103）。
Bootloader/Recovery 同步升级后按 72B 布局读写，与主固件自愈后的记录二进制兼容。

---

## 修订

- 2026-08-14：首版方案，待审核。
- 2026-08-14（同日修订）：§2.2 增补完整调用点清单（全仓 7 处 `ADDR_CONFIG_SECTOR` 使用点 + LDI 侧 4 处调用 + 4 处独立结构体定义）与布局锁定步骤；§2.3 DoD 门禁扩大至全仓；新增 §8 失败场景与掉电语义；§7 补充断电与擦写寿命风险。
- 2026-08-14（同日再修订）：新增 **§9 先行修复定案**——Q1（镜像 Bootloader 语义）、Q2（如实应答 + 上位机重发）、Q3（接受现状 + 同值跳过）三决策落地；§2.2 步骤 6、§6、§8.3 中「待用户确认」标记改为定案结论；范围声明为「先行修复，07 解耦主体重构暂缓」。
- 2026-08-14（同日三修订）：新增 **§10 补充修复定案**——方案 1（细化守卫：升级中间态拒绝、损坏自愈覆盖、同值跳过条件 !empty→valid）、方案 2（4B02 应答 len 0→1 结果码 0/1）与四状态×两入口行为矩阵；§9 Q2 及走查第 6 步的「损坏拒绝覆盖」表述标注已被取代；§8.2 增补损坏自愈语义。
- 2026-08-14（同日四修订）：**方案 A 落地**——按 §2.2 步骤执行：新建 `Application/Config/app_board_net_cfg.{c,h}`（Sector1 布局唯一归属 + 8 条 static_assert 锁定 + `get/update/read` API），LDI 4 处与 IAP 3 处调用迁移，`app_iap_cfg.{c,h}` 与死代码 `Device/Inc/config_info.h` 删除；Makefile / eide.yml 同步；DoD §2.3 全项验证通过（全量 + LDI-only + IAP-only 三种链接，产物尺寸零变化）。
- 2026-08-14（同日五修订）：§7 风险表新增「Bootloader 条件 D `app_info` CRC 校验 × 烧录器重烧主固件 → 永远进 Recovery」风险行——Recovery 升级过一次后用烧录器直接重烧主固件会 CRC 失配被判 App 损坏；缓解为 `tool/flash_all.sh` 默认烧后擦 Sector1 恢复出厂态（`--keep-config` 保留），手动烧录/单固件烧录遵守同纪律，net_cfg 由主固件上电从 W25 同步回写。烧录纪律同步写入 `doc/CLAUDE.md` 烧录小节。
- 2026-08-18：新增 **§11 三固件出厂默认网络配置统一约定**（192.168.114.200/24、网关 192.168.114.1、端口 9528）及三项一致性修复落地：Recovery `MX_LWIP_Init` 读 Sector1.net_cfg 配 netif（空/无效回退统一默认）+ Recovery 判空对齐 Bootloader；主固件 `ldi_ctx_init` W25 空 + Sector1 有效分支补 `pl_net_set_ip`；主固件 `dev_eth.c` 出厂默认 IP 统一为 114.200/114.1。
- 2026-08-18（同日再修订）：新增 **§12 网络配置真源与生效语义**——**方案 X 落地**：Sector1.net_cfg 为唯一真源、W25 网络字段为恢复镜像；`ldi_ctx_init` 重构为三分支（Sector1 优先 / W25 自愈回写 / 出厂默认），缺陷 B 修复（分支 1 端口改读 Sector1.net_cfg.port）；0AH/4B02 修改后重启生效定为既定策略；UDP 发现口 10011 固定为宏 `LDI_DISCOVERY_PORT`，删除 `app_udp_set_port` 修改接口。
- 2026-08-18（12H 端口字段语义修复）：`cmd_search` 12H 应答 port 字段由误用 `LDI_DISCOVERY_PORT`(10011) 改为设备配置功能端口 `g_ldi.cfg.device_port`（未配置回退 `LDI_DEFAULT_CONFIG_PORT`=9528）；新增宏 `LDI_DEFAULT_CONFIG_PORT (9528U)`（app_ldi.h）。修复 10011 污染循环（12H 报 10011 → 上位机写回 → TCP Server 错误监听 10011）；存量污染设备需 0AH 重设 9528 或擦 Sector1。详见 §12 ④。
- 2026-08-18（同日三修订）：§12 增补 **缺陷 A/C/D/E 修复落地**——①分支 1 镜像自愈（Sector1 与 W25 网络字段不一致即刷新镜像，覆盖 4B02 单写与 W25 单次写失败的陈旧）；②Bootloader 判空改 magic 哨兵（magic 不匹配即空）+ magic 匹配但 CRC 错走条件 C 重建出厂记录（App 损坏仍 Recovery；Recovery 判空保持两哨兵防死循环的分工原因）；③所有改 IP 接口（LDI 0AH / IAP 4B02 / Recovery IAP）统一重启生效（4B02 删除 `IP4_ADDR`+`tcpip_callback` 即时应用段与 `iap_update_ip`）。
- 2026-08-18（同日五修订）：§13 增补 **RTT 诊断日志**——`app_board_net_cfg_fw_version_update` 各分支打印 `[fwver] ...`（失败/首写/跳过路径），现场可区分「主固件未运行（无日志）/ 写失败 / 同值跳过」；`doc/CLAUDE.md` 烧录小节同步补「0x03 版本为空判断路径」。
- 2026-08-18（同日五修订）：**升级中间态守卫语义修订**——修复「中间态冻结 net_cfg」缺陷：`app_board_net_cfg_update`（0AH/4B02 网络配置写入）升级中间态不再拒绝，仅放行 net_cfg 字段更新（update_sta/app_info 从 Flash 读出原样保留，Bootloader 条件 D 判定不受影响），不再返回 -1；`app_board_net_cfg_fw_version_update`（0x03 版本落库）保持中间态拒绝 -1 不变（version 属 app_info，由 Recovery 升级流程写入）。§10.1 表/§10.3 行为矩阵/§10.2 结果码表/§12 分支 2/§13 错误码注记同步。
- 2026-08-18（同日六修订）：**0x03 应答 version 字节序修复**——应答载荷 version 由 memcpy 裸拷改为逐 word 大端构造（对齐 0x01 IP 约定），存储侧保持纯 ASCII；`app_iap_cmd.c cmd_ReportFirmwareStatus_03` 与 Recovery `cmd.c` 同构修改，上位机显示不再每 4 字节一组反转（"9K10212482" 此前显示为 "01K9421228"）。
- 2026-08-21：**§12 分支 1 条件修订（setip 中间态覆盖修复）**——`ldi_ctx_init` 分支 1 条件由「Sector1 有效（magic+CRC）且 `update_sta==APP_BOARD_UPDATED`」改为「Sector1 有效且 net_cfg 合法（IP 非 0、port 1~65535）」，`update_sta` 不再参与分支判定；分支 2 条件相应收窄为「Sector1 空/损坏/记录无效或 net_cfg 非法」。根因：2026-08-18 起 `app_board_net_cfg_update` 在升级中间态也放行 net_cfg 更新（CQ setip/0AH/4B02 改 IP 中间态应生效），但 `ldi_ctx_init` 仍把中间态记录判入分支 2 并以 W25 陈旧镜像回写覆盖 Sector1，导致 setip 写入的 net_cfg 在重启后被冲掉。同步补 RTT 观察点：`[netcfg] ldi_ctx_init`（三分支判定值）、`[netcfg] branch1/2/3`、`[netcfg] branch2 write-back sector1 ret`、`[netcfg] boot-apply`。
- 2026-08-21（同日再修订）：**网络配置应用横切职责解耦**——新增 §14：中立模块 `app_net_boot`（`app_net_boot_apply`）统一承担「Sector1 net_cfg → netif + TCP Server 口」应用，`ldi_ctx_init` 三分支末尾的 `pl_net_set_ip`/`app_tcp_server_set_port` 与 CQ `_cq_net_cfg_apply`（含 s_cq_def_* 常量）整块删除；accept_write 两口径统一（Sector1 空/损坏/非法写本构建默认记录落盘并应用：CQ 192.168.1.5/20103、dev 192.168.114.200/9528）；CQ 构建不启动 TCP Server/Client 通道；Makefile/eide.yml 编入 app_net_boot.c；`[netcfg] boot-apply` 替代 `[netcfg] apply` / `[cq] net_cfg_apply` 观察点。
- 2026-08-21（同日三修订）：**方案 B 落地（新增 §15）**——Sector1.net_cfg 新增 `udp_port`（CQ UDP 业务口，默认 20103），`port` 恒为 TCP 业务口（9528）不再被 setip 污染；记录 68B→72B（NetConfig 16B→20B），三固件 config_info 同步扩展 + static_assert 锁定；`app_board_net_cfg_update` 签名加 udp_port（同值跳过比较含 udp_port），5 处调用点按「写本协议端口、保留另一端口现值」适配；`_udp_cq_read_port` PROTO_CHONGQING 分支改读 udp_port；Recovery 0x02 改为逐字段写 ip/mask/gw/port（udp_port 保留现值）；Bootloader/Recovery 初始化默认补 udp_port=20103。**旧 68B 记录升级后 CRC 失配 → 自愈重写，网段配置丢失一次（已接受）**。
- 2026-08-21（同日四修订）：**调试期 RTT 日志清理 + CQ 心跳宽松语义**——删除本会话为排障添加的全部 `[cq]`/`[netcfg]` SEGGER_RTT 日志（app_cq_proto/parse/cmd、app_udp `udp_cq port`、app_ldi 三分支、app_net_boot boot-apply）与相应 `SEGGER_RTT.h` include，§14 RTT 观察点同步改述；仅 `[fwver]` 保留。CQ 心跳复位放宽为「帧结构合法的 JSON（OK/ERR_CMD/ERR_PARAM）均视为上位机存活」，故障屏显示后下一存活帧先 `app_default_display_show()` 恢复默认画面；`s_cq_sync_counter`/`s_cq_warn_seconds` 跨任务读写加临界区保护（doc/03 PartB B.7.2）。
- 2026-08-21（同日五修订）：**LDI 搜索应答丢包修复（广播路径根治）**——现场「LDI 12H 搜索应答 50-75% 无响应」根因：dev 共存构建 baseline 4 netconn 已满（UDP 10011 + UDP 20103 + TCP Server listener + TCP Client），`app_udp_broadcast` 每次临时 `netconn_new` 必然 NULL 静默失败（TCP Client 周期性断开重连制造「池偶尔有空位」窗口，解释 25-50% 偶尔成功）。修复三层：① `lwipopts.h` 显式 `MEMP_NUM_NETCONN=8`、`MEMP_NUM_UDP_PCB=8`（opt.h 默认 4；bss 增量 304B，PBUF_POOL_SIZE 不动——SRAM 水位 87-94%，另行核算）；② `app_udp_broadcast`/`app_udp_cq_broadcast` 改为复用常驻通道 conn（`udp_task`/`udp_cq_task` 创建时已设 SOF_BROADCAST，经 `app_channel_get` 回验 + conn 非空判空，deinit 新增 conn=NULL），常驻未就绪回退临时 conn；③ LDI 12H 应答改「广播为主、回源为辅」——先广播、回源失败静默。**已知限制（本轮不做）**：回源依赖通道级 `src_ip/src_port` 快照，LDI 帧排队期间 10011 上后续 IAP/CQ 帧覆盖快照 → 回源可能发错目标；IAP 回源同源竞态不修改（IAP 停等一问一答、低流量）。帧级 src 快照（frame_msg_t 扩展）属架构改造，留待后续。
- 2026-08-24：**TCP 口应用两口径化（混合口径 bug 修复）**——现场「LDI 上位机把设备端口改为 9529 重启后，LDI 12H 与 IAP 0x01 均上报 9529，但 TCP 连接 9529 失败、9528 却能连上」根因：设备固件为 EIDE Debug「LDI 编入 + defineList 含 PROTO_CHONGQING」的混合口径构建，`app_net_boot_apply` 中 `app_tcp_server_set_port` 被 `#ifndef PROTO_CHONGQING` 裁剪（CQ 构建假定无 TCP 业务），而 `app_boot.c` 实际无口径裁剪、TCP Server 照常启动 → 绑定编译期默认 `TCP_SERVER_PORT`=9528（app_tcp_server.c），与搜索/上报的 Sector1 `net_cfg.port`（9529）矛盾。修复：`app_net_boot.c` 删除端口应用的两处 `#ifndef PROTO_CHONGQING` 裁剪（含 `app_tcp_server.h` include），TCP 口两口径统一应用 `net_cfg.port`——任意构建口径下 TCP 监听口恒与 LDI 12H / IAP 0x01 上报一致；CQ setip 语义不受影响（仍只写 `udp_port`、`port` 保留现值）。同步更正「CQ 构建不启动 TCP Server/Client 通道」的过时表述（app_boot.c 实无裁剪），doc/CLAUDE.md 与 doc/03 PartB B.7.1 同步。EIDE Debug 口径错配（defineList 与 excludeList 不成对）仍应修正，但代码已对混合口径兜底。
