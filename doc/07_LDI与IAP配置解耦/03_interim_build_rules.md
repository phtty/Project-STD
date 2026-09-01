# 03 解耦前：工程选编临时规则（已废止）

> **状态**：**已废止（2026-08-14）**　|　[02](./02_decoupling_solutions.md) 方案 A 已落地（配置层抽出为 `Application/Config/app_board_net_cfg.c`，公共必选），本文件仅存历史。  
> **历史目的**：在不改代码的前提下，避免「只加 LDI」链接失败。

---

## §1 规则（强制）

### 编入 LDI 时

| 必须编入 | 可选 |
|----------|------|
| `Application/Src/LDI/**`（协议） | — |
| **`Application/Src/IAP/app_iap_cfg.c`** | — |
| `Application/Inc/IAP/app_iap_cfg.h`（头路径） | — |
| LDI 已有的存储依赖（`app_ldi_cfg` / W25 等） | — |
| | `app_iap.c` / `app_iap_cmd.c`（仅当需要 UDP IAP 升级） |

**一句话**：LDI ⇒ 必带 **IAP 配置存储**；不必带 **IAP 升级协议**。

### 不编 LDI、只编 IAP 升级时

| 必须 | 说明 |
|------|------|
| `app_iap_cfg.c` + `app_iap.c` + `app_iap_cmd.c` | 协议本身依赖配置区 |

### 两者都不编

可不编 `app_iap_cfg.c`（无引用则 `--gc-sections` 可丢；若他处仍引用则仍要保留）。

---

## §2 EIDE 操作提示

1. 排除整个 `IAP/` 目录前，确认是否仍勾选了 LDI。  
2. 若保留 LDI：在源文件列表中 **单独加回** `app_iap_cfg.c`（不要依赖「整个 IAP 文件夹」开关）。  
3. 链接失败时先 `rg app_flash_iap Application/Src/LDI`，对照本表，而不是先怀疑工具链。

---

## §3 Makefile 对照

全量 `make` 默认编入 IAP+LDI，一般无此问题。  
若增加 `PROTOCOL=ldi-only` 之类裁剪目标，须在源列表中显式保留 `app_iap_cfg.c`，或改为已解耦的 `app_board_net_cfg.c`。

---

## §4 验收（临时）

- [ ] EIDE：LDI + 仅 `app_iap_cfg.c`，无 `app_iap.c` → **链接成功**  
- [ ] EIDE：LDI，无任何 IAP 文件 → **链接失败**（符合当前代码预期；解耦后此条应反过来）

---

## §5 已知限制（临时规则期间）

临时规则只解决链接问题；以下现状缺陷**已按 [02 §9](./02_decoupling_solutions.md) 先行修复**（2026-08-14），剩余限制继续有效：

| 限制 | 状态 / 说明 |
|------|------|
| 出厂新机 0AH 改 IP 不同步 Sector1 | **已修复（Q1）**：空 Sector1 写路径完整初始化（镜像 Bootloader 语义），Bootloader/Recovery 可读到该 IP |
| 0AH 写失败/断电无保护 | **已修复（Q2）**：两步写返回值如实应答（00H/01H），上位机重发恢复；上电同步钩子覆盖 Sector1 空/损坏（均完整初始化自愈，升级中间态仅放行 net_cfg 更新、update_sta/app_info 保留，见 [02 §10](./02_decoupling_solutions.md)）；掉电后按 [02 §8](./02_decoupling_solutions.md) 语义回退 |
| 运行中擦写 Sector1 停顿 1~2s | 每次 16KB 全扇区擦除（Q3 接受现状，仅增加同值跳过，不做磨损均衡）；出厂配置操作可接受，高频改 IP 需产品论证 |

---

## 修订

- 2026-08-14：与 01/02 同步首版。
- 2026-08-14（同日修订）：新增 §5 已知限制（出厂新机 0AH、写失败/断电、擦写停顿）。
- 2026-08-14（同日再修订）：§5 改为「已修复/剩余限制」两态，同步 [02 §9](./02_decoupling_solutions.md) 三决策落地状态。
- 2026-08-14（同日三修订）：§5「写失败/断电」行同步 [02 §10](./02_decoupling_solutions.md) 守卫细化——Sector1 空/损坏均完整初始化自愈，仅升级中间态拒绝覆盖。
- 2026-08-14（同日四修订）：**废止**——方案 A 落地后「必带 `app_iap_cfg.c`」不再必要；配置层 `app_board_net_cfg.c` 列为公共必选，LDI / IAP 协议目录均可独立裁剪。
