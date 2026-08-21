协议解析部分的修改请务必慎重，这部分是调试很久的结果，其他架构部分可以修改。

改各种api时需要考虑这种嵌入式平台的实现难易度

这个项目的开源协议是lgpl，别搞混了

别主动提交git，我叫你提交再提交

成员变量末尾不要加"_"

push前先提交代码，push和提交请分别执行不要放在一起

有新版本tag的时候别忘记更新CMakeLists.txt里的版本号

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