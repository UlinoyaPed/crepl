#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Type.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Interpreter/Interpreter.h"
#include "clang/Interpreter/Value.h"
#include "clang/Options/OptionUtils.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/LineEditor/LineEditor.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <bitset>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>


// ============================================================
// 二进制 / 十六进制格式化
// ============================================================

static bool color_output = false;


static bool can_color(
    llvm::raw_ostream& out
)
{
    return color_output && out.has_colors();
}


static void set_color(
    llvm::raw_ostream& out,
    llvm::raw_ostream::Colors color,
    bool bold = false
)
{
    if (can_color(out))
        out.changeColor(color, bold);
}


static void reset_color(
    llvm::raw_ostream& out
)
{
    if (can_color(out))
        out.resetColor();
}


static void print_label(
    llvm::raw_ostream& out,
    llvm::StringRef text
)
{
    set_color(out, llvm::raw_ostream::BRIGHT_BLACK);
    out << text;
    reset_color(out);
}


static void print_heading(
    llvm::raw_ostream& out,
    llvm::StringRef text
)
{
    set_color(out, llvm::raw_ostream::BRIGHT_CYAN, true);
    out << text;
    reset_color(out);
}


static void print_type_name(
    llvm::raw_ostream& out,
    llvm::StringRef text
)
{
    set_color(out, llvm::raw_ostream::CYAN);
    out << text;
    reset_color(out);
}


static void print_help_line(
    llvm::StringRef command,
    llvm::StringRef description
)
{
    llvm::raw_ostream& out = llvm::outs();
    set_color(out, llvm::raw_ostream::BRIGHT_CYAN, true);
    out << command;
    reset_color(out);
    out << std::string(
        command.size() < 22 ? 22 - command.size() : 1,
        ' '
    );
    print_label(out, description);
    out << "\n";
}


static void print_colored_bits(
    llvm::raw_ostream& out,
    llvm::StringRef bits,
    llvm::StringRef changed = {}
)
{
    for (std::size_t index = 0; index < bits.size(); ++index) {
        const char ch = bits[index];
        const bool is_changed =
            index < changed.size() && changed[index] == '^';

        if (is_changed)
            set_color(out, llvm::raw_ostream::BRIGHT_RED, true);
        else if (ch == '1')
            set_color(out, llvm::raw_ostream::BRIGHT_CYAN, true);
        else if (ch == '0')
            set_color(out, llvm::raw_ostream::BRIGHT_BLACK);

        out << ch;
        reset_color(out);
    }
}


static void print_colored_hex(
    llvm::raw_ostream& out,
    llvm::StringRef hex
)
{
    for (char ch : hex) {
        if (ch == '0')
            set_color(out, llvm::raw_ostream::BRIGHT_BLACK);
        else if (std::isxdigit(static_cast<unsigned char>(ch)))
            set_color(out, llvm::raw_ostream::BRIGHT_YELLOW, true);

        out << ch;
        reset_color(out);
    }
}

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
    set_color(out, llvm::raw_ostream::CYAN);
    value.printType(out);
    reset_color(out);

    out
        << ") "
        << decimal_string(number)
        << "\n";

    // 二进制
    print_label(out, "bits : ");
    print_colored_bits(out, group_bits(binary));
    out << "\n";

    // 十六进制，每个 digit 与上面的 nibble 对齐
    print_label(out, "hex  : ");
    print_colored_hex(out, align_hex(hex));
    out << "\n";
}


// ============================================================
// 默认 Value printer
//
// 整数：我们的 printer
// bool / float / double / pointer / object：Clang 原来的 printer
// ============================================================

static void print_array_element(
    llvm::raw_ostream& out,
    const clang::ASTContext& context,
    clang::QualType type,
    const unsigned char* data
);


static void print_array_data(
    llvm::raw_ostream& out,
    const clang::ASTContext& context,
    const clang::ConstantArrayType& array,
    const unsigned char* data
)
{
    const clang::QualType element_type =
        array.getElementType();
    const std::uint64_t count =
        array.getSize().getZExtValue();
    const std::size_t stride = static_cast<std::size_t>(
        context.getTypeSizeInChars(element_type).getQuantity()
    );

    out << "[";

    for (std::uint64_t index = 0; index < count; ++index) {
        if (index != 0)
            out << ", ";

        print_array_element(
            out,
            context,
            element_type,
            data + index * stride
        );
    }

    out << "]";
}


static void print_array_integer(
    llvm::raw_ostream& out,
    clang::QualType type,
    const unsigned char* data,
    std::size_t size
)
{
    std::uint64_t raw = 0;
    std::memcpy(&raw, data, size);

    if (type->isBooleanType()) {
        out << (raw == 0 ? "false" : "true");
        return;
    }

    if (!type->isSignedIntegerType()) {
        out << raw;
        return;
    }

    if (size == sizeof(std::int64_t)) {
        std::int64_t signed_value = 0;
        std::memcpy(&signed_value, data, sizeof(signed_value));
        out << signed_value;
        return;
    }

    const unsigned bits = static_cast<unsigned>(size * CHAR_BIT);
    const std::uint64_t sign = std::uint64_t{1} << (bits - 1);
    const std::int64_t signed_value =
        static_cast<std::int64_t>((raw ^ sign) - sign);
    out << signed_value;
}


static void print_array_element(
    llvm::raw_ostream& out,
    const clang::ASTContext& context,
    clang::QualType type,
    const unsigned char* data
)
{
    if (const auto* nested =
            context.getAsConstantArrayType(type)) {
        print_array_data(out, context, *nested, data);
        return;
    }

    const std::size_t size = static_cast<std::size_t>(
        context.getTypeSizeInChars(type).getQuantity()
    );

    if (type->isIntegerType() && size <= sizeof(std::uint64_t)) {
        print_array_integer(out, type, data, size);
        return;
    }

    if (type->isRealFloatingType()) {
        if (size == sizeof(float)) {
            float number = 0;
            std::memcpy(&number, data, sizeof(number));
            out << number;
            return;
        }

        if (size == sizeof(double)) {
            double number = 0;
            std::memcpy(&number, data, sizeof(number));
            out << number;
            return;
        }

        if (size == sizeof(long double)) {
            long double number = 0;
            std::memcpy(&number, data, sizeof(number));
            std::ostringstream formatted;
            formatted << number;
            out << formatted.str();
            return;
        }
    }

    // Keep the shape useful even when an element type is not yet printable.
    out << "<value>";
}


static bool print_array(
    llvm::raw_ostream& out,
    const clang::Value& value
)
{
    const clang::ASTContext& context =
        value.getASTContext();
    const auto* array =
        context.getAsConstantArrayType(value.getType());

    if (!array)
        return false;

    out << "(";
    set_color(out, llvm::raw_ostream::CYAN);
    value.printType(out);
    reset_color(out);
    out << ") ";
    print_array_data(
        out,
        context,
        *array,
        static_cast<const unsigned char*>(value.getPtr())
    );
    out << "\n";
    return true;
}


static bool is_readable_memory(
    std::uintptr_t address,
    std::size_t size
);


struct SequenceInfo {
    const clang::ASTContext* context;
    clang::QualType element_type;
    std::uintptr_t data;
    std::size_t count;
};


static std::string centered(
    llvm::StringRef text,
    std::size_t width
);


static std::optional<std::uint64_t> find_field_bit_offset(
    const clang::ASTContext& context,
    const clang::CXXRecordDecl* record,
    llvm::StringRef field_name,
    std::uint64_t base_offset,
    std::set<const clang::CXXRecordDecl*>& active
)
{
    record = record ? record->getDefinition() : nullptr;

    if (!record || !record->isCompleteDefinition() ||
        !active.insert(record).second) {
        return std::nullopt;
    }

    for (const clang::FieldDecl* field : record->fields()) {
        const std::uint64_t offset =
            base_offset + context.getFieldOffset(field);

        if (field->getName() == field_name) {
            active.erase(record);
            return offset;
        }

        if (const auto* nested =
                field->getType()->getAsCXXRecordDecl()) {
            if (auto found = find_field_bit_offset(
                    context,
                    nested,
                    field_name,
                    offset,
                    active
                )) {
                active.erase(record);
                return found;
            }
        }
    }

    const clang::ASTRecordLayout& layout =
        context.getASTRecordLayout(record);

    for (const clang::CXXBaseSpecifier& base : record->bases()) {
        const auto* base_record =
            base.getType()->getAsCXXRecordDecl();

        if (!base_record)
            continue;

        const std::uint64_t offset = base_offset +
            static_cast<std::uint64_t>(
                (base.isVirtual()
                     ? layout.getVBaseClassOffset(base_record)
                     : layout.getBaseClassOffset(base_record))
                    .getQuantity() * CHAR_BIT
            );

        if (auto found = find_field_bit_offset(
                context,
                base_record,
                field_name,
                offset,
                active
            )) {
            active.erase(record);
            return found;
        }
    }

    active.erase(record);
    return std::nullopt;
}


static std::optional<std::uint64_t> find_field_bit_offset(
    const clang::ASTContext& context,
    const clang::CXXRecordDecl* record,
    llvm::StringRef field_name
)
{
    std::set<const clang::CXXRecordDecl*> active;
    return find_field_bit_offset(
        context,
        record,
        field_name,
        0,
        active
    );
}


static std::optional<SequenceInfo> get_sequence_info(
    const clang::Value& value
)
{
    const clang::ASTContext& context = value.getASTContext();
    clang::QualType type = value.getType();

    if (type->isReferenceType())
        type = type->getPointeeType();

    if (const auto* array = context.getAsConstantArrayType(type)) {
        return SequenceInfo{
            &context,
            array->getElementType(),
            reinterpret_cast<std::uintptr_t>(value.getPtr()),
            static_cast<std::size_t>(array->getSize().getZExtValue())
        };
    }

    const auto* record = type->getAsCXXRecordDecl();
    const auto* specialization =
        llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
            record
        );

    if (!specialization || specialization->getTemplateArgs().size() < 2)
        return std::nullopt;

    const std::string qualified_name =
        specialization->getQualifiedNameAsString();
    const clang::TemplateArgument& element_argument =
        specialization->getTemplateArgs().get(0);

    if (element_argument.getKind() != clang::TemplateArgument::Type)
        return std::nullopt;

    const clang::QualType element_type =
        element_argument.getAsType();
    const std::uintptr_t object =
        reinterpret_cast<std::uintptr_t>(value.getPtr());

    if (qualified_name == "std::array") {
        const clang::TemplateArgument& count_argument =
            specialization->getTemplateArgs().get(1);

        if (count_argument.getKind() != clang::TemplateArgument::Integral)
            return std::nullopt;

        const std::size_t count = static_cast<std::size_t>(
            count_argument.getAsIntegral().getZExtValue()
        );
        const auto data_offset =
            find_field_bit_offset(context, specialization, "_M_elems");

        if (!data_offset)
            return std::nullopt;

        return SequenceInfo{
            &context,
            element_type,
            object + *data_offset / CHAR_BIT,
            count
        };
    }

    if (qualified_name == "std::vector" &&
        !element_type->isBooleanType()) {
        const auto start_offset =
            find_field_bit_offset(context, specialization, "_M_start");
        const auto finish_offset =
            find_field_bit_offset(context, specialization, "_M_finish");

        if (!start_offset || !finish_offset)
            return std::nullopt;

        const std::uintptr_t start_address =
            object + *start_offset / CHAR_BIT;
        const std::uintptr_t finish_address =
            object + *finish_offset / CHAR_BIT;

        if (!is_readable_memory(start_address, sizeof(std::uintptr_t)) ||
            !is_readable_memory(finish_address, sizeof(std::uintptr_t))) {
            return std::nullopt;
        }

        std::uintptr_t start = 0;
        std::uintptr_t finish = 0;
        std::memcpy(
            &start,
            reinterpret_cast<const void*>(start_address),
            sizeof(start)
        );
        std::memcpy(
            &finish,
            reinterpret_cast<const void*>(finish_address),
            sizeof(finish)
        );

        const std::size_t stride = static_cast<std::size_t>(
            context.getTypeSizeInChars(element_type).getQuantity()
        );

        if (finish < start || stride == 0 ||
            (finish - start) % stride != 0) {
            return std::nullopt;
        }

        const std::size_t count = (finish - start) / stride;

        if (count > 100000)
            return std::nullopt;

        return SequenceInfo{&context, element_type, start, count};
    }

    return std::nullopt;
}


static std::string sequence_element_string(
    const SequenceInfo& sequence,
    std::size_t index
)
{
    const std::size_t stride = static_cast<std::size_t>(
        sequence.context->getTypeSizeInChars(sequence.element_type)
            .getQuantity()
    );
    std::string result;
    llvm::raw_string_ostream out(result);
    print_array_element(
        out,
        *sequence.context,
        sequence.element_type,
        reinterpret_cast<const unsigned char*>(sequence.data) +
            index * stride
    );
    out.flush();
    return result;
}


static bool print_sequence(
    llvm::raw_ostream& out,
    const clang::Value& value
)
{
    const auto sequence = get_sequence_info(value);

    if (!sequence)
        return false;

    const std::size_t stride = static_cast<std::size_t>(
        sequence->context->getTypeSizeInChars(sequence->element_type)
            .getQuantity()
    );

    if (stride != 0 &&
        sequence->count >
            std::numeric_limits<std::size_t>::max() / stride) {
        return false;
    }

    if (sequence->count != 0 &&
        !is_readable_memory(
            sequence->data,
            sequence->count * stride
        )) {
        return false;
    }

    out << "(";
    set_color(out, llvm::raw_ostream::CYAN);
    value.printType(out);
    reset_color(out);
    out << ") [";

    for (std::size_t index = 0; index < sequence->count; ++index) {
        if (index != 0)
            out << ", ";
        out << sequence_element_string(*sequence, index);
    }

    out << "]\n";
    return true;
}


static bool print_sequence_index(
    llvm::raw_ostream& out,
    const clang::Value& value
)
{
    const auto sequence = get_sequence_info(value);

    if (!sequence)
        return false;

    const std::size_t stride = static_cast<std::size_t>(
        sequence->context->getTypeSizeInChars(sequence->element_type)
            .getQuantity()
    );

    if (stride == 0 ||
        sequence->count >
            std::numeric_limits<std::size_t>::max() / stride ||
        (sequence->count != 0 &&
         !is_readable_memory(
             sequence->data,
             sequence->count * stride
         ))) {
        return false;
    }

    std::vector<std::string> indexes;
    std::vector<std::string> values;
    std::vector<std::size_t> widths;

    for (std::size_t index = 0; index < sequence->count; ++index) {
        indexes.push_back(std::to_string(index));
        values.push_back(sequence_element_string(*sequence, index));
        widths.push_back(std::max(
            indexes.back().size(),
            values.back().size()
        ));
    }

    print_label(out, "index :");
    for (std::size_t index = 0; index < sequence->count; ++index)
        out << " " << centered(indexes[index], widths[index]);

    out << "\n";
    print_label(out, "value :");
    for (std::size_t index = 0; index < sequence->count; ++index)
        out << " " << centered(values[index], widths[index]);

    out << "\n";
    return true;
}


static std::uint64_t load_integer_bits(
    const unsigned char* data,
    std::size_t size
)
{
    std::uint64_t raw = 0;
    std::memcpy(&raw, data, size);
    return raw;
}


static void print_arrow_type(
    llvm::raw_ostream& out,
    llvm::StringRef indent,
    llvm::StringRef type_name
)
{
    out << indent;
    print_heading(out, "->");
    out << " (";
    print_type_name(out, type_name);
    out << ") ";
}


static void print_pointer_target(
    llvm::raw_ostream& out,
    const clang::ASTContext& context,
    clang::QualType type,
    std::uintptr_t address,
    llvm::StringRef indent,
    unsigned depth
)
{
    if (depth >= 4 || type->isVoidType() || type->isFunctionType())
        return;

    const auto size = context.getTypeSizeInCharsIfKnown(type);

    if (!size ||
        !is_readable_memory(
            address,
            static_cast<std::size_t>(size->getQuantity())
        )) {
        out << indent;
        print_heading(out, "->");
        set_color(out, llvm::raw_ostream::BRIGHT_RED, true);
        out << " <unreadable>";
        reset_color(out);
        out << "\n";
        return;
    }

    const auto* data =
        reinterpret_cast<const unsigned char*>(address);
    const std::string type_name =
        type.getAsString(context.getPrintingPolicy());

    if (const auto* array = context.getAsConstantArrayType(type)) {
        print_arrow_type(out, indent, type_name);
        print_array_data(out, context, *array, data);
        out << "\n";
        return;
    }

    if (type->isIntegerType() &&
        size->getQuantity() <= static_cast<std::int64_t>(sizeof(std::uint64_t))) {
        print_arrow_type(out, indent, type_name);
        print_array_integer(
            out,
            type,
            data,
            static_cast<std::size_t>(size->getQuantity())
        );
        out << "\n";

        if (!type->isBooleanType()) {
            const unsigned width = static_cast<unsigned>(
                size->getQuantity() * CHAR_BIT
            );
            const std::uint64_t raw = load_integer_bits(
                data,
                static_cast<std::size_t>(size->getQuantity())
            );
            const std::string bits =
                std::bitset<64>(raw).to_string().substr(64 - width);
            std::ostringstream hex;
            hex << std::hex
                << std::nouppercase
                << std::setfill('0')
                << std::setw(static_cast<int>((width + 3) / 4))
                << raw;

            out << indent << "   ";
            print_label(out, "bits : ");
            print_colored_bits(out, group_bits(bits));
            out << "\n" << indent << "   ";
            print_label(out, "hex  : ");
            print_colored_hex(out, align_hex(hex.str()));
            out << "\n";
        }
        return;
    }

    if (type->isRealFloatingType()) {
        print_arrow_type(out, indent, type_name);
        print_array_element(out, context, type, data);
        out << "\n";
        return;
    }

    if (type->isPointerType()) {
        std::uintptr_t next = 0;
        std::memcpy(&next, data, sizeof(next));
        print_arrow_type(out, indent, type_name);

        if (next == 0) {
            out << "nullptr\n";
            return;
        }

        out << "0x" << llvm::utohexstr(next) << "\n";
        print_pointer_target(
            out,
            context,
            type->getPointeeType(),
            next,
            (indent + "   ").str(),
            depth + 1
        );
        return;
    }

    print_arrow_type(out, indent, type_name);
    set_color(out, llvm::raw_ostream::BRIGHT_MAGENTA);
    out << "@0x" << llvm::utohexstr(address);
    reset_color(out);
    out << "\n";
}


static bool print_pointer(
    llvm::raw_ostream& out,
    const clang::Value& value
)
{
    clang::QualType type = value.getType();

    if (!type->isPointerType())
        return false;

    out << "(";
    set_color(out, llvm::raw_ostream::CYAN);
    value.printType(out);
    reset_color(out);
    out << ") ";
    set_color(out, llvm::raw_ostream::BRIGHT_MAGENTA);
    value.printData(out);
    reset_color(out);
    out << "\n";

    const std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(value.getPtr());

    if (address != 0) {
        print_pointer_target(
            out,
            value.getASTContext(),
            type->getPointeeType(),
            address,
            "  ",
            0
        );
    }

    return true;
}


static void print_default_value(
    llvm::raw_ostream& out,
    const clang::Value& value
)
{
    out << "(";
    set_color(out, llvm::raw_ostream::CYAN);
    value.printType(out);
    reset_color(out);
    out << ") ";
    value.printData(out);
    out << "\n";
}

static void print_value(
    llvm::raw_ostream& out,
    const clang::Value& value
)
{
    if (!value.hasValue())
        return;

    if (print_array(out, value))
        return;

    if (print_pointer(out, value))
        return;

    if (print_sequence(out, value))
        return;

    // enum 保留 Clang 自己的输出。
    //
    // Clang 默认会尽可能显示枚举名，
    // 比单纯显示底层整数更有用。
    if (value.getType()->isEnumeralType()) {
        print_default_value(out, value);
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
        print_default_value(out, value);
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
        print_default_value(out, value);
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
    bool is_array;
    bool is_pointer;
    bool is_sequence;
    std::string fingerprint;
    std::string rendered;
};


struct MemoryRegion {
    std::uintptr_t address;
    std::size_t size;
    std::string type;
};


struct IntegerView {
    std::string type;
    unsigned width;
    std::uint64_t raw;
};


struct TypeInfo {
    std::string type;
    std::optional<std::size_t> size;
    std::optional<std::size_t> alignment;
    bool is_integer;
    bool is_boolean;
    bool is_signed;
    unsigned width;
};


struct SnapshotValue {
    std::string fingerprint;
    std::string rendered;
    std::optional<IntegerView> integer;
};


struct Snapshot {
    std::map<std::string, SnapshotValue> values;
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


static void append_input_line(
    std::string& input,
    llvm::StringRef line,
    bool add_newline = true
)
{
    if (!input.empty() && add_newline)
        input += '\n';
    input += line.str();
}


static bool needs_more_input(
    llvm::StringRef code
)
{
    enum class LexState {
        Normal,
        String,
        Character,
        LineComment,
        BlockComment
    };

    LexState state = LexState::Normal;
    bool escaped = false;
    int parentheses = 0;
    int brackets = 0;
    int braces = 0;

    for (std::size_t index = 0; index < code.size(); ++index) {
        const char ch = code[index];
        const char next = index + 1 < code.size()
            ? code[index + 1]
            : '\0';

        if (state == LexState::LineComment) {
            if (ch == '\n')
                state = LexState::Normal;
            continue;
        }

        if (state == LexState::BlockComment) {
            if (ch == '*' && next == '/') {
                state = LexState::Normal;
                ++index;
            }
            continue;
        }

        if (state == LexState::String || state == LexState::Character) {
            if (escaped) {
                escaped = false;
                continue;
            }

            if (ch == '\\') {
                escaped = true;
                continue;
            }

            if ((state == LexState::String && ch == '"') ||
                (state == LexState::Character && ch == '\'')) {
                state = LexState::Normal;
            }
            continue;
        }

        if (ch == '/' && next == '/') {
            state = LexState::LineComment;
            ++index;
        }
        else if (ch == '/' && next == '*') {
            state = LexState::BlockComment;
            ++index;
        }
        else if (ch == '"') {
            state = LexState::String;
        }
        else if (ch == '\'') {
            state = LexState::Character;
        }
        else if (ch == '(') {
            ++parentheses;
        }
        else if (ch == ')') {
            parentheses = std::max(0, parentheses - 1);
        }
        else if (ch == '[') {
            ++brackets;
        }
        else if (ch == ']') {
            brackets = std::max(0, brackets - 1);
        }
        else if (ch == '{') {
            ++braces;
        }
        else if (ch == '}') {
            braces = std::max(0, braces - 1);
        }
    }

    return parentheses > 0 || brackets > 0 || braces > 0 ||
        state == LexState::String ||
        state == LexState::Character ||
        state == LexState::BlockComment;
}


static llvm::Expected<CapturedValue> capture_expression(
    clang::Interpreter& interpreter,
    llvm::StringRef expression
)
{
    std::optional<CapturedValue> captured;
    std::optional<std::string> validation_error;

    {
        clang::Value value;

        if (auto error =
                interpreter.ParseAndExecute(expression, &value)) {
            return std::move(error);
        }

        if (!value.hasValue()) {
            validation_error = "expression has no value";
        }
        else {
            std::string type;
            llvm::raw_string_ostream type_out(type);
            value.printType(type_out);
            type_out.flush();

            const std::string rendered = value_string(value);

            captured = CapturedValue{
                value.getKind(),
                value.getType()->isArrayType(),
                value.getType()->isPointerType(),
                get_sequence_info(value).has_value(),
                type + "\n" + rendered,
                rendered
            };
        }
    }

    // The evaluation above is an implementation detail.  Remove its PTU so
    // that a later %undo still targets the user's previous input.
    if (auto error = interpreter.Undo())
        return std::move(error);

    if (validation_error) {
        return llvm::createStringError(
            std::make_error_code(std::errc::invalid_argument),
            *validation_error
        );
    }

    return std::move(*captured);
}


static llvm::Error print_index_expression(
    clang::Interpreter& interpreter,
    llvm::StringRef expression
)
{
    bool printed = false;

    {
        clang::Value value;

        if (auto error = interpreter.ParseAndExecute(expression, &value))
            return error;

        if (value.hasValue())
            printed = print_sequence_index(llvm::outs(), value);
    }

    if (auto error = interpreter.Undo())
        return error;

    if (!printed) {
        return llvm::createStringError(
            std::errc::invalid_argument,
            "expression is not a supported C array, std::array, or "
            "std::vector"
        );
    }

    return llvm::Error::success();
}


static llvm::Expected<MemoryRegion> capture_memory_region(
    clang::Interpreter& interpreter,
    llvm::StringRef expression,
    std::optional<std::size_t> requested_size
)
{
    std::optional<MemoryRegion> region;
    std::optional<std::string> validation_error;
    const std::string query = requested_size
        ? expression.str()
        : "&(" + expression.str() + ")";

    {
        clang::Value value;

        if (auto error = interpreter.ParseAndExecute(query, &value))
            return std::move(error);

        if (!value.hasValue()) {
            validation_error = "expression has no value";
        }
        else {
            const clang::ASTContext& context = value.getASTContext();
            clang::QualType type = value.getType();
            std::uintptr_t address = 0;
            std::size_t size = 0;

            if (requested_size) {
                if (type->isPointerType() || type->isArrayType()) {
                    address =
                        reinterpret_cast<std::uintptr_t>(value.getPtr());
                }
                else if (type->isIntegerType()) {
                    address = value.convertTo<std::uintptr_t>();
                }
                else {
                    validation_error =
                        "an explicit byte count requires a pointer, array, "
                        "or integer address";
                }

                size = *requested_size;
            }
            else if (!type->isPointerType()) {
                validation_error =
                    "cannot take the address of this expression";
            }
            else {
                address = reinterpret_cast<std::uintptr_t>(value.getPtr());
                type = type->getPointeeType();
                size = static_cast<std::size_t>(
                    context.getTypeSizeInChars(type).getQuantity()
                );
            }

            if (!validation_error) {
                region = MemoryRegion{
                    address,
                    size,
                    type.getAsString(context.getPrintingPolicy())
                };
            }
        }
    }

    if (auto error = interpreter.Undo())
        return std::move(error);

    if (validation_error) {
        return llvm::createStringError(
            std::make_error_code(std::errc::invalid_argument),
            *validation_error
        );
    }

    return std::move(*region);
}


static llvm::Expected<IntegerView> capture_integer(
    clang::Interpreter& interpreter,
    llvm::StringRef expression
)
{
    std::optional<IntegerView> result;
    std::optional<std::string> validation_error;

    {
        clang::Value value;

        if (auto error = interpreter.ParseAndExecute(expression, &value))
            return std::move(error);

        if (!value.hasValue()) {
            validation_error = "expression has no value";
        }
        else {
            const clang::QualType type = value.getType();

            if (!type->isIntegerType() && !type->isEnumeralType()) {
                validation_error = "expression is not an integer";
            }
            else {
                std::string type_name;
                llvm::raw_string_ostream type_out(type_name);
                value.printType(type_out);
                type_out.flush();

                const unsigned width = static_cast<unsigned>(
                    value.getASTContext().getTypeSizeInChars(type)
                        .getQuantity() * CHAR_BIT
                );

                if (width > 64) {
                    validation_error =
                        "integer widths greater than 64 are not supported";
                }
                else {
                    std::uint64_t raw = value.convertTo<std::uint64_t>();

                    if (width < 64)
                        raw &= (std::uint64_t{1} << width) - 1;

                    result = IntegerView{type_name, width, raw};
                }
            }
        }
    }

    if (auto error = interpreter.Undo())
        return std::move(error);

    if (validation_error) {
        return llvm::createStringError(
            std::make_error_code(std::errc::invalid_argument),
            *validation_error
        );
    }

    return std::move(*result);
}


static llvm::Expected<TypeInfo> capture_type_info(
    clang::Interpreter& interpreter,
    llvm::StringRef expression
)
{
    std::optional<TypeInfo> result;
    std::optional<std::string> validation_error;

    {
        clang::Value value;

        if (auto error = interpreter.ParseAndExecute(expression, &value))
            return std::move(error);

        if (!value.isValid()) {
            validation_error = "expression has no type";
        }
        else {
            const clang::ASTContext& context = value.getASTContext();
            clang::QualType type = value.getType();

            if (type->isReferenceType())
                type = type->getPointeeType();

            const auto size = context.getTypeSizeInCharsIfKnown(type);
            std::optional<std::size_t> size_bytes;
            std::optional<std::size_t> alignment_bytes;

            if (size) {
                size_bytes = static_cast<std::size_t>(size->getQuantity());
                alignment_bytes = static_cast<std::size_t>(
                    context.getTypeAlignInChars(type).getQuantity()
                );
            }

            const bool is_integer =
                type->isIntegerType() || type->isEnumeralType();
            const unsigned width = is_integer && size
                ? static_cast<unsigned>(size->getQuantity() * CHAR_BIT)
                : 0;

            result = TypeInfo{
                type.getAsString(context.getPrintingPolicy()),
                size_bytes,
                alignment_bytes,
                is_integer,
                type->isBooleanType(),
                type->isSignedIntegerOrEnumerationType(),
                width
            };
        }
    }

    if (auto error = interpreter.Undo())
        return std::move(error);

    if (validation_error) {
        return llvm::createStringError(
            std::make_error_code(std::errc::invalid_argument),
            *validation_error
        );
    }

    return std::move(*result);
}


static std::string integer_bits(
    const IntegerView& value
)
{
    return std::bitset<64>(value.raw)
        .to_string()
        .substr(64 - value.width);
}


static std::string centered(
    llvm::StringRef text,
    std::size_t width
)
{
    const std::size_t padding = width - text.size();
    const std::size_t left = padding / 2;
    return std::string(left, ' ') + text.str() +
        std::string(padding - left, ' ');
}


static void print_detailed_bits(
    const IntegerView& value
)
{
    const std::string bits = integer_bits(value);
    const unsigned nibbles = (value.width + 3) / 4;
    std::vector<std::string> labels;
    std::vector<std::size_t> columns;

    for (unsigned index = 0; index < nibbles; ++index) {
        const unsigned high = value.width - index * 4 - 1;
        const unsigned low = high >= 3 ? high - 3 : 0;
        const std::string label =
            std::to_string(high) + ".." + std::to_string(low);
        labels.push_back(label);
        columns.push_back(std::max<std::size_t>(4, label.size()));
    }

    llvm::raw_ostream& out = llvm::outs();
    print_label(out, "type : ");
    print_type_name(out, value.type);
    out << "\n";
    print_label(out, "bit  : ");

    for (unsigned index = 0; index < nibbles; ++index) {
        if (index != 0)
            out << " ";
        print_label(out, centered(labels[index], columns[index]));
    }

    out << "\n";
    print_label(out, "bits : ");

    for (unsigned index = 0; index < nibbles; ++index) {
        if (index != 0)
            out << " ";
        print_colored_bits(
            out,
            centered(
                llvm::StringRef(bits).substr(index * 4, 4),
                columns[index]
            )
        );
    }

    out << "\n";
    print_label(out, "hex  : ");
    const std::string hex = hex_string(value.raw)
        .substr(16 - nibbles);

    for (unsigned index = 0; index < nibbles; ++index) {
        if (index != 0)
            out << " ";
        print_colored_hex(
            out,
            centered(
                llvm::StringRef(hex).substr(index, 1),
                columns[index]
            )
        );
    }

    out << "\n";
}


static void print_integer_diff(
    const IntegerView& old_value,
    const IntegerView& new_value
)
{
    const std::string old_bits = integer_bits(old_value);
    const std::string new_bits = integer_bits(new_value);
    std::string difference;

    for (std::size_t index = 0; index < old_bits.size(); ++index) {
        if (index != 0 &&
            (old_bits.size() - index) % 4 == 0) {
            difference += ' ';
        }

        difference += old_bits[index] == new_bits[index] ? ' ' : '^';
    }

    llvm::raw_ostream& out = llvm::outs();
    print_label(out, "old  : ");
    print_colored_bits(out, group_bits(old_bits), difference);
    out << "\n";
    print_label(out, "new  : ");
    print_colored_bits(out, group_bits(new_bits), difference);
    out << "\n";
    print_label(out, "diff : ");
    set_color(out, llvm::raw_ostream::BRIGHT_RED, true);
    out << difference;
    reset_color(out);
    out << "\n";
}


static std::string integer_limit(
    bool is_signed,
    bool minimum,
    unsigned width
)
{
    if (!is_signed) {
        if (minimum)
            return "0";

        const std::uint64_t maximum = width == 64
            ? std::numeric_limits<std::uint64_t>::max()
            : (std::uint64_t{1} << width) - 1;
        return std::to_string(maximum);
    }

    if (width == 64) {
        return minimum
            ? std::to_string(std::numeric_limits<std::int64_t>::min())
            : std::to_string(std::numeric_limits<std::int64_t>::max());
    }

    const std::int64_t boundary =
        std::int64_t{1} << (width - 1);
    return std::to_string(minimum ? -boundary : boundary - 1);
}


static void print_type_info(
    const TypeInfo& info
)
{
    llvm::raw_ostream& out = llvm::outs();
    print_label(out, "type     : ");
    print_type_name(out, info.type);
    out << "\n";

    print_label(out, "size     : ");
    if (info.size)
        out << *info.size << " bytes\n";
    else
        out << "incomplete\n";

    if (info.alignment) {
        print_label(out, "align    : ");
        out << *info.alignment << " bytes\n";
    }

    if (info.is_integer) {
        const std::string minimum = info.is_boolean
            ? "0"
            : integer_limit(info.is_signed, true, info.width);
        const std::string maximum = info.is_boolean
            ? "1"
            : integer_limit(info.is_signed, false, info.width);

        print_label(out, "bits     : ");
        out << info.width << "\n";
        print_label(out, "signed   : ");
        out << (info.is_signed ? "yes" : "no") << "\n";
        print_label(out, "min      : ");
        out << minimum << "\n";
        print_label(out, "max      : ");
        out << maximum << "\n";
    }
}


static const clang::RecordDecl* find_record(
    clang::ASTContext& context,
    llvm::StringRef name
)
{
    if (!is_identifier(name))
        return nullptr;

    const clang::DeclarationName declaration_name(
        &context.Idents.get(name)
    );
    const auto declarations =
        context.getTranslationUnitDecl()->lookup(declaration_name);

    for (const clang::NamedDecl* declaration : declarations) {
        if (const auto* record =
                llvm::dyn_cast<clang::RecordDecl>(declaration)) {
            if (const clang::RecordDecl* definition =
                    record->getDefinition()) {
                return definition;
            }
        }

        if (const auto* type_name =
                llvm::dyn_cast<clang::TypedefNameDecl>(declaration)) {
            if (const auto* record =
                    type_name->getUnderlyingType()->getAsRecordDecl()) {
                if (const clang::RecordDecl* definition =
                        record->getDefinition()) {
                    return definition;
                }
            }
        }
    }

    return nullptr;
}


static std::string byte_count(
    std::uint64_t bytes
)
{
    return std::to_string(bytes) + (bytes == 1 ? " byte" : " bytes");
}


static void print_layout(
    clang::ASTContext& context,
    const clang::RecordDecl& record
)
{
    const clang::ASTRecordLayout& layout =
        context.getASTRecordLayout(&record);
    const std::uint64_t size = static_cast<std::uint64_t>(
        layout.getSize().getQuantity()
    );
    const std::uint64_t alignment = static_cast<std::uint64_t>(
        layout.getAlignment().getQuantity()
    );
    const char* tag = record.isUnion()
        ? "union"
        : record.isClass() ? "class" : "struct";

    llvm::raw_ostream& out = llvm::outs();
    print_heading(
        out,
        std::string(tag) + " " + record.getName().str()
    );
    out << "    ";
    print_label(out, "size=");
    out << size << " ";
    print_label(out, "align=");
    out << alignment << "\n\n";
    print_label(out, "offset  member");
    out << "\n";

    std::uint64_t cursor_bits = 0;

    for (const clang::FieldDecl* field : record.fields()) {
        const std::uint64_t offset_bits =
            context.getFieldOffset(field);
        const std::uint64_t field_bits = field->isBitField()
            ? field->getBitWidthValue()
            : context.getTypeSize(field->getType());

        if (!record.isUnion() &&
            offset_bits > cursor_bits &&
            cursor_bits % CHAR_BIT == 0 &&
            offset_bits % CHAR_BIT == 0) {
            const std::uint64_t padding =
                (offset_bits - cursor_bits) / CHAR_BIT;
            out << cursor_bits / CHAR_BIT
                << std::string(
                       cursor_bits / CHAR_BIT < 10 ? 7 : 6,
                       ' '
                   )
                ;
            set_color(out, llvm::raw_ostream::BRIGHT_YELLOW);
            out << "padding    " << byte_count(padding);
            reset_color(out);
            out << "\n";
        }

        out << offset_bits / CHAR_BIT;

        if (field->isBitField())
            out << "." << offset_bits % CHAR_BIT;

        out << std::string(offset_bits / CHAR_BIT < 10 ? 7 : 6, ' ');
        print_type_name(
            out,
            field->getType().getAsString(context.getPrintingPolicy())
        );
        out << " "
            << (field->getName().empty() ? "<unnamed>" : field->getName())
            << "    ";

        if (field->isBitField())
            out << field_bits << " bits\n";
        else
            out << byte_count(field_bits / CHAR_BIT) << "\n";

        if (!record.isUnion())
            cursor_bits = std::max(cursor_bits, offset_bits + field_bits);
    }

    const std::uint64_t total_bits = size * CHAR_BIT;

    if (!record.isUnion() &&
        total_bits > cursor_bits &&
        cursor_bits % CHAR_BIT == 0) {
        out << cursor_bits / CHAR_BIT
            << std::string(cursor_bits / CHAR_BIT < 10 ? 7 : 6, ' ')
            ;
        set_color(out, llvm::raw_ostream::BRIGHT_YELLOW);
        out << "padding    "
            << byte_count((total_bits - cursor_bits) / CHAR_BIT);
        reset_color(out);
        out << "\n";
    }
}


static bool is_readable_memory(
    std::uintptr_t address,
    std::size_t size
)
{
    if (address == 0 || size == 0 ||
        address > UINTPTR_MAX - size) {
        return false;
    }

    const std::uintptr_t requested_end = address + size;
    std::ifstream maps("/proc/self/maps");
    std::string line;

    while (std::getline(maps, line)) {
        std::istringstream fields(line);
        std::string range;
        std::string permissions;
        fields >> range >> permissions;

        const std::size_t dash = range.find('-');
        if (dash == std::string::npos ||
            permissions.empty() || permissions.front() != 'r') {
            continue;
        }

        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        std::istringstream(range.substr(0, dash)) >> std::hex >> begin;
        std::istringstream(range.substr(dash + 1)) >> std::hex >> end;

        if (address >= begin && requested_end <= end)
            return true;
    }

    return false;
}


static void print_memory(
    const MemoryRegion& region
)
{
    llvm::raw_ostream& out = llvm::outs();
    print_label(out, "address : ");
    set_color(out, llvm::raw_ostream::BRIGHT_MAGENTA);
    out << "0x" << llvm::utohexstr(region.address);
    reset_color(out);
    out << "\n";
    print_label(out, "type    : ");
    print_type_name(out, region.type);
    out << "\n";
    print_label(out, "size    : ");
    out << region.size
        << (region.size == 1 ? " byte\n" : " bytes\n")
        << "\n";
    print_heading(out, "memory:");
    out << "\n";
    print_label(out, "offset   hex   bits");
    out << "\n";

    const auto* bytes =
        reinterpret_cast<const unsigned char*>(region.address);

    for (std::size_t offset = 0; offset < region.size; ++offset) {
        const unsigned char byte = bytes[offset];
        std::ostringstream hex;
        hex << std::hex
            << std::nouppercase
            << std::setfill('0')
            << std::setw(2)
            << static_cast<unsigned>(byte);

        print_label(out, "+");
        print_label(out, std::to_string(offset));
        out
            << std::string(
                   offset < 10 ? 7 : offset < 100 ? 6 : 5,
                   ' '
               )
            ;
        print_colored_hex(out, hex.str());
        out << "    ";
        print_colored_bits(out, group_bits(binary_string(byte)));
        out << "\n";
    }

    const std::uint16_t one = 1;
    const bool little_endian =
        *reinterpret_cast<const unsigned char*>(&one) == 1;

    out << "\n";
    print_label(
        out,
        little_endian ? "little endian" : "big endian"
    );
    out << "\n";
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

        print_heading(llvm::outs(), watch.name);
        llvm::outs() << ":\n" << captured->rendered;
    }

    return changed;
}


static bool is_integer_kind(
    clang::Value::Kind kind
)
{
    return kind >= clang::Value::K_Bool &&
        kind <= clang::Value::K_ULongLong;
}


static llvm::Expected<Snapshot> capture_snapshot(
    clang::Interpreter& interpreter,
    const std::vector<Watch>& watches
)
{
    Snapshot snapshot;

    for (const Watch& watch : watches) {
        auto captured = capture_expression(interpreter, watch.name);

        if (!captured)
            return captured.takeError();

        std::optional<IntegerView> integer;

        if (is_integer_kind(captured->kind)) {
            auto integer_result =
                capture_integer(interpreter, watch.name);

            if (integer_result)
                integer = std::move(*integer_result);
            else
                llvm::consumeError(integer_result.takeError());
        }

        snapshot.values.emplace(
            watch.name,
            SnapshotValue{
                captured->fingerprint,
                captured->rendered,
                std::move(integer)
            }
        );
    }

    return snapshot;
}


static void print_state(
    const Snapshot& snapshot
)
{
    if (snapshot.values.empty()) {
        print_label(llvm::outs(), "no watched state\n");
        return;
    }

    for (const auto& entry : snapshot.values) {
        print_heading(llvm::outs(), entry.first);
        llvm::outs() << ":\n" << entry.second.rendered;
    }
}


static void print_snapshot_diff(
    const Snapshot& before,
    const Snapshot& after
)
{
    std::set<std::string> names;

    for (const auto& entry : before.values)
        names.insert(entry.first);
    for (const auto& entry : after.values)
        names.insert(entry.first);

    bool changed = false;

    for (const std::string& name : names) {
        const auto old_value = before.values.find(name);
        const auto new_value = after.values.find(name);

        if (old_value != before.values.end() &&
            new_value != after.values.end() &&
            old_value->second.fingerprint ==
                new_value->second.fingerprint) {
            continue;
        }

        changed = true;
        print_heading(llvm::outs(), name);
        llvm::outs() << ":\n";

        if (old_value == before.values.end()) {
            print_label(llvm::outs(), "before: ");
            llvm::outs() << "<not watched>\n";
            print_label(llvm::outs(), "after:\n");
            llvm::outs() << new_value->second.rendered;
            continue;
        }

        if (new_value == after.values.end()) {
            print_label(llvm::outs(), "before:\n");
            llvm::outs() << old_value->second.rendered;
            print_label(llvm::outs(), "after: ");
            llvm::outs() << "<not watched>\n";
            continue;
        }

        print_label(llvm::outs(), "before:\n");
        llvm::outs() << old_value->second.rendered;
        print_label(llvm::outs(), "after:\n");
        llvm::outs() << new_value->second.rendered;

        if (old_value->second.integer &&
            new_value->second.integer &&
            old_value->second.integer->width ==
                new_value->second.integer->width) {
            print_heading(llvm::outs(), "changed bits:");
            llvm::outs() << "\n";
            print_integer_diff(
                *old_value->second.integer,
                *new_value->second.integer
            );
        }
    }

    if (!changed)
        print_label(llvm::outs(), "no changes\n");
}


// ============================================================
// LLVM error 输出
// ============================================================

static void print_error(
    llvm::Error error
)
{
    if (can_color(llvm::errs())) {
        set_color(
            llvm::errs(),
            llvm::raw_ostream::BRIGHT_RED,
            true
        );
        llvm::errs() << "error: ";
        reset_color(llvm::errs());
        llvm::logAllUnhandledErrors(
            std::move(error),
            llvm::errs()
        );
        return;
    }

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
    color_output =
        std::getenv("NO_COLOR") == nullptr &&
        llvm::outs().has_colors();
    llvm::outs().enable_colors(color_output);
    llvm::errs().enable_colors(color_output);

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
                "#include <array>\n"
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
    bool join_next_line = false;
    std::vector<Watch> watches;
    std::vector<std::string> history;
    std::map<std::string, Snapshot> snapshots;


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
            append_input_line(
                input,
                current.drop_back(1),
                !join_next_line
            );
            join_next_line = true;

            editor.setPrompt("crepl... ");

            continue;
        }


        append_input_line(input, current, !join_next_line);
        join_next_line = false;

        if (needs_more_input(input)) {
            editor.setPrompt("crepl... ");
            continue;
        }


        // ----------------------------------------------------
        // REPL command
        // ----------------------------------------------------

        if (input == "%quit") {
            break;
        }


        if (input == "%help") {
            print_help_line("%help", "show commands");
            print_help_line("%watch [name...]", "watch variables");
            print_help_line("%unwatch [name...]", "stop watching variables");
            print_help_line("%mem <name> [bytes]", "inspect memory");
            print_help_line("%bits <expr>", "show indexed integer bits");
            print_help_line("%diff <old> <new>", "compare bits or snapshots");
            print_help_line("%type <expr>", "show type and integer limits");
            print_help_line("%index <expr>", "show sequence indexes");
            print_help_line("%sizeof <arg>", "show object size");
            print_help_line("%alignof <type>", "show type alignment");
            print_help_line("%layout <type>", "show record layout");
            print_help_line("%state", "show watched values");
            print_help_line("%snapshot <name>", "save watched state");
            print_help_line("%history [count]", "show successful inputs");
            print_help_line("%undo", "undo previous user input");
            print_help_line("%lib <path>", "load dynamic library");
            print_help_line("%quit", "exit");

            input.clear();
            editor.setPrompt("crepl> ");

            continue;
        }


        if (input == "%state") {
            auto state = capture_snapshot(*interpreter, watches);

            if (!state)
                print_error(state.takeError());
            else
                print_state(*state);

            input.clear();
            editor.setPrompt("crepl> ");
            continue;
        }


        if (
            input == "%snapshot" ||
            llvm::StringRef(input).starts_with("%snapshot ")
        ) {
            const llvm::StringRef name = input == "%snapshot"
                ? llvm::StringRef()
                : llvm::StringRef(input).drop_front(10).trim();

            if (name.empty() || !is_identifier(name)) {
                llvm::errs() << "error: usage: %snapshot <name>\n";
            }
            else if (watches.empty()) {
                llvm::errs()
                    << "error: add variables with %watch before taking a "
                    << "snapshot\n";
            }
            else {
                auto snapshot =
                    capture_snapshot(*interpreter, watches);

                if (!snapshot) {
                    print_error(snapshot.takeError());
                }
                else {
                    snapshots[name.str()] = std::move(*snapshot);
                    print_label(llvm::outs(), "saved snapshot ");
                    print_heading(llvm::outs(), name);
                    llvm::outs() << "\n";
                }
            }

            input.clear();
            editor.setPrompt("crepl> ");
            continue;
        }


        if (
            input == "%history" ||
            llvm::StringRef(input).starts_with("%history ")
        ) {
            const llvm::StringRef count_text = input == "%history"
                ? llvm::StringRef()
                : llvm::StringRef(input).drop_front(9).trim();
            std::uint64_t requested = history.size();
            bool valid = true;

            if (!count_text.empty() &&
                (count_text.getAsInteger(10, requested) || requested == 0)) {
                valid = false;
                llvm::errs()
                    << "error: usage: %history [positive-count]\n";
            }

            if (valid) {
                const std::size_t count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(requested, history.size())
                );
                const std::size_t begin = history.size() - count;

                for (std::size_t index = begin; index < history.size();
                     ++index) {
                    llvm::outs()
                        << index + 1
                        << "  "
                        << history[index]
                        << "\n";
                }
            }

            input.clear();
            editor.setPrompt("crepl> ");
            continue;
        }


        if (
            input == "%bits" ||
            llvm::StringRef(input).starts_with("%bits ")
        ) {
            const llvm::StringRef expression = input == "%bits"
                ? llvm::StringRef()
                : llvm::StringRef(input).drop_front(6).trim();

            if (expression.empty()) {
                llvm::errs() << "error: usage: %bits <expression>\n";
            }
            else {
                auto integer =
                    capture_integer(*interpreter, expression);

                if (!integer)
                    print_error(integer.takeError());
                else
                    print_detailed_bits(*integer);
            }

            input.clear();
            editor.setPrompt("crepl> ");
            continue;
        }


        if (
            input == "%type" ||
            llvm::StringRef(input).starts_with("%type ")
        ) {
            const llvm::StringRef expression = input == "%type"
                ? llvm::StringRef()
                : llvm::StringRef(input).drop_front(6).trim();

            if (expression.empty()) {
                llvm::errs() << "error: usage: %type <expression>\n";
            }
            else {
                auto info =
                    capture_type_info(*interpreter, expression);

                if (!info)
                    print_error(info.takeError());
                else
                    print_type_info(*info);
            }

            input.clear();
            editor.setPrompt("crepl> ");
            continue;
        }


        if (
            input == "%index" ||
            llvm::StringRef(input).starts_with("%index ")
        ) {
            const llvm::StringRef expression = input == "%index"
                ? llvm::StringRef()
                : llvm::StringRef(input).drop_front(7).trim();

            if (expression.empty()) {
                llvm::errs() << "error: usage: %index <expression>\n";
            }
            else if (auto error =
                         print_index_expression(
                             *interpreter,
                             expression
                         )) {
                print_error(std::move(error));
            }

            input.clear();
            editor.setPrompt("crepl> ");
            continue;
        }


        if (
            input == "%sizeof" ||
            llvm::StringRef(input).starts_with("%sizeof ")
        ) {
            const llvm::StringRef argument = input == "%sizeof"
                ? llvm::StringRef()
                : llvm::StringRef(input).drop_front(8).trim();

            if (argument.empty()) {
                llvm::errs()
                    << "error: usage: %sizeof <type-or-expression>\n";
            }
            else {
                auto size = capture_integer(
                    *interpreter,
                    "sizeof(" + argument.str() + ")"
                );

                if (!size)
                    print_error(size.takeError());
                else
                    print_label(llvm::outs(), "size : ");
                    llvm::outs() << size->raw << " bytes\n";
            }

            input.clear();
            editor.setPrompt("crepl> ");
            continue;
        }


        if (
            input == "%alignof" ||
            llvm::StringRef(input).starts_with("%alignof ")
        ) {
            const llvm::StringRef argument = input == "%alignof"
                ? llvm::StringRef()
                : llvm::StringRef(input).drop_front(9).trim();

            if (argument.empty()) {
                llvm::errs() << "error: usage: %alignof <type>\n";
            }
            else {
                auto alignment = capture_integer(
                    *interpreter,
                    "alignof(" + argument.str() + ")"
                );

                if (!alignment)
                    print_error(alignment.takeError());
                else
                    print_label(llvm::outs(), "alignment : ");
                    llvm::outs() << alignment->raw << " bytes\n";
            }

            input.clear();
            editor.setPrompt("crepl> ");
            continue;
        }


        if (
            input == "%layout" ||
            llvm::StringRef(input).starts_with("%layout ")
        ) {
            const llvm::StringRef name = input == "%layout"
                ? llvm::StringRef()
                : llvm::StringRef(input).drop_front(8).trim();

            if (name.empty() || !is_identifier(name)) {
                llvm::errs() << "error: usage: %layout <record-name>\n";
            }
            else if (const clang::RecordDecl* record =
                         find_record(
                             interpreter->getASTContext(),
                             name
                         )) {
                print_layout(interpreter->getASTContext(), *record);
            }
            else {
                llvm::errs()
                    << "error: complete record type not found: "
                    << name
                    << "\n";
            }

            input.clear();
            editor.setPrompt("crepl> ");
            continue;
        }


        if (
            input == "%diff" ||
            llvm::StringRef(input).starts_with("%diff ")
        ) {
            const llvm::StringRef arguments = input == "%diff"
                ? llvm::StringRef()
                : llvm::StringRef(input).drop_front(6).trim();
            const auto split = arguments.split(' ');
            const llvm::StringRef old_expression = split.first.trim();
            const llvm::StringRef new_expression = split.second.trim();

            if (old_expression.empty() || new_expression.empty()) {
                llvm::errs()
                    << "error: usage: %diff <old> <new-expression>\n";
            }
            else if (snapshots.count(old_expression.str()) != 0 &&
                     snapshots.count(new_expression.str()) != 0) {
                print_snapshot_diff(
                    snapshots.at(old_expression.str()),
                    snapshots.at(new_expression.str())
                );
            }
            else {
                auto old_value =
                    capture_integer(*interpreter, old_expression);
                auto new_value =
                    capture_integer(*interpreter, new_expression);

                if (!old_value) {
                    print_error(old_value.takeError());
                    if (!new_value)
                        llvm::consumeError(new_value.takeError());
                }
                else if (!new_value) {
                    print_error(new_value.takeError());
                }
                else if (old_value->width != new_value->width) {
                    llvm::errs()
                        << "error: integer widths differ ("
                        << old_value->width
                        << " vs "
                        << new_value->width
                        << ")\n";
                }
                else {
                    print_integer_diff(*old_value, *new_value);
                }
            }

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
                    print_label(llvm::outs(), "no watched variables\n");
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

                    if (captured->kind == clang::Value::K_PtrOrObj &&
                        !captured->is_array &&
                        !captured->is_pointer &&
                        !captured->is_sequence) {
                        llvm::errs()
                            << "error: %watch currently supports scalar "
                            << "and C array variables only: "
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
                        print_label(llvm::outs(), "watching ");
                        print_heading(llvm::outs(), name);
                        llvm::outs() << "\n";
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
                print_label(llvm::outs(), "cleared all watches\n");
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


        if (
            input == "%mem" ||
            llvm::StringRef(input).starts_with("%mem ")
        ) {
            const llvm::StringRef arguments = input == "%mem"
                ? llvm::StringRef()
                : llvm::StringRef(input).drop_front(5);
            std::istringstream words(
                arguments.str()
            );
            std::string expression;
            std::string size_text;
            std::string extra;
            words >> expression;
            words >> size_text;
            words >> extra;

            if (expression.empty() || !extra.empty()) {
                llvm::errs()
                    << "error: usage: %mem <name> [bytes]\n";
            }
            else {
                std::optional<std::size_t> requested_size;
                std::uint64_t parsed_size = 0;

                if (!size_text.empty()) {
                    if (llvm::StringRef(size_text).getAsInteger(
                            0,
                            parsed_size
                        ) ||
                        parsed_size == 0 ||
                        parsed_size > 65536) {
                        llvm::errs()
                            << "error: byte count must be between 1 and "
                            << "65536\n";
                    }
                    else {
                        requested_size =
                            static_cast<std::size_t>(parsed_size);
                    }
                }

                if (size_text.empty() || requested_size) {
                    auto region = capture_memory_region(
                        *interpreter,
                        expression,
                        requested_size
                    );

                    if (!region) {
                        print_error(region.takeError());
                    }
                    else if (!is_readable_memory(
                                 region->address,
                                 region->size
                             )) {
                        llvm::errs()
                            << "error: memory range is null, unmapped, or "
                            << "not readable\n";
                    }
                    else {
                        print_memory(*region);
                    }
                }
            }

            input.clear();
            editor.setPrompt("crepl> ");
            continue;
        }


        if (input == "%undo") {
            if (history.empty()) {
                llvm::errs() << "error: no user input to undo\n";
            }
            else if (
                auto error =
                    interpreter->Undo()
            ) {
                print_error(
                    std::move(error)
                );
            }
            else {
                refresh_watches(*interpreter, watches);
                if (!history.empty())
                    history.pop_back();
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
            history.push_back(input);
            const bool watched_changed =
                refresh_watches(*interpreter, watches);

            if (!watched_changed && result_output)
                print_value(llvm::outs(), value);
        }


        input.clear();

        editor.setPrompt("crepl> ");
    }


    return 0;
}
