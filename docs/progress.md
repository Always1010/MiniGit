# 开发进度

## 当前状态

当前版本 `0.1.0`，Phase 0—4 的约定范围已完成并通过整体验收。验证平台为 Linux/POSIX，其他平台尚未验证。命令边界见[命令参考](commands.md)，格式与故障边界见[架构](architecture.md)。

## 阶段交付

| 阶段 | 已交付范围 | 状态 |
| --- | --- | --- |
| Phase 0：构建基础 | C++20 核心库／CLI、依赖、帮助／版本、CTest | 已验收 |
| Phase 1：本地快照 | 对象库、Index、init／add／commit／status／log、轻量 tag | 已验收 |
| Phase 2：查询与安全更新 | diff／show、branch／switch／restore／reset／rm／mv、reflog、短事务／recover | 已验收 |
| Phase 3：合并与恢复 | 三方引擎、merge／cherry-pick／revert、Stash 保存与恢复、continue／abort | 已验收 |
| Phase 4：重放与回收 | 线性 Rebase 队列及恢复、完整语义根与松散对象 GC | 已验收 |

## 验收结果

以下是已有代码验收结果，不代表每次文档整理都重新执行了这些检查。

| 范围 | Debug | sanitized |
| --- | --- | --- |
| Phase 1 整体 | 54 项通过 | 54 项通过 |
| Phase 2 整体 | 147 项通过 | 147 项通过 |
| Phase 3 整体 | 237 项通过（216 单元、21 CLI） | 同范围全部通过 |
| Phase 4／最终整体 | 260 项通过（237 单元、23 CLI） | 同范围全部通过 |
| Rebase 定向检查 | 38 项通过 | 38 项通过 |
| GC 定向检查 | 24 项通过 | 24 项通过 |

最终完整回归命令：

```sh
ctest --preset debug --output-on-failure -j2
ctest --preset sanitized --output-on-failure -j2
```

两个完整回归进程退出码均为 0，无 ASan／UBSan 诊断。对应构建成功且无编译警告。

命令实验随对应功能验收执行；Rebase 与 GC 实验均在两个预设实际通过。最终整体验收沿用已有实验结果，没有重跑所有历史脚本。实验入口见[学习实验](learning-notes.md)，选择检查范围见[构建与测试](testing.md)。

## 后续范围

当前约定目标已完成。Remote、pack、重命名检测、多个 merge-base 合成、交互式 Rebase 和 reflog 过期策略均未实现，属于后续独立扩展。
