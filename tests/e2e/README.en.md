# e2e End-to-End Tests

The test scripts in this directory run in WSL2 (Ubuntu): start a mock server →
attach with the local usbip client → verify the virtual device's behavior
(network / serial / block device / HID / audio / video).

> **Recommendation**: If possible, run these tests inside a VM (the physical
> Windows disks appear directly under /dev in WSL2 and can be damaged just the
> same), so that device enumeration, network changes, and block device
> operations do not disturb your daily environment on the physical machine.

## Dependencies

| Dependency | Purpose | Notes |
|---|---|---|
| WSL2 (Ubuntu) | Test environment | Scripts run inside WSL; invoke from Windows via `wsl -e bash` |
| Passwordless `sudo` | Required for usbip attach/detach | vhci writes to /sys as root; without passwordless sudo the scripts hang at password prompts |
| usbip client (`/usr/local/sbin/usbip`) | attach/list/detach | The kernel usbip userspace tool, supports `-t <port>`; **must run with sudo** |
| usbip-win2 vhci kernel module | USB bus emulation | Confirm with `lsmod \| grep vhci`; attach fails if not loaded |
| Build artifacts (`build_wsl2/`) | mock server binaries | Build first with `cmake --build build_wsl2 -j6` (built inside WSL, deps via vcpkg) |
| python3 + pyusb (`python3-usb` package) | cdc_acm / pipe data-plane tests | These two scripts fail without it |
| alsa-utils (aplay/arecord) | audio / speaker tests | Scripts SKIP or fail if missing |
| ffmpeg | uvc capture test | SKIP if missing |
| evtest | HID (keyboard/mouse/gamepad) tests | Event stream capture |
| iproute2 / util-linux / coreutils | ss, lsblk, blockdev, truncate, cmp, etc. | Present by default in WSL |

## Before Running

1. **Build**: `cd <project root> && cmake --build build_wsl2 -j6` (run inside
   WSL; build dir per CLAUDE.local.md; never use all cores, max `-j6`)
2. **Confirm the vhci module is loaded**: `lsmod | grep vhci` — attach fails
   immediately if not
3. **Confirm sudo works and is passwordless**: `sudo -n true` should return 0
4. **Clean environment** (the scripts do this automatically at startup, but
   check before running manually):
   - No leftover listener on port 53240 (`ss -tln | grep 53240`)
   - No leftover devices in `/dev/input/by-path/`, `/dev/ttyACM*`,
     `/proc/asound/cards`
5. **Disk safety**: Confirm the device names of physical disks attached to WSL
   (`lsblk`). The mock_msc's new disk is double-confirmed by the script, but
   human interference (see "Forbidden") can defeat that confirmation

## Running

```bash
# Single test
bash tests/e2e/test_msc.sh

# All tests (continues on failure, runs one by one)
bash tests/e2e/run_all.sh
```

- Each script has its own work directory (`/tmp/usbip_e2e/<test name>`), logs
  under `<workdir>/logs/`; servers run in background via `setsid` and are
  cleaned up automatically at script end
- Scripts assume access to port 53240 on localhost (override with the `PORT`
  environment variable)
- mock server binaries are looked up in `build_wsl2/` by default (override
  with `BUILD_DIR`, e.g.
  `BUILD_DIR=/path/to/build_wsl2_asan bash tests/e2e/test_msc.sh`)

## Forbidden During Tests

1. **Never mix querying and writing MSC devices in one command**: The U-disk
   script is strictly stepwise internally (confirm new disk → size check →
   write → read back). Do **not** manually run anything like
   `lsblk && dd if=/dev/sdX ...` — one command chaining a device query with a
   write — after attach. WSL has other physical disks attached; writing the
   wrong device destroys data
2. **Never use `pkill -f mock_xxx`**: `-f` matches the whole command line and
   will kill the shell running the tests (its command line contains
   `mock_xxx`). Use `pkill -x mock_xxx` (executable name; names over 15
   characters are truncated by the kernel — check the real name first with
   `ps -eo comm`)
3. **Never manually attach/detach other usbip devices mid-test**: This pollutes
   new-device discovery (block device comparison, dmesg delta, input device
   enumeration); the scripts may misidentify devices or time out
4. **Never occupy port 53240**: If another program holds the port, the server
   fails to start and attach connects to the *old* server (looks bizarre:
   attach succeeds but the device is stale)
5. **Never plug/unplug physical USB devices during tests**: A newly inserted
   physical disk / input device can be mistaken for a virtual one by the lsblk
   comparison / by-path enumeration — at best the test fails, at worst (a
   physical disk mistaken for the mock_msc disk) data is written to the wrong
   device
6. **Never manually kill a mock server mid-test**: The scripts manage server
   lifecycle; manual kills break their cleanup logic (detach, waiting for port
   release)

## Risks

- **Data corruption (highest risk)**: The MSC test writes to a block device.
  Script defenses: ① lsblk snapshot comparison before/after attach ② kernel
  dmesg device-name cross-check (both must agree) ③ `blockdev --getsize64`
  must equal the mock image size (2MiB = 2097152) — any mismatch **aborts
  immediately, no write is performed**. Violating the "Forbidden" rules
  bypasses these defenses
- **Leftover servers / stale sessions**: A mock process left over from a
  previous crash holds the port, so a new attach connects to the old device.
  The scripts handle this (sudo-kill the process on the port + wait for port
  release), but be careful when debugging manually
- **Bogus leftover tty nodes**: After detach, commands like `tee` can create
  `/dev/ttyACM0` as a regular file (not a character device); subsequent
  stty/read-write fail mysteriously. `wait_dev` only accepts character devices
  (`-c`) to avoid this; if stty reports "Inappropriate ioctl for device" during
  manual work, check `ls -la /dev/ttyACM*` to see whether it is a regular file
- **Serial canonical-mode trap**: Serial ports default to canonical mode
  (icanon), where reads return per *line* — with no newline, the reader never
  returns. The scripts use python to disable icanon/echo before reading and
  writing; when testing a serial port manually, first run
  `stty -F /dev/ttyACM0 -icanon -echo -icrnl -onlcr`
- **Sound card resources**: WSL2 has no physical sound card; audio/speaker
  depend on the USB sound card enumerated by vhci. The card number changes
  between consecutive tests; scripts fetch the current number live from
  `/proc/asound/cards`
- **ECM test changes networking**: test_ecm adds 192.168.53.2/24 to the enx
  interface and pings; the script cleans up the interface afterwards. Devices
  on the same subnet on the LAN could conflict during the run

## Per-Test Description

| Script | Device | Verification |
|---|---|---|
| test_cdc_acm.sh | Virtual serial port | python(termios) single-fd write/read, echo matches exactly |
| test_ecm.sh | Virtual NIC | enx interface + ping 192.168.53.1 |
| test_msc.sh | USB flash drive | lsblk+dmesg dual confirmation of new disk → size check → sector-100 write/read cmp |
| test_keyboard.sh | HID keyboard | evtest captures KEY_A events |
| test_mouse.sh | HID mouse | evtest captures REL motion events |
| test_gamepad.sh | HID gamepad | evtest captures gamepad events |
| test_cdc_throttle.sh | Serial (throttled) | ttyACM count report |
| test_pipe.sh | vendor pipe | pyusb bulk OUT→IN echo + periodic-send logs |
| test_audio.sh | UAC microphone | arecord actually records for 2 s |
| test_speaker.sh | UAC speaker | aplay plays a 440 Hz sine; received WAV compared at content level (frequency / extrema count) |
| test_uvc.sh | UVC camera | ffmpeg actually captures 5 frames |

Python helper scripts: `cdc_acm_test.py` (serial read/write),
`pipe_test.py` (libusb transfer), `speaker_verify.py` (audio content
comparison).
