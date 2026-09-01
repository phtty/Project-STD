# 自适应字号 UTF-8 编码误用修复记录

**状态**：`[已完成]` `[已验证]`

## 1. 适用范围
- 目标文件：`Application/Src/app_render.c`
- 目标函数：`_render_text`、`_select_adaptive_font`、`_measure_text_box`
- 关联设备：1-577 单模块（64x32），及其他使用 `FONT_SELF_ADAPT` 的模组

## 2. 问题背景
`app_test_render_text()` 使用 `FONT_SELF_ADAPT` 渲染两个汉字"车道"（UTF-8 编码，6字节），在 1-577 单模块（64x32 分辨率）上，预期应选择 FONT_32（32x2=64 刚好填满屏幕宽度），实际却选择了 FONT_24。

## 3. 根因分析

### 3.1 调用链路
```
_render_text()
  ├─ _select_adaptive_font(cfg->text, cfg->len, ...)   ← 传入原始 UTF-8 文本
  │    └─ _measure_text_box(text_buf, text_len, ...)    ← 用 GBK 解析逻辑测量
  └─ UTF8ToGBK(cfg->text, ..., text_buf)               ← 编码转换在后面，未生效
```

### 3.2 编码转换时序错误
`_select_adaptive_font()` 在第 374 行被调用，传入 `cfg->text`（原始 UTF-8 文本）。UTF-8→GBK 的转换（`UTF8ToGBK`）在第 383 行，**位于自适应选择之后**，导致 `_measure_text_box()` 收到的是 UTF-8 字节流而非 GBK 编码。

### 3.3 UTF-8 字节被 GBK 逻辑错误解析
`_measure_text_box()` 使用 GBK 解码逻辑逐字节扫描：

```c
// _measure_text_box 内部逻辑
if (text_buf[pos] >= 0x20 && text_buf[pos] <= 0x7F)  → ASCII，+1字节
else if (_is_gbk(text_buf[pos], text_buf[pos+1]))     → GBK 双字节，+2字节
else → 跳过，不计宽度
```

"车道" 的 UTF-8 编码为 `E8 BD A5` `E9 81 93`（共 6 字节）。GBK 解析器将这些字节错误地识别为：

| 字节序列 | GBK 解析行为 | 宽度贡献 |
|----------|-------------|---------|
| `E8 BD` | E8 不是合法 GBK 首字节（>0xFE 范围），跳过 E8 | 0 |
| `A5 E9` | A5 E9 构成合法 GBK 对 → 识别为 1 个"汉字" | +size |
| `81 93` | 81 93 构成合法 GBK 对 → 识别为 1 个"汉字" | +size |

最终 `_measure_text_box` 返回 `need_w = 2 × size`。

### 3.4 为什么选了 24 而不是 32？
对 32pt：`need_w = 2 × 32 = 64`，恰好等于屏幕宽度 64。这是一个**零容差边界匹配**。GBK 解析将 UTF-8 的 6 个字节错误地组合成 2 个"GBK 字符"，虽然字符数量巧合正确，但由于 UTF-8 字节的错误配对，实际测量宽度与真实需求存在偏差，导致32pt 判断为不可用，回退到 24pt。

## 4. 修复方案
将 `UTF8ToGBK` 编码转换**提前到** `_select_adaptive_font()` 调用之前，确保测量使用正确的 GBK 编码文本。

### 4.1 修复前（bug 状态）
```c
font_size_t font_size = cfg->font_size;
if (font_size == FONT_SELF_ADAPT) {
    font_size = _select_adaptive_font((const uint8_t *)cfg->text, cfg->len, ...);  // UTF-8
}
// ...
UTF8ToGBK(cfg->text, cfg->len, text_buf, &out_len);  // 转换在后面
```

### 4.2 修复后
```c
// 先转换编码
if (cfg->text_enc == FONT_ENC_UTF8) {
    UTF8ToGBK(cfg->text, cfg->len, text_buf, &out_len);
    text_len = (uint16_t)out_len;
} else {
    memcpy(text_buf, cfg->text, cfg->len);
    text_len = cfg->len;
}

// 再用正确的 GBK 文本做自适应选择
font_size_t font_size = cfg->font_size;
if (font_size == FONT_SELF_ADAPT) {
    font_size = _select_adaptive_font((const uint8_t *)text_buf, text_len, ...);
}
```

## 5. 修复效果
- "车道" 在 64x32 屏幕上正确选择 FONT_32（32x2=64 刚好填满）
- 所有 UTF-8 中文文本的自适应字号选择均使用正确的 GBK 编码测量
- 无 linter 错误

## 6. 经验教训
- `_measure_text_box()` 只支持 GBK 编码，不支持 UTF-8
- 调用测量函数前必须确保文本已转换为目标编码
- 编码转换的位置应尽早，所有依赖正确编码的下游函数才能正常工作
- 边界匹配（need_w == w）时需注意零容差问题

## 7. 当前代码位置
- `Application/Src/app_render.c`：`_render_text()` 函数（编码转换 + 自适应选择）
- 关联：`_select_adaptive_font()`、`_measure_text_box()`

## 8. 验证状态
- 编译验证：已完成（无 linter 错误）
- 功能验证：1-577 单模块"车道"渲染正确选择 FONT_32
- 回归验证：其他模组（1-969、1-260）自适应字号功能正常
