# crepl

`crepl` 是一个基于 Clang Interpreter API 的增量 C++ REPL。它保留
Clang REPL 对普通声明、表达式和对象的执行能力，并为裸整数表达式提供
二进制与十六进制视图。它还可以监视变量变化、格式化 C 数组，并逐字节
查看对象或指针指向的内存。

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
指针、枚举和对象等其余值继续走 `clang::Value::print()`。

原生 C 数组使用紧凑的方括号格式，支持整数、`bool`、浮点元素和多维数组：

```text
crepl> int a[] = {5, 2, 8, 1, 7};
crepl> a
(int[5]) [5, 2, 8, 1, 7]

crepl> int matrix[2][3] = {{1, 2, 3}, {4, 5, 6}};
crepl> matrix
(int[2][3]) [[1, 2, 3], [4, 5, 6]]
```

不支持格式化的数组元素暂时显示为 `<value>`。`std::array` 和
`std::vector` 目前仍按普通对象处理，不属于原生 C 数组 printer 的范围。

## 监视变量

`%watch` 接受一个或多个变量名。注册时保存当前值；之后每成功执行一段
用户代码，只打印值确实发生变化的监视变量：

```text
crepl> int x = 13;
crepl> %watch x
watching x

crepl> x <<= 1;
x:
(int) 26
bits : 0000 0000 0000 0000 0000 0000 0001 1010
hex  :    0    0    0    0    0    0    1    a

crepl> x |= 1;
x:
(int) 27
bits : 0000 0000 0000 0000 0000 0000 0001 1011
hex  :    0    0    0    0    0    0    1    b
```

可以同时监视多个变量：

```text
%watch i left right mid a
```

当前支持标量和原生 C 数组。数组任一元素变化时会打印数组的新状态：

```text
crepl> %watch a
watching a
crepl> a[2] = 3;
a:
(int[5]) [5, 2, 3, 1, 7]
```

不带参数的 `%watch` 列出监视项。`%unwatch x` 删除指定监视项；不带参数的
`%unwatch` 清空全部监视项。监视求值产生的内部增量编译单元会立即撤销，
因此不会占用用户可见的 `%undo` 历史。

## 查看内存

不指定长度时，`%mem` 查看变量对象自身的全部字节：

```text
crepl> int x = 0x12345678;
crepl> %mem x
address : 0x7F...
type    : int
size    : 4 bytes

memory:
offset   hex   bits
+0       78    0111 1000
+1       56    0101 0110
+2       34    0011 0100
+3       12    0001 0010

little endian
```

指定长度时，参数被当作指针或整数地址，从该地址开始读取相应字节数：

```text
crepl> int *p = &x;
crepl> %mem p 32
```

两种形式的差别是：`%mem p` 查看指针变量 `p` 自身的 `sizeof(p)` 个字节，
而 `%mem p 32` 查看 `p` 指向地址开始的 32 字节。单次长度限制为
1–65536 字节。

读取前程序会检查 Linux `/proc/self/maps`，拒绝 null、地址溢出、未映射或
不可读的范围。这能阻止常见的错误地址导致进程崩溃，但内存查看仍应只用于
当前进程中生命周期有效的对象和指针。

## REPL 命令

```text
%help          显示命令
%watch [name...] 监视或列出变量
%unwatch [name...] 停止监视或清空监视项
%mem <name> [bytes] 查看对象或指针内存
%undo          撤销上一段增量输入
%lib <path>    加载动态库
%quit          退出
```

`%undo` 撤销上一段增量编译输入，但不会回滚那段代码已经造成的内存写入或
其他运行时副作用；这是 Clang Interpreter 的原始语义。

## LLVM API 注意事项

本项目直接使用 `clang::Interpreter`、`clang::Value` 和
`IncrementalCompilerBuilder`。这些 Interpreter API 在 Clang 头文件中标为
实验性接口，LLVM 大版本升级时应重新确认：

- `Interpreter::ParseAndExecute(code, Value*)` 的签名；
- `Value::Kind` 枚举和相应的 `getInt()` 等访问器；
- `IncrementalCompilerBuilder::CreateCpp()` 的返回类型；
- `clang::GetResourcesPath()` 的位置和行为。

变量监视和内存查看还依赖 `Interpreter::Undo()` 清理内部求值产生的增量
编译单元。内存范围检查通过 `/proc/self/maps` 实现，因此 `%mem` 当前明确
面向 Linux。

`crepl.cpp` 显式包含 `clang/Frontend/CompilerInstance.h`。这是必要的：
`CreateCpp()` 返回 `std::unique_ptr<clang::CompilerInstance>`，而
`Interpreter.h` 在 Clang 22 中只提供该类型的前置声明。
