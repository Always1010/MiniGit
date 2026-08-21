# 代码导读

先看[原理图解](git-internals.md)，再选一条命令追踪“输入来自哪里、哪些数据会改变、失败留下什么”。公共接口在 `include/minigit/`，实现在 `src/core/`。

## 工程目录

```text
src/cli/               参数解析与输出
include/minigit/       核心公共接口
src/core/              数据模型、算法与业务实现
tests/unit/            Catch2 核心测试
tests/integration/     CMake CLI 集成测试
examples/experiments/  临时仓库学习实验
```

[main.cpp](../src/cli/main.cpp) 的 `main`／`run` 分派命令，统一处理输出与错误。[CMakeLists.txt](../CMakeLists.txt) 将核心库与 CLI 分开，[CMakePresets.json](../CMakePresets.json) 定义 Debug 和 sanitized。

命令参数解析与输出按用途归组，每个命令有独立的 `run_<command>` 函数：

| CLI 文件 | 命令 |
| --- | --- |
| [basic_commands.cpp](../src/cli/basic_commands.cpp) | help、version、init、hash-object、cat-file、add、config、commit、status |
| [inspection_commands.cpp](../src/cli/inspection_commands.cpp) | diff、show、log、reflog |
| [checkout_commands.cpp](../src/cli/checkout_commands.cpp) | switch、mv、rm、reset、restore、branch、tag |
| [history_commands.cpp](../src/cli/history_commands.cpp) | merge、rebase、cherry-pick／revert、gc、recover |
| [stash_commands.cpp](../src/cli/stash_commands.cpp) | stash 子命令与输出 |

[command_support.cpp](../src/cli/command_support.cpp) 只共享数量解析、历史操作参数和冲突输出。各命令保留自己的选项规则，入口使用直接分派，便于从命令名跳到实现。

## 按数据流阅读

| 主题 | 源码入口 | 重点 |
| --- | --- | --- |
| 摘要与封装 | [object_id.cpp](../src/core/object_id.cpp)、[objects.cpp](../src/core/objects.cpp) | 原始摘要与文本 ID；类型／长度／NUL 头 |
| 文件与仓库 | [fs.cpp](../src/core/fs.cpp)、[repository.cpp](../src/core/repository.cpp) | 路径检查、RepoLock、稳定读取与初始化 |
| 对象存储 | [object_store.cpp](../src/core/object_store.cpp) | zlib 保存、复用、读取校验 |
| 暂存 | [repo_path.cpp](../src/core/repo_path.cpp)、[index.cpp](../src/core/index.cpp)、[worktree.cpp](../src/core/worktree.cpp) | MGIX 编解码、`add_paths`、逐文件保存 |
| 快照与引用 | [snapshot.cpp](../src/core/snapshot.cpp)、[refs.cpp](../src/core/refs.cpp) | Tree 往返、未出生 HEAD、预期旧值校验 |
| 提交 | [history.cpp](../src/core/history.cpp) | `create_commit` 与 `store_commit_snapshot` 的发布边界 |
| 状态与差异 | [status.cpp](../src/core/status.cpp)、[diff.cpp](../src/core/diff.cpp)、[inspection.cpp](../src/core/inspection.cpp) | 三棵树、纯 EditScript、内容来源选择 |
| 历史与事件 | [dag.cpp](../src/core/dag.cpp)、[reflog.cpp](../src/core/reflog.cpp) | 全父图、拓扑顺序、引用事件规划 |

`add_paths`：枚举选择 → 保存 Blob → 更新 Index。注意后续选择失败时旧 Index 保留，但新 Blob 可能已存在。

`create_commit`：持锁 → 读身份／HEAD／Index → 构造 Tree 与 Commit → 规划引用／日志 → 执行事务。普通提交和存储工厂不能互相替代：后者不读取 Index、不移动 HEAD。

`repository_diff`／`show_object`：解析来源 → 读取对象或 Work → 纯格式化。未 add 的工作字节须直接读取，不能假设其摘要已存在于对象库。

## 安全更新与恢复

先看[worktree_update.hpp](../include/minigit/worktree_update.hpp) 的目标与授权，再看 [worktree_update.cpp](../src/core/worktree_update.cpp)、[transaction.cpp](../src/core/transaction.cpp)。前者规划实际节点，后者发布或恢复 before／after。

`plan_worktree_update` 的步骤是展开目标、观察并授权目录、构造改动、校验拓扑与组合预算、捕获真实备份。私有 `WorktreePlanner::inventory` 单独负责递归清点与未知子项保护。所有预算检查先于备份写入；最后才替换调用者的计划。

| 业务入口 | 重点 |
| --- | --- |
| [branches.cpp](../src/core/branches.cpp)、[tags.cpp](../src/core/tags.cpp) | 引用管理与安全删除，不改 Work |
| [switch.cpp](../src/core/switch.cpp) | A/B/C/D 兼容矩阵，组合 ref／Index／Work |
| [restore.cpp](../src/core/restore.cpp)、[reset.cpp](../src/core/reset.cpp) | 来源与目的独立、模式决定发布范围 |
| [remove.cpp](../src/core/remove.cpp)、[move.cpp](../src/core/move.cpp) | 内容保护、B／C 独立迁移、目的占用拒绝 |
| [checkout_support.cpp](../src/core/checkout_support.cpp) | 私有路径选择与对象检查；路径和 Index 不变量由模型层定义 |

检查 `TransactionPlan` 时区分 File、Directory 和 Absent；元数据 before 是精确原字节。Prepared 回真实现场，不能以旧 HEAD 快照代替它。

共享的节点镜像在 [worktree_image.hpp](../include/minigit/worktree_image.hpp)。事务实现按职责阅读：[transaction_journal.cpp](../src/core/transaction_journal.cpp) 只编解码 MGTR 字节与备份 ID；[transaction_metadata.cpp](../src/core/transaction_metadata.cpp) 检查白名单、载荷格式和 Stash 栈／引用配对；[transaction.cpp](../src/core/transaction.cpp) 加载备份、验证现场、准备 staging，再按 Prepared／Committed 发布或恢复。

## 合并、Stash 与 Rebase

按“纯算法 → 对象适配 → 历史策略 → 状态发布”阅读：

1. [merge_text.cpp](../src/core/merge_text.cpp)：base 坐标中的编辑交互、marker 和预算。
2. [merge_snapshot.cpp](../src/core/merge_snapshot.cpp)：模式、二进制、D/F 布局与原始 stages；只保存不可变对象。
3. [dag.cpp](../src/core/dag.cpp) 的 `find_merge_base`：先校验完整父图，再选择唯一 lowest。
4. [operation.cpp](../src/core/operation.cpp)：先读 `validate_operation_context`、`validate_operation_work`、`operation_encoded_size`，分别理解状态规则、Work 拓扑和编码预算，再看 codec、typed roots 与受约束更新；kind／phase／专属上下文规则列在 [operation.hpp](../include/minigit/operation.hpp)。
5. [operation_support.cpp](../src/core/operation_support.cpp)：真实冲突区域备份与共同 abort 规划。

| 业务源码 | 重点 |
| --- | --- |
| [merge.cpp](../src/core/merge.cpp) | 快进／两父提交、冲突发布、Index-only continue |
| [replay.cpp](../src/core/replay.cpp) | CherryPick／Revert 的基线方向、作者与空结果 |
| [stash_stack.cpp](../src/core/stash_stack.cpp)、[stash.cpp](../src/core/stash.cpp) | 全栈 roots、H/I/W/U、exact 栈字节 |
| [stash_restore.cpp](../src/core/stash_restore.cpp) | RW／RI 两次合并、栈绑定、完整成功后 pop |
| [rebase.cpp](../src/core/rebase.cpp) | `run_steps` 的 Ready／Conflicted 分派，随后提交本步、推进队列或 `finish`；`accumulate_work` 保留首次 before |

特别检查 Ready 的重试是否真的执行当前来源，以及文件变目录后累计备份是否仍可恢复原文件。

## GC

[gc.cpp](../src/core/gc.cpp) 按 `inventory → describe_graph → deletion_order → roots → mark → sweep` 阅读。全对象校验与拓扑预检必须先于删除；semantic roots 要展开备份容器内部 ID，freshness 要沿依赖闭包保留旧对象。

## 用测试核对理解

| 问题 | 对应测试 |
| --- | --- |
| 提交是否保存暂存 B，而非 Work C？ | [test_history.cpp](../tests/unit/test_history.cpp)、[commits_cli.cmake](../tests/integration/commits_cli.cmake) |
| 编辑脚本能否重放出完整新字节？ | [test_diff.cpp](../tests/unit/test_diff.cpp)、[test_merge_text.cpp](../tests/unit/test_merge_text.cpp) |
| 中断后恢复的是哪个现场？ | [test_transaction.cpp](../tests/unit/test_transaction.cpp) |
| Marker 是否混入原始 stages？ | [test_merge_snapshot.cpp](../tests/unit/test_merge_snapshot.cpp) |
| 独立暂存意图与 Work 是否都保留？ | [test_stash_restore.cpp](../tests/unit/test_stash_restore.cpp) |
| Ready 与 Conflicted 是否走不同继续路径？ | [test_rebase.cpp](../tests/unit/test_rebase.cpp) |
| Orphan 损坏是否先拒绝，新根是否保护旧依赖？ | [test_gc.cpp](../tests/unit/test_gc.cpp) |

可执行观察入口见[学习实验](learning-notes.md)，验证命令见[构建与测试](testing.md)。
