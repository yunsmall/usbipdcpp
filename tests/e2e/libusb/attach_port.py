#!/usr/bin/env python3
"""把 USB/IP 服务器上的设备导入到指定的 vhci 端口。

usbip 命令行固定选第一个匹配速度的空闲端口，无法指定控制器；这里自己
完成 OP_REQ_IMPORT 握手，然后把 "port sockfd devid speed" 写进 attach
sysfs。端口号是全局编号：0-7 是控制器 0 的 hs，8-15 是控制器 0 的 ss，
16-23 是控制器 1 的 hs，以此类推（每控制器 16 个端口）。

把两个 usbip 导入设备放到不同控制器的 hs HCD 上，是为了避开内核
usb_set_interface() 持 bandwidth_mutex 发 SET_INTERFACE 造成的双跳死锁：
同一 usb_hcd 上的两个导入设备共用一把锁，跨控制器才是两把。
"""

import argparse
import sys

import usbip_raw


def main():
    parser = argparse.ArgumentParser(description="导入 USB/IP 设备到指定 vhci 端口")
    parser.add_argument("--server-port", type=int, required=True, help="USB/IP 服务器端口")
    parser.add_argument("--busid", required=True, help="服务器上设备的 busid")
    parser.add_argument("--vhci-port", type=int, required=True, help="目标 vhci 全局端口号")
    parser.add_argument("--host", default="127.0.0.1", help="服务器地址（默认 127.0.0.1）")
    parser.add_argument("--attach-path", default="/sys/devices/platform/vhci_hcd.0/attach",
                        help="vhci 的 attach sysfs 路径")
    args = parser.parse_args()

    reply = usbip_raw.connect_and_import(args.host, args.server_port, args.busid)
    sock = reply["sock"]
    try:
        print(f"导入应答: busid={reply['busid']} busnum={reply['busnum']} devnum={reply['devnum']} "
              f"speed={reply['speed']} devid=0x{reply['devid']:08x}", file=sys.stderr)

        # attach 里写的是本进程的 socket fd；内核 sockfd_lookup 会自己持有
        # 引用，写完关闭进程内 fd 不影响连接
        try:
            with open(args.attach_path, "w") as attach:
                attach.write(f"{args.vhci_port} {sock.fileno()} {reply['devid']} {reply['speed']}\n")
        except OSError as error:
            raise SystemExit(f"写 {args.attach_path} 失败（端口可能已被占用）：{error}")
    finally:
        sock.close()

    print(f"已导入到 vhci 端口 {args.vhci_port}")


if __name__ == "__main__":
    main()
