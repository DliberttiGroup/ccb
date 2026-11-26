<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/diagnostics.md. -->

# 诊断代码

每次失败都带有一个稳定的代码。代码按阶段分组。退出码（spec §6.4）：0 成功，1 构建失败，2 配置错误，3 工具链缺失/版本错误，4 签名错误，5 设备错误。

人类可读模式的诊断无论严重程度如何，打印时都带有 `error[...]` 前缀：例如 CCB-P1017 这样的告警会在 stderr 上打印为 `error[CCB-P1017] ...`。请按下面表格中的诊断严重程度和退出码判断通过/失败，而不是按此前缀。

## 配置（退出码 2）

| 代码 | 含义 |
| ---- | ---- |
| CCB-E1001 | `ccb.toml` 中的未知键（硬错误；会注明键和表） |
| CCB-E1002 | 缺失必填的 `ccb_version` |
| CCB-E1003 | 不支持的 `ccb_version`（仅支持 1） |
| CCB-E1004 | 缺失必填键 |
| CCB-E1005 | 无效值（错误的变体、SDK 范围、min 大于 target、manifest package 不匹配、debug 下 minify，……） |
| CCB-E1006 | 声明的路径不存在 |
| CCB-E1007 | 键的值类型错误 |
| CCB-E1008 | TOML 解析错误（附行号） |
| CCB-E1009 | 配置文件未找到 |
| CCB-E1010 | 模块依赖循环（注明循环路径，例如 `a -> b -> a`） |
| CCB-E1011 | 模块依赖未知的模块（注明模块和该依赖） |

`ccb check` 会一次性报告所有配置问题，而不只是第一个。

## 工具链（退出码 3）

| 代码 | 含义 |
| ---- | ---- |
| CCB-E2001 | 未找到 Android SDK（设置 `[sdk] home` 或 `ANDROID_HOME`） |
| CCB-E2002 | 请求的 build-tools 版本未安装 |
| CCB-E2003 | 未找到请求的平台 `android.jar` |
| CCB-E2004 | 未找到 kotlinc（设置 `[sdk] kotlin_home` 或 `KOTLIN_HOME`） |
| CCB-E2005 | java 缺失或低于 17 |
| CCB-E2006 | kotlinc 不是 2.x |
| CCB-E2007 | 请求版本的 NDK 未安装 |
| CCB-E2008 | NDK 修订版本不匹配 |
| CCB-E2009 | 必需的工具二进制文件缺失或不可执行。也涵盖损坏的显式工具覆盖：`[sdk] aapt2` / `[sdk] zipalign` 键或 `CCB_AAPT2` / `CCB_ZIPALIGN` 环境变量指向缺失或不可执行的路径时，会报此代码，消息会注明覆盖来源和路径。不会静默回退到默认工具。 |

## 构建（退出码 1，除非另有注明）

| 代码 | 含义 |
| ---- | ---- |
| CCB-E3001 | 发布签名预检查失败（退出码 4） |
| CCB-E3002 | aapt2 compile 失败 |
| CCB-E3003 | aapt2 link 失败 |
| CCB-E3004 | kotlinc 失败 |
| CCB-E3005 | javac 失败 |
| CCB-E3006 | d8 失败 |
| CCB-E3007 | APK 打包失败（包括重复的原生库） |
| CCB-E3008 | zipalign 失败 |
| CCB-E3009 | 签名失败（退出码 4） |
| CCB-E3010 | apksigner verify 失败 |
| CCB-E3011 | aapt2 dump badging 失败 |
| CCB-E3012 | NDK 编译失败 |
| CCB-E3013 | NDK 链接失败 |
| CCB-E3014 | 请求了 r8，但未设置 `[sdk] r8_jar`（release + minify） |
| CCB-E3015 | AAR 解压失败 |
| CCB-E3016 | 无法创建或写入构建目录或文件 |
| CCB-E3017 | r8 失败 |
| CCB-E3018 | 合并期间 manifest 格式错误（注明相关 AAR 或 app manifest） |
| CCB-E3019 | Maven 解析失败：无法解析的坐标、格式错误或不支持的 POM，或在任何已配置仓库中找不到构件 |
| CCB-E3020 | Maven 校验和不匹配：下载或缓存的文件与已发布或已记录的校验和不符 |
| CCB-E3021 | Maven 构件不在本地缓存中；运行 `ccb setup` 预取它（构建永不下载） |
| CCB-E3022 | 库模块名与已声明 AAR 的基名冲突（它们的解压路径会互相覆盖） |
| CCB-E3023 | Maven 锁文件不匹配：解析出的树与 `ccb.lock` 不符（版本、打包类型、哈希、缺失或过期条目），或锁文件格式错误；运行 `ccb setup` 重新生成它 |
| CCB-E3024 | Maven 父 POM 链错误：链中有循环，或祖先超过 10 个，注明该链 |
| CCB-E3025 | Dex 检查失败：dex 引用了一个或多个它未定义的类（注明前 10 个，然后是 "+ N more"），或 dex 文件格式错误/被截断；构建在打包前停止 |

## 预览（退出码 1，除非另有注明）

| 代码 | 含义 |
| ---- | ---- |
| CCB-P1001 | 未知的 CSS 属性（警告；在 `--strict` / `[preview] strict` 下为错误） |
| CCB-P1002 | CSS 解析错误，保留部分解析结果（警告） |
| CCB-P1003 | 方框重叠（警告） |
| CCB-P1004 | 方框超出内部边界（警告） |
| CCB-P1005 | 导航到不匹配任何目标的位置：没有对应的屏幕、导航图目标和 manifest activity（错误） |
| CCB-P1006 | 不可达的屏幕，没有任何导航指向它（警告） |
| CCB-P1007 | 空屏幕，没有组件（警告） |
| CCB-P1008 | 重复的屏幕名，第 2 次及之后的出现（错误） |
| CCB-P1009 | 文本被裁剪到其方框内（警告） |
| CCB-P1010 | Flow 解析错误，保留部分解析结果（警告） |
| CCB-P1011 | 未找到屏幕（警告） |
| CCB-P1012 | 未知的 Kotlin 结构，保留为通用方框（警告） |
| CCB-P1013 | 帧宽度被钳制到最少 24 列（警告） |
| CCB-P1014 | CSS 值超出子集，例如不支持的单位或 display 值（警告；在 `--strict` / `[preview] strict` 下为错误） |
| CCB-P1015 | CSS 文件被忽略：既不是 `common.css`，也不是项目中某个屏幕的 `<Screen>.css` 文件（警告） |
| CCB-P1016 | 未解析的 `@string/` 或 `@color/` 引用，或格式错误的 values 条目 / 被跳过的非十六进制颜色值（警告；保留字面量） |
| CCB-P1017 | 点按目标过小：`button` / `textfield` 小于 8 列或 2 行（警告，始终）。完整规则：[docs/preview-styles.md](preview-styles.md) |
| CCB-P1018 | 文本对比度低于阈值：WCAG 比率低于 4.5:1，或 `font-size` ≥ 24px 时低于 3.0:1（警告，始终）。完整规则：[docs/preview-styles.md](preview-styles.md) |
| CCB-P1019 | 找到处理函数，但按钮未绑定：点击处理函数的接收者没有 `[preview.bindings]` 条目（警告，始终；不记录边） |
| CCB-P1020 | 声明的导航与同一按钮的派生边不一致；声明的边保留，派生的边被丢弃（警告，始终） |
| CCB-P1021 | XML `android:onClick` 按钮在任何屏幕中都找不到：绑定的组件标签没有出现在任何屏幕的组件树中，因此其派生边无处可附着（警告，始终；不记录边） |
| CCB-P1022 | 场景条目与项目不匹配（警告，始终；场景永远不会使构建门禁失败） |
| CCB-P1023 | 追踪步骤与项目不匹配（警告，始终；追踪永远不会使构建门禁失败） |
| CCB-P1024 | 歧义的函数引用：点击处理函数的方法引用或辅助调用命名了在多个 flow 文件中定义过的顶层 fun，因此不会为它记录任何边——该引用永远不会被猜测。每个 (file, name) 只警告一次（警告，始终；永远不会使构建门禁失败） |
| CCB-P1025 | 未归属的导航：flow 文件中的导航调用没有被任何解析记录为边——位于嵌套块内、被跳过的语句中，或从按钮扫描时从未扫描过的辅助函数中。报告时会附带 file:line，因此它永远不会被悄悄丢弃；永远不会凭空造出一条边。已被 CCB-P1019（未绑定的处理函数）或 CCB-P1024（歧义引用）报告过的导航不会在这里重复（警告，始终；永远不会使构建门禁失败） |

预览警告的退出码为 0；只有 CCB-P1005、CCB-P1008，以及（在 `--strict` 下）CCB-P1001/CCB-P1014 的退出码为 1。CCB-P1016–P1025 即使在 `--strict` 下也是警告，因此永远不会使构建门禁失败。没有可用 `[preview]` 小节的项目根本不是诊断：`ccb preview` 会打印 "preview not applicable to this project" 并以 0 退出。

## 设备（退出码 5）

| 代码 | 含义 |
| ---- | ---- |
| CCB-E5001 | adb 命令失败或没有连接设备（见 docs/device-setup.md） |
| CCB-E5002 | 没有可安装的 APK（先运行 `ccb build`） |

工具失败会包含失败的确切命令及其 stderr 尾部。密钥库密码永远不会包含在内：发布签名仅以 `env:VAR` 引用的形式将其传递给 apksigner。
