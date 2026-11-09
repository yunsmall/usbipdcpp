# Windows e2e tests

Import virtual devices on Windows and verify the data path. Windows USB driver
stacks (usbser/usbvideo/uaudio/usb8023) validate more strictly than Linux, so
enumeration success does not mean data works — each test goes as far as making
sure data actually flows and the content is correct.

## Prerequisites

- **Administrator privileges**: attach/detach writes the system device tree.
- `usbip.exe` in PATH (usbip-win2), `ffmpeg` in PATH (UVC/UAC tests),
  `python` + `numpy` (WAV spectrum analysis).
- `mock_*.exe` built under `build`.
- Port 53240 free (each test checks).

## Running

```powershell
# all
powershell -NoProfile -File tests\e2e\windows\run_all.ps1

# single test
powershell -NoProfile -File tests\e2e\windows\test_uac_speaker.ps1
```

Optional parameters: `-Port <port>`, `-BusId <busid>`, `-BuildDir <build dir>`.

Each test owns the full cycle:

1. start the mock server;
2. `usbip attach`;
3. wait for the PnP device (polling, not a fixed sleep);
4. verify enumeration / data path;
5. **`usbip detach` first and poll until the port really disappears, then stop
   the server.**

The last step is a hard requirement: dropping the socket without a detach leaves
the usbip-win2 driver in a bad state and can blue-screen the machine.
`Stop-ServerSafely` in `common.ps1` refuses to stop the server while the device
is still imported.

## Tests

| Script | Coverage |
|---|---|
| `test_cdc_acm.ps1` | COM port enumeration, serial echo |
| `test_hid.ps1` | keyboard / mouse / gamepad enumerate as the right HID class |
| `test_keyboard_led.ps1` | keyboard LED: enum only by default; `-TestInput` toggles Caps Lock to verify SET_REPORT(Output) (state restored afterwards) |
| `test_multi_interface_hid.ps1` | composite HID: mouse and keyboard in one configuration enumerate as separate child devices |
| `test_msc.ps1` | USB disk, 2 MiB capacity (read-only check, no writes) |
| `test_rndis.ps1` | Remote NDIS adapter Up, ping + TCP echo |
| `test_stress_rndis.ps1` | RNDIS network stress: loops 8 KB TCP echo for 60 s by default, checks no failures and process alive |
| `test_uac_mic.ps1` | microphone records 2 seconds, sane data size |
| `test_uac_speaker.ps1` | plays 440 Hz sine, FFT verifies the received peak |
| `test_uvc.ps1` | camera captures one JPEG frame |
| `test_reconnect.ps1` | reconnect: detach→attach and server restart→attach, serial echo verified each time |
| `test_repeat_import.ps1` | repeated import: a second client import must fail while in use; the first session is unaffected |
| `test_multi_devices.ps1` | multi-device: one server exports 3 devices imported together; detaching one leaves the others intact |

HID tests start mocks with `--no-demo`: the keyboard otherwise types `A` and the
mouse moves the cursor, and an automated test must never inject input into the
host.

The UAC speaker test does not stop at "bytes arrived": `uaudio.sys` may accept
data with a wrong format. The script plays a known tone, then runs
`analyze_wav.py` (numpy) over the received file and checks the peak per segment,
catching a locally-correct but globally-corrupted stream.

## MSC safety

Windows assigns a drive letter and mounts the volume automatically.
`test_msc.ps1` only reads capacity and serial, it never writes or mounts a file
system. Do not run `format` / `diskpart clean` on this device — locate it by
`SerialNumber -like 'SBIPDCPSN*'` and confirm the disk number before any manual
operation.

**Do not plug in, add or remove any disk while a test is running** (USB flash
drives, external disks, `diskpart` / storage-pool operations): drive letters and
`Get-Disk` disk numbers shift when disks are added or removed, so scripted or
manual operations that address a disk by number can hit the wrong disk. Stop the
test first.

## Logs

Server stdout/stderr, captured frames, recordings and received WAVs are under
`$env:TEMP\usbip_e2e_win\<test name>\`.
