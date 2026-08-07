# 构建与测试

需要 CMake 3.20+、C++20 编译器、OpenSSL 3、zlib、Catch2 v3，依赖由环境提供。

## 构建预设

| 预设 | 用途与选项 |
| --- | --- |
| `debug` | 未优化调试构建，适合逐行阅读 |
| `sanitized` | `-g -O1`、ASan／UBSan、帧指针；Linux 使用非 PIE |

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug -R '^cli_basics$' --output-on-failure

cmake --preset sanitized
cmake --build --preset sanitized
ctest --preset sanitized -R '^cli_basics$' --output-on-failure
```

Sanitized 保留调试符号，优化可能改变单步顺序和局部变量可见性。可在构建目录 `compile_commands.json` 核对选项。

## 选择检查范围

普通改动只运行受影响模块和 CLI；阶段验收或影响整个构建的改动运行完整回归。CTest 注册总数与实际执行数分开记录，中途取消不算通过。

例如修改 Rebase 时筛选其业务、状态与 CLI 测试：

```sh
ctest --preset debug -R '^(unit\.(Rebase|OperationRebase)|rebase_cli$)' --output-on-failure
```

这只是筛选示例；涉及共享事务、工作规划器等模块时，需包含它们及调用方。

完整回归：

```sh
ctest --preset debug --output-on-failure
ctest --preset sanitized --output-on-failure
```

文档改动检查本地链接、标题锚点、代码围栏、图示和 `git diff --check`，不需要运行完整 C++ 回归。学习脚本可用 `bash -n` 检查语法，运行方式见[学习实验](learning-notes.md)。

单元测试关注格式、算法和失败后状态，CLI 测试检查输出、退出码与磁盘结果。预算用例保留真实输入；编译成功不能替代行为检查。已有结果集中在[开发进度](progress.md)。
