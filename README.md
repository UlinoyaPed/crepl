# crepl

`crepl` 是一个基于 Clang Interpreter API 的增量 C++ REPL。它保留
Clang REPL 对普通声明、表达式和对象的执行能力，并为裸整数表达式提供
二进制与十六进制视图。

## 依赖

当前实现已在 Fedora Linux、Clang/LLVM 22.1.8、GCC/libstdc++ 16 上验证。
需要安装：

```bash
sudo dnf install clang-devel llvm-devel gcc-c++
```

运行时需要能在 `PATH` 中找到与链接的 `libclang-cpp` 相匹配的 `clang++`。
程序会从该可执行文件推导 Clang 的 resource directory，以便解释器能找到
`stddef.h` 等 Clang builtin headers。

## 编译

在项目目录执行：

```bash
clang++ \
    crepl.cpp \
    -o crepl \
    $(llvm-config --cxxflags) \
    -std=c++17 \
    $(llvm-config --ldflags) \
    -lclang-cpp \
    -lLLVM \
    $(llvm-config --system-libs)
```

然后运行：

```bash
./crepl
```

解释器内使用 C++20（`crepl.cpp` 传给 `IncrementalCompilerBuilder` 的选项），
而宿主程序本身以 C++17 编译。

## 使用

普通声明会进入增量编译环境；有值的裸表达式会被打印：

```text
crepl> int x = 13;

crepl> x
(int) 13
bits : 0000 0000 0000 0000 0000 0000 0000 1101
hex  :    0    0    0    0    0    0    0    d

crepl> ~x
(int) -14
bits : 1111 1111 1111 1111 1111 1111 1111 0010
hex  :    f    f    f    f    f    f    f    2

crepl> (-x == ~x + 1)
(bool) true

crepl> unsigned char c = 13;

crepl> c
(unsigned char) 13
bits : 0000 1101
hex  :    0    d

crepl> 1.0 / 3
(double) 0.33333333
```

以 `\` 结束一行可继续输入多行代码。对于某些以 `-` 或 `~` 开头的顶层
表达式，Clang 增量解析可能要求加括号，例如输入 `(~x + 1)`。

## 输出规则

以下类型使用自定义整数输出：

- `char`、`signed char`、`unsigned char`
- `short`、`unsigned short`
- `int`、`unsigned int`
- `long`、`unsigned long`
- `long long`、`unsigned long long`

第一行是 Clang 风格的 `(type) value`。`bits` 显示该类型完整机器位宽的
二进制补码，每四位分组；`hex` 的每个 digit 与相应四位 nibble 对齐。

`bool` 继续使用 Clang 默认的 `(bool) true` / `(bool) false` 格式。浮点数、
指针、枚举、数组和对象等其余值也继续走 `clang::Value::print()`。

## REPL 命令

```text
%help          显示命令
%undo          撤销上一段增量输入
%lib <path>    加载动态库
%quit          退出
```

## LLVM API 注意事项

本项目直接使用 `clang::Interpreter`、`clang::Value` 和
`IncrementalCompilerBuilder`。这些 Interpreter API 在 Clang 头文件中标为
实验性接口，LLVM 大版本升级时应重新确认：

- `Interpreter::ParseAndExecute(code, Value*)` 的签名；
- `Value::Kind` 枚举和相应的 `getInt()` 等访问器；
- `IncrementalCompilerBuilder::CreateCpp()` 的返回类型；
- `clang::GetResourcesPath()` 的位置和行为。

`crepl.cpp` 显式包含 `clang/Frontend/CompilerInstance.h`。这是必要的：
`CreateCpp()` 返回 `std::unique_ptr<clang::CompilerInstance>`，而
`Interpreter.h` 在 Clang 22 中只提供该类型的前置声明。
