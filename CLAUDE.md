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

## mock 设备本地验证的坑

在 WSL 里起 mock 服务器并 attach 验证时，两个坑容易踩到：

- **mock 服务器依赖 `std::cin.get()` 等回车**：直接 `< /dev/null` 会让
  stdin 立即 EOF，server 秒退。必须用管道把 stdin 挂住才能常驻，例如
  `setsid nohup bash -c 'sleep infinity | ./mock_xxx -p 53240' < /dev/null > log 2>&1 &`
- **清理旧进程时禁用 `pkill -f mock_xxx`**：`-f` 按整条命令行匹配，
  会把自己的 shell（bash -lc 命令串里含 "mock_xxx"）杀掉。改用精确
  进程名 `pkill -x mock_xxx`（可执行名，不含路径/参数）