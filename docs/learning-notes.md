# 学习笔记与观察实验

MiniGit 的目标是让数据模型和行为之间的关系可以亲手观察。下面先提供可在真实 Git 临时仓库进行的实验；MiniGit 的对应命令属于阶段规划，未验收前不要直接替换命令前缀。

## 实验一：内容与名称是两件事

在独立临时目录创建 Git 仓库并比较同内容文件的 blob：

```sh
git init
printf 'hello\n' > a.txt
printf 'hello\n' > b.txt
git hash-object a.txt
git hash-object b.txt
git add a.txt b.txt
git ls-files --stage
```

两个文件的 blob 摘要相同，Index 中的路径不同。随后修改一个文件，只有其内容对应的摘要改变。对象和文件名的分工见 [Git Objects](https://git-scm.com/book/en/v2/Git-Internals-Git-Objects)。MiniGit 验收时应复现这一复用性质，并分别验证 tree 中的文件名。

## 实验二：暂存不是承诺未来内容

继续在实验仓库中操作：

```sh
printf 'staged\n' > a.txt
git add a.txt
printf 'working\n' > a.txt
git diff --cached
git diff
git show :a.txt
```

预期 `git show :a.txt` 显示 `staged`，工作区仍为 `working`。这展示了为什么一次 status 可以同时报告同一个路径已暂存和未暂存。MiniGit Phase 1 对应验收用例应直接读出 commit 中的 blob，证明提交使用暂存版本。

## 实验三：从 blob 拼出快照

真实 Git 可通过底层命令观察手工构造 tree 与 commit 的过程，具体步骤见 [Git Objects](https://git-scm.com/book/en/v2/Git-Internals-Git-Objects)。阅读 MiniGit 的 tree 编码时，先写出两个普通文件和一个子目录的条目，再逐字段检查模式、名称、NUL 与二进制摘要。

练习中改变输入枚举顺序，应保持输出 tree 字节相同；改变一个 basename，应改变所属 tree；改变子目录内容，变化应沿父 tree 传播到根。这个传播过程说明了快照复用的边界。

## 实验四：从失败理解更新顺序

对象可以在引用更新前存在。模拟写入 commit 后、更新分支前失败：旧 HEAD 应仍可读取，新对象可能不可达。再模拟 Index 格式截断：读取应明确拒绝，不能把损坏误判为空暂存区。

Phase 2 应增加锁冲突、临时文件残留、工作区覆盖风险和失败恢复实验。Phase 3 的冲突实验要重启进程，再验证继续与中止，确认状态保存在磁盘而非进程内存中。可恢复操作的状态设计见 [架构](architecture.md)。

## 取舍与待回答的问题

| 选择 | 学习收益 | 成本或后续问题 |
| --- | --- | --- |
| 固定 SHA-256 与 EVP | 明确摘要输入和字节表示，使用现有密码库 | 不能把哈希替换等同于 Git 格式兼容 |
| zlib 松散对象 | 每个对象可单独检查，易理解持久化 | 大量对象的空间与性能需以后改善 |
| 自有 Index | 先掌握暂存快照与冲突状态 | 必须自行设计版本、校验和迁移策略 |
| 首版拒绝软链接 | 收窄文件类型并集中验证路径安全 | 不能覆盖真实 Git 的全部文件语义 |
| 分阶段交付命令 | 每个阶段形成可验证的数据流 | 文档必须持续区分实现与计划 |

接下来的学习问题包括：路径排序究竟按什么字节规则？commit 的身份和业务时间怎样规范化？引用与 Index 更新失败怎样恢复？合并怎样区分文本冲突与二进制冲突？gc 哪些根对象和保留规则能防止误回收？答案应来自已实现的格式、测试和观测，再写入文档。

## 常见误区

- blob 包含内容，路径来自 tree；没有文件名并不妨碍文件版本复用。
- commit 保存快照入口和父关系，不能仅把它理解为补丁集合。
- 相同文件快照不保证相同 commit，元数据与父关系也参与摘要。
- 删除分支名称不立即删除提交；对象回收需要检查可达性。
- 原子替换一个文件不等于整个命令的所有磁盘修改具备事务性。
- 自有格式可以借鉴 Git 的原则，但兼容必须有明确格式规范和互操作验证。

以上实验仅用于临时学习仓库。阶段验收的最小闭环见 [进度](progress.md)。
