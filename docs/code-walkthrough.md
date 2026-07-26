# 代码导读

Phase 0 已完成。当前代码只包含构建基础、版本函数和 CLI 帮助及版本处理；下面的业务模块阅读路线是计划。完成和验证状态见 [进度](progress.md)。

## 从一次命令开始

从 [src/cli/main.cpp](../src/cli/main.cpp) 的 `main` 开始：仅接受一个参数，`--help` 和 `help` 输出帮助，`--version` 调用核心库的版本函数。成功返回 `0`，空参数、未知参数或多余参数写入标准错误并返回 `2`。这些路径不访问工作区，也不创建仓库。

版本函数声明位于 [include/minigit/version.hpp](../include/minigit/version.hpp)，实现在 [src/core/version.cpp](../src/core/version.cpp)，当前返回 `0.1.0`。这个小边界验证了 CLI 与核心库的连接，为后续业务模块提供可构建起点。

构建定义在 [CMakeLists.txt](../CMakeLists.txt)，预设在 [CMakePresets.json](../CMakePresets.json)。`minigit_core` 为 C++20 库，CLI 链接它；OpenSSL 3 和 zlib 已接入，SHA-256 与压缩业务代码尚待实现。Catch2 v3 已进行依赖检测，当前的 CLI 集成测试由 [tests/integration/cli_basics.cmake](../tests/integration/cli_basics.cmake) 通过 CTest 执行。

Debug 工作流：

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Debug 和 sanitized 的 `cli_basics` 均已通过，验证帮助输出、版本格式、空参数、未知参数和多余参数。`help` 别名已实现，但当前测试脚本没有单独执行该别名。具体证据由 [进度](progress.md) 记录。

## 业务实现后的阅读顺序

| 顺序 | 计划模块或类型 | 阅读时要回答的问题 |
| --- | --- | --- |
| 1 | 错误类型与文件工具 | 错误是否带上下文？读取失败与格式损坏能否区分？ |
| 2 | `ObjectId` | 如何区分固定 32 字节和 64 位显示字符串？非法输入在哪里拒绝？ |
| 3 | 对象编码与 `ObjectStore` | 哈希的确切输入是什么？读取如何验证长度、类型和摘要？ |
| 4 | Blob / Tree / Commit | 二进制如何保留？排序和身份元数据如何影响确定性？ |
| 5 | 仓库发现与 `init` | 如何识别仓库？重复初始化和半成品目录如何处理？ |
| 6 | Index 与 `add` | 暂存的是哪个版本？路径越界和软链接如何拒绝？ |
| 7 | Refs / HEAD 与 `commit` | 空历史怎样表示？引用何时更新？中途失败留下什么？ |
| 8 | `status`、`log`、轻量 `tag` | 比较哪些状态？历史如何遍历？引用如何校验？ |
| 9 | Diff、Worktree、Operations | 恢复、覆盖保护和继续/中止怎样跨进程保持？ |

表中名称是架构职责和计划类型，待实现后替换为对应真实源码链接。依赖关系见 [架构](architecture.md)。

## 跟踪一份文件的版本

Phase 1 验收后，选择一个含多行文本的普通文件，在测试仓库依次完成创建、add、修改、status 和 commit。阅读时同时记录工作区字节、Index 指向的 blob 和 HEAD tree 中的 blob，不只观察 CLI 提示。

当工作区从 `A` 变为 `B`，add 暂存 `B`，随后又编辑成 `C`，一次 commit 应记录 `B`。若源码在 commit 时重新读取文件并存为 `C`，就破坏了暂存区语义。相应测试应比较对象内容，而不是只检查命令返回成功。

随后为同一目录改变文件遍历顺序，检查 tree 字节是否一致；向对象文件注入截断或错误数据，检查读取是否拒绝。通过不变量追踪比逐行阅读压缩库调用更容易理解边界。

## 验证层次

- 单元测试：摘要编码、对象头解析、zlib 往返、tree 顺序、commit 编解码、Index 格式。
- 集成测试：在临时仓库组合仓库发现、对象写入、Index 与引用更新。
- 端到端测试：从 CLI 验证退出码、输出和真实磁盘状态，测试结束清理临时目录。

测试名称应描述外部行为或数据不变量。需要构造损坏数据时保留最小反例，避免用和实现相同的算法生成预期结果。安全更新机制引入后，对失败边界和未提交内容覆盖保护补充有针对性的集成用例。
