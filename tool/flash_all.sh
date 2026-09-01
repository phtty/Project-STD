#!/usr/bin/env bash
#===============================================================================
# flash_all.sh - STM32F407 多固件一键烧录
#               Multi-firmware one-shot flash for STM32F407ZGT6
#
# 在一个 J-Link 会话中烧录 Bootloader + Recovery + Project-STD
# Flashes all 3 firmwares in ONE J-Link session to prevent
# intermediate full-chip erase wiping previous loads.
#
# Flash Layout (STM32F407ZGT6, 1MB):
#   Bootloader:  0x08000000  16 KB  (Sector 0)
#   [Board cfg]  0x08004000  16 KB  (Sector 1, 板级系统配置:
#                                  magic + update_sta + FWInfo + NetConfig + CRC32)
#   Recovery:    0x08008000  224 KB (Sectors 2-5)
#   Project-STD: 0x08040000  768 KB (Sectors 6-11)
#
# 为什么默认擦除 Sector1（重要，2026-08-14 定案）:
#   Bootloader 条件 D（非 debug 模式）会校验 Sector1 记录中 app_info.size/crc32
#   与 0x08040000 实机固件 CRC 是否一致，而 app_info 真实值只由 Recovery 升级
#   流程写入。因此：一旦 Recovery 成功升级过一次（app_info 为旧固件真实值），
#   再用 J-Link/EIDE 直接重烧主固件而不更新 Sector1 → 条件 D 校验失配 →
#   Bootloader 判「App 损坏」→ 设备永远进 Recovery，主固件再也起不来。
#   规避：本脚本默认烧完三个固件后擦除 Sector1 恢复出厂态。Sector1 空 →
#   Bootloader 走条件 C（出厂初始化）→ 正常跳主固件。net_cfg 副作用可接受：
#   主固件上电时 ldi_ctx_init 会从 W25Qxx 外部 Flash 的 LDI 配置同步回写
#   Sector1（空扇区自动完整初始化）。
#
# Usage:
#   ./flash_all.sh               # 使用默认编译产物 (默认跳过整片擦除和校验,
#                                # 但默认擦除 Sector1 恢复出厂态)
#   ./flash_all.sh --dry-run     # 预览命令不执行
#   ./flash_all.sh --keep-config # 保留 Sector1 板级配置 (跳过 Sector1 擦除;
#                                # 仅当 Sector1 app_info 与实机固件一致时使用,
#                                # 例如一直只经 Recovery 升级、从未用烧录器重烧)
#   ./flash_all.sh -b <path>     # 自定义 Bootloader
#   ./flash_all.sh -r <path>     # 自定义 Recovery
#   ./flash_all.sh -a <path>     # 自定义 Project-STD
#   ./flash_all.sh -s <kHz>      # J-Link 速度 (默认: 40000)
#   ./flash_all.sh --erase       # 强制整片擦除 (默认跳过; 与 Sector1 擦除共存
#                                # 时天然幂等, 无需特殊处理)
#   ./flash_all.sh --no-erase    # 跳过整片擦除
#   ./flash_all.sh --verify      # 强制校验 (默认跳过)
#   ./flash_all.sh --no-verify   # 跳过校验
#   ./flash_all.sh --fast        # 快捷: --no-erase --no-verify (默认行为)
#===============================================================================
set -euo pipefail
shopt -s nullglob

# ── Color ────────────────────────────────────────────────────────────────────
readonly C_R='\033[0;31m'  C_G='\033[0;32m'  C_Y='\033[1;33m'
readonly C_B='\033[0;34m'  C_N='\033[0m'
info()  { printf "%b[INFO]%b  %s\n"  "$C_G" "$C_N" "$*"; }
warn()  { printf "%b[WARN]%b  %s\n"  "$C_Y" "$C_N" "$*"; }
error() { printf "%b[ERROR]%b %s\n" "$C_R" "$C_N" "$*"; }
step()  { printf "%b[STEP]%b  %s\n"  "$C_B" "$C_N" "$*"; }

# ── Resolve script directory (relative path base) ────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Project root: tool/../
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# Workspace root: 3833024/
WS_ROOT="$(cd "$PROJECT_ROOT/.." && pwd)"

# ── Default firmware paths (relative to WS_ROOT) ─────────────────────────────
readonly DFL_BOOT="${WS_ROOT}/STM32F407-Bootloader-master/build/Debug/Bootloader.hex"
readonly DFL_RECV="${WS_ROOT}/STM32F407-Recovery-master/build/Debug/Recovery.hex"
readonly DFL_APP="${WS_ROOT}/Project-STD-main/build/Debug/Project_STD.hex"

# ── User-configurable ────────────────────────────────────────────────────────
BOOT_FILE=""; RECV_FILE=""; APP_FILE=""
JLINK_DEVICE="STM32F407ZG"
JLINK_IF="SWD"
JLINK_SPEED="40000"
DRY_RUN=false
SKIP_ERASE=true
SKIP_VERIFY=true
KEEP_CONFIG=false
JLINK_SCRIPT=""
START_TIME=""

# ── Usage ────────────────────────────────────────────────────────────────────
usage() {
    cat << 'EOF'
Usage: ./flash_all.sh [OPTIONS]

STM32F407 Multi-Firmware Flash Tool (Bootloader + Recovery + Project-STD)

OPTIONS:
  -b, --bootloader <file>  Bootloader hex/bin
  -r, --recovery <file>    Recovery hex/bin
  -a, --app <file>         Project-STD hex/bin
  -d, --device <name>      J-Link device   (default: STM32F407ZG)
  -i, --interface <if>     SWD | JTAG      (default: SWD)
  -s, --speed <kHz>        Interface speed  (default: 40000)
  --dry-run                Print commands without flashing
  --erase                  Force chip erase
  --no-erase               Skip chip erase (default)
  --keep-config            Keep Sector1 board config (skip Sector1 erase;
                           default: erase Sector1 for factory reset)
  --verify                 Force verify after load
  --no-verify              Skip verify after load (default)
  --fast                   Shortcut: --no-erase --no-verify (default behavior)
  -h, --help               Show this help

FLASH LAYOUT:
  0x08000000  Bootloader   16 KB
  0x08004000  Board cfg    16 KB  (Sector 1; ERASED by default, see below)
  0x08008000  Recovery    224 KB
  0x08040000  Project-STD 768 KB

SECTOR1: erased after load by default (factory reset) to avoid the
         Bootloader condition-D app_info CRC mismatch trap (reflashing
         the app without updating Sector1 after a Recovery upgrade
         bricks the boot: App judged corrupt -> stuck in Recovery).
         Use --keep-config only when Sector1 matches the on-chip app.

DEFAULTS: skip chip erase, skip verify, speed 40000 kHz,
          ERASE Sector1 (factory reset)
          Use --erase and/or --verify to enable those steps.
EOF
}

# ── Parse args ───────────────────────────────────────────────────────────────
while (($#)); do
    case "$1" in
        -b|--bootloader) BOOT_FILE="$2"; shift 2 ;;
        -r|--recovery)   RECV_FILE="$2"; shift 2 ;;
        -a|--app)        APP_FILE="$2";  shift 2 ;;
        -d|--device)     JLINK_DEVICE="$2"; shift 2 ;;
        -i|--interface)  JLINK_IF="$2"; shift 2 ;;
        -s|--speed)      JLINK_SPEED="$2"; shift 2 ;;
        --dry-run)       DRY_RUN=true; shift ;;
        --erase)         SKIP_ERASE=false; shift ;;
        --no-erase)      SKIP_ERASE=true; shift ;;
        --keep-config)   KEEP_CONFIG=true; shift ;;
        --verify)        SKIP_VERIFY=false; shift ;;
        --no-verify)     SKIP_VERIFY=true; shift ;;
        --fast)          SKIP_ERASE=true; SKIP_VERIFY=true; shift ;;
        -h|--help)       usage; exit 0 ;;
        *) error "Unknown option: $1"; usage; exit 1 ;;
    esac
done

BOOT_FILE="${BOOT_FILE:-$DFL_BOOT}"
RECV_FILE="${RECV_FILE:-$DFL_RECV}"
APP_FILE="${APP_FILE:-$DFL_APP}"

# ── Validate firmware files ──────────────────────────────────────────────────
validate_files() {
    local name label file size ok=0
    local -a batch=( \
        "Bootloader|BOOT|$BOOT_FILE" \
        "Recovery  |RECV|$RECV_FILE" \
        "Project-STD|APP|$APP_FILE" \
    )
    for entry in "${batch[@]}"; do
        IFS='|' read -r label _ file <<< "$entry"
        if [[ -f "$file" ]]; then
            size=$(du -h "$file" | cut -f1)
            [[ "${file##*.}" =~ [hH][eE][xX] ]] \
                && info "$label: ${file} (${size}, HEX self-addressed)" \
                || info "$label: ${file} (${size}, BIN)"
        else
            error "$label: NOT FOUND -> ${file}"
            ok=1
        fi
    done
    return $ok
}

# ── Find JLinkExe ────────────────────────────────────────────────────────────
find_jlink() {
    local p
    command -v JLinkExe 2>/dev/null && return
    for p in /opt/SEGGER/JLink/JLinkExe /usr/bin/JLinkExe; do
        [[ -x "$p" ]] && { echo "$p"; return; }
    done
    for p in /opt/SEGGER/JLink*/JLinkExe; do
        [[ -x "$p" ]] && { echo "$p"; return; }
    done
    return 1
}

# ── Format elapsed time ──────────────────────────────────────────────────────
format_elapsed() {
    local seconds="$1"
    local mins=$((seconds / 60))
    local secs=$((seconds % 60))
    if ((mins > 0)); then
        printf '%dm %ds' "$mins" "$secs"
    else
        printf '%ds' "$secs"
    fi
}

# ── Generate J-Link script ───────────────────────────────────────────────────
generate_jlink_script() {
    local tmp
    tmp="$(mktemp /tmp/flash_all_XXXXX.jlink)"
    {
        printf 'device %s\n' "$JLINK_DEVICE"
        printf 'si %s\n' "$JLINK_IF"
        printf 'speed %s\n' "$JLINK_SPEED"
        printf 'h\n'
        if ! $SKIP_ERASE; then
            printf 'erase\n'
        fi
        if $SKIP_VERIFY; then
            printf 'SetCompareMode 0\n'
        fi
        printf 'loadfile %s\n' "$BOOT_FILE"
        printf 'loadfile %s\n' "$RECV_FILE"
        printf 'loadfile %s\n' "$APP_FILE"
        # 默认擦除 Sector1 (0x08004000~0x08007FFF, 板级配置扇区) 恢复出厂态:
        # 规避 Bootloader 条件 D 的 app_info CRC 失配坑 (详见脚本头注释)。
        # J-Link 范围擦除语法: Erase [<SAddr>, <EAddr>] (逗号分隔, 大小写不敏感)。
        # 与 --erase 整片擦除共存时天然幂等 (整片擦除已覆盖 Sector1, 无需特殊处理)。
        if ! $KEEP_CONFIG; then
            printf 'erase 0x08004000, 0x08007FFF\n'
        fi
        if $SKIP_VERIFY; then
            printf 'SetCompareMode 1\n'
        fi
        printf 'r\ng\nqc\n'
    } > "$tmp"
    echo "$tmp"
}

# ══════════════════════════════════════════════════════════════════════════════
# Main
# ══════════════════════════════════════════════════════════════════════════════

printf '\n============================================\n'
printf ' STM32F407 Multi-Firmware Flash Tool\n'
printf '============================================\n\n'

step "Validating firmware files..."
validate_files || exit 1

step "Locating J-Link Commander..."
JLINK_EXE=$(find_jlink) || {
    error "JLinkExe not found! Install: https://www.segger.com/downloads/jlink/"
    exit 1
}
info "Found: $JLINK_EXE"

step "Generating command script..."
JLINK_SCRIPT="$(generate_jlink_script)"

# ── Summary ──────────────────────────────────────────────────────────────────
printf -- '\n--------------------------------------------\n'
printf "  %-13s %s\n"  "Device:"     "$JLINK_DEVICE"
printf "  %-13s %s @ %s kHz\n" "Interface:" "$JLINK_IF" "$JLINK_SPEED"
printf "  %-13s %s\n"  "Bootloader:" "${BOOT_FILE##*/}"
printf "  %-13s %s\n"  "Recovery:"   "${RECV_FILE##*/}"
printf "  %-13s %s\n"  "App:"        "${APP_FILE##*/}"
printf "  %-13s %s\n"  "Erase:"      "$($SKIP_ERASE && echo 'SKIPPED' || echo 'YES (single)')"
printf "  %-13s %s\n"  "Erase Sec1:" "$($KEEP_CONFIG && echo 'KEPT' || echo 'YES (factory reset)')"
printf "  %-13s %s\n"  "Verify:"     "$($SKIP_VERIFY && echo 'SKIPPED' || echo 'YES')"
printf "  %-13s %s\n"  "Dry run:"    "$($DRY_RUN && echo 'YES' || echo 'no')"
printf -- '--------------------------------------------\n\n'

step "Commands to execute:"
while IFS= read -r line || [[ -n "$line" ]]; do
    printf '  %s\n' "$line"
done < "$JLINK_SCRIPT"
printf '\n'

# ── Dry-run ──────────────────────────────────────────────────────────────────
if $DRY_RUN; then
    warn "DRY RUN -- no flash executed."
    rm -f "$JLINK_SCRIPT"
    exit 0
fi

# ── Execute ──────────────────────────────────────────────────────────────────
step "Flashing... (this may take a moment)"
echo ''

START_TIME=$(date +%s)

set +e
"$JLINK_EXE" \
    -device "$JLINK_DEVICE" \
    -if "$JLINK_IF" \
    -speed "$JLINK_SPEED" \
    -autoconnect 1 \
    -CommanderScript "$JLINK_SCRIPT"
rc=$?
set -e

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

rm -f "$JLINK_SCRIPT"

printf '\n'
if ((rc == 0)); then
    info "============================================"
    info "  Flash SUCCESS"
    info "  Bootloader  @ 0x08000000"
    info "  Recovery    @ 0x08008000"
    info "  Project-STD @ 0x08040000"
    info "  Elapsed: $(format_elapsed "$ELAPSED")"
    info "============================================"
else
    error "============================================"
    error "  Flash FAILED (exit: $rc)"
    error "  Elapsed: $(format_elapsed "$ELAPSED")"
    error "  Check J-Link connection"
    error "============================================"
fi

exit $rc
