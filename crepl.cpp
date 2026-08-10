#include "clang/AST/Type.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Interpreter/Interpreter.h"
#include "clang/Interpreter/Value.h"
#include "clang/Options/OptionUtils.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/LineEditor/LineEditor.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <bitset>
#include <cctype>
#include <climits>
#include <cstddef>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>


// ============================================================
// 二进制 / 十六进制格式化
// ============================================================

template <typename T>
static std::string binary_string(T value)
{
    using V = typename std::remove_cv<T>::type;
    using U = typename std::make_unsigned<V>::type;

    static_assert(
        sizeof(V) <= sizeof(unsigned long long),
        "integer too wide"
    );

    constexpr std::size_t width =
        sizeof(V) * CHAR_BIT;

    U u = static_cast<U>(value);

    return std::bitset<width>(
        static_cast<unsigned long long>(u)
    ).to_string();
}


static std::string group_bits(
    const std::string& s
)
{
    std::ostringstream out;

    for (std::size_t i = 0; i < s.size(); ++i) {

        if (
            i != 0 &&
            (s.size() - i) % 4 == 0
        ) {
            out << ' ';
        }

        out << s[i];
    }

    return out.str();
}


template <typename T>
static std::string hex_string(T value)
{
    using V = typename std::remove_cv<T>::type;
    using U = typename std::make_unsigned<V>::type;

    constexpr std::size_t width =
        sizeof(V) * CHAR_BIT;

    constexpr std::size_t hex_width =
        (width + 3) / 4;

    U u = static_cast<U>(value);

    std::ostringstream out;

    out
        << std::hex
        << std::nouppercase
        << std::setfill('0')
        << std::setw(
               static_cast<int>(hex_width)
           )
        << static_cast<unsigned long long>(u);

    return out.str();
}


static std::string align_hex(
    const std::string& s
)
{
    std::ostringstream out;

    for (std::size_t i = 0; i < s.size(); ++i) {

        if (i != 0)
            out << ' ';

        // 一个 hex digit 对齐上面的 4 bit
        out << "   " << s[i];
    }

    return out.str();
}


// ============================================================
// 十进制输出
//
// char / unsigned char 也作为数字处理。
// ============================================================

template <typename T>
static std::string decimal_string(T value)
{
    std::ostringstream out;

    if constexpr (std::is_signed<T>::value) {
        out << static_cast<long long>(value);
    }
    else {
        out << static_cast<unsigned long long>(value);
    }

    return out.str();
}


// ============================================================
// 我们自己的整数 printer
// ============================================================

template <typename T>
static void print_integer(
    llvm::raw_ostream& out,
    const clang::Value& value,
    T number
)
{
    const std::string binary =
        binary_string(number);

    const std::string hex =
        hex_string(number);

    // 第一行尽量保持 clang-repl 原来的风格：
    //
    // (int) 13

    out << "(";

    value.printType(out);

    out
        << ") "
        << decimal_string(number)
        << "\n";

    // 二进制
    out
        << "bits : "
        << group_bits(binary)
        << "\n";

    // 十六进制，每个 digit 与上面的 nibble 对齐
    out
        << "hex  : "
        << align_hex(hex)
        << "\n";
}


// ============================================================
// 默认 Value printer
//
// 整数：我们的 printer
// bool / float / double / pointer / object：Clang 原来的 printer
// ============================================================

static void print_value(
    llvm::raw_ostream& out,
    const clang::Value& value
)
{
    if (!value.hasValue())
        return;

    // enum 保留 Clang 自己的输出。
    //
    // Clang 默认会尽可能显示枚举名，
    // 比单纯显示底层整数更有用。
    if (value.getType()->isEnumeralType()) {
        value.print(out);
        return;
    }

    switch (value.getKind()) {

    // --------------------------------------------------------
    // bool 保持：
    //
    // (bool) true
    //
    // 否则比较表达式会显得太啰嗦。
    // --------------------------------------------------------

    case clang::Value::K_Bool:
        value.print(out);
        return;


    // --------------------------------------------------------
    // char
    // --------------------------------------------------------

    case clang::Value::K_Char_S:
        print_integer(
            out,
            value,
            value.getChar_S()
        );
        return;

    case clang::Value::K_SChar:
        print_integer(
            out,
            value,
            value.getSChar()
        );
        return;

    case clang::Value::K_Char_U:
        print_integer(
            out,
            value,
            value.getChar_U()
        );
        return;

    case clang::Value::K_UChar:
        print_integer(
            out,
            value,
            value.getUChar()
        );
        return;


    // --------------------------------------------------------
    // short
    // --------------------------------------------------------

    case clang::Value::K_Short:
        print_integer(
            out,
            value,
            value.getShort()
        );
        return;

    case clang::Value::K_UShort:
        print_integer(
            out,
            value,
            value.getUShort()
        );
        return;


    // --------------------------------------------------------
    // int
    // --------------------------------------------------------

    case clang::Value::K_Int:
        print_integer(
            out,
            value,
            value.getInt()
        );
        return;

    case clang::Value::K_UInt:
        print_integer(
            out,
            value,
            value.getUInt()
        );
        return;


    // --------------------------------------------------------
    // long
    // --------------------------------------------------------

    case clang::Value::K_Long:
        print_integer(
            out,
            value,
            value.getLong()
        );
        return;

    case clang::Value::K_ULong:
        print_integer(
            out,
            value,
            value.getULong()
        );
        return;


    // --------------------------------------------------------
    // long long
    // --------------------------------------------------------

    case clang::Value::K_LongLong:
        print_integer(
            out,
            value,
            value.getLongLong()
        );
        return;

    case clang::Value::K_ULongLong:
        print_integer(
            out,
            value,
            value.getULongLong()
        );
        return;


    // --------------------------------------------------------
    // 其他：
    //
    // float
    // double
    // long double
    // pointer
    // struct/class
    // array
    // ...
    //
    // 全部继续使用 Clang 默认输出。
    // --------------------------------------------------------

    default:
        value.print(out);
        return;
    }
}


static std::string value_string(
    const clang::Value& value
)
{
    std::string result;
    llvm::raw_string_ostream out(result);
    print_value(out, value);
    out.flush();
    return result;
}


static void print_error(llvm::Error error);


// ============================================================
// 变量监视
// ============================================================

struct Watch {
    std::string name;
    std::string fingerprint;
};


struct CapturedValue {
    clang::Value::Kind kind;
    std::string fingerprint;
    std::string rendered;
};


static bool is_identifier(
    llvm::StringRef name
)
{
    if (name.empty() ||
        !(std::isalpha(static_cast<unsigned char>(name.front())) ||
          name.front() == '_')) {
        return false;
    }

    for (char ch : name.drop_front()) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
            return false;
    }

    return true;
}


static llvm::Expected<CapturedValue> capture_expression(
    clang::Interpreter& interpreter,
    llvm::StringRef expression
)
{
    std::optional<CapturedValue> captured;

    {
        clang::Value value;

        if (auto error =
                interpreter.ParseAndExecute(expression, &value)) {
            return std::move(error);
        }

        if (!value.hasValue()) {
            return llvm::createStringError(
                std::errc::invalid_argument,
                "expression has no value"
            );
        }

        std::string type;
        llvm::raw_string_ostream type_out(type);
        value.printType(type_out);
        type_out.flush();

        std::string data;
        llvm::raw_string_ostream data_out(data);
        value.printData(data_out);
        data_out.flush();

        captured = CapturedValue{
            value.getKind(),
            type + "\n" + data,
            value_string(value)
        };
    }

    // The evaluation above is an implementation detail.  Remove its PTU so
    // that a later %undo still targets the user's previous input.
    if (auto error = interpreter.Undo())
        return std::move(error);

    return std::move(*captured);
}


static bool refresh_watches(
    clang::Interpreter& interpreter,
    std::vector<Watch>& watches
)
{
    bool changed = false;

    for (Watch& watch : watches) {
        auto captured =
            capture_expression(interpreter, watch.name);

        if (!captured) {
            llvm::errs() << "watch " << watch.name << ": ";
            print_error(captured.takeError());
            continue;
        }

        if (captured->fingerprint == watch.fingerprint)
            continue;

        watch.fingerprint = captured->fingerprint;
        changed = true;

        llvm::outs()
            << watch.name
            << ":\n"
            << captured->rendered;
    }

    return changed;
}


// ============================================================
// LLVM error 输出
// ============================================================

static void print_error(
    llvm::Error error
)
{
    llvm::logAllUnhandledErrors(
        std::move(error),
        llvm::errs(),
        "error: "
    );
}


// ============================================================
// main
// ============================================================

int main()
{
    // --------------------------------------------------------
    // 初始化 LLVM JIT 所需 target。
    //
    // 官方 clang-repl 做的也是这一类初始化。
    // --------------------------------------------------------

    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllAsmParsers();


    // --------------------------------------------------------
    // 创建 IncrementalCompilerBuilder
    // --------------------------------------------------------

    clang::IncrementalCompilerBuilder builder;


    // 我们明确用 C++20。
    //
    // 想换成 C++17：
    //
    //     "-std=c++17"
    //
    // IncrementalCompilerBuilder does not infer Clang's builtin-header
    // directory for an arbitrary embedding executable.  Locate the matching
    // clang++ driver and derive its resource directory (for stddef.h, etc.).
    std::string clang_binary = "clang++";

    if (auto path =
            llvm::sys::findProgramByName("clang++")) {
        clang_binary = *path;
    }

    std::vector<std::string> clang_arg_storage = {
        "-std=c++20",
        "-resource-dir=" +
            clang::GetResourcesPath(clang_binary)
    };

    std::vector<const char*> clang_args;
    clang_args.reserve(clang_arg_storage.size());

    for (const std::string& arg : clang_arg_storage)
        clang_args.push_back(arg.c_str());

    builder.SetCompilerArgs(clang_args);


    // --------------------------------------------------------
    // CompilerInstance
    // --------------------------------------------------------

    auto compiler_or_error =
        builder.CreateCpp();

    if (!compiler_or_error) {
        print_error(
            compiler_or_error.takeError()
        );

        return 1;
    }


    // --------------------------------------------------------
    // Interpreter
    // --------------------------------------------------------

    auto interpreter_or_error =
        clang::Interpreter::create(
            std::move(*compiler_or_error)
        );

    if (!interpreter_or_error) {
        print_error(
            interpreter_or_error.takeError()
        );

        return 1;
    }

    std::unique_ptr<clang::Interpreter> interpreter =
        std::move(*interpreter_or_error);


    // --------------------------------------------------------
    // 自动 include
    //
    // 所以进入 REPL 后不用再：
    //
    //     #include <iostream>
    //     #include <bitset>
    // --------------------------------------------------------

    if (
        auto error =
            interpreter->ParseAndExecute(
                "#include <iostream>\n"
                "#include <bitset>\n"
                "#include <vector>\n"
                "#include <string>\n"
                "#include <algorithm>\n"
                "#include <cstdint>\n"
            )
    ) {
        print_error(std::move(error));
        return 1;
    }


    // --------------------------------------------------------
    // REPL
    // --------------------------------------------------------

    llvm::LineEditor editor("crepl");

    editor.setPrompt("crepl> ");

    std::string input;
    std::vector<Watch> watches;


    while (
        std::optional<std::string> line =
            editor.readLine()
    ) {
        llvm::StringRef current =
            llvm::StringRef(*line).trim();


        // 空行
        if (
            current.empty() &&
            input.empty()
        ) {
            continue;
        }


        // ----------------------------------------------------
        // 和 clang-repl 一样：
        //
        // 行尾 \ 表示继续下一行
        // ----------------------------------------------------

        if (current.ends_with("\\")) {

            input +=
                current.drop_back(1).str();

            // 预处理指令换行有意义
            if (current.starts_with("#"))
                input += '\n';

            editor.setPrompt("crepl... ");

            continue;
        }


        input += current.str();


        // ----------------------------------------------------
        // REPL command
        // ----------------------------------------------------

        if (input == "%quit") {
            break;
        }


        if (input == "%help") {

            llvm::outs()
                << "%help          show commands\n"
                << "%watch [name...] watch scalar variables\n"
                << "%unwatch [name...] stop watching variables\n"
                << "%undo          undo previous input\n"
                << "%lib <path>    load dynamic library\n"
                << "%quit          exit\n";

            input.clear();
            editor.setPrompt("crepl> ");

            continue;
        }


        if (
            input == "%watch" ||
            llvm::StringRef(input).starts_with("%watch ")
        ) {
            std::istringstream words(input);
            std::string command;
            std::string name;
            words >> command;

            if (!(words >> name)) {
                if (watches.empty()) {
                    llvm::outs() << "no watched variables\n";
                }
                else {
                    for (const Watch& watch : watches)
                        llvm::outs() << watch.name << "\n";
                }
            }
            else {
                do {
                    if (!is_identifier(name)) {
                        llvm::errs()
                            << "error: invalid variable name: "
                            << name
                            << "\n";
                        continue;
                    }

                    auto captured =
                        capture_expression(*interpreter, name);

                    if (!captured) {
                        llvm::errs() << "watch " << name << ": ";
                        print_error(captured.takeError());
                        continue;
                    }

                    if (captured->kind == clang::Value::K_PtrOrObj) {
                        llvm::errs()
                            << "error: %watch currently supports scalar "
                            << "variables only: "
                            << name
                            << "\n";
                        continue;
                    }

                    auto existing = std::find_if(
                        watches.begin(),
                        watches.end(),
                        [&](const Watch& watch) {
                            return watch.name == name;
                        }
                    );

                    if (existing == watches.end()) {
                        watches.push_back(
                            Watch{name, captured->fingerprint}
                        );
                        llvm::outs() << "watching " << name << "\n";
                    }
                    else {
                        existing->fingerprint = captured->fingerprint;
                    }
                } while (words >> name);
            }

            input.clear();
            editor.setPrompt("crepl> ");
            continue;
        }


        if (
            input == "%unwatch" ||
            llvm::StringRef(input).starts_with("%unwatch ")
        ) {
            std::istringstream words(input);
            std::string command;
            std::string name;
            words >> command;

            if (!(words >> name)) {
                watches.clear();
                llvm::outs() << "cleared all watches\n";
            }
            else {
                do {
                    auto new_end = std::remove_if(
                        watches.begin(),
                        watches.end(),
                        [&](const Watch& watch) {
                            return watch.name == name;
                        }
                    );
                    watches.erase(new_end, watches.end());
                } while (words >> name);
            }

            input.clear();
            editor.setPrompt("crepl> ");
            continue;
        }


        if (input == "%undo") {

            if (
                auto error =
                    interpreter->Undo()
            ) {
                print_error(
                    std::move(error)
                );
            }
            else {
                refresh_watches(*interpreter, watches);
            }

            input.clear();
            editor.setPrompt("crepl> ");

            continue;
        }


        if (
            llvm::StringRef(input)
                .starts_with("%lib ")
        ) {
            std::string path =
                llvm::StringRef(input)
                    .drop_front(5)
                    .trim()
                    .str();

            if (
                auto error =
                    interpreter
                        ->LoadDynamicLibrary(
                            path.c_str()
                        )
            ) {
                print_error(
                    std::move(error)
                );
            }

            input.clear();
            editor.setPrompt("crepl> ");

            continue;
        }


        // ----------------------------------------------------
        // 这里就是关键。
        //
        // 官方 clang-repl 大体相当于：
        //
        //     ParseAndExecute(input)
        //
        // 我们传入 Value：
        //
        //     ParseAndExecute(input, &value)
        //
        // 于是结果不会立即被官方 printer 打印，
        // 而是交给我们。
        // ----------------------------------------------------

        clang::Value value;
        std::optional<std::string> result_output;
        bool execution_succeeded = false;

        if (
            auto error =
                interpreter->ParseAndExecute(
                    input,
                    &value
                )
        ) {
            print_error(
                std::move(error)
            );
        }
        else if (value.hasValue()) {
            result_output = value_string(value);
            execution_succeeded = true;
        }
        else {
            execution_succeeded = true;
        }

        if (execution_succeeded) {
            const bool watched_changed =
                refresh_watches(*interpreter, watches);

            if (!watched_changed && result_output)
                llvm::outs() << *result_output;
        }


        input.clear();

        editor.setPrompt("crepl> ");
    }


    return 0;
}
