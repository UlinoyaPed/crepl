# crepl

`crepl` 是一个基于 Clang Interpreter API 的增量 C++ REPL。它保留
Clang REPL 对普通声明、表达式和对象的执行能力，并为裸整数表达式提供
二进制与十六进制视图。它还可以监视状态变化、比较 bit、跟随指针、格式化
数组与常用顺序容器、检查类型和结构体布局，并逐字节查看内存。

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

## 彩色输出

在交互式终端中，`crepl` 自动使用一组低干扰颜色增强层次：

- 青色用于类型、watch/快照名称和指针层级；
- 暗灰色用于标签、bit 0 和零值 hex digit；
- 亮青色用于 bit 1；
- 黄色用于非零 hex digit 和 record padding；
- 红色用于发生变化的 bit、不可读目标和错误前缀；
- 紫色用于内存地址。

prompt 和普通数值主体保持终端默认颜色，避免影响 LineEditor 的光标宽度，
也避免高密度输出过于杂乱。输出被重定向或通过管道传递时会自动关闭颜色，
因此脚本、测试和日志仍是纯文本。

遵循通用的 `NO_COLOR` 约定，可以显式禁用交互颜色：

```bash
NO_COLOR=1 ./crepl
```

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

括号、方括号或花括号尚未闭合时，REPL 会自动切换到 `crepl...` prompt：

```cpp
crepl> int factorial(int n) {
crepl... if (n <= 1)
crepl...     return 1;
crepl... return n * factorial(n - 1);
crepl... }
```

自动判断会忽略字符串、字符字面量和注释中的 delimiter。行尾 `\` 的手动
续行方式仍然保留。当前不会把模板尖括号当作续行 delimiter；对于以 `-`
或 `~` 开头而被 Clang 顶层解析器拒绝的表达式，可加括号，例如
`(~x + 1)`。

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
枚举和不支持特殊格式的对象继续走 `clang::Value::print()`。

原生 C 数组使用紧凑的方括号格式，支持整数、`bool`、浮点元素和多维数组：

```text
crepl> int a[] = {5, 2, 8, 1, 7};
crepl> a
(int[5]) [5, 2, 8, 1, 7]

crepl> int matrix[2][3] = {{1, 2, 3}, {4, 5, 6}};
crepl> matrix
(int[2][3]) [[1, 2, 3], [4, 5, 6]]
```

`std::array` 和普通 `std::vector<T>` 也使用相同的列表格式：

```text
crepl> std::array<int, 3> sa = {1, 2, 3};
crepl> sa
(std::array<int, 3> &) [1, 2, 3]

crepl> std::vector<int> v = {4, 5, 6};
crepl> v
(std::vector<int, std::allocator<int> > &) [4, 5, 6]

crepl> %index v
index : 0 1 2
value : 4 5 6
```

`%index` 同样支持原生 C 数组。`std::vector` 的读取使用本机 Clang AST 定位
libstdc++ 的 `_M_start` / `_M_finish` 字段，因此这是明确的 libstdc++ ABI
耦合点。位压缩特化 `std::vector<bool>` 保留默认输出；嵌套标准容器元素
暂时显示为 `<value>`，原生多维 C 数组不受此限制。

## Bit 与类型检查

`%bits` 在普通整数输出之外增加 bit index 标尺：

```text
crepl> %bits x
type : int
bit  : 31..28 27..24 23..20 19..16 15..12 11..8 7..4 3..0
bits :  0000   0000   0000   0000   0000  0000  0000 1101
hex  :   0      0      0      0      0      0    0    d
```

`%diff` 比较两个相同位宽的整数表达式。第一个参数是不含空格的旧值表达式，
第二个参数可以包含空格：

```text
crepl> %diff x (x ^ 7)
old  : 0000 0000 0000 0000 0000 0000 0000 1101
new  : 0000 0000 0000 0000 0000 0000 0000 1010
diff :                                     ^^^
```

`%type` 显示表达式的类型、大小、对齐；整数还会显示位宽、符号和范围：

```text
crepl> %type 1ULL
type     : unsigned long long
size     : 8 bytes
align    : 8 bytes
bits     : 64
signed   : no
min      : 0
max      : 18446744073709551615
```

## 指针输出

非 null 指针会在原地址输出之后安全地展示目标值。整数目标继续显示完整
bits/hex，数组指针显示数组，指针链最多跟随四层：

```text
crepl> int *p = &x;
crepl> p
(int *) 0x7F...
  -> (int) 13
     bits : 0000 0000 0000 0000 0000 0000 0000 1101
     hex  :    0    0    0    0    0    0    0    d
```

解引用前同样检查 `/proc/self/maps`。对象指针只显示目标类型和地址，不会猜测
业务对象内部关系；链表等 record 级可视化仍属于后续扩展。

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

当前支持标量、指针、原生 C 数组、`std::array` 和普通 `std::vector`。
序列任一元素变化时会打印新状态：

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

## 状态、快照与历史

`%state` 打印所有当前监视值。`%snapshot name` 保存整个 `%watch` 集合的
当前状态；当 `%diff` 的两个参数都是快照名时，会自动进行快照比较：

```text
crepl> %snapshot before
saved snapshot before
crepl> x <<= 3;
x:
(int) 104
bits : 0000 0000 0000 0000 0000 0000 0110 1000
hex  :    0    0    0    0    0    0    6    8
crepl> %snapshot after
saved snapshot after
crepl> %diff before after
x:
before:
(int) 13
...
after:
(int) 104
...
changed bits:
old  : 0000 0000 0000 0000 0000 0000 0000 1101
new  : 0000 0000 0000 0000 0000 0000 0110 1000
diff :                                ^^   ^ ^
```

快照只包含拍摄时的监视集合。`%history` 列出成功执行的 C++ 输入，
`%history 10` 只显示最后十项；多行定义算一项。成功 `%undo` 会同步移除
最后一项历史，但 Clang Interpreter 不会回滚该输入已经产生的内存副作用。

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

## 大小、对齐与 record 布局

`%sizeof` 接受类型或表达式，`%alignof` 接受类型。`%layout` 接受当前翻译
单元中的简单 struct/class/union 名称，并显示字段、padding、总大小和对齐：

```text
crepl> struct A { char c; int x; short s; };
crepl> %layout A
struct A    size=12 align=4

offset  member
0       char c    1 byte
1       padding    3 bytes
4       int x    4 bytes
8       short s    2 bytes
10      padding    2 bytes

crepl> %sizeof A
size : 12 bytes
crepl> %alignof A
alignment : 4 bytes
```

union 和 bit-field 也可检查。当前 `%layout` 以直接字段布局为重点，不展开
C++ 对象的继承树或虚表。

## REPL 命令

```text
%help          显示命令
%watch [name...] 监视或列出变量
%unwatch [name...] 停止监视或清空监视项
%mem <name> [bytes] 查看对象或指针内存
%bits <expr>       显示带 bit index 的整数视图
%diff <old> <new>  比较整数或两个快照
%type <expr>       显示类型、大小、对齐和整数范围
%index <expr>      显示序列索引和值
%sizeof <arg>      计算类型或表达式大小
%alignof <type>    计算类型对齐
%layout <type>     显示 record 字段和 padding
%state             显示全部监视值
%snapshot <name>   保存监视状态
%history [count]   显示成功执行的 C++ 输入
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
- `clang::GetResourcesPath()` 的位置和行为；
- `ASTContext` 的 record layout、field/base offset 和 template specialization
  接口。

变量监视和内存查看还依赖 `Interpreter::Undo()` 清理内部求值产生的增量
编译单元。内存范围检查通过 `/proc/self/maps` 实现，因此 `%mem` 当前明确
面向 Linux。

`std::vector` pretty printer 通过 Clang AST 查找当前 libstdc++ 实例化中的
`_M_start` 和 `_M_finish`，不硬编码字节 offset，但仍依赖这些字段名和三指针
布局语义。换用 libc++ 或升级到改变内部结构的 libstdc++ 时会自动回退为
Clang 默认对象输出，应重新验证这一部分。

自动多行输入由宿主侧 lexer 状态和 `()[]{}` 平衡判断完成，不调用 Clang 的
私有 parser recovery 状态。这覆盖函数、控制块、初始化列表和跨行表达式，
但复杂 raw string delimiter 等边界仍可能需要行尾 `\` 手动续行。

`crepl.cpp` 显式包含 `clang/Frontend/CompilerInstance.h`。这是必要的：
`CreateCpp()` 返回 `std::unique_ptr<clang::CompilerInstance>`，而
`Interpreter.h` 在 Clang 22 中只提供该类型的前置声明。
