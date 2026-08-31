#!/usr/bin/env bash
# e2e 总脚本：挨个运行 tests/e2e/ 下每个设备测试脚本，失败不中断（看全貌），
# 全部跑完后按失败数退出非 0。每个脚本内部自己清理服务器/设备
set -u

E2E_DIR="$(cd "$(dirname "$0")" && pwd)"
FAILED=""

# 依次跑 test_*.sh（shell 排序即执行顺序）
for t in "$E2E_DIR"/test_*.sh; do
    [ -e "$t" ] || continue
    name=$(basename "$t" .sh)
    echo ""
    echo "==================== $name ===================="
    if bash "$t"; then
        echo "[$name] OK"
    else
        echo "[$name] FAILED"
        FAILED="$FAILED $name"
    fi
done

echo ""
echo "=============================================="
if [ -n "$FAILED" ]; then
    echo "FAILED:$FAILED"
    exit 1
fi
echo "All e2e tests passed"
