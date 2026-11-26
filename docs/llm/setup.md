<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/setup.md. -->

# 工具链设置

[文档索引](index.md)

子页面：[安装工具](setup-tools.md)（JDK、kotlinc、NDK、r8、主机工具、自定义路径）

## CCB 需要什么

| 工具 | 版本 |
| ---- | ------- |
| JDK（`java`、`javac`、`keytool`） | 17+ |
| Android SDK | `platforms;android-34`、`-35` 或 `-36`；`build-tools;35.x` 或 `36.x`；`platform-tools`（用于 `adb`） |
| kotlinc | 2.x |
| NDK | r27，仅用于 `[ndk]` 项目 |
| r8 | 仅用于 release + minify（[setup-tools.md](setup-tools.md#r8)） |

## 解析顺序

- SDK home：`[sdk] home`，然后是 `ANDROID_HOME`，然后是 `ANDROID_SDK_ROOT`，然后是 `~/Android/Sdk`。
- Kotlin home：`[sdk] kotlin_home`，然后是 `KOTLIN_HOME`，然后是 PATH 上的 `kotlinc`，然后是 `<sdk-home>/kotlinc`（setup 安装的编译器，最后手段）。发行版根目录（`…/kotlinc`）及其父目录都被接受：如果 `$KOTLIN_HOME/bin/kotlinc` 缺失，但 `$KOTLIN_HOME/kotlinc/bin/kotlinc` 存在，则使用嵌套布局。
- Java：`$JAVA_HOME/bin/java`，然后是 PATH 上的 `java`。

`ccb check` 和 `ccb doctor` 会准确报告缺失的内容。

## `ccb setup`

通过 `sdkmanager` 为当前项目安装缺失的组件（或默认值：platform android-35、build-tools 35.0.0）。当 platform、build-tools 和 platform-tools 都已存在时，setup 会完全跳过许可证和 sdkmanager，直接进入其余步骤——因此即使在代理屏蔽 sdkmanager 的情况下也能工作，只要 SDK 已经配置好。找到但加载失败的配置是错误，而不是静默回退到默认 SDK home；默认值仅在完全没有 `ccb.toml` 时适用。

| # | 步骤 |
| - | ---- |
| 1 | 缺失 JDK 17+ 时，主动提出安装一个（见 [setup-tools.md](setup-tools.md)），而不是在 sdkmanager 步骤以通用错误失败。 |
| 2 | 在没有 cmdline-tools 时，从 Google 官方存档（`commandlinetools-linux-11076708_latest.zip`）引导安装，并在解压前用固定的 SHA-256 校验。 |
| 3 | 接受 SDK 许可证。 |
| 4 | 安装组件。 |
| 5 | 在继续之前验证组件确实已安装——静默地什么都没安装的安装操作（例如全新 SDK 上许可证尚未接受）会被报告为组件缺失，绝不会报告为成功。 |
| 6 | 在没有可用的 2.x 可以解析时安装 kotlinc。 |
| 7 | 在项目声明 `[ndk]` 时安装 NDK 修订版。 |
| 8 | 在项目启用 minify 但没有显式 jar 时安装 r8。 |
| 9 | 从平台包管理器安装缺失的主机工具。 |
| 10 | 把 `[deps] maven` 产物预取到本地 Maven 缓存中（见 [maven.md](maven.md)）。 |
| 11 | 最终验证（见下文）。 |
| 12 | 打印 "SDK ready"。 |

## 最终验证

在每个安装步骤之后，setup 会解析工具链（加上 adb），并以警告形式报告仍需注意的事项——例如 PATH 上的旧 kotlinc 遮蔽了 SDK-home 的安装，或某个主机工具没有软件包修复办法。警告不会使 setup 失败：setup 已经做了能做的事，剩余事项需要用户操作。当一切都解析成功时，setup 会打印 `toolchain check: ok`。“SDK ready”表示 SDK 组件已就绪；最终验证才是让它意味着工具链可用的部分。

## `--upgrade` 与 `--print-env`

- `--upgrade`：当 setup 管理的工具（SDK-home kotlinc、SDK-home r8）与固定版本不同时重新安装它们，并对 JDK 和主机工具提示隐含 `--yes`。它永远不会触碰用户管理的工具，也永远不会提升配置固定的 SDK 版本。
- `--print-env`：在 setup 成功后，把已配置工具链的 shell 导出打印到 stdout，而不是打印 "SDK ready" 行（该行移到 stderr，因此 stdout 保持可安全 eval）：`ANDROID_HOME`、`JAVA_HOME`（有效值，未设置时推导得出），以及 SDK-home kotlinc bin 存在时的 `PATH` 前置。不打印任何 `CCB_*` 覆盖：借助 PATH 回退，CCB 无需它们就能在 PATH 上找到 Termux 工具。

## 已知限制：需要认证的出口代理

`sdkmanager` 没有代理认证的标志，它的 Java HTTP 栈在面对需要认证的出口代理时会失败。在这种环境中，`ccb setup` 会在 sdkmanager 步骤以仓库错误失败；此时必须通过直接下载来配置 SDK（curl 能很好地处理代理认证）。缺失的 JDK 会在 sdkmanager 运行之前单独报告，因此代理说明不会掩盖它。
