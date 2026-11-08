#!/usr/bin/env bash
# uvc 分辨率协商：以非默认 640x480@30 启动服务器，验证描述符随参数生成
# （v4l2-ctl 枚举出的分辨率正确）、ffmpeg 按该分辨率拉流成功。
# 现有 test_uvc 只验证默认 320x240 能出帧，覆盖不到描述符宽高参数路径。
set -e
source "$(dirname "$0")/common.sh"

setup_workdir uvc_formats
echo "== uvc_formats: 启动服务器（640x480@30）"
start_server mock_uvc --width 640 --height 480 --fps 30 || { report uvc_formats; exit 1; }
attach_device 1-1

VIDEO=$(wait_dev "/dev/video" 15) || true
assert "video 设备出现（$VIDEO）" [ -n "$VIDEO" ]
[ -n "$VIDEO" ] || { stop_server; report uvc_formats; exit 1; }

# 枚举格式：必须能在内核侧看到 YUYV 且分辨率是启动参数给的 640x480
if command -v v4l2-ctl > /dev/null 2>&1; then
    FORMATS=$(v4l2-ctl -d "$VIDEO" --list-formats-ext 2>/dev/null || true)
    assert "枚举到 YUYV 格式" grep -qi "YUYV" <<<"$FORMATS"
    assert "枚举到 640x480" grep -q "640x480" <<<"$FORMATS"
else
    echo "  [SKIP] 无 v4l2-ctl，跳过格式枚举（ffmpeg 校验仍执行）"
fi

if command -v ffmpeg > /dev/null 2>&1; then
    OUT="$WORK_DIR/frame_640x480.jpg"
    timeout 15 ffmpeg -loglevel error -f v4l2 -video_size 640x480 -i "$VIDEO" \
        -frames:v 5 -update 1 -y "$OUT" || true
    assert "按 640x480 拉流成功（$OUT）" [ -s "$OUT" ]

    # 实际输出分辨率要与请求一致（ffprobe 可用时）
    if command -v ffprobe > /dev/null 2>&1; then
        DIM=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
            -of csv=p=0 "$OUT" 2>/dev/null || true)
        assert "实际帧分辨率为 640,480（$DIM）" [ "$DIM" = "640,480" ]
    fi

    # 连续第二次打开取流：模拟应用关闭后重新打开摄像头（排空/重配 alt setting）
    OUT2="$WORK_DIR/frame_second.jpg"
    timeout 15 ffmpeg -loglevel error -f v4l2 -video_size 640x480 -i "$VIDEO" \
        -frames:v 3 -update 1 -y "$OUT2" || true
    assert "关闭后重新打开取流成功（$OUT2）" [ -s "$OUT2" ]
else
    echo "  [SKIP] 无 ffmpeg，跳过取流验证"
fi

stop_server
report uvc_formats
