<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/build.md. -->

# 构建流水线与缓存

## 各阶段（按顺序）

1. **Stage 0** — 加载并验证 `ccb.toml`，一次报告所有问题。解析工具链。Release 构建在编译任何内容之前验证签名配置。
2. **deps** — 解压 AAR（类、资源、assets、JNI）。
3. **merge-manifest** — 按配置顺序把 AAR 的 `AndroidManifest.xml` 文件合并到应用 manifest 中。输出：`build/intermediates/merged-manifest/`。没有 AAR 时跳过。名称冲突时应用条目获胜（警告会注明 AAR 和该条目）；两个 AAR 中相同的条目会静默去重；同名但属性不同的条目保留第一个 AAR 的版本（警告会注明两个 AAR）。合并的内容：应用组件（`activity`、`service`、`receiver`、`provider`、`activity-alias`）、`uses-permission`、`uses-feature`。库 manifest 中的其他所有内容都会被忽略。相对组件名会根据所属 manifest 的 package 解析，并改写为完全限定名。格式错误的 AAR manifest 会以 `CCB-E3018` 使构建失败，且在打包之前；没有 manifest 的 AAR 合并后相当于什么都没有。根据输入生成指纹，因此任何 manifest 变化都会使 `link` 重新运行。
4. **compile-res** — 对每个资源目录运行 `aapt2 compile`，生成 `.flat` zip。
5. **link** — 运行 `aapt2 link` 生成 `linked.ap_` 并生成 `R.java`。有 AAR 时使用合并后的 manifest，否则使用应用 manifest。manifest 的 `package` 必须与 `[project] package` 一致。
6. **compile-code** — 先运行 `kotlinc`（Kotlin + `R.java` + Java 源码），再运行 `javac` 编译 Java 源码和生成的 `R.java`。kotlinc 能解析 R 符号但不生成 R.class 文件，因此必须由 javac 编译它们——否则运行时 dex 中会缺少库的 R 类。Classpath：`android.jar` + `deps.jars` + AAR 的 `classes.jar` 文件。
7. **dex** — 运行 `d8`（release + minify 时运行 `r8`）。`.class` 文件逐个传入，而非传入目录：kotlinc 还会写入 `META-INF/*.kotlin_module` 文件，而 d8 会拒绝目录输入中的非类文件。对于 Kotlin 项目，kotlinc stdlib jar 会自动作为程序输入加入（项目已通过 `[deps]` 或 Maven 提供时跳过）；release + minify 时它是 R8 的程序输入，而非 `--lib`。
   - **Stdlib dex 缓存。** stdlib jar 只会被 dex 一次，存入全局缓存 `$XDG_CACHE_HOME/ccb/dex-stdlib`（否则为 `~/.cache/ccb/dex-stdlib`，与 Maven 缓存同根，`ccb clean` 后仍然保留），键为 stdlib jar 哈希 + min-api + build-tools 版本。之后的构建会把缓存的 `.dex` 传给 d8，而不是传 jar。单个 `<key>.dex` 文件，通过原子重命名发布，读取时校验（dex 魔数 + 头部文件大小）。缺失或损坏的条目会回退到 jar，绝不会导致构建失败。Release + minify 除外：R8 接收 stdlib 作为程序输入 JAR，而非 dex。
   - **R8 keep 规则（release + minify）。** CCB 生成 keep 规则（`build/intermediates/r8-keep.pro`，在项目自己的 `proguard_rules` 文件之前传给 R8）：应用的整个包（`-keep class <package>.** { *; }`）、`<application android:name>` 以及每个 manifest 组件（activity、service、receiver、provider、activity-alias 及其 `targetActivity`），以及 layout XML 中的视图类（带点的元素名、`class`、`android:name`、`layout_behavior`）。Manifest 条目来自应用、AAR 和库模块的 manifest；布局来自应用和模块的 res 目录以及 AAR zip。生成的文本是 dex 指纹的一部分，因此 manifest 或布局变化会重新运行 dex。R8 能容忍针对不存在类的 keep（仅警告），所以过期的布局引用不会破坏构建。仅通过反射引用的内容（`Class.forName` 字符串）无法自动保留：把这些 keep 写进项目自己的 `proguard_rules` 文件，该文件在 `minify = true` 时仍然是必需的（其他情况下可以只包含一条注释）。
   - **Dexcheck 仍会运行**，在 dex 之后，并且仍然通过——缓存的 stdlib 类已包含在合并后的 dex 中。
   - **实测，Showcase：** 冷构建之前为 57s，冷缓存时为 63s（一次性填充），热缓存时为 33–34s。Release + minify：APK 在 minify 关闭时为 2,651,777 字节，开启时为 75,393 字节；dex 从 2,615,024 变为 37,160。
   - **d8 堆。** build-tools 的 `d8` 包装脚本默认使用 `-Xmx2G`，它会胜过 CCB 的 `JAVA_TOOL_OPTIONS=-Xmx1g`（命令行 `-Xmx` 优先于环境变量）。CCB 改为传入 `-JXmx<N>m`，其中 N = 256 + 每 MiB dex 程序输入 16（应用 `.class` 文件 + `[deps]` jar + AAR `classes.jar` 文件 + stdlib 输入，缓存的 `.dex` 或 jar），上限为 1024MB。填充 stdlib dex 缓存时使用同样的标志（输入：仅 stdlib jar）。下限依据（2026-11-25，build-tools 36.0.0）：d8 在 Showcase 热输入和单独的 stdlib jar 上以 128m 成功，在 10,000 个合成类 + stdlib dex（输入 8.2MB）上以 256m 成功，且与 1024m 相比没有时间损失；Showcase 的 dex 在 256m 和 2G 默认值下字节完全相同。堆大小永远不会改变 dex 输出，因此它不是 dex 指纹的一部分。R8 不受影响：全程序缩减不遵循此规则，而且它已有的 1G 上限与此处的上限相同。
8. **dexcheck** — 解析 dex 文件，验证 dex 引用的每个类都在 dex 中定义。被引用但未定义的程序类会在打包之前使构建失败，错误为 `CCB-E3025`，注明缺失的类。平台类（`java.*`、`android.*`，……）来自 `--lib` 输入，永远不在 dex 中：会被列入白名单，永不报告。仅出现在注解和签名中的引用仍然计入。即使 dex 因缓存而跳过，以及 AAB 构建时，它都会运行。只需几毫秒，没有自己的缓存，也不计入 `steps_run`，因此无操作重建仍会报告 "up to date"。`--explain` 会打印它的计划行；`--dry-run` 不运行它。
9. **native** — 每个 ABI 运行 NDK clang（仅当存在 `[ndk]` 小节时）。
10. **package** — CCB 自有的 ZIP 写入器：`linked.ap_` 条目、dex 文件、assets（同名时应用 assets 优先于 AAR assets）、`lib/<abi>/*.so`（重复的 `.so` 名称是错误）。
11. **align** — `zipalign -f 4`。
12. **sign** — `apksigner`。Debug 使用 `~/.android/debug.keystore`（缺失时用 keytool 创建）。Release 使用 `env:VAR` 密码引用；密码绝不会出现在命令或日志中。
13. **verify** — `apksigner verify` 和 `aapt2 dump badging`。只有两者都通过，APK 才算构建成功。

## AAB 输出（`--aab`）

同样的阶段，bundle 方面的差异（spec §5.10）：link 使用 `--proto-format` 运行，生成 `linked-aab.ap_`（有自己的缓存键）；package 按 base 模块布局（`base/manifest/`、`base/resources.pb`、`base/res/`、`base/dex/`、`base/assets/`、`base/lib/<abi>/`），再加一个手工编码的 `BundleConfig.pb`；跳过 align；sign 增加 `--min-sdk-version`；verify 是 `jarsigner -verify` 加结构条目检查（apksigner verify 无法验证 bundle——它要求有根 manifest）。输出：`build/outputs/<variant>/app.aab`。`bundletool validate` 对 CCB 构建的 bundle 可以通过。

## 缓存

粒度为每个步骤：每个步骤的指纹是其输入的哈希（文件内容 + 相关配置值 + 工具版本）。未变化的步骤会被跳过，复用此前的输出。

- 仅改注释的 Kotlin 变更会重新运行 kotlinc（源码哈希变了），但会跳过 dex 及其下游：`.class` 文件——进而每个下游指纹——都是相同的。
- 字符串*值*的变更会重新运行 compile、link、package、align、sign、verify，但跳过 kotlinc 和 dex（`R.java` 未变）。
- 工具升级会改变工具指纹，使所有步骤失效。
- `--explain` 会打印运行/跳过计划而不执行。它在 `link` 下游是保守的——见 [docs/json-protocol.md](json-protocol.md)。
- `--dry-run` 会打印确切的命令且不执行任何内容。

缓存位于 `build/ccb-cache.json`。缺失或损坏的缓存意味着“全部重建”，绝不是错误。`ccb clean` 会删除 `build/`，包括缓存。

## 可复现构建

ZIP 写入器使用固定的 1980-01-01 时间戳和确定的条目顺序（先是按 aapt2 顺序排列的已链接资源，然后是排序后的 dex、assets、原生库）。相同输入的两次干净构建会生成字节完全相同的 APK：

```
ccb clean && ccb build && sha256sum build/outputs/debug/app.apk
ccb clean && ccb build && sha256sum build/outputs/debug/app.apk
```

apksigner 还会在 APK 旁边写入一个附属 `app.apk.idsig` 文件；比较的是 APK 本身的字节。

## 已知限制与成本

- **JVM 启动主导了耗时。** kotlinc、d8 和 apksigner 各自都会启动一个 JVM（在慢速虚拟机上需要数秒）。CCB 自身的工作——哈希、决策、打包——远不到一秒。Spec §8 的目标（冷构建 <10 s，Kotlin 增量 <2 s）在这台虚拟机上无法达到：hello-world 冷构建约需 23 s，单个 Kotlin 文件重建约需 14 s。下文的 opt-in kotlinc daemon 能让热重建跳过 kotlinc 的 JVM 启动，但对冷构建没有帮助——启动 daemon 的成本与启动 kotlinc 差不多。
- **工具探测缓存。** `kotlinc -version` / `java -version` 探测过去主导了无操作重建的耗时（约 3.4 s 的无操作重建中有约 3 s 是探测）。现在它们缓存在 `build/intermediates/tool-probes.json` 中，键为工具路径 + mtime + 大小。工具未变的重复构建会跳过两次探测（热无操作约 0.2 s）。不匹配、缺失条目或损坏文件会重新探测；`ccb clean` 会随 `build/` 的其余部分一起删除它。它永远不会改变构建输出：它只提供工具指纹，而未变更二进制文件的缓存版本与全新探测的结果相同。
- **d8 输入大小。** `.class` 文件作为单独的参数传入；极大的项目可能接近操作系统参数上限。如果真遇到这个问题，d8 argfile 就是修复方案。
- **build-tools 包装脚本**（`d8`、`apksigner`）会从 PATH 执行普通的 `java`，并忽略 `JAVA_HOME`，因此 CCB 会把解析出的 JDK `bin` 目录前置到 PATH 上，供 JVM 工具运行使用。
- **此环境中没有设备或模拟器**，因此 install/launch 仅验证到设备边界为止（见 [docs/device-setup.md](device-setup.md)）。

## Kotlinc daemon（opt-in）

默认：普通的 `kotlinc` 子进程。一个二进制文件、一条命令、一个结果，没有后台进程。选择加入后，同一项目的多次构建会复用一个 kotlinc 编译 daemon——热重建可以跳过 kotlinc 的 JVM 启动。

```toml
[build]
kotlinc_daemon = true            # default false
kotlinc_daemon_idle_seconds = 300 # default 300, allowed 60-3600
```

```sh
ccb build --kotlinc-daemon      # opt in for this build (overrides config)
ccb build --no-kotlinc-daemon   # opt out for this build (overrides config)
ccb daemon stop                 # ask the project's daemon to shut down
```

同时使用两个标志是错误。`ccb clean` 会在删除 `build/` 之前尽力停止 daemon（仅当配置中启用了 daemon 模式时）。

- 按项目隔离：运行文件位于 `build/kotlin-daemon` 下。两个项目永远不会共用一个 daemon；`ccb clean` 会随 `build/` 的其余部分一起删除它。
- CCB 直接调用 daemon 客户端（`org.jetbrains.kotlin.daemon.client.KotlinCompilerClient`），并使用显式 classpath——客户端 7 个 jar，编译器 6 个 jar（包括 `kotlin-daemon.jar`）——绝不使用 `lib/*` 通配符。缺少 jar 时会静默回退到普通 kotlinc。
- 客户端和 daemon 的 JVM 都会获得 `-Djava.net.preferIPv4Stack=true`：没有它时，daemon 到回环注册表的 RMI/JRMP 握手会在这个沙箱虚拟机中挂起。该标志是关键的；不要删除它。
- 两个 JVM 的堆都限制为 512 MB（`-Xmx512m`，不可配置）。
- 空闲关闭需要两个选项，两个都需要：`--daemon-autoshutdownIdleSeconds=<idle>` 和 `--daemon-shutdownDelayMilliseconds=<idle * 1000>`。仅空闲超时只在客户端连接期间生效；CCB 的客户端在每次构建后退出，因此没有关闭延迟的话，daemon 会在第一次构建后约 1 s 退出，永远不会被复用。
- `ccb daemon stop` 会要求 daemon 关闭，但 daemon 仍会遵守关闭延迟：进程在空闲超时内退出，而不是立即退出。如果需要等待，请轮询它。
- 即使编译失败，daemon 客户端也会以 0 退出，因此 CCB 不信任其退出码：它会解析捕获的输出，查找 `result code: 0`（成功）和行首的 `ERROR\t`（编译错误，报告为 `CCB-E3004`）。
- 过期或崩溃的 daemon 永远不会导致构建失败：客户端无法连接（或退出时既没有结果码也没有错误行）→ CCB 会记录一条回退说明并运行普通 kotlinc。只有普通 kotlinc 也失败时，构建才会失败。
- daemon 日志中可能包含脚本插件（`ScriptingK2CompilerPluginRegistrar`）的 `ClassNotFoundException`：这是良性的，脚本插件不在 daemon classpath 上，而且 CCB 从不编译脚本。
- 开关 daemon 不会使输出失效：相同的编译器、相同的参数，compile-code 指纹不变。

在此虚拟机上的实测（2026-11-25，kotlinc 2.4.21，单次运行，共享虚拟机，约 1.5 CPU——数据有噪声，请把冷构建数字视为范围，而非承诺）：

| 项目 | 冷构建，daemon 关闭 | 冷构建，daemon 开启 | 单文件重建，关闭 | 单文件重建，开启 |
|---|---|---|---|---|
| hello | 23–29 s | 25 s | 13.6 s | 5.2 s |
| Showcase (copy) | 34–64 s | 66–77 s | 21.6 s | 17.8 s |

冷构建基本打平（daemon 启动成本与普通 kotlinc 启动成本差不多，Showcase 的冷构建范围在两种方式下重叠）。热重建会受益——kotlinc 步骤本身的耗时从约 8 s 降至约 2.5 s。Showcase 热重建的收益绝对值较小，因为其重建耗时主要由非 kotlinc 阶段（aapt2、dex、打包）主导。

## Release 二进制（静态 + stripped）

可交付的二进制是 release 构建：`ccb` 以 `-static` 链接并 stripped。dev 和 sanitizer 构建保持动态且不 stripped——ASan 不能与静态二进制一起工作，因此这两个选项在 CMake 中互斥（同时配置会失败）。

```
cmake --preset release && cmake --build --preset release
# manual form:
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCCB_STATIC_RELEASE=ON
cmake --build build-release -j4
```

`CCB_STATIC_RELEASE` 仅适用于 `ccb` 可执行文件：`-static` 加上构建后 strip。同一目录下的测试可执行文件保持动态且不 stripped（开发产物，不会交付）。`dev` 和 `sanitizer` preset 会配置 `build/` 和 `build-san/`，并关闭该选项，因此普通和 sanitizer 工作流不变。`ccb check` 和完整应用构建已用静态二进制验证通过（Showcase 干净构建，2026-11-25）。

实测大小（2026-11-25）：

| 二进制 | 大小 |
|---|---|
| Dev `build/ccb` (Debug, dynamic, unstripped) | 1,151,424 |
| Dynamic Release, stripped (for comparison) | 531,208 |
| Static Release, unstripped | 1,373,432 |
| Static Release, stripped (`build-release/ccb`) | 1,268,272 |

有时引用的约 605 KB 数字是 stripped 后的*动态*二进制；静态二进制约为 1.27 MB，因为链接了 glibc。无论如何，它都远低于 requirement.md §8 中的 5 MB 上限，而且静态构建使 requirement.md §2.5 中“一个静态二进制”的表述对 release 产物成立。验证方法：

```
file build-release/ccb   # statically linked, stripped
ldd build-release/ccb    # not a dynamic executable
```

## 运行测试

先构建 CCB（[README](../README.md#install-from-source)）。然后：

```
ctest --test-dir build            # sequential; fastest on this VM
ctest --test-dir build -j4        # parallel; safe (each test binary
                                  # uses its own mkdtemp dir), but slower
                                  # here: 4 parallel JVM-heavy builds
                                  # share 1.5 CPUs and 6 GB RAM
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DCCB_SANITIZE=ON
cmake --build build-san && ctest --test-dir build-san
```

绝不要同时运行两次 `ctest` 调用（plain + sanitizer，或两次 plain）：请依次运行。每个测试二进制使用各自进程的 `mkdtemp` 目录（`/tmp/ccb_test_<name>_XXXXXX`），因此一次 `ctest -jN` 运行内的并行测试不会冲突——但两次并发的 `ctest` 运行仍会共享 CPU 和内存。在这台虚拟机上 `-j4` plain 实测为 1037s，而顺序运行为 739s，`-j2` 为 1082s（2026-11-25，47 个测试）。瓶颈是 CPU 密集的 JVM 工作（kotlinc + d8），而非测试串行化。d8 的堆现在已按大小设置（256MB 下限，1024MB 上限——见上文 dex），因此四个并行的 d8 运行最多提交 4G 而非 8G；-j4 变慢本身是 CPU 密集导致的，而非内存。上文的 stdlib dex 缓存才是真正缩短每次构建时间的东西（Showcase 57s → 热缓存 33–34s；`test_aab` 107s → 90s）。加入缓存后的完整 plain 套件顺序运行实测为 1060s（2026-11-25）——与 739s 基线不能直接比较：套件多了 3 次构建（`test_dex_contents` 中的新缓存测试，+116s），而且这次运行与虚拟机上的其他工作共享资源（长测试慢了约 20%）。共 50 个测试。
