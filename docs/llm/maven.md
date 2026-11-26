<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/maven.md. -->

# 远程 Maven 依赖

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

- `maven`：`group:artifact:version` 坐标数组。仅接受确切版本：范围（`[1.0,2.0)`）、`LATEST`、`RELEASE`、`-SNAPSHOT` 会在配置加载时被拒绝，错误为 `CCB-E1005`，注明该坐标。本地 `jars`/`aars` 可与 `maven` 并用。
- `maven_central`：为 `true` 时，在 Google Maven 之后搜索 Maven Central。默认 `false`：仅 Google Maven。
- `maven_exclude`：排除项数组，每个为 `group:artifact`（所有版本）或 `group:artifact:version`（仅该版本）。被排除的构件永远不会解析，即使是传递依赖。排除一个直接依赖是加载时的配置错误，错误为 `CCB-E1005`，注明排除项和该依赖；排除某个直接依赖的其他版本是允许的。

## 仓库

- Google Maven（`https://dl.google.com/dl/android/maven2/`）永远排第一。
- Maven Central（`https://repo.maven.apache.org/maven2/`）排第二，仅当 `maven_central = true` 时。
- v1 没有其他仓库。没有按依赖选择仓库：每个坐标都会按顺序在每个已配置仓库中尝试，第一个提供 POM 的仓库获胜。

测试覆盖：`CCB_MAVEN_REPOS`（逗号分隔的基础 URL 列表）会整体替换已配置的仓库。它的存在是为了让测试可以指向本地 `file://` 夹具仓库；在 v1 中，它不是添加私有仓库的支持方式。

## 打包类型：AAR 或 JAR

构件类型来自该构件 POM 的 `<packaging>`：

- `aar` → 作为 AAR 下载，与本地 AAR 完全一样处理（解压、`classes.jar` 进 classpath、`res/` 合并、manifest 合并）。
- `jar`（或缺失 `<packaging>`，Maven 默认即为 jar）→ 作为 JAR 下载，放到编译 classpath 和 dex 输入上。
- `pom` → 不下载构件；该 POM 自身的依赖仍会解析（BOM 风格 POM 就是这样处理的）。
- 其他任何值 → `CCB-E3019`，注明该坐标。

## 传递依赖

直接依赖和传递依赖都从 POM 的 `<dependencies>` 解析：

- 作用域：`compile`（包括缺失作用域，Maven 将其视为 compile）和 `runtime` 会解析。`test`、`provided`、`system`、`import` 会跳过，标记为 `<optional>true</optional>` 的依赖也会跳过。
- 版本冲突按最近者获胜解析：从直接声明出发按广度优先遍历依赖，某个 `group:artifact` 第一次出现的版本获胜。之后更深（或同深度、后声明）的版本会被跳过；使用 `--verbose` 时，跳过会打印为 `maven: <g:a>: requested <v>, using nearest <w>`。
- 循环安全：已见过的 `group:artifact` 永远不会重新入队。
- 属性：`${...}` 从同一 POM 的 `<properties>` 以及 `project.groupId` / `project.artifactId` / `project.version` 解析。依赖版本也可以来自同一 POM 的 `<dependencyManagement>` 条目。
- 父 POM：当 POM 带有 `<parent>` 块时，父 POM 会从同一仓库取回（与任何其他 POM 相同的缓存和校验和规则），并应用到子 POM 上，沿链递归向上。父 POM 的 `<properties>` 和 `<dependencyManagement>` 会合并进子 POM：子 POM 的条目获胜，父 POM 之间最近的祖先获胜；`${...}` 展开使用合并后的集合。省略 `groupId` 或 `version` 的 POM 会从其 `<parent>` 块继承这些值。链最多 10 个祖先；更长的链，或循环（父坐标重复链中更早的坐标），会以 `CCB-E3024` 失败，注明该链。不继承的内容：父 POM 自身的 `<dependencies>` 列表、profiles、父 POM 声明的排除项（会被忽略）。
- 排除项（`[deps] maven_exclude`）在最近者获胜之前检查：被排除的构件永远不会进入已见集合，因此被排除的版本永远不会获胜，同一构件的另一个版本可以从之后的路径获胜。被排除构件自身的传递依赖会随之剪掉：永远不会入队。使用 `--verbose` 时，每次跳过的排除项会打印为 `maven: <g:a:v>: excluded, skipping`。
- Profiles 会被整体忽略：profile 属性和 profile 依赖都不会解析。只在 profile 内声明依赖的 POM，其行为就如同没有该依赖。
- 经过 dependencyManagement 回退后仍没有版本的依赖，会以 `CCB-E3019` 失败，注明该依赖。

## 缓存与校验和

解析出的构件存放在专用的 Maven 缓存中，而不是扁平的下载缓存：

- 位置：`$XDG_CACHE_HOME/ccb/maven2`，否则为 `~/.cache/ccb/maven2`（`CCB_MAVEN_CACHE` 可覆盖）。布局与仓库一致：`<缓存>/<group 路径>/<artifact>/<version>/<artifact>-<version>.<扩展名>`，`.pom` 放在旁边。
- 校验和：下载时先校验已发布的 `.sha256`，否则校验已发布的 `.sha1`。不匹配会以 `CCB-E3020` 失败，注明该构件。
- 没有已发布校验和：下载会做结构校验（curl `--fail`、完整传输），并把它的 SHA-256 记录在 `<文件>.ccb-sha256` 边车文件中。之后的缓存命中会对照边车文件校验，因此损坏的缓存文件会以 `CCB-E3020` 失败，而不是悄悄毒化一次构建。
- 没有边车的缓存文件（手工放进去的，或旧版 CCB 留下的）会在首次使用时被信任并收编：它的 SHA-256 会被记录下来，供之后的命中使用。Explain 和 dry-run 模式永远不会写边车文件。

## 封闭性：setup 预取，构建永不下载

`ccb setup` 是唯一允许访问网络的命令（spec §6.1），Maven 也保持这一点：

- 在带有 `[deps] maven` 的项目中，`ccb setup` 会把每个直接 + 传递构件预取到缓存中（在 SDK 步骤之后、最终验证之前），并报告 `maven: prefetched N artifacts (A aar, J jar)`。预取失败会带着诊断停止 setup——半预取的项目永远不会被报告为就绪。预取会为每个解析出的 POM 取回完整的父 POM 链。
- `ccb build` 和 `ccb check` 只解析缓存。某个从未预取过的坐标会以 `CCB-E3021` 失败，注明该坐标并告诉你运行 `ccb setup`。失败发生在第 0 阶段，在任何构建阶段运行之前。
- 工作流：添加一个坐标，运行 `ccb setup`，然后离线构建。删除缓存（或在新机器上检出）只是意味着再运行一次 `ccb setup`。

`ccb check` 会在成功时报告解析出的集合：`maven: N artifacts (A aar, J jar)`。配置了排除项时，check 还会报告它们：`maven: excluded <g:a>, ...`（`--json` 输出中还有一个 `maven_excluded` 数组）。

## 锁文件（`ccb.lock`）

`ccb setup` 会在成功预取后把完整解析出的树写入项目根目录的 `ccb.lock`，`ccb check` / `ccb build` 在锁文件存在时强制执行它：

- 格式：每行一个坐标，按 `group:artifact` 排序，附带打包类型和该构件的 SHA-256：`com.example:lib:1.0 aar <sha256>`。注释行（`#`）会被忽略。对于 `pom` 打包类型（没有构件文件），记录的哈希是 POM 的哈希，因此改动过的 BOM 仍会使强制执行失败。
- 强制执行是精确的：解析出的版本、打包类型或哈希与锁文件不符会以 `CCB-E3023` 失败，注明该坐标；解析出的坐标在锁文件中缺失，或锁定的坐标不再能解析时也是如此。过期的锁文件永远不会悄悄解析出别的东西：运行 `ccb setup` 重新生成它（setup 会根据声明重新解析并覆盖该文件，即使存在过期的锁文件）。
- 没有锁文件意味着没有强制执行：锁文件之前的项目行为与之前完全相同。没有 `[deps] maven` 的项目永远不会写入或查阅锁文件，因此删除所有 Maven 坐标后，过期的 `ccb.lock` 会无害地留在原地。
- 锁文件是项目文件：提交它。它让一次检出可复现，而无需信任缓存：缓存按坐标键控，但锁文件固定了确切的树（包括传递版本）和每个构件的哈希。

## 限制（v1，特意如此）

- 仅确切版本；没有范围、快照，也没有超出最近者获胜的版本调解。
- 仅 Google Maven + 可选的 Maven Central；没有经过身份验证或私有的仓库（不存在凭据处理）。
- 父 POM 只贡献属性和 dependencyManagement：父 POM 自身的 `<dependencies>`、profiles 和父 POM 声明的排除项不会被继承。
- 没有签名（PGP）验证：只有校验和。

## 故障排查

代码含义：[docs/diagnostics.md](diagnostics.md)。修复方法：

- `CCB-E3021`：构件尚未进入本地缓存。运行 `ccb setup`，然后重新构建。父 POM 从缓存中被逐出时也一样：setup 会重新预取整条链。
- `CCB-E3024`：父链循环，或祖先超过 10 个。在 POM 中修复该链。
- `CCB-E3020`：下载的文件与仓库已发布的校验和不符。删除缓存条目（或整个 `<缓存>/<group 路径>/<artifact>/<version>/` 目录），重新运行 `ccb setup`；如果问题仍然存在，该仓库或镜像在提供错误的字节。
- `CCB-E3019`：坐标在 Google Maven（或已启用时的 Central）中不存在。检查拼写和版本，以及该构件是否确实在 Central 上（是的话设置 `maven_central`）。
- `CCB-E1005`（坐标）：声明的坐标格式错误或未固定。修复 `ccb.toml` 中的字符串。
- `CCB-E1005`（排除项）：排除项移除了项目直接声明的东西。删除该排除项或该直接坐标。
- `CCB-E3023`：解析出的树与 `ccb.lock` 不符（版本、打包类型、哈希、缺失或过期条目），或锁文件格式错误。运行 `ccb setup`，根据当前声明重新生成它。
