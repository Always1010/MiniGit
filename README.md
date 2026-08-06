# MiniGit

MiniGit 是使用 C++20 编写的简化本地版本管理器，用于通过可阅读的实现学习 Git 的对象模型、暂存区、引用、历史和合并。项目采用自己的仓库格式，不承诺与 Git 仓库互读；网络协议、远程仓库和托管服务不在范围内。

Phase 0 构建基础、Phase 1 本地快照闭环、Phase 2 比较与安全更新及 Phase 3 合并与应用均已验收，当前版本仍为 `0.1.0`。Phase 1 验收时 Debug 与 sanitized 各 54 项检查全部通过；各阶段真实范围与验证记录见 [进度](docs/progress.md)。

Phase 2 的八个命令 diff／show／branch／switch／restore／reset／rm／mv 已完成整体验收，Debug 与 sanitized 各 147 项全量检查全部通过（130 单元、17 CLI），无 ASan／UBSan 报告。共同的数据模型、事务架构、与原生 Git 的边界及 Phase 3 依赖链集中记录在 [Phase 2 阶段总结](docs/progress.md#phase-2-阶段总结)。版本仍为 0.1.0；Phase 3 合并与应用的后续整体验收见下文，该 Phase 2 验收时 Phase 4 仍待实现；当前 Rebase 子项与 GC 边界见下文，最终目标未完成。

Phase 3 复用纯文本三方合并、Snapshot 的内容／模式／存在与 D/F 合并、完整 DAG 基线选择、显式提交工厂和真实 before 镜像。业务再组合 Index／Work／引用与日志，并以短事务发布；冲突状态支持跨进程继续／中止。各层接口与逐项验证历史集中在进度页，详细调用链见 [代码导读](docs/code-walkthrough.md)。

Merge 使用唯一最低共同基线；CherryPick／Revert 根据单来源父快照正向／反向重放，成功保留空结果。Stash 独立保存 H/I/W/U，默认恢复 Work，--index 另合并暂存意图；pop 完整成功才删除条目，显式继续／中止不创建提交。首版 clean tracked、跨层 D/F、来源父数、未知节点和真实权限保护等限制见阶段总结，不承诺原生 Git 的全部策略或仓库格式兼容。

Phase 3 的完整 Merge／CherryPick／Revert／Stash（限定本项目范围）已完成独立整体验收。冻结代码的 Debug 与 sanitized 各 237 项全量检查（216 单元、21 CLI）全部通过，用时 192.25／581.97 秒，无 ASan／UBSan 报告；共享架构、核心学习结果、Git 差异与 Phase 4 依赖集中见 [Phase 3 阶段总结](docs/progress.md#phase-3-阶段总结已验收)。版本仍为 0.1.0；Rebase 业务现已通过子项验收，GC 也已通过子项验收；Phase 4 全量整体验收与最终目标收尾仍待完成。

Phase 4 首个子项 Rebase 状态持久化基础已通过子项验收：明确 Ready／Conflicted、线性 queue／cursor、onto 与原分支镜像，以及受约束的 canonical CAS 更新。状态基础源码与五组测试已冻结核对，两个预设相关 25 项及完整只读审查通过；该基础验收轮没有 rebase CLI 或实际重放；当前 Rebase 业务生命周期也已通过子项验收：两预设各 38 项相关检查及 rebase 实验通过；GC 也已通过子项验收：两预设各 24 项与 GC 实验通过；当前注册 260 项，尚未全量执行或完成 Phase 4 整体验收。范围见 [状态基础进度](docs/progress.md#rebase-状态基础已验收) 与 [Rebase 业务验收](docs/progress.md#rebase-业务生命周期已验收)，Phase 3 全量验收证据保持历史范围。

## 构建与运行

需要 CMake 3.20 或更高版本、支持 C++20 的编译器、OpenSSL 3、zlib 和 Catch2 v3。CLI 集成检查通过 CMake 脚本注册到 CTest，核心单元测试使用 Catch2。预设负责配置项目，依赖由环境提供，不在构建过程中隐式安装系统软件。

普通 Debug 保留未优化调试构建；sanitized 使用 `-g -O1` 并保留 ASan／UBSan 与帧指针。定向检查和全量回归的选择见 [构建与验证策略](docs/testing.md)。

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
| `switch [--] branch` | 切换已有短分支，保留兼容的暂存与工作区修改 |
| `switch -c name [source]`、`switch --detach [revision]` | 创建并切换，或进入分离 HEAD；来源默认 HEAD |
| `restore [--source revision] [--staged] [--worktree] [--] paths...` | 选择性恢复 Work／Index，来源与目的独立，HEAD 与日志不动 |
| `reset [--soft\|--mixed\|--hard] [--] [revision]` | 整体重置当前历史位置，默认 mixed，按模式更新 Index／Work |
| `rm [-r] [-f] [--cached] [--] paths...` | 删除 Index 已知路径，默认同时删工作文件；已验收 |
| `mv [--] source destination` | 同事务迁移 Index 路径与实际工作文件，首版无覆盖选项；已验收 |
| `merge [-m message] [--] revision`、`merge --continue`、`merge --abort` | 单源合并、从 Index 继续或恢复真实备份；已验收 |
| `cherry-pick [--message text] [--] revision`、`cherry-pick --continue`、`cherry-pick --abort` | 应用单来源变化并保留来源作者；已验收 |
| `revert [--message text] [--] revision`、`revert --continue`、`revert --abort` | 反向应用单来源变化，保存新的单父提交；已验收 |
| `rebase [--] revision`、`rebase --continue`、`rebase --abort` | 重放线性来源、继续或中止；已通过子项验收 |
| `gc [--prune-now]` | 松散对象完整前检与可达闭包回收；已通过子项验收 |
| `stash`、`stash push [-u] [-m message]` | 保存独立 Index／Work 与可选 untracked 后清理；已验收 |
| `stash list`、`stash show [--] [stash@{N}]` | 列表与已保存 H→W+U 差异，不读取当前 Index／Work |
| `stash drop [--] [stash@{N}]`、`stash clear` | 删除位置或清空活跃栈，不移动 HEAD／日志；已验收 |
| `stash apply [--index] [--] [stash@{N}]`、`stash pop [--index] [--] [stash@{N}]` | 恢复 Work 与可选独立暂存意图；已验收 |
| `stash --continue`、`stash --abort` | 从 resolved Index 完成，或恢复实际 before；无其他参数，已验收 |

对象 ID 必须为完整 64 位十六进制文本，尚不支持缩写。不带 `-w` 的摘要计算可以在仓库外执行；写入和读取需要当前目录或祖先存在有效 `.minigit` 仓库。当前文件读取上限为 64 MiB，软链接和非普通文件被拒绝。

`add` 立即保存文件字节并更新 `.minigit/index`，之后继续编辑工作区不会改变暂存内容。仓库根的 `.minigitignore` 支持注释、普通路径、目录尾斜杠和 `*`、`?` 模式；无斜杠规则匹配任意层的名称，路径规则相对于根目录。不支持 `!` 和 `**`，遇到这些语法明确报错。忽略规则不移除已跟踪路径，明确添加被忽略的新文件会失败。

轻量标签只是 `refs/tags/name` 中保存的提交 ID，后续 commit 不会移动它。重复名称默认失败，只有 `-f` 明确移动；尚未提交的 HEAD 不能创建默认标签。标签名称遵循引用路径约束，保留 HEAD、refs/ 前缀及完整十六进制 ID，以免与修订解析歧义。当前不支持附注标签或指向 Blob 的标签。

`branch` 按名称列出短名与完整提交 ID，当前符号分支标 `*`，未出生分支显示 `(unborn)`；分离 HEAD 时没有分支被标为当前。创建默认从 HEAD 取目标，未出生 HEAD 不能提供默认目标。创建或强制移动只更新分支引用和它的 reflog，不切换 HEAD，也不改 Index 或工作文件。分支短名禁止 HEAD、refs/ 前缀和纯 64 位十六进制 ID，并遵循严格引用名称规则；可用 `branch -- -topic` 传入选项样式名称。

已有分支默认拒绝覆盖，`-f` 明确移动；`-f` 只用于创建／移动，不与 `-d`、`-D` 组合，强制删除使用 `-D`。当前符号分支始终拒绝覆盖或删除，即使目标 ID 相同。非当前分支强制移动到原 ID 时，验证目标后不追加日志。`-d` 要求该分支提交位于当前 HEAD 的完整 parent 闭包内；多父图会完整校验，不能只查第一父链。`-D` 跳过这项可达性要求，仍校验原分支快照、日志格式与日志引用的 Commit。删除同时移除分支 ref 与分支日志，HEAD 日志保留；尚无永久保存已删分支日志的功能。

`switch` 普通形式只选择已有短分支，同名标签不会造成歧义；`--detach` 才解析修订，标签与分支同名时需给完整引用。`-c` 创建新分支并切换，分支引用与日志、HEAD 与日志、Index 和实际文件在同一事务中发布。不同分支即使指向同一提交，也记录 HEAD 切换事件。可用 `switch -- -topic` 选择选项样式名称。

切换逐路径比较旧 HEAD、Index、实际工作区与目标。旧 HEAD 或 Index 已等于目标时保留该路径的暂存及工作文件；其他路径只有 Index 等于旧 HEAD 且工作文件等于 Index 才能更新。任何不兼容路径使整个操作拒绝；不提供 force、丢弃修改或前一分支选择器。未知、忽略文件及陌生目录子项仍受保护；冲突或损坏 Index 即使切回当前分支也拒绝。

未出生 HEAD 下，不带显式来源的 `switch -c new` 只选择新的符号 HEAD，保留 Index 和实际文件，不创建空 ref 或 Commit；HEAD 日志记录缺失端点到缺失端点。新名称不能是当前分支或已有分支。切换当前未出生分支可无操作，其他不存在的分支拒绝；显式 HEAD 来源或 detach 无法解析尚不存在的提交。

Restore 核心与 CLI 已通过子项验收，用于分别恢复工作文件和暂存内容，验证证据见进度页。默认从 Index 恢复选中工作文件；--staged 默认从 HEAD 恢复选中 Index；同时 --staged --worktree 默认用 HEAD 更新两者。显式 --source（-s，亦可 --source=revision）指定 Commit 来源，--staged／--worktree 的短形式为 -S／-W；路径可直接跟在命令后，-- 用于结束选项解析。

选择按已知版本路径 exact／目录 prefix 匹配，不支持 glob；每个原始参数都必须在 source∪Index 中匹配，其他有效路径不能掩盖一个无效参数。来源没有的已选路径表示删除，空 Blob 表示零字节文件。默认 Index→Work 拒绝选中的冲突，未选冲突保留；显式 Commit→Work 保留 Index 阶段，只有 staged 目的清除所选冲突并写 stage 0。Index-only 不探测工作区软链，实际工作区恢复仍保护未知、忽略文件和陌生目录子项。

Restore 不改变 HEAD、分支引用或 reflog；两个目的组合仍一次事务发布。[恢复实验](examples/experiments/restore.sh) 在两种预设下均已通过，解释及参数范围见架构与学习笔记。

Reset 核心与 CLI 已通过子项验收，执行证据见进度页。soft 只移动 HEAD／当前分支与日志；默认 mixed 再将完整 Index 替换为目标；hard 再恢复旧 HEAD、旧 Index 与目标并集内的工作文件。当前分支名称或 detached 状态保持，指定另一个分支为来源也不会切换到它。成功没有标准输出。

soft 拒绝任何冲突阶段，mixed／hard 清除它们；所有模式先校验完整 Index 与目标对象。hard 明确允许丢弃已有跟踪文件的修改，仍保护需要改写的未知／忽略文件和陌生目录子项；不是清理整个工作区。soft／mixed 不探测实际工作文件。相同提交 ID 也不能跳过 Index／Work 恢复，只是不追加移动日志。

未给修订且 HEAD 未出生时，目标为空：soft 保留原状，mixed 清 Index，hard 再删除 Index 已跟踪文件，未知文件保留；不建立空 ref 或日志。显式 HEAD 则必须解析为提交，因此未出生时失败。首版没有路径 reset、ORIG_HEAD、keep／merge 或 reflog 选择器；路径暂存恢复用 restore --staged。[重置实验](examples/experiments/reset.sh) 在两种预设下均已通过。

Rm 已通过子项验收，用于把 Index 已知路径的删除准备给下一次 commit。普通 rm 要求 Index 等于 HEAD，实际工作文件等于 Index 或真正缺失；--cached 只移除 Index，要求 Index 等于 HEAD 或工作文件。前者成立时不探测 Work，后者才读取需要比较的工作文件。-f 明确放弃这项内容一致性保护，可清所选冲突 stages，仍不跳过对象、预算及实际工作区安全检查。

候选仅来自 Index，不提供 glob；每个参数须 exact／目录 prefix 匹配。prefix 包含后代时必须 -r（--recursive）；-f 的别名是 --force，各模式标志只可指定一次，重复别名也报参数错误。只删所选 tracked 叶文件，保留空父目录与陌生兄弟文件；原 tracked 文件变目录时普通模式不能当作缺失，force 也不授权递归删除未知子项。--cached 不删 Work，--cached -f 完全不探测 Work。成功静默，HEAD、ref、reflog 与身份不动；Index／实际文件一起事务发布。[删除实验](examples/experiments/remove.sh) 两种预设均已运行通过。

Mv 已通过子项验收，每次接受一个源和一个目的：源为 Index 已知的普通文件或目录 prefix。它把原 Index 的 mode／ID 放到新路径，并独立迁移实际工作文件的字节与完整权限；源即使是 MM，也不能用 add 新路径替代这两份版本。所选冲突拒绝，未选冲突保留；成功静默，不改 HEAD、ref、日志或身份。

目的若为已有普通目录，只追加一次源 basename；解析后的目的必须没有实际节点或 Index 占用，同字节文件也拒绝覆盖。目录源必须完整由所选跟踪文件构成，未知／忽略子文件、陌生空目录、软链与特殊节点均阻止迁移。首版不支持 -f、多源或 dry-run，自身／祖先／后代及仓库根迁移拒绝；新父目录为 0755，已有父目录权限保留，不迁移源目录权限。规划拒绝不发布 Work／Index，可遗留不可变备份 Blob；执行中断可能留下 pending journal 与部分更新，需 recover。源删除、目的发布和 Index 映射一起事务执行，[迁移实验](examples/experiments/move.sh) 两种预设均已运行通过。

Merge 首版不提供强制覆盖、--no-ff 或多源形式。-m 只用于启动，continue／abort 不接收修订或消息；-- 可传入选项样式修订。正常结果输出说明并返回 0，已发布冲突输出路径并返回 1，业务错误返回 1，参数错误返回 2。冲突展示有 64 MiB 上限，超限省略剩余路径并提示使用 status，Index／State 仍记录完整结果。受影响 Work 备份包含保持 ours 的冲突同值节点及 D/F 区域内必要真实目录权限；abort 不只撤销实际写过的文件。实际覆盖仍保护未知内容；incoming 未跟踪节点若与目标字节及权限相同、无需改写，可保留并跟踪，不表示支持覆盖。[合并实验](examples/experiments/merge.sh) 已在两种构建运行通过，核对原 stages、完整权限／raw metadata 中止和从 Index 继续。精确验证范围见进度页。

CherryPick／Revert 的 [重放实验](examples/experiments/replay.sh) 在两种构建均运行通过。它检验正向变化与反向变化、来源 author／当前 committer、单父与空重放，以及 0640 权限／raw Index／raw HEAD 中止。成功结果返回 0，已发布冲突报告路径并返回 1，业务错误为 1，参数错误为 2；冲突展示沿用有界省略提示，完整 Index／State 不裁剪。

Stash 无参数等于 push，-u 的别名为 --include-untracked；show／drop 默认位置 0，仅接受 stash@{N}，不接受裸数字或对象 ID。list／show 可在 active operation 下查询，push／drop／clear 拒绝 active operation。list 不展开历史 Tree；show 完整校验所选 H/I/W/U，但只展示 H→W+U，独立 I 不在补丁中显示。drop／clear 的事务 refs/stash 预检会展开旧 top Work Tree，损坏时可保守拒绝；不读取当前 Index／Work不能推导为不读历史 Tree。[保存实验](examples/experiments/stash.sh) 在两种预设构建均运行通过。

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

Reflog 使用自有 MGRL v1 格式，每份日志最多 8 MiB、100000 个事件；原因是非空单行 UTF-8，最多 4096 字节。完整 CLI 输出预检为 64 MiB，待恢复事务会阻止查询。当前没有 actor、时间、过期清理、选择器或 reflog 撤销命令；branch 已通过子项验收，switch 已接入不同分支指向同一提交时的 HEAD 切换事件。接入验证状态见进度页。

构建后可运行 [对象观察实验](examples/experiments/objects.sh)：

```sh
bash examples/experiments/objects.sh ./build/debug/minigit
bash examples/experiments/commits.sh ./build/debug/minigit
bash examples/experiments/diff.sh ./build/debug/minigit
bash examples/experiments/show.sh ./build/debug/minigit
bash examples/experiments/reflog.sh ./build/debug/minigit
bash examples/experiments/branches.sh ./build/debug/minigit
bash examples/experiments/switch.sh ./build/debug/minigit
bash examples/experiments/restore.sh ./build/debug/minigit
bash examples/experiments/reset.sh ./build/debug/minigit
```

[提交实验](examples/experiments/commits.sh) 进一步检查暂存 B、工作区编辑为 C 后仍提交 B，以及下一次提交链接前一个 parent；还核对 status、log 和标签在分支前进后保持原提交。脚本创建临时仓库并在结束时自动清理，不调用系统 Git。测试结果以 [进度](docs/progress.md) 为准。

[差异实验](examples/experiments/diff.sh) 将 HEAD A、Index B、工作区 C 的三种比较实际重放，再提交 B，并验证历史 A→B 与未暂存 B→C。补丁文件放在临时仓库外，脚本结束自动清理。

[内容实验](examples/experiments/show.sh) 读取提交 A、暂存 B，并验证工作区仍为 C；通过原始 Blob ID 与 Index 路径输出含 NUL 的二进制文件，再用 `cmp` 核对字节。

[引用日志实验](examples/experiments/reflog.sh) 连续提交 A、B，核对缺失端点→A→B 的事件及 HEAD／main 两份日志字节一致。它展示的是引用事件，不是提交父图；两种预设的实验及 reflog 子项验收已通过。

[分支实验](examples/experiments/branches.sh) 创建 topic 指向 A，再让 main 提交 B，证明创建分支不切换工作区且 topic 仍固定 A；随后安全删除祖先分支，验证 topic/sub 删除后 topic 名称可复用。两种预设的实验已通过，详细验证见进度页。

[切换实验](examples/experiments/switch.sh) 创建 topic 并提交 B，再回 main 读取 A；重叠的 staged／working 修改阻止切换，同快照新分支保留两份修改，随后展示干净切换、detached HEAD 与 reflog。两个预设实验已通过，定向测试的完整验收状态见进度页。

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

[worktree_update.hpp](include/minigit/worktree_update.hpp) 供 switch、restore、reset、rm 复用，并为已接入的 mv 提供共同机制，本子项不增加 CLI。调用者在同一 RepoLock 内提供“目标路径状态”和“允许改写的旧跟踪文件路径”；规划器检查实际文件与目录，捕获真实 before 字节和权限，再生成 TransactionPlan。HEAD 与日志的变化仍由业务层规划，最终由短事务一起执行。

目标缺失与空文件分别表示，已有未授权文件、未知目录、软链和特殊文件受到保护。精确授权的旧文件位置若已变为空目录，可显式移除该空节点；授权不会延伸到陌生子文件或空子目录。部分路径更新保留未变化父目录与无关兄弟文件；目录改为文件时必须显式删除全部已授权的旧文件，不能递归清掉未知内容。规划可能保存不可变备份 Blob，但不会写工作文件或元数据。具体边界及 C++ 例子见架构、导读与学习笔记；构建与验证证据见进度页。

## 阅读路线

1. [架构](docs/architecture.md)：模块职责、依赖方向、数据不变量与数据流。
2. [Git 原理](docs/git-internals.md)：内容寻址、快照、三棵树、引用与真实 Git 的差异。
3. [代码导读](docs/code-walkthrough.md)：从 CLI 到存储的阅读顺序和验证方法。
4. [学习笔记](docs/learning-notes.md)：可执行的观察实验、取舍与常见误区。
5. [进度](docs/progress.md)：完成状态、验证证据、阶段验收与下一步。

Phase 1 的暂存、提交与历史实验可直接运行。branch 管理已通过子项验收；switch 与 restore 已通过子项验收，reset 已通过子项验收，rm 删除与取消暂存已通过子项验收，mv 文件与目录迁移已通过子项验收，合并及历史重写随后续阶段交付，当前状态统一由进度文档记录。

### 中断事务的恢复

工作区短事务与 `minigit recover` 已完成子项验收。发生中断后，状态／历史查询和业务修改会提示先恢复，原始对象仍可通过 `cat-file` 或完整 Blob/Tree ID 的 `show` 观察。恢复拒绝损坏日志、缺失备份和计划外修改，并保留现场；不要通过手工删除日志来把未知状态当作成功。

事务使用 `.minigit/transaction` 与内部 `transaction-staging`。只读空目录在目标缺失时以 exclusive mkdir 设置精确权限，可写目录继续暂存后 rename；系统调用内部临时权限若不匹配日志镜像会严格拒绝恢复。当前 Linux 实现依赖可用的 `/proc/self/fd`，工作区相关目录和元数据暂存必须在同一文件系统；源文件需具备 owner-read 权限，目录需可读取和遍历，包含更新子项的目录需 owner-rwx。这些约束属于当前实现边界，具体已验证范围见 [进度](docs/progress.md)。commit 已接入共同事务与 reflog 规划；branch 已通过同一事务发布引用与分支日志的子项验收。switch／restore／reset／rm／mv 已接入共同更新机制；OperationState 基础已接入事务，Merge 生命周期已通过子项验收。

可重复的 [事务实验](examples/experiments/transactions.sh) 使用单独构建的 C++ 教学程序，在 a 已更新、b 尚未更新时保留 Prepared 日志，再通过真实 CLI 恢复并核对两次结果：

```sh
cmake --build --preset debug --target minigit_transaction_lab -j 2
bash examples/experiments/transactions.sh ./build/debug/minigit ./build/debug/minigit_transaction_lab
```

[教学程序](examples/experiments/transaction.cpp) 只允许空目录，并调用核心库的公开测试钩子；它不是生产 CLI 的隐含模式，也不模拟断电系统调用。

恢复保留内容和权限位，不还原 owner/group 或时间戳。已验证的故障点以 API 持久化步骤完成为边界；若系统调用内部中断留下未记录临时项，恢复会保留现场并拒绝自动清理，不承诺任意断电或 SIGKILL 都能自动恢复。
