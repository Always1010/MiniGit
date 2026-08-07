# 学习实验

从项目根运行。脚本会将二进制路径转为绝对路径，创建临时仓库并在退出时清理；直接调用 MiniGit。

```sh
bash examples/experiments/objects.sh ./build/debug/minigit
```

替换脚本名即可选择下表实验；sanitized 构建使用 `./build/sanitized/minigit`。建议先看[原理图解](git-internals.md)，带着“HEAD／Index／Work 各保存什么”观察输出。

## 对象与快照

| 实验 | 观察重点 |
| --- | --- |
| [objects.sh](../examples/experiments/objects.sh) | 同内容复用 ID；Work 编辑不改变已保存 Blob |
| [commits.sh](../examples/experiments/commits.sh) | 暂存 B 后编辑 C，提交仍保存 B；parent 与固定标签 |
| [diff.sh](../examples/experiments/diff.sh) | A→B、B→C、A→C 三种比较的来源 |
| [show.sh](../examples/experiments/show.sh) | `HEAD:path` 与 `:path` 分别读取历史和暂存；二进制字节核对 |

![三棵树与内容来源](images/three-trees.svg)

练习：暂存后再次编辑，比较 `show :note` 和实际文件。再提交，解释为什么工作区还能显示未暂存变化。不要用 Shell 变量保存含 NUL 的输出，应重定向后用 `cmp` 检查。

## 引用与路径更新

| 实验 | 观察重点 |
| --- | --- |
| [reflog.sh](../examples/experiments/reflog.sh) | 引用事件与提交 parent 图各记录什么 |
| [branches.sh](../examples/experiments/branches.sh) | 创建分支不切换内容；引用固定与安全删除 |
| [switch.sh](../examples/experiments/switch.sh) | 兼容修改保留、不兼容修改拒绝、detached HEAD |
| [restore.sh](../examples/experiments/restore.sh) | 相同来源分别恢复 Index 或 Work，HEAD 不动 |
| [reset.sh](../examples/experiments/reset.sh) | soft／mixed／hard 的状态更新范围 |
| [remove.sh](../examples/experiments/remove.sh) | 普通／cached／force 怎样保护仅在 Index 中的版本 |
| [move.sh](../examples/experiments/move.sh) | Index B 与 Work C 迁移后仍为两份内容 |

练习：比较“先 mv 再 add”与 MiniGit `mv`。后者为何能保留原来的暂存版本？再向目录加入未跟踪文件，观察目录迁移为何拒绝。

## 冲突与重放

| 实验 | 观察重点 |
| --- | --- |
| [merge.sh](../examples/experiments/merge.sh) | 原始 stage 1／2／3、marker、resolved Index 与中止备份 |
| [replay.sh](../examples/experiments/replay.sh) | 正向／反向变化、原 author／当前 committer、单父空重放 |
| [rebase.sh](../examples/experiments/rebase.sh) | 两步新链、最终移动分支、冲突继续与中止 |

练习：在冲突解决并 add 后再次编辑 Work。Continue 为什么提交 Index？Rebase 还有下一步时，为何未暂存编辑又可能阻挡它？结合[恢复图解](architecture.md#冲突与恢复)区分当前冲突完成与下一来源启动。

## Stash 与 GC

| 实验 | 观察重点 |
| --- | --- |
| [stash.sh](../examples/experiments/stash.sh) | 独立保存 H／I／W／U 与位置栈 |
| [stash-restore.sh](../examples/experiments/stash-restore.sh) | apply／pop、独立 `--index`、冲突时栈保留 |
| [gc.sh](../examples/experiments/gc.sh) | 默认保留窗口、orphan 清理、日志／全栈／操作备份保护 |

练习：保存 Index=B、Work=C 后恢复。为什么默认 apply 和 `apply --index` 的 Index 不同？再想一个新 Commit 指向旧 Tree／Blob：如果 GC 只保留新文件，会留下什么问题？

## 中断事务

事务实验使用独立教学程序，通过公开测试钩子在持久化步骤后退出：

```sh
cmake --build --preset debug --target minigit_transaction_lab -j 2
bash examples/experiments/transactions.sh ./build/debug/minigit ./build/debug/minigit_transaction_lab
```

[transactions.sh](../examples/experiments/transactions.sh) 检查 a 已更新、b 尚未更新时查询被阻挡，再通过真实 CLI recover 回到原内容并重复恢复。[教学程序](../examples/experiments/transaction.cpp) 只接受空目录。

练习：操作前 Work=C、旧 HEAD=A，为什么 rollback 应回 C？这些实验验证持久化步骤边界，系统调用内部中断的限制见[架构](architecture.md#资源与平台)。

## 常见误区

- 文件名在 Tree，字节在 Blob；Commit 保存快照入口与父关系。
- 文件快照相同不意味着 Commit 相同，元数据也参与摘要。
- Marker 不是冲突的原始 stages，Work 不是暂存结果。
- 单文件原子替换不保证整条命令的多文件更新一起完成。
- 删除分支不立即删除对象；GC 还要检查日志、栈和备份的语义根。

源码与对应反例见[代码导读](code-walkthrough.md)，已有验收结果见[开发进度](progress.md)。
