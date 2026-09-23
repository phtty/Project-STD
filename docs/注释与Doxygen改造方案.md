# 第 ③ 项执行方案：陈旧注释清扫 + Doxygen 化

- **依据**：`docs/命名约定.md` §12（Doxygen 最小必需集）
- **输入**：两路只读清点（陈旧注释 / Doxygen 覆盖缺口）
- **状态**：**待评审**（评审通过后再动代码）

---

## 0. 三类工作

| 类 | 内容 | 规模 | 风险 |
|---|---|---|---|
| **A** | 修**内容已不成立**的注释 | 约 22 条（Application 15 / boards 5 / Device 1） | 低 |
| **B** | 补 **Doxygen 标签** | 量大（`@brief` ~656、`@return` 145、参数方向 168、成员 583、`@file` 17+） | 低但耗时 |
| **C** | 新建 `Doxyfile` 并生成一次 | 1 个文件 | 无 |

**共性约束**：只补标签、只订正事实；**不重写**已有的设计性论述。

---

## 1. A 类：陈旧注释清单（逐条）

### 1.1 数值错误（已抽验确认）

| 位置 | 现状 | 应改为 |
|---|---|---|
| `Application/Inc/app_dispatch.h:41` | "加帧头与载荷头共 **1430**" | **1428**（`CASC_FRAME_MAX` = 15 + 13 + 1400） |
| `Application/Src/CASC/app_casc.c:88` | "队列元素 = 8 + **1427** = **1435**" | 8 + **1428** = **1436** |
| `Application/Src/CASC/app_casc.c:96,124,155,310,762` | "**1427** 字节"（6 处） | **1428** |
| `Application/Src/IAP/app_iap_cfg.c:37` | "前 **4 个扇区**留给 bootloader" | 0x08040000 = 256KB = 扇区 0–5（4×16K+64K+128K）；建议改写"前 256KB"更稳 |
| `Application/Src/app_diag.c:15` vs `:50` | "30×40 = 1.2KB" vs "约 700 字节"（任务上限 24） | 按 `APP_DIAG_TASK_MAX` 实测值统一 |

### 1.2 阶段陈述过期（已实装，却仍写"P1/留到/改成"）

| 位置 | 问题 |
|---|---|
| `app_casc.c:456` | "P2 只三条" —— 命令表已 7 条（`g_casc_cmd`） |
| `app_casc.c:488` | "P4 建卡表时这里改成填表" —— 紧接着 496-504 就在填 |
| `app_casc.c:1353` | "运行期重新枚举…留到 P4" —— 已实装（`CASC_REENUM_MS`） |
| `app_casc.c:165` | "P4 要做的…的雏形" —— 同上 |
| `app_casc.c:1375` | "P3 起每轮 SYNC_BEGIN 带 bright" —— `SYNC_BEGIN` 已删除，bright 现挂 IMAGE |
| `app_casc.c:690` | "SET_COLOR/SET_LAYOUT/BLANK 归后续期" —— 前两者是**裁掉**（颜色随 IMAGE），且无 `SET_LAYOUT` 类型 |
| `app_screen.c:5-7` | "P1 阶段不开级联…级联接入后…" —— 级联已接入 |
| `app_screen.c:438-439` | "级联接入后这里要改成按切分表抽本卡矩形" —— 已实现（`app_screen_extract`） |
| `app_screen.h:254`、`board.h`×2 | "本期取编译期常量…后续期由切分表记录覆盖" —— 运行期身份已实现并持久化（`_casc_id_boot`） |

### 1.3 与代码不符

| 位置 | 问题 |
|---|---|
| `Device/Inc/Network/dev_eth.h:5,13` | "预留硬件初始化" —— `dev_eth.c:60-85` 已完整实现 MAC+PHY |
| `board.h`×2（`BOARD_SCREEN_COLOR`） | 注释 `/* COLOR_GREEN */` → `DEV_DISPLAY_COLOR_GREEN` |
| `app_render.h:7,83`、`app_render.c:8`、`board.h`×2 | 板级路径缺 `Application/`（`boards/*/Src/...` → `boards/*/Application/Src/...`） |

### 1.4 调试残留（4 处）

| 位置 | 内容 |
|---|---|
| `Application/Src/LDI/app_ldi_cmd.c:737` | `// 调试：排除 RTC 并发写`（在命令处理路径上） |
| `Application/Src/app_diag.c:22-23` | "查完再打开" |
| `Application/Src/app_dispatch.c:23,27,31` | "查完连着开关一起删"（`DISPATCH_DIAG` 默认 0） |
| `Application/Src/Channel/app_tcp_server.c:72-73` | `s_tcp_server_connected` —— **只写不读的死调试变量**（处理方式见 §5 决策） |

### 1.5 明确**不要动**（避免误伤）

历史归因说明（"原先的规矩""缺陷历史""2026-09-22 撤掉对齐轮"）、真前向标记（"后续期 W25Qxx 切分表""未实现""预留段"、真 TODO）、`test_*` 里的回归案例历史、`lwipopts.h`（§14 E2）、`Compiler/startup.h`（E3）、`test/stubs/*`。

### 1.6 源码注释里的占位符写法（本轮确立）

源码注释中的板级占位符统一写 **`boards/&lt;板&gt;/…`**（Doxygen 转义；生成的文档里正确渲染为 `boards/<板>/`）。

**不能写 `boards/*/`**：`*/` 会提前闭合 C 块注释（直接编译失败），`/*` 会被 Doxygen 当成嵌套注释（引入 14 条新告警）。`boards/ * /`（星号两侧加空格）虽可编译，但渲染到文档里像笔误，不采用。

同理，其它占位符（`&lt;层&gt;`、`&lt;直烧板&gt;` 等）在源码注释里也用 HTML 转义。

---

## 2. B 类：Doxygen 化

### 2.1 缺口（启发式计数，执行时逐条核对）

**Doxygen 告警基线（2026-09-23 首次生成）：1197 条** ← 每批改造后的验收计分器（只增不减即为回归）。

另有 12 处 `<>` 被 Doxygen 当作 HTML 标签解析（注释里写 `boards/<板>/` 这类），需转义为 `boards/&lt;板&gt;/` 或包进 `@c`；归入 B0。

| 项 | 缺口 |
|---|---|
| `@file` 文件头 | 17 个自研头（`.c` 文件头另需补查） |
| 导出符号 `@brief` | 921 − 265 ≈ **656** |
| 非 void 缺 `@return`/`@retval` | **145** |
| 非 const 指针参数缺方向 | **168 处裸 `@param`，方向标注全仓 0 处** |
| 成员/宏/枚举 `/**<` | 614 − 31 ≈ **583** |
| `@defgroup`/`@warning`/`@see` | 0（本轮不加 group） |

最大单点：`Device/Inc/Network/dev_dp83848.h`（176 导出、0 `@brief`、154 宏无文档）。

### 2.2 分批（按价值排序，每批可独立验收）

| 批 | 范围 | 动作 |
|---|---|---|
| **B0** | 全部自研 `.c`/`.h` 文件头 | 补/规范 `@file` + `@brief` |
| **B1** | 15 个优先契约头（见下） | 导出符号 `@brief` + `@return` + **参数方向** + 成员 `/**<` |
| **B2** | 其余头文件的导出符号 | `@brief` + `@return` + 方向 |
| **B3** | 用法归一 | `@retval` 仅用于"枚举具体取值"，其余改 `@return`（§12） |

**B1 的 15 个契约头**：`app_dispatch.h`、`dev_storage.h`、`app_screen.h`、`app_render.h`、`dev_display.h`、`dev_cfg_record.h`、`ring_buffer.h`、`pl_eth.h`、`app_casc.h`、`app_iap_cmd.h`、`app_ldi.h`、`app_ldi_cmd.h`、`app_rls.h`、`app_ahmq.h`、`board.h`（两块板）。

### 2.3 参数方向的判定规则（本次唯一的"判断点"）

> 只对**会被函数写入**的参数标 `@param[out]` / `@param[in,out]`；`const T*` 与按值参数不标。

执行者需逐函数判定"这个参数是否被写"；**不确定的列出来汇报，不要猜**。

### 2.4 不做

- 不重写设计论述（`app_screen.h:1-18`、`app_dispatch.h:1-22`、契约交叉引用等）
- 不加 `@defgroup`/`@ingroup`
- 不动 `lwipopts.h`、`Compiler/startup.h`、`test/stubs/*`

---

## 3. C 类：`Doxyfile` 与生成

关键配置（执行时按此写）：

```
RECURSIVE        = YES
FILE_PATTERNS    = *.h *.c
EXCLUDE          = Drivers Middlewares boards/*/Core build test/stubs Platform/Inc/lwipopts.h
EXTRACT_ALL      = NO
WARN_IF_UNDOCUMENTED = YES      # 把缺口当"计分器"
OUTPUT_DIRECTORY = build/doxygen
GENERATE_HTML    = YES
GENERATE_XML     = YES
```

每批结束后生成一次，确认无坏引用、无意外告警（作为验收手段）。输出在 `build/` 下，已被 `.gitignore` 排除。

---

## 4. 执行与验证

- 注释改动不影响编译，但每批仍跑 `make test` + 两个固件目标（防手滑改到代码）
- 分批提交；`Doxyfile` 单独一个 commit
- 每批结束条件：该批范围内 §12 五类标签零缺口（用 `grep`/Doxygen 告警交叉核对）

---

## 5. 已裁决（2026-09-23）

| # | 事项 | 结论 |
|---|---|---|
| 1 | **范围** | **A + B0 + B1**（分批推进）：先做陈旧注释清扫、文件头 `@file`/`@brief`、再 15 个契约头的完整标签。B2/B3 视首批效果再定 |
| 2 | **`Doxyfile` 归属** | **仓库根**（`doxygen Doxyfile` 直接可用；输出 `build/doxygen/` 已被忽略） |
| 3 | **`s_tcp_server_connected`** | **删掉**（连同"调试变量"注释）；需要时用日志代替 |

---
