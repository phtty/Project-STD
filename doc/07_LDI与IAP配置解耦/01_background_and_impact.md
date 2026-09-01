# 01 背景、根因与影响

> **状态**：现行事实　|　2026-08-14  
> **触发**：EIDE 工程仅勾选 LDI 相关源文件时链接失败（见终端 `undefined reference to app_flash_iap_*` / `g_config`）。  
> **解耦方案** → [02_decoupling_solutions.md](./02_decoupling_solutions.md)  
> **临时选编** → [03_interim_build_rules.md](./03_interim_build_rules.md)

---

## §1 问题现象

典型链接错误（EIDE / unify_builder）：

```
undefined reference to `app_flash_iap_is_config_valid'
undefined reference to `app_flash_iap_update_net_cfg'
undefined reference to `g_config'
```

出现在：

- `Application/Src/LDI/app_ldi.c` → `ldi_ctx_init`
- `Application/Src/LDI/app_ldi_cmd.c` → `cmd_set_ip`

Makefile 全量编译时不易暴露；**按协议目录裁剪（只留 LDI）时必现**。

---

## §2 背景与设计意图（为何当初耦合）

板级存在两套「网络相关」持久化：

| 存储 | 位置 | 模块 | 内容 |
|------|------|------|------|
| LDI 设备配置 | W25Qxx 末 4KB 扇区 | `app_ldi_cfg`（实际文件 `Application/Src/LDI/app_ldi_cfg.c`） | 车道、证书、模块表、IP/端口等 |
| IAP 系统配置 | 内部 Flash Sector1 `0x08004000` | `app_iap_cfg`（实际文件 `Application/Src/IAP/app_iap_cfg.c`；`Device/Inc/config_info.h` 为同名旧定义，全仓无引用，已废弃） | magic、升级状态、FW 信息、**net_cfg(IP/掩码/网关)** |

产品需求（历史合理）：

1. Bootloader / Recovery / 主固件共享 **同一套上电 IP**（写在 Sector1）。  
2. LDI `0AH` 改 IP 后，除写入 W25 外，还要 **同步到 Sector1**，避免下次启动 Boot 仍用旧 IP。  
3. 上电时若 W25 无 LDI 配置，可 **回退读取** Sector1 的 `net_cfg`。

因此 LDI 直接调用了 `app_iap_cfg` API，而不是通过中立的「板级网络配置」抽象。

---

## §3 根因（精确到模块边界）

### 3.1 依赖不是「IAP 协议」，而是「IAP 配置存储」

```
LDI 协议模块                IAP 目录
─────────────────          ─────────────────────────────
app_ldi.c  ──────────────► app_iap_cfg.c/.h   ← 链接必需
app_ldi_cmd.c              app_iap.c/.h         ← 升级协议，可裁
                           app_iap_cmd.c        ← 可裁
```

未定义符号均来自 **`app_iap_cfg.c`**：

| 符号 | 作用 |
|------|------|
| `g_config` | 映射到 `0x08004000` |
| `app_flash_iap_is_config_valid` | 校验 Sector1 记录 |
| `app_flash_iap_update_net_cfg` | 擦写更新 IP/掩码/网关 |

### 3.2 调用点（代码事实）

**上电 `ldi_ctx_init`**（`Application/Src/LDI/app_ldi.c:100-108`）：

- W25 有有效 LDI 配置，**且 Sector1 记录有效（`app_flash_iap_is_config_valid(g_config)` 为真）**，且与 Sector1 不一致 → `app_flash_iap_update_net_cfg`（`app_ldi.c:100-104`）
- W25 无效 → Sector1 有效则从 `g_config->net_cfg` 拷贝 IP（`app_ldi.c:108`）；Sector1 也无效 → 用上电默认 IP
- **前置条件（已修复，2026-08-14）**：Sector1 为空/无效时原代码**不会**同步写入；现改为统一调用 `app_flash_iap_update_net_cfg`——空/损坏扇区均完整初始化覆盖（镜像 Bootloader 语义，损坏自愈；升级中间态——valid 且 update_sta≠updated——仅放行 net_cfg 字段更新、update_sta/app_info 原样保留，2026-08-18 修订，见 [02 §10](./02_decoupling_solutions.md)）。出厂新机（Sector1 空）走 0AH 改 IP 后 Sector1 已有有效记录，Bootloader/Recovery 能读到该 IP（见 [02 §9](./02_decoupling_solutions.md)）

**运行时 `cmd_set_ip`（0AH）**（`Application/Src/LDI/app_ldi_cmd.c:430-431`）：

- `app_flash_ldi_save_config`（W25）之后立刻 `app_flash_iap_update_net_cfg`（Sector1）；两步写结果现均被检查：任一失败 0AH 应答 ErrCode=01H（00H=成功），固件不自动重试，由上位机重发恢复。失败/断电语义见 [02 §8](./02_decoupling_solutions.md)

### 3.3 与 doc/05「目录选编」的张力

doc/05 目标：**工程目录选编协议**（exclude 即可裁剪）。  
现行 LDI 破坏了「只编 LDI 目录」的可链接性——exclude 整个 `IAP/` 会连配置层一起丢掉。

### 3.4 现状缺陷 A：空 Sector1 写路径不置 magic

`app_flash_iap_update_net_cfg`（`Application/Src/IAP/app_iap_cfg.c:108-122`）为「读-改-net_cfg-写」，**从不写入 magic**：

1. 空扇区（magic 与 crc 均为 0xFFFFFFFF）通过 `app_flash_iap_is_config_empty` 守卫后，仅改 net_cfg、重算 CRC、擦写回盘；落盘记录 magic 仍为 0xFFFFFFFF。
2. 出厂新机（Sector1 从未写入）走 LDI 0AH 改 IP：擦写一次 Sector1，但记录仍「空」（Bootloader/Recovery 校验 magic 失败）→ Boot/Recovery 上电仍用默认 IP。
3. **修复（已定案并落地，2026-08-14）**：空记录路径改为完整初始化，镜像老 Bootloader `Init_Config_Info` 首次上电语义（magic + update_sta=updated + app_info 全 0 且 crc32=0xFFFFFFFF + net_cfg + CRC32），0AH 同步时把 device_port 一并传入；4B02 改走同一路径。详见 [02 §9](./02_decoupling_solutions.md)。

### 3.5 现状缺陷 B：模块命名与陈旧注释勘误

- 不存在 `dev_flash_iap` / `dev_flash_ldi` 模块；实际实现是 `Application/Src/IAP/app_iap_cfg.c` 与 `Application/Src/LDI/app_ldi_cfg.c`，本目录文档统一按此引用。
- `Device/Inc/config_info.h` 与 `Application/Inc/IAP/app_iap_cfg.h` 重复定义同一套 Sector1 结构体；`config_info.h` 全仓（含 Bootloader/Recovery 项目）无任何 include，为死代码。Bootloader/Recovery 各自另有独立的 `Drivers/BSP/Config/config_info.h`，结构体共 4 处独立定义、无共享头（布局锁定要求见 02 方案 A）。
- LDI 记录大小注释陈旧：`app_ldi_cfg.c:24` 注释「116 byte = 29 words」为 MAX_MODULES=6 时代的数值；现 `APP_FLASH_LDI_MAX_MODULES=11`，实际 `sizeof(app_flash_ldi_record_t)=176B=44 words`。文档引用请以 176B 为准。

---

## §4 影响面

### 4.1 构建 / 产品裁剪

| 场景 | 结果 |
|------|------|
| EIDE 只勾 LDI，排除整个 `Application/Src/IAP/` | **链接失败** |
| 只勾 LDI + `app_iap_cfg.c` | 可链接；无 UDP IAP 升级能力 |
| Makefile 全量 | 通过（掩盖问题） |

### 4.2 架构 / 可维护性

- 协议层（LDI）依赖另一协议目录下的实现，**违反「目录即模块、可独立选编」**。  
- 新人易误判为「LDI 必须绑死 IAP 升级协议」。  
- 重命名/搬迁 IAP 目录时易再次踩链接雷。

### 4.3 运行行为（解耦前仍成立的业务耦合）

即使编译通过，运行时仍假定：

- Sector1 布局与 `app_flash_iap_sys_info_t` 一致；  
- Boot/Recovery 读同一结构；  
- LDI 改 IP 会擦写内部 Flash（有总线停顿风险，代码已避免在「无配置上电路径」写 Flash）；
- 0AH 改 IP 在运行中擦写 Sector1：16KB 全扇区擦除，停总线 1~2s；擦写失败/断电无提示、无回退动作（掉电语义声明见 [02 §8](./02_decoupling_solutions.md)）；
- Sector1 无有效记录时，Bootloader/Recovery 回退各自默认 IP；主固件回退顺序为 W25 → Sector1 → 默认。出厂新机 0AH 改 IP 后 Boot/Recovery 恒用默认 IP（解耦前缺陷，已按 Q1 修复，见 §3.4）。

解耦时必须保留或显式放弃这些产品语义，不能只删调用。

### 4.4 非影响（澄清）

| 项 | 说明 |
|----|------|
| 多协议 RB / 链式 probe | 与本问题无关（doc/05） |
| LDI 与 IAP **帧队列** | 本就独立，不共享 |
| 仅缺 `app_iap.c` | **不会**因本问题报上述符号缺失 |

---

## §5 问题定性

| 维度 | 结论 |
|------|------|
| 缺陷类型 | **编译期模块边界错误**（强符号依赖放错层） |
| 严重度 | 裁剪构建 **阻塞**；全量构建 **隐性** |
| 是否安全漏洞 | 否 |
| 是否必须整包 IAP | **否**；见 [03](./03_interim_build_rules.md) |
| 附带现状缺陷 | 空 Sector1 写路径不置 magic（§3.4）、0AH 写失败/断电无保护（§4.3）、模块命名与注释勘误（§3.5） |

---

## 修订

- 2026-08-14：首版，对齐 EIDE「只加 LDI」链接失败现场。
- 2026-08-14（同日修订）：§2 统一模块命名（`dev_flash_ldi` 混名 → `app_ldi_cfg`）；§3.2 补充同步前置条件（Sector1 有效才同步）；新增 §3.4 空 Sector1 写路径缺陷、§3.5 命名/注释勘误；§4.3 补充 0AH 擦写停顿与掉电回退语义；§5 增加现状缺陷条目。
- 2026-08-14（同日再修订）：§3.2/§3.4 更新为「已定案并落地」状态——空 Sector1 写路径按 [02 §9](./02_decoupling_solutions.md) Q1 修复（镜像 Bootloader 语义），0AH 两步写返回值如实应答（Q2），同值跳过擦写（Q3）。
- 2026-08-14（同日三修订）：§3.2 守卫语义同步 [02 §10](./02_decoupling_solutions.md)——空/损坏均完整初始化覆盖（自愈），仅升级中间态拒绝覆盖；§4.3 出厂新机缺陷句标注「已修复」。
