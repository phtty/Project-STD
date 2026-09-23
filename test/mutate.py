"""
变异测试 —— 故意往被测代码里注入缺陷，看测试套件能否发现。

    python3 test/mutate.py

为什么值得做：本仓库的 IAP 记录套件在"反向验证"时差点全员通过 —— 因为被测试文件
#include 的实现 TU 不在 TEST_*_SRCS 里，改了源码 make 不重编，跑的仍是旧二进制。
没有基线守卫与逐变异体重编，这套工具本身就会骗人。

三条硬要求（缺一条结果就无意义）：
  1. **基线守卫** —— 套件本身必须是绿的，否则每个变异体都"被杀死"
  2. **每个变异体都重编** —— 否则测的是上一个变异体的二进制
  3. **异常也还原** —— 脚本被信号打断时源文件会停在变异状态（本仓库就发生过一次，
     VSCode 崩溃后 app_iap_cfg.c 里少了一行 memcpy）。finally + 信号处理都要有。
  4. **还原后 touch 源文件** —— 还原回来的 mtime 早于"用变异源码编出来的二进制"，
     make 会认为目标最新而不重编，之后所有运行跑的都是变异版本。
     防住了"还原"不等于防住了"重编"。

另外：**"存活"不等于测试有缺口**，也可能是等价变异体（语义与原代码相同）。
本文件里就写错过一个 —— 注入 `else if (0) { } else if (...)` 与原链完全等价。
报出存活前先读一遍那个变异体，确认它真的改变了行为。
"""

import subprocess, shutil, sys, signal, os

A = "/home/phtty/workspace/source/Project_STD/"
BIN = A + "build/test/test_iap_cfg"
TARGET = A + "Application/Src/IAP/app_iap_cfg.c"
TARGET2 = A + "Device/Storage/dev_flash_int.c"

MUTANTS = [
 (TARGET,  "C1  magic 检查删掉",        "    if (info->magic != APP_FLASH_IAP_MAGIC) return false;\n\n", ""),
 (TARGET,  "C2  crc 比较取反",          "return info->config_crc == _iap_cfg_crc", "return info->config_crc != _iap_cfg_crc"),
 (TARGET,  "C3  empty 的 && 改 ||",     "info->magic == 0xFFFFFFFF && info->config_crc", "info->magic == 0xFFFFFFFF || info->config_crc"),
 (TARGET,  "C4  CRC 范围含自身",        "sizeof(app_flash_iap_sys_info_t) - sizeof(info->config_crc));", "sizeof(app_flash_iap_sys_info_t));"),
 (TARGET,  "C5  CRC 范围少 4 字节",     "- sizeof(info->config_crc));", "- sizeof(info->config_crc) - 4);"),
 (TARGET,  "C6  播种 update_status=FAILED","info.update_status = APP_FLASH_IAP_UPDATED;\n        memset", "info.update_status = APP_FLASH_IAP_FAILED;\n        memset"),
 (TARGET,  "C7  播种不设 magic",        "info.magic      = APP_FLASH_IAP_MAGIC;\n        info.update_status = APP_FLASH_IAP_UPDATED;", "info.update_status = APP_FLASH_IAP_UPDATED;"),
 (TARGET,  "C8  去重判定取反",          "memcmp(info.net_cfg.ip, ip, 4) == 0", "memcmp(info.net_cfg.ip, ip, 4) != 0"),
 (TARGET,  "C9  去掉去重分支",          "    } else if (memcmp(info.net_cfg.ip, ip, 4) == 0", "    } else if (0) { } else if (memcmp(info.net_cfg.ip, ip, 4) == 0"),
 (TARGET,  "C10 写记录漏掉 gw",         "    memcpy(info.net_cfg.gw, gw, 4);\n", ""),
 (TARGET,  "C11 早退不释放锁",          "        if (s_lock) osMutexRelease(s_lock);\n        return; /* 内容未变", "        return; /* 内容未变"),
 (TARGET,  "C12 编程前不擦除",          "    _erase_config_unlocked();\n    _write_config_unlocked(info);", "    _write_config_unlocked(info);"),
 (TARGET,  "C13 擦除后不编程",          "    _erase_config_unlocked();\n    _write_config_unlocked(info);", "    _erase_config_unlocked();"),
 (TARGET,  "C14 写入长度截短 4 字节",   "0, (uint8_t *)info, sizeof(*info));", "0, (uint8_t *)info, sizeof(*info) - 4);"),
 (TARGET2, "F1  编程失败不上报",        "        if (pl_flash_program_word(abs_addr + i * 4, tmp[i]) != 0)\n            r = -1; /* 编程失败必须上报：调用方据此判定配置未落盘 */",
                                        "        pl_flash_program_word(abs_addr + i * 4, tmp[i]);"),
 (TARGET2, "F2  写入地址偏移错位",      "uint32_t abs_addr     = self->base_addr + addr;", "uint32_t abs_addr     = self->base_addr + addr + 4;"),
 (TARGET2, "F3  _read 返回字节数",      "return 0; /* 约定: 0 = 成功（不返回字节数） */", "return (int32_t)len;"),
]

def build_and_run():
    b = subprocess.run(["make", "build/test/test_iap_cfg"], cwd=A, capture_output=True, text=True)
    if b.returncode != 0:
        return None, "编译失败"
    r = subprocess.run([BIN], capture_output=True, text=True, timeout=120,
                       env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    first = next((l.strip() for l in r.stdout.split("\n") if "FAIL " in l), "")
    return r.returncode, first

# ---- 基线守卫 ----
rc, detail = build_and_run()
if rc != 0:
    print("!! 基线是红的，先修测试。变异结果全部无意义。")
    print(detail)
    sys.exit(2)
print("基线：绿\n")

def restore():
    for f in (TARGET, TARGET2):
        if os.path.exists(f + ".mutbak"):
            shutil.move(f + ".mutbak", f)
            # **必须 touch**：还原回来的 mtime 是"做备份那一刻"，早于用它编译出的
            # 那个二进制 —— make 会认为目标是最新的，不重编，于是后面所有运行
            # （包括 make test）跑的都是**变异后的二进制**。
            # 这个坑让本仓库的 test_iap_cfg 在"内容未变→不擦写"用例上无故变红，
            # 排查了一圈才发现是二进制陈旧，不是回归。
            os.utime(f, None)

signal.signal(signal.SIGTERM, lambda *a: (restore(), sys.exit(3)))
signal.signal(signal.SIGINT,  lambda *a: (restore(), sys.exit(3)))

print("用法：python3 test/mutate.py（结果仅供参考，存活项需人工确认是否等价变异体）")
print(f"{'变异体':<26} {'结果':<10} 证据")
print("-" * 92)
alive = []
try:
    for f, name, old, new in MUTANTS:
        shutil.copy(f, f + ".mutbak")
        src = open(f, encoding="utf-8").read()
        if old not in src:
            print(f"{name:<26} {'无法施加':<10} 模式未匹配")
            restore(); continue
        open(f, "w", encoding="utf-8").write(src.replace(old, new, 1))
        rc, detail = build_and_run()
        restore()
        if rc is None:
            print(f"{name:<26} {'编译失败':<10} {detail}")
        elif rc != 0:
            print(f"{name:<26} {'杀死':<10} {detail[:58]}")
        else:
            print(f"{name:<26} {'**存活**':<10}")
            alive.append(name)
finally:
    restore()

print(f"\n共 {len(MUTANTS)} 个变异体，存活 {len(alive)}" + (f"：{', '.join(alive)}" if alive else ""))
