# Git 原理与 MiniGit 的选择

本文介绍 MiniGit 将要实现的模型。实现状态见 [进度](progress.md)，对象格式约束见 [架构](architecture.md)。真实 Git 的行为以本页链接的官方资料为参考，MiniGit 的自有格式不视为兼容性实现。

## 内容寻址与快照

内容寻址把对象完整内容映射为摘要，再用摘要检索对象。blob 保存文件字节，文件名和模式由 tree 保存；commit 连接根 tree、父提交和提交元数据。因而重命名文件通常改变 tree，文件内容不变时 blob 可以复用。真实 Git 的对象模型和松散对象封装见 [Git Objects](https://git-scm.com/book/en/v2/Git-Internals-Git-Objects)。

MiniGit 计划使用以下未压缩字节作为 SHA-256 输入：

```text
blob 3\0abc
```

这里 `\0` 表示一个 NUL 字节，不是反斜杠和数字两个字符；`3` 是 payload 的字节长度。对头部和 payload 一起求摘要，使类型和长度参与对象身份。随后压缩这串字节并写入对象库。读取过程先解压，再校验头部、长度和摘要。

文件的相同 payload 会得到相同 blob 摘要。两次提交即使文件内容一致，只要作者、提交者、业务时间、父提交或消息改变，commit 摘要仍可能改变。

## Tree 为什么是二进制结构

MiniGit 的计划条目编码为：

```text
mode SP basename NUL <32 bytes object id>
```

模式区分普通文件、可执行文件和目录。文件条目指向 blob，目录条目指向子 tree。文件名没有跨目录部分，因此 `src/main.cpp` 在根 tree 中表现为 `src` 子树，在子树中表现为 `main.cpp` 文件。

名称排序必须固定，否则同一个目录从不同遍历顺序构造，会得到不同 tree 摘要。MiniGit 将采用自己的明确字节排序规则；没有验证真实 Git 的特殊目录排序规则之前，不能将“排序一致”当作格式兼容的证据。

## 工作区、Index 与 HEAD

这三个状态回答不同问题：工作区是现在的文件，Index 是下一次提交准备记录的内容，HEAD 的根 tree 是当前提交已经记录的内容。

| 比较 | 含义 | 例子 |
| --- | --- | --- |
| HEAD → Index | 已暂存改动 | 文件从 `A` 暂存成 `B` |
| Index → 工作区 | 未暂存改动 | 暂存 `B` 后继续编辑成 `C` |
| 工作区路径不在 Index | 未跟踪文件候选 | 新建文件尚未 add |

因此 `add` 后继续编辑，再执行 `commit`，应提交暂存的版本。Index 不是“被跟踪文件名的清单”，而是一次待提交快照的持久化表示。真实 Git index 还含冲突阶段、统计信息及扩展等结构，详见 [gitformat-index](https://git-scm.com/docs/gitformat-index)；MiniGit 使用单独版本化的自有格式。

## 引用、提交图与恢复

分支是可移动的提交引用；轻量标签通常保持指向选定对象。HEAD 可符号指向分支，也可直接指向提交，后者称为分离 HEAD。提交通过 parent 构成有向无环历史，分支名不存入 commit 内容，见 [Git References](https://git-scm.com/book/en/v2/Git-Internals-Git-References)。

规划中的 merge 为两个分支找到共同祖先，比较 base、ours、theirs，得到结果 tree；产生新合并提交时记录两个 parent。cherry-pick 把一个提交相对其父提交的改变应用到当前历史，revert 则应用逆向改变。冲突状态需要持久化，才能在解决后继续或中止，不能只留下冲突标记而丢失操作上下文。

引用移动不等于删除对象。reflog 记录移动的线索，gc 根据根集合分析可达性，再遵守保留规则回收不可达对象。真实 Git 还有 packfile 和增量对象，见 [gitformat-pack](https://git-scm.com/docs/gitformat-pack)。MiniGit 首先使用松散对象，gc 的具体策略在 Phase 4 实现时定义。

## 与真实 Git 的差异

| 方面 | MiniGit 的目标 | 真实 Git 的参考 |
| --- | --- | --- |
| 哈希 | 固定 SHA-256，32 字节内部表示 | 同时涉及 SHA-1 与 SHA-256 仓库和迁移机制 |
| 仓库格式 | 自有格式和版本标识 | 生产级格式、扩展和兼容约束 |
| Index | 按学习需求设计的版本化格式 | 具有多版本、扩展与冲突阶段的格式 |
| 对象存储 | 首先只支持 zlib 松散对象 | 包含 packfile、增量和维护机制 |
| 路径与类型 | 首版普通 UTF-8 路径、二进制文件、可执行位；拒绝软链接 | 支持范围更广，具体行为受平台影响 |
| 命令 | 分阶段实现本地版本管理 | 包含网络传输、远程、钩子、子模块等大量功能 |

真实 Git 的 SHA-256 格式与转换包含对象引用及兼容映射等要求，单独替换哈希函数不足以实现兼容，见 [hash-function-transition](https://git-scm.com/docs/hash-function-transition)。MiniGit 不使用 Git 命令或其内部库代替自身业务算法。
