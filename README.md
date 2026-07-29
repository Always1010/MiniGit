# MiniGit

MiniGit 是使用 C++20 编写的简化本地版本管理器，用于通过可阅读的实现学习 Git 的对象模型、暂存区、引用、历史和合并。项目采用自己的仓库格式，不承诺与 Git 仓库互读；网络协议、远程仓库和托管服务不在范围内。

Phase 0 构建与 CLI 基础已完成，当前版本为 `0.1.0`。Phase 1 的本地快照闭环已验收，Debug 与 sanitized 各 54 项检查全部通过；Phase 2 正在逐项推进，已形成差异引擎、diff/show 内容来源协调和短事务恢复机制；reflog 与 commit 的事务发布也已通过子项验收；branch 列表、创建及删除已通过子项验收；共享工作区与 Index 更新规划器核心实现已完成。详细范围与后续分支、工作区更新、合并及历史重写验收见 [进度](docs/progress.md)。

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
| `add [--] paths...` | 暂存指定文件、目录内的修改和删除；路径相对于当前目录 |
| `add -A`、`add --all` | 暂存整个仓库的新增、修改及删除，不与路径参数组合 |
| `config user.name [value]`、`config user.email [value]` | 读取或设置仓库本地提交身份 |
| `commit -m message [--allow-empty]` | 将 Index 保存为 Tree 和 Commit，并移动当前引用 |
| `status [--porcelain]` | 显示分支及三棵树比较；`--porcelain`、`--short`、`-s` 仅显示状态行 |
| `diff [--cached] [-U count] [revision [revision]]` | 默认比较 Index→工作区；已暂存、修订→工作区及双修订比较见下文 |
| `show [--] [spec]` | 默认检查 HEAD；提交、原始对象、提交内文件与 Index stage 选择见下文 |
| `log [--oneline] [-n count] [revision]` | 沿全部 parent 展示去重历史，默认从 HEAD 开始 |
| `reflog [--] [HEAD\|branch\|refs/heads/branch]` | 只读展示本地 HEAD 或分支移动事件，默认 HEAD，最新事件在前 |
| `recover` | 按短事务的 Prepared／Committed 状态恢复，重复执行安全 |
| `tag`、`tag [-f] name [revision]`、`tag -d name` | 列出、创建／强制移动或删除指向 Commit 的轻量标签 |
| `branch`、`branch [-f] [--] name [revision]` | 列出分支，或从修订创建／强制移动分支，默认来源 HEAD |
| `branch -d [--] name`、`branch -D [--] name` | 安全删除／强制删除非当前分支及其分支日志 |

对象 ID 必须为完整 64 位十六进制文本，尚不支持缩写。不带 `-w` 的摘要计算可以在仓库外执行；写入和读取需要当前目录或祖先存在有效 `.minigit` 仓库。当前文件读取上限为 64 MiB，软链接和非普通文件被拒绝。

`add` 立即保存文件字节并更新 `.minigit/index`，之后继续编辑工作区不会改变暂存内容。仓库根的 `.minigitignore` 支持注释、普通路径、目录尾斜杠和 `*`、`?` 模式；无斜杠规则匹配任意层的名称，路径规则相对于根目录。不支持 `!` 和 `**`，遇到这些语法明确报错。忽略规则不移除已跟踪路径，明确添加被忽略的新文件会失败。

轻量标签只是 `refs/tags/name` 中保存的提交 ID，后续 commit 不会移动它。重复名称默认失败，只有 `-f` 明确移动；尚未提交的 HEAD 不能创建默认标签。标签名称遵循引用路径约束，保留 HEAD、refs/ 前缀及完整十六进制 ID，以免与修订解析歧义。当前不支持附注标签或指向 Blob 的标签。

`branch` 按名称列出短名与完整提交 ID，当前符号分支标 `*`，未出生分支显示 `(unborn)`；分离 HEAD 时没有分支被标为当前。创建默认从 HEAD 取目标，未出生 HEAD 不能提供默认目标。创建或强制移动只更新分支引用和它的 reflog，不切换 HEAD，也不改 Index 或工作文件。分支短名禁止 HEAD、refs/ 前缀和纯 64 位十六进制 ID，并遵循严格引用名称规则；可用 `branch -- -topic` 传入选项样式名称。

已有分支默认拒绝覆盖，`-f` 明确移动；`-f` 只用于创建／移动，不与 `-d`、`-D` 组合，强制删除使用 `-D`。当前符号分支始终拒绝覆盖或删除，即使目标 ID 相同。非当前分支强制移动到原 ID 时，验证目标后不追加日志。`-d` 要求该分支提交位于当前 HEAD 的完整 parent 闭包内；多父图会完整校验，不能只查第一父链。`-D` 跳过这项可达性要求，仍校验原分支快照、日志格式与日志引用的 Commit。删除同时移除分支 ref 与分支日志，HEAD 日志保留；尚无永久保存已删分支日志的功能。

首次提交前设置显式身份。身份单独保存在 `.minigit/identity`，不改写仓库格式标识：

```sh
minigit init demo
cd demo
minigit config user.name Learner
minigit config user.email learner@example.test
printf 'hello\n' > note.txt
minigit add note.txt
minigit commit -m '记录首个快照'
```

将 `minigit` 换成构建产物的绝对路径，或自行将其加入可执行文件搜索路径。提交时采用运行时 UTC 业务时间，不从本项目的开发提交日期取得时间。默认拒绝空快照首提交及没有快照变化的提交，`--allow-empty` 明确允许；未解决的 Index 冲突仍被拒绝。

`status` 的状态行采用 `XY path`：X 比较 HEAD 与 Index，Y 比较 Index 与工作区；`A/M/D` 表示新增／修改／删除，`??` 表示未跟踪。`MM note` 表示已暂存的修改之后又有工作区修改。路径含空格或控制字符时使用可读引号和转义；冲突显示 `UU/AA/UD/DU/AU/UA/DD`。当前不推断重命名，路径移动表现为删除和新增。

`diff` 默认只检查 Index 路径，不显示其他未跟踪文件；`--cached`（别名 `--staged`）比较 HEAD→Index，未出生 HEAD 当作空快照，也可指定一个修订代替 HEAD。不带该选项时，一个修订比较其快照与工作区，候选为该快照和当前 Index 的路径并集；两个修订只比较历史快照，不依赖当前 Index。涉及 Index 的比较拒绝未解决冲突。`-U count`、`-Ucount`、`--unified=count`、`--unified count` 设置上下文，默认 3、最多 100000；不支持路径筛选、重命名推断或 `--exit-code`。

差异展示执行位和二进制变化，二进制只显示摘要；文本保留 CRLF 与末尾 LF 区别。大变化超出 LCS 表格预算时整体替换变化区间，每侧文本最多 100000 行，完整输出最多 64 MiB，显示路径最多 4096 字节。它不保存工作区 Blob、Index 或引用，头部使用 `diff --minigit`，不承诺完整 Git 补丁兼容。

`show` 直接读取已保存内容：

| spec | 输出 |
| --- | --- |
| `HEAD`、分支、标签、完整 Commit ID | 元数据与相对第一父提交的差异；首提交与空快照比较 |
| 完整 Blob / Tree ID | Blob 原始字节，或 Tree 的模式、完整 ID、转义名称 |
| `revision:path` | 指定提交中的文件原始字节 |
| `:path`、`:0:path` | Index 的 stage 0 内容 |
| `:1:path`、`:2:path`、`:3:path` | 冲突阶段的 base、ours、theirs 内容；不存在的 stage 明确失败 |

含 `1:` 等前缀的真实文件名可用 `:0:1:name` 避免 stage 歧义。历史和原始对象查询不解析当前 Index，工作区编辑不影响它们。提交差异采用三行上下文，多父提交列出全部 parent，但只与第一父提交比较；不支持 combined diff 或目录路径展示。完整 show 结果最多 64 MiB，选择串最多 8193 字节。输入带选项样式的引用名时使用 `show -- name`。

`log` 使用子提交先于父提交的拓扑顺序，并对共享祖先去重；同时可输出的节点按完整 ID 排序。显示缩写为 12 位，解析仍要求完整 ID。支持 HEAD、分支、标签或完整引用名称，名称歧义明确失败。`-n` 只限制显示数量，仍读取所选提交的完整可达历史；目前限 100000 个提交、1000000 条父边及 64 MiB 图数据。

log 与 show 共用提交文本格式化；单个格式化提交条目及完整 log 输出最多 64 MiB，以限制消息缩进与条目累积后的显示规模；log 先完成格式化，超限时返回错误且不留下部分输出。

`reflog` 观察引用曾怎样移动，与沿 parent 读取的 `log` 分开。当前 commit 将分支引用、HEAD 日志与分支日志放进同一个短事务；分离 HEAD 则发布 HEAD 与它的日志。输出用完整 ID 显示 `old -> new reason`，缺失端点为 `-`；文件缺失时显示空日志，标签不支持 reflog。`HEAD@{0}` 等是显示位置，尚不能作为修订选择串使用。带选项样式的分支名可用 `reflog -- name`。

Reflog 使用自有 MGRL v1 格式，每份日志最多 8 MiB、100000 个事件；原因是非空单行 UTF-8，最多 4096 字节。完整 CLI 输出预检为 64 MiB，待恢复事务会阻止查询。当前没有 actor、时间、过期清理、选择器或 reflog 撤销命令；branch 已通过子项验收，switch 尚未交付，核心已能规划不同分支指向同一提交时的 HEAD 切换事件。接入验证状态见进度页。

构建后可运行 [对象观察实验](examples/experiments/objects.sh)：

```sh
bash examples/experiments/objects.sh ./build/debug/minigit
bash examples/experiments/commits.sh ./build/debug/minigit
bash examples/experiments/diff.sh ./build/debug/minigit
bash examples/experiments/show.sh ./build/debug/minigit
bash examples/experiments/reflog.sh ./build/debug/minigit
bash examples/experiments/branches.sh ./build/debug/minigit
```

[提交实验](examples/experiments/commits.sh) 进一步检查暂存 B、工作区编辑为 C 后仍提交 B，以及下一次提交链接前一个 parent；还核对 status、log 和标签在分支前进后保持原提交。脚本创建临时仓库并在结束时自动清理，不调用系统 Git。测试结果以 [进度](docs/progress.md) 为准。

[差异实验](examples/experiments/diff.sh) 将 HEAD A、Index B、工作区 C 的三种比较实际重放，再提交 B，并验证历史 A→B 与未暂存 B→C。补丁文件放在临时仓库外，脚本结束自动清理。

[内容实验](examples/experiments/show.sh) 读取提交 A、暂存 B，并验证工作区仍为 C；通过原始 Blob ID 与 Index 路径输出含 NUL 的二进制文件，再用 `cmp` 核对字节。

[引用日志实验](examples/experiments/reflog.sh) 连续提交 A、B，核对缺失端点→A→B 的事件及 HEAD／main 两份日志字节一致。它展示的是引用事件，不是提交父图；两种预设的实验及 reflog 子项验收已通过。

[分支实验](examples/experiments/branches.sh) 创建 topic 指向 A，再让 main 提交 B，证明创建分支不切换工作区且 topic 仍固定 A；随后安全删除祖先分支，验证 topic/sub 删除后 topic 名称可复用。两种预设的实验已通过，详细验证见进度页。

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

## 共享更新规划器：核心接口已实现

[worktree_update.hpp](include/minigit/worktree_update.hpp) 为后续 switch、restore、reset、rm、mv 准备共同机制，本子项不增加 CLI。调用者在同一 RepoLock 内提供“目标路径状态”和“允许改写的旧跟踪文件路径”；规划器检查实际文件与目录，捕获真实 before 字节和权限，再生成 TransactionPlan。HEAD 与日志的变化仍由业务层规划，最终由短事务一起执行。

目标缺失与空文件分别表示，已有未授权文件、未知目录、软链和特殊文件受到保护。精确授权的旧文件位置若已变为空目录，可显式移除该空节点；授权不会延伸到陌生子文件或空子目录。部分路径更新保留未变化父目录与无关兄弟文件；目录改为文件时必须显式删除全部已授权的旧文件，不能递归清掉未知内容。规划可能保存不可变备份 Blob，但不会写工作文件或元数据。具体边界及 C++ 例子见架构、导读与学习笔记；构建与验证证据见进度页。

## 阅读路线

1. [架构](docs/architecture.md)：模块职责、依赖方向、数据不变量与数据流。
2. [Git 原理](docs/git-internals.md)：内容寻址、快照、三棵树、引用与真实 Git 的差异。
3. [代码导读](docs/code-walkthrough.md)：从 CLI 到存储的阅读顺序和验证方法。
4. [学习笔记](docs/learning-notes.md)：可执行的观察实验、取舍与常见误区。
5. [进度](docs/progress.md)：完成状态、验证证据、阶段验收与下一步。

Phase 1 的暂存、提交与历史实验可直接运行。branch 管理已通过子项验收；切换分支、工作区恢复、合并及历史重写随后续阶段交付，当前状态统一由进度文档记录。

### 中断事务的恢复

工作区短事务与 `minigit recover` 已完成子项验收。发生中断后，状态／历史查询和业务修改会提示先恢复，原始对象仍可通过 `cat-file` 或完整 Blob/Tree ID 的 `show` 观察。恢复拒绝损坏日志、缺失备份和计划外修改，并保留现场；不要通过手工删除日志来把未知状态当作成功。

事务使用 `.minigit/transaction` 与内部 `transaction-staging`。当前 Linux 实现依赖可用的 `/proc/self/fd`，工作区相关目录和元数据暂存必须在同一文件系统；源文件需具备 owner-read 权限，目录需可读取和遍历，包含更新子项的目录需 owner-rwx。这些约束属于当前实现边界，具体已验证范围见 [进度](docs/progress.md)。commit 已接入共同事务与 reflog 规划；branch 已通过同一事务发布引用与分支日志的子项验收。工作区切换与恢复业务命令将在后续子项接入。

可重复的 [事务实验](examples/experiments/transactions.sh) 使用单独构建的 C++ 教学程序，在 a 已更新、b 尚未更新时保留 Prepared 日志，再通过真实 CLI 恢复并核对两次结果：

```sh
cmake --build --preset debug --target minigit_transaction_lab -j 2
bash examples/experiments/transactions.sh ./build/debug/minigit ./build/debug/minigit_transaction_lab
```

[教学程序](examples/experiments/transaction.cpp) 只允许空目录，并调用核心库的公开测试钩子；它不是生产 CLI 的隐含模式，也不模拟断电系统调用。

恢复保留内容和权限位，不还原 owner/group 或时间戳。已验证的故障点以 API 持久化步骤完成为边界；若系统调用内部中断留下未记录临时项，恢复会保留现场并拒绝自动清理，不承诺任意断电或 SIGKILL 都能自动恢复。
