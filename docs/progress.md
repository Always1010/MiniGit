# 进度与阶段验收

本页记录公开项目状态，不记录开发时间戳或内部协作日志。“计划”表示未交付，“待验证”表示需要实际运行并补充证据，“已验收”必须有代码与检查结果支撑。

## 当前状态

Phase 0 与 Phase 1 已验收，版本为 `0.1.0`。当前 CLI 完成 `init/add/commit/status/log/tag`，并提供对象观察与本地身份配置。Debug 与 sanitized 各 54 个 CTest 用例全部通过（47 个单元、7 个 CLI）；两个学习实验通过。Phase 2 至 Phase 4 尚未交付，继续逐项实现、测试与验收。

| 项目 | 当前状态 | 验证证据 |
| --- | --- | --- |
| CMake 3.20+、C++20 核心库与 CLI 分层 | 已验收 | Debug 和 sanitized 配置、构建成功，GCC 无警告 |
| OpenSSL 3、zlib、Catch2 v3、CTest 依赖连接 | 已验收 | 实际对象业务使用 EVP 和 zlib；当前注册 47 个单元及 7 个 CLI 用例 |
| CLI 帮助、版本与错误处理 | 已验收 | 两种预设的 `cli_basics` 均通过，错误输入退出码为 `2` |
| 中文架构、原理、导读、学习笔记 | 初版 | 六份文档相对链接均可解析，未包含具体开发日期 |
| Phase 1 子项：init、摘要、对象编解码与存储 | 已验收 | Debug 与 sanitized 构建通过，模块／CLI 回归及对象实验通过，详细范围见下文 |
| Phase 1 子项：Index 与 add | 已验收 | Debug 与 sanitized 的 9 个模块用例及 2 个相关 CLI 用例全部通过 |
| Phase 1 子项：引用、快照、commit 与本地身份 | 已验收 | Debug 与 sanitized 首轮 16 个相关用例通过，引用审查修正后各复验 10 项，提交实验通过 |
| Phase 1 子项：三棵树 status | 已验收 | Debug 与 sanitized 的 6 个状态单元用例及 status_cli、cli_basics 各 8 项通过 |
| Phase 1 子项：DAG 与 log | 已验收 | Debug 与 sanitized 各通过 4 个 DAG/日志单元用例及 log_cli、cli_basics，共 6 项 |
| Phase 1 子项：轻量 tag | 已验收 | Debug 与 sanitized 各通过 3 个标签单元用例及 tags_cli、log_cli、cli_basics，共 6 项 |
| Phase 1 整体 | 已验收 | 两种预设各 54 项全量检查通过，两个实验及文档检查通过 |
| Phase 2 至 Phase 4 | 计划 | 尚未实现或验收 |

已执行的 Debug 验证：

```sh
cmake --preset debug
cmake --build --preset debug -j 2
ctest --preset debug -R cli_basics --output-on-failure
```

Phase 0 的一个 CTest 用例通过，覆盖 `--help`、版本格式和空参数、未知参数、多余参数；对象库子项又补充了 `help` 别名与标准输出失败检查。相同基础用例在 `sanitized` 预设下通过，启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。Linux 是当前验证平台，其他平台尚未验证。

## Phase 0：可构建基础

验收条件：

- CMake 预设能配置和构建 `minigit_core` 及 CLI，核心库使用 C++20。
- 构建连接 OpenSSL 3 和 zlib，测试使用 Catch2 v3 并注册到 CTest。
- `--help`、`help`、`--version` 输出稳定并成功退出；未知命令返回明确错误和非零退出码。
- CLI 基础测试通过，文档提供可复用命令和真实能力边界。

## Phase 1：本地快照闭环

已完成 `init`、`add`、`commit`、`status`、`log` 和轻量 `tag`，通过整体回归与实验验收。

当前子项范围：

- `Repository::init/discover/root/metadata`，`.minigit` 版本与哈希配置，指向 main 的未出生 HEAD。
- `ObjectId`、EVP SHA-256、blob/tree/commit 封装与编解码、zlib 松散对象写入和读取。
- `RepoLock`、`read_file`、`atomic_write`，普通文件与软链接检查，64 MiB 文件上限。
- CLI `init [directory]`、`hash-object [-w] file`、`cat-file -t|-p full-id`。
- 版本化 MGIX Index、按选择范围保存 Blob 的 `add`、暂存删除及文件模式；支持简化 `.minigitignore`，支持冲突阶段的持久化与 add 解决。
- 版本化本地身份、符号／分离 HEAD、严格引用及预期旧值更新；Snapshot 与分层 Tree 往返；从 Index 构造带运行时 UTC 身份及 parent 的 Commit。
- 三棵树状态比较、所有冲突阶段状态、完整 DAG 的确定性拓扑日志，以及指向 Commit 的轻量标签列表／创建／强制移动／删除。
- 对象 CLI 集成用例与 [对象实验](../examples/experiments/objects.sh)，均已通过。

验证证据：

- Debug 与 sanitized 配置和构建通过，GCC 无警告。
- `ctest --preset debug --output-on-failure`：最终 15 个用例全部通过（13 个单元、2 个 CLI）。
- `ctest --preset sanitized --output-on-failure`：15 个用例全部通过，没有 ASan/UBSan 报告；最后目录同步修正后，以 `-R 'Loose blobs|Repository initializes|Repeated init|Relative filesystem|cli_basics|objects_cli'` 复验受影响的 6 个用例，全部通过。
- `bash examples/experiments/objects.sh ./build/debug/minigit`：保存复用、工作区编辑后读回旧 blob、重复初始化保留对象通过。
- 六份文档相对链接、实验脚本语法与 `git diff --check` 通过。

Index/add 验证：`ctest --preset debug -R 'unit\.(Index|Invalid index|Add|Selected add|Ignore rules|Invalid later add)|staging_cli|cli_basics' --output-on-failure` 和对应 sanitized 命令，各 11 个用例全部通过，无 ASan/UBSan 报告。覆盖损坏索引与校验和、排序和冲突阶段、二进制与空文件、可执行位、暂存后修改、选择性与全仓库删除、子目录参数、忽略与已跟踪例外、软链和原始 `missing/../file` 等无效路径、文件目录转换、失败保持原 Index。

引用／提交验证：Debug 和 sanitized 的 `-R 'unit\.(Commit|References|Snapshots|Identity)|commits_cli|objects_cli|staging_cli|cli_basics'` 首轮各 16 项通过。审查补充嵌套引用目录清理、重试同步和完整快照发布检查后，`-R 'unit\.(References|Commit)|commits_cli'` 最终各 10 项通过，无 ASan/UBSan 报告。`bash examples/experiments/commits.sh ./build/debug/minigit` 通过，脚本语法、六份文档 66 个相对链接与 `git diff --check` 通过。

新增覆盖：首提交与 parent 链、暂存后编辑、二进制／可执行快照、共享子 Tree 展开、深度边界、空提交与分离 HEAD、消息和身份拒绝、未解决冲突、损坏 HEAD、缺失或错误类型 Blob/Tree、CAS 与同名引用歧义、嵌套引用删除及失败重试、失败保持引用和身份文件。未引入 reflog 和多文件事务；引用发布不遍历所有历史祖先，DAG/log 已检测完整可达提交闭包中的缺失或损坏对象。

Status 验证：`ctest --preset debug -R 'unit\.Status|status_cli|cli_basics' --output-on-failure` 与 sanitized 对应命令各 8 项通过，无 ASan/UBSan 报告；六份文档 71 个相对链接及 `git diff --check` 通过。覆盖未出生仓库、未跟踪／暂存／工作区组合、可执行模式、删除与同路径未跟踪替代、文件目录转换、忽略例外、全部七种冲突阶段组合、路径转义、损坏 Index／对象／HEAD、软链与 FIFO；查询前后完整元数据字节保持一致。

DAG/log 验证：`ctest --preset debug -R 'unit\.(DAG|Log)|log_cli|cli_basics' --output-on-failure` 与 sanitized 对应命令各 6 项通过，无 ASan/UBSan 报告；六份文档 76 个相对链接及 `git diff --check` 通过。覆盖线性及菱形多父图、共享祖先去重、拓扑顺序与确定性并列顺序、多起点、重复父边、缺失／非 Commit／损坏祖先、纯算法输入的缺节点与环、空历史、起点解析、完整／单行输出和显示限制。

轻量 tag 验证：Debug 与 sanitized 的 `-R 'unit\.Tag|tags_cli|log_cli|cli_basics'` 各 6 项通过。阶段总验收使用 `ctest --preset debug --output-on-failure` 和 `ctest --preset sanitized --output-on-failure`，各 54 项全部通过，无 ASan/UBSan 报告。`bash examples/experiments/objects.sh ./build/debug/minigit` 与 `bash examples/experiments/commits.sh ./build/debug/minigit` 均通过；两个脚本语法、六份文档 80 个相对链接与 `git diff --check` 通过。

关键覆盖包括空与二进制 blob、SHA-256 已知向量、压缩截断及尾部数据、解压大小限制、错误类型／长度／哈希、Tree 顺序和非法名称、Commit 父顺序、多行消息与字段注入、相对路径、软链接及 `link/..`、FIFO、无效仓库、锁竞争和输出失败。

当前限制：使用 Linux/POSIX 接口；没有断电故障注入；单文件替换完成后的同步失败可能留下完整新文件；Commit 编解码只检查身份字段结构，业务 create_commit 已负责显式身份及运行时 UTC 时间；没有命令级断电事务或全祖先损坏审计。

验收条件：

- 初始化仓库后能发现仓库，空历史、重复初始化和非仓库位置均有定义行为。
- SHA-256 对象封装、zlib 保存与读取完成往返，长度、类型、摘要和压缩损坏均能拒绝。
- blob 保留二进制字节；tree 使用固定 32 字节摘要并确定排序；commit 编解码保留 tree、parent、身份、业务时间和消息。
- add 持久化版本化 Index，拒绝越界路径和软链接；普通文件模式及可执行位有验证。
- 首个和后续 commit 正确更新引用；提交读取暂存版本；status 区分已暂存、未暂存和未跟踪；log 展示正确父链。
- 轻量标签的创建、解析及非法名称检查有验证。
- 对象与可变元数据写入采用写锁、临时文件与原子替换，失败不能留下可见半成品。
- 临时仓库端到端用例覆盖文本、二进制、子目录、重复内容、暂存后再编辑和失败输入。

### Phase 1 阶段总结

本阶段形成真实快照闭环：字节保存为不可变 Blob，Index 独立保存待提交版本，目录 Tree 连接文件和模式，Commit 连接根 Tree 及 parent，分支引用发布历史，轻量标签保持选定位置。模块由单纯对象库扩展为 Index、Working Tree、Refs、History、DAG 与状态命令；CLI 仅解析参数和显示返回结果，核心测试直接调用 C++ 库。

最值得掌握的是三条边界：内容与名称分别由 Blob 和 Tree 保存；暂存内容不随工作区编辑变化；不可变对象与可变引用分开更新。拓扑顺序来自父关系，不来自作者时间；分支和标签轻量是因为只保存一个 ID。

与真实 Git 的主要差异：自有 SHA-256 仓库及 MGIX 格式；只处理普通 UTF-8 路径和可执行位，拒绝软链；简化忽略语法；标签只指向 Commit；不提供缩写解析、附注标签、packfile 或远程功能。当前验证平台为 Linux，单文件原子替换不构成多文件断电事务。

Phase 2 将复用快照、引用和 DAG。先增加差异引擎与可恢复工作区更新，再实现分支切换、恢复、重置、删除与移动；修改命令必须共同使用覆盖保护、事务状态和 reflog，避免逐命令重复实现或覆盖未提交内容。

## Phase 2：比较、分支与安全更新

计划范围为 `diff`、`show`、`branch`、`switch`、`restore`、`reset`、`rm`、`mv`。

验收条件：不同快照之间的差异和对象内容可检查；分支与分离 HEAD 行为明确；恢复和重置对 HEAD、Index、工作区的影响分别验证。覆盖未提交内容或未跟踪文件前执行保护检查。在 Phase 1 写锁与原子元数据更新基础上，扩展多文件事务、异常恢复和 reflog，并随修改命令一起验证，不能只验证正常路径。

## Phase 3：可恢复的合并与应用

计划范围为 `merge`、`cherry-pick`、`revert`、`stash`。

验收条件：快进与三方合并、文本和二进制冲突、父关系、暂存区冲突状态均有定义与验证。会暂停等待用户处理的操作，在同一交付中提供继续和中止能力；重启后能读取状态，中止恢复原先状态，继续拒绝尚未解决的冲突。stash 保存与恢复需要明确工作区和 Index 边界，并检查恢复冲突。

## Phase 4：历史编辑与对象回收

计划范围为 `rebase` 和 `gc`。

验收条件：rebase 的提交重放顺序、冲突处理、继续与中止经端到端验证；gc 有明确根集合和保留策略，能证明可达对象不会被误删，并覆盖无引用、reflog 及进行中操作的保护要求。packfile 与网络能力不自动纳入该阶段。

## 下一步

Phase 1 已验收，下一步先处理暂存过程的内存占用，再引入 Phase 2 的差异与安全工作区更新，持续推进至 Phase 4。每次更新本页同时补充实际验证命令、结果、能力边界和未完成验收条件。详细设计见 [架构](architecture.md)，学习路线见 [README](../README.md)。
