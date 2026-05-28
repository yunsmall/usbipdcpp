---
name: vcpkg-ci-debug
description: 从 Azure DevOps 获取 vcpkg CI 构建失败日志
disable-model-invocation: true
---

# 获取 vcpkg CI 编译报错

## 1. 从 PR checks 获取 buildId 和 project GUID

```bash
gh pr checks <PR_NUMBER> -R microsoft/vcpkg
```

输出中的 URL 形如 `https://dev.azure.com/vcpkg/<GUID>/_build/results?buildId=<ID>`

## 2. 列出构建产物

```bash
gh api "https://dev.azure.com/vcpkg/<GUID>/_apis/build/builds/<BUILD_ID>/artifacts"
```

找到 `failure logs for <platform>` 的 `downloadUrl`。

## 3. 下载

```bash
curl -L -o /tmp/failure_logs.zip "<downloadUrl>"
unzip -q /tmp/failure_logs.zip -d /tmp/failure_logs
```

## 4. 读报错

`issue_body.md` 的 `<details>` 折叠块里有完整编译输出，包含文件名、行号、错误内容。
