#!/usr/bin/env python3
"""test/check_conventions.py —— 测试套件结构规则检查器（`make lint` 的实体）

规则正文与例外登记见 docs/测试约定.md（本文不改它）；这里只实现其中**可机器强制**
的部分（R1~R11），每条违规都定位到 `文件:行号`。设计约束：

  * 纯文本解析，不依赖编译产物，不做编译 —— 秒级跑完，能挂在 `make test` 前面；
  * 例外走内建白名单（`--list-exceptions` 可打印），粒度精确到符号；
    白名单不是"免检"：登记的替代形态（如 fork 型汇总行）仍会被校验；
    R10 与 docs/测试约定.md §8 双向对账，R11 检测过期条目（不再抑制任何违规）；
  * 检查器自身的错误（Makefile 读不了、test: 规则找不到、§8 解析不出 E 编号）退出码 2，
    与"有违规"（1）区分。

用法：
    python3 test/check_conventions.py [--json] [--list-rules] [--list-exceptions]
                                      [--audit-exceptions] [--root <dir>]

退出码：0 = 全过；1 = 有违规；2 = 检查器自身错误。
"""

from __future__ import annotations

import argparse
import ast
import json
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------- 规则表
# (规则号, 一句说明, 机器校验手段)。顺序即汇报顺序，勿随意打乱（输出要可 diff）。

RULES: list[tuple[str, str, str]] = [
    (
        "R1",
        "套件登记完整性：磁盘 test/test_*.c、Makefile test: 依赖段、test: 运行段三者一致",
        "解析 Makefile 的 test: 规则，按 $(TEST_BUILD)/test_<name> 取依赖段与运行段的套件名，与磁盘文件集合比对（防编了不跑/漏登记）",
    ),
    (
        "R2",
        "断言宏必须是已登记变体 A/B/C 之一，不得出现新宏名或新形态（C 仅白名单文件）",
        "按 #define CHECK / CHECK_MSG / TEST_BEGIN 与 g_pass/g_fail/g_failures 判定变体，并扫描 CHECK*/TEST_BEGIN 命名空间的额外宏与反相/三参等形态",
    ),
    (
        "R3",
        "必须有标准汇总行 printf(\"\\n通过 %d，失败 %d\\n\", g_pass, g_fail) 与失败退出码",
        "在 main() 体内匹配汇总 printf 与 return g_fail ? 1 : 0（语义等价的 g_fail == 0 ? 0 : 1 亦认）；E1/E2 白名单按登记的替代形态校验",
    ),
    (
        "R4",
        "每个用例函数的紧邻上文注释块必须以 /** @brief 开头",
        "定位用例定义行，向上跳过空行取紧邻注释块，规范化开头后匹配 @brief",
    ),
    (
        "R5",
        "每个用例函数必须在同文件 main() 体内登记（顺序调用或表内引用）",
        "在 main() 体内按标识符查用例名",
    ),
    (
        "R6",
        "单个用例函数体内 CHECK( / CHECK_MSG( 的静态书写行数不得 > 20",
        "按括号配平取用例函数体，逐行计数（同一行多个只算一行）",
    ),
    (
        "R7",
        "CHECK_MSG(1, 成功落点所在函数必须另有能失败的真断言，且断言总数 ≥ 2",
        "按括号配平定位落点所在函数；须存在首参不是字面 1 的 CHECK(/CHECK_MSG(（矩阵走完且没红才落点，判据不绑死某一种失败形态）",
    ),
    (
        "R8",
        "不得掩盖失败：#if 0、被注释掉的 CHECK/CHECK_MSG 调用、早退 exit(0)",
        "词法扫描：注释文本正则 CHECK(_MSG)?\\s*\\(；代码正则 ^#if 0 与 exit(0)",
    ),
    (
        "R9",
        "会无限阻塞的写法（join/barrier/waitpid/fork/osWaitForever）必须有 alarm() 挂死保护",
        "有效代码中出现 pthread_join( / pthread_barrier_wait( / waitpid( / osThreadJoin( / fork( 或 osWaitForever（按标识符边界）时必须出现 alarm(；起线程本身（pl_task_new/osThreadNew）与有限超时的 osMessageQueueGet( 不算——有限超时不会无限等；一旦有人把它改成 osWaitForever，字面出现即触发，规则自纠",
    ),
    (
        "R10",
        "文档 §8 例外登记表与检查器内建例外表对账（防偷偷放水）",
        "解析 docs/测试约定.md §8 表格首列 E<n>；抽取检查器规则文本与例外表里的 §8 E<n> 引用；检查器引用而文档没有 → 报（检查器行号）；文档有且被检查器引用、但例外表缺条目 → 报（文档行号）；§8 解析不出任何 E 编号 → 退出码 2。与 R1~R9 不相交、且没有被任何机器规则文本引用的例外允许只存在于文档",
    ),
    (
        "R11",
        "例外过期检测：不再抑制任何违规的例外条目 = 过期",
        "逐条把某例外从生效集合移除后重跑对应规则；不再产生新违规 → 报该条已过期（--audit-exceptions 可单独排查）",
    ),
]

RULE_INDEX = {r[0]: i for i, r in enumerate(RULES)}

# ---------------------------------------------------------------- 例外白名单
# (规则号, 文件, 符号名或 None=整文件, 理由, 文档锚点)
# 粒度纪律：test/test_*.c 之外的整文件豁免只允许 R2/R3（宏与骨架变体）；
# 质量规则（R4/R6/R7/R8）只允许符号级。理由必须写明"为什么这是有意的、不是漏网"。

EXCEPTIONS: list[tuple[str, str, str | None, str, str]] = [
    (
        "R2",
        "test/test_ring_buffer.c",
        None,
        "有意：早期 g_failures 变体（反相 CHECK + 三参 CHECK_MSG + 无 TEST_BEGIN）按 E2 冻结不改写，改动只补 @brief",
        "docs/测试约定.md §8 E2",
    ),
    (
        "R2",
        "test/test_iap_cfg.c",
        None,
        "有意：同 E2 早期变体（含 __VA_OPT__ 三参 CHECK_MSG）；冻结，不重写宏",
        "docs/测试约定.md §8 E2",
    ),
    (
        "R3",
        "test/test_ring_buffer.c",
        None,
        "有意：E2 汇总行为 `=== 全部通过 ===`，退出码由 g_failures 分支给出；替代形态仍被校验",
        "docs/测试约定.md §8 E2",
    ),
    (
        "R3",
        "test/test_cfg_sched.c",
        None,
        "有意：fork 隔离型套件，子进程内汇总，父进程只能按退出码统计；替代形态（失败计数汇总 + 失败退出码）仍被校验",
        "docs/测试约定.md §8 E1",
    ),
    (
        "R3",
        "test/test_ldi_0ah.c",
        None,
        "有意：fork 隔离型套件，同上 E1",
        "docs/测试约定.md §8 E1",
    ),
    (
        "R3",
        "test/test_iap_cfg.c",
        None,
        "有意：fork 隔离型 + 早期变体（E1/E2 叠加），父进程只能按退出码汇总",
        "docs/测试约定.md §8 E1/E2",
    ),
]


# R11 审计用的"当前生效例外表"：正常流程恒等于 EXCEPTIONS；审计时临时抽掉一条重跑。
_ACTIVE_EXCEPTIONS: list[tuple[str, str, str | None, str, str]] = EXCEPTIONS


class CheckerError(Exception):
    """检查器自身的错误（缺文件、Makefile 结构不可解析等）→ 退出码 2。"""


# ---------------------------------------------------------------- 文本视图
# 三种视图各有用途，别混：
#   raw     原文                    —— 需要字符串内容（如汇总行的中文）时用
#   code    注释置空、字符串保留     —— 需要"注释不参与"的代码匹配时用
#   struct  注释与字符串都置空       —— 括号配平、标识符索引时用（字符串里可能有花括号）

def scan_views(text: str) -> tuple[str, str, list[tuple[int, int, str]]]:
    """返回 (code, struct, comments)，comments 为 [(起始行, 结束行, 文本)]（行号 1 基）。

    code   = 注释置空、字符串保留（汇总行的中文在字符串里，不能被抹掉）
    struct = 注释与字符串都置空（字符串里可能有花括号，不能参与配平）
    """
    out_code = list(text)
    out_struct = list(text)
    comments: list[tuple[int, int, str]] = []
    n = len(text)
    i = 0
    line = 1

    def blank(start: int, end: int, blank_code: bool, blank_struct: bool) -> None:
        for k in range(start, min(end, n)):
            if blank_code and out_code[k] != "\n":
                out_code[k] = " "
            if blank_struct and out_struct[k] != "\n":
                out_struct[k] = " "

    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in "\"'":
            start = i
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    if i + 1 < n and text[i + 1] == "\n":
                        line += 1
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                if text[i] == "\n":
                    line += 1
                i += 1
            blank(start, i, blank_code=False, blank_struct=True)  # 字符串只对 struct 无效
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            start = i
            start_line = line
            j = i
            while j < n and text[j] != "\n":
                j += 1
            comments.append((start_line, start_line, text[start:j]))
            blank(start, j, blank_code=True, blank_struct=True)
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            start = i
            start_line = line
            j = i + 2
            while j < n and not (text[j] == "*" and j + 1 < n and text[j + 1] == "/"):
                if text[j] == "\n":
                    line += 1
                j += 1
            end = min(j + 2, n)
            comments.append((start_line, line, text[start:end]))
            blank(start, end, blank_code=True, blank_struct=True)
            i = end
            continue
        i += 1

    return "".join(out_code), "".join(out_struct), comments


def rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


CHECKER_REL = "test/check_conventions.py"


def is_exempt(rule: str, file: str, symbol: str | None = None) -> bool:
    """整文件豁免（symbol=None）命中任何符号级查询；符号级豁免只命中同名符号。

    `_ACTIVE_EXCEPTIONS` 让 R11 审计可以临时关掉某一条后重跑 —— 正常流程下恒等于
    EXCEPTIONS；除此之外不要改它（单线程脚本，够用且不必把表穿透到每个检查函数）。
    """
    for r, f, s, _reason, _anchor in _ACTIVE_EXCEPTIONS:
        if r != rule or f != file:
            continue
        if s is None or s == symbol:
            return True
    return False


# ---------------------------------------------------------------- 括号配平
def brace_end(lines: list[str], open_idx: int) -> int:
    """从含左花括号的 open_idx 行起配平，返回右花括号所在行（0 基）。"""
    depth = 0
    for i in range(open_idx, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if depth == 0:
            return i
    return len(lines) - 1


def open_brace_of(lines: list[str], sig_idx: int) -> int | None:
    """签名行的左花括号：同行 ')' 后，或紧随其后第一个以 '{' 开头的非空行。"""
    if "{" in lines[sig_idx]:
        return sig_idx
    for i in range(sig_idx + 1, min(sig_idx + 6, len(lines))):
        s = lines[i].strip()
        if not s:
            continue
        if s.startswith("{"):
            return i
        break
    return None


def enclosing_open_brace(lines: list[str], line_idx: int) -> int | None:
    """从 line_idx 行向上找包住它的最外层左花括号所在行（0 基）。"""
    depth = 0
    for i in range(line_idx, -1, -1):
        row = lines[i]
        for j in range(len(row) - 1, -1, -1):
            ch = row[j]
            if ch == "}":
                depth += 1
            elif ch == "{":
                depth -= 1
                if depth < 0:
                    return i
    return None


def function_name_at(lines: list[str], brace_idx: int) -> str | None:
    """在左花括号上方找最近的函数名（签名可能跨行，向上拼到出现 '(' 为止）。"""
    buf = lines[brace_idx].split("{", 1)[0]
    k = brace_idx - 1
    while "(" not in buf and k >= 0:
        buf = lines[k] + " " + buf
        k -= 1
    found = re.findall(r"([A-Za-z_]\w*)\s*\(", buf)
    return found[-1] if found else None


def function_span(lines: list[str], line_idx: int) -> tuple[str | None, int, int] | None:
    """line_idx 行所在函数的 (名字, 起始行, 结束行)（0 基、含）。"""
    b = enclosing_open_brace(lines, line_idx)
    if b is None:
        return None
    return function_name_at(lines, b), b, brace_end(lines, b)


# ---------------------------------------------------------------- 用例函数
CASE_SIG_RE = re.compile(
    r"^\s*(?!\s*#)(?:static\s+)?(?:inline\s+)?[A-Za-z_][A-Za-z0-9_\s]*?[*\s]\s*"
    r"(case_[A-Za-z0-9_]+|test_[A-Za-z0-9_]+)\s*\(\s*void\s*\)\s*(\{)?\s*$"
)


def find_cases(struct_lines: list[str]) -> list[dict]:
    """定位所有 `case_*` / `test_*`（void 参数）的函数定义与其函数体。"""
    cases: list[dict] = []
    for idx, line in enumerate(struct_lines):
        m = CASE_SIG_RE.match(line)
        if not m:
            continue
        name = m.group(1)
        ob = idx if m.group(2) else open_brace_of(struct_lines, idx)
        if ob is None:
            continue
        cases.append({"name": name, "sig_line": idx + 1, "body_lines": (ob + 1, brace_end(struct_lines, ob) + 1)})
    return cases


# ---------------------------------------------------------------- 规则实现
def add(violations: list[dict], file: str, line: int, rule: str, message: str, symbol: str | None = None) -> None:
    violations.append({"file": file, "line": line, "rule": rule, "symbol": symbol, "message": message})


# ---- R1 ------------------------------------------------------------------
def check_r1(root: Path, mf_lines: list[str], suite_files: list[str], violations: list[dict]) -> None:
    test_line_idx = None
    for i, line in enumerate(mf_lines):
        if re.match(r"^test:", line):
            test_line_idx = i
            break
    if test_line_idx is None:
        raise CheckerError("Makefile 里找不到 `test:` 规则（R1 无法判定登记完整性）")

    # 依赖段 = test: 逻辑行（含 '\' 续行）；运行段 = 紧随其后的制表符配方行。
    dep_line_idx: dict[str, int] = {}
    run_line_idx: dict[str, int] = {}
    i = test_line_idx
    logical = mf_lines[i]
    dep_end = i
    while logical.rstrip().endswith("\\") and i + 1 < len(mf_lines):
        i += 1
        logical = mf_lines[i]
        dep_end = i
    for j in range(test_line_idx, dep_end + 1):
        for m in re.finditer(r"\$\(TEST_BUILD\)/(test_\w+)", mf_lines[j]):
            dep_line_idx.setdefault(m.group(1), j + 1)
    j = dep_end + 1
    while j < len(mf_lines) and mf_lines[j].startswith("\t"):
        for m in re.finditer(r"\$\(TEST_BUILD\)/(test_\w+)", mf_lines[j]):
            run_line_idx.setdefault(m.group(1), j + 1)
        j += 1

    disk = {Path(f).stem for f in suite_files}
    dep = set(dep_line_idx)
    run = set(run_line_idx)

    for name in sorted(disk - dep):
        add(violations, "Makefile", test_line_idx + 1, "R1",
            f"套件 {name}.c 在磁盘上存在，但未登记进 test: 依赖段（编了不跑的第一步）", name)
    for name in sorted(dep - disk):
        add(violations, "Makefile", dep_line_idx[name], "R1",
            f"test: 依赖了磁盘上不存在的套件 {name}（文件已删/改名？）", name)
    for name in sorted(dep - run):
        add(violations, "Makefile", dep_line_idx[name], "R1",
            f"套件 {name} 已编进 test: 依赖，但运行段没有执行它 —— 编了不跑", name)
    for name in sorted(run - dep):
        add(violations, "Makefile", run_line_idx[name], "R1",
            f"运行段执行了未列入 test: 依赖的套件 {name}", name)


# ---- R2 ------------------------------------------------------------------
def macro_def(code_lines: list[str], name: str) -> tuple[int, str] | None:
    """取 `#define <name>(...)` 的续行合并文本，返回 (定义行 1 基, 合并文本)。"""
    pat = re.compile(r"^\s*#\s*define\s+" + re.escape(name) + r"\s*\(")
    for i, line in enumerate(code_lines):
        if not pat.match(line):
            continue
        parts = [line]
        j = i
        while parts[-1].rstrip().endswith("\\") and j + 1 < len(code_lines):
            j += 1
            parts.append(code_lines[j])
        joined = "\n".join(p[:-1].rstrip() if p.rstrip().endswith("\\") else p.rstrip() for p in parts)
        return i + 1, joined
    return None


def macro_params(joined: str) -> str:
    m = re.search(r"#\s*define\s+\w+\s*\((.*?)\)", joined, re.S)
    return m.group(1) if m else ""


def check_r2(rel_file: str, code: str, code_lines: list[str], violations: list[dict]) -> None:
    if is_exempt("R2", rel_file):
        return

    has_check = bool(re.search(r"^\s*#\s*define\s+CHECK\s*\(", code, re.M))
    has_cm = bool(re.search(r"^\s*#\s*define\s+CHECK_MSG\s*\(", code, re.M))
    has_tb = bool(re.search(r"^\s*#\s*define\s+TEST_BEGIN\s*\(", code, re.M))
    has_g_pass = bool(re.search(r"\bg_pass\b", code))
    has_g_fail = bool(re.search(r"\bg_fail\b", code))
    has_g_failures = bool(re.search(r"\bg_failures\b", code))

    # 额外断言宏名（尤其 CHECK_* 包裹宏）：一律报，要求先登记进 docs/测试约定.md §8
    for m in re.finditer(r"^\s*#\s*define\s+((?:CHECK|TEST_BEGIN|EXPECT|ASSERT|VERIFY|REQUIRE)[A-Za-z0-9_]*)\s*\(", code, re.M):
        name = m.group(1)
        if name in ("CHECK", "CHECK_MSG", "TEST_BEGIN"):
            continue
        line = code[: m.start()].count("\n") + 1
        add(violations, rel_file, line, "R2",
            f"出现未登记的断言宏名 {name} —— 新宏必须先登记进 docs/测试约定.md §8 再使用", name)

    variant = None
    if has_g_failures and not has_g_pass and not has_tb and has_check and has_cm:
        variant = "C"
    elif not has_g_failures and has_g_pass and has_g_fail and has_cm and has_tb:
        variant = "A" if has_check else "B"
    if variant is None:
        add(violations, rel_file, 1, "R2",
            "断言宏组合不属于已登记变体 A/B/C（A: CHECK+CHECK_MSG+TEST_BEGIN；B: 仅 CHECK_MSG+TEST_BEGIN；C: 早期 g_failures）")
        return

    if variant == "C":
        d = macro_def(code_lines, "CHECK_MSG")
        add(violations, rel_file, d[0] if d else 1, "R2",
            "早期 g_failures 变体（C）只允许白名单：test_ring_buffer.c、test_iap_cfg.c（docs/测试约定.md §8 E2）")
        return

    # A/B 形态：计数必须落在 g_pass 上；CHECK_MSG 不得是三参变体；TEST_BEGIN 必须打 ▶
    for name in ("CHECK", "CHECK_MSG"):
        d = macro_def(code_lines, name)
        if d and "g_pass" not in d[1]:
            add(violations, rel_file, d[0], "R2",
                f"{name} 的定义没有 g_pass 计数（反相/异常形态；E2 变体只允许白名单文件）", name)
    d = macro_def(code_lines, "CHECK_MSG")
    if d and "fmt" in macro_params(d[1]):
        add(violations, rel_file, d[0], "R2",
            "三参 CHECK_MSG(cond, fmt, ...) 属早期变体（E2），不得扩散", "CHECK_MSG")
    d = macro_def(code_lines, "TEST_BEGIN")
    if d and "▶" not in d[1]:
        add(violations, rel_file, d[0], "R2", "TEST_BEGIN 形态异常（应打印 ▶ 与用例名）", "TEST_BEGIN")


# ---- R3 ------------------------------------------------------------------
STD_SUMMARY_RE = re.compile(r'printf\s*\(\s*"\\n通过 %d，失败 %d\\n"\s*,\s*g_pass\s*,\s*g_fail\s*\)')
STD_EXIT_RE = re.compile(r"return\s+g_fail\s*\?\s*1\s*:\s*0\s*;|return\s+g_fail\s*==\s*0\s*\?\s*0\s*:\s*1\s*;")


def check_r3(rel_file: str, mains: list[dict], violations: list[dict]) -> None:
    def std_ok(body: str) -> tuple[bool, bool]:
        return bool(STD_SUMMARY_RE.search(body)), bool(STD_EXIT_RE.search(body))

    def e1_ok(body: str) -> bool:
        # fork 型：子进程内汇总（printf 里含 "失败 %d"），父进程按失败计数退出。
        return bool(re.search(r"printf\s*\([^;]*失败\s*%d", body, re.S)) and bool(
            re.search(r"return\s+[^;]*fail", body)
        )

    def e2_ok(body: str) -> bool:
        # 早期变体：`=== 全部通过 ===` + g_failures 分支给退出码。
        return ("=== 全部通过 ===" in body and "g_failures" in body
                and re.search(r"return\s+1\s*;", body) is not None
                and re.search(r"return\s+0\s*;", body) is not None)

    if not mains:
        add(violations, rel_file, 1, "R3", "找不到 main()，无法校验汇总行与退出码")
        return

    for m in mains:
        body = m["code_body"]
        if is_exempt("R3", rel_file):
            if not (e1_ok(body) or e2_ok(body)):
                add(violations, rel_file, m["line"], "R3",
                    "白名单（E1/E2）登记的替代形态也不满足：需要子进程内失败汇总 + 按失败计数退出，或 `=== 全部通过 ===` + g_failures 退出码")
            continue
        has_summary, has_exit = std_ok(body)
        if not has_summary:
            add(violations, rel_file, m["line"], "R3",
                '缺少标准汇总行 printf("\\n通过 %d，失败 %d\\n", g_pass, g_fail)')
        if not has_exit:
            add(violations, rel_file, m["line"], "R3",
                "缺少失败退出码（须为 return g_fail ? 1 : 0，或其等价形态 g_fail == 0 ? 0 : 1）")


# ---- R4 ------------------------------------------------------------------
COMMENT_NORM_BRIEF_RE = re.compile(r"@brief(?![A-Za-z0-9_])")


def check_r4(rel_file: str, raw_lines: list[str], comments: list[tuple[int, int, str]],
             cases: list[dict], violations: list[dict]) -> None:
    for case in cases:
        sig = case["sig_line"]
        nearest = None
        for start, end, text in comments:
            if end < sig and (nearest is None or end > nearest[1]):
                nearest = (start, end, text)
        if nearest is None:
            add(violations, rel_file, sig, "R4",
                "用例函数上方缺少紧邻的注释块（必须以 /** @brief 开头）", case["name"])
            continue
        _start, end, text = nearest
        if any(l.strip() for l in raw_lines[end: sig - 1]):
            add(violations, rel_file, sig, "R4",
                "用例函数与上方注释块之间夹着代码/另一段注释，不是紧邻的注释块", case["name"])
            continue
        norm = text[3:] if text.startswith("/**") else text
        norm = norm.lstrip(" \t\n*")
        if not COMMENT_NORM_BRIEF_RE.match(norm):
            add(violations, rel_file, sig, "R4",
                "用例函数的紧邻注释块不是以 /** @brief 开头", case["name"])


# ---- R5 ------------------------------------------------------------------
MAIN_RE = re.compile(r"^\s*(?:int|void)\s+main\s*\(\s*(?:void)?\s*\)\s*(\{)?\s*$")
TABLE_RE = re.compile(r"\b(\w+)\s*\[\s*\]\s*=\s*\{")


def collect_mains(struct_lines: list[str], code_lines: list[str]) -> list[dict]:
    mains: list[dict] = []
    for i, line in enumerate(struct_lines):
        if not MAIN_RE.match(line):
            continue
        ob = open_brace_of(struct_lines, i)
        if ob is None:
            continue
        end = brace_end(struct_lines, ob)
        mains.append({
            "line": i + 1,
            "struct_body": "\n".join(struct_lines[ob: end + 1]),
            "code_body": "\n".join(code_lines[ob: end + 1]),
        })
    return mains


def collect_tables(struct_lines: list[str]) -> list[tuple[str, str]]:
    """文件级/函数内的 `name[] = { ... }` 初始化块 —— fork 型套件的登记表可能在 main 之外。"""
    text = "\n".join(struct_lines)
    tables: list[tuple[str, str]] = []
    for m in TABLE_RE.finditer(text):
        open_pos = text.rindex("{", 0, m.end())
        depth = 0
        for k in range(open_pos, len(text)):
            if text[k] == "{":
                depth += 1
            elif text[k] == "}":
                depth -= 1
                if depth == 0:
                    tables.append((m.group(1), text[open_pos: k + 1]))
                    break
    return tables


def check_r5(rel_file: str, mains: list[dict], tables: list[tuple[str, str]],
             cases: list[dict], violations: list[dict]) -> None:
    if not mains:
        add(violations, rel_file, 1, "R5", "找不到 main()，用例登记无法校验")
        return
    for case in cases:
        pat = re.compile(r"\b" + re.escape(case["name"]) + r"\b")
        if any(pat.search(m["struct_body"]) for m in mains):
            continue
        # fork 型套件：登记表可能定义在 main 之外，但必须被某个 main 引用。
        registered = False
        for tbl_name, tbl_body in tables:
            if not pat.search(tbl_body):
                continue
            tpat = re.compile(r"\b" + re.escape(tbl_name) + r"\b")
            if any(tpat.search(m["struct_body"]) for m in mains):
                registered = True
                break
        if not registered:
            add(violations, rel_file, case["sig_line"], "R5",
                "用例函数没有在 main() 体内登记（顺序调用或表内引用）", case["name"])


# ---- R6 ------------------------------------------------------------------
ASSERT_LINE_RE = re.compile(r"(?<![A-Za-z0-9_])(CHECK|CHECK_MSG)\s*\(")


def check_r6(rel_file: str, code_lines: list[str], cases: list[dict], violations: list[dict]) -> None:
    for case in cases:
        first, last = case["body_lines"]
        count = sum(1 for i in range(first - 1, last) if ASSERT_LINE_RE.search(code_lines[i]))
        if count > 20:
            add(violations, rel_file, case["sig_line"], "R6",
                f"用例体内断言静态书写行数 {count} > 20，应拆分为多个 case", case["name"])


# ---- R7 ------------------------------------------------------------------
SUCCESS_LANDING_RE = re.compile(r"(?<![A-Za-z0-9_])CHECK_MSG\s*\(\s*1\s*,")
# "能失败的真断言"：CHECK/CHECK_MSG 的首参不是字面 1（1U/1L 也算字面 1）。
FAILABLE_ASSERT_RE = re.compile(r"(?<![A-Za-z0-9_])(CHECK|CHECK_MSG)\s*\(\s*(?!1[UuLl]*\s*(?:,|\)))")
ANY_ASSERT_RE = re.compile(r"(?<![A-Za-z0-9_])(CHECK|CHECK_MSG)\s*\(")


def check_r7(rel_file: str, code_lines: list[str], struct_lines: list[str], violations: list[dict]) -> None:
    for i, line in enumerate(code_lines):
        if not SUCCESS_LANDING_RE.search(line):
            continue
        span = function_span(struct_lines, i)
        if span is None:
            add(violations, rel_file, i + 1, "R7",
                "CHECK_MSG(1, 落点不在任何函数体内（无法确认失败路径与矩阵非空）", "CHECK_MSG")
            continue
        name, first, last = span
        body = "\n".join(code_lines[first: last + 1])
        has_failable = FAILABLE_ASSERT_RE.search(body) is not None
        asserts = len(ANY_ASSERT_RE.findall(body))
        if not has_failable:
            add(violations, rel_file, i + 1, "R7",
                "成功落点所在函数没有能失败的真断言（其余断言首参都是字面 1）—— 落点恒真", name)
        if asserts < 2:
            add(violations, rel_file, i + 1, "R7",
                f"成功落点所在函数断言总数 {asserts} < 2（落点覆盖的矩阵不得为空）", name)


# ---- R8 ------------------------------------------------------------------
COMMENTED_CHECK_RE = re.compile(r"(?<![A-Za-z0-9_])CHECK(?:_MSG)?\s*\(")
IF_ZERO_RE = re.compile(r"^\s*#\s*if\s+0\b")
EXIT_ZERO_RE = re.compile(r"(?<![A-Za-z0-9_])_?exit\s*\(\s*0\s*\)")


def check_r8(rel_file: str, struct_lines: list[str], comments: list[tuple[int, int, str]],
             violations: list[dict]) -> None:
    for start, _end, text in comments:
        for m in COMMENTED_CHECK_RE.finditer(text):
            line = start + text[: m.start()].count("\n")
            add(violations, rel_file, line, "R8",
                "注释里残留了被注释掉的断言调用（掩盖失败：要么恢复、要么删干净）", m.group(0).rstrip("("))
    for i, line in enumerate(struct_lines):
        if IF_ZERO_RE.match(line):
            add(violations, rel_file, i + 1, "R8", "出现 #if 0：整块用例被静默关闭")
        for m in EXIT_ZERO_RE.finditer(line):
            add(violations, rel_file, i + 1, "R8", "早期 exit(0)：失败路径被掩盖为成功", m.group(0))


# ---- R9 ------------------------------------------------------------------
# 只收"真的会无限等"的形态：join / barrier / waitpid / fork / osWaitForever。
# 起线程本身（pl_task_new/osThreadNew）与有限超时的 osMessageQueueGet( 不算 ——
# 有限超时不会无限阻塞；有人把超时改成 osWaitForever 时字面出现即触发（规则自纠）。
HANG_TRIGGER_RE = re.compile(
    r"(?<![A-Za-z0-9_])(pthread_join|pthread_barrier_wait|waitpid|osThreadJoin|fork)\s*\("
)
INFINITE_WAIT_TOKEN_RE = re.compile(r"(?<![A-Za-z0-9_])osWaitForever(?![A-Za-z0-9_])")
ALARM_RE = re.compile(r"(?<![A-Za-z0-9_])alarm\s*\(")


def check_r9(rel_file: str, code: str, violations: list[dict]) -> None:
    # 按字面规则扫描有效代码（注释与字符串已置空）。
    triggers: dict[str, int] = {}
    for m in HANG_TRIGGER_RE.finditer(code):
        line = code[: m.start()].count("\n") + 1
        triggers.setdefault(m.group(1), line)
    for m in INFINITE_WAIT_TOKEN_RE.finditer(code):
        line = code[: m.start()].count("\n") + 1
        triggers.setdefault("osWaitForever", line)
    if triggers and not ALARM_RE.search(code):
        for name, line in sorted(triggers.items(), key=lambda kv: kv[1]):
            display = name if name == "osWaitForever" else name + "("
            add(violations, rel_file, line, "R9",
                f"出现 {display} 但整份套件没有 alarm() 挂死保护（会无限等的写法必须能超时退出）", name)


# ---------------------------------------------------------------- R10：文档 ↔ 检查器例外表对账
DOC_REL = "docs/测试约定.md"


def _module_list_tables(tree: ast.Module) -> list[tuple[str, ast.List]]:
    """模块级 `NAME: list[...] = [...]` / `NAME = [...]` 两种写法都收（AnnAssign 与 Assign）。"""
    out: list[tuple[str, ast.List]] = []
    for node in tree.body:
        if isinstance(node, ast.AnnAssign):
            tgt, value = node.target, node.value
        elif isinstance(node, ast.Assign) and len(node.targets) == 1:
            tgt, value = node.targets[0], node.value
        else:
            continue
        if isinstance(tgt, ast.Name) and isinstance(value, ast.List):
            out.append((tgt.id, value))
    return out


def parse_doc_exceptions(root: Path) -> dict[str, int]:
    """解析 docs/测试约定.md §8 表格首列的 E<n> → 行号；格式被破坏时抛 CheckerError（退出码 2）。"""
    path = root / DOC_REL
    if not path.is_file():
        raise CheckerError(f"R10 需要 {DOC_REL}，但找不到：{path}")
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    start = next((i for i, l in enumerate(lines) if re.match(r"^##\s*8[\.\s]", l)), None)
    if start is None:
        raise CheckerError(f"{DOC_REL} 里找不到 §8 例外登记表（标题形如 `## 8. …`）")
    end = len(lines)
    for i in range(start + 1, len(lines)):
        if re.match(r"^##\s", lines[i]):
            end = i
            break
    found: dict[str, int] = {}
    for i in range(start, end):
        m = re.match(r"^\|\s*(E\d+)\s*\|", lines[i])
        if m:
            found.setdefault(m.group(1), i + 1)
    if not found:
        raise CheckerError(f"{DOC_REL} §8 表格格式被破坏：解析不出任何 E 编号（R10 不静默通过）")
    return found


def checker_e_refs() -> list[tuple[str, int, str]]:
    """检查器自身文本里引用的 §8 E<n>：(来源 RULES/EXCEPTIONS, 定义行, E 编号)。

    用 ast 取每个表元素的定义行 —— 不靠字符串搜索，重排/编辑注释都不会错位。
    """
    path = Path(__file__).resolve()
    src = path.read_text(encoding="utf-8")
    tree = ast.parse(src, filename=str(path))
    refs: list[tuple[str, int, str]] = []
    for name, lst in _module_list_tables(tree):
        if name not in ("RULES", "EXCEPTIONS"):
            continue
        for el in lst.elts:
            seg = ast.get_source_segment(src, el) or ""
            for m in re.finditer(r"(?<![A-Za-z0-9_])E(\d+)(?![0-9])", seg):
                refs.append((name, el.lineno, f"E{m.group(1)}"))
    return refs


def checker_exception_lines() -> list[int]:
    """EXCEPTIONS 列表每个元素的定义行（1 基），供 R11 定位。"""
    path = Path(__file__).resolve()
    src = path.read_text(encoding="utf-8")
    tree = ast.parse(src, filename=str(path))
    for name, lst in _module_list_tables(tree):
        if name == "EXCEPTIONS":
            return [el.lineno for el in lst.elts]
    return []


def check_r10(root: Path, violations: list[dict]) -> None:
    doc = parse_doc_exceptions(root)  # 解析失败 → CheckerError → 退出码 2
    refs = checker_e_refs()
    table_ids = {eid for kind, _line, eid in refs if kind == "EXCEPTIONS"}

    for _kind, line, eid in refs:
        if eid not in doc:
            # 检查器有、文档没有：锚点失效 / 编号漂移。
            add(violations, CHECKER_REL, line, "R10",
                f"检查器文本引用了 {eid}，但 {DOC_REL} §8 里没有该例外（锚点失效/编号漂移）", eid)
        elif eid not in table_ids:
            # 文档有、检查器也谈到（规则说明里），但例外表没有条目。
            # E3~E7 之类与 R1~R9 不相交、且在检查器里完全没被谈到的例外不在此列。
            add(violations, DOC_REL, doc[eid], "R10",
                f"{eid} 被检查器的规则说明引用（{CHECKER_REL}:{line}），但例外表没有对应条目 —— 要么补条目，要么不要在机器规则文本里引用它", eid)


# ---------------------------------------------------------------- R11：例外过期检测
def check_r11(inputs: dict, baseline: list[dict], violations: list[dict]) -> None:
    global _ACTIVE_EXCEPTIONS
    saved = _ACTIVE_EXCEPTIONS
    exc_lines = checker_exception_lines()
    try:
        for idx, exc in enumerate(EXCEPTIONS):
            rule, file, symbol, _reason, anchor = exc
            # 文件级例外只重跑对应套件；R1/Makefile 之类就全量重跑。
            file_name = file.rsplit("/", 1)[-1]
            only = file_name if file_name in inputs["files"] else None

            _ACTIVE_EXCEPTIONS = [e for j, e in enumerate(EXCEPTIONS) if j != idx]
            fresh = run_rule_checks(inputs, only_file=only)

            def key(v: dict) -> tuple:
                return (v["file"], v["line"], v["rule"], v["symbol"] or "", v["message"])

            if only is None:
                base_keys = {key(v) for v in baseline}
                new_keys = {key(v) for v in fresh}
            else:
                base_keys = {key(v) for v in baseline if v["file"] == file}
                new_keys = {key(v) for v in fresh if v["file"] == file}
            suppressed = new_keys - base_keys
            relevant = [k for k in suppressed if k[2] == rule and (symbol is None or k[3] == symbol)]

            if not relevant:
                m_e = re.search(r"E\d+", anchor or "")
                add(violations, CHECKER_REL, exc_lines[idx] if idx < len(exc_lines) else 1, "R11",
                    f"例外已过期：{rule} × {file}" + (f" × {symbol}" if symbol else "")
                    + f" 当前不再抑制任何违规（该例外已无实际作用，应删除或补充说明；anchor={anchor}）",
                    m_e.group(0) if m_e else None)
    finally:
        _ACTIVE_EXCEPTIONS = saved


# ---------------------------------------------------------------- 主流程
class Inputs(dict):
    """load_inputs() 的返回：Makefile 行 + 每个套件的预解析视图。"""


def load_inputs(root: Path) -> Inputs:
    makefile = root / "Makefile"
    if not makefile.is_file():
        raise CheckerError(f"找不到 Makefile：{makefile}")
    mf_lines = makefile.read_text(encoding="utf-8", errors="replace").splitlines()

    test_dir = root / "test"
    suite_files = sorted(p.name for p in test_dir.glob("test_*.c"))
    if not suite_files:
        raise CheckerError(f"test/ 下没有 test_*.c 套件文件：{test_dir}")

    files: dict[str, dict] = {}
    for filename in suite_files:
        path = test_dir / filename
        try:
            raw = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:  # noqa: PERF203
            raise CheckerError(f"读不了套件文件 {path}：{exc}") from exc
        code, struct, comments = scan_views(raw)
        code_lines = code.splitlines()
        struct_lines = struct.splitlines()
        files[filename] = {
            "rel": rel(path, root),
            "code": code,
            "struct": struct,
            "code_lines": code_lines,
            "struct_lines": struct_lines,
            "raw_lines": raw.splitlines(),
            "comments": comments,
            "cases": find_cases(struct_lines),
            "mains": collect_mains(struct_lines, code_lines),
            "tables": collect_tables(struct_lines),
        }
    return Inputs(root=root, mf_lines=mf_lines, suite_files=suite_files, files=files)


def run_rule_checks(inputs: Inputs, only_file: str | None = None) -> list[dict]:
    """跑 R1~R9（R10/R11 是元规则，不在这里）。only_file 供 R11 审计只重跑相关套件。"""
    violations: list[dict] = []
    if only_file is None:
        check_r1(inputs["root"], inputs["mf_lines"], inputs["suite_files"], violations)
    for filename in inputs["suite_files"]:
        if only_file is not None and filename != only_file:
            continue
        f = inputs["files"][filename]
        check_r2(f["rel"], f["code"], f["code_lines"], violations)
        check_r3(f["rel"], f["mains"], violations)
        check_r4(f["rel"], f["raw_lines"], f["comments"], f["cases"], violations)
        check_r5(f["rel"], f["mains"], f["tables"], f["cases"], violations)
        check_r6(f["rel"], f["code_lines"], f["cases"], violations)
        check_r7(f["rel"], f["code_lines"], f["struct_lines"], violations)
        check_r8(f["rel"], f["struct_lines"], f["comments"], violations)
        check_r9(f["rel"], f["struct"], violations)
    return violations


def run_checks(root: Path, audit_exceptions: bool = False) -> tuple[list[dict], int]:
    inputs = load_inputs(root)
    baseline = run_rule_checks(inputs)

    if audit_exceptions:
        # 只输出 R11（便于单独排查过期条目），但内部仍要跑基线以判断"是否抑制了什么"。
        violations: list[dict] = []
        check_r11(inputs, baseline, violations)
    else:
        violations = list(baseline)
        check_r10(root, violations)
        check_r11(inputs, baseline, violations)

    violations.sort(key=lambda v: (RULE_INDEX.get(v["rule"], 99), v["file"], v["line"], v["symbol"] or ""))
    return violations, len(inputs["suite_files"])


# ---------------------------------------------------------------- 输出
def print_rules() -> None:
    for rid, desc, how in RULES:
        print(f"{rid} | {desc} | 校验手段：{how}")


def print_exceptions() -> None:
    print("规则 | 文件 | 符号 | 理由")
    for rid, file, symbol, reason, anchor in EXCEPTIONS:
        print(f"{rid} | {file} | {symbol or '（整文件）'} | {reason}（{anchor}）")


def print_human(violations: list[dict], suite_count: int) -> None:
    for v in violations:
        sym = f"（{v['symbol']}）" if v["symbol"] else ""
        print(f"{v['file']}:{v['line']}  [{v['rule']}] {v['message']}{sym}")
    counts = {rid: 0 for rid in RULE_INDEX}
    for v in violations:
        counts[v["rule"]] = counts.get(v["rule"], 0) + 1
    detail = " ".join(f"{rid}×{counts[rid]}" for rid in RULE_INDEX if counts.get(rid, 0)) or "无"
    print(f"共 {suite_count} 个套件，违规 {len(violations)} 条（{detail}）")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="check_conventions.py",
        description="测试套件结构规则检查器（docs/测试约定.md 的机器强制部分，R1~R9）",
    )
    parser.add_argument("--json", action="store_true", help="以 JSON 输出")
    parser.add_argument("--list-rules", action="store_true", help="列出规则号、说明与校验手段")
    parser.add_argument("--list-exceptions", action="store_true", help="列出例外白名单（规则 | 文件 | 符号 | 理由）")
    parser.add_argument("--audit-exceptions", action="store_true",
                        help="只跑例外过期检测（R11），不输出 R1~R9/R10 —— 单独排查用")
    parser.add_argument("--root", default=None, help="仓库根目录（默认取脚本所在目录的上一级）")
    args = parser.parse_args(argv)

    if args.list_rules:
        print_rules()
        return 0
    if args.list_exceptions:
        print_exceptions()
        return 0

    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parent.parent
    try:
        violations, suite_count = run_checks(root, audit_exceptions=args.audit_exceptions)
    except CheckerError as exc:
        print(f"检查器错误：{exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"检查器错误（IO）：{exc}", file=sys.stderr)
        return 2

    if args.json:
        counts = {rid: 0 for rid in RULE_INDEX}
        for v in violations:
            counts[v["rule"]] = counts.get(v["rule"], 0) + 1
        payload = {
            "root": str(root),
            "rules": [{"id": rid, "description": desc, "check": how} for rid, desc, how in RULES],
            "exceptions": [
                {"rule": rid, "file": file, "symbol": symbol, "reason": reason, "anchor": anchor}
                for rid, file, symbol, reason, anchor in EXCEPTIONS
            ],
            "violations": violations,
            "summary": {
                "suites": suite_count,
                "violations": len(violations),
                "by_rule": counts,
                "exit_code": 1 if violations else 0,
            },
        }
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        print_human(violations, suite_count)
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
