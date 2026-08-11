# crepl

`crepl` 是一个基于 Clang Interpreter API 的增量 C++ REPL。它保留
Clang REPL 对普通声明、表达式和对象的执行能力，并为裸整数表达式提供
二进制与十六进制视图。它还可以监视状态变化、比较 bit、跟随指针、格式化
数组与常用顺序容器、检查类型和结构体布局，并逐字节查看内存。

## 依赖

当前实现已在 Fedora Linux、Clang/LLVM 22.1.8、GCC/libstdc++ 16 上验证。
构建需要 CMake 3.20+、Ninja、Clang/LLVM 22，以及包含
`LLVMConfig.cmake`、`ClangConfig.cmake`、Clang Interpreter 头文件和库的
LLVM/Clang 开发包。Fedora 可安装：

```bash
sudo dnf install clang-devel llvm-devel gcc-c++ libedit-devel cmake ninja-build
```

CMake 会把所链接 LLVM 安装中的 `clang++` 路径编入程序。程序从该可执行文件
推导 Clang 的 resource directory，以便解释器能找到 `stddef.h` 等 builtin
headers，并避免系统中存在多个 LLVM 版本时误用不匹配的驱动。

## 编译

仓库提供了 Clang + Ninja preset。在项目目录执行：

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

然后运行：

```bash
./build/bin/crepl
```

在本机 LLVM/Clang 22 路径可由 `llvm-config` 发现时，等价的单文件命令是：

```bash
clang++ crepl.cpp -o crepl \
    $(llvm-config --cxxflags) -std=c++20 \
    $(llvm-config --ldflags) \
    -lclang-cpp -lLLVM -ledit \
    $(llvm-config --system-libs)
```

也可以在已构建后直接运行黑盒回归测试：

```bash
tests/regression.sh
```

宿主程序和解释器内执行的代码现在都使用 C++20。CMake 会优先链接发行版提供的
`clang-cpp`/`LLVM` 共享目标；若发行版只提供拆分库，则回退到所需组件。
交互终端由系统 libedit 提供，因此链接时还需要 `libedit-devel`。

如果 CMake 没有自动找到 LLVM，可显式指定配置目录：

```bash
cmake --preset dev \
    -DLLVM_DIR="$(llvm-config --cmakedir)" \
    -DClang_DIR="$(llvm-config --cmakedir | sed 's#/llvm$#/clang#')"
```

## 平台与 CI

GitHub Actions 使用原生 `ubuntu-24.04` x86_64 和 `ubuntu-24.04-arm` arm64
runner，从 LLVM 官方 APT 仓库安装 LLVM 22，以 Clang、CMake 和 Ninja 完成
构建、回归测试并保存可执行文件。Ubuntu 24.04 自带的 LLVM 18 缺少本项目所用
的较新 Clang Interpreter API，不能用于该 CI 构建。

Windows 内存代码路径已使用 `VirtualQuery` 与 `ReadProcessMemory` 实现范围检查
和安全复制，移除了源代码对 `/proc/self/maps`、`process_vm_readv()` 的硬依赖。
不过 `crepl` 嵌入实验性的 Clang Interpreter，链接时需要完整 LLVM/Clang
开发 SDK。首次 CI 探测显示，Windows x86_64 runner 没有可用的完整 SDK；
Windows arm64 预览 runner 虽然带有 CMake package 和链接库，当前仍缺少 LLVM
目标引用的 ARM64 `diaguids.lib`，构建会在链接前停止。因此 Windows 还不能
作为可复现的发布目标。新的交互层还依赖 Unix libedit/termios，CMake 会在
非 Unix 主机明确拒绝配置；workflow 仍在两个 Windows 架构上记录 SDK 探测
结果。Windows 支持目前应视为未实现，而不是可用但未验证。

## 彩色输出

在交互式终端中，`crepl` 自动使用一组低干扰颜色增强层次：

- prompt 和正在编辑的整条输入使用亮绿色，清楚分隔输入与执行结果；
- 青色用于类型、watch/快照名称和指针层级；
- 暗灰色用于标签、bit 0 和零值 hex digit；
- 亮青色用于 bit 1；
- 黄色用于非零 hex digit 和 record padding；
- 红色用于发生变化的 bit、不可读目标和错误前缀；
- 紫色用于内存地址。

提示符的不可见 ANSI 序列通过 libedit 的 `EL_PROMPT_ESC` 标记，因此不会干扰
光标宽度计算。当前输入采用统一绿色；LLVM 22/libedit 没有增量语法高亮回调，
所以尚未在编辑过程中按 token 动态换色。普通数值主体仍保持终端默认颜色，
避免高密度输出过于杂乱。输出被重定向或通过管道传递时会自动关闭颜色，
因此脚本、测试和日志仍是纯文本。`TERM=dumb` 时也会关闭颜色；其他交互式
终端不依赖 LLVM 内置的终端名称列表，因此 Alacritty 等新终端同样可用。

遵循通用的 `NO_COLOR` 约定，可以显式禁用交互颜色：

```bash
NO_COLOR=1 ./build/bin/crepl
```

## 使用

普通声明会进入增量编译环境；有值的裸表达式会被打印：

```text
crepl [1]
> int x = 13;

crepl [2]
> x
(int) 13
bits : 0000 0000 0000 0000 0000 0000 0000 1101
hex  :    0    0    0    0    0    0    0    d

crepl [3]
> ~x
(int) -14
bits : 1111 1111 1111 1111 1111 1111 1111 0010
hex  :    f    f    f    f    f    f    f    2

crepl [4]
> (-x == ~x + 1)
(bool) true

crepl [5]
> unsigned char c = 13;

crepl [6]
> c
(unsigned char) 13
bits : 0000 1101
hex  :    0    d

crepl [7]
> 1.0 / 3
(double) 0.33333333
```

编号只在代码实际提交给 Interpreter 后推进；空输入和纯前端命令不推进。
括号、方括号或花括号尚未闭合时，REPL 自动使用 `. ` continuation prompt：

```cpp
crepl [8]
> int factorial(int n) {
.   if (n <= 1)
.       return 1;
.   return n * factorial(n - 1);
. }
```

自动判断会忽略普通/raw 字符串、字符字面量和注释中的 delimiter。行尾 `\`
的手动续行方式仍然保留。当前不会把模板尖括号当作续行 delimiter；对于以 `-`
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
耦合点。当前只接受 `std::vector<T, std::allocator<T>>`，并验证两个字段确实
是大小匹配、指向 `T` 的原生指针；自定义 allocator、fancy pointer、
`std::vector<bool>` 或检查不通过的实现会回退到 Clang 默认对象输出。
嵌套标准容器元素暂时显示为 `<value>`，原生多维 C 数组不受此限制。

所有序列视图和 watch fingerprint 对整个对象共享 256 个叶子元素的预算，
不是每一维各自拥有 256 项；多维数组和嵌套容器因此不会产生乘法级输出。
超出时显示 `... <N more>`。watch 能检测序列长度和预算覆盖范围内的变化，
但不会因预算之外的单独变化而触发输出；这是为了让每条 REPL 输入的自动
监视成本保持有界。

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

类型查询通过内部 `decltype((expr))` 在 unevaluated context 中完成，不会执行
表达式；例如 `%type ++x` 不会修改 `x`。查询产生的临时 type alias 会立即用
`Interpreter::Undo()` 清除，也不会占用用户可见的撤销历史。整数位宽来自
`ASTContext::getIntWidth()`，min/max 使用 `llvm::APInt` 计算，因此
`__int128` 和 `_BitInt(N)` 不受宿主 `uint64_t` 位宽限制。

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

解引用前先检查当前平台的内存映射，再把目标字节安全复制到本地 buffer，
渲染层不会直接访问目标地址。对象指针只显示目标类型和地址，
不会猜测业务对象内部关系；链表等 record 级可视化仍属于后续扩展。

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
预算覆盖范围内的序列元素变化时会打印新状态：

```text
crepl> %watch a
watching a
crepl> a[2] = 3;
a:
(int[5]) [5, 2, 3, 1, 7]
```

watch 的状态判定不使用 pretty-print 文本：sequence fingerprint 会递归记录
类型、元素数量和元素状态。因此即使嵌套 vector 当前显示为 `[<value>]`，
其内部元素变化仍会触发 watch 和 snapshot diff；暂不理解的 record 则记录
受字节预算约束的 object representation，而不是固定占位符。

有返回值的裸表达式总是先输出自身结果，再输出 watch 更新。例如 `x++` 会先
显示旧值，再显示 watch 中的新 `x`。写成 `x++;` 时属于 Clang 的
discarded-value statement，不产生表达式结果，但 watch 仍会更新。

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

快照只包含拍摄时的监视集合。`%history` 列出本 session 中已提交 Interpreter
的 execution，失败输入也会占用编号；`%history 10` 只显示最后十项，多行定义
算一项。`%rerun n`（缩写 `%r n`）把该 execution 的原始输入作为新 execution
执行。libedit 还把真正提交的输入持久化到
`$XDG_DATA_HOME/crepl/history`，未设置时使用
`~/.local/share/crepl/history`；空输入、命令和 Ctrl-C 取消内容不会写入。

每个有返回值的 execution 保存一份 `clang::Value` 副本。`$_` 打印最近值，
`$n` 打印指定 execution 的值；两者是只读前端查询，不推进 execution 编号。
无值声明和不存在的编号会给出明确诊断。成功 `%undo` 只撤销上一段成功的增量
编译输入；它不会回滚运行时副作用，也不会改写已经分配的 execution 编号。

`%time expression` 对这次 `ParseAndExecute`、值渲染和 watch 刷新的整体墙钟
耗时计时，并自动选择 ns/us/ms/s。LLVM 22 的公开 Interpreter API 没有稳定的
compile/execute 分段计时接口，因此当前只报告诚实的总耗时。

启动时会在内建常用头文件之后执行 `~/.config/crepl/init.hpp`（或
`$XDG_CONFIG_HOME/crepl/init.hpp`）。文件不存在时静默跳过；诊断不会阻止进入
REPL。`%reset` 重建 Interpreter、清除 session 状态和值、重置编号并重新执行
startup。`%reload` 采用相同的完整 session 重建语义，以避免普通变量和函数的
C++ 重定义；持久化编辑历史不会被删除。

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
1–65536 字节；该限制同时应用于显式长度和默认 `sizeof(object)` 路径。

读取前程序会检查内存映射，拒绝 null、地址溢出、未映射或不可读的范围；
Linux 使用 `/proc/self/maps` 与 `process_vm_readv()`，Windows 使用
`VirtualQuery` 与 `ReadProcessMemory`，之后统一复制到本地 buffer 再渲染。
这会降低常见非法地址、映射竞争和渲染期间地址变化导致崩溃的概率，但仍是
best-effort 保护，不是 debugger 的一致性快照：其他线程仍可能并发修改内容，
读取也可能因为映射或后备文件状态变化而失败。内存查看应只用于当前进程中
生命周期有效的对象和指针。

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

union 和 bit-field 也可检查。为避免把基类子对象或 vptr 错报成 padding，
当前 `%layout` 遇到有基类或 dynamic class 的 C++ record 会明确拒绝诊断；
尚未实现继承树、virtual base 和虚表布局展示。

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
%history [count]   显示本 session 的 execution
%rerun <n> / %r <n> 重新执行指定输入
%time <expr>       执行并报告整体耗时
%reset             重建 Interpreter 和 session
%reload            重建 session 并重新读取 init.hpp
%undo          撤销上一段增量输入
%lib <path>    加载动态库
%quit          退出
$_              打印最近一次返回值
$n              打印 execution n 的返回值
```

交互按键：Tab 使用当前 Clang session 做语义补全；↑/↓ 和 Ctrl-P/Ctrl-N 浏览
历史；Ctrl-R 反向搜索持久历史；Ctrl-L 清屏。编辑时 Ctrl-C 会清除整个尚未
提交的单行或多行区域，在同一个 execution prompt 原地重绘，不输出 `^C`、
不记录历史，也不改变 Interpreter。运行中 JIT 代码的安全中断尚未实现。

`%undo` 撤销上一段增量编译输入，但不会回滚那段代码已经造成的内存写入或
其他运行时副作用；这是 Clang Interpreter 的原始语义。

## LLVM API 注意事项

本项目直接使用 `clang::Interpreter`、`clang::Value` 和
`IncrementalCompilerBuilder`。这些 Interpreter API 在 Clang 头文件中标为
实验性接口，LLVM 大版本升级时应重新确认：

- `Interpreter::ParseAndExecute(code, Value*)` 的签名；
- `Value::Kind` 枚举和相应的 `getInt()` 等访问器；
- `IncrementalCompilerBuilder::CreateCpp()` 的返回类型；
- `ReplCodeCompleter::codeComplete()` 及 parent `CompilerInstance` 导入方式；
- `clang::GetResourcesPath()` 的位置和行为；
- `ASTContext::getIntWidth()`、record layout、field/base offset 和 template
  specialization 接口。

变量监视、快照和辅助捕获依赖 `Interpreter::Undo()` 清理内部求值产生的增量
编译单元；`%type` 则用同一机制清理 unevaluated type alias。Linux 内存范围
检查使用 `/proc/self/maps`，安全复制使用
`process_vm_readv()`；Windows 对应路径使用 `VirtualQuery` 与
`ReadProcessMemory`。Windows 的完整程序构建仍取决于可链接的 Clang
Interpreter 开发 SDK。

`std::vector` pretty printer 通过 Clang AST 查找当前 libstdc++ 实例化中的
`_M_start` 和 `_M_finish`，不硬编码字节 offset，但仍依赖这些字段名和布局
语义。它只为 `std::allocator<T>` 启用，并检查字段是同类型的原生 `T*`；
检查不通过时回退为 Clang 默认对象输出。字段名称保持不变但语义发生变化的
未来 ABI 仍无法仅靠 AST 完全证明安全，因此升级 libstdc++ 后应重新验证。

自动多行输入由宿主侧 lexer 状态、raw string delimiter 和 `()[]{}` 平衡判断
完成，不调用 Clang 的私有 parser recovery 状态。LLVM 22 没有公开的、可在不
污染增量会话的前提下查询“输入是否仅因 EOF 而不完整”的 API。这覆盖函数、
控制块、初始化列表、lambda、注释、字符串和跨行表达式，但没有显式 delimiter
的语法续行仍可能需要行尾 `\`。

Tab 首先复用 LLVM 22 的 `clang::ReplCodeCompleter`。该版本的 importer 能补全
当前 session 的顶层用户声明，但不会递归导入 `std` NamespaceDecl 的成员；
crepl 因此用当前 Interpreter AST 对限定命名空间做语义兜底查找，例如
`std::vec` → `std::vector`。候选仍来自 Clang AST，不是静态字符串表。LLVM 22
实现还把 completion source line 固定为 1，因此 continuation 行只补全已经提交
到 session 的名字，不承诺识别尚未提交函数体中的局部声明。

`crepl.cpp` 显式包含 `clang/Frontend/CompilerInstance.h`。这是必要的：
`CreateCpp()` 返回 `std::unique_ptr<clang::CompilerInstance>`，而
`Interpreter.h` 在 Clang 22 中只提供该类型的前置声明。
