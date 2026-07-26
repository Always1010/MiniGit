# MiniGit

MiniGit 是使用 C++20 编写的简化本地版本管理器，用于通过可阅读的实现学习 Git 的对象模型、暂存区、引用、历史和合并。项目采用自己的仓库格式，不承诺与 Git 仓库互读；网络协议、远程仓库和托管服务不在范围内。

Phase 0 构建与 CLI 基础已完成，当前版本为 `0.1.0`。Phase 1 的首个子项加入仓库初始化和内容寻址对象库，验证状态见 [进度](docs/progress.md)。`add`、`commit`、`status`、`log`、`tag` 仍在规划中，Phase 1 尚未整体验收。

## 构建与运行

需要 CMake 3.20 或更高版本、支持 C++20 的编译器、OpenSSL 3、zlib 和 Catch2 v3。CLI 集成检查通过 CMake 脚本注册到 CTest，核心单元测试使用 Catch2。预设负责配置项目，依赖由环境提供，不在构建过程中隐式安装系统软件。

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
./build/debug/minigit --help
./build/debug/minigit --version
```

Debug 与 `sanitized` 预设的 CLI 基础检查均已在 Linux 通过；后者启用地址和未定义行为检查。详细验证范围见 [进度](docs/progress.md)，其他平台尚未验证。

## 当前 CLI

| 命令 | 行为 |
| --- | --- |
| `--help`、`help` | 显示帮助 |
| `--version` | 显示版本 |
| `init [directory]` | 在指定目录或当前目录初始化 `.minigit`，重复初始化保留已有数据 |
| `hash-object [-w] file` | 对普通文件字节计算 blob 摘要；`-w` 将对象写入当前仓库 |
| `cat-file -t full-id` | 显示对象类型 |
| `cat-file -p full-id` | 输出 blob 或 commit 内容，tree 按条目显示 |

对象 ID 必须为完整 64 位十六进制文本，尚不支持缩写。不带 `-w` 的摘要计算可以在仓库外执行；写入和读取需要当前目录或祖先存在有效 `.minigit` 仓库。当前文件读取上限为 64 MiB，软链接和非普通文件被拒绝。

构建后可运行 [对象观察实验](examples/experiments/objects.sh)：

```sh
bash examples/experiments/objects.sh ./build/debug/minigit
```

脚本创建临时仓库，保存 blob 后修改工作区，再读取并比较旧对象内容，结束时自动清理。该实验不调用系统 Git。测试结果以 [进度](docs/progress.md) 为准。

## 设计范围

文件内容以字节保存，支持二进制文件。首版处理普通 UTF-8 相对路径和普通文件可执行位，拒绝软链接。对象使用 SHA-256，摘要通过 OpenSSL EVP 计算，松散对象通过 zlib 压缩。路径检查、仓库元数据隔离、确定性序列化和损坏检查属于核心约束。

| 阶段 | 目标 |
| --- | --- |
| Phase 0 | CMake、核心库与 CLI 分层、依赖连接、帮助和版本、测试入口 |
| Phase 1 | `init`、`add`、`commit`、`status`、`log`、轻量 `tag`；对象、暂存区、引用形成闭环，写锁与原子元数据更新 |
| Phase 2 | `diff`、`show`、`branch`、`switch`、`restore`、`reset`、`rm`、`mv`；多文件事务、恢复和 reflog |
| Phase 3 | `merge`、`cherry-pick`、`revert`、`stash`；需要继续或中止的操作同时提供相应恢复入口 |
| Phase 4 | `rebase`、`gc`，完成复杂历史编辑和对象回收 |

表中列出整个阶段的目标，当前 CLI 表列出本子项实现范围；阶段完成必须有验收证据，不能仅由命令名称存在判定。

## 阅读路线

1. [架构](docs/architecture.md)：模块职责、依赖方向、数据不变量与数据流。
2. [Git 原理](docs/git-internals.md)：内容寻址、快照、三棵树、引用与真实 Git 的差异。
3. [代码导读](docs/code-walkthrough.md)：从 CLI 到存储的阅读顺序和验证方法。
4. [学习笔记](docs/learning-notes.md)：可执行的观察实验、取舍与常见误区。
5. [进度](docs/progress.md)：完成状态、验证证据、阶段验收与下一步。

涉及暂存、提交或合并的 MiniGit 工作流只有在对应阶段实现并验收后才可执行。需要体验完整 Git 功能时，请使用真实 Git。
