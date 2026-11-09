#!/bin/bash
# 顺序运行全部 libusb e2e 用例（手动运行，不进 ctest）
#
# 用法: sudo ./run_all.sh [--help]
set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

usage() {
    cat <<EOF
用法: $0

依次运行全部 libusb 后端 e2e 用例（需要 root）：
control、bulk、interrupt、iso、raw。iso 用例需要
CONFIG_USBIP_VHCI_NR_HCS>=2（详见 README.md）。

环境变量:
  BUILD_DIR  构建目录（默认自动查找 build，再尝试 build_wsl2）
  USBIP      usbip 可执行文件（默认从 PATH 查找）
EOF
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
    "")
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

FAILED=()
for profile in control bulk interrupt iso raw; do
    echo "==================== $profile ===================="
    if ! "$SCRIPT_DIR/run_one.sh" "$profile"; then
        FAILED+=("$profile")
    fi
done

echo "==================== 汇总 ===================="
if [ ${#FAILED[@]} -eq 0 ]; then
    echo "全部通过"
    exit 0
fi
echo "失败: ${FAILED[*]}"
exit 1
