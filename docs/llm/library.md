<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/library.md. -->

# 独立库项目

[文档索引](index.md)

项目可以是库而不是应用：它构建出一个 `.aar`，而不是 APK。应用通过本地路径（库项目目录或构建好的 `.aar` 文件）或 Maven 坐标来消费库。树内的 `[modules.<name>]` 库是另一回事——见 [modules.md](modules.md)：它们位于应用项目内部，而独立库是拥有自己的 `ccb.toml` 的独立项目。

## 声明库项目

```toml
[project]
name = "orbit"
package = "orbit"
version = "0.15.9"
# min_sdk / target_sdk / compile_sdk 与往常一样

[paths]
manifest = "src/main/AndroidManifest.xml"
kotlin = "src/main/kotlin"
# res = "src/main/res"（可选）

[library]
```

- `[library]`（空表）把项目标记为库。它下面的任何键都是硬错误（`CCB-E1001`）。
- `[project]` 的 name、package 和 version 是 AAR 的身份：构建会写入 `build/<name>-<version>.aar`。
- `[paths]` 的 manifest 是必填的；`res`、`assets`、`kotlin`、`java` 是可选的，位于 `src/main/` 下的标准布局。
- 库项目不能声明 `[modules.<name>]`、`[ndk]`、`[deps]` 的 jars/aars/maven，也不能声明自己的路径库（`CCB-E1005`）：它使用消费应用的工具链和 classpath 构建。它不能构建 AAB（`CCB-E1005`）。
- 在库项目中运行 `ccb build` 会先跑 deps，然后做单模块构建（资源、代码、AAR），然后停止：不做合并、链接、dex 或打包。`ccb build --explain` / `--dry-run` 同样这样规划。

## AAR 内容

- `classes.jar`：已编译的 Kotlin/Java 代码，生成的 R 类除外（`R.class`、`R$*.class`）。消费应用的链接会为该库 package 生成真正的 R 类；同时交付两者会使 d8 因重复而拒绝。
- `AndroidManifest.xml`：库 manifest 的副本。
- `res/` 和 `assets/`：原始树，在消费应用链接时编译。
- `R.txt`：该库链接的符号表（每个符号一行 `int string name 0x...`）。没有资源的库会交付一个空的 `R.txt`。每个 AAR 都带有该文件，包括中间模块 AAR。

库 R 字段是非 final 的（见 [modules.md](modules.md)）：库链接生成 R.java，CCB 对其去 final 化，应用链接再赋予最终 ID。库代码以符号方式引用 R 字段。

## 通过路径消费库

```toml
[deps.library.orbit]
path = "../orbit"
```

- `path`（必填）：库项目目录，或构建好的 `.aar` 文件。
- 项目目录必须包含一个声明了 `[library]` 的 `ccb.toml`（在加载时仅做最小读取：不做递归配置加载，因此库循环无法递归）、一个 `[project]` name/version/package，以及一个 package 与 `[project] package` 一致的 manifest。不是库项目的目录是硬错误（`CCB-E1005`）；缺失的路径是 `CCB-E1006`；缺失 `path` 键是 `CCB-E1004`。
- `.aar` 文件在构建时（deps 步骤）加入已声明的 AAR；项目目录会先构建为 `build/intermediates/libraries/<name>/<name>.aar`，然后通过与已声明 AAR 和模块相同的 AAR 机制消费。
- 路径库按声明顺序构建，位于 deps 和 modules 之间。后面的库可以在其 kotlinc classpath 上看到前面库的 `classes.jar`，但看不到它们的 R.java：路径库之间是独立的（它们之间没有 `depends`）。
- 库名不得与模块名冲突（`CCB-E1005`），也不得与已声明 AAR 的基名冲突（`CCB-E3022`，在构建时检查，因为解压目录会互相覆盖）。
- 库 package 必须与应用 package、每个模块 package 以及每个其他库 package 都不同（`CCB-E1005`）。

## 通过 Maven 消费库

通过 Maven 解析得到的 AAR 依赖与本地 AAR 的工作方式相同：见 [maven.md](maven.md)（打包类型 `aar`）。解析器的 AAR 会在第 0 阶段加入已声明的 AAR，因此 manifest 合并、res 合并、classpath 和 dex 都把它们当作本地文件处理。`ccb setup` 会预取它们；构建永不下载。

## 资源合并与命名空间

- 库 `res/` 在链接时与应用 `res/` 合并。应用会在冲突中获胜：链接会把库 zip 作为 aapt2 `-R` 覆盖层传入，按反向合并顺序排列（因此库之间第一个库获胜），应用 zip 最后。没有这一点，同名的应用/库资源会是硬链接错误。
- R 符号按 package 命名空间区分：应用链接会把每个库 package 作为 `--extra-packages` 传入，因此每个库都有自己的 R 类（`com.example.lib.R`），不会与应用的 R 类冲突。
- 一个资源由多个来源定义时，会在链接时发出警告，注明获胜者和被遮蔽的来源：`resource 'string/shared' defined by app and library orbit; app wins`。扫描只按 (type, name)：带限定符的变体（`values-night`）算作同一条目，因此该警告意味着获胜者的定义在两者都适用的地方生效。警告只在链接运行时打印，缓存跳过的链接永远不会打印。
- Manifest 合并与已声明 AAR 的相同：见 [build.md](build.md)（第 4 阶段）。库组件会被保留；名称冲突时应用 manifest 的条目获胜。

## `ccb check` 报告库

对于库项目，`check` 会打印该库及其 AAR：

```
  library: mylib 1.2 (com.example.mylib)
    aar: build/mylib-1.2.aar (classes.jar, 1 res files, 2 R symbols)
```

对于应用，每个消费的库（路径、Maven、已声明 AAR）各有一个块，附带 AAR 内容摘要（classes.jar 是否存在、res 文件数、R.txt 符号数）。尚未构建的路径库会报告 `(not built yet)`。`--json` 以 `"library"` 对象（库项目）或 `"libraries"` 数组（应用）携带相同的数据，每个条目包含 `name`、`version`、`package`、`source`（`path`/`file`/`maven`）、`ref`、`aar`、`built`、`classes`、`res_files`、`r_symbols`。

## 限制（v1）

- 没有按库的 `[deps]`、`[ndk]` 或签名：库在 classpath 上使用应用的 jar/AAR，没有原生库。
- 不支持把库项目发布到 Maven 仓库（`ccb` 构建出 `.aar` 文件；发布它不在范围内）。
- 路径库之间没有 `depends`，没有传递路径库（库项目不能消费路径库）。
- 没有动态功能模块。
