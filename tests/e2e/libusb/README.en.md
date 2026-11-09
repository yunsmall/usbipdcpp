# libusb backend real-device e2e (WSL2, no Windows)

This procedure uses the kernel's `dummy_hcd` plus configfs to create a "real USB device", lets the
libusb backend access it exactly like physical hardware, and imports it from another WSL distro
over USB/IP for read/write verification. The Windows usbip-win2 client is not involved.

## Topology

```
Ubuntu 24.04 (exporter)                        Kali (importer)
┌──────────────────────────────┐              ┌──────────────────────────────┐
│ dummy_hcd + libcomposite     │              │ vhci_hcd                     │
│   gadget: mass_storage       │              │   usbip client               │
│   host side enumerates /dev/sdg             │   usbcore re-enumerates      │
│                              │              │                              │
│ libusb_server -d -p 53241 ───┼── TCP ──────>│ usbip --tcp-port 53241        │
│   (opens /dev/bus/usb/...)   │  127.0.0.1   │   attach -r 127.0.0.1 -b 3-1 │
└──────────────────────────────┘              └──────────────────────────────┘
```

Key premise: **all WSL2 distros run in the same lightweight VM and share one kernel instance**,
including the network namespace and kernel objects (`/sys`, `/dev`, `dmesg`, modules). Therefore:

- Loading `dummy_hcd` or creating a gadget in one distro is immediately visible in another
- A server listening on `127.0.0.1` in one distro is reachable from another
  (`hostname -I` is identical)
- This validates the libusb backend + USB/IP + vhci path; it does **not** validate different
  kernel versions

## Automated cases (FunctionFS device + libusb client)

`tests/e2e/libusb/` contains a repeatable e2e suite: the control/bulk/interrupt device side
is implemented with FunctionFS (configfs `functions/ffs.*`) and the client is a libusb
program; iso uses the project's virtual isochronous device over a two-hop chain because
dummy_hcd does not support iso. **These cases are run manually and are not registered in
ctest.**

```bash
# Run inside WSL2 Ubuntu as root (gadget creation, usbfs access, vhci attach all need it)
cd <project root>
cmake --build <build dir> -j6            # build the e2e targets first
sudo tests/e2e/libusb/run_one.sh bulk   # one case
sudo tests/e2e/libusb/run_all.sh        # run all five in order
```

`run_one.sh --help` / `run_all.sh --help` list the available cases, prerequisites and
environment variables (`BUILD_DIR`, `USBIP`).

Chain for each case:

```
e2e_ffs_<profile> (device daemon)
  └─ configfs gadget ffs.<name> → dummy_hcd's dummy_udc.0
       └─ host side of the same kernel enumerates /dev/bus/usb/... → libusb_server (interactive
          mode, `b <busid>` over a FIFO)
            └─ USB/IP (127.0.0.1:5326x) → vhci_hcd attach
                 └─ e2e_libusb_<profile>_client (libusb client)
```

Case breakdown (one device per client):

| Case | VID:PID | Device side | Client coverage |
|---|---|---|---|
| control | 1d6b:0104 | interface 0 with alt 0/alt 1 (no endpoints); vendor requests for IN/OUT/STALL/short/zero/delay | 35 checks: control IN/OUT of many lengths, STALL, short packets, zero-length packets, async cancel, standard descriptor reads, GET/SET_CONFIGURATION, GET/SET_INTERFACE alt switching |
| bulk | 1d6b:0105 | interface 0 alt 0 (0x81/0x02), alt 1 (0x83/0x04); interface 1 alt 0 (no endpoints) / alt 1 (0x85/0x06) | 45 checks: IN lengths (multiples of 512 up to 1 MiB), zero-length packets, OUT lengths, 16 concurrent async IN, cancel, timeout, IN/OUT halt + clear_halt recovery, 10 rounds of interface/alt switching (each round transfers on the endpoints of the active alt) |
| interrupt | 1d6b:0106 | one interface, interrupt IN 0x81 / OUT 0x02; IN keeps emitting 8-byte counter blocks | 18 checks: lengths, counter continuity across requests, concurrency, cancel, timeout, IN/OUT halt + recovery |
| iso | 1d6b:0107 | project virtual isochronous device (no gadget): alt 0 has no endpoints, alt 1 has 1024B iso IN/OUT, alt 2 has 512B iso IN; vendor requests expose statistics, an IN delay and a failure mask | 38 checks: descriptor layout, alt 0/1/2 switching, IN with 8/32/64 packets plus variable/zero-length packets, 4 concurrent transfers, OUT with three lengths plus device-side byte verification, vendor requests, nonexistent alt rejected, five alt 1↔0 rounds, submit on alt 0 rejected, UNLINK while pending, partial packet failure (per-packet status + device stats) |
| raw | 1d6b:0107 | shares the virtual device with iso; no second vhci, talks the USB/IP wire protocol to libusb_server directly | 19 checks: endpoint missing in the active alt is silently dropped, wLength greater than transfer_buffer_length, CLEAR_FEATURE fallback submit, duplicate seqnum, ISO number_of_packets=0, invalid ISO descriptor disconnects; after every error the session/device must recover, and a pending transfer must be cleaned up when the client drops the socket |

The client never assumes endpoint addresses; it looks them up per interface/alt from the
enumerated configuration descriptor, because UDC autoconfig may rewrite them (e.g. 0x83 becomes
0x86).

`run_one.sh` performs the whole setup and teardown: configfs gadget, functionfs mount, waiting
for the device daemon, starting `libusb_server` in interactive mode (so `-d`'s auto-bind cannot
re-export the vhci-imported device), attach, run the client, and cleanup in a trap (detach, quit
server, tear down gadget).

### iso case (virtual isochronous device, two hops)

dummy_hcd does not support iso, so the iso case does not use a gadget:

```
e2e_vdev_iso (USB/IP server, project virtual device)
  └─ imported by vhci controller 0 (port 0) → libusb_server opens it with libusb and re-exports
       └─ imported by vhci controller 1 (port 16) → e2e_libusb_iso_client
```

**The two hops must land on different vhci controllers.** The kernel's
`usb_set_interface()` (`drivers/usb/core/message.c`) sends SET_INTERFACE synchronously while
holding `hcd->bandwidth_mutex`. Two imported devices on the same `usb_hcd` share that mutex:
hop2's SET_INTERFACE waits for the one libusb_server forwards to hop1, which needs the same
mutex, giving an ABBA deadlock (the client hangs in `usb_kill_urb`, the whole vhci device is
lost, and only restarting the WSL VM recovers). With two controllers there are two mutexes,
so alt switching can be tested.

The kernel needs `CONFIG_USBIP_VHCI_NR_HCS>=2` (this case uses controllers 0 and 1) and
`CONFIG_USBIP_VHCI_HC_PORTS=8`. Port numbers are global: 0–7 are controller 0's hs ports,
8–15 controller 0's ss ports, 16–23 controller 1's hs ports, and so on.

`usbip attach` has no option to pick a controller or port
(`usbip_vhci_get_free_port()` always returns the first free matching-speed port, i.e.
controller 0), so hop2 uses `attach_port.py`: it performs the `OP_REQ_IMPORT` handshake and
writes `"16 <sockfd> <devid> <speed>"` to `/sys/devices/platform/vhci_hcd.0/attach`. hop1
still uses the usbip command line.

On a kernel built with a single controller this case exits with a clear error (`run_one.sh`
checks the controller count).

### raw case (negative paths over the USB/IP wire protocol)

Requests that a libusb client rejects locally (an endpoint missing in the active alt,
`wLength` larger than the buffer, `number_of_packets=0`, an invalid ISO descriptor) never
reach the server, so the server's own validation, response and cleanup paths stay
uncovered. `raw_client.py` uses `usbip_raw.py` to talk to `libusb_server` directly and
asserts:

- an endpoint missing in the active alt is silently dropped (no response, matching the
  kernel stub), and following requests still work
- `wLength > transfer_buffer_length` returns EPIPE without disconnecting (prevents libusb
  from writing past the buffer)
- CLEAR_FEATURE for an isochronous endpoint present in the active alt: `libusb_clear_halt`
  fails, the server falls back to a plain control transfer, the device answers Request Error
  and the session survives (an entirely nonexistent endpoint cannot be used: usbfs
  `check_ctrlrecip()` validates wIndex against the descriptors at submit time and returns
  ENOENT)
- a duplicate seqnum returns EPIPE while the original transfer still completes
- ISO `number_of_packets=0` disconnects the session per policy: under the strict policy it
  drops the connection like the kernel's `SDEV_EVENT_ERROR_SUBMIT` and does not guarantee a
  RET_SUBMIT; the device returns to the available list and can be imported again
- an invalid ISO descriptor (length sum exceeds the buffer) disconnects the session
  instead of reading/writing out of bounds
- if the client drops the socket with a transfer still pending (no detach, no UNLINK), the
  server cancels the transfer, returns the device to the available list, and the device can
  be imported again and used normally

It only needs the virtual device, not a second vhci controller.

## Prerequisites

- WSL2 kernel with `dummy_hcd`, `libcomposite`, `usb_f_fs`, `vhci_hcd` (the automated cases
  need `usb_f_fs`; the manual mass-storage example additionally needs `usb_f_mass_storage`)
- the iso case additionally needs `CONFIG_USBIP_VHCI_NR_HCS>=2` (see "iso case")
- `/sys/kernel/config` mounted (configfs)
- `libusb_server` on the exporter (normally `<build dir>/libusb_server`)
- usbip client on the importer (Kali: `apt install usbip`, port option is `--tcp-port`)
- root privileges (gadget creation, libusb device access, vhci attach/detach)

## Steps

### 1. Create the gadget on the exporter (root)

```bash
modprobe dummy_hcd
modprobe libcomposite

cd /sys/kernel/config/usb_gadget
mkdir g1
cd g1
echo 0x1d6b > idVendor
echo 0x0104 > idProduct
mkdir -p strings/0x409
echo "usbipdcpp-gadget" > strings/0x409/serialnumber
echo "usbipdcpp" > strings/0x409/manufacturer
echo "Gadget MSC" > strings/0x409/product

mkdir -p functions/mass_storage.0
truncate -s 2M /tmp/gadget_msc.img
echo /tmp/gadget_msc.img > functions/mass_storage.0/lun.0/file

mkdir -p configs/c.1
ln -sf functions/mass_storage.0 configs/c.1/
echo dummy_udc.0 > UDC
```

`dummy_hcd` provides both a virtual UDC (device side) and a virtual host controller (host side),
so once the UDC is bound the host side of the same kernel enumerates the device and `usb-storage`
binds it as `/dev/sdX`. No C code is needed: configfs file operations are the kernel's API.

### 2. Verify host-side enumeration

```bash
lsusb | grep 1d6b:0104
lsblk -o NAME,SIZE,MODEL,TRAN | grep -i gadget
```

### 3. Start the libusb server on the exporter

```bash
cd <project root>
setsid ./build/libusb_server -d -p 53241 < /dev/null \
    > tmp/libusb_server_gadget.log 2>&1 &
```

`-d` is daemon mode: bind all non-hub devices and wait for a signal. The log should show the
gadget's busid (e.g. `3-1`) added to the available list.

### 4. List and attach on the importer

```bash
usbip --tcp-port 53241 list -r 127.0.0.1
usbip --tcp-port 53241 attach -r 127.0.0.1 -b 3-1
usbip port
```

After a successful attach, `usbip port` shows `High Speed(480Mbps)` and
`Multifunction Composite Gadget`.

### 5. Verify identity and perform I/O on the importer

Check identity and size before writing. The example writes sector 100 of the 2 MiB disposable
image and reads it back:

```bash
dev=sdg
model=$(lsblk -no MODEL /dev/$dev | tr -d ' ')
size=$(blockdev --getsize64 /dev/$dev)
[ "$size" = "2097152" ] || { echo ABORT_SIZE; exit 1; }
case "$model" in *File-Stor*Gadget*) ;; *) echo ABORT_MODEL; exit 1 ;; esac

head -c 512 /dev/urandom > /tmp/payload.bin
dd if=/tmp/payload.bin of=/dev/$dev bs=512 seek=100 conv=fsync status=none
dd if=/dev/$dev of=/tmp/back.bin bs=512 skip=100 count=1 status=none
cmp /tmp/payload.bin /tmp/back.bin && echo IO_OK
```

### 6. Cleanup (order matters)

```bash
# importer: detach first, then stop the server
usbip detach -p 0

# exporter: stop the server gracefully
kill $(pgrep -x libusb_server)

# exporter: tear down the gadget
cd /sys/kernel/config/usb_gadget/g1
echo '' > UDC
rm -f configs/c.1/mass_storage.0
rmdir configs/c.1 strings/0x409 functions/mass_storage.0
cd .. && rmdir g1
rm -f /tmp/gadget_msc.img
```

## Safety notes

- Only use a disposable image under `/tmp`; never back the gadget with a physical disk
- Always verify `size` and `model` before writing; abort on mismatch
- **Do not plug in, add or remove any disk while an e2e case is running** (including
  mounting a new disk inside WSL or changing mount points): `/dev/sdX` names shift when
  disks are added or removed, so a write could target the wrong disk. Stop the test first
- On attach, the server detaches the host-side kernel driver (e.g. `usb-storage`), so the
  exporter's original `/dev/sdX` disappears; the imported device then takes the same free sd
  name. The `Synchronizing SCSI cache` and re-enumeration entries in `dmesg` are expected
- Both distros share the same kernel: `/sys`, `/dev`, `dmesg` changes are visible on both sides

## Known observations

- During vhci enumeration the server may log a few `error on endpoint 129` (0x81 Bulk IN)
  messages. They belong to the Bulk-Only short-packet path; enumeration and I/O still work.
  Investigate only if real I/O failures appear
- Port option differs by usbip build: distro packages (e.g. Kali) use `--tcp-port`, a
  self-built newer usbip uses `-t`

### dummy_hcd behavioural limits (important when writing cases)

- **No iso support**: the ISO entries in `ep_info[]` are commented out; ISO URBs fail with
  `-EINVAL`
- **Non-multiple bulk IN short reads**: when the host asks for a length that is not a multiple
  of maxpacket while the gadget still has more queued data, dummy_hcd returns `-EOVERFLOW`
  (mapped to PIPE by the server). The bulk case therefore only reads multiples of 512 from the
  full-speed stream and uses a dedicated short-packet endpoint for the short-packet behaviour
- **OUT request length must stay within one maxpacket**: if a gadget-side `read` asks for more,
  dummy_hcd accumulates several host URBs into one request, so the read returns late and both
  statistics and flow control lag
- **UDC autoconfig rewrites endpoint addresses**: the device must use the address reported by
  `FUNCTIONFS_ENDPOINT_DESC`
- **usbfs control transfers are capped at 4096 bytes** (`MAX_CTRL_BUFFER_LENGTH`); larger ones
  return `LIBUSB_ERROR_INVALID_PARAM`
- **SET_FEATURE/CLEAR_FEATURE endpoint halt is handled inside dummy_hcd**, so the FunctionFS
  userspace never sees those requests (a real UDC forwards them; the device code keeps the
  handler for that case)
- **Switching an alt setting disables and re-enables every endpoint of the function** and emits
  a single FUNCTIONFS_ENABLE event; disabling cancels blocked requests with `ESHUTDOWN`, so
  device workers must retry until the endpoints are enabled again

## Scope

Covers libusb backend open/claim, control/bulk/interrupt transfers against a real USB device,
iso transfers (through the virtual-device two-hop chain), USB/IP export, vhci import,
interface/alt-setting switching and block-device I/O. Does not cover: iso on dummy_hcd (the
kernel gadget does not support it; iso is covered by the virtual device), kernel-version
differences (all WSL2 distros share one kernel), the Windows client, or hotplug.
