<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/modules.md. -->

# 库模块

项目可以在根 `ccb.toml` 中声明库模块。每个模块会被构建为中间 AAR，并通过现有的 AAR 机制消费：manifest 合并、res 在链接时合并、类被 dex、assets 被打包。app 模块是唯一的 APK/AAB 输出；库模块不产生 APK/AAB。

## 配置

```toml
[modules.base]
dir = "libbase"

[modules.core]
dir = "libcore"
depends = ["base"]
```

- `dir`（必填）：模块根目录，相对于项目根目录，除非是绝对路径。其下的标准布局与 `[paths]` 一致：`src/main/AndroidManifest.xml`（必填）、`src/main/res`、`src/main/assets`、`src/main/kotlin`、`src/main/java`（各为可选；缺失表示该模块没有此类输入）。
- `depends`（可选，默认 `[]`）：该模块依赖的其他模块名。每个声明的模块都会被构建并被 app 消费；没有单独的 "app 依赖于" 列表。
- 模块表中的未知键是硬错误（`CCB-E1001`）。

## 图规则（在加载时验证，`ccb check` 会报告它们）

- 模块按拓扑顺序构建，依赖先行。
- 依赖循环是硬错误，会注明循环路径（`CCB-E1010`，例如 `module dependency cycle: a -> b -> a`）。
- 依赖未声明的模块是硬错误，会注明该模块（`CCB-E1011`）。
- 模块 manifest 必须存在并声明 package；该 package 必须与 app 的 package 以及每个其他模块的 package 不同（`CCB-E1005`）。
- 模块名不得与已声明 AAR 的基名冲突（`CCB-E3022`，在 Maven 解析后的构建时检查，因为解压目录会互相覆盖）。

## 构建流程

流水线顺序：deps（已声明的 AAR）→ modules → merge-manifest → compile-res → link → code → dex → …。manifest 合并先看到已声明的 AAR，然后按拓扑顺序看到模块 AAR。合并规则见 [docs/build.md](build.md)（第 3 步）。

每个模块按拓扑顺序执行：

1. 指纹：工具版本、模块 manifest、res、assets、Kotlin 和 Java 源码、已声明的 jar、已声明的 AAR 类，以及上游模块输出（AAR + 生成的 R.java）。输入不变则跳过该模块。
2. 资源（仅当模块既有代码又有资源时）：`aapt2 compile` 编译模块 res，然后 `aapt2 link` 对照模块 manifest 生成该模块 package 的 R.java。链接输出（一次性的 `.ap_`）会被丢弃；只保留 R.java。
3. 代码：`kotlinc` 处理模块 Kotlin 源码、Java 源码、生成的 R.java 和上游模块的 R.java（模块可以引用依赖的 R 类），classpath = `android.jar` + 已声明的 jar + 已声明和已构建的模块类。随后 `javac` 按 app 的规范方式编译 Java 源码。没有 Kotlin/Java 源码的模块跳过此步。
4. `classes.jar`：除生成的 R 类（`R.class`、`R$*.class`）之外的每个已编译文件。app 链接会为该模块 package 生成真正的 R 类，同时交付两者会使 d8 因重复而拒绝。纯资源模块不贡献任何类。
5. 中间 AAR（`build/intermediates/modules/<name>/<name>.aar`）：`classes.jar`（如果存在）、`AndroidManifest.xml`（模块 manifest 的副本）、原始 `res/` 和 `assets/` 树。aapt2 在 app 链接时编译 res，与已声明的 AAR 完全相同。

## R 语义（写库代码前请先读）

库 R 字段是**非 final**的。模块链接生成 R.java，CCB 将其字段从 `public static final int` 改写为 `public static int`，app 链接再赋予最终的资源 ID。库代码以符号方式引用 R 字段，因此在运行时对照 app 的 R 类解析。内联库链接时的常量会交付错误的 ID。

推论：

- 库代码可以引用自身的 R 类及其依赖的 R 类。
- 不要在有资源的模块中把自己的类命名为 `R`：它会与生成的 R 类冲突。

## 增量构建

库模块的输出参与 app 模块的指纹，因此改动库会重建 app：

- 模块 res 经 compile-res（文件哈希）和 link（已编译的 zip + 合并指纹）流动。
- 模块 manifest 经合并指纹流动。
- 模块类经 app 代码指纹（哈希 classpath jar 的内容，而不只是路径）和 dex 指纹（哈希 classes jar）流动。

对库源码仅改注释只会重建该模块：其类字节不变，因此 AAR 哈希不变，依赖模块、app 代码和 dex 都会正确跳过。

## 限制（v1）

- 没有按模块的 `[deps]`、`[ndk]` 或签名小节：模块在 classpath 上使用项目的 jar/AAR，没有原生库。
- 没有动态功能模块（从 Q1 起已不在范围内）。
- 不支持将库模块发布到 Maven 仓库。
- 除了 aapt2 已有的功能（`--extra-packages` + `--auto-add-overlay`）之外，没有跨模块资源命名空间。
- app 模块不能按模块配置：所有声明的模块都会在每次构建中被构建和消费。
