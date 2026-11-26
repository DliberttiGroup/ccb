<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/setup.md. -->

# 工具链设置

## CCB 需要什么

- JDK 17+（`java`、`javac`、`keytool`）
- Android SDK：`platforms;android-34`、`android-35` 或 `android-36`，`build-tools;35.x` 或 `36.x`，`platform-tools`（用于 `adb`）
- kotlinc 2.x
- NDK r27（仅用于 `[ndk]` 项目）
- r8（仅用于 release + minify——见下文 "r8"）

## 解析顺序

- SDK home：`[sdk] home`，然后是 `ANDROID_HOME`，然后是 `ANDROID_SDK_ROOT`，然后是 `~/Android/Sdk`。
- Kotlin home：`[sdk] kotlin_home`，然后是 `KOTLIN_HOME`，然后是 PATH 上的 `kotlinc`，然后是 `<sdk-home>/kotlinc`（setup 安装的编译器，最后手段）。发行版根目录（`…/kotlinc`）及其父目录都被接受：如果 `$KOTLIN_HOME/bin/kotlinc` 缺失，但 `$KOTLIN_HOME/kotlinc/bin/kotlinc` 存在，则使用嵌套布局。
- Java：`$JAVA_HOME/bin/java`，然后是 PATH 上的 `java`。

`ccb check` 和 `ccb doctor` 会准确报告缺失的内容。

## `ccb setup`

通过 `sdkmanager` 为当前项目安装缺失的组件（或默认值：platform android-35、build-tools 35.0.0）。当 platform、build-tools 和 platform-tools 都已存在时，setup 会完全跳过许可证和 sdkmanager 步骤，直接进入其余步骤——因此即使在代理屏蔽 sdkmanager 的情况下也能工作，只要 SDK 已经配置好。顺序：

1. 缺失 JDK 17+ 时，主动提出安装一个（见下文），而不是在 sdkmanager 步骤以通用错误失败。
2. 在没有 cmdline-tools 时，从 Google 官方存档（`commandlinetools-linux-11076708_latest.zip`）引导安装，并在解压前用固定的 SHA-256 校验。
3. 接受 SDK 许可证。
4. 安装组件。
5. 在继续之前验证组件确实已安装——静默地什么都没安装的安装操作（例如全新 SDK 上许可证尚未接受）会被报告为组件缺失，绝不会报告为成功。
6. 在没有可用的 2.x 可以解析时安装 kotlinc。
7. 在项目声明 `[ndk]` 时安装 NDK 修订版。
8. 在项目启用 minify 但没有显式 jar 时安装 r8。
9. 从平台包管理器安装缺失的主机工具。
10. 把 `[deps] maven` 产物预取到本地 Maven 缓存中（见 [docs/maven.md](maven.md)）。
11. 最终验证（见下文）。
12. 打印 "SDK ready"。

找到但加载失败的配置是错误，而不是静默回退到默认 SDK home；默认值仅在完全没有 `ccb.toml` 时适用。

## 安装缺失的 JDK

没有 JDK 17+ → 用平台的包管理器安装 OpenJDK 17：

- Termux：`pkg install -y openjdk-17`（通过 `$PREFIX/bin/pkg` 检测）。
- 常规 Linux：`apt-get`（Debian/Ubuntu，`openjdk-17-jdk`）、`dnf`/`yum`（`java-17-openjdk-devel`）、`pacman`（`jdk17-openjdk`）、`zypper`（`java-17-openjdk-devel`）或 `apk`（`openjdk17`）——以 PATH 上有的为准。非 root 运行时，如果可用，命令会在 `sudo` 下运行；既无 root 也无 sudo 时则没有可用的安装器，setup 会报告这一点。
- 其他任何平台，或找不到安装器时：setup 会打印一条普通的手动安装消息（安装 JDK 17+，设置 `JAVA_HOME` 或把 `java` 放到 PATH 上），并以退出码 3 退出。

默认情况下安装是交互式的：在 TTY 上，setup 会询问 "Install OpenJDK 17 via \<installer\>? [y/N]"，仅在回答 "y"/"yes" 时安装；任何其他回答都会中止 setup。传入 `--yes` 可不经询问直接安装（用于脚本和非 TTY 运行）。没有 `--yes` 且没有 TTY 时，setup 会拒绝无人值守安装，并告诉你在终端中运行或传入 `--yes`。安装后，setup 会重新解析 `java`，如果仍然找不到则失败——报告成功却未提供可用 JDK 的包管理器不会被误认为成功。当 `JAVA_HOME` 未设置时，setup 还会把它指向解析出的 JDK，供其自身的 sdkmanager 调用使用（存在时使用 Termux 的 `$PREFIX/lib/jvm/java-17-openjdk`，否则使用 `java` 路径的 `bin/` 的父目录）。

## 安装缺失的 kotlinc

没有可用的 kotlinc 2.x（缺失，或版本过旧）→ 把官方 Kotlin 编译器 2.4.21 安装到 `<sdk-home>/kotlinc`，解析器会在最后手段时找到它（在 `[sdk] kotlin_home`、`KOTLIN_HOME`、PATH 之后）。安装是自动的，与 SDK 组件安装一样：目标是 CCB 自己的目录，而非系统包，因此没有提示，`--yes` 对这一步也不需要。存档来自 JetBrains 的官方 GitHub 发布（`kotlin-compiler-2.4.21.zip`），解压前用固定的 SHA-256 校验；安装后，setup 会运行新的 `kotlinc -version`，如果它没有报告 2.x 则失败。

找到非 2.x 的 kotlinc 时（例如 PATH 上有一个旧版本）：setup 仍会安装 2.4.21，但会警告——在旧版本被移除、或 `[sdk] kotlin_home` / `KOTLIN_HOME` 指向 SDK-home 安装之前，构建仍会继续选用旧版本；SDK-home 的副本在解析顺序中排最后，永远不会覆盖显式配置、环境变量或 PATH 中的 kotlinc。

覆盖（镜像、离线测试）：`CCB_KOTLIN_URL` 替换下载源，`CCB_KOTLIN_SHA256` 替换期望的哈希。自定义 URL 但没有自定义哈希时，会跳过校验和验证，并给出提示——固定的哈希仅适用于官方发布。安装需要在 PATH 上有 `unzip`。

`ccb setup --upgrade` 会在 setup 管理的（SDK-home）kotlinc 不是固定版本时重新安装它。用户管理的 kotlinc（配置、`KOTLIN_HOME`、PATH）永远不会被 `--upgrade` 触碰，SDK 组件版本也仍由配置固定——`--upgrade` 不是“全部最新”开关。

## 安装 NDK

项目声明 `[ndk]` 并带有 `[sdk] ndk_version`：当 `<sdk-home>/ndk/<version>` 缺失时，setup 会通过 sdkmanager（`ndk;<version>`）安装该修订版，并验证目录已落地。没有 `[ndk]`：跳过。有 `[ndk]` 但没有 `ndk_version`：无法安装（构建会报告缺失的版本）；setup 也会跳过它。

## r8

r8 不随 build-tools 提供；它通过 Google 的 Maven 仓库分发（`com.android.tools:r8`）。把 `[sdk] r8_jar` 设置为下载的 r8 jar（例如来自 `https://dl.google.com/dl/android/maven2/com/android/tools/r8/` 的 `r8-9.5.28.jar`），或在 minify 项目中运行 `ccb setup`，把官方 jar 9.5.28 安装到 `<sdk-home>/r8/r8.jar`，用固定的 SHA-256 校验。Setup 会验证它能运行（`java -cp <jar> com.android.tools.r8.R8 --version`——该 jar 的 manifest Main-Class 已损坏，因此像构建时一样显式调用该类）。当 `[sdk] r8_jar` 未设置时，构建会选用 setup 管理的副本；显式指定的 jar 始终优先。覆盖：`CCB_R8_URL` 和 `CCB_R8_SHA256`，语义与 kotlinc 的相同。使用 `--upgrade` 时，哈希与固定值不同的 setup 管理 jar 会被重新安装；自定义 URL/哈希不受升级管理。

`[build] minify = true`，同时 `[build] proguard_rules` 指向你的规则文件。`minify = true` 但没有 `r8_jar`（显式或 setup 管理的都没有）的 release 构建会失败，错误为 `CCB-E3014`。CCB 为 r8 无法看到的入口点生成的 keep 规则见 [docs/build.md](build.md)（dex 步骤）。你的规则文件也会传给 r8，用于 CCB 无法推断的 keep——仅通过反射引用的内容（`Class.forName` 字符串）——因此在 `minify = true` 时它仍然是必需的，但当应用不需要额外的 keep 时，它可以只包含一条注释。

## 主机工具（Termux 和 Linux）

SDK 的原生工具可能存在却无法运行：在 Termux（aarch64）上，build-tools 的 `aapt2` 和 `zipalign` 以及 platform-tools 的 `adb` 都是仅 x86-64 的。SDK 步骤之后，setup 会检查每个主机工具：可运行的 SDK 副本优先，其次是 PATH 上可运行的副本，否则——当没有覆盖固定选择时——setup 会安装平台软件包：

- Termux（`$PREFIX/bin/pkg`）：从官方仓库安装 `aapt2` 和 `android-tools`（用于 adb）；`zipalign` 从 rendiix 仓库安装（见下文），仅在官方仓库没有候选时使用。
- 常规 Linux：从发行版包管理器安装 `adb`（apt 上为 `adb`，其他地方为 `android-tools`，安装器检测和 sudo 规则与 JDK 安装相同）。Linux 上的 `aapt2`/`zipalign` 在 x86-64 上是 SDK 原生的，没有软件包修复办法；在 aarch64 Linux 上，最终验证会改为发出警告。

安装的限制与 JDK 安装相同：`--yes`（或 `--upgrade`，它对这些提示隐含 `--yes`）会不经询问直接安装；在 TTY 上，setup 会询问一次，指明工具（以及 zipalign 需要时涉及的 rendiix 仓库）；没有 TTY 且没有 `--yes` 时，setup 会打印一条说明并继续——主机工具是尽力而为，与 JDK 不同，没有 JDK setup 无法继续。拒绝也只是跳过安装。安装失败会报错（setup 已经承诺安装）；每次安装后，setup 会验证 PATH 上有可运行的副本。覆盖（`[sdk]` 键或 `CCB_*` 环境变量）会固定选择：对于被覆盖的工具，setup 永远不会安装。

### 第三方 Termux 仓库（rendiix）

官方 Termux 仓库没有 `zipalign`。当 setup 在 Termux 上需要它时，会先检查 `apt-cache policy zipalign`：有候选意味着官方（或已添加的）仓库提供了它，setup 会直接安装。只有在没有候选时，setup 才会添加 rendiix 仓库——列表文件 `$PREFIX/etc/apt/sources.list.d/rendiix.list`（`deb https://rendiix.github.io android-tools termux`），密钥 `https://rendiix.github.io/rendiix.gpg` 放入 `$PREFIX/etc/apt/trusted.gpg.d/`，然后运行 `apt update`——再从中安装 `zipalign`。添加仓库是幂等的（已有的列表文件会跳过整个添加过程），添加失败时会再次删除列表文件，以便重试时重新添加。信任提示：rendiix 是第三方仓库，使用自断言的密钥，不是官方 Termux 源；在添加任何内容之前，提示中会指明它。

## 最终验证

在每个安装步骤之后，setup 会解析工具链（加上 adb），并以警告形式报告仍需注意的事项——例如 PATH 上的旧 kotlinc 遮蔽了 SDK-home 的安装，或某个主机工具没有软件包修复办法。警告不会使 setup 失败：setup 已经做了能做的事，剩余事项需要用户操作。当一切都解析成功时，setup 会打印 `toolchain check: ok`。“SDK ready”表示 SDK 组件已就绪；最终验证才是让它意味着工具链可用的部分。

## `--upgrade` 和 `--print-env`

- `--upgrade`：当 setup 管理的工具（SDK-home kotlinc、SDK-home r8）与固定版本不同时重新安装它们，并对 JDK 和主机工具提示隐含 `--yes`。它永远不会触碰用户管理的工具，也永远不会提升配置固定的 SDK 版本。
- `--print-env`：在 setup 成功后，把已配置工具链的 shell 导出打印到 stdout，而不是打印 "SDK ready" 行（该行移到 stderr，因此 stdout 保持可安全 eval）：`ANDROID_HOME`、`JAVA_HOME`（有效值，未设置时推导得出），以及 SDK-home kotlinc bin 存在时的 `PATH` 前置。不打印任何 `CCB_*` 覆盖：借助 PATH 回退，CCB 无需它们就能在 PATH 上找到 Termux 工具。

## 下载缓存

固定下载（cmdline-tools、kotlinc、r8）缓存在 `$XDG_CACHE_HOME/ccb/downloads` 下（否则为 `~/.cache/ccb/downloads`），按 URL basename 作为键，当缓存副本的哈希匹配时复用——因此约 250MB 的 setup 下载每个版本只需获取一次，即使跨 SDK home 也是如此。部分下载会在下次运行时续传（curl `-C -`）；拒绝范围请求的服务器会回退到全新下载。未固定的下载和 `file://` URL 永远不会缓存。当没有可用的缓存目录时，setup 会直接下载并仍然校验哈希。

## 已知限制：需要认证的出口代理

`sdkmanager` 没有代理认证的标志，它的 Java HTTP 栈在面对需要认证的出口代理时会失败（`HttpURLConnection` 隧道中的 `NoSuchElementException`）。在这种环境中，`ccb setup` 会在 sdkmanager 步骤以仓库错误失败；此时必须通过直接下载来配置 SDK（curl 能很好地处理代理认证），这台虚拟机上的 SDK 就是这样配置的。这是环境限制，不是 CCB 的 bug：在能直接访问仓库的机器上，`ccb setup` 可以正常工作。缺失的 JDK 会在 sdkmanager 运行之前单独报告，因此代理说明不会掩盖它。

## 自定义工具路径

`aapt2`、`zipalign` 和 `adb` 不必来自 SDK。在 `ccb.toml` 中设置 `[sdk] aapt2` / `[sdk] zipalign` / `[sdk] adb`，或使用环境变量 `CCB_AAPT2` / `CCB_ZIPALIGN` / `CCB_ADB`。优先级，从高到低：`[sdk]` 键、环境变量、默认值（aapt2/zipalign 为 `<sdk-home>/build-tools/<version>/<tool>`，adb 为 `<sdk-home>/platform-tools/adb`），然后是 PATH 上可运行的副本。空的环境变量值视同未设置。相对值会按原样使用（相对于工具运行时的当前工作目录，而非项目根目录或 SDK home）。已设置但损坏的覆盖会在任何构建阶段之前失败，错误为 `CCB-E2009`，指明来源和路径——CCB 永远不会静默地从显式选择回退。但缺失或无法运行（架构错误）的默认 SDK 副本会回退到 PATH：“可运行”通过实际启动工具来证明（`aapt2 version`、`adb version`、`zipalign` 不带参数——除 127（执行失败退出码）之外的任何退出码都算数）。`ccb doctor` 和 `ccb check` 会以文本和 `--json` 输出打印 aapt2 和 zipalign 的有效路径及其来源（`config`、`env`、`default` 或 `path`）。

典型场景：在 Termux（aarch64）上，SDK build-tools 的 `aapt2` 是仅 x86-64 的二进制（2.19），无法原生运行，而 Termux 软件包的 `aapt2`（16.0.0.4-4，报告为 aapt 2.20）可以原生运行。用 `[sdk] aapt2 = "/data/data/com.termux/files/usr/bin/aapt2"`（`$PREFIX/bin/aapt2` 的展开形式；TOML 值不会经过 shell 展开）或在 shell 中设置 `CCB_AAPT2=$PREFIX/bin/aapt2` 把 CCB 指向它——或者直接安装 Termux 软件包，让 PATH 回退找到它（见“主机工具”）。同样的回退也适用于 `zipalign`（rendiix 仓库）和 `adb`（`android-tools` 软件包）。
