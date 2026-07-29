# 构建与验证策略

普通 `debug` 预设保留未优化的调试构建，适合逐行观察算法。`sanitized` 使用 `-g -O1`，同时启用 AddressSanitizer、UndefinedBehaviorSanitizer 和帧指针；当前 Linux 构建仍使用已验证的非 PIE 配置。优化允许编译器内联大量短小的标准库函数，调试符号保留，局部变量和源码单步顺序可能与无优化构建不同。

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug -R '^cli_basics$' --output-on-failure

cmake --preset sanitized
cmake --build --preset sanitized
ctest --preset sanitized -R '^cli_basics$' --output-on-failure
```

采用基础优化的依据是实际测量：大数量路径用例在无优化的 GCC sanitizer 构建中，采样多次落在 ASan FakeStack 的短函数调用处理；同一用例在普通 Debug 中只需数秒。编译时的 `-O1` 可以减少这种调用开销。AddressSanitizer 的[官方使用文档](https://clang.llvm.org/docs/AddressSanitizer.html#usage)也建议使用至少 `-O1` 取得合理性能；该资料描述 Clang，本项目当前的验证编译器是 GCC。

当前 Linux/GCC 环境的 100000 条 reflog 编解码用例，未优化 sanitized 曾用时约 192 秒，本轮 `-O1` 用时约 21 秒。该对比用于解释工程选择，各机器的耗时应通过实际运行测量。

数量和字节预算的回归保留真实输入，包含展开路径、组合后的 Index、损坏对象及失败后状态不变等检查。构建保留已有 sanitizer 选项和完整断言。

通常只运行新功能及其受影响模块，具体筛选与结果写在 [进度](progress.md)。阶段验收和影响整个构建的编译选项调整运行完整回归。完整 sanitizer 命令为：

```sh
ctest --preset sanitized --output-on-failure
```

CTest 注册总数与本次实际执行数分别记录。遇到失败先定位；受影响范围已经确定时只复验修补的部分。中途取消的检查不算通过，编译成功也不能代替行为测试。sanitizer 通过表示本轮执行未报告对应错误，不证明全部平台、线程交错或任意系统调用中断均已覆盖。

编译参数可在构建目录的 `compile_commands.json` 中核对；`-O1`、`-g`、`-fsanitize=address,undefined` 与 `-fno-omit-frame-pointer` 应同时存在。可重复 CLI 实验使用临时仓库，见 [学习笔记](learning-notes.md)，核心原理从 [代码导读](code-walkthrough.md)进入。
