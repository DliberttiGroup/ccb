<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/device-setup.md. -->

# 通过 CCB 连接设备

`ccb install`、`ccb run` 和 `ccb logcat` 通过 `adb` 与设备通信。CCB 从不把 adb 暴露到公共互联网。从这台虚拟机到用户手机的支持路线是**通过私有 Tailscale 网络进行无线调试**。

## 为什么不用普通的互联网 adb

adb 没有身份验证：任何能到达 adb 端口的人都可以在设备上运行命令。绝不要通过 ngrok、Cloudflare 隧道或任何其他公共隧道转发 adb 端口，也绝不要在公共网络上运行 `adb tcpip`。

## 支持的路线：通过 Tailscale 无线调试

手机和这台虚拟机位于同一个私有 Tailscale tailnet 上。只有 tailnet 内的设备才能互相到达，因此 adb 流量留在私有 mesh 内。

手机上的一次性设置：

1. 启用开发者选项（设置 → 关于手机 → 点按版本号 7 次，如果尚未启用）。
2. 打开开发者选项 → **无线调试** → 开启。
3. 点按**使用配对码配对设备**。手机会显示一个 IP 地址、一个配对端口和一个 6 位代码。记下 IP：它是手机的 tailnet 地址（tailnet 名称 `pixel-7-pro-1.tail428c53.ts.net`）。
4. 在这台虚拟机上配对一次（配对端口仅用于配对）：`adb pair <手机 tailnet IP>:<配对端口>`，输入代码。
5. 然后连接（手机在无线调试下还会显示一个单独的连接端口）：`adb connect <手机 tailnet IP>:<连接端口>`。
6. 验证：`adb devices` 显示手机为 `device`。

之后 `ccb install`、`ccb run` 和 `ccb logcat` 都照常工作。配对在重连后仍然有效；如果手机重启或 tailnet 地址变化，重复步骤 3–5。

## 没有连接设备时 CCB 的行为

- `ccb install` 和 `ccb run` 会以 `CCB-E5001` 失败，并给出具体的下一步；它们绝不会在没有设备的情况下报告成功。
- `ccb logcat` 先检查 `adb devices`，失败时直接以 `CCB-E5001` 快速失败，而不是卡在 adb 的 "- waiting for device -" 上。
- `ccb install --dry-run` 和 `ccb run --dry-run` 会打印将要运行的确切 adb 命令，而不触碰设备。

## 说明

- 手机的 Termux sshd（走同一个 tailnet）是 shell 访问的独立通道；它不是 adb，不能让 `ccb install` 工作。
- `ccb install` 安装 `build/outputs/debug/app.apk` 中的 debug APK（release 用 `--variant release`）。`ccb run` 总是先构建，再安装，然后从 manifest 启动 MAIN/LAUNCHER activity。
