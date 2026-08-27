#!/usr/bin/env bash
# 把崩溃日志里的地址栈符号化为 文件:行号。
#
# 用法：
#   cat crash.log | tools/addr2line_crash.sh
#   tools/addr2line_crash.sh crash.log
#
# 支持两种帧格式（测试的 crash handler 打印的 backtrace）：
#   [/path/module(+0x1d0ef) [0x...]      ← 无符号：偏移相对模块，直接 addr2line
#   [/path/module(symbol+0x20) [0x...]   ← 有符号：偏移相对符号，用 nm 转绝对 VMA
#
# 模块路径从日志里解析（runner 和本地都能用），只要模块二进制还在
# 输入日志对应的机器上即可。

set -u

log_file="${1:-/dev/stdin}"

# 已解析过的模块行缓存，避免同一地址重复查
declare -A seen

resolve() {
    local frame="$1"
    local module inner
    module=$(echo "$frame" | sed -E 's/\(.*//')
    inner=$(echo "$frame" | sed -E 's/^[^(]*\(([^)]*)\).*/\1/')

    [ "$inner" = "nil" ] && return 0
    [ -f "$module" ] || { echo "  模块不存在: $module"; return 1; }

    if [[ "$inner" == +0x* ]]; then
        # 无符号：+偏移 相对模块，addr2line 直接用
        addr2line -f -C -e "$module" "0x${inner#+}" 2>/dev/null || echo "  addr2line 失败"
        return
    fi

    # 有符号：inner = symbol+0x偏移，用 nm 找符号 VMA 再算绝对地址
    local sym="${inner%%+*}"
    local off="${inner#*+}"
    # nm 的导出符号带版本后缀（如 pthread_detach@@GLIBC_2.34），按裸名匹配；
    # 地址补 0x 前缀（nm 输出无前缀，bash 算术里前导零会被当八进制）
    local sym_addr
    sym_addr=$(nm -D "$module" 2>/dev/null | awk -v s="$sym" '
        index($3, s) == 1 && substr($3, length(s) + 1, 2) == "@@" { print "0x" $1; exit }')
    if [ -z "$sym_addr" ]; then
        sym_addr=$(nm "$module" 2>/dev/null | awk -v s="$sym" '$3 == s { print "0x" $1; exit }')
    fi
    if [ -z "$sym_addr" ]; then
        echo "  找不到符号: $sym"
        return 1
    fi
    local abs_addr
    abs_addr=$(printf "0x%x" "$((sym_addr + off))")
    addr2line -f -C -e "$module" "$abs_addr" 2>/dev/null || echo "  addr2line 失败"
}

# 逐行匹配帧；帧格式：  [n] /path/module(...) [0x...] 或  [n] (nil)
while IFS= read -r line; do
    frame=$(echo "$line" | grep -oE '\[[0-9]+\][[:space:]]*[^[:space:]]+\([^)]*\)' | sed -E 's/^\[[0-9]+\][[:space:]]*//')
    [ -n "$frame" ] || continue
    [ -n "${seen[$frame]:-}" ] && continue
    seen[$frame]=1
    echo "=== $frame ==="
    resolve "$frame"
done < "$log_file"
