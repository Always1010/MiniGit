# 代码导读

当前实现包括 Phase 0 构建基础、Phase 1 的初始化、对象库、Index、引用、快照及对应命令，以及 Phase 2 的纯差异引擎。差异 CLI、分支与工作区操作仍为计划。完成和验证状态见 [进度](progress.md)。

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

Debug 和 sanitized 的 `cli_basics` 均已通过，验证帮助和别名、版本格式、空参数、未知参数和多余参数，并检查输出失败的退出码。具体证据由 [进度](progress.md) 记录。

## 当前对象库阅读路线

1. [bytes.hpp](../include/minigit/bytes.hpp)：`Bytes` 是字节向量，文本转换辅助函数不改变 payload 的二进制含义。
2. [object_id.hpp](../include/minigit/object_id.hpp) 与 [object_id.cpp](../src/core/object_id.cpp)：`ObjectId::from_hex/from_bytes/hex/bytes` 分离显示与原始摘要，`sha256` 封装 EVP。
3. [objects.hpp](../include/minigit/objects.hpp) 与 [objects.cpp](../src/core/objects.cpp)：`hash_object` 构造类型、字节长度和 NUL 头；`encode_tree/decode_tree`、`encode_commit/decode_commit` 处理 payload。
4. [fs.hpp](../include/minigit/fs.hpp) 与 [fs.cpp](../src/core/fs.cpp)：`read_file` 拒绝软链接和特殊文件，`atomic_write` 处理临时文件与同步，`RepoLock` 管理持久锁文件的互斥生命周期。
5. [repository.hpp](../include/minigit/repository.hpp)：`Repository::init/discover` 提供 `.minigit` 根和元数据路径，不承担全部命令业务。
6. [object_store.hpp](../include/minigit/object_store.hpp) 与 [object_store.cpp](../src/core/object_store.cpp)：`write` 存储 zlib 对象，`read` 校验封装，`object_path` 将完整摘要映射到两级路径。

沿 CLI 的 `hash-object -w` 阅读可看到：先读文件字节，发现仓库，取得 `RepoLock`，调用 `ObjectStore::write`，打印完整摘要。ObjectStore 不自行取得写锁，调用者必须保证锁生命周期覆盖写入。`cat-file -p` 对 blob 使用原始字节输出，对 tree 解码后逐条显示。

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
| 9 | Diff、Worktree、Operations（计划） | 恢复、覆盖保护和继续/中止怎样跨进程保持？ |

表中前八项已有接口，阅读链接见相关章节；后续分支及工作区操作仍为计划。依赖关系见 [架构](architecture.md)。

## 从 Index 到提交

读取 [refs.hpp](../include/minigit/refs.hpp)、[refs.cpp](../src/core/refs.cpp)，先看 `read_head` 怎样表示未出生分支，再看 `update_ref` 如何检查预期旧值。读取 [snapshot.hpp](../include/minigit/snapshot.hpp)、[snapshot.cpp](../src/core/snapshot.cpp) 的 `write_snapshot/read_snapshot`，观察路径映射如何递归构造 Tree，以及每个 Tree 条目的模式如何约束所引用的对象类型。

然后阅读 [history.hpp](../include/minigit/history.hpp)、[history.cpp](../src/core/history.cpp) 的提交协调与身份。`create_commit` 的调用链是：获取写锁 → 读取配置身份和 Index → 检查冲突条目 → 构造根 Tree → 校验旧 HEAD → 保存带 parent 的 Commit → 更新引用。失败可能留下未引用对象，但不会发布尚未完整保存的提交。底层编码保留读取教学对象的自由，业务层要求身份和消息有效。

[test_history.cpp](../tests/unit/test_history.cpp) 检查快照、parent 与失败边界；[commits_cli.cmake](../tests/integration/commits_cli.cmake) 在工作区编辑后直接读取已提交 Tree 和 Blob，证明提交来源是 Index；[提交实验](../examples/experiments/commits.sh) 可重复演示同一机制。

## 先阅读纯差异算法

新增差异引擎从 [diff.hpp](../include/minigit/diff.hpp) 进入 [diff.cpp](../src/core/diff.cpp)。`split_lines` 把字节拆成借用视图和末尾 LF 标记，`diff_lines` 去掉共同前后缀、生成精确行 ID，并从 LCS 表回溯成 EditScript。Edit 的坐标同时指向完整旧行和新行序列，不随先前删除或插入改变，便于后续 Merge 统一在 base 坐标下比较。

`make_hunks` 校验区间、顺序和未改动间隔，再把相邻上下文合并；`format_file_diff` 负责缺失文件、模式、摘要、二进制和无末尾换行等显示策略。它不访问仓库，也不把工作区未保存的 ID 当成已存在对象。CLI 接入将独立协调内容来源，进度见 [progress.md](progress.md)。

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
