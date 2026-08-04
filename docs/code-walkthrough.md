# 代码导读

当前实现包括 Phase 0 构建基础、Phase 1 的初始化、对象库、Index、引用、快照及对应命令，以及 Phase 2 的差异引擎、diff/show 查询与短事务恢复。Reflog 编解码、移动规划及 commit 事务发布已通过子项验收，branch 管理已通过子项验收，共享工作区与 Index 规划器已验收，switch 工作区切换已通过子项验收；restore 内容恢复已通过子项验收；reset 三模式重置已通过子项验收；rm 删除与取消暂存已通过子项验收；mv 文件与目录迁移已通过子项验收。Phase 3 纯文本引擎已通过子项验收，SnapshotMerge 适配器已通过子项验收，single-lowest merge-base helper 已通过子项验收，显式 Directory target 已通过子项验收，CommitSnapshot 存储工厂已通过子项验收，OperationState v1 基础已通过子项验收，Merge 生命周期已通过子项验收，CherryPick／Revert 已通过子项验收，Stash 保存子项已通过验收。完成和验证状态见 [进度](progress.md)。

## 工程目录

```text
CMakeLists.txt / CMakePresets.json  构建与验证预设
include/minigit/                   核心模型与公开函数接口
src/core/                          对象、暂存、快照、引用与历史实现
src/cli/main.cpp                   参数解析与结果显示
tests/unit/                        Catch2 模块与数据不变量测试
tests/integration/                 CMake 临时仓库命令串
tests/support/                     临时目录清理辅助
examples/experiments/               可独立重放的学习实验
docs/                              架构、原理、导读、笔记与验收
```

核心逻辑直接链接为 `minigit_core`，测试可绕过 CLI 调用接口；CLI 测试再检查真实进程与磁盘结果。命令协调目前使用明确的自由函数，随历史操作复杂度增加再调整文件边界。

## 从一次命令开始

从 [src/cli/main.cpp](../src/cli/main.cpp) 的 `main` 和 `run` 开始：帮助与版本不访问工作区，`init` 调用仓库初始化，`hash-object` 读取文件并可选写入对象，`cat-file` 发现仓库并读取对象。成功返回 `0`，参数错误返回 `2`，文件、仓库或对象等运行错误返回 `1`，错误统一写入标准错误。

版本函数声明位于 [include/minigit/version.hpp](../include/minigit/version.hpp)，实现在 [src/core/version.cpp](../src/core/version.cpp)，当前返回 `0.1.0`。这个小边界验证了 CLI 与核心库的连接，为后续业务模块提供可构建起点。

构建定义在 [CMakeLists.txt](../CMakeLists.txt)，预设在 [CMakePresets.json](../CMakePresets.json)。`minigit_core` 为 C++20 库，CLI 链接它；SHA-256 使用 OpenSSL 3 EVP，松散对象使用 zlib。CLI 集成测试包括 [cli_basics.cmake](../tests/integration/cli_basics.cmake) 和 [objects_cli.cmake](../tests/integration/objects_cli.cmake)，核心单元测试使用 Catch2 v3。

Debug 工作流：

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Debug 和 sanitized 的 `cli_basics` 均已通过，验证帮助和别名、版本格式、空参数、未知参数和多余参数，并检查输出失败的退出码。具体证据由 [进度](progress.md) 记录。当前 sanitized 使用 `-g -O1`，优化可能改变单步次序与局部变量可见性；编译参数核对及验证范围见 [构建与验证策略](testing.md)。

## 当前对象库阅读路线

1. [bytes.hpp](../include/minigit/bytes.hpp)：`Bytes` 是字节向量，文本转换辅助函数不改变 payload 的二进制含义。
2. [object_id.hpp](../include/minigit/object_id.hpp) 与 [object_id.cpp](../src/core/object_id.cpp)：`ObjectId::from_hex/from_bytes/hex/bytes` 分离显示与原始摘要，`sha256` 封装 EVP。
3. [objects.hpp](../include/minigit/objects.hpp) 与 [objects.cpp](../src/core/objects.cpp)：`hash_object` 构造类型、字节长度和 NUL 头；`encode_tree/decode_tree`、`encode_commit/decode_commit` 处理 payload。
4. [fs.hpp](../include/minigit/fs.hpp) 与 [fs.cpp](../src/core/fs.cpp)：`read_file` 拒绝软链接和特殊文件，`atomic_write` 处理临时文件与同步，`RepoLock` 管理持久锁文件的互斥生命周期。
5. [repository.hpp](../include/minigit/repository.hpp)：`Repository::init/discover` 提供 `.minigit` 根和元数据路径，不承担全部命令业务。
6. [object_store.hpp](../include/minigit/object_store.hpp) 与 [object_store.cpp](../src/core/object_store.cpp)：`write` 存储 zlib 对象，`read` 校验封装，`object_path` 将完整摘要映射到两级路径。

沿 CLI 的 `hash-object -w` 阅读可看到：先读文件字节，发现仓库，取得 `RepoLock`，调用 `ObjectStore::write`，打印完整摘要。ObjectStore 不自行取得写锁，调用者必须保证锁生命周期覆盖写入。`cat-file -p` 对 blob 使用原始字节输出，对 tree 解码后逐条显示。

文件接口的下一条阅读链是 `read_file_data` → 同一 fd 的读取前后 stat → 字节和权限，工作区的 `read_worktree_file` 用这一份数据计算 Blob ID 及 `100644/100755`。恢复需要精确权限时使用 `atomic_write_with_permissions`，顺序为写内容 → fchmod 临时文件 → 核对权限 → 同步 → 替换 → 同步目录。默认元数据写入复用同一内部流程，但不强制覆盖 umask。相关反例仍集中在文件工具与暂存单元测试，不新增另一个文件工具实现。

运行 [对象实验](../examples/experiments/objects.sh) 可观察保存前后对象内容不变；实验已通过，验证细节见 [进度](progress.md)。

保护对象格式的测试位于 [test_objects.cpp](../tests/unit/test_objects.cpp)，文件路径、初始化和进程锁测试位于 [test_repository.cpp](../tests/unit/test_repository.cpp)。先阅读损坏对象、相对路径和 `link/..` 的反例，再回到实现检查错误如何被阻止。

## 业务实现后的阅读顺序

暂存模块从 [index.hpp](../include/minigit/index.hpp)、[index.cpp](../src/core/index.cpp) 阅读校验与编解码，再到 [worktree.hpp](../include/minigit/worktree.hpp)、[worktree.cpp](../src/core/worktree.cpp) 的 `add_paths`。调用链是参数选择 → 路径验证 → 读取选择范围 → 写入 Blob → 移除旧条目或冲突阶段 → 保存完整 Index。文件字节逐份保存并释放，暂存流程只长期保留路径、模式和 ID；后续选择失败仍保持旧 Index，可能留下可回收的对象。单元测试 [test_staging.cpp](../tests/unit/test_staging.cpp) 直接读回 Blob，检查暂存后编辑、删除和目录类型转换，避免仅凭 CLI 成功判断暂存语义。

| 顺序 | 模块或类型 | 阅读时要回答的问题 |
| --- | --- | --- |
| 1 | 错误类型与文件工具 | 错误是否带上下文？读取失败与格式损坏能否区分？ |
| 2 | `ObjectId` | 如何区分固定 32 字节和 64 位显示字符串？非法输入在哪里拒绝？ |
| 3 | 对象编码与 `ObjectStore` | 哈希的确切输入是什么？读取如何验证长度、类型和摘要？ |
| 4 | Blob / Tree / Commit | 二进制如何保留？排序和身份元数据如何影响确定性？ |
| 5 | 仓库发现与 `init` | 如何识别仓库？重复初始化和半成品目录如何处理？ |
| 6 | Index 与 `add` | 暂存的是哪个版本？路径越界和软链接如何拒绝？ |
| 7 | Refs / HEAD 与 `commit` | 空历史怎样表示？引用何时更新？中途失败留下什么？ |
| 8 | `status`、`log`、轻量 `tag` | 比较哪些状态？历史如何遍历？引用如何校验？ |
| 9 | Diff 与 Inspection | 差异怎样计算？历史、Index 与工作文件的内容来源如何区分？ |
| 10 | Transaction、Reflog、Branches | 引用及日志怎样一起发布？分支管理为何不改变工作区？ |
| 11 | WorktreeUpdate | 部分目标、精确授权与实际 before 怎样变成事务计划？ |
| 12 | Switch | 四状态矩阵怎样保留兼容修改并拒绝覆盖？ |
| 13 | Restore | 来源、路径范围与 Work／Index 目的如何分开？ |
| 14 | Reset | 模式如何扩大发布范围，同 ID 为何仍处理 Index／Work？ |
| 15 | Remove（已验收） | 为何 cached 有两条安全路径，force 仍保护未知内容？ |
| 16 | Move（已验收） | Index 路径映射怎样与实际工作字节迁移分开？ |
| 17 | 纯文本合并（已验收） | 基线坐标、点插入、完整区块重放与 LF 怎样决定冲突？ |
| 18 | SnapshotMerge（已验收） | 原始 stages、marker 工作内容与 D/F 区域如何区分？ |
| 19 | Single-lowest merge base（已验收） | 如何从完整父图剔除非最低共同祖先？ |
| 20 | CommitSnapshot 工厂（已验收） | 显式内容存储为何不能从当前 Index 或 HEAD 推断？ |
| 21 | OperationState v1（已验收） | 状态、语义根与业务 guard 怎样分工？ |
| 22 | Merge 生命周期（已验收） | start／continue／abort 如何组合已验收的 primitives？ |
| 23 | CherryPick／Revert（已验收） | 如何由来源父快照决定正向／反向变化？ |
| 24 | Stash 保存（已验收） | H/I/W/U 与活跃栈、回滚镜像怎样分开？ |

表中二十四项已有接口，阅读链接见相关章节；分支管理见新增章节，共享更新规划器见新增章节；switch 已接入，Restore 内容恢复已通过子项验收，Reset 已通过子项验收，Remove 已通过子项验收，Move 已通过子项验收。依赖关系见 [架构](architecture.md)。

## 从 Index 到提交

读取 [refs.hpp](../include/minigit/refs.hpp)、[refs.cpp](../src/core/refs.cpp)，先看 `read_head` 怎样表示未出生分支，再看 `update_ref` 如何检查预期旧值。读取 [snapshot.hpp](../include/minigit/snapshot.hpp)、[snapshot.cpp](../src/core/snapshot.cpp) 的 `write_snapshot/read_snapshot`，观察路径映射如何递归构造 Tree，以及每个 Tree 条目的模式如何约束所引用的对象类型。

然后阅读 [history.hpp](../include/minigit/history.hpp)、[history.cpp](../src/core/history.cpp) 的提交协调与身份。`create_commit` 的调用链是：获取写锁 → 读取配置身份和 Index → 检查冲突条目 → 构造根 Tree → 校验旧 HEAD → 保存带 parent 的 Commit → `plan_move_head` → `execute_transaction`。失败可能留下未引用对象或待恢复事务；提交目标先完整保存，引用与 reflog 随后共同发布。底层编码保留读取教学对象的自由，业务层要求身份和消息有效。

[test_history.cpp](../tests/unit/test_history.cpp) 检查快照、parent 与失败边界；[commits_cli.cmake](../tests/integration/commits_cli.cmake) 在工作区编辑后直接读取已提交 Tree 和 Blob，证明提交来源是 Index；[提交实验](../examples/experiments/commits.sh) 可重复演示同一机制。

## 先阅读纯差异算法

新增差异引擎从 [diff.hpp](../include/minigit/diff.hpp) 进入 [diff.cpp](../src/core/diff.cpp)。`split_lines` 把字节拆成借用视图和末尾 LF 标记，`diff_lines` 去掉共同前后缀、生成精确行 ID，并从 LCS 表回溯成 EditScript。Edit 的坐标同时指向完整旧行和新行序列，不随先前删除或插入改变，便于后续 Merge 统一在 base 坐标下比较。

`make_hunks` 校验区间、顺序和未改动间隔，再把相邻上下文合并；`format_file_diff` 负责缺失文件、模式、摘要、二进制和无末尾换行等显示策略。它不访问仓库，也不把工作区未保存的 ID 当成已存在对象。

从 CLI 的 `diff` 分支进入 [inspection.cpp](../src/core/inspection.cpp) 的 `repository_diff`：验证请求 → 获取锁 → 解析修订或读取 Index → 选定候选路径 → 从对象库或 WorktreeFile 取得两侧 → 纯格式化 → 返回完整结果。`diff_snapshots` 验证对象类型，即使文件没有变化，也不能以相等 ID 掩盖损坏对象。双修订比较独立于工作区和 Index，便于在冲突现场仍检查历史。

[test_inspection.cpp](../tests/unit/test_inspection.cpp) 验证内容来源与读取失败不改变元数据；[diff_cli.cmake](../tests/integration/diff_cli.cmake) 重放真实命令、上下文别名、模式与删除、损坏 Index 和错误退出码；[差异实验](../examples/experiments/diff.sh) 让三棵树关系可重复观察。

`show` 从同一 Inspection 模块的 `show_object` 阅读。先区分是否有路径选择：提交路径展开 Tree 后查询，Index 路径按 stage 查找；两者最终读取 Blob 原始字节。没有路径时，完整对象 ID 支持 Blob/Tree/Commit，普通名称解析提交；Commit 使用 `format_commit` 的元数据文本及 `diff_snapshots` 的第一父差异。log 也调用同一纯格式化函数，避免两套消息缩进规则。[show_cli.cmake](../tests/integration/show_cli.cmake) 检查 root/parent、暂存与历史来源、嵌套空格路径、错误不产生部分输出；[内容实验](../examples/experiments/show.sh) 再通过文件重定向及 cmp 验证二进制原样输出。

[test_diff.cpp](../tests/unit/test_diff.cpp) 的关键断言是把编辑实际应用到旧行后得到完整新字节。阅读预算回退、无 LF 和 hunk 零长度反例，有助于区分算法正确性与补丁是否精简。纯算法直接使用 C++ 库测试，不需要构造提交。

## 三棵树比较

从 [status.cpp](../src/core/status.cpp) 的 `collect_status` 阅读：加载 HEAD 快照及 Index，验证引用的 Blob，再扫描工作区。正常条目分别比较 HEAD→Index 和 Index→工作区；冲突路径读取 stage 集合，未跟踪路径另行枚举。先生成完整 `StatusReport`，再由 CLI 输出，读取失败不会留下半份状态报告。

[test_status.cpp](../tests/unit/test_status.cpp) 检查 `MM`、模式变化、删除、忽略例外、冲突和损坏；[status_cli.cmake](../tests/integration/status_cli.cmake) 通过命令串观察 `?? → A → AM → 未暂存修改 → MM → 删除`。[format.cpp](../src/core/format.cpp) 的 `quote_path` 将空格、换行及控制字节转义，避免一个文件名伪装成多行报告。

Snapshot 持有路径、模式和对象 ID 的通用模型；Working Tree 负责文件选择与读取；History 负责可变 HEAD 语义与提交。将这三项分开，使引用验证与 Diff 复用快照转换时无需拉入命令层逻辑。

## 从一条父链到 DAG

[dag.cpp](../src/core/dag.cpp) 的 `load_commit_graph` 收集所有可达 Commit，使用发现集合避免共享祖先重复入队。`topological_order` 为每条 parent 边统计尚未输出的子提交数，用有序集合维护就绪节点；输出一个提交后降低其父节点的计数，只有所有子边已输出，父节点才就绪。

[test_dag.cpp](../tests/unit/test_dag.cpp) 手工保存线性、多父及共享祖先对象，验证去重和拓扑约束，同时覆盖缺失父对象、错误类型及纯算法输入的环。[log_cli.cmake](../tests/integration/log_cli.cmake) 检查实际命令的顺序、身份、消息、起点与显示限制。输入时间不决定顺序，因而历史关系不会被时钟偏差改变。

## 轻量标签为何不需要新对象

[tags.cpp](../src/core/tags.cpp) 的 `create_tag` 获取写锁、校验名称、解析修订、检查已有标签并调用 `update_ref`。`list_tags` 与 `delete_tag` 复用同一引用存储；[test_tags.cpp](../tests/unit/test_tags.cpp) 检查对象库不增加、HEAD/Index 保持原值、标签固定目标及失败保护。[tags_cli.cmake](../tests/integration/tags_cli.cmake) 将 tag 与 log 组合，证明新 commit 前进分支，旧标签仍展示原历史。

## 跟踪一份文件的版本

Phase 1 验收后，选择一个含多行文本的普通文件，在测试仓库依次完成创建、add、修改、status 和 commit。阅读时同时记录工作区字节、Index 指向的 blob 和 HEAD tree 中的 blob，不只观察 CLI 提示。

当工作区从 `A` 变为 `B`，add 暂存 `B`，随后又编辑成 `C`，一次 commit 应记录 `B`。若源码在 commit 时重新读取文件并存为 `C`，就破坏了暂存区语义。相应测试应比较对象内容，而不是只检查命令返回成功。

随后为同一目录改变文件遍历顺序，检查 tree 字节是否一致；向对象文件注入截断或错误数据，检查读取是否拒绝。通过不变量追踪比逐行阅读压缩库调用更容易理解边界。

## 验证层次

- 单元测试：摘要编码、对象头解析、zlib 往返、tree 顺序、commit 编解码、Index 格式。
- 集成测试：在临时仓库组合仓库发现、对象写入、Index 与引用更新。
- 端到端测试：从 CLI 验证退出码、输出和真实磁盘状态，测试结束清理临时目录。

测试名称应描述外部行为或数据不变量。需要构造损坏数据时保留最小反例，避免用和实现相同的算法生成预期结果。安全更新机制引入后，对失败边界和未提交内容覆盖保护补充有针对性的集成用例。

### 从多文件修改到恢复

事务的阅读入口是 [transaction.hpp](../include/minigit/transaction.hpp)，再读 [transaction.cpp](../src/core/transaction.cpp)。`TransactionPlan` 明确列出每个工作节点和元数据的 before/after；调用者负责业务语义，执行器负责验证、持久化日志、发布和恢复。二者的边界让 switch、restore 以及后续冲突操作复用同一故障机制。

`execute_transaction` 必须在调用者已持有 RepoLock 时调用；它不能再次取得锁。准备阶段将实际旧文件保存为 Blob，构造内部暂存文件，再发布 Prepared 日志。工作区文件按拓扑顺序处理，元数据随后发布，最后原子将日志改为 Committed。`recover_transaction` 按日志状态选择 before 或 after，先完成全局检查再写入；不能一边检查一边恢复，以免后面的外部修改使前面的文件已经被覆盖。

Index 增加独立的 `encode_index/decode_index`，`save_index/load_index` 复用同一编解码流程。这不是新索引格式，而是让事务先在内存中准备准确的新 Index 字节，随后与 HEAD、引用等一起发布。安全删除、目录权限和内部暂存节点发布集中在文件工具中；业务模块不用重复实现系统调用。

[transaction.cpp 教学程序](../examples/experiments/transaction.cpp) 展示公共接口的最小调用链：实际节点备份 → 生成两个目标 Blob → TransactionPlan → durable-write 回调中断。它是按需构建的实验目标，不参与生产 CLI 命令解析；[脚本](../examples/experiments/transactions.sh) 用 CLI 完成恢复并逐字节比较文件。

## 从引用移动读懂 C++ 异常保证

先读 [reflog.hpp](../include/minigit/reflog.hpp) 的 `ReflogEntry`：`optional<ObjectId>` 表示端点可能不存在，`std::string` 保存原因，`vector` 保留事件顺序。再读 [reflog.cpp](../src/core/reflog.cpp) 的 `encode_reflog/decode_reflog`；不要把这些结构体的内存直接写盘，因为填充、指针和机器端序并不是磁盘协议。

| 函数 | 阅读时追踪的问题 |
| --- | --- |
| `validate_reason` | UTF-8 如何按字节检查？为何长度限制统计字节，禁止控制字符后原因才能安全显示在一行？ |
| `Reader::take`、`Reader::number` | 借用的 `span` 怎样先检查剩余长度，再取字段？大端整数如何由移位组合？ |
| `read_reflog` | 日志缺失与损坏为什么走不同结果？调用者怎样保证仓库锁已取得？ |
| `plan_append_reflog` | 追加到已存在的计划时怎样读取 after，怎样核对尾项连续性？ |
| `plan_move_head` | 相同 ID、不同符号分支为何仍是移动？当前分支 commit 与 detached commit 分别规划哪些文件？ |
| `create_commit`、`execute_transaction` | 不可变对象、可变引用与日志分别在何时保存？ |
| `recover_transaction`、`transaction_roots` | 为什么恢复不调用追加函数？日志字节之外还需要保留哪些对象？ |

这里的 `span<const uint8_t>` 借用调用者缓冲区，不拥有内存；Reader 只在解码期间借用它，返回条目的 reason 则复制到拥有自身存储的 string。`take` 先比较 `size > remaining`，避免用可能溢出的 `position + size` 判断边界。编码预先计算有界大小，再 reserve，减少逐字段扩容，但 reserve 不能替代输入校验。

两个规划入口先执行 `auto next = plan`，把可能失败的读取、检查、分配和编码放在副本上，最后才 `plan = std::move(next)`。这是强异常保证的具体例子：失败时调用者的计划仍是原值，磁盘尚未修改。它与事务的磁盘恢复保证分属两个层次；不能从 C++ 容器回滚推导出文件系统会自动回滚。

[reflog 单元测试](../tests/unit/test_reflog.cpp) 覆盖格式、原因与边界、移动规划、异常保护及事务恢复；[reflog CLI 测试](../tests/integration/reflog_cli.cmake) 检查实际命令的倒序输出和错误行为。[引用日志实验](../examples/experiments/reflog.sh) 用两个 commit 核对 HEAD 与 main 的日志一致，可与 `log --oneline` 对照阅读事件顺序和父图。

## 从分支列表到安全删除

从 [branches.hpp](../include/minigit/branches.hpp) 阅读 `BranchInfo`，再进入 [branches.cpp](../src/core/branches.cpp)。`branch_ref` 将验证后的短名转换成唯一 namespace，避免业务代码四处拼接 refs/heads。列表把当前 unborn 分支补进结果，而不是要求磁盘已经存在对应 ref；detached HEAD 只验证当前提交，不把某条同 ID 分支误标成当前。

`list_branches` 用 map 按名称排序，用 set 保存已经验证的 ObjectId。相同提交只展开一次 Snapshot，随后将 BranchInfo 移动到返回 vector。这展示了几个不同职责：map 管唯一名称与顺序，set 只管校验去重，vector 提供稳定返回序列。64 MiB 短名预算限制输入名称数据；它不表示必须长期持有所有分支的文件内容。

`create_branch` 是自持锁的业务入口：解析目标 → `plan_create_branch` → `execute_transaction`。规划接口不再次加锁，先完成目标快照检查，再复制计划并追加 ref 和分支日志，成功才移动赋回；switch -c 复用它。当前分支保护先于“目标 ID 是否相同”的判断，非当前分支 same-ID 强制移动才是无操作。

`delete_branch` 的普通模式调用 `load_commit_graph` 收集 HEAD 闭包，再执行 `topological_order`，最后用 `graph.contains(branch_id)` 判断。发现集合负责去重，不能单靠它证明无环；拓扑校验因此不是多余的一步。强制模式只省略图可达性判断，仍进入同一事务验证，不能跳过旧 ref 和日志检查。

[test_branches.cpp](../tests/unit/test_branches.cpp) 与 [branches_cli.cmake](../tests/integration/branches_cli.cmake) 是分支行为的验证入口；相关定向检查已通过，证据见进度页。阅读 [分支实验](../examples/experiments/branches.sh)，同时观察 topic=A、main=B、HEAD、Index 和 note 内容，确认创建引用不等于切换工作区。再跟踪 ref／日志删除与空目录 prune，理解 topic/sub 清理后为何可以重用 topic 名称。

## 核心 API 练习：先规划，再统一执行

[worktree_update.hpp](../include/minigit/worktree_update.hpp) 中，`WorktreeTargets` 是按路径索引的 map，owned_paths 是 set，两者承担目标描述与覆盖授权的不同职责。`WorktreeTarget` 的两个 optional 与默认 directory=false 字段区分缺失、空 Blob、显式 Directory 及默认／明确权限；不要把缺失路径偷换成空文件或空目录。

下面演示单路径临时仓库的调用顺序：假设唯一旧跟踪文件是 dir/note，业务层已确认它的当前修改可以覆盖。Index 参数是完整目标 Index，而 worktree targets 可以只描述部分路径。

```cpp
RepoLock lock(repo.metadata());
ObjectStore store(repo.metadata() / "objects");
const auto blob = store.write(ObjectType::Blob, to_bytes("new\n"));
TransactionPlan plan;
const WorktreeTargets targets{
    {"dir/note", {FileState{"100644", blob}, std::nullopt}}
};
plan_worktree_update(repo, plan, targets, {"dir/note"});
plan_index_update(repo, plan, Index{{"dir/note", "100644", blob}});
execute_transaction(repo, std::move(plan));
```

前两次调用只准备计划，工作区规划可以写不可变备份 Blob；最后一次才发布实际文件及 Index。这个例子没有移动 HEAD，并未调用 switch。若目标改为删除普通文件，使用 WorktreeTarget{std::nullopt, std::nullopt}；同一计划若还包含它下面的新文件，该空目标允许旧文件被移除，再由必要父目录补齐把位置变成 Directory。若要保留零字节文件，应先保存空 Blob 再提供 FileState。多路径仓库需保留完整 Index 的其他条目，不能把部分 worktree targets 直接当作整个 Index。

所有调用共用外层 RepoLock，规划函数不重复取得锁；已有 metadata 可以继续组合，但 worktree 规划要求 plan.work 为空，Index 规划拒绝重复的 index MetadataChange。异常保护针对调用者的计划值，之前已经保存的备份对象不会被 C++ 容器回滚删除。

实现接着从 [worktree_update.cpp](../src/core/worktree_update.cpp) 阅读：`validate_paths` 用模板参数和取键 lambda 共用 map/set 的路径检查，随后校验所有目标模式与 Blob，推导必要父目录。私有 `current` 缓存轻量 NodeImage，`inventory` 对要移除的目录逐层检查授权及显式删除；文件内容读取后释放，不把全部字节塞进缓存。

`owned_directory` 使用 set 的 `lower_bound(path + "/")` 查询是否存在已授权后代，必须再检查完整分隔符前缀，不能把 dir-extra 当作 dir 的子项。精确 owned 的文件位置本身若变为空目录，也视为已授权位置；递归到陌生空子目录时要重新判断，父授权不传给它。新目标补出来的父目录与已有父目录分别处理：已有目录权限保留、状态相同则不记录 WorkChange，因此局部更新无需清理无关兄弟。

目录扫描可以往 desired map 加入旧子项；实现先保存初始 roots，再展开递归清单，避免新插入节点改变本轮入口集合。map 插入不会使已有元素引用失效，因此 `after` 和 `before` 的引用可以保持到当前检查结束。这里只缓存对象 ID、节点类型及权限，不拥有文件缓冲区。desired 与 actual 每次加入新键前还需检查 200000 节点／64 MiB 展开名称预算，不能等所有父路径已经分配后才看最终 changed 数量。

所有变化路径先检查完，最后才逐项 `capture_work_node` 保存实际 before；捕获值须与此前 NodeImage 一致，否则拒绝期间发生的变化。`auto next = plan` 隔离可抛异常的追加，完成后 move 赋回。`plan_index_update` 再独立编码完整目标、读取精确旧字节、校验全部 stage 的 typed Blob，并拒绝已有 index 计划。函数没有发布动作，最终发布仍由 Transaction 执行。

[test_worktree_update.cpp](../tests/unit/test_worktree_update.cpp) 直接调用这些 API：多文件及空 Blob、明确权限、新父目录、双向文件目录转换、未变父目录的未知兄弟保护、精确授权、Index 缺失与冲突 stage、格式和预算拒绝，以及 Prepared／Committed 后工作区与 Index 的联合恢复。Debug 定向检查已通过，完整验证证据见进度页；本项没有 CLI 用例或命令实验。已有事务的完整崩溃矩阵不由这里重新实现，新测试只验证规划结果能交给共享执行器。

### 显式目录目标：检查权限也检查子项

共享 planner 的 directory=false 默认字段扩展已通过子项验收。用 WorktreeTarget{std::nullopt, 0700U, true} 描述显式 Directory，区别于原两字段的 Absent；file 与 directory 不能同时存在。既有调用仍可保持两字段 aggregate，默认第三字段为 false。冻结后的 header 与下述私有链已核对。

阅读时分别找删除 inventory 与保留 inventory：前者要求旧文件明确删除，后者为目录权限变化保留已知子项，并生成必要的 before==after WorkChange。它不是随意增加无操作，而是满足 Transaction 对记录目录的完整实际子项契约。所有目标拓扑、权限和完整预算须先通过，之后才 capture；强异常保证仍保留原 plan。实际链为目录 schema／权限检查 → File 与 Directory 正目标父扩展 → current 缓存 → preserve_directory_permissions → inventory 的 keep／delete 访问记录与 forced 集合 → changed∪forced → 两侧拓扑／含后代目录 owner-rwx／validate_transaction_budget → capture 值一致性检查 → move 赋回 next。inventoried 的访问模式与 forced 同值记录分开，避免文件 noop 干扰目录遍历；KEEP 遇到明确的子目录删除／替换再转 DELETE，不擅自保留其旧子文件。

[test_worktree_update.cpp](../tests/unit/test_worktree_update.cpp) 新增六组目录语义：空节点／完整权限与默认保持、双向文件目录转换／显式祖先权限、已知后代 noops 与普通执行及 Prepared／Committed 恢复、未知子项保护／相同目录跳过 inventory、schema／两侧权限与拓扑拒绝先于 backup，以及只读目录／文件双向转换的 Prepared／Committed 四个恢复场景。测试用真实执行核对包含 forced 子项的计划是否满足现有 Transaction，目录 inventory、外部修改保护与恢复镜像的安全契约没有放宽；执行器发布方式在本项中有必要修改。

初次实际运行发现 Linux 跨父移动只读目录需要 owner-write；当前 transaction.cpp 对 current=Absent 的 readonly Directory 改用 create_directory_exact(..., true)，可写目录仍由暂存 rename。fs.hpp 的第三 default bool exclusive=false 保持既有调用，true 遇 EEXIST 立即拒绝且不 chmod；[test_repository.cpp](../tests/unit/test_repository.cpp) 已在现有文件工具组核对这项权限保护。mkdir／最终 chmod 之间若中断留下非 before／after 的 0000／0700 节点，恢复严格拒绝，不把公开步骤钩子测试扩大为任意 syscall 内部可恢复承诺。修正后两种预设构建与定向回归均通过，精确范围见进度页。无新 CLI；复用既有事务矩阵回归，没有复制矩阵实现。

## Switch：组合策略与现有规划器

先读 [switch.hpp](../include/minigit/switch.hpp) 的 SwitchOptions，再读 [switch.cpp](../src/core/switch.cpp) 的 `switch_worktree`。入口只取得一次 RepoLock：检查待恢复事务 → 选择目标 Head → 读取 A/D 快照与 Index → 第一遍 A/B/D 选择 → 完整目标 Index 预算与编码校验 → 第二遍更新路径 C 校验 → 组合各规划器 → execute_transaction。返回 Head 供 CLI 输出短分支或 detached 完整 ID。

私有 `validate_index_budget` 同时限制原 Index 与最终组合 Index；`state_at` 返回 optional<FileState>，让 map 中无条目和零字节 Blob 区分；`checked_index` 对同 ID 去重校验 typed Blob，再拒绝任何非零 stage。因此底层 Index 规划支持冲突阶段，不代表 switch 可以携带冲突切换。即使目标 Head 没有变化，入口仍校验 Index 和快照。

第一遍路径循环比较 `old == target || staged == target`，满足就 continue；其余要求 staged==old，并先构造 next_index 和 targets，不读取 C。完整目标 Index 随后校验 100000 条目／64 MiB 数据预算及编码中的 D/F 约束；两份合法输入的组合仍可能超限，不能把逐路径判断误当成全局证明。第二遍只为 targets 调用 read_worktree_entry 并要求工作状态等于原 B。这既保留无关修改，也让预算拒绝发生在工作区读取与备份捕获之前；需要覆盖的节点仍严格检查。

调用顺序随后为可选 plan_create_branch → plan_move_head → 必要的 plan_index_update → plan_worktree_update。它们都是 caller-lock 规划接口，不能替换成会再次加锁的 create_branch。创建分支的 ref 已在 plan 中，HEAD 规划可据此核对目标；最后仅在计划确实有变化时 execute_transaction。备份对象可能在发布前写入，但 ref、Index 和工作文件不会逐步骤提前公布。

`new_branch` 与 detach 互斥；未出生且新分支没有显式来源时，desired.id 保持 nullopt，这条路径不尝试解析不存在的 HEAD Commit。Head equality 同时比较 symbolic 和 id，所以两条同 ID 分支依然是切换事件。

[test_switch.cpp](../tests/unit/test_switch.cpp) 覆盖矩阵保留与拒绝、完整 Index 拓扑、模式／空文件／双向 D/F 转换、未知与忽略内容、原子 -c、unborn／detached／同名标签及损坏输入；[switch_cli.cmake](../tests/integration/switch_cli.cmake) 再核对真实进程和磁盘结果。相关定向检查、预算补充复验与切换实验的证据见进度页。

## Restore：先匹配全部输入，再组合目的

[restore.hpp](../include/minigit/restore.hpp) 的 RestoreOptions 分开 source、staged、worktree；[restore.cpp](../src/core/restore.cpp) 的 restore_paths 取得一次 RepoLock，先检查待恢复事务。来源选择内容，路径选择范围，两个标志选择发布目的；HEAD 本身在全部方向保持原位。

实际调用链为 checkout_support::normalize_selectors → load_index／check_index → 选择来源 → select_paths → 必要的 next_index 预算／编码／排序校验 → Work 目标与 owned → plan_index_update／plan_worktree_update → 至多一次 execute_transaction。共享 normalize_selectors 仅作词法规范化，支持相对当前目录与绝对路径；Index-only 因此不读取物理软链父目录或 FIFO。

Restore 私有 check_index 先调用共享 check_index_objects，校验 100000 不同路径、300000 stage 记录及编码字节预算，按 ID 去重检查全部 typed Blob，再分别收集 stage 0 来源与冲突路径。默认 Index 来源仅拒绝 selected 冲突，不让未选冲突阻止正常文件恢复；显式历史来源先验证完整快照，即使选择部分路径也不能忽略来源其他损坏对象。

Restore 私有 select_paths 组合 source∪Index 候选，再调用共享 select_known_paths；它用排序候选集的 exact 查找与 lower_bound(path + "/") 范围，保留分隔符边界。每个原参数先确认至少一个匹配，再合并重复及祖先范围，避免有效目录掩盖无效参数；排序的祖先检查跳过已覆盖扫描，不逐参数遍历所有文件。

staged 目的从 next_index 移除 selected 全阶段，再加入来源 stage 0，保留 unselected 原条目；完整预算与 encode_index 先通过，随后按 path／stage 排序，才能比较是否无操作。工作区目标为来源 FileState 或 nullopt，owned 来自旧 HEAD 和原 Index 全阶段，文件内容捕获由共享 planner 完成。它不调用 plan_move_head，失败也不先公布部分 Index。成功的 Index-only 事务可保存元数据镜像 Blob，因此“不读取 Work”不能作为对象数量不变的依据；无操作与预算失败另有对应保护。

[test_restore.cpp](../tests/unit/test_restore.cpp) 已写入八组场景，包含方向选择、词法路径、冲突范围、unborn、空文件／删除、双向 D/F、未知保护、对象／预算／最终拓扑及事务恢复；[restore_cli.cmake](../tests/integration/restore_cli.cmake) 核对真实参数和磁盘状态。[restore.sh](../examples/experiments/restore.sh) 重放独立来源／目的及 HEAD／日志不变。两种预设的相关测试与实验已通过，具体筛选及复验记录见进度页。

## Reset：枚举模式与可选修订避免混淆无操作

从 [reset.hpp](../include/minigit/reset.hpp) 的 enum class 与 optional revision，读到 [reset.cpp](../src/core/reset.cpp) 的 reset_repository。入口一次锁和 pending guard 后，校验模式 → check_index → 选择 target_id 并完整读取 target → mixed／hard 的 after_index 预算与编码 → hard 的 paths／owned／targets → plan_move_head／Index／Work 规划 → 非空计划 execute_transaction。

Reset 私有 check_index 先调用共享 check_index_objects，对全部 stages 的 ID 去重检查 typed Blob，soft 再拒绝非零 stage；不能因为 soft 不改 Index 就跳过损坏校验。硬重置的 BoundedPaths 按新键检查数量和字节预算，旧 HEAD 与 Index 加入 owned，目标独有路径只加入目标，不自动得到覆盖许可。

plan_move_head 使用原 before_head.symbolic 与 target_id，保留当前分支名称／detached 状态。只有 Head 真正变化才写日志；没有按 ID 提前返回，after_index 与实际工作目标仍独立比较。空 optional target_id 只来自默认 unborn，显式 HEAD 走 resolve_commit 并报错。共享 planner 先做完整预算与拓扑检查，再捕获真实编辑文件，最后一次事务发布。

[reset.sh](../examples/experiments/reset.sh) 已准备模式、默认、unborn、detached、same-ID dirty 以及 D/F 未知保护的实际 CLI 重放；拒绝场景先保存 Index、HEAD、ref、日志和文件快照，再 cmp 验证原状。[test_reset.cpp](../tests/unit/test_reset.cpp) 已写入八组语义测试，[reset_cli.cmake](../tests/integration/reset_cli.cmake) 重放真实参数与磁盘状态；两种预设的相关测试与实验均通过，具体命令和实际范围见进度页。

## Remove：共享纯检查，不共享业务安全策略

[remove.hpp](../include/minigit/remove.hpp) 的 RemoveOptions 使用三个 bool 保存 cached、force、recursive，remove_paths 在 [remove.cpp](../src/core/remove.cpp) 自取一次锁。核心与实验已通过子项验收；CLI 参数重放见 [remove_cli.cmake](../tests/integration/remove_cli.cmake)。此次 core-private [checkout_support.hpp](../src/core/checkout_support.hpp)／[checkout_support.cpp](../src/core/checkout_support.cpp) 提取 Restore／Reset 已重复的词法选择、有界集合、Index 预算与 typed Blob 校验；A/B/C 删除策略仍留 Remove。

实际顺序为 normalize_selectors → load_index／check_index_objects → HEAD 完整快照 → select_known_paths／has_descendants 的 recursive 判定 → 最终 Index 预算与 encode → 安全矩阵 → plan_index_update → 非 cached 的 plan_worktree_update／check_observations → execute_transaction。共享 helper 没有命令模式开关：同一候选算法接收 Restore 的 source∪Index 或 Remove 的 Index，业务分别决定如何授权。

阅读时区分两个 short-circuit：cached 先 B==A，可以不读 C；否则必须证明 B==C。普通模式还要求真正 Absent 或 C==B。私有 observe_work 逐祖先用 symlink_status 检查，并由同 fd 的 read_worktree_file 取得 File 的 ID 与完整权限；不能把 read_worktree_entry 的 nullopt（也可能是 Directory）直接当缺失。force 的 selected conflict 清理与正常路径 stage 0 比较分开，避免缺失阶段产生 null==null 绕过。

最终 Index 保留 unselected 全阶段，只移除 selected；非 cached 为所选 Index 路径生成 null WorktreeTargets，并用同一组精确 owned 调共享 planner。私有 check_observations 将安全检查取得的 NodeImage 与 planner before 比较，后来的内容变化不能偷偷成为合法 before。cached 不创建假 WorkChange，HEAD 和日志不调用移动规划。

[remove.sh](../examples/experiments/remove.sh) 已准备 clean／真实缺失、staged-only／dirty拒绝、cached 两条件／force、目录 -r、未知 child、选项样名称及 HEAD／日志不变。每个失败先保存元数据 baseline，实际文件也在操作前复制再 cmp；两种预设均已运行通过。

[test_remove.cpp](../tests/unit/test_remove.cpp) 的七组语义用例覆盖普通批次原子保护、cached 短路与 force、词法范围与 -r、所选／未选冲突、真实缺失与未知目录保护、unborn／detached／坏输入及 Prepared／Committed 两个恢复边界。本轮还复验共享提取影响的 Restore、Reset 和关联 CLI；实际筛选与注册规模分别记录在进度页。

## Move：复制状态模型，再统一发布

Mv 已通过子项验收，[move.hpp](../include/minigit/move.hpp) 的 move_path 在 [move.cpp](../src/core/move.cpp) 自取一次锁；CLI 参数重放见 [move_cli.cmake](../tests/integration/move_cli.cmake)，单元覆盖见 [test_move.cpp](../tests/unit/test_move.cpp)，两种预设定向检查均通过。共享 checkout_support 只负责词法规范化、有界集合、选择及 typed Index，不承担目的覆盖或源目录授权。

C++ 阅读重点是把两份状态分别保存：IndexEntry 的 mode／ID 可值复制并改 path；实际 C 的 NodeImage／FileData 包含完整权限，不能从 B 推导 C。每个工作文件读取后释放 payload，只保留有限路径／ID／权限，不持有整目录字节。目的已有 Directory 只影响 final root 解释一次，不能反复追加 basename。

实际顺序为 normalize_selectors → load_index／check_index_objects → select_known_paths／所选冲突 → inspect_path 源目的拓扑与 final root 碰撞 → 路径映射／最终 Index encode → plan_index_update／元数据计划 validate_transaction_budget → C 读取与 capture_work_node → plan_worktree_update 的目录库存／完整预算 → planned_change 核对 → execute_transaction。inspect_path 只读无跟随拓扑，不读 payload；完整目录库存留给共享 planner。source before 须保持此前观察的 C，destination 须真正 Absent；即使 planner 因同字节未知目的而省略 change，业务也不能默认许可覆盖。晚规划拒绝可留下不可变对象，不发布 Work／Index；执行中断可能留下待恢复事务与部分更新，需 recover。私有 below 检查带分隔符的祖先边界，planned_change 要求 source／destination 真有对应改动，避免“规划器省略”掩盖外部变化。

[move.sh](../examples/experiments/move.sh) 已准备 MM 中 B／C 内容与执行位分离、完整权限、同字节与 Index-only 碰撞、已有目的目录 basename、多叶目录、未知／忽略 child／emptyDir、选项样名称及 HEAD／日志不变；两种构建的实际实验均通过。

Move 的七组单元用例分别检查 B／C 字节与模式／完整权限、目的碰撞和 basename、未知库存、自身重叠与 Index 拓扑／冲突、全对象与不安全节点、unborn／detached／词法路径，以及 dirty 源和目录映射的 Prepared／Committed 恢复。该恢复用例验证执行中断后的真实现场，不能由“规划失败无发布”替代恢复契约；实际命令与范围见进度页。

## 纯文本合并：hunk 是展示范围，edit 才是修改范围

[merge_text.hpp](../include/minigit/merge_text.hpp) 定义 TextConflict／TextMergeResult，落盘 [merge_text.cpp](../src/core/merge_text.cpp) 实施纯 merge_text；单元文件为 [test_merge_text.cpp](../tests/unit/test_merge_text.cpp)，两种预设的相关检查及独立性质验证已通过。merge_text 只接受原始三份字节，不接受外部伪造 EditScript；内部复用 split_lines／diff_lines 的合法脚本，避免新增一份 LCS。

阅读重点是同一份 base 的 old 坐标：点插入不消费基线，正区间消费半开范围。同坐标先点后区间，内部交互形成传递闭包，分别重放两侧包含未修改基线的完整区块，再比较 bytes 和 terminated。make_hunks 会人为加入显示上下文，两个 hunk 接近不表示两份修改互相冲突，不能把显示结果用作合并判定。

span 与 Line 只借用输入，TextMergeResult 的 Bytes 拥有输出；base 行坐标和 output 字节偏移须分别计算。输入与输出增量预算、EOF 无 LF 特例和 coarse 都属于语义，不能在格式化最后才补救。实际链为 validate_input → split_lines → 恒等快捷返回 → 两次 diff_lines → order_edits → 严格区块／EOF 合组 → replay → Output::clean／conflict。Output::append 在每次写入前检查剩余字节预算，冲突偏移从真实 content.size() 计算。七组单元覆盖恒等输入校验、独立与边界修改、同点插入及 marker 偏移、完整重放与传递重叠、EOF／LF、预算与 coarse。fs.hpp 仅复用 max_file_size 常量，无文件 IO。实际验证范围见进度页，当前不是 merge／continue／abort 的命令导读。

## SnapshotMerge：先验证所有输入，再保存新对象

[merge_snapshot.hpp](../include/minigit/merge_snapshot.hpp) 定义 MergeConflictKind、含 mode_conflict 的 MergeConflict 及 SnapshotMergeResult；[merge_snapshot.cpp](../src/core/merge_snapshot.cpp) 的 merge_snapshots 已通过子项验收；[test_merge_snapshot.cpp](../tests/unit/test_merge_snapshot.cpp) 包含八组语义测试。merge_snapshots 接收外部 Snapshot，因此不能假设输入都来自 read_snapshot；路径、mode、该侧无 D/F、100000 文件与结构预算先检查；snapshot_index 在复制路径／追加 entry 之前逐项检查剩余编码预算，所有 typed Blob 再验证，之后才可生成新对象。

阅读时追踪两个输出：IndexEntry 保存原始 B/O/T 的 ID 与 mode，work 保存干净选择、合并或 marker 的 FileState。全状态 optional 快捷判断包含存在和 mode，随后内容与模式分别作三方判断；创建冲突须确认“base 不存在、两侧内容 ID 不同”，不能把缺失当空字节交给恒等快捷路径。NUL 优先分类 Binary 且保留 ours 字节；非 NUL 由 add_add_marker 生成 whole-file marker，分类 AddAdd，TextConflict 的 base_count=0。mode_conflict 与主 kind 并行，多个冲突维度不因分类互相覆盖。

D/F 用可能的最终存在路径找最短区域，而不是机械合并三个输入的名字；祖先检查须覆盖全部路径分量，p-other 不能让 p 与 p/x 的碰撞漏掉。整个区域的原 stage 0 候选也要升级为原始 stages，工作布局只保留 ours。最终 sorted Index 要经预算与 encode_index，work 要复验无 D/F；实际链为 snapshot_index 的各输入 schema → 有界 union 与去重 typed IDs → choose 计算 possible → region_at 最短 D/F 区域 → scalar 的内容／mode 或 read_blob／add_add_marker／merge_text → IndexOutput → 最终 Index 与 work schema。choose 比较完整 optional<FileState>，scalar 才分别比较单项；D/F 在文本化前判定，避免生成本会丢弃的 marker。八组测试覆盖完整状态选择、内容／mode 独立组合、原始 stages 与 marker、含空文件的 AddAdd、删除与二进制、最短 D/F 区域、全部输入前检及 union／最终 distinct 预算。AddAdd 单独验证 100001 行拒绝与接近 64 MiB 的 marker 膨胀，不借空 base 的快捷路径绕过限制。核心已验收，没有 CLI／事务发布调用；下节 single-base 选择复用完整 DAG 校验。

## Merge base：两轮传播都走全部 parents

find_merge_base 已通过子项验收，不是新命令。先复习 [dag.hpp](../include/minigit/dag.hpp) 与 [dag.cpp](../src/core/dag.cpp)：load_commit_graph 有界加载完整 Commit 闭包，topological_order 验证全部 parent membership 与环，提供 child-before-parent 顺序，available-node ties 用 ObjectId 字节顺序。这个 tie 规则只保证遍历稳定，不可代替多个基线的业务定义。

实际链为 load_commit_graph → topological_order → Reach 的 mask／common_descendant 初始化 → 两 root 的两位标记 → 第一轮沿全部 parents OR → 第二轮向 parents 传播共同祖先／共同后代 → 收集 common 且无共同后代的节点。不能在 same ID／ancestor 检查前漏掉完整图校验；重复父边不应改变集合含义。[test_merge_base.cpp](../tests/unit/test_merge_base.cpp) 已有八组语义，涵盖 same tip、祖先双方向、私有链、非 first-parent／父顺序、diamond、crisscross、无关同 Tree／重复父边及损坏深祖先。无 Tree 展开也有实际 fixture：删除 Tree 对象后，父图查询仍应得到相同结果；它不承诺该 Commit 可被发布。未重复已有 incomplete／cycle／大预算测试，八组与相关 DAG／日志回归均通过，实际范围见进度页。返回 optional 区分“没有共同祖先”，多最低基线则 Error；后续业务另处理这个缺失值。

## CommitSnapshot：caller-lock 存储供业务复用

工厂接口与核心已通过子项验收。阅读 [history.hpp](../include/minigit/history.hpp) 与 [history.cpp](../src/core/history.cpp) 时先区分 self-lock create_commit 和 store_commit_snapshot：前者选择当前 Index、HEAD parent 与空提交策略并发布，后者只接收明确输入、构造 actors／frame 和保存对象。

校验顺序是可观察行为：create_commit 消息仍在锁前拒绝，显式 identity 缺失仍先于 HEAD／Index 损坏报错。公开 store_commit_snapshot 对全部显式父 Commit 的本身快照、候选模式／Blob、raw author 与完整 framing 提前校验，之后才写 Tree；parents 可重复但不重排，不以 first parent 省掉其他父的快照校验。不加载父 DAG，不在共享入口再次取锁。已保存对象不可变，后期错误不回滚对象，也不改变任何可变状态。

实际公开签名借用 const Snapshot&、const vector<ObjectId>&、message string_view 与 optional<string_view> original_author；author 只借用本次调用，prepare_commit 拷贝为 Commit 的拥有字符串。调用链为 validate_message → require_no_transaction → validate_parent_count → required_identity／prepare_commit（validate_commit_frame＋placeholder Tree ID 的 encode_commit）→ 去重逐 parent 的 read_commit／read_snapshot → write_snapshot → store_final_commit。已知 frame 超预算在复制 parents／message 前拒绝。

ordinary create_commit 的链保持 validate_message → 取锁／pending → required_identity → HEAD／parent 自快照 → Index／root-empty → write_snapshot／Tree ID empty → prepare_commit／store_final_commit → plan_move_head／execute_transaction。共享私有构造器不代表它改为经 public factory：普通提交的 actor／frame 仍晚于候选 Tree，可能留下不可达 Tree，绝无提前可变发布。

[test_history.cpp](../tests/unit/test_history.cpp) 新增五组：明确输入不读取损坏当前 HEAD／Index 与有序重复 parents（含不遍历父祖先）、原 author 字节／当前 committer、全部父和候选对象／schema 写 Tree 前校验、100001 parent／完整 frame／pending 拒绝、ordinary 空子 Tree 与消息→身份→HEAD→Index 错误顺序。显式 empty Snapshot 及 same-tree 存储允许。两种预设构建与定向回归通过，精确范围见进度页；无 CLI 或新实验，持久 Operation codec／roots 与业务 guard 的验收见下节；下一依赖为 Merge start／continue／abort 生命周期。

## OperationState：pure codec、semantic roots 与业务 guard

本项基础模块已通过子项验收。[operation.hpp](../include/minigit/operation.hpp) 定义 OperationState 与纯 codec／caller-lock 接口；[operation.cpp](../src/core/operation.cpp) 实现 encode_operation／decode_operation、caller-lock read_operation／require_no_operation／state_roots，以及 plan_operation_update(repo, plan, expected, desired)。codec 不读 Repository；read／roots 做 typed 对象语义校验但不探测 Work；guard 对任意现存 operation 节点拒绝，无需为了阻止业务先扫描备份对象。

plan 首先 require_no_transaction、拒绝已有 operation metadata change，并 validate_transaction_budget 原 plan，再读取真实可选 raw before；存在时 decode／roots 并和 expected canonical 编码比较。expected=null 只允许缺失，desired=null 表示删除；同 State 为 noop，但不跳过期待值或计划前置校验。v1 不支持 existing→不同 State。desired 的 codec／roots 与 complete combined 事务预算通过后才 move 赋回，任何失败磁盘不变、caller plan 不变。

检验两个不同的根层：journal 保存 operation metadata payload 的 backup Blob；State 自己另保存 original raw Index backup Blob。后者必须继续解码所有 stages 与 typed Blob，不是只验它为 Blob。original／current 是保存的历史结构，不可和现盘 HEAD 在 codec／roots 中比较；当前 HEAD CAS 留给后续业务。事务 execute／recover／roots 共用 operation before／after 的语义分支，避免只有正常执行支持而恢复不保根。

guard 只在 self-lock commit／switch／reset／branch mutations 加入，内部 store_commit_snapshot 与各 planner 不加；允许解决工具与身份配置仍遵守原规则。该基础验收轮仅 Merge／Conflicted；现已增加 CherryPick／Revert kind，read／plan 本身不会捕获新的备份 Blob，当前没有 Ready／todo／position 的万能字段包。实际 encode 链为 validate_state 的增量大小／Head／NodeImage／双侧拓扑预验 → reserve 有界空间 → BE 字段写入 → SHA-256；decode 先总大小与 checksum，再 Reader 消费字段、在 count reserve 前验证上限与最小剩余字节，最后复验 validate_state 并拒绝 trailing bytes。roots 用 ObjectId→ObjectType map 去重 typed 校验，Index header 的 300000 stages 前检早于 decode_index 的 reserve，再按完整 Index budget 与所有 stage Blob 取根。

read_operation 为 raw_operation → decode_operation → state_roots，刻意不自加短事务 guard、也不 inspect live HEAD／Work，以便 recovery 复用。plan 为原 plan／before 语义 → expected 编码 CAS → desired 编码／roots → same noop 或 v1 replacement 拒绝 → next combined budget → move 赋回。原 metadata 精确字节而不是重编码值进入短事务 CAS。

[test_operation.cpp](../tests/unit/test_operation.cpp) 已落盘五组：canonical roundtrip、authenticated malformed／资源与双侧拓扑、历史 typed roots／Index 全 stages、expected CAS／strong plan／combined budget、operation＋Index＋Work 的 Prepared 创建／Committed 删除恢复与 before／after 根。两组 [test_operation_guards.cpp](../tests/unit/test_operation_guards.cpp) 检查有效／坏／unsafe State 节点阻止历史移动、允许查询／身份／解决工具／tags／内部 primitives 并保留 operation 字节。冻结源码审查、两种预设最终构建及 31 项定向回归通过，精确范围见进度页；该基础验收轮没有 merge／continue／abort CLI，接入见下节。

## Merge：先分清业务分支，再找事务边界

[merge.hpp](../include/minigit/merge.hpp) 已冻结：MergeOptions 指定 Start／Continue／Abort，revision 仅启动且最多 4096 字节，optional message 也仅启动；MergeResult 返回 outcome、Head 与冲突列表。merge_repository 自取一次锁，hook 只用于持久边界测试。已落盘 [merge.cpp](../src/core/merge.cpp)，相关回归、实验及完整只读审查通过。merge_repository 为 validate_options→一次 RepoLock／pending→三个动作分发。start_merge 为 operation guard→raw_metadata／checked_index／resolved_snapshot→source 完整快照／find_merge_base（unborn 用完整 graph＋topo）→clean_work→快捷分支或 SnapshotMerge→final Index schema→changed_targets／Index＋Work planners→check_work_before。clean 存两父 Commit 后 plan_move_head；conflict 的 conflict_work 扩充完整受影响区域，再备份 whole raw Index／raw HEAD 与 plan_operation_update。check_original_metadata 后才执行一次事务，不另从历史生成工作备份。

continue_merge 为共享 active_operation(Merge) 的 State／current Head CAS→checked_index／resolved_snapshot→store_commit_snapshot 两父完整自快照→MoveHead＋删除 State→raw Index 复检→一次事务，不读取 Work。abort_merge 现调用共享 plan_abort_operation，从精确备份生成 File／Directory／Absent 目标，保留 full permissions／emptyDir／dirty bytes；require_untouched_parents 保护区域外 parent，再补当前 Index 下必要删除目标，恢复整份可选 Index 与 raw HEAD，并复用 Work planner／State CAS／metadata 复检／一次事务。unknown 后代仍由共享 planner 拒绝。受影响区域还包括非零 stages／D/F root 的同值节点：Binary／Mode／D/F 保持 ours 时也必须备份，D/F root→leaf 的真实目录权限同样记录；外部未受影响 parent 不因此获得授权。原 File／Absent 在解决中变为目录，abort 仅为该恢复 root 下当前 Index 新跟踪子项补删除目标，unknown 子项仍拒绝。0700 后代约束不放宽，无法生成可恢复镜像的启动应拒绝。[test_merge.cpp](../tests/unit/test_merge.cpp) 已落盘八组：unborn／FF／detached／up-to-date，clean 两父，Index 继续，raw metadata／权限中止，同值 Mode／Binary／D/F 区域，dirty／unsafe／历史损坏，State CAS／未知中止子项，以及 Prepared 冲突／Committed 继续恢复。八组单元与 [merge_cli.cmake](../tests/integration/merge_cli.cmake) 的对应检查在两种预设均通过，精确初轮／单项复验范围见进度页；完整只读审查通过，无另一份差异算法或恢复执行器。CLI 支持 --；已发布冲突返回 1，展示预算不足省略剩余路径，完整 Index／State 不裁剪。


## Replay：公开两入口，kind 由入口决定

已落盘 [replay.hpp](../include/minigit/replay.hpp)：cherry_pick_repository／revert_repository 各接收 ReplayOptions，字段为 action（Start／Continue／Abort）、启动专用 revision 与 optional message；ReplayResult 为 Applied／Conflicted／Aborted、Head 与冲突列表。两入口 self-lock，hook 只作持久步骤测试，不让调用者另传任意 operation kind。

[replay.cpp](../src/core/replay.cpp) 已冻结：两个公开函数固定 kind 后进入 replay_repository，validate_options→一次锁／pending→Start／Continue／Abort 分发。start_replay 为 operation guard／non-unborn→raw metadata／checked_index／HEAD full Snapshot→source≤1 parent→双方完整 graph＋topo→source 与唯一 parent full Snapshot→默认消息校验→clean_work→正向或反向 SnapshotMerge→final Index budget／encode→Index／Work planners。clean 总调用 factory 单父 [current]，CP original_author 返回 source.author，Revert 返回 nullopt；conflict 保存 kind、真实 conflict_work、whole optional Index／raw HEAD 后创建 State。最后 metadata 复检再 execute 一次，没有空变化 shortcut。

continue_replay 为 active_operation 的 kind／HEAD CAS→typed resolved Index→source Commit 重验≤1 parent→单父 factory→MoveHead＋State 删除→raw Index 复检→一次事务，不重新遍历 source DAG／Tree，也不探测 Work。factory 验证当前父的完整自快照，不能把 continue 与 start 的历史校验范围混写。abort 由 shared plan_abort_operation 构造 fresh plan，调用方统一执行；logical Head noop 不造日志，必要 raw HEAD patch 保留原精确字节。

[operation_support.hpp](../src/core/operation_support.hpp) 与 [operation_support.cpp](../src/core/operation_support.cpp) 仅供 core：借用 caller-held RepoLock，不自加锁或执行事务，公共 capture、same-value noops、D/F Directory 祖先与 require_untouched_parents 的实际恢复约束由 Merge／Replay 两个 caller 共用。业务作者、父数、基线方向仍留在各业务，不新增支持任意 kind 的公开包装器。

[test_replay.cpp](../tests/unit/test_replay.cpp) 八组覆盖跨历史／root CP 与原作者、反向／root Revert、preserve-empty、kind-aware Index 继续（含 Work FIFO 不读取）、raw HEAD／whole optional Index／权限／D/F 中止、unsafe／多父／深历史拒绝、CAS／未知中止子项，以及 Prepared／Committed durable 恢复。旧 Operation tests 增加两种 kind 只改现有 byte 与 checksum、拒绝闭集外值；[replay_cli.cmake](../tests/integration/replay_cli.cmake) 用一个注册入口覆盖两命令及 usage／wrong kind。完整只读审查、两个预设的相关 31 项及两个实验回归通过，精确范围见进度页。

CLI 的两命令消息覆盖统一为启动专用 --message text，拒绝 -m／--mainline；共用 parse_history_arguments 的 message_option 选择与 有界 report_conflicts 展示，不改变 Merge 的 -m。CLI 与实验已冻结，两个预设运行验收通过。


## StashStack：条目内容和列表 CAS 分开

已落盘 [stash_stack.hpp](../include/minigit/stash_stack.hpp)：StashStack 是 newest-first vector<ObjectId>，StashStackState 同时保存 entries 和 optional raw_stack／raw_ref；StashEntry 给出 work／head／index／optional untracked。encode／decode 为纯 MGST v1，BE version/count、固定 32-byte ID、SHA256，最多 100000 entries／64 MiB，编码恰为 44+32×count，合法 encoded-empty 与 metadata 缺失都表示空列表。

caller-lock read_stash_entry 只验证 W parents=[H,I,(U)]、I parents=[H]、U root 和 typed Commit schema；stash_roots 校验每个条目，缓存读取并限制合计 decoded payload 64 MiB，不展开 Tree／父 DAG，不读当前 Index／Work。read_stash_stack 再检查 top 与 refs/stash 一致，并保留 exact raw bytes。plan_stash_stack_update 在 expected entries 与两份 raw CAS、desired 全部 roots 及 combined 预算成功后赋回计划，不写对象或执行事务；desired-empty 删除两份 metadata，相同逻辑 no-op 保留原 bytes。

[stash.hpp](../include/minigit/stash.hpp) 与 [stash.cpp](../src/core/stash.cpp) 已冻结：StashPushOptions 为 include_untracked／optional message，stash_push 返回 optional Work ID，nullopt 为 NoChanges；list 返回 position／work／message，show 接受 position 与默认 context=3，drop／clear 支持测试 hook。业务一次锁／pending，mutators 再 operation guard；query 不加 operation guard。

push 实际链为 stack／HEAD／raw Index→typed resolved Index／HEAD Snapshot→snapshot_index(H) 与 validate_saved_tree(I)→H∪I exact tracked capture_saved_file→可选 scan_worktree 减 tracked 的 U→combined_work→NoChanges 的 metadata／stack CAS，或 cleanup targets／Index＋Work planner→before 观察比较→store_commit_snapshot 保存 I／可选 U／W→stack CAS→metadata 复检→一次 Txn。三份 factory 共用真实保存消息（默认 WIP 加完整 HEAD ID），不硬编码独立 Index 标签。capture 逐文件保存 bytes 并观察 full permissions；相同 FileState 的 cleanup 不改完整权限。

validate_saved_tree 在写 Tree 前检查 File＋Directory 共 100000 条目与展开数据 64 MiB，combined_work 拒绝 W／U 重复或 D/F。scan_worktree 的 walk 对 visited 节点（含 Directory／ignored 节点）在 stat 前检查 200000、累计路径 64 MiB、4096 字节／128 components；Add 仍用 budget=null，不能把新 scanner 预算说成所有 add 路径的行为。

[stash_stack.cpp](../src/core/stash_stack.cpp) 的 Entries 缓存 typed Commit、entry 校验父角色；read stack raw pair→decode／roots→top 一致，plan 再 exact entries/raw CAS→desired roots→next combined budget。Txn 对 stashstack＋refs/stash 的两侧镜像配对从 journal bytes 校验，不依赖正在半发布的 live pair。list 仅 Commit message；show 完整 H/I/W/U 自快照校验、combined W+U 后 diff_snapshots；drop／clear 的 Txn 对旧 refs/stash 仍验证 top Work Tree，不能声称这些更新完全不展开 Tree。

[test_stash.cpp](../tests/unit/test_stash.cpp) 八组涵盖分层保存、增删／missing／binary／D/F、NoChanges、ignore-aware -u、query／middle drop／clear、unknown cleanup、非法保存及完整树预算、Prepared push／Committed drop。[test_stash_stack.cpp](../tests/unit/test_stash_stack.cpp) 八组涵盖 codec、有界认证帧、全部 roots、原 bytes／paired reader、strong CAS/noop、空栈／pending、Txn 两侧 schema、Prepared／Committed 恢复。实际 [stash_cli.cmake](../tests/integration/stash_cli.cmake) 已落盘，交叉完整审查及两个预设的相关 38 项／保存实验通过，范围见进度页；无 apply／pop／--index／continue／abort。
