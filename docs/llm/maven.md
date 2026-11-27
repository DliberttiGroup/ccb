<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/maven.md. -->

# 远程 Maven 依赖

[文档索引](index.md)

CCB 解析 `ccb.toml` 中声明的远程 Maven 构件，并把它们像本地 AAR/JAR 一样喂给 deps 步骤。没有 Gradle：没有 daemon、没有构建脚本、没有插件解析——只有 POM 解析、下载和本地缓存。

## 声明依赖

```toml
[deps]
maven = [
  "androidx.core:core-ktx:1.13.1",
  "com.google.code.gson:gson:2.11.0",
]
maven_central = true
```

- `maven`：`group:artifact:version` 坐标数组。版本规格：
  - 确切版本：`1.13.1`
  - 范围：`[1.0,2.0)`、`[1.0]`、`(,1.5]`——从该构件的 `maven-metadata.xml` 解析；范围内已发布的最高版本获胜
  - `LATEST` / `RELEASE`：来自元数据的 `<latest>` / `<release>` 标签
  - `-SNAPSHOT`：`1.0-SNAPSHOT` 会解析为快照元数据中最新的带时间戳构建（`1.0-20261126.123456-3`）；锁文件记录具体的带时间戳版本
  版本比较遵循 Maven 的 ComparableVersion 规则（数字标记按数值比较；限定符顺序为 alpha < beta < milestone < rc < snapshot < release < sp；`1.0` == `1.0.0`）。没有已发布版本满足的范围，或没有快照元数据的快照，会以 `CCB-E3019` 失败。元数据在 setup 时会重新下载最新版本，因此范围和快照会浮动；仅用缓存的构建使用缓存的副本，因此构建永远不会下载。本地 `jars`/`aars` 可与 `maven` 并用。
- `maven_central`：为 `true` 时，在 Google Maven 之后搜索 Maven Central。默认 `false`：仅 Google Maven。
- `maven_profiles`：要激活的 profile id 数组（见下面的 Profiles）。默认空：只有 `<activeByDefault>` 的 profile 会生效。
- `maven_repos`：额外仓库数组（见下面的仓库）。
- `maven_exclude`：排除项数组，每个为 `group:artifact`（所有版本）或 `group:artifact:version`（仅该版本）。被排除的构件永远不会解析，即使是传递依赖。排除一个直接依赖是加载时的配置错误，错误为 `CCB-E1005`，注明排除项和该依赖；排除某个直接依赖的其他版本是允许的。

## 编译器插件

`[build] kotlin_plugins` 以 Maven 坐标声明 kotlinc 编译器插件：

```toml
[build]
kotlin_plugins = ["org.jetbrains.kotlin:kotlin-compose-compiler-plugin:2.4.21"]
kotlin_plugin_options = ["androidx.compose.compiler.plugins.kotlin:generateFunctionKeyMetaClasses=true"]
```

- 插件坐标必须是确切版本（范围、`LATEST`、`RELEASE`、`-SNAPSHOT` 会在配置加载时以 `CCB-E1005` 被拒绝）：插件必须与 kotlinc 版本匹配。
- 插件 jar 像任何构件一样通过 Maven 解析（相同的仓库、缓存和校验和），但不做传递遍历：插件 jar 必须是自包含的。`pom` 或 `aar` 打包类型会以 `CCB-E3019` 失败。插件 jar 不在 `ccb.lock` 中：`ccb.toml` 中的确切坐标就是它们的固定版本。
- `ccb setup` 会随 Maven 构件一起预取插件 jar；`ccb check` 会报告 `kotlin plugins: N jars`；缓存缺失时构建会在第 0 阶段以 `CCB-E3021` 失败。
- kotlinc 会为每个插件 jar 得到 `-Xplugin=<jar>`，并为每个 `kotlin_plugin_options` 条目得到 `-P plugin:<id>:<key>=<value>`（每个选项的格式为 `plugin-id:key=value`）。编译指纹包含插件 jar 和选项，因此改动其中任何一个都会重新编译。
- Compose：Compose 编译器插件的路径是 `org.jetbrains.kotlin:kotlin-compose-compiler-plugin:<kotlin-version>`，配合 `maven_central = true`，以及来自 Google Maven 的 Compose 库（runtime、ui、foundation-layout、material3、activity-compose）。使用普通的构件，不要使用 `kotlin-compose-compiler-plugin-embeddable`：`-embeddable` 变体是为 Gradle 的内嵌编译器构建的（带阴影的 `org.jetbrains.kotlin.com.intellij.*` 类），而 CCB 运行的是独立的 `kotlinc`，其 `kotlin-compiler.jar` 携带的是未带阴影的 `com.intellij.*` 类——`-embeddable` 插件会使 kotlinc 以 `ClassNotFoundException` 崩溃。直接固定 `androidx.core:core-ktx`（activity 1.13.0 对应 1.18.0）：否则最近者获胜会保留某些 Compose 库声明的较旧 core（1.16.0），而它缺少 `ComponentActivity` 1.13.0 需要的 `PictureInPictureProvider` 超类型——Gradle 的最新者获胜掩盖了这一点，Maven 的最近者获胜不会。同样适用于 `androidx.activity:activity-ktx`（1.13.0）、`androidx.lifecycle:lifecycle-runtime-ktx`（2.10.0）、`androidx.lifecycle:lifecycle-viewmodel-ktx`（2.10.0），以及 savedstate 三件套（`androidx.savedstate:savedstate` 1.5.0、`androidx.savedstate:savedstate-compose-android` 1.5.0、`androidx.savedstate:savedstate-ktx` 1.5.0——第一个带来 `kotlinx-serialization-core` 1.7.3，而 `runtime-saveable` 需要它）：较旧版本打包的类在较新版本中已经移动，因此过期的版本会在 dex 时造成类重复。还要排除 `androidx.compose.animation:animation-core`：动画库以两个二进制不兼容的名称发布——新的 KMP `animation-core`（1.2.1，重写过的 `Transition` API）和旧的 `animation-core-android`（1.12.0）。Material3 1.5.0 和旧的动画系列调用的都是旧 core 独有的方法，而图中没有任何东西调用新 core 的独有方法，因此应该保留旧 core。一个最小的 Compose 示例可以用 `ccb build` 端到端构建。

## 仓库

- Google Maven（`https://dl.google.com/dl/android/maven2/`）永远排第一。
- Maven Central（`https://repo.maven.apache.org/maven2/`）排第二，仅当 `maven_central = true` 时。
- 额外仓库按声明顺序排在 Google Maven 和 Central 之后：`[deps] maven_repos = ["https://repo.example.com/maven2", "https://priv.example.com/m2 alice env:PRIV_PASS"]`。每个条目是一个基础 URL，或 `url username password-source`，其中密码来源是 `env:VAR`（在解析时从环境中读取）或字面密码。凭据只通过 curl 的 `-u` 参数传递，绝不会出现在 URL、日志或诊断中：URL 中的 userinfo（`user:pass@host`）会在配置加载时以 `CCB-E1005` 被拒绝，缺失的凭据变量会以 `CCB-E3019` 失败，注明仓库和该变量。
- 没有按依赖选择仓库：每个坐标都会按顺序在每个已配置仓库中尝试，第一个提供 POM 的仓库获胜。

测试覆盖：`CCB_MAVEN_REPOS`（逗号分隔的基础 URL 列表）会整体替换已配置的仓库。它的存在是为了让测试可以指向本地 `file://` 夹具仓库；它不是添加私有仓库的支持方式。

## 打包类型：AAR 或 JAR

构件类型来自该构件 POM 的 `<packaging>`：

| 打包类型 | 结果 |
| --------- | ------ |
| `aar` | 作为 AAR 下载，与本地 AAR 完全一样处理（解压、`classes.jar` 进 classpath、`res/` 合并、manifest 合并）。 |
| `jar`（或缺失 `<packaging>`，Maven 默认即为 jar） | 作为 JAR 下载，放到编译 classpath 和 dex 输入上。 |
| `pom` | 不下载构件；该 POM 自身的依赖仍会解析（BOM 风格 POM 就是这样处理的）。 |
| 其他任何值 | `CCB-E3019`，注明该坐标。 |

## 传递依赖

直接依赖和传递依赖都从 POM 的 `<dependencies>` 解析：

- 作用域：`compile`（包括缺失作用域，Maven 将其视为 compile）和 `runtime` 会解析。`test`、`provided`、`system`、`import` 会跳过，标记为 `<optional>true</optional>` 的依赖也会跳过。
- 版本冲突按最近者获胜解析：从直接声明出发按广度优先遍历依赖，某个 `group:artifact` 第一次出现的版本获胜。之后更深（或同深度、后声明）的版本会被跳过；使用 `--verbose` 时，跳过会打印为 `maven: <g:a>: requested <v>, using nearest <w>`。
- 循环安全：已见过的 `group:artifact` 永远不会重新入队。
- 属性：`${...}` 从同一 POM 的 `<properties>` 以及 `project.groupId` / `project.artifactId` / `project.version` 解析。依赖版本也可以来自同一 POM 的 `<dependencyManagement>` 条目。
- 父 POM：当 POM 带有 `<parent>` 块时，父 POM 会从同一仓库取回（与任何其他 POM 相同的缓存和校验和规则），并应用到子 POM 上，沿链递归向上。父 POM 的 `<properties>`、`<dependencyManagement>`、`<dependencies>` 和 `<profiles>` 会合并进子 POM：子 POM 的条目获胜，父 POM 之间最近的祖先获胜；`${...}` 展开使用合并后的集合。省略 `groupId` 或 `version` 的 POM 会从其 `<parent>` 块继承这些值。链最多 10 个祖先；更长的链，或循环（父坐标重复链中更早的坐标），会以 `CCB-E3024` 失败，注明该链。父版本本身也可以是一个范围或 `-SNAPSHOT` 规格。不继承的内容：父 POM 声明的排除项（会被忽略）。
- 排除项（`[deps] maven_exclude`）在最近者获胜之前检查：被排除的构件永远不会进入已见集合，因此被排除的版本永远不会获胜，同一构件的另一个版本可以从之后的路径获胜。被排除构件自身的传递依赖会随之剪掉：永远不会入队。使用 `--verbose` 时，每次跳过的排除项会打印为 `maven: <g:a:v>: excluded, skipping`。
- KMP 变体：当某个 `-jvm` 构件的 `-android` 兄弟构件（同一 group、同一版本）也被解析出来时，该 `-jvm` 构件会被丢弃——Kotlin 多平台库会同时发布两者，而 dex 会拒绝两者都定义的类。没有 `-android` 兄弟构件的 `-jvm` 构件会被保留（纯 JVM 库没有兄弟构件）。兄弟构件必须是同一版本；版本不匹配时两者都保留。
- Profiles：见下面的 Profiles 一节。
- 经过 dependencyManagement 回退后仍没有版本的依赖，会以 `CCB-E3019` 失败，注明该依赖。

## Profiles

- 当某个 profile 的 id 列在 `[deps] maven_profiles` 中时，它是激活的；或者，当该列表为空时，它的 `<activation><activeByDefault>` 为 true 时是激活的。任何其他激活方式（`<jdk>`、`<os>`、`<property>`、`<file>`）都不会被求值：除非被列出，否则该 profile 保持未激活。
- 激活的 profile 的属性和 dependencyManagement 会就地覆盖 POM 自身的；它的依赖会追加到 POM 自身的依赖之后。继承自父 POM 的 profile 同样如此。
- `maven_profiles` 中未知的 profile id 不会激活任何东西，也不会使任何东西失败。

## 缓存与校验和

解析出的构件存放在专用的 Maven 缓存中，而不是扁平的下载缓存：

- 位置：`$XDG_CACHE_HOME/ccb/maven2`，否则为 `~/.cache/ccb/maven2`（`CCB_MAVEN_CACHE` 可覆盖）。布局与仓库一致：`<缓存>/<group 路径>/<artifact>/<version>/<artifact>-<version>.<扩展名>`，`.pom` 放在旁边。
- 校验和：下载时先校验已发布的 `.sha256`，否则校验已发布的 `.sha1`。不匹配会以 `CCB-E3020` 失败，注明该构件。
- 没有已发布校验和：下载会做结构校验（curl `--fail`、完整传输），并把它的 SHA-256 记录在 `<文件>.ccb-sha256` 边车文件中。之后的缓存命中会对照边车文件校验，因此损坏的缓存文件会以 `CCB-E3020` 失败，而不是悄悄毒化一次构建。
- 没有边车的缓存文件（手工放进去的，或旧版 CCB 留下的）会在首次使用时被信任并收编：它的 SHA-256 会被记录下来，供之后的命中使用。Explain 和 dry-run 模式永远不会写边车文件。

## 封闭性：setup 预取，构建永不下载

`ccb setup` 是唯一允许访问网络的命令（spec §6.1）：

- 在带有 `[deps] maven` 的项目中，`ccb setup` 会把每个直接 + 传递构件预取到缓存中（在 SDK 步骤之后、最终验证之前），并报告 `maven: prefetched N artifacts (A aar, J jar)`。预取失败会带着诊断停止 setup——半预取的项目永远不会被报告为就绪。预取会为每个解析出的 POM 取回完整的父 POM 链，并重新下载版本元数据的最新版本，因此范围和快照会浮动到最新的匹配项。在带有 `[build] kotlin_plugins` 的项目中，setup 还会预取插件 jar，并报告 `maven: prefetched N compiler plugin jars`。
- `ccb build` 和 `ccb check` 只解析缓存。某个从未预取过的坐标会以 `CCB-E3021` 失败，注明该坐标并告诉你运行 `ccb setup`。失败发生在第 0 阶段，在任何构建阶段运行之前。
- 工作流：添加一个坐标，运行 `ccb setup`，然后离线构建。删除缓存（或在新机器上检出）只是意味着再运行一次 `ccb setup`。

`ccb check` 会在成功时报告解析出的集合：`maven: N artifacts (A aar, J jar)`。配置了排除项时，check 还会报告它们：`maven: excluded <g:a>, ...`（`--json` 输出中还有一个 `maven_excluded` 数组）。配置了编译器插件时，check 还会报告 `kotlin plugins: N jars`。

## 锁文件（`ccb.lock`）

`ccb setup` 会在成功预取后把完整解析出的树写入项目根目录的 `ccb.lock`，`ccb check` / `ccb build` 在锁文件存在时强制执行它：

- 格式：每行一个坐标，按 `group:artifact` 排序，附带打包类型和该构件的 SHA-256：`com.example:lib:1.0 aar <sha256>`。注释行（`#`）会被忽略。对于 `pom` 打包类型（没有构件文件），记录的哈希是 POM 的哈希，因此改动过的 BOM 仍会使强制执行失败。
- 强制执行是精确的：解析出的版本、打包类型或哈希与锁文件不符会以 `CCB-E3023` 失败，注明该坐标；解析出的坐标在锁文件中缺失，或锁定的坐标不再能解析时也是如此。过期的锁文件永远不会悄悄解析出别的东西：运行 `ccb setup` 重新生成它（setup 会根据声明重新解析并覆盖该文件，即使存在过期的锁文件）。
- 没有锁文件意味着没有强制执行：锁文件之前的项目行为与之前完全相同。没有 `[deps] maven` 的项目永远不会写入或查阅锁文件，因此删除所有 Maven 坐标后，过期的 `ccb.lock` 会无害地留在原地。
- 锁文件是项目文件：提交它。它让一次检出可复现，而无需信任缓存：缓存按坐标键控，但锁文件固定了确切的树（包括传递版本）和每个构件的哈希。

## 限制

- 没有签名（PGP）验证：只有校验和。
- POM 中没有按依赖的 `<exclusions>`：排除项只有项目级的 `[deps] maven_exclude`。
- Profile 激活只按 id：`<jdk>`、`<os>`、`<property>`、`<file>` 激活不会被求值。
- 父 POM 声明的 `<exclusions>` 会被忽略。

## 故障排查

代码含义：[diagnostics.md](diagnostics.md)。

| 代码 | 修复方法 |
| ---- | --- |
| `CCB-E3021` | 构件尚未进入本地缓存。运行 `ccb setup`，然后重新构建。父 POM 从缓存中被逐出时也一样：setup 会重新预取整条链。 |
| `CCB-E3024` | 父链循环，或祖先超过 10 个。在 POM 中修复该链。 |
| `CCB-E3020` | 下载的文件与仓库已发布的校验和不符。删除缓存条目（或整个 `<缓存>/<group 路径>/<artifact>/<version>/` 目录），重新运行 `ccb setup`；如果问题仍然存在，该仓库或镜像在提供错误的字节。 |
| `CCB-E3019` | 坐标在 Google Maven（或已启用时的 Central）中不存在。检查拼写和版本，以及该构件是否确实在 Central 上（是的话设置 `maven_central`）。另外：没有已发布版本满足的范围、没有快照元数据的快照，或私有仓库缺失的凭据变量（诊断会注明该变量）。 |
| `CCB-E1005`（坐标） | 声明的坐标格式错误或未固定。修复 `ccb.toml` 中的字符串。插件坐标必须是确切版本。 |
| `CCB-E1005`（仓库条目） | `[deps] maven_repos` 条目格式错误：使用 `url` 或 `url username env:VAR`（或字面密码）。URL 中不允许出现 userinfo。 |
| `CCB-E1005`（排除项） | 排除项移除了项目直接声明的东西。删除该排除项或该直接坐标。 |
| `CCB-E3023` | 解析出的树与 `ccb.lock` 不符（版本、打包类型、哈希、缺失或过期条目），或锁文件格式错误。运行 `ccb setup`，根据当前声明重新生成它。 |
