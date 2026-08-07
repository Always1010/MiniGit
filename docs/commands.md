# 命令参考

`minigit` 指构建得到的可执行文件，位置见[快速开始](../README.md#快速开始)。多数命令在当前目录或祖先目录发现 `.minigit`；`init` 和不带 `-w` 的 `hash-object` 可在仓库外执行。

## 参数约定

- 修订支持 `HEAD`、分支、轻量标签、完整引用和完整 64 位十六进制对象 ID；同名分支与标签需使用完整引用。
- 不支持对象 ID 缩写、`~`／`^` 运算和 reflog 选择器。日志里的 12 位 ID 仅用于显示。
- 路径相对于当前目录。路径选择命令使用文件名或目录前缀，不支持 glob；支持 `--` 的命令可用它结束选项解析。
- 成功返回 `0`，业务错误或已发布冲突返回 `1`，参数错误返回 `2`。

## 暂存与提交

| 命令 | 用途 |
| --- | --- |
| `init [directory]` | 初始化仓库，重复初始化保留已有数据 |
| `config user.name [value]`、`config user.email [value]` | 读取或设置仓库本地提交身份 |
| `add [--] paths...` | 暂存选中范围的新增、修改和删除 |
| `add -A`、`add --all` | 暂存整个仓库，不与路径参数组合 |
| `commit -m message [--allow-empty]` | 提交 Index；默认拒绝空首提交或无变化提交 |
| `status [--porcelain]` | 比较 HEAD、Index 与工作区；`--short`／`-s` 同为简短输出 |

`add` 保存当时的字节，之后编辑工作文件不会改变暂存内容。`commit` 不接收未解决的冲突阶段。

`status` 的两列分别表示 HEAD→Index 和 Index→工作区：`A/M/D` 是新增／修改／删除，`??` 是未跟踪，`MM` 是暂存后又编辑。移动路径显示为删除和新增。

仓库根的 `.minigitignore` 支持注释、路径、目录尾斜杠及 `*`、`?`；无斜杠规则匹配任意层名称。`!` 和 `**` 明确报错。已跟踪文件不受忽略规则排除，明确添加被忽略的新文件会失败。

## 差异与内容查询

| 命令 | 内容来源 |
| --- | --- |
| `diff` | Index→工作区，不展示其他未跟踪文件 |
| `diff --cached [revision]` | HEAD 或指定修订→Index；`--staged` 为别名 |
| `diff revision` | 指定修订→工作区 |
| `diff old new` | 两个历史快照，不读取当前 Index |
| `show [--] [spec]` | 默认查看 HEAD，选择方式见下表 |
| `log [--oneline] [-n count] [revision]` | 遍历全部 parent，默认从 HEAD 开始 |
| `reflog [--] [HEAD\|branch\|refs/heads/branch]` | 展示本地引用移动事件，最新在前 |
| `hash-object [-w] file` | 计算 Blob 摘要；`-w` 同时保存对象 |
| `cat-file -t full-id`、`cat-file -p full-id` | 查看对象类型或内容 |

`diff` 的 `-U count` 设置上下文，默认 3；也接受 `-Ucount`、`--unified count`、`--unified=count`，最多 100000。涉及 Index 的比较拒绝冲突。含 NUL 的文件只显示二进制差异摘要；文本保留 CRLF 和末尾 LF 的区别。不提供路径筛选、重命名检测或 `--exit-code`。

| `show` 选择串 | 输出 |
| --- | --- |
| `HEAD`、分支、标签、完整 Commit ID | 元数据及与第一父提交的差异 |
| 完整 Blob／Tree ID | 原始文件字节，或 Tree 条目 |
| `revision:path` | 提交内文件字节 |
| `:path`、`:0:path` | Index stage 0 字节 |
| `:1:path`、`:2:path`、`:3:path` | 冲突的 base、ours、theirs 字节 |

不存在的 stage 报错。文件名包含 stage 样式前缀时，可用 `:0:1:name` 明确选择。历史及原始对象查询不读取当前 Index；目录路径和 combined diff 不支持。

`log -n` 只限制显示数量，仍校验完整可达历史。`reflog` 不接受标签；`HEAD@{0}` 仅为显示序号，不能传给修订参数。

## 分支与标签

| 命令 | 用途 |
| --- | --- |
| `tag`、`tag [-f] name [revision]`、`tag -d name` | 列出、创建／移动、删除轻量标签 |
| `branch`、`branch [-f] [--] name [revision]` | 列出、创建／移动分支，来源默认 HEAD |
| `branch -d [--] name`、`branch -D [--] name` | 安全删除或强制删除非当前分支 |
| `switch [--] branch` | 切换到已有短分支 |
| `switch -c name [source]` | 创建并切换，来源默认 HEAD |
| `switch --detach [revision]` | 进入分离 HEAD，目标默认 HEAD |

分支和标签默认拒绝覆盖，`-f` 明确移动。当前分支不能通过 `branch` 覆盖或删除。`branch -d` 要求 tip 位于 HEAD 的完整祖先集合中；`-D` 只跳过该条件，仍做数据校验。删除分支同时删除它的分支日志。

`branch` 只改变引用，`switch` 还更新 Index 和工作文件，并保留兼容的本地修改；不兼容时整体拒绝，判断矩阵见[架构](architecture.md#工作区更新)。未出生 HEAD 下，`switch -c new` 无显式来源时仅改变分支绑定，保留 Index 和工作文件。

轻量标签只指向 Commit。引用名称禁止 `HEAD`、`refs/` 短名前缀和纯 64 位十六进制名称；选项样式短名可按支持情况使用 `--`。

## 恢复、重置与路径操作

| 命令 | 效果 |
| --- | --- |
| `restore [--source revision] [--staged] [--worktree] [--] paths...` | 恢复选中路径，不移动 HEAD |
| `reset [--soft\|--mixed\|--hard] [--] [revision]` | 移动当前位置，默认 mixed，目标默认 HEAD |
| `rm [-r] [-f] [--cached] [--] paths...` | 删除 Index 已知路径，默认同时删除工作文件 |
| `mv [--] source destination` | 同事务迁移路径，分别保留暂存与工作版本 |

| `restore` 目的 | 默认来源 |
| --- | --- |
| 默认／`--worktree` | Index |
| `--staged` | HEAD |
| `--staged --worktree` | HEAD，同时更新两处 |

`--source`（`-s`）可覆盖默认来源，`--staged`／`--worktree` 的短形式为 `-S`／`-W`。每个路径参数都须在来源或 Index 中匹配。来源缺少所选路径表示删除，空 Blob 表示空文件；只有 staged 目的清除所选冲突阶段。

| `reset` 模式 | HEAD／当前引用 | Index | 工作文件 |
| --- | --- | --- | --- |
| soft | 移动 | 保留，拒绝冲突 | 保留 |
| mixed | 移动 | 替换并清冲突 | 保留 |
| hard | 移动 | 替换并清冲突 | 恢复已授权路径，丢弃其修改 |

Reset 保留当前分支名称或 detached 状态。同 ID 的 mixed／hard 仍恢复内容；未出生且未给修订时，mixed 清 Index、hard 再删除 Index 跟踪文件，显式 `HEAD` 则报错。没有路径 reset，取消暂存用 `restore --staged`。

普通 `rm` 要求 Index 等于 HEAD，工作文件等于 Index 或确实缺失；`--cached` 要求 Index 等于 HEAD 或工作文件，仅删 Index。`-f` 跳过内容一致性并清所选冲突，目录前缀有后代时需 `-r`；这些选项不授权删除未知内容。

`mv` 每次接受一个源和目的，所选冲突拒绝。目的为已有目录时追加源 basename；最终目的必须未占用，首版无覆盖选项。目录源内不能含未跟踪、忽略、陌生空目录、软链或特殊节点。Index 的 B 与工作文件的 C 分别迁移，不会把 C 自动暂存。

## 合并与历史重放

| 命令 | 作用 |
| --- | --- |
| `merge [-m message] [--] revision` | 单源合并；快进或创建两父提交 |
| `cherry-pick [--message text] [--] revision` | 应用单个来源的变化，保留原作者 |
| `revert [--message text] [--] revision` | 应用单个来源的反向变化 |
| `rebase [--] revision` | 按原顺序重放线性来源提交 |

启动要求已跟踪状态干净。CherryPick／Revert／Rebase 要求已出生 HEAD；Merge 可从未出生 HEAD 接收来源。分叉合并只接受唯一最低共同祖先，无共同祖先或多基线时拒绝；CherryPick／Revert 接受 root 或单父来源，多父来源拒绝。重放即使结果为空也创建单父提交。Rebase 保留原作者与消息、生成新 committer，最后才移动原分支；无交互式、`--onto`、`--skip` 或 merge-preserving 模式。

发生冲突后：编辑文件 → `add` 或删除对应冲突路径 → 执行对应命令的 `--continue`。放弃则执行 `--abort`，例如：

```sh
minigit add note.txt
minigit merge --continue
# 或放弃本次合并
minigit merge --abort
```

继续／中止不接收其他参数。Conflicted 继续以 resolved Index 为准，保留未暂存 Work；Rebase 还有下一来源时，下一步仍要求已跟踪状态干净。中止恢复原始 Index、HEAD 和受影响区域的实际备份，未知子项可能阻挡恢复。图解见[架构](architecture.md#冲突与恢复)。

## Stash

| 命令 | 用途 |
| --- | --- |
| `stash`、`stash push [-u] [-m message]` | 保存独立的 Index／Work；`-u` 包含非忽略未跟踪文件 |
| `stash list`、`stash show [--] [stash@{N}]` | 列表或查看已保存的 HEAD→Work 加 untracked 差异 |
| `stash drop [--] [stash@{N}]`、`stash clear` | 删除指定位置或清空活跃栈 |
| `stash apply [--index] [--] [stash@{N}]` | 恢复并保留栈条目 |
| `stash pop [--index] [--] [stash@{N}]` | 恢复完整成功后删除所选条目 |
| `stash --continue`、`stash --abort` | 完成冲突恢复或中止 |

位置默认 `stash@{0}`，不接受裸数字或对象 ID；Shell 中可写 `'stash@{0}'`。保存要求已出生 HEAD 和无冲突 Index；恢复另要求已跟踪状态干净。默认恢复 Work，`--index` 另恢复原暂存意图；不能用全部 add 代替它。

未跟踪保存内容只恢复到实际缺失的位置；已有同字节文件也拒绝占用。部分跨层文件／目录冲突保守拒绝。Pop 冲突时保留栈，continue 完整成功后才删除。继续不创建提交、不移动 HEAD；中止保留栈。Active operation 下允许 list／show，其他栈修改受限。

## 事务恢复与对象回收

`recover` 按未完成短事务恢复：Prepared 回到真实 before，Committed 确认 after；重复执行安全。出现待恢复事务时先运行它，再继续业务操作。恢复遇到损坏备份、计划外修改或未知节点会拒绝并保留现场，不要手工删除日志绕过它。

`gc [--prune-now]` 完整验证松散对象和根，再回收不可达对象。默认一天保留窗口将新对象及其依赖全部保留；`--prune-now` 关闭该窗口，未来 mtime 对象仍保留。GC 不移动引用、不读工作区、不清理 reflog；允许活跃操作，拒绝待恢复短事务。删除可能部分完成，具体边界见[架构](architecture.md#对象回收与边界)。
