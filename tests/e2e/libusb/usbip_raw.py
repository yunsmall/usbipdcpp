#!/usr/bin/env python3
"""USB/IP 线协议的最小实现（e2e 负路径测试用）。

只实现 e2e 需要的部分：OP_REQ_IMPORT 握手、CMD_SUBMIT、RET_SUBMIT 与数据/
iso 描述符读取。不走内核 vhci，因此可以构造 libusb 客户端在本地就拒绝的
非法请求，验证服务端自己的校验与应答路径。

线格式（USB/IP 1.1.1）：
- CMD_SUBMIT: 48 字节头（command + 基本字段 + 传输参数 + setup），
  OUT 方向后跟 transfer_buffer_length 字节数据，ISO 再跟每包 16 字节描述符
- RET_SUBMIT: 48 字节（40 字节头 + 8 字节 padding），IN 方向后跟
  actual_length 字节数据，ISO 再跟每包 16 字节描述符
"""

import socket
import struct

USBIP_VERSION = 0x0111
OP_REQ_IMPORT = 0x8003
OP_REP_IMPORT = 0x0003
CMD_SUBMIT = 0x00000001
RET_SUBMIT = 0x00000003

DIR_OUT = 0
DIR_IN = 1

# path[256] + busid[32] + busnum/devnum/speed（各 4 字节）+ 三个 u16 版本/ID
# + 6 个单字节字段
DEVICE_DESC_SIZE = 312


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("连接被对端关闭")
        data += chunk
    return bytes(data)


def connect_and_import(host, port, busid, timeout=10.0):
    """连接服务器并完成 OP_REQ_IMPORT 握手。

    @return 字典：sock、busid、busnum、devnum、speed、devid
    """
    sock = socket.create_connection((host, port), timeout=timeout)
    busid_bytes = busid.encode("ascii")
    if len(busid_bytes) >= 32:
        sock.close()
        raise ValueError("busid 超过 31 字节")
    sock.sendall(struct.pack("!HHI", USBIP_VERSION, OP_REQ_IMPORT, 0) + busid_bytes.ljust(32, b"\0"))
    _version, code, status = struct.unpack("!HHI", recv_exact(sock, 8))
    if code != OP_REP_IMPORT:
        sock.close()
        raise RuntimeError(f"应答命令码不对：0x{code:04x}")
    if status != 0:
        sock.close()
        raise RuntimeError(f"服务器拒绝导入，status={status}")
    desc = recv_exact(sock, DEVICE_DESC_SIZE)
    busnum, devnum, speed = struct.unpack_from("!III", desc, 288)
    return {
        "sock": sock,
        "busid": desc[256:288].split(b"\0", 1)[0].decode("ascii", "replace"),
        "busnum": busnum,
        "devnum": devnum,
        "speed": speed,
        "devid": (busnum << 16) | devnum,
    }


def submit(sock, seqnum, devid, direction, ep, setup=b"\0" * 8, transfer_buffer_length=0,
           number_of_packets=0, transfer_flags=0, start_frame=0, interval=0,
           out_data=b"", iso_descriptors=()):
    """发一条 CMD_SUBMIT。

    iso_descriptors 是 (offset, length, actual_length, status) 的序列，
    按协议在网络序下发；IN 方向不发数据段（由内核约定），OUT 方向发送
    transfer_buffer_length 字节。
    """
    header = struct.pack("!IIIII", CMD_SUBMIT, seqnum, devid, direction, ep)
    header += struct.pack("!IIIII", transfer_flags, transfer_buffer_length, start_frame,
                          number_of_packets, interval)
    header += setup
    sock.sendall(header)
    if direction == DIR_OUT and transfer_buffer_length:
        data = out_data
        if len(data) < transfer_buffer_length:
            data += b"\0" * (transfer_buffer_length - len(data))
        sock.sendall(data[:transfer_buffer_length])
    for offset, length, actual_length, status in iso_descriptors:
        sock.sendall(struct.pack("!IIII", offset, length, actual_length, status))


def read_ret_submit(sock, direction):
    """读一条 RET_SUBMIT（含数据段与 iso 描述符），返回字典。

    RET_SUBMIT 在线上是 48 字节（40 字节头 + 8 字节 padding，与内核
    usbip_header 的 union 大小一致），头里的方向字段被服务端清零，因此
    数据段是否存在只能由调用方按自己 CMD_SUBMIT 的方向判断——传进来的
    direction 就是该方向。
    """
    header = recv_exact(sock, 48)
    command, seqnum, devid, _ret_direction, ep = struct.unpack("!IIIII", header[:20])
    if command != RET_SUBMIT:
        raise RuntimeError(f"应答命令码不对：0x{command:08x}（seq={seqnum}）")
    status, actual_length, start_frame, number_of_packets, error_count = struct.unpack(
        "!iIIII", header[20:40])
    payload = b""
    if direction == DIR_IN and actual_length > 0:
        payload = recv_exact(sock, actual_length)
    iso = [struct.unpack("!IIII", recv_exact(sock, 16)) for _ in range(number_of_packets)]
    return {
        "seqnum": seqnum,
        "devid": devid,
        "direction": direction,
        "ep": ep,
        "status": status,
        "actual_length": actual_length,
        "start_frame": start_frame,
        "number_of_packets": number_of_packets,
        "error_count": error_count,
        "payload": payload,
        "iso": iso,
    }
