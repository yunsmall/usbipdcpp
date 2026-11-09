#!/usr/bin/env python3
"""libusb_server 负路径测试：直接走 USB/IP 线协议，绕开 libusb 的前置检查。

libusb 客户端在本地就会拒绝的请求（当前 alt 不存在的端点、wLength 超过
缓冲区、number_of_packets=0、非法 iso 描述符等）根本到不了服务端，服务端
自己的校验/应答/清理路径因此没有 e2e 覆盖。本脚本直接连 libusb_server，
按协议构造这些请求，逐条断言服务端行为，并在每条错误后验证会话/设备仍
可恢复。

用法（由 run_one.sh 的 raw 用例调用）：
    python3 raw_client.py --server-port 53267 --busid 1-1
"""

import argparse
import socket
import struct
import sys
import time

import usbip_raw as u

EPIPE = -32

GET_DESCRIPTOR = 0x06
SET_INTERFACE = 0x0B
CLEAR_FEATURE = 0x01
REQUEST_DEVICE_TO_HOST = 0x80
REQUEST_HOST_TO_DEVICE = 0x00
REQUEST_STANDARD = 0x00
REQUEST_VENDOR = 0x40
RECIPIENT_DEVICE = 0x00
RECIPIENT_INTERFACE = 0x01
RECIPIENT_ENDPOINT = 0x02


class Checker:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, ok, name, detail=""):
        if ok:
            self.passed += 1
            print(f"[PASS] {name}")
        else:
            self.failed += 1
            print(f"[FAIL] {name}" + (f" ({detail})" if detail else ""))

    def summary(self):
        print(f"==== {self.passed} passed, {self.failed} failed ====")
        return 0 if self.failed == 0 else 1


def setup_packet(bm_request_type, request, value, index, length):
    return struct.pack("<BBHHH", bm_request_type, request, value, index, length)


def control(sock, seqnum, devid, bm_request_type, request, value, index, length,
            transfer_buffer_length=None):
    """发一条控制传输（方向由 bmRequestType 的最高位决定）。"""
    direction = u.DIR_IN if bm_request_type & REQUEST_DEVICE_TO_HOST else u.DIR_OUT
    if transfer_buffer_length is None:
        transfer_buffer_length = length
    u.submit(sock, seqnum, devid, direction, 0,
             setup=setup_packet(bm_request_type, request, value, index, length),
             transfer_buffer_length=transfer_buffer_length)


def read_device_descriptor(sock, seqnum, devid):
    """发 GET_DESCRIPTOR(Device) 并校验应答；用于证明会话仍然正常。"""
    control(sock, seqnum, devid, REQUEST_DEVICE_TO_HOST | REQUEST_STANDARD | RECIPIENT_DEVICE,
            GET_DESCRIPTOR, 0x0100, 0, 18)
    ret = u.read_ret_submit(sock, u.DIR_IN)
    ok = (ret["seqnum"] == seqnum and ret["status"] == 0 and
          ret["actual_length"] == 18 and len(ret["payload"]) == 18 and
          ret["payload"][0] == 18)
    detail = (f"seq={ret['seqnum']} status={ret['status']} "
              f"actual={ret['actual_length']}") if not ok else ""
    return ok, detail


def set_interface(sock, seqnum, devid, alternate):
    control(sock, seqnum, devid,
            REQUEST_HOST_TO_DEVICE | REQUEST_STANDARD | RECIPIENT_INTERFACE,
            SET_INTERFACE, alternate, 0, 0)
    ret = u.read_ret_submit(sock, u.DIR_OUT)
    return ret["seqnum"] == seqnum and ret["status"] == 0, ret


def set_in_delay(sock, seqnum, devid, milliseconds):
    control(sock, seqnum, devid,
            REQUEST_HOST_TO_DEVICE | REQUEST_VENDOR | RECIPIENT_INTERFACE,
            0x32, milliseconds, 0, 0)
    ret = u.read_ret_submit(sock, u.DIR_OUT)
    return ret["seqnum"] == seqnum and ret["status"] == 0


def submit_iso_in(sock, seqnum, devid, length, descriptors):
    u.submit(sock, seqnum, devid, u.DIR_IN, 1, transfer_buffer_length=length,
             number_of_packets=len(descriptors), iso_descriptors=descriptors)


def expect_closed(sock, timeout=5.0):
    """等对端关闭连接。

    @return (是否在超时前关闭, 失败说明)：失败说明区分超时和收到多余字节，
            避免排查时分不清是服务端没断连还是断连前多发了东西
    """
    sock.settimeout(timeout)
    try:
        data = sock.recv(1)
    except socket.timeout:
        return False, f"{timeout}s 内没有关闭"
    except OSError:
        return True, ""
    if data == b"":
        return True, ""
    return False, f"关闭前收到多余字节 {data.hex()}"


def read_ret_submit_optional(sock, direction, timeout=2.0):
    """尝试读一条 RET_SUBMIT；对端在应答前关闭或超时返回 None。

    严格策略下提交失败会直接丢弃连接（与内核 SDEV_EVENT_ERROR_SUBMIT 一致），
    应答是尽力而为的，不能假设一定存在。
    """
    sock.settimeout(timeout)
    try:
        return u.read_ret_submit(sock, direction)
    except (ConnectionError, OSError):
        return None


def import_with_retry(checker, name):
    """会话结束后服务端要清理完才把设备放回可用列表，留足重试窗口。"""
    for attempt in range(60):
        try:
            return u.connect_and_import(ARGS.host, ARGS.server_port, ARGS.busid)
        except (ConnectionError, RuntimeError, socket.timeout) as error:
            if attempt == 59:
                checker.check(False, name, str(error))
                return None
            time.sleep(0.25)
    return None


def run_negative_tests(checker):
    reply = u.connect_and_import(ARGS.host, ARGS.server_port, ARGS.busid)
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        # 1. 当前 alt（0）没有端点：服务端静默丢弃，不回任何响应。
        #    随后用一条正常请求证明“丢弃”没有留下半个会话；读到的第一条
        #    应答必须是探测请求的 seq，不能是没被丢弃的 CMD_SUBMIT 的应答。
        u.submit(sock, 1, devid, u.DIR_IN, 1, transfer_buffer_length=1024)
        ok, detail = read_device_descriptor(sock, 2, devid)
        checker.check(ok, "不存在的端点被静默丢弃（无多余应答）", detail)

        # 2. 切到 alt 1，后续 iso 请求才有端点
        ok, ret = set_interface(sock, 3, devid, 1)
        checker.check(ok, "SET_INTERFACE alt 1", f"status={ret['status']}")
        if not ok:
            return

        # 3. 控制传输 wLength > transfer_buffer_length：必须拒绝且不断连
        #    （防 libusb 按 wLength 写越界）
        control(sock, 4, devid, REQUEST_DEVICE_TO_HOST | REQUEST_STANDARD | RECIPIENT_DEVICE,
                GET_DESCRIPTOR, 0x0100, 0, 100, transfer_buffer_length=8)
        ret = u.read_ret_submit(sock, u.DIR_IN)
        checker.check(ret["seqnum"] == 4 and ret["status"] == EPIPE,
                      "wLength 超过 transfer_buffer_length 被拒绝",
                      f"status={ret['status']} actual={ret['actual_length']}")
        ok, detail = read_device_descriptor(sock, 5, devid)
        checker.check(ok, "wLength 拒绝后会话仍正常", detail)

        # 4. tweak 失败后的降级提交：对 0x02 发 CLEAR_FEATURE。0x02 是当前
        #    alt 1 里的等时 OUT 端点（存在，但等时端点不可 halt），hop1 的
        #    libusb_clear_halt 返回 PIPE；服务端应把请求当作普通控制传输
        #    转给设备，由设备按 USB 2.0 §9.4.5 回 Request Error，会话不能断。
        #    不能用当前 alt 不存在的地址（如 0x0F/0x83）：0x0F 会被 usbfs 的
        #    check_ctrlrecip() 在提交阶段按描述符拒绝（ENOENT→LIBUSB_ERROR_IO）；
        #    0x83 虽能通过描述符校验，但虚拟设备对“当前 alt 找不到端点”的
        #    请求会同时置 ec 并按内部错误拆掉 hop，测的就不是这条路径了
        control(sock, 41, devid,
                REQUEST_HOST_TO_DEVICE | REQUEST_STANDARD | RECIPIENT_ENDPOINT,
                CLEAR_FEATURE, 0x0000, 0x02, 0)
        ret = u.read_ret_submit(sock, u.DIR_OUT)
        checker.check(ret["seqnum"] == 41 and ret["status"] == EPIPE,
                      "CLEAR_FEATURE 降级提交由设备回 EPIPE",
                      f"status={ret['status']}")
        ok, detail = read_device_descriptor(sock, 42, devid)
        checker.check(ok, "CLEAR_FEATURE 降级后会话仍正常", detail)

        # 5. 重复 seqnum：第一条挂起（设备延迟 500ms），第二条必须回 EPIPE，
        #    且第一条仍能正常完成（pending 记录没有互相踩）
        checker.check(set_in_delay(sock, 6, devid, 500), "设置设备 IN 延迟")
        submit_iso_in(sock, 7, devid, 1024, [(0, 1024, 0, 0)])
        submit_iso_in(sock, 7, devid, 1024, [(0, 1024, 0, 0)])
        first = u.read_ret_submit(sock, u.DIR_IN)
        checker.check(first["seqnum"] == 7 and first["status"] == EPIPE,
                      "重复 seqnum 被拒绝", f"status={first['status']}")
        second = u.read_ret_submit(sock, u.DIR_IN)
        iso_ok = (second["seqnum"] == 7 and second["status"] == 0 and
                  second["actual_length"] == 1024 and len(second["iso"]) == 1 and
                  second["iso"][0][2] == 1024)
        checker.check(iso_ok, "重复 seqnum 后原传输仍正常完成",
                      f"status={second['status']} actual={second['actual_length']} "
                      f"iso={second['iso']}")
        checker.check(set_in_delay(sock, 8, devid, 0), "清除设备 IN 延迟")
        ok, detail = read_device_descriptor(sock, 9, devid)
        checker.check(ok, "重复 seqnum 后会话仍正常", detail)

        # 6. ISO 的 number_of_packets=0：libusb 拒绝。严格策略下服务端按内核
        #    语义（stub_rx.c 的 SDEV_EVENT_ERROR_SUBMIT）直接丢连接、不回
        #    RET_SUBMIT；若确实收到应答，它必须是 EPIPE
        submit_iso_in(sock, 10, devid, 1024, [])
        ret = read_ret_submit_optional(sock, u.DIR_IN)
        checker.check(ret is None or (ret["seqnum"] == 10 and ret["status"] == EPIPE),
                      "ISO number_of_packets=0 被拒绝",
                      "" if ret is None else f"status={ret['status']}")
        closed, why = expect_closed(sock)
        checker.check(closed, "提交失败后按策略断开会话", why)
    finally:
        sock.close()

    # 7. 非法 ISO 描述符（长度和超过缓冲区）必须断开会话而不是越界写；
    #    顺带验证上一轮断开后设备回到可用、可以重新导入
    reply = import_with_retry(checker, "断开后重新导入设备")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        checker.check(True, "断开后重新导入设备")
        ok, ret = set_interface(sock, 1, devid, 1)
        checker.check(ok, "重新导入后 SET_INTERFACE alt 1", f"status={ret['status']}")
        submit_iso_in(sock, 2, devid, 1024, [(0, 2048, 0, 0)])
        closed, why = expect_closed(sock)
        checker.check(closed, "非法 ISO 描述符断开会话", why)
    finally:
        sock.close()

    # 8. 传输挂起时客户端直接断 socket（不 detach、不发 UNLINK）：服务端
    #    必须取消在途传输、把设备放回可用列表，之后还能重新导入
    reply = import_with_retry(checker, "异常断开后重新导入设备")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        checker.check(True, "异常断开后重新导入设备")
        set_interface(sock, 1, devid, 1)
        set_in_delay(sock, 2, devid, 2000)
        submit_iso_in(sock, 3, devid, 1024, [(0, 1024, 0, 0)])
    finally:
        sock.close()

    reply = import_with_retry(checker, "挂起传输随客户端断开而清理")
    if reply is None:
        return
    sock = reply["sock"]
    devid = reply["devid"]
    try:
        checker.check(True, "挂起传输随客户端断开而清理")
        ok, detail = read_device_descriptor(sock, 1, devid)
        checker.check(ok, "清理后新会话收发正常", detail)
    finally:
        sock.close()


def main():
    checker = Checker()
    try:
        run_negative_tests(checker)
    except (ConnectionError, RuntimeError, socket.timeout, OSError) as error:
        checker.check(False, "协议流程异常", str(error))
    return checker.summary()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="libusb_server 负路径测试")
    parser.add_argument("--server-port", type=int, required=True, help="USB/IP 服务器端口")
    parser.add_argument("--busid", required=True, help="服务器上设备的 busid")
    parser.add_argument("--host", default="127.0.0.1", help="服务器地址（默认 127.0.0.1）")
    ARGS = parser.parse_args()
    sys.exit(main())
