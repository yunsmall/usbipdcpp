# e2e 测试

端到端测试按平台分目录：

| 目录 | 平台 | 说明 |
|---|---|---|
| `linux/` | WSL2 / Linux | 启动 mock 服务器 → 本机 usbip 客户端 attach → 验证设备数据面；用法见 `linux/README.md` |
| `windows/` | Windows | 管理员权限下用 usbip-win2 真正导入设备并验证数据面；用法见 `windows/README.md` |
| `libusb/` | WSL2 / Linux | libusb 后端专项 e2e（FunctionFS 设备 + 虚拟 iso 设备），手动运行、不注册 ctest；用法见 `libusb/README.md` |

三套脚本互不依赖，各自有 `run_all` 入口。
