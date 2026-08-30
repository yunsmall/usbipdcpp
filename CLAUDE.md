协议解析部分的修改请务必慎重，这部分是调试很久的结果，其他架构部分可以修改。

改各种api时需要考虑这种嵌入式平台的实现难易度

这个项目的开源协议是lgpl，别搞混了

别主动提交git，我叫你提交再提交

成员变量末尾不要加"_"

push前先提交代码，push和提交请分别执行不要放在一起

有新版本tag的时候别忘记更新CMakeLists.txt里的版本号

所有编译出来的二进制在编译目录根目录，别搞错了

## git 工作流

单 main 分支模型（develop 已删除）：

- 日常提交直接进 main
- 大的实验性功能：开临时 feature 分支开发，完成后合并回 main 并删除
- 外部贡献者：fork + PR 到 main
- 发版本：先更新 CMakeLists.txt 里的版本号并提交推送，再打 v* tag 推送，CI 自动构建发布包

## usbip命令使用

假设有个usbip服务器在本机的53240端口监听，请你attach或者list时使用
`usbip -t 53240 attach/list ...`这种命令，注意-t必须紧跟usbip的后面，
处于attach list等所有子命令的前面

**usbip 的 attach 必须用 sudo**：attach 要写 /sys 并移交 fd 给 vhci_hcd，
WSL 默认用户非 root 时权限不足会报 `usbip: error: import device`（极易误判成
服务器/协议问题）。list 只读不需要 sudo，但为统一可直接
`sudo usbip -t 53240 ...`。验证导入状态用 `sudo usbip port`。

## mock 设备本地验证的坑

在 WSL 里起 mock 服务器并 attach 验证时，注意：

- **启动**：mock 服务器（wait_for_exit）等退出信号（POSIX：SIGINT/SIGTERM；
  Windows：控制台事件），后台运行不需要挂 stdin。直接
  `setsid ./mock_xxx -p 53240 < /dev/null > log 2>&1 &`
- **关闭**：`kill -TERM <pid>` 或 `pkill -x mock_xxx`（都是 SIGTERM），
  服务器走正常清理路径（server.stop：关连接、释放端口）优雅退出。
  退出后 `ss -tln | grep 53240` 确认端口释放（端口被占会导致下一个
  服务器起不来，误 attach 到旧设备上）
- **清理旧进程时禁用 `pkill -f mock_xxx`**：`-f` 按整条命令行匹配，
  会把自己的 shell（bash -lc 命令串里含 "mock_xxx"）杀掉。改用精确
  进程名 `pkill -x mock_xxx`（可执行名，不含路径/参数）
- **进程名超 15 字符会被内核截断**（Linux comm 上限）：`pkill -x` 匹配的
  是截断后的名字，如 `mock_cdc_throttle`→`mock_cdc_thrott`、
  `multi_interface_hid`→`multi_interface`、`multi_devices` 恰好 13 字符不截断。
  杀不掉时先 `ps -eo comm | grep -i mock` 看真实 comm 名再用 `pkill -x <真实名>`

**MSC（U盘）块设备读写必须分步，禁止一条命令里混查询和写**：
attach 后先单独确认新出现的块设备是哪个（如 attach 前后 `lsblk` 对比、
`dmesg | tail` 看内核枚举的 sdX 名），确认无误后**再单独执行**读写命令。
严禁 `lsblk ... && dd if=/dev/sdX ...` 这类"查设备 + 写设备"一条命令
串联——WSL 里还有其他物理硬盘，写错盘符会毁数据