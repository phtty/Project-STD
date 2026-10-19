# CDSTD 显示开关（A0H）与亮度调节（A2H）—— 设计待落

> **状态：门面 API 已落（2026-10-19）**；A0H/A2H **命令层**待实现。
> 适用面：跑 CDSTD 的产品（当前 5006048 折叠屏）；亮度/光敏这条路径在单卡板（3833024，LDI）同样在用。

## 1. 调查结论（事实，带位置）

- **工程没有独立的"屏开关"接口**：
  - `dev_display_t`（`Device/Inc/Display/dev_display.h:44-80`）只有几何参数、缓冲指针、`light_level`、脏标记；**无 on/off / blank / enable / power 字段**，`dev_display_ops` 也只有 `prepare/scan/set_row`。
  - 模组实例（`boards/<板>/Device/Src/dev_p10_112x10_*.c:156-173` 一类）= `dev_display_t base` 的指定初始化器 + 三个 ops，**模组级没有开关参数或机制**。
  - 平台层最接近的只有**无状态**的 `pl_hub75_oe_set(bool blank)`（`boards/*/Platform/Inc/pl_hub75.h`），被扫描行切换消隐与亮度 PWM 复用，表达不了"当前屏是关着的"。
- **"关屏" = 亮度 0**（三处代码已如此定义：`dev_display.h:158`、`app_light_sensor.h:20`、AHMQ 上报 `app_ahmq.c:201` 的 `light_level ? '1' : '0'`）。
  硬件落点：`Device/Display/dev_display.c:232-239` 的 PWM 回调 `pl_hub75_oe_set(s_pwm_cnt >= dev->light_level)` → `light_level = 0` 时 **OE 恒消隐**。**扫描任务与定时器照跑，不省电**。
- **唯一亮度入口** `app_screen_set_brightness(uint8_t 0..7)`（`Application/Src/Render/app_screen_brightness.c:20-34`；`>7` 钳到 7；置"待下发"标志）。`dev_display_set_brightness` 只改本卡实屏，**不能**用于命令层（从卡收不到）。
- **光敏会覆盖**：`app_light_sensor_task` 每 **1s** 调一次、**仅主卡**（`Application/Src/app_light_sensor.c:16-28`）。
  `app_light_sensor_set_fixed(level)` = 停跟随 + 设值（`:55-59`），`app_light_sensor_resume()` = 恢复跟随 + 立即调一次（`:61-65`）。
  光敏读取 `8 - temp/500` 钳 **1~7**、**永不返回 0**（`Device/IO/dev_light_sensor.c:37-40`）——"0 = 关屏"是留给命令的。
- **级联自动同步**：亮度走"整屏属性"通道 —— 主卡 `take_pending` → `app_casc_broadcast_bright`（`Application/Src/CASC/app_casc.c:1442`），从卡套用（`:512-521`），每轮 IMAGE 再断言一次（`:600-602`）。**屏开关借这条通道即可，不必新增机制**（广播"有效值"，关屏时就是 0）。
- 亮度**不落盘**，重启回模组默认（`light_level = 7`）。

## 2. 已定口径（用户，2026-10-16）

1. **开关与亮度分开**；关屏时**缓存**亮度，开屏**恢复**它。
2. 缓存策略 = **冻结在关屏那一刻的值**：关屏期间不跟随环境光；光敏保持"停跟随"，开屏后也**不自动恢复**——要恢复自动调光得发 A2 的 `0x30`（这条要写进注释与测试，免得后人当 bug）。
3. 开关状态放**门面层**（`app_screen`），**不**给 `dev_display_t` 加字段。理由：① 多卡"主卡统一关屏"只有门面层能做（实例字段不会广播）；② 今天写亮度的真实路径只有门面一条（`app_screen_brightness.c:24` 是 `dev_display_set_brightness` 的唯一真实调用者），够用；③ 光敏那条"`apply` 为 NULL 时直写 `display->light_level`"的旁路（`Device/IO/dev_light_sensor.c:68-70`）在本工程未启用（app 侧已设 `apply` 回调走门面）。将来若真出现绕过门面的写者，再上提为"实例加字段"的纵深防御。

## 3. 待实现清单（下次）

**门面层 —— ✅ 已完成（2026-10-19）**
- `app_screen_set_display_on(bool)` / `app_screen_is_display_on()`（声明 `Application/Inc/Render/app_screen.h`，实现 `Application/Src/Render/app_screen_brightness.c`；上电默认**开**）；
- `app_screen_set_brightness()` 施加 **`显示开 ? 期望值 : 0`**（期望值仍记在 `s_bright_level`，即"缓存"本身；`>7` 仍钳到 7）；
- `app_screen_brightness_take_pending()` 返回 **有效值**（关屏时为 0），级联据此让从卡一起灭；
- 新增 host 套件 `test/test_screen_brightness.c`（8 case / 33 断言——该 TU 此前**零覆盖**），已按 Makefile 四处登记（闸① R1 校验：19 套件 0 违规）；
- 反向验证：去掉关屏门控 → 失败 2；`take_pending` 退回返回期望值 → 失败 2；两板复跑均逐字节还原。

**命令层（CDSTD）**
- **A0H**：`0x30` 关闭 / `0x31` 打开；`data_len != 1` 或取值越界 → 回 `0x30` 失败（不改状态）；**重复关闭幂等且不得破坏冻结值**；应答 `0xB0`。
- **A2H**：`0x30`（自动调光）→ `app_light_sensor_resume()`；`0x31..0x38` → `code - 0x30` 后**钳到 7**（即 0x37 与 0x38 视觉相同）；**关屏期间只更新期望值，不点屏**；应答 `0xB2`。
- 关屏动作序列（冻结 + 停跟随 + 门控）：
  ```c
  uint8_t frozen = app_screen_get_brightness();
  app_light_sensor_set_fixed(frozen);   /* 停跟随、值保持 frozen */
  app_screen_set_display_on(false);     /* 门控：硬件施加 0，屏灭；frozen 留着 */
  ```
  开屏：`app_screen_set_display_on(true);`（硬件回到 frozen）。

**测试与验证（照 `docs/测试约定.md` §3）**
- `case_cmd_screen_switch`、`case_cmd_light_adjust` + 各自越界矩阵 + 在 `case_task_dispatch` 加贯通格；
- **变异集补两个必须被抓住的变异体**：① 去掉关屏门控（光敏会把屏点亮）② 档位映射少一档；
- 双板全套 + 两板固件 + 逐处反向验证（改的是整屏门面，要跑全套）。

## 4. 未决 / 待确认

- **A0H 是否只在主卡生效**（重要，实现命令层时必须二选一）：从卡的 `s_display_on` 只由它自己收到 A0H 才改变。若命令落到从卡，它的门控会把主卡广播的 >0 亮度压成 0 → **该卡一直黑**。取值：① 只在主卡执行（命令本就发给主控卡，最简单）；② 从卡收到时也改自己的开关并同步广播。当前按 ① 设计。
- A2 的 8 档（`0x31..0x38`）→ 7 档的合并方式：按"`code - 0x30` 后钳到 7"实现（0x37 与 0x38 视觉相同）。用户未否，若协议方另有口径需改。
- 关屏后 AHMQ 的状态上报仍读硬件亮度（`light_level ? '1' : '0'`），语义自动一致，无需改动。
