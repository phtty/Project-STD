#!/usr/bin/env python3
"""变异测试 —— 故意往被测代码里注入缺陷，看测试套件能否发现。

    python3 test/mutate.py                       # 默认套件 test_iap_cfg（IAP 记录 + 内部 Flash）
    python3 test/mutate.py --dry-run             # 只校验基线 + 检查每个变异体模式能否匹配，不施加
    python3 test/mutate.py --check-only          # 只查模式匹配（不编译、不跑基线），最快
    python3 test/mutate.py --target Device/Storage/dev_flash_int.c   # 只跑该目标上的变异体
    python3 test/mutate.py --suite test_cdstd --fail-on-alive        # CDSTD 命令层（A1/A3 断言）
    python3 test/mutate.py --fail-on-alive       # 有未登记的存活项 → 退出 1（可接 CI）
    python3 test/mutate.py --allow-alive C9      # 登记等价变异体：存活但不计入失败

为什么值得做：本仓库的 IAP 记录套件在"反向验证"时差点全员通过 —— 因为被测试文件
#include 的实现 TU 不在 TEST_*_SRCS 里，改了源码 make 不重编，跑的仍是旧二进制。
没有基线守卫与逐变异体重编，这套工具本身就会骗人。

四条不变量（缺一条结果就无意义）：
  1. **基线守卫** —— 套件本身必须是绿的，否则每个变异体都"被杀死"（其实是套件本来就红）。
     这个坑真实发生过：脚本曾引用旧布局（build/test/…）的二进制，基线红/绿都是错的。
  2. **每个变异体都重编** —— 否则测的是上一个变异体的二进制。
  3. **异常也还原** —— 脚本被信号打断时源文件会停在变异状态。本仓库发生过一次：
     VSCode 崩溃后 app_iap_cfg.c 里少了一行 memcpy，当时没人知道源码正被改写。
     finally + 信号处理都要有，另外启动时若发现上次遗留的 .mutbak 也要先还原。
  4. **还原后 touch 源文件** —— 还原回来的 mtime 是"做备份那一刻"，早于用它编译出的
     那个二进制；make 会认为目标最新而不重编，之后所有运行（包括 make test）跑的
     都是**变异后的二进制**。防住了"还原"不等于防住了"重编"。

另外：**"存活"不等于测试有缺口**，也可能是等价变异体（语义与原代码相同）。
本文件里就写错过一个 —— 注入 `else if (0) { } else if (...)` 与原链完全等价。
报出存活前先读一遍那个变异体，确认它真的改变了行为；确认等价后用 --allow-alive 登记。

退出码：
    0  跑完（可能有存活项；未开 --fail-on-alive 时不视为失败）
    1  预检失败（模式匹配不到/无效变异体），或 --fail-on-alive 且有未登记存活项
    2  用法/文件/编译/基线错误（基线红是致命的前置条件）
    3  被 SIGINT/SIGTERM 打断（源码已还原）
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUN_TIMEOUT_S = 120
DEFAULT_SUITE = "test_iap_cfg"
DEFAULT_BOARD = "3833024"

# 套件 → 变异体集合。每项：(目标文件（相对仓库根）, 名字, 原文, 替换文本)。
# 名字的首 token（C1 / F2 …）是稳定编号，--allow-alive 用它登记等价变异体。
# 加新套件：在这里加一组，并保证 Makefile 里能 `make build/<board>/test/<套件名>`。
SUITES: dict[str, list[tuple[str, str, str, str]]] = {
    "test_iap_cfg": [
        ("Application/Src/IAP/app_iap_cfg.c",
         "C1  magic 检查删掉",
         "    if (info->magic != APP_FLASH_IAP_MAGIC) return false;\n\n", ""),
        ("Application/Src/IAP/app_iap_cfg.c",
         "C2  crc 比较取反",
         "return info->config_crc == _iap_cfg_crc", "return info->config_crc != _iap_cfg_crc"),
        ("Application/Src/IAP/app_iap_cfg.c",
         "C3  empty 的 && 改 ||",
         "info->magic == 0xFFFFFFFF && info->config_crc", "info->magic == 0xFFFFFFFF || info->config_crc"),
        ("Application/Src/IAP/app_iap_cfg.c",
         "C4  CRC 范围含自身",
         "sizeof(app_flash_iap_sys_info_t) - sizeof(info->config_crc));", "sizeof(app_flash_iap_sys_info_t));"),
        ("Application/Src/IAP/app_iap_cfg.c",
         "C5  CRC 范围少 4 字节",
         "- sizeof(info->config_crc));", "- sizeof(info->config_crc) - 4);"),
        ("Application/Src/IAP/app_iap_cfg.c",
         "C6  播种 update_status=FAILED",
         "info.update_status = APP_FLASH_IAP_UPDATED;\n        memset", "info.update_status = APP_FLASH_IAP_FAILED;\n        memset"),
        ("Application/Src/IAP/app_iap_cfg.c",
         "C7  播种不设 magic",
         "info.magic         = APP_FLASH_IAP_MAGIC;\n        info.update_status = APP_FLASH_IAP_UPDATED;",
         "info.update_status = APP_FLASH_IAP_UPDATED;"),
        ("Application/Src/IAP/app_iap_cfg.c",
         "C8  去重判定取反",
         "memcmp(info.net_cfg.ip, ip, 4) == 0", "memcmp(info.net_cfg.ip, ip, 4) != 0"),
        ("Application/Src/IAP/app_iap_cfg.c",
         "C9  去掉去重分支",
         "    } else if (memcmp(info.net_cfg.ip, ip, 4) == 0",
         "    } else if (0) { } else if (memcmp(info.net_cfg.ip, ip, 4) == 0"),
        ("Application/Src/IAP/app_iap_cfg.c",
         "C10 写记录漏掉 gw",
         "    memcpy(info.net_cfg.gw, gw, 4);\n", ""),
        ("Application/Src/IAP/app_iap_cfg.c",
         "C11 早退不释放锁",
         "        if (s_lock) osMutexRelease(s_lock);\n        return; /* 内容未变",
         "        return; /* 内容未变"),
        ("Application/Src/IAP/app_iap_cfg.c",
         "C12 编程前不擦除",
         "    _erase_config_unlocked();\n    _write_config_unlocked(info);",
         "    _write_config_unlocked(info);"),
        ("Application/Src/IAP/app_iap_cfg.c",
         "C13 擦除后不编程",
         "    _erase_config_unlocked();\n    _write_config_unlocked(info);",
         "    _erase_config_unlocked();"),
        ("Application/Src/IAP/app_iap_cfg.c",
         "C14 写入长度截短 4 字节",
         "0, (uint8_t *)info, sizeof(*info));", "0, (uint8_t *)info, sizeof(*info) - 4);"),
        ("Device/Storage/dev_flash_int.c",
         "F1  编程失败不上报",
         "        if (pl_flash_program_word(abs_addr + i * 4, tmp[i]) != 0)\n"
         "            r = -1; /* 编程失败必须上报：调用方据此判定配置未落盘 */",
         "        pl_flash_program_word(abs_addr + i * 4, tmp[i]);"),
        ("Device/Storage/dev_flash_int.c",
         "F2  写入地址偏移错位",
         "uint32_t abs_addr     = self->base_addr + addr;",
         "uint32_t abs_addr     = self->base_addr + addr + 4;"),
        ("Device/Storage/dev_flash_int.c",
         "F3  _read 返回字节数",
         "return 0; /* 约定: 0 = 成功（不返回字节数） */", "return (int32_t)len;"),
    ],
    # CDSTD 协议命令（A1 文字显示 / A3 全屏填充）——断言见 test/test_cdstd.c。
    "test_cdstd": [
        ("Application/Src/CDSTD/app_cdstd_cmd.c",
         "D1  字号表 0x31 档 24→32",
         "static const app_font_size_t s_size_map[] = {APP_FONT_SIZE_16, APP_FONT_SIZE_24, APP_FONT_SIZE_32, APP_FONT_SIZE_48};",
         "static const app_font_size_t s_size_map[] = {APP_FONT_SIZE_16, APP_FONT_SIZE_32, APP_FONT_SIZE_32, APP_FONT_SIZE_48};"),
        ("Application/Src/CDSTD/app_cdstd_cmd.c",
         "D2  字型表首项 黑体→宋体",
         "static const app_font_type_t s_type_map[] = {APP_FONT_TYPE_HT, APP_FONT_TYPE_FS, APP_FONT_TYPE_KT, APP_FONT_TYPE_ST};",
         "static const app_font_type_t s_type_map[] = {APP_FONT_TYPE_ST, APP_FONT_TYPE_FS, APP_FONT_TYPE_KT, APP_FONT_TYPE_ST};"),
        ("Application/Src/CDSTD/app_cdstd_cmd.c",
         "D3  颜色判未转换表达式（低位漏放行）",
         "        dev_display_color_t color = (dev_display_color_t)(ctx->color - 0x30);\n"
         "        if (color > DEV_DISPLAY_COLOR_WHITE) {",
         "        dev_display_color_t color = (dev_display_color_t)(ctx->color - 0x30);\n"
         "        if (ctx->color - 0x30 > DEV_DISPLAY_COLOR_WHITE) {"),
        ("Application/Src/CDSTD/app_cdstd_cmd.c",
         "D4  区域宽回满屏（w 不减 x）",
         "            .w     = (uint16_t)(w - x),", "            .w     = w,"),
        ("Application/Src/CDSTD/app_cdstd_cmd.c",
         "D5  A1 长度守卫去掉",
         "        if (data_len <= sizeof(app_cdstd_text_display_t)) { /* 文字数据至少 1 字节；清屏走 A3 */",
         "        if (0) { /* 文字数据至少 1 字节；清屏走 A3 */"),
        ("Application/Src/CDSTD/app_cdstd_cmd.c",
         "D6  A3 颜色直传协议原字节",
         "            .color = color,",
         "            .color = (dev_display_color_t)(*(uint8_t *)data),"),
        ("Application/Src/CDSTD/app_cdstd_cmd.c",
         "D7  A1 文字长度少算 1 字节",
         "            .len       = (uint16_t)(data_len - sizeof(app_cdstd_text_display_t)),",
         "            .len       = (uint16_t)(data_len - sizeof(app_cdstd_text_display_t) - 1),"),
        ("Application/Src/CDSTD/app_cdstd_cmd.c",
         "D8  A3 置 persist=true",
         "            .color = color,\n        });",
         "            .color = color,\n            .persist = true,\n        });"),
        ("Application/Src/CDSTD/app_cdstd_cmd.c",
         "D9  A1 长度守卫 <= 改 <（恰好头部漏过）",
         "        if (data_len <= sizeof(app_cdstd_text_display_t)) {",
         "        if (data_len < sizeof(app_cdstd_text_display_t)) {"),
    ],
}


def mutant_id(name: str) -> str:
    return name.split()[0]


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        prog="mutate.py",
        description="变异测试：往生产源码注入缺陷，看对应套件能否发现（结果仅供参考，存活需人工确认）。",
        epilog="退出码：0 跑完；1 预检失败或 --fail-on-alive 命中；2 用法/编译/基线错误；3 被信号打断。",
    )
    ap.add_argument("--suite", default=DEFAULT_SUITE,
                    help=f"套件名（Makefile 里的 build/<board>/test/<suite>）；已登记：{', '.join(SUITES)}；默认 {DEFAULT_SUITE}")
    ap.add_argument("--board", default=DEFAULT_BOARD,
                    help=f"板名（对应 build/<board>/test/ 与 make BOARD=<board>）；默认 {DEFAULT_BOARD}")
    ap.add_argument("--target", action="append", default=[], metavar="生产.c",
                    help="只跑这些目标文件上的变异体（相对仓库根的路径，可多次；与 --suite 的集合取交集）")
    ap.add_argument("--dry-run", action="store_true",
                    help="校验基线（编译并运行一次，必须绿）+ 检查每个变异体模式能否在当前源码匹配；不施加、不重编")
    ap.add_argument("--check-only", action="store_true",
                    help="只检查变异体模式与目标文件（不编译、不跑基线）；比 --dry-run 更轻，--dry-run 已包含它")
    ap.add_argument("--fail-on-alive", action="store_true",
                    help="有未登记的存活项时退出 1（配合 --allow-alive 可接 CI）")
    ap.add_argument("--allow-alive", default="", metavar="C1,C2",
                    help="登记为等价变异体的存活编号（按名字首 token 匹配），其存活不计入失败")
    return ap.parse_args(argv)


def _failure_line(stdout: str) -> str:
    """失败证据行：优先 `FAIL …`（早期变体），否则取带 ✘ 的那行（主流变体）；去掉 ANSI 颜色。"""
    ansi = re.compile(r"\x1b\[[0-9;]*m")
    lines = stdout.splitlines()
    for line in lines:
        if "FAIL " in line:
            return ansi.sub("", line).strip()
    for line in lines:
        if "✘" in line:
            return ansi.sub("", line).strip()
    return ""


def build_and_run(board: str, suite: str) -> tuple[int | None, str]:
    """重编并运行套件。返回 (退出码, 证据)；退出码 None 表示编译失败或文件缺失。"""
    binary = ROOT / "build" / board / "test" / suite
    build = subprocess.run(
        ["make", f"BOARD={board}", f"build/{board}/test/{suite}"],
        cwd=ROOT, capture_output=True, text=True,
    )
    if build.returncode != 0:
        tail = (build.stderr or build.stdout).strip().splitlines()[-1:] or [""]
        return None, f"编译失败（{tail[0][:40]}）"
    if not binary.is_file():
        return None, f"二进制不存在：{binary}"
    try:
        run = subprocess.run(
            [str(binary)], capture_output=True, text=True, timeout=RUN_TIMEOUT_S,
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"},
        )
    except subprocess.TimeoutExpired:
        return 124, f"运行超时（{RUN_TIMEOUT_S}s）"
    return run.returncode, _failure_line(run.stdout)


class Session:
    """一次运行的全部可变状态：目标文件集合、还原、打印。"""

    def __init__(self, board: str, suite: str, mutants: list[tuple[str, str, str, str]]) -> None:
        self.board = board
        self.suite = suite
        self.mutants = mutants
        self.targets = sorted({t for t, *_ in mutants})
        self._install_signal_handlers()

    # ---- 不变量 3/4：还原 + touch ----
    def restore(self) -> None:
        for target in self.targets:
            path = ROOT / target
            backup = Path(str(path) + ".mutbak")
            if not backup.exists():
                continue
            shutil.move(str(backup), str(path))
            # **必须 touch**：还原回来的 mtime 是"做备份那一刻"，早于用它编译出的
            # 那个二进制 —— make 会认为目标是最新的，不重编，于是后面所有运行
            # （包括 make test）跑的都是**变异后的二进制**。
            # 这个坑让本仓库的 test_iap_cfg 在"内容未变→不擦写"用例上无故变红，
            # 排查了一圈才发现是二进制陈旧，不是回归。
            os.utime(path, None)

    def _install_signal_handlers(self) -> None:
        def handler(*_a):
            self.restore()
            print("\n!! 收到信号，源码已还原，退出（3）", file=sys.stderr)
            sys.exit(3)

        signal.signal(signal.SIGTERM, handler)
        signal.signal(signal.SIGINT, handler)

    def recover_stale_backups(self) -> None:
        stale = [t for t in self.targets if Path(str(ROOT / t) + ".mutbak").exists()]
        if not stale:
            return
        print("!! 发现上次遗留的 .mutbak（脚本/编辑器崩溃过？），先还原再继续：")
        for t in stale:
            print(f"   {t}")
        self.restore()

    # ---- 打印 ----
    def table_header(self) -> None:
        width = max(26, *(len(m[1]) + 2 for m in self.mutants)) if self.mutants else 26
        self.width = width
        print(f"{'变异体':<{width}} {'结果':<10} 证据")
        print("-" * (width + 10 + 62))

    def row(self, name: str, status: str, evidence: str = "") -> None:
        print(f"{name:<{self.width}} {status:<10} {evidence[:58]}")


def select_mutants(all_mutants: list[tuple[str, str, str, str]], targets: list[str]) -> list[tuple[str, str, str, str]]:
    if not targets:
        return list(all_mutants)
    wanted = {os.path.normpath(t) for t in targets}
    return [m for m in all_mutants if os.path.normpath(m[0]) in wanted]


def check_patterns(session: Session, verbose: bool) -> tuple[int, int]:
    """校验每个变异体的模式能否在目标文件里唯一/至少匹配一次。返回 (可施加数, 问题数)。"""
    ok = 0
    bad = 0
    for target, name, old, new in session.mutants:
        path = ROOT / target
        if not path.is_file():
            session.row(name, "目标缺失", f"{target} 不存在")
            bad += 1
            continue
        src = path.read_text(encoding="utf-8")
        count = src.count(old)
        if old == "":
            session.row(name, "空模式", "old 为空（追加型），跳过匹配检查")
            ok += 1
        elif old == new:
            session.row(name, "无效变异体", "old 与 new 相同")
            bad += 1
        elif count == 0:
            session.row(name, "**模式未匹配**", f"在 {target} 里找不到 old 文本（变异集已过时？）")
            bad += 1
        else:
            note = f"{target}（{count} 处匹配，施加时只替换第 1 处）" if count > 1 else target
            session.row(name, "可施加" if verbose else "模式命中", note)
            ok += 1
    return ok, bad


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    if args.suite not in SUITES:
        print(f"未知套件 {args.suite}；已登记的变异集合：{', '.join(SUITES)}", file=sys.stderr)
        return 2
    if not (ROOT / "boards" / args.board).is_dir():
        print(f"板不存在：boards/{args.board}", file=sys.stderr)
        return 2

    mutants = select_mutants(SUITES[args.suite], args.target)
    if not mutants:
        print(f"--target 过滤后套件 {args.suite} 没有变异体可跑：{args.target}", file=sys.stderr)
        return 2

    allowed = {s.strip() for s in args.allow_alive.split(",") if s.strip()}
    known_ids = {mutant_id(m[1]) for m in mutants}
    unknown = sorted(allowed - known_ids)
    if unknown:
        print(f"注意：--allow-alive 里的编号不在本次变异集：{', '.join(unknown)}", file=sys.stderr)

    session = Session(args.board, args.suite, mutants)
    session.recover_stale_backups()

    print(f"套件 {args.suite}（board={args.board}）")
    print("用法：python3 test/mutate.py（结果仅供参考，存活项需人工确认是否等价变异体）")
    if args.check_only:
        print("模式：--check-only（只查模式匹配，不编译、不跑基线）\n")
        session.table_header()
        try:
            _ok, bad = check_patterns(session, verbose=False)
        finally:
            session.restore()
        print(f"\n预检：模式未匹配/无效 {bad} 个" + ("——先修变异集或源码" if bad else "——全部可施加"))
        return 1 if bad else 0

    # ---- 不变量 1：基线守卫 ----
    print()
    print("基线：编译并运行一次……")
    rc, detail = build_and_run(args.board, args.suite)
    if rc is None or rc != 0:
        print("!! 基线是红的，先修测试。变异结果全部无意义。")
        print(detail)
        return 2
    print("基线：绿\n")

    if args.dry_run:
        print("模式：--dry-run（校验基线 + 模式匹配，不施加、不重编）\n")
        session.table_header()
        try:
            _ok, bad = check_patterns(session, verbose=True)
        finally:
            session.restore()
        print(f"\n预检：模式未匹配/无效 {bad} 个" + ("——先修变异集或源码" if bad else "——全部可施加"))
        return 1 if bad else 0

    # ---- 正常循环：不变量 2：每个变异体都重编 ----
    session.table_header()
    alive: list[str] = []
    equivalent: list[str] = []
    unmatched: list[str] = []
    compile_failed: list[str] = []
    killed = 0
    try:
        for target, name, old, new in session.mutants:
            path = ROOT / target
            source = path.read_text(encoding="utf-8")
            if old == "":
                session.row(name, "空模式", "old 为空（追加型），跳过匹配检查")
                unmatched.append(name)
                continue
            if old not in source:
                session.row(name, "无法施加", "模式未匹配")
                unmatched.append(name)
                continue
            shutil.copy(str(path), str(path) + ".mutbak")
            path.write_text(source.replace(old, new, 1), encoding="utf-8")
            rc, detail = build_and_run(args.board, args.suite)
            session.restore()
            if rc is None:
                session.row(name, "编译失败", detail)
                compile_failed.append(name)
            elif rc != 0:
                session.row(name, "杀死", detail)
                killed += 1
            elif mutant_id(name) in allowed:
                session.row(name, "存活（已登记）", "等价变异体，见 --allow-alive")
                equivalent.append(name)
            else:
                session.row(name, "**存活**")
                alive.append(name)
    finally:
        session.restore()

    total = len(session.mutants)
    print(f"\n共 {total} 个变异体：杀死 {killed}，存活 {len(alive)}（未登记），"
          f"等价已登记 {len(equivalent)}，无法施加 {len(unmatched)}，编译失败 {len(compile_failed)}")
    if alive:
        print("存活（未登记）：" + "、".join(alive))
    if unmatched:
        print("无法施加（模式未匹配）：" + "、".join(unmatched))
    if compile_failed:
        print("编译失败（可能变异体本身语法不成立）：" + "、".join(compile_failed))

    failed = bool(alive) and args.fail_on_alive
    if args.fail_on_alive:
        print(f"--fail-on-alive=on：未登记存活 {len(alive)} 项 → 退出码 {1 if failed else 0}")
    else:
        print(f"--fail-on-alive=off：未登记存活 {len(alive)} 项不改变退出码（加 --fail-on-alive 时为 "
              f"{1 if alive else 0}）")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
