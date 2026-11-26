<!-- CCB tool docs for LLMs / AI agents. Human docs live in docs/ (English). This file is the Chinese version of docs/build-release.md. -->

# 发布二进制（静态 + stripped）

[文档索引](index.md) · [构建](build.md)

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
| Dev `build/ccb`（Debug，动态，未 stripped） | 1,151,424 |
| Dynamic Release，stripped（用于对比） | 531,208 |
| Static Release，未 stripped | 1,373,432 |
| Static Release，stripped（`build-release/ccb`） | 1,268,272 |

有时引用的约 605 KB 数字是 stripped 后的*动态*二进制；静态二进制约为 1.27 MB，因为链接了 glibc。无论如何，它都远低于 requirement.md §8 中的 5 MB 上限。验证方法：

```
file build-release/ccb   # statically linked, stripped
ldd build-release/ccb    # not a dynamic executable
```
