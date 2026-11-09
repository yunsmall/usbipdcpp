# e2e Tests

End-to-end tests are split by platform:

| Directory | Platform | Description |
|---|---|---|
| `linux/` | WSL2 / Linux | Start a mock server → attach with the local usbip client → verify the device data plane; see `linux/README.md` |
| `windows/` | Windows | Import devices with usbip-win2 under an administrator account and verify the data plane; see `windows/README.md` |
| `libusb/` | WSL2 / Linux | libusb backend e2e (FunctionFS devices + virtual iso device), run manually and not registered in ctest; see `libusb/README.md` |

The three sets are independent and each has its own `run_all` entry point.
