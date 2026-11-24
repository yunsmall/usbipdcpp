#!/usr/bin/env python3
"""bulk OUT 分块路径的线协议负路径测试。

不经过 vhci/libusb 客户端，直接连 libusb_server 构造两种正常客户端很难
复现的场景：

1. 半包断开：声明 1MB 的 OUT，只发一部分 payload 就 RST。服务端的分块
   流水线必须能收尾、清理会话，设备回到可用列表，重新导入后数据流还能
   从设备实际接收的位置接上。
2. 在途重复 seqnum：先发一条设备侧延迟应答的控制 IN（同 seqnum 仍在
   transfers_ 里），再发同 seqnum 的分块 OUT，必须回 EPIPE 且不读 payload；
   延迟的控制传输仍能正常完成，会话不断。

用法（由 run_one.sh 的 chunk-raw 用例调用）：
    python3 chunk_raw_client.py --server-port 53271 --busid 7-1 \
        --in-ep 0x81 --out-ep 0x02
"""

import argparse
import socket
import struct
import sys
import time

import usbip_raw as u
import raw_common as rc
from raw_common import (
    Checker, EPIPE, RECIPIENT_DEVICE, REQUEST_DEVICE_TO_HOST, REQUEST_VENDOR,
    control, control_submit, read_device_descriptor,
)

GET_OUT_STATS = 0x22
DELAY_IN = 0x17

ARGS = None


def get_out_stats(sock, seqnum, devid):
    ret = control(sock, seqnum, devid,
                  REQUEST_DEVICE_TO_HOST | REQUEST_VENDOR | RECIPIENT_DEVICE,
                  GET_OUT_STATS, 0, ARGS.out_ep, 16)
    if ret["status"] != 0 or len(ret["payload"]) != 16:
        return None, f"status={ret['status']} actual={ret['actual_length']}"
    chunks, total_bytes, errors, shorts = struct.unpack("<IIII", ret["payload"])
    return {"chunks": chunks, "bytes": total_bytes, "errors": errors, "shorts": shorts}, ""


def import_with_retry(checker, name):
    return rc.import_with_retry(ARGS.host, ARGS.server_port, ARGS.busid, checker, name)


def pattern_from(seed, length):
    return bytes((seed + i) & 0xff for i in range(length))


def test_half_payload_disconnect(checker):
    reply = import_with_retry(checker, "半包测试：首次导入")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        stats, detail = get_out_stats(sock, 1, devid)
        if stats is None:
            checker.check(False, "半包测试：读取设备统计", detail)
            return

        # 声明 1MB，只发 3000 字节后 RST。分块流水线会读到半块后阻塞在
        # socket 上，RST 让它从读错误路径收尾
        sock.sendall(u.submit_header(2, devid, u.DIR_OUT, ARGS.out_ep & 0x7f,
                                     transfer_buffer_length=1 << 20))
        sock.sendall(pattern_from(stats["bytes"], 3000))
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        sock.close()
        sock = None
    finally:
        if sock is not None:
            sock.close()

    time.sleep(0.5)
    reply = import_with_retry(checker, "半包断开后重新导入")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        ok, detail = read_device_descriptor(sock, 10, devid)
        checker.check(ok, "半包断开后会话仍可用", detail)
        if not ok:
            return

        stats, detail = get_out_stats(sock, 11, devid)
        if stats is None:
            checker.check(False, "半包断开后读取设备统计", detail)
            return
        # 设备只接收了半包中的前若干个完整分块；从实际字节数继续同一条
        # 递增数据流，验证分块路径没有把后续流的位置搞乱
        data = pattern_from(stats["bytes"], 512)
        u.submit(sock, 12, devid, u.DIR_OUT, ARGS.out_ep & 0x7f,
                 transfer_buffer_length=len(data), out_data=data)
        ret = u.read_ret_submit(sock, u.DIR_OUT)
        after, detail = get_out_stats(sock, 13, devid)
        ok = (ret["seqnum"] == 12 and ret["status"] == 0 and
              ret["actual_length"] == len(data) and after is not None and
              after["bytes"] == stats["bytes"] + len(data) and
              after["errors"] == stats["errors"])
        if not ok:
            detail = (f"ret_status={ret['status']} ret_actual={ret['actual_length']} "
                      f"bytes={after['bytes'] if after else '?'}/"
                      f"{stats['bytes'] + len(data)}")
        checker.check(ok, "半包断开后分块 OUT 仍能接续数据流", detail)
    finally:
        sock.close()


def test_duplicate_seqnum(checker):
    reply = import_with_retry(checker, "重复 seqnum 测试：导入")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        stats, detail = get_out_stats(sock, 20, devid)
        if stats is None:
            checker.check(False, "重复 seqnum 测试：读取设备统计", detail)
            return

        # 设备侧延迟 500ms 应答的控制 IN：该 seqnum 在 transfers_ 里保持
        # 在途；紧接着同 seqnum 的分块 OUT 必须被拒绝
        control_submit(sock, 21, devid,
                       REQUEST_DEVICE_TO_HOST | REQUEST_VENDOR | RECIPIENT_DEVICE,
                       DELAY_IN, 500, 0, 1)
        data = pattern_from(stats["bytes"], 2048)
        u.submit(sock, 21, devid, u.DIR_OUT, ARGS.out_ep & 0x7f,
                 transfer_buffer_length=len(data), out_data=data)

        first = u.read_ret_submit(sock, u.DIR_IN)
        second = u.read_ret_submit(sock, u.DIR_IN)
        rets = [first, second]
        rejected = any(r["seqnum"] == 21 and r["status"] == EPIPE and
                       r["actual_length"] == 0 for r in rets)
        delayed = any(r["seqnum"] == 21 and r["status"] == 0 and
                      r["actual_length"] == 1 and r["payload"] == b"\xa5" for r in rets)
        checker.check(rejected, "分块 OUT 与在途传输重复 seqnum 被拒绝",
                      " ".join(f"seq={r['seqnum']} status={r['status']} "
                               f"actual={r['actual_length']}" for r in rets))
        checker.check(delayed, "重复 seqnum 被拒后原在途传输正常完成",
                      " ".join(f"seq={r['seqnum']} status={r['status']} "
                               f"actual={r['actual_length']}" for r in rets))

        ok, detail = read_device_descriptor(sock, 22, devid)
        checker.check(ok, "重复 seqnum 后会话仍正常", detail)
    finally:
        sock.close()


def read_ret_any(sock):
    """读一条 RET_SUBMIT 或 RET_UNLINK，返回 (类型, 字典)。

    这里只会出现 OUT 的 RET_SUBMIT 与 RET_UNLINK：OUT 应答的
    actual_length 只是上报长度，线上没有数据段，不能被当成 IN 去读。
    """
    header = u.recv_exact(sock, 48)
    command, seqnum = struct.unpack("!II", header[:8])
    if command == u.RET_UNLINK:
        status = struct.unpack("!i", header[20:24])[0]
        return "unlink", {"seqnum": seqnum, "status": status}
    if command == u.RET_SUBMIT:
        status, actual = struct.unpack("!iI", header[20:28])
        return "submit", {"seqnum": seqnum, "status": status, "actual_length": actual}
    raise RuntimeError(f"未知应答命令码 0x{command:08x}")


def test_unlink_after_chunked_out(checker):
    """分块 OUT 完成后才被处理的 UNLINK 必须回 RET_UNLINK(0)。"""
    reply = import_with_retry(checker, "UNLINK 测试：导入")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        stats, detail = get_out_stats(sock, 40, devid)
        if stats is None:
            checker.check(False, "UNLINK 测试：读取设备统计", detail)
            return

        data = pattern_from(stats["bytes"], 4096)
        u.submit(sock, 41, devid, u.DIR_OUT, ARGS.out_ep & 0x7f,
                 transfer_buffer_length=len(data), out_data=data)
        # UNLINK 紧跟在 payload 后面：receiver 处理完整个分块 URB 才会读到它，
        # 此时传输已完成，服务端应回 RET_UNLINK(0) 而不是取消
        u.send_unlink(sock, 42, devid, 41)

        responses = [read_ret_any(sock), read_ret_any(sock)]
        submit_ret = next((r for kind, r in responses if kind == "submit" and r["seqnum"] == 41),
                          None)
        unlink_ret = next((r for kind, r in responses if kind == "unlink" and r["seqnum"] == 42),
                          None)
        ok = (submit_ret is not None and submit_ret["status"] == 0 and
              submit_ret["actual_length"] == len(data) and
              unlink_ret is not None and unlink_ret["status"] == 0)
        checker.check(ok, "分块 OUT 完成后的 UNLINK 回 status=0",
                      " ".join(f"{kind}:seq={r['seqnum']} status={r['status']}"
                               for kind, r in responses))

        after, detail = get_out_stats(sock, 43, devid)
        checker.check(after is not None and after["bytes"] == stats["bytes"] + len(data) and
                      after["errors"] == stats["errors"],
                      "UNLINK 不影响已完成的 OUT", detail)
        alive, detail = read_device_descriptor(sock, 44, devid)
        checker.check(alive, "UNLINK 后会话仍正常", detail)
    finally:
        sock.close()


def main():
    global ARGS
    parser = argparse.ArgumentParser(description="bulk OUT 分块路径的线协议负路径测试")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--server-port", type=int, required=True)
    parser.add_argument("--busid", required=True)
    parser.add_argument("--in-ep", type=lambda x: int(x, 0), required=True)
    parser.add_argument("--out-ep", type=lambda x: int(x, 0), required=True)
    ARGS = parser.parse_args()

    checker = Checker()
    test_half_payload_disconnect(checker)
    test_duplicate_seqnum(checker)
    test_unlink_after_chunked_out(checker)
    return checker.summary()


if __name__ == "__main__":
    sys.exit(main())
