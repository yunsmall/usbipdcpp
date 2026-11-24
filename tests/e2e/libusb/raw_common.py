#!/usr/bin/env python3
"""e2e 线协议客户端的公共部分。

usbip_raw.py 只负责线格式；本模块放多个脚本共用的请求构造、断言计数器、
导入重试等流程。设备/类特有的 vendor 请求仍留在各自脚本里。
"""

import socket
import struct
import time

import usbip_raw as u

EPIPE = -32

GET_DESCRIPTOR = 0x06
REQUEST_DEVICE_TO_HOST = 0x80
REQUEST_HOST_TO_DEVICE = 0x00
REQUEST_STANDARD = 0x00
REQUEST_VENDOR = 0x40
RECIPIENT_DEVICE = 0x00
RECIPIENT_INTERFACE = 0x01
RECIPIENT_ENDPOINT = 0x02


class Checker:
    """测试用例计数器与汇总输出。"""

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
    """拼 USB SETUP 包（8 字节，小端）。"""
    return struct.pack("<BBHHH", bm_request_type, request, value, index, length)


def control_submit(sock, seqnum, devid, bm_request_type, request, value, index, length,
                   transfer_buffer_length=None):
    """只发控制传输请求，不等应答。"""
    direction = u.DIR_IN if bm_request_type & REQUEST_DEVICE_TO_HOST else u.DIR_OUT
    if transfer_buffer_length is None:
        transfer_buffer_length = length
    u.submit(sock, seqnum, devid, direction, 0,
             setup=setup_packet(bm_request_type, request, value, index, length),
             transfer_buffer_length=transfer_buffer_length)


def control(sock, seqnum, devid, bm_request_type, request, value, index, length):
    """发控制传输并读回 RET_SUBMIT。"""
    control_submit(sock, seqnum, devid, bm_request_type, request, value, index, length)
    return u.read_ret_submit(sock, u.DIR_IN if bm_request_type & REQUEST_DEVICE_TO_HOST
                             else u.DIR_OUT)


def read_device_descriptor(sock, seqnum, devid):
    """发 GET_DESCRIPTOR(Device) 并校验应答；用于证明会话仍然正常。"""
    ret = control(sock, seqnum, devid,
                  REQUEST_DEVICE_TO_HOST | REQUEST_STANDARD | RECIPIENT_DEVICE,
                  GET_DESCRIPTOR, 0x0100, 0, 18)
    ok = (ret["seqnum"] == seqnum and ret["status"] == 0 and
          ret["actual_length"] == 18 and len(ret["payload"]) == 18 and
          ret["payload"][0] == 18)
    detail = (f"seq={ret['seqnum']} status={ret['status']} "
              f"actual={ret['actual_length']}") if not ok else ""
    return ok, detail


def import_with_retry(host, port, busid, checker, name):
    """会话结束后服务端要清理完才把设备放回可用列表，留足重试窗口。"""
    for attempt in range(60):
        try:
            return u.connect_and_import(host, port, busid)
        except (ConnectionError, RuntimeError, socket.timeout) as error:
            if attempt == 59:
                checker.check(False, name, str(error))
                return None
            time.sleep(0.25)
    return None
