# MiniGit

用 C++20 编写的本地版本管理器，用于学习内容寻址、快照、分支、合并与恢复。核心独立实现，不调用系统 Git。

支持暂存与提交、差异与历史查询、分支与标签、合并与重放、Stash、线性 Rebase 和对象回收。采用自有 `.minigit` 格式，不与 Git 仓库互读。

## 快速开始

需要 CMake 3.20+、C++20 编译器、OpenSSL 3、zlib 和 Catch2 v3，当前支持环境为 Linux/POSIX。

```sh
cmake --preset debug
cmake --build --preset debug
./build/debug/minigit --help
```

从项目根目录运行以下示例，创建独立的练习仓库：

```sh
MINIGIT_BIN="$PWD/build/debug/minigit"
"$MINIGIT_BIN" init demo
cd demo
"$MINIGIT_BIN" config user.name Learner
"$MINIGIT_BIN" config user.email learner@example.test
printf 'hello\n' > note.txt
"$MINIGIT_BIN" add note.txt
"$MINIGIT_BIN" commit -m '记录首个快照'
"$MINIGIT_BIN" status
"$MINIGIT_BIN" log --oneline
```

## 文档

- [文档目录](docs/README.md)：按用途查找资料。
- [命令参考](docs/commands.md)：参数、常用操作与恢复方式。
- [原理图解](docs/git-internals.md)：三棵树、对象与提交历史。
- [学习实验](docs/learning-notes.md)：运行脚本，观察状态变化。
- [架构](docs/architecture.md)与[代码导读](docs/code-walkthrough.md)：理解实现。

构建检查见[构建与测试](docs/testing.md)。
