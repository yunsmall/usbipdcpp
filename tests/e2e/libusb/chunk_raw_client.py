#!/usr/bin/env python3
"""bulk OUT 分块路径的线协议测试（不用 vhci/libusb 客户端）。

分块是流式流水线：服务器从 socket 读一块就提交给设备，读下一块时前一块
仍在设备侧传输，在途内存约为 depth × chunk。受 TCP 线序限制，receiver
必须把这笔 URB 的完整 payload 读完（且等所有在途块回调）才能解析下一条
命令，所以 CMD_UNLINK 总是排在这笔 URB 完成之后处理。

本脚本直接连 libusb_server，精确控制命令顺序和时序，覆盖：

1. 半包断开：声明 1MB 的 OUT，只发一部分 payload 就 RST。流式循环必须从
   socket 读错误收尾、清理会话，设备回到可用列表，重新导入后数据流还能
   从设备实际接收的位置接上。
2. 在途重复 seqnum：先发一条设备侧延迟应答的控制 IN，再发同 seqnum 的
   分块 OUT，必须回 EPIPE 且不读 payload；原控制传输仍正常完成。
3. 流水线顺序：大分块 OUT 之后立刻从另一端点发 IN。receiver 还在处理
   这笔 OUT，必须先回 OUT 的 RET_SUBMIT，再处理 IN。
4. 分组在途时客户端断开：流式循环读到 socket 错误后等待在途块回调落地，
   会话清理完设备可重新导入。
5. 在途 UNLINK：UNLINK 在 payload 之后，服务器必须先把整笔数据传完并回
   RET_SUBMIT(0)，再回 RET_UNLINK(0)；取消不可能抢在 URB 完成之前。
6. 零长分块 URB 的 UNLINK：先回 RET_SUBMIT(actual=0)，再回 RET_UNLINK(0)。
7. STALL 与 UNLINK 竞争：URB 以 EPIPE 结束并回 RET_SUBMIT，随后 UNLINK
   回 status=0；清除 halt 后 OUT 能继续。
8. 反复「大 OUT + 晚到 UNLINK + 恢复」：压 FFS 端点和分块缓冲回收的稳定性。
9. 已完成 URB 的 UNLINK：回 RET_UNLINK(0)。

用法（由 run_one.sh 的 chunk-raw 用例调用）：
    python3 chunk_raw_client.py --server-port 53271 --busid 7-1 \
        --in-ep 0x81 --out-ep 0x02
"""

import argparse
import socket
import struct
import sys
import time

import raw_common as rc
import usbip_raw as u
from raw_common import (
    Checker, EPIPE, RECIPIENT_DEVICE, REQUEST_DEVICE_TO_HOST, REQUEST_HOST_TO_DEVICE,
    REQUEST_VENDOR, control, control_submit, read_device_descriptor,
)

GET_OUT_STATS = 0x22
STALL_OUT_AFTER = 0x23
DELAY_IN = 0x17
SET_OUT_DELAY = 0x25
CLEAR_FEATURE = 0x01
ENDPOINT_HALT = 0x00
RECIPIENT_ENDPOINT = 0x02

# 设备侧延迟：每次 OUT 读取前睡 2ms。大 OUT 取 256KiB，按 512 字节/包
# 算约 1 秒，保证 IN/UNLINK 到达时流式循环仍在处理这笔 URB。
OUT_DELAY_MS = 2
LARGE_OUT_BYTES = 256 * 1024

ARGS = None


def get_out_stats(sock, seqnum, devid):
    ret = control(sock, seqnum, devid,
                  REQUEST_DEVICE_TO_HOST | REQUEST_VENDOR | RECIPIENT_DEVICE,
                  GET_OUT_STATS, 0, ARGS.out_ep, 16)
    if ret["status"] != 0 or len(ret["payload"]) != 16:
        return None, f"status={ret['status']} actual={ret['actual_length']}"
    chunks, total_bytes, errors, shorts = struct.unpack("<IIII", ret["payload"])
    return {"chunks": chunks, "bytes": total_bytes, "errors": errors, "shorts": shorts}, ""


def set_out_delay(sock, seqnum, devid, delay_ms):
    ret = control(sock, seqnum, devid,
                  REQUEST_HOST_TO_DEVICE | REQUEST_VENDOR | RECIPIENT_DEVICE,
                  SET_OUT_DELAY, delay_ms, ARGS.out_ep, 0)
    return ret["status"] == 0


def import_with_retry(checker, name):
    return rc.import_with_retry(ARGS.host, ARGS.server_port, ARGS.busid, checker, name)


def pattern_from(seed, length):
    return bytes((seed + i) & 0xff for i in range(length))


def read_ret_any(sock, in_seqnums=()):
    """读一条 RET_SUBMIT 或 RET_UNLINK，返回 (类型, 字典)。

    RET_SUBMIT 的 actual_length 只有在对应的 CMD_SUBMIT 是 IN 方向时才有
    数据段。in_seqnums 是本次会话发过的 IN seqnum 集合；OUT 应答只上报
    长度，线上没有数据，不能当成 IN 去读。
    """
    header = u.recv_exact(sock, 48)
    command, seqnum = struct.unpack("!II", header[:8])
    if command == u.RET_UNLINK:
        status = struct.unpack("!i", header[20:24])[0]
        return "unlink", {"seqnum": seqnum, "status": status}
    if command == u.RET_SUBMIT:
        status, actual = struct.unpack("!iI", header[20:28])
        number_of_packets = struct.unpack("!I", header[32:36])[0]
        payload = b""
        if seqnum in in_seqnums and actual > 0:
            payload = u.recv_exact(sock, actual)
        # 本文件只用非 iso 传输，number_of_packets 恒为 0
        if number_of_packets != 0:
            raise RuntimeError(f"RET_SUBMIT number_of_packets={number_of_packets}")
        return "submit", {"seqnum": seqnum, "status": status,
                          "actual_length": actual, "payload": payload}
    raise RuntimeError(f"未知应答命令码 0x{command:08x}")


def expect_no_more_responses(sock, detail_prefix):
    """短超时内不应再有任何应答（防止重复 RET_SUBMIT/RET_UNLINK）。"""
    previous = sock.gettimeout()
    sock.settimeout(0.5)
    try:
        data = sock.recv(1)
    except socket.timeout:
        return True, ""
    finally:
        sock.settimeout(previous)
    return False, (f"{detail_prefix}：收到额外应答字节 0x{data[0]:02x}" if data
                   else f"{detail_prefix}：连接被服务端关闭")


def recover_out(checker, sock, devid, seqnum, name, errors_baseline):
    """从设备实际接收位置继续一笔小 OUT，验证端点仍可用且数据流连续。

    @return (是否成功, 下一个可用 seqnum)
    """
    stats, detail = get_out_stats(sock, seqnum, devid)
    if stats is None:
        checker.check(False, name + "：读取恢复前统计", detail)
        return False, seqnum + 3
    data = pattern_from(stats["bytes"], 4096)
    out_seq = seqnum + 1
    stat_seq = seqnum + 2
    u.submit(sock, out_seq, devid, u.DIR_OUT, ARGS.out_ep & 0x7f,
             transfer_buffer_length=len(data), out_data=data)
    ret = u.read_ret_submit(sock, u.DIR_OUT)
    after, detail = get_out_stats(sock, stat_seq, devid)
    ok = (ret["seqnum"] == out_seq and ret["status"] == 0 and
          ret["actual_length"] == len(data) and after is not None and
          after["bytes"] == stats["bytes"] + len(data) and
          after["errors"] == errors_baseline)
    if not ok:
        detail = (f"ret_status={ret['status']} ret_actual={ret['actual_length']} "
                  f"bytes={after['bytes'] if after else '?'}/"
                  f"{stats['bytes'] + len(data)} "
                  f"errors={after['errors'] if after else '?'}/{errors_baseline}")
    checker.check(ok, name, detail)
    return ok, seqnum + 3


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

        # 声明 1MB，只发 3000 字节后 RST。流式循环会阻塞在 socket 上，
        # RST 让它从读错误路径收尾
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


def test_pipeline_order(checker):
    """大分块 OUT 还在传输时，另一端点 IN 的请求要排在它后面处理。"""
    reply = import_with_retry(checker, "流水线测试：导入")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        stats, detail = get_out_stats(sock, 50, devid)
        if stats is None:
            checker.check(False, "流水线测试：读取设备统计", detail)
            return
        if not set_out_delay(sock, 51, devid, OUT_DELAY_MS):
            checker.check(False, "流水线测试：设置设备 OUT 延迟")
            return

        data = pattern_from(stats["bytes"], LARGE_OUT_BYTES)
        u.submit(sock, 60, devid, u.DIR_OUT, ARGS.out_ep & 0x7f,
                 transfer_buffer_length=len(data), out_data=data)
        # OUT payload 之后立刻发另一端点（bulk IN）的 URB。streaming 的
        # receiver 还在读/发这笔 OUT，必须先把 OUT 完成并回 RET_SUBMIT，
        # 才能解析这条 IN；sender 按入队顺序发送，所以 OUT 应答先到
        u.submit(sock, 61, devid, u.DIR_IN, ARGS.in_ep & 0x7f, transfer_buffer_length=512)

        responses = [read_ret_any(sock, in_seqnums={61}) for _ in range(2)]
        out_ret = next((r for kind, r in responses
                        if kind == "submit" and r["seqnum"] == 60), None)
        in_ret = next((r for kind, r in responses
                       if kind == "submit" and r["seqnum"] == 61), None)
        order = [r["seqnum"] for _, r in responses]
        checker.check(order[0] == 60, "分块 OUT 完成前不处理下一端点命令",
                      f"期望先回 60，实际 {order}")
        checker.check(out_ret is not None and out_ret["status"] == 0 and
                      out_ret["actual_length"] == len(data),
                      "分块 OUT 完整传输", f"{out_ret}")
        checker.check(in_ret is not None and in_ret["status"] == 0 and
                      in_ret["actual_length"] == 512,
                      "另一端点 IN 正常完成", f"{in_ret}")

        checker.check(set_out_delay(sock, 62, devid, 0),
                      "流水线测试：恢复设备 OUT 延迟")
        after, detail = get_out_stats(sock, 63, devid)
        checker.check(after is not None and after["bytes"] == stats["bytes"] + len(data) and
                      after["errors"] == stats["errors"],
                      "流水线测试：设备收到全部数据且无连续性错误", detail)
    finally:
        sock.close()


def test_disconnect_in_flight(checker):
    """大分块 OUT 在途时客户端 RST：流式循环收尾，设备回到可用列表。"""
    reply = import_with_retry(checker, "在途断开测试：导入")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        stats, detail = get_out_stats(sock, 70, devid)
        if stats is None:
            checker.check(False, "在途断开测试：读取设备统计", detail)
            return
        if not set_out_delay(sock, 71, devid, OUT_DELAY_MS):
            checker.check(False, "在途断开测试：设置设备 OUT 延迟")
            return

        data = pattern_from(stats["bytes"], LARGE_OUT_BYTES)
        u.submit(sock, 72, devid, u.DIR_OUT, ARGS.out_ep & 0x7f,
                 transfer_buffer_length=len(data), out_data=data)
        # payload 已全部发完（server 已从 socket 读入/提交），设备侧还有
        # 约 1 秒才消费完；RST 后流式循环在下一块 socket 读上拿错误，
        # 等在途块回调落地后返回，会话正常清理
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        sock.close()
        sock = None
    finally:
        if sock is not None:
            sock.close()

    time.sleep(1.5)
    reply = import_with_retry(checker, "在途断开后重新导入")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        alive, detail = read_device_descriptor(sock, 73, devid)
        checker.check(alive, "分块 URB 在途断开后设备可重新导入", detail)
        if not alive:
            return
        checker.check(set_out_delay(sock, 74, devid, 0),
                      "在途断开测试：恢复设备 OUT 延迟")
        stats, detail = get_out_stats(sock, 75, devid)
        if stats is None:
            checker.check(False, "在途断开后读取设备统计", detail)
            return
        recover_out(checker, sock, devid, 200, "在途断开后分块路径仍能继续传输",
                    stats["errors"])
    finally:
        sock.close()


def test_unlink_after_payload(checker):
    """UNLINK 排在 payload 之后：先回完整 RET_SUBMIT(0)，再回 RET_UNLINK(0)。"""
    reply = import_with_retry(checker, "在途 UNLINK 测试：导入")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        stats, detail = get_out_stats(sock, 80, devid)
        if stats is None:
            checker.check(False, "在途 UNLINK 测试：读取设备统计", detail)
            return
        if not set_out_delay(sock, 81, devid, OUT_DELAY_MS):
            checker.check(False, "在途 UNLINK 测试：设置设备 OUT 延迟")
            return

        data = pattern_from(stats["bytes"], LARGE_OUT_BYTES)
        u.submit(sock, 82, devid, u.DIR_OUT, ARGS.out_ep & 0x7f,
                 transfer_buffer_length=len(data), out_data=data)
        u.send_unlink(sock, 83, devid, 82)

        # 线序决定了 UNLINK 只能在这笔 URB 的 payload 之后被解析；streaming
        # 此时已经同步完成整笔 URB，所以先有 RET_SUBMIT(0)，后有 UNLINK(0)
        submit_ret = u.read_ret_submit(sock, u.DIR_OUT)
        unlink_ret = u.read_ret_unlink(sock)
        ok = (submit_ret["seqnum"] == 82 and submit_ret["status"] == 0 and
              submit_ret["actual_length"] == len(data) and
              unlink_ret["seqnum"] == 83 and unlink_ret["status"] == 0)
        checker.check(ok, "在途 UNLINK 不打断已开始的 URB",
                      f"submit(status={submit_ret['status']},"
                      f"actual={submit_ret['actual_length']}) "
                      f"unlink(seq={unlink_ret['seqnum']},status={unlink_ret['status']})")
        no_more, detail = expect_no_more_responses(sock, "在途 UNLINK")
        checker.check(no_more, "在途 UNLINK 应答数量正确", detail)

        checker.check(set_out_delay(sock, 84, devid, 0),
                      "在途 UNLINK 测试：恢复设备 OUT 延迟")
        after, detail = get_out_stats(sock, 85, devid)
        checker.check(after is not None and after["bytes"] == stats["bytes"] + len(data),
                      "UNLINK 前数据已全部到达设备", detail)
        errors_baseline = after["errors"] if after is not None else stats["errors"]
        recover_out(checker, sock, devid, 210, "在途 UNLINK 后 OUT 仍连续",
                    errors_baseline)
        alive, detail = read_device_descriptor(sock, 86, devid)
        checker.check(alive, "在途 UNLINK 后会话仍正常", detail)
    finally:
        sock.close()


def test_unlink_zero_length(checker):
    """零长分块 URB 被 UNLINK：先回 RET_SUBMIT(0,0)，再回 RET_UNLINK(0)。"""
    reply = import_with_retry(checker, "零长 UNLINK 测试：导入")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        stats, detail = get_out_stats(sock, 130, devid)
        if stats is None:
            checker.check(False, "零长 UNLINK 测试：读取设备统计", detail)
            return
        if not set_out_delay(sock, 131, devid, 50):
            checker.check(False, "零长 UNLINK 测试：设置设备 OUT 延迟")
            return

        u.submit(sock, 132, devid, u.DIR_OUT, ARGS.out_ep & 0x7f,
                 transfer_buffer_length=0)
        u.send_unlink(sock, 133, devid, 132)
        submit_ret = u.read_ret_submit(sock, u.DIR_OUT)
        unlink_ret = u.read_ret_unlink(sock)
        checker.check(submit_ret["seqnum"] == 132 and submit_ret["status"] == 0 and
                      submit_ret["actual_length"] == 0 and
                      unlink_ret["seqnum"] == 133 and unlink_ret["status"] == 0,
                      "零长分块 URB 的 UNLINK 语义",
                      f"submit(status={submit_ret['status']},"
                      f"actual={submit_ret['actual_length']}) "
                      f"unlink(seq={unlink_ret['seqnum']},status={unlink_ret['status']})")
        no_more, detail = expect_no_more_responses(sock, "零长 UNLINK")
        checker.check(no_more, "零长 UNLINK 应答数量正确", detail)

        checker.check(set_out_delay(sock, 134, devid, 0),
                      "零长 UNLINK 测试：恢复设备 OUT 延迟")
        after, detail = get_out_stats(sock, 135, devid)
        checker.check(after is not None and after["bytes"] == stats["bytes"],
                      "零长 UNLINK 不改变设备字节数", detail)
        errors_baseline = after["errors"] if after is not None else stats["errors"]
        recover_out(checker, sock, devid, 220, "零长 UNLINK 后 OUT 仍连续",
                    errors_baseline)
        alive, detail = read_device_descriptor(sock, 136, devid)
        checker.check(alive, "零长 UNLINK 后会话仍正常", detail)
    finally:
        sock.close()


def test_stall_then_unlink(checker):
    """设备 STALL 后紧跟 UNLINK：URB 先以 EPIPE 结束，UNLINK 回 status=0。"""
    reply = import_with_retry(checker, "STALL+UNLINK 测试：导入")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        stats, detail = get_out_stats(sock, 140, devid)
        if stats is None:
            checker.check(False, "STALL+UNLINK 测试：读取设备统计", detail)
            return
        stall_arm = control(sock, 141, devid,
                            REQUEST_HOST_TO_DEVICE | REQUEST_VENDOR | RECIPIENT_DEVICE,
                            STALL_OUT_AFTER, 1, ARGS.out_ep, 0)
        if stall_arm["status"] != 0:
            checker.check(False, "STALL+UNLINK 测试：布防 STALL")
            return
        if not set_out_delay(sock, 142, devid, OUT_DELAY_MS):
            checker.check(False, "STALL+UNLINK 测试：设置设备 OUT 延迟")
            return

        data = pattern_from(stats["bytes"], LARGE_OUT_BYTES)
        u.submit(sock, 143, devid, u.DIR_OUT, ARGS.out_ep & 0x7f,
                 transfer_buffer_length=len(data), out_data=data)
        # 先让设备读到第一包并真正 halt（2ms/包的延迟下 50ms 足够），再发
        # UNLINK，确保测的是「STALL 已发生」之后晚到的 UNLINK
        time.sleep(0.05)
        u.send_unlink(sock, 144, devid, 143)

        submit_ret = u.read_ret_submit(sock, u.DIR_OUT)
        unlink_ret = u.read_ret_unlink(sock)
        checker.check(submit_ret["seqnum"] == 143 and submit_ret["status"] == EPIPE and
                      unlink_ret["seqnum"] == 144 and unlink_ret["status"] == 0,
                      "STALL 的 URB 先回 EPIPE，UNLINK 再回 status=0",
                      f"submit(status={submit_ret['status']},"
                      f"actual={submit_ret['actual_length']}) "
                      f"unlink(seq={unlink_ret['seqnum']},status={unlink_ret['status']})")
        no_more, detail = expect_no_more_responses(sock, "STALL+UNLINK")
        checker.check(no_more, "STALL+UNLINK 应答数量正确", detail)

        checker.check(set_out_delay(sock, 145, devid, 0),
                      "STALL+UNLINK 测试：恢复设备 OUT 延迟")
        # 清除可能残留的 STALL 布防，避免恢复传输再次 halt 端点
        clear_arm = control(sock, 149, devid,
                            REQUEST_HOST_TO_DEVICE | REQUEST_VENDOR | RECIPIENT_DEVICE,
                            STALL_OUT_AFTER, 0, ARGS.out_ep, 0)
        checker.check(clear_arm["status"] == 0, "STALL+UNLINK 测试：清除 STALL 布防",
                      f"status={clear_arm['status']}")
        # 标准 CLEAR_FEATURE(ENDPOINT_HALT)：服务器 tweak 成 libusb_clear_halt
        clear = control(sock, 146, devid,
                        RECIPIENT_ENDPOINT, CLEAR_FEATURE, ENDPOINT_HALT, ARGS.out_ep, 0)
        checker.check(clear["status"] == 0, "STALL 后 CLEAR_FEATURE 成功",
                      f"status={clear['status']}")

        after, detail = get_out_stats(sock, 147, devid)
        if after is None:
            checker.check(False, "STALL+UNLINK 后读取设备统计", detail)
            return
        errors_baseline = after["errors"]
        recover_out(checker, sock, devid, 230, "STALL+UNLINK 后 OUT 仍连续",
                    errors_baseline)
        alive, detail = read_device_descriptor(sock, 148, devid)
        checker.check(alive, "STALL+UNLINK 后会话仍正常", detail)
    finally:
        sock.close()


def test_repeated_unlink_recovery(checker):
    """反复「大 OUT + 晚到 UNLINK + 恢复」，压 FFS 端点状态和缓冲回收。"""
    reply = import_with_retry(checker, "重复 UNLINK 测试：导入")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        stats, detail = get_out_stats(sock, 150, devid)
        if stats is None:
            checker.check(False, "重复 UNLINK 测试：读取设备统计", detail)
            return
        if not set_out_delay(sock, 151, devid, 1):
            checker.check(False, "重复 UNLINK 测试：设置设备 OUT 延迟")
            return
        base_errors = stats["errors"]
        seq = 160
        all_ok = True
        for index in range(8):
            cur, detail = get_out_stats(sock, seq, devid)
            seq += 1
            if cur is None:
                checker.check(False, f"重复 UNLINK #{index + 1}：读取统计", detail)
                all_ok = False
                break
            data = pattern_from(cur["bytes"], 16 * 1024)
            out_seq = seq
            unlink_seq = seq + 1
            seq += 2
            u.submit(sock, out_seq, devid, u.DIR_OUT, ARGS.out_ep & 0x7f,
                     transfer_buffer_length=len(data), out_data=data)
            u.send_unlink(sock, unlink_seq, devid, out_seq)
            submit_ret = u.read_ret_submit(sock, u.DIR_OUT)
            unlink_ret = u.read_ret_unlink(sock)
            transfer_ok = (submit_ret["seqnum"] == out_seq and
                           submit_ret["status"] == 0 and
                           submit_ret["actual_length"] == len(data) and
                           unlink_ret["seqnum"] == unlink_seq and
                           unlink_ret["status"] == 0)
            now, detail = get_out_stats(sock, seq, devid)
            seq += 1
            if now is None:
                checker.check(False, f"重复 UNLINK #{index + 1}：完成后统计", detail)
                all_ok = False
                break
            rec = pattern_from(now["bytes"], 4096)
            rec_seq = seq
            seq += 1
            u.submit(sock, rec_seq, devid, u.DIR_OUT, ARGS.out_ep & 0x7f,
                     transfer_buffer_length=len(rec), out_data=rec)
            ret = u.read_ret_submit(sock, u.DIR_OUT)
            after, detail = get_out_stats(sock, seq, devid)
            seq += 1
            ok = (transfer_ok and ret["seqnum"] == rec_seq and ret["status"] == 0 and
                  ret["actual_length"] == len(rec) and after is not None and
                  after["bytes"] == now["bytes"] + len(rec) and
                  after["errors"] == base_errors)
            if not ok:
                detail = (f"transfer_ok={transfer_ok} ret_status={ret['status']} "
                          f"ret_actual={ret['actual_length']} "
                          f"bytes={after['bytes'] if after else '?'}/"
                          f"{now['bytes'] + len(rec)} "
                          f"errors={after['errors'] if after else '?'}/{base_errors}")
            checker.check(ok, f"重复 UNLINK+恢复 #{index + 1}", detail)
            all_ok = all_ok and ok

        checker.check(set_out_delay(sock, seq, devid, 0),
                      "重复 UNLINK 测试：恢复设备 OUT 延迟")
        checker.check(all_ok, "反复 UNLINK 后 FFS OUT 端点状态稳定")
        alive, detail = read_device_descriptor(sock, seq + 1, devid)
        checker.check(alive, "重复 UNLINK 后会话仍正常", detail)
    finally:
        sock.close()


def test_unlink_after_completed_out(checker):
    """已完成（entry 已移除）的 URB 被 UNLINK：回 RET_UNLINK(0)。"""
    reply = import_with_retry(checker, "完成后 UNLINK：导入")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        stats, detail = get_out_stats(sock, 40, devid)
        if stats is None:
            checker.check(False, "完成后 UNLINK：读取设备统计", detail)
            return

        data = pattern_from(stats["bytes"], 4096)
        u.submit(sock, 41, devid, u.DIR_OUT, ARGS.out_ep & 0x7f,
                 transfer_buffer_length=len(data), out_data=data)
        submit_ret = u.read_ret_submit(sock, u.DIR_OUT)
        ok = (submit_ret["seqnum"] == 41 and submit_ret["status"] == 0 and
              submit_ret["actual_length"] == len(data))
        checker.check(ok, "完成后 UNLINK：OUT 正常完成",
                      f"status={submit_ret['status']} actual={submit_ret['actual_length']}")

        u.send_unlink(sock, 42, devid, 41)
        unlink_ret = u.read_ret_unlink(sock)
        checker.check(unlink_ret["seqnum"] == 42 and unlink_ret["status"] == 0,
                      "已完成 URB 的 UNLINK 回 status=0",
                      f"seq={unlink_ret['seqnum']} status={unlink_ret['status']}")

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
    parser = argparse.ArgumentParser(description="bulk OUT 分块路径的线协议测试")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--server-port", type=int, required=True)
    parser.add_argument("--busid", required=True)
    parser.add_argument("--in-ep", type=lambda x: int(x, 0), required=True)
    parser.add_argument("--out-ep", type=lambda x: int(x, 0), required=True)
    ARGS = parser.parse_args()

    checker = Checker()
    test_half_payload_disconnect(checker)
    test_duplicate_seqnum(checker)
    test_pipeline_order(checker)
    test_disconnect_in_flight(checker)
    test_unlink_after_payload(checker)
    test_unlink_zero_length(checker)
    test_stall_then_unlink(checker)
    test_repeated_unlink_recovery(checker)
    test_unlink_after_completed_out(checker)
    return checker.summary()


if __name__ == "__main__":
    sys.exit(main())
