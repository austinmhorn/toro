#include "toro/CGenerator.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toro {
namespace {

enum class CValueKind { Int, Dec, Bool, String, Struct, Class, Enum, Result, Void };

struct CValueType {
    CValueKind kind;
    std::string nominal_name;
    std::vector<CValueType> arguments;
    bool nullable;

    CValueType(
        CValueKind kind,
        std::string nominal_name = {},
        std::vector<CValueType> arguments = {},
        bool nullable = false)
        : kind(kind)
        , nominal_name(std::move(nominal_name))
        , arguments(std::move(arguments))
        , nullable(nullable)
    {
    }

    bool operator==(const CValueType&) const = default;
};

const CValueType int_type{CValueKind::Int, {}};
const CValueType dec_type{CValueKind::Dec, {}};
const CValueType bool_type{CValueKind::Bool, {}};
const CValueType string_type{CValueKind::String, {}};
const CValueType void_type{CValueKind::Void, {}};

struct ValueInfo {
    CValueType type;
    std::string c_name;
    bool pointer{false};
    bool owned_local{false};
};

struct FunctionInfo {
    const FunctionDeclarationStmt* declaration;
    CValueType return_type;
    std::vector<CValueType> parameter_types;
};

struct StructFieldInfo {
    const StructField* declaration;
    CValueType type;
};

struct MethodInfo {
    const MethodDeclaration* declaration;
    CValueType return_type;
    std::vector<CValueType> parameter_types;
};

struct StructInfo {
    const StructDeclarationStmt* declaration;
    std::vector<StructFieldInfo> fields;
    std::unordered_map<std::string, std::size_t> field_indices;
    std::unordered_map<std::string, MethodInfo> methods;
};

struct ClassFieldInfo {
    const ClassField* declaration;
    CValueType type;
};

struct ClassInfo {
    const ClassDeclarationStmt* declaration;
    std::vector<ClassFieldInfo> fields;
    std::unordered_map<std::string, std::size_t> field_indices;
    std::unordered_map<std::string, MethodInfo> methods;
};

struct EnumVariantInfo {
    const EnumVariant* declaration;
    std::optional<CValueType> payload_type;
};

struct EnumInfo {
    const EnumDeclarationStmt* declaration;
    std::vector<EnumVariantInfo> variants;
    std::unordered_map<std::string, std::size_t> variant_indices;
};

struct ResultInfo {
    CValueType type;
    SourceLocation location;
};

struct GeneratedExpression {
    std::string code;
    CValueType type;
    bool addressable{false};
    bool pointer{false};
    std::string prelude;
    bool owned{false};

    GeneratedExpression(
        std::string code,
        CValueType type,
        bool addressable = false,
        bool pointer = false,
        std::string prelude = {},
        bool owned = false)
        : code(std::move(code))
        , type(std::move(type))
        , addressable(addressable)
        , pointer(pointer)
        , prelude(std::move(prelude))
        , owned(owned)
    {
    }
};

[[noreturn]] void throw_backend_error(
    SourceLocation location,
    const std::string& message)
{
    throw std::runtime_error(
        "line " + std::to_string(location.line) + ", column "
        + std::to_string(location.column) + ": backend error: " + message);
}

std::string indent(std::size_t depth)
{
    return std::string(depth * 4, ' ');
}

std::string indent_prelude(std::string_view prelude, std::size_t depth)
{
    std::string output;
    std::size_t start = 0;
    while (start < prelude.size()) {
        const std::size_t newline = prelude.find('\n', start);
        const std::size_t length = newline == std::string_view::npos
            ? prelude.size() - start
            : newline - start;
        output += indent(depth);
        output.append(prelude.substr(start, length));
        output += '\n';
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
    return output;
}

std::string function_name(std::string_view name)
{
    return "toro_fn_" + std::string(name);
}

std::string struct_name(std::string_view name)
{
    return "toro_struct_" + std::string(name);
}

std::string class_name(std::string_view name)
{
    return "toro_class_" + std::to_string(name.size()) + "_"
        + std::string(name);
}

std::string retain_name(std::string_view name)
{
    return class_name(name) + "_retain";
}

std::string release_name(std::string_view name)
{
    return class_name(name) + "_release";
}

std::string enum_name(std::string_view name)
{
    return "toro_enum_" + std::to_string(name.size()) + "_"
        + std::string(name);
}

std::string enum_tag_type_name(std::string_view name)
{
    return enum_name(name) + "_tag";
}

std::string enum_tag_name(std::string_view enum_type, std::string_view variant)
{
    return enum_name(enum_type) + "_tag_" + std::string(variant);
}

std::string enum_payload_name(std::string_view variant)
{
    return "toro_variant_" + std::string(variant);
}

std::string type_mangle(const CValueType& type)
{
    switch (type.kind) {
    case CValueKind::Int: return "i";
    case CValueKind::Dec: return "d";
    case CValueKind::Bool: return "b";
    case CValueKind::String: return "s";
    case CValueKind::Struct:
        return "s" + std::to_string(type.nominal_name.size()) + "_"
            + type.nominal_name;
    case CValueKind::Class:
        return "c" + std::to_string(type.nominal_name.size()) + "_"
            + type.nominal_name;
    case CValueKind::Enum:
        return "e" + std::to_string(type.nominal_name.size()) + "_"
            + type.nominal_name;
    case CValueKind::Result:
        return "r" + std::to_string(type_mangle(type.arguments[0]).size()) + "_"
            + type_mangle(type.arguments[0]) + "_"
            + std::to_string(type_mangle(type.arguments[1]).size()) + "_"
            + type_mangle(type.arguments[1]);
    case CValueKind::Void: return "v";
    }
    return "unknown";
}

std::string result_name(const CValueType& type)
{
    return "toro_result_" + type_mangle(type);
}

std::string field_name(std::string_view name)
{
    return "toro_field_" + std::string(name);
}

std::string method_name(std::string_view owner, std::string_view name)
{
    return "toro_method_" + std::to_string(owner.size()) + "_"
        + std::string(owner) + "_" + std::string(name);
}

std::string parameter_name(std::string_view name)
{
    return "toro_arg_" + std::string(name);
}

std::string variable_name(std::string_view name)
{
    return "toro_var_" + std::string(name);
}

std::string c_type_name(CValueType type)
{
    switch (type.kind) {
    case CValueKind::Int: return "int64_t";
    case CValueKind::Dec: return "double";
    case CValueKind::Bool: return "bool";
    case CValueKind::String: return "const char*";
    case CValueKind::Struct: return struct_name(type.nominal_name);
    case CValueKind::Class: return class_name(type.nominal_name) + "*";
    case CValueKind::Enum: return enum_name(type.nominal_name);
    case CValueKind::Result: return result_name(type);
    case CValueKind::Void: return "void";
    }
    return "void";
}

std::string escape_c_string(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 2);
    result += '"';
    for (const char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += character; break;
        }
    }
    result += '"';
    return result;
}

class Generator {
public:
    std::string generate(const Program& program)
    {
        collect_types(program);
        collect_functions(program);
        collect_local_types(program);

        std::string output =
            "#include <stdbool.h>\n"
            "#include <stdint.h>\n"
            "#include <stdio.h>\n"
            "#include <stdlib.h>\n"
            "#include <string.h>\n\n";

        if (!class_order_.empty()) {
            output +=
                "typedef struct toro_weak_control\n"
                "{\n"
                "    void* toro_object;\n"
                "    uint64_t toro_weak_count;\n"
                "} toro_weak_control;\n\n"
                "typedef struct toro_weak_ref\n"
                "{\n"
                "    toro_weak_control* toro_control;\n"
                "} toro_weak_ref;\n\n"
                "static void toro_weak_clear(toro_weak_ref* toro_ref)\n"
                "{\n"
                "    toro_weak_control* toro_control = toro_ref->toro_control;\n"
                "    toro_ref->toro_control = NULL;\n"
                "    if (toro_control != NULL && --toro_control->toro_weak_count == 0)\n"
                "    {\n"
                "        free(toro_control);\n"
                "    }\n"
                "}\n\n"
                "static void toro_weak_set(toro_weak_ref* toro_ref, toro_weak_control* toro_control)\n"
                "{\n"
                "    if (toro_ref->toro_control == toro_control) { return; }\n"
                "    toro_weak_clear(toro_ref);\n"
                "    toro_ref->toro_control = toro_control;\n"
                "    if (toro_control != NULL) { ++toro_control->toro_weak_count; }\n"
                "}\n\n"
                "static void* toro_weak_load(const toro_weak_ref* toro_ref)\n"
                "{\n"
                "    return toro_ref->toro_control == NULL\n"
                "        ? NULL\n"
                "        : toro_ref->toro_control->toro_object;\n"
                "}\n\n";
        }

        for (const auto& struct_declaration : struct_order_) {
            output += "typedef struct " + struct_name(struct_declaration->name)
                + " " + struct_name(struct_declaration->name) + ";\n";
        }
        for (const auto& class_declaration : class_order_) {
            output += "typedef struct " + class_name(class_declaration->name)
                + " " + class_name(class_declaration->name) + ";\n";
        }
        for (const auto& enum_declaration : enum_order_) {
            output += "typedef struct " + enum_name(enum_declaration->name)
                + " " + enum_name(enum_declaration->name) + ";\n";
        }
        for (const auto& result : result_order_) {
            output += "typedef struct " + result_name(result.type) + " "
                + result_name(result.type) + ";\n";
        }
        if (!struct_order_.empty() || !class_order_.empty() || !enum_order_.empty()
            || !result_order_.empty()) {
            output += '\n';
        }
        for (const auto* declaration : class_order_) {
            output += "static void " + retain_name(declaration->name) + "("
                + class_name(declaration->name) + "* toro_value);\n";
            output += "static void " + release_name(declaration->name) + "("
                + class_name(declaration->name) + "* toro_value);\n";
        }
        if (!class_order_.empty()) {
            output += '\n';
        }

        std::unordered_map<std::string, int> definition_state;
        for (const auto& type : nominal_order_) {
            output += emit_type_definition(type, definition_state);
        }
        for (const auto& result : result_order_) {
            output += emit_type_definition(result.type, definition_state);
        }

        for (const auto& statement : program.statements) {
            if (statement->kind == StmtKind::StructDeclaration
                || statement->kind == StmtKind::ClassDeclaration
                || statement->kind == StmtKind::EnumDeclaration) {
                continue;
            }
            if (statement->kind != StmtKind::FunctionDeclaration) {
                throw_backend_error(
                    statement->location,
                    "top-level statements are not supported by the C backend");
            }
            output += function_declaration(
                static_cast<const FunctionDeclarationStmt&>(*statement));
            output += ";\n";
        }
        for (const auto* declaration : struct_order_) {
            const auto& info = structs_.at(declaration->name);
            for (const auto& method : declaration->methods) {
                const auto& method_declaration =
                    static_cast<const MethodDeclaration&>(*method);
                output += method_declaration_text(
                    declaration->name,
                    CValueKind::Struct,
                    method_declaration,
                    info.methods.at(method_declaration.name));
                output += ";\n";
            }
        }
        for (const auto* declaration : class_order_) {
            const auto& info = classes_.at(declaration->name);
            for (const auto& member : declaration->members) {
                if (member->kind != ClassMemberKind::Method) {
                    continue;
                }
                const auto& method = static_cast<const MethodDeclaration&>(*member);
                output += method_declaration_text(
                    declaration->name,
                    CValueKind::Class,
                    method,
                    info.methods.at(method.name));
                output += ";\n";
            }
        }
        if (!program.statements.empty() || !struct_order_.empty()
            || !class_order_.empty()
            || !enum_order_.empty()) {
            output += '\n';
        }

        for (const auto& statement : program.statements) {
            if (statement->kind == StmtKind::StructDeclaration
                || statement->kind == StmtKind::ClassDeclaration
                || statement->kind == StmtKind::EnumDeclaration) {
                continue;
            }
            output += emit_function(
                static_cast<const FunctionDeclarationStmt&>(*statement));
            output += '\n';
        }
        for (const auto* declaration : struct_order_) {
            const auto& info = structs_.at(declaration->name);
            for (const auto& method : declaration->methods) {
                const auto& method_declaration =
                    static_cast<const MethodDeclaration&>(*method);
                output += emit_method(
                    declaration->name,
                    CValueKind::Struct,
                    method_declaration,
                    info.methods.at(method_declaration.name));
                output += '\n';
            }
        }
        for (const auto* declaration : class_order_) {
            const auto& info = classes_.at(declaration->name);
            for (const auto& member : declaration->members) {
                if (member->kind != ClassMemberKind::Method) {
                    continue;
                }
                const auto& method = static_cast<const MethodDeclaration&>(*member);
                output += emit_method(
                    declaration->name,
                    CValueKind::Class,
                    method,
                    info.methods.at(method.name));
                output += '\n';
            }
        }

        const auto main_function = functions_.find("main");
        if (main_function != functions_.end()) {
            const auto& main = main_function->second;
            if (!main.parameter_types.empty()) {
                throw_backend_error(
                    main.declaration->location,
                    "toro entry function 'main' cannot have parameters in the C backend");
            }
            output += "int main(void)\n{\n";
            if (main.return_type == void_type) {
                output += "    " + function_name("main") + "();\n";
                output += "    return 0;\n";
            } else if (main.return_type == int_type) {
                output += "    return (int)" + function_name("main") + "();\n";
            } else {
                throw_backend_error(
                    main.declaration->location,
                    "toro entry function 'main' must return int or have no return type");
            }
            output += "}\n";
        }

        return output;
    }

private:
    static bool contains_class_reference(const CValueType& type)
    {
        if (type.kind == CValueKind::Class) {
            return true;
        }
        return std::ranges::any_of(
            type.arguments,
            [](const CValueType& argument) {
                return contains_class_reference(argument);
            });
    }

    static bool statement_guarantees_return(const Stmt& statement)
    {
        switch (statement.kind) {
        case StmtKind::Return:
            return true;
        case StmtKind::Block:
            return statements_guarantee_return(
                static_cast<const BlockStmt&>(statement).statements);
        case StmtKind::If: {
            const auto& conditional = static_cast<const IfStmt&>(statement);
            return conditional.else_branch
                && statements_guarantee_return(conditional.then_block->statements)
                && statement_guarantees_return(*conditional.else_branch);
        }
        case StmtKind::Handle: {
            const auto& handle = static_cast<const HandleStmt&>(statement);
            return !handle.cases.empty()
                && std::ranges::all_of(
                    handle.cases,
                    [](const HandleCase& handle_case) {
                        return statements_guarantee_return(
                            handle_case.body->statements);
                    });
        }
        default:
            return false;
        }
    }

    static bool statements_guarantee_return(
        const std::vector<std::unique_ptr<Stmt>>& statements)
    {
        return !statements.empty()
            && statement_guarantees_return(*statements.back());
    }

    void collect_types(const Program& program)
    {
        for (const auto& statement : program.statements) {
            if (statement->kind == StmtKind::StructDeclaration) {
                const auto& declaration =
                    static_cast<const StructDeclarationStmt&>(*statement);
                if (!declaration.generic_parameters.empty()) {
                    throw_backend_error(
                        declaration.location,
                        "generic structs are not supported by the C backend");
                }
                if (!declaration.interfaces.empty()) {
                    throw_backend_error(
                        declaration.location,
                        "struct interface implementations are not supported by the C backend");
                }
                if (!declaration.conversions.empty()) {
                    throw_backend_error(
                        declaration.location,
                        "struct conversion overloads are not supported by the C backend");
                }
                structs_.emplace(
                    declaration.name, StructInfo{&declaration, {}, {}, {}});
                struct_order_.push_back(&declaration);
                nominal_order_.push_back(
                    CValueType{CValueKind::Struct, declaration.name});
            } else if (statement->kind == StmtKind::EnumDeclaration) {
                const auto& declaration =
                    static_cast<const EnumDeclarationStmt&>(*statement);
                enums_.emplace(declaration.name, EnumInfo{&declaration, {}, {}});
                enum_order_.push_back(&declaration);
                nominal_order_.push_back(
                    CValueType{CValueKind::Enum, declaration.name});
            } else if (statement->kind == StmtKind::ClassDeclaration) {
                const auto& declaration =
                    static_cast<const ClassDeclarationStmt&>(*statement);
                if (declaration.is_abstract) {
                    throw_backend_error(
                        declaration.location,
                        "abstract classes are not supported by the C backend");
                }
                if (!declaration.generic_parameters.empty()) {
                    throw_backend_error(
                        declaration.location,
                        "generic classes are not supported by the C backend");
                }
                if (declaration.base_type) {
                    throw_backend_error(
                        declaration.location,
                        "class inheritance is not supported by the C backend");
                }
                if (!declaration.interfaces.empty()) {
                    throw_backend_error(
                        declaration.location,
                        "class interface implementations are not supported by the C backend");
                }
                classes_.emplace(
                    declaration.name, ClassInfo{&declaration, {}, {}, {}});
                class_order_.push_back(&declaration);
                nominal_order_.push_back(
                    CValueType{CValueKind::Class, declaration.name});
            }
        }

        for (const auto* declaration : struct_order_) {
            auto& info = structs_.at(declaration->name);
            for (const auto& field : declaration->fields) {
                const CValueType field_type = lower_type(field.type);
                if (contains_class_reference(field_type)) {
                    throw_backend_error(
                        field.location,
                        "class-reference struct fields are not supported by the C backend");
                }
                info.field_indices.emplace(field.name, info.fields.size());
                info.fields.push_back(StructFieldInfo{&field, field_type});
            }
            for (const auto& member : declaration->methods) {
                const auto& method = static_cast<const MethodDeclaration&>(*member);
                if (!method.generic_parameters.empty()) {
                    throw_backend_error(
                        method.location,
                        "generic struct methods are not supported by the C backend");
                }
                if (method.is_virtual || method.is_override || !method.body) {
                    throw_backend_error(
                        method.location,
                        "virtual or bodyless struct methods are not supported by the C backend");
                }
                MethodInfo method_info{&method, void_type, {}};
                if (method.return_type) {
                    method_info.return_type = lower_type(*method.return_type);
                }
                for (const auto& parameter : method.parameters) {
                    method_info.parameter_types.push_back(lower_type(parameter.type));
                }
                if (!info.methods.emplace(method.name, std::move(method_info)).second) {
                    throw_backend_error(
                        method.location,
                        "struct method overloads are not supported by the C backend: '"
                            + declaration->name + "." + method.name + "'");
                }
            }
        }

        for (const auto* declaration : enum_order_) {
            auto& info = enums_.at(declaration->name);
            for (const auto& variant : declaration->variants) {
                std::optional<CValueType> payload_type;
                if (variant.payload_type) {
                    payload_type = lower_type(*variant.payload_type);
                    if (contains_class_reference(*payload_type)) {
                        throw_backend_error(
                            variant.location,
                            "class-reference enum payloads are not supported by the C backend");
                    }
                }
                info.variant_indices.emplace(variant.name, info.variants.size());
                info.variants.push_back(EnumVariantInfo{
                    &variant,
                    std::move(payload_type),
                });
            }
        }

        for (const auto* declaration : class_order_) {
            auto& info = classes_.at(declaration->name);
            for (const auto& member : declaration->members) {
                if (member->kind == ClassMemberKind::Conversion) {
                    throw_backend_error(
                        member->location,
                        "class conversion overloads are not supported by the C backend");
                }
                if (member->kind == ClassMemberKind::Field) {
                    const auto& field = static_cast<const ClassField&>(*member);
                    const CValueType field_type = lower_type(field.type);
                    if (field.is_weak
                        && (field_type.kind != CValueKind::Class
                            || !field_type.nullable)) {
                        throw_backend_error(
                            field.location,
                            "weak fields require a nullable class type");
                    }
                    info.field_indices.emplace(field.name, info.fields.size());
                    info.fields.push_back(ClassFieldInfo{&field, field_type});
                    continue;
                }

                const auto& method = static_cast<const MethodDeclaration&>(*member);
                if (!method.generic_parameters.empty()) {
                    throw_backend_error(
                        method.location,
                        "generic class methods are not supported by the C backend");
                }
                if (method.is_virtual || method.is_override || !method.body) {
                    throw_backend_error(
                        method.location,
                        "virtual, override, or bodyless class methods are not supported by the C backend");
                }
                MethodInfo method_info{&method, void_type, {}};
                if (method.return_type) {
                    method_info.return_type = lower_type(*method.return_type);
                }
                for (const auto& parameter : method.parameters) {
                    method_info.parameter_types.push_back(lower_type(parameter.type));
                }
                if (!info.methods.emplace(method.name, std::move(method_info)).second) {
                    throw_backend_error(
                        method.location,
                        "class method overloads are not supported by the C backend: '"
                            + declaration->name + "." + method.name + "'");
                }
            }
        }
    }

    void collect_functions(const Program& program)
    {
        for (const auto& statement : program.statements) {
            if (statement->kind != StmtKind::FunctionDeclaration) {
                continue;
            }
            const auto& function =
                static_cast<const FunctionDeclarationStmt&>(*statement);
            if (!function.generic_parameters.empty()) {
                throw_backend_error(
                    function.location,
                    "generic functions are not supported by the C backend");
            }
            FunctionInfo info{&function, void_type, {}};
            if (function.return_type) {
                info.return_type = lower_type(*function.return_type);
            }
            for (const auto& parameter : function.parameters) {
                info.parameter_types.push_back(lower_type(parameter.type));
            }
            if (!functions_.emplace(function.name, std::move(info)).second) {
                throw_backend_error(
                    function.location,
                    "function overloads are not supported by the C backend: '"
                        + function.name + "'");
            }
        }
    }

    CValueType lower_type(const TypeReference& type)
    {
        if (type.nullable) {
            if (type.arguments.empty() && classes_.contains(type.name)) {
                return CValueType{CValueKind::Class, type.name, {}, true};
            }
            throw_backend_error(
                type.location,
                "nullable type '" + type.name
                    + "?' is not supported by the C backend");
        }
        if (type.name == "Result") {
            if (type.arguments.size() != 2) {
                throw_backend_error(
                    type.location, "Result requires exactly two type arguments");
            }
            CValueType result{
                CValueKind::Result,
                {},
                {lower_type(type.arguments[0]), lower_type(type.arguments[1])},
            };
            if (contains_class_reference(result)) {
                throw_backend_error(
                    type.location,
                    "class-reference Result payloads are not supported by the C backend");
            }
            if (std::ranges::none_of(
                    result_order_,
                    [&](const ResultInfo& existing) {
                        return existing.type == result;
                    })) {
                result_order_.push_back(ResultInfo{result, type.location});
            }
            return result;
        }
        if (!type.arguments.empty()) {
            throw_backend_error(
                type.location,
                "type '" + type.name + "' is not supported by the C backend");
        }
        if (type.name == "int") {
            return int_type;
        }
        if (type.name == "dec") {
            return dec_type;
        }
        if (type.name == "bool") {
            return bool_type;
        }
        if (type.name == "string") {
            return string_type;
        }
        if (structs_.contains(type.name)) {
            return CValueType{CValueKind::Struct, type.name};
        }
        if (classes_.contains(type.name)) {
            return CValueType{CValueKind::Class, type.name};
        }
        if (enums_.contains(type.name)) {
            return CValueType{CValueKind::Enum, type.name};
        }
        throw_backend_error(
            type.location,
            "type '" + type.name + "' is not supported by the C backend");
    }

    void collect_local_types(const Program& program)
    {
        for (const auto& statement : program.statements) {
            collect_local_types(*statement);
        }
        for (const auto* declaration : struct_order_) {
            for (const auto& method : declaration->methods) {
                const auto& method_declaration =
                    static_cast<const MethodDeclaration&>(*method);
                if (method_declaration.body) {
                    collect_local_types(*method_declaration.body);
                }
            }
        }
        for (const auto* declaration : class_order_) {
            for (const auto& member : declaration->members) {
                if (member->kind != ClassMemberKind::Method) {
                    continue;
                }
                const auto& method = static_cast<const MethodDeclaration&>(*member);
                if (method.body) {
                    collect_local_types(*method.body);
                }
            }
        }
    }

    void collect_local_types(const Stmt& statement)
    {
        switch (statement.kind) {
        case StmtKind::VariableDeclaration: {
            const auto& declaration =
                static_cast<const VariableDeclarationStmt&>(statement);
            if (declaration.explicit_type) {
                static_cast<void>(lower_type(*declaration.explicit_type));
            }
            return;
        }
        case StmtKind::FunctionDeclaration:
            collect_local_types(
                *static_cast<const FunctionDeclarationStmt&>(statement).body);
            return;
        case StmtKind::Block:
            for (const auto& nested : static_cast<const BlockStmt&>(statement).statements) {
                collect_local_types(*nested);
            }
            return;
        case StmtKind::If: {
            const auto& conditional = static_cast<const IfStmt&>(statement);
            collect_local_types(*conditional.then_block);
            if (conditional.else_branch) {
                collect_local_types(*conditional.else_branch);
            }
            return;
        }
        case StmtKind::While:
            collect_local_types(*static_cast<const WhileStmt&>(statement).body);
            return;
        case StmtKind::ForIn:
            collect_local_types(*static_cast<const ForInStmt&>(statement).body);
            return;
        case StmtKind::Handle:
            for (const auto& handle_case :
                static_cast<const HandleStmt&>(statement).cases) {
                collect_local_types(*handle_case.body);
            }
            return;
        case StmtKind::Assignment:
        case StmtKind::MemberAssignment:
        case StmtKind::Expression:
        case StmtKind::Return:
        case StmtKind::Stop:
        case StmtKind::Continue:
        case StmtKind::EnumDeclaration:
        case StmtKind::StructDeclaration:
        case StmtKind::ClassDeclaration:
        case StmtKind::InterfaceDeclaration:
            return;
        }
    }

    std::string emit_type_definition(
        const CValueType& type,
        std::unordered_map<std::string, int>& state) const
    {
        const std::string& name = type.nominal_name;
        const std::string key = c_type_name(type);
        if (state[key] == 2) {
            return {};
        }
        if (state[key] == 1) {
            SourceLocation location{1, 1};
            if (type.kind == CValueKind::Struct) {
                location = structs_.at(name).declaration->location;
            } else if (type.kind == CValueKind::Class) {
                location = classes_.at(name).declaration->location;
            } else if (type.kind == CValueKind::Enum) {
                location = enums_.at(name).declaration->location;
            } else if (const auto result = std::ranges::find_if(
                           result_order_,
                           [&](const ResultInfo& info) {
                               return info.type == type;
                           });
                       result != result_order_.end()) {
                location = result->location;
            }
            throw_backend_error(
                location,
                "recursive by-value type layout is not supported by the C backend: '"
                    + key + "'");
        }
        state[key] = 1;
        std::string output;
        if (type.kind == CValueKind::Class) {
            const auto& info = classes_.at(name);
            for (const auto& field : info.fields) {
                if (field.type.kind == CValueKind::Struct
                    || field.type.kind == CValueKind::Enum
                    || field.type.kind == CValueKind::Result) {
                    output += emit_type_definition(field.type, state);
                }
            }
            const std::string object = class_name(name);
            output += "struct " + object + "\n{\n";
            output += "    uint64_t toro_strong_count;\n";
            output += "    toro_weak_control* toro_weak_control;\n";
            for (const auto& field : info.fields) {
                output += "    "
                    + std::string(field.declaration->is_weak
                            ? "toro_weak_ref"
                            : c_type_name(field.type))
                    + " "
                    + field_name(field.declaration->name) + ";\n";
            }
            output += "};\n\n";
            const auto destroy = info.methods.find("destroy");
            if (destroy != info.methods.end()) {
                output += "static void " + method_name(name, "destroy")
                    + "(" + object + "* toro_self);\n\n";
            }
            output += "static void " + retain_name(name) + "(" + object
                + "* toro_value)\n{\n";
            output += "    if (toro_value != NULL)\n    {\n";
            output += "        if (toro_value->toro_strong_count == 0) { abort(); }\n";
            output += "        ++toro_value->toro_strong_count;\n    }\n";
            output += "}\n\n";
            output += "static void " + release_name(name) + "(" + object
                + "* toro_value)\n{\n";
            output += "    if (toro_value != NULL && --toro_value->toro_strong_count == 0)\n";
            output += "    {\n";
            output += "        toro_weak_control* toro_control = toro_value->toro_weak_control;\n";
            output += "        toro_control->toro_object = NULL;\n";
            if (destroy != info.methods.end()) {
                output += "        " + method_name(name, "destroy")
                    + "(toro_value);\n";
            }
            for (const auto& field : info.fields) {
                const std::string access = "toro_value->"
                    + field_name(field.declaration->name);
                if (field.declaration->is_weak) {
                    output += "        toro_weak_clear(&" + access + ");\n";
                } else if (field.type.kind == CValueKind::Class) {
                    output += "        " + release_name(field.type.nominal_name)
                        + "(" + access + ");\n";
                }
            }
            output += "        if (--toro_control->toro_weak_count == 0) { free(toro_control); }\n";
            output += "        free(toro_value);\n    }\n";
            output += "}\n\n";
            state[key] = 2;
            return output;
        }
        if (type.kind == CValueKind::Result) {
            for (const auto& argument : type.arguments) {
                if (argument.kind == CValueKind::Struct
                    || argument.kind == CValueKind::Enum
                    || argument.kind == CValueKind::Result) {
                    output += emit_type_definition(argument, state);
                }
            }
            const std::string result = result_name(type);
            output += "typedef enum " + result + "_tag\n{\n";
            output += "    " + result + "_tag_ok,\n";
            output += "    " + result + "_tag_error\n";
            output += "} " + result + "_tag;\n\n";
            output += "struct " + result + "\n{\n";
            output += "    " + result + "_tag toro_tag;\n";
            output += "    union\n    {\n";
            output += "        " + c_type_name(type.arguments[0])
                + " toro_ok;\n";
            output += "        " + c_type_name(type.arguments[1])
                + " toro_error;\n";
            output += "    } toro_payload;\n";
            output += "};\n\n";
            state[key] = 2;
            return output;
        }
        if (type.kind == CValueKind::Enum) {
            const auto& info = enums_.at(name);
            for (const auto& variant : info.variants) {
                if (variant.payload_type
                    && (variant.payload_type->kind == CValueKind::Struct
                        || variant.payload_type->kind == CValueKind::Enum
                        || variant.payload_type->kind == CValueKind::Result)) {
                    output += emit_type_definition(*variant.payload_type, state);
                }
            }
            output += "typedef enum " + enum_tag_type_name(name) + "\n{\n";
            for (std::size_t index = 0; index < info.variants.size(); ++index) {
                output += "    " + enum_tag_name(
                    name, info.variants[index].declaration->name);
                if (index + 1 != info.variants.size()) {
                    output += ',';
                }
                output += '\n';
            }
            output += "} " + enum_tag_type_name(name) + ";\n\n";
            output += "struct " + enum_name(name) + "\n{\n";
            output += "    " + enum_tag_type_name(name) + " toro_tag;\n";
            const bool has_payload = std::ranges::any_of(
                info.variants,
                [](const EnumVariantInfo& variant) {
                    return variant.payload_type.has_value();
                });
            if (has_payload) {
                output += "    union\n    {\n";
                for (const auto& variant : info.variants) {
                    if (variant.payload_type) {
                        output += "        " + c_type_name(*variant.payload_type)
                            + " "
                            + enum_payload_name(variant.declaration->name)
                            + ";\n";
                    }
                }
                output += "    } toro_payload;\n";
            }
            output += "};\n\n";
            state[key] = 2;
            return output;
        }

        const auto& info = structs_.at(name);
        for (const auto& field : info.fields) {
            if (field.type.kind == CValueKind::Struct
                || field.type.kind == CValueKind::Enum
                || field.type.kind == CValueKind::Result) {
                output += emit_type_definition(field.type, state);
            }
        }
        output += "struct " + struct_name(name) + "\n{\n";
        if (info.fields.empty()) {
            output += "    uint8_t toro_empty;\n";
        } else {
            for (const auto& field : info.fields) {
                output += "    " + c_type_name(field.type) + " "
                    + field_name(field.declaration->name) + ";\n";
            }
        }
        output += "};\n\n";
        state[key] = 2;
        return output;
    }

    std::string method_declaration_text(
        const std::string& owner,
        CValueKind owner_kind,
        const MethodDeclaration& method,
        const MethodInfo& info) const
    {
        std::string output = "static " + c_type_name(info.return_type) + " "
            + method_name(owner, method.name) + "("
            + c_type_name(CValueType{owner_kind, owner})
            + (owner_kind == CValueKind::Struct ? "*" : "")
            + " toro_self";
        for (std::size_t index = 0; index < method.parameters.size(); ++index) {
            output += ", " + c_type_name(info.parameter_types[index]) + " "
                + parameter_name(method.parameters[index].name);
        }
        output += ")";
        return output;
    }

    std::string function_declaration(
        const FunctionDeclarationStmt& function) const
    {
        const auto& info = functions_.at(function.name);
        std::string output = "static ";
        output += c_type_name(info.return_type) + " " + function_name(function.name) + "(";
        if (function.parameters.empty()) {
            output += "void";
        } else {
            for (std::size_t index = 0; index < function.parameters.size(); ++index) {
                if (index != 0) {
                    output += ", ";
                }
                output += c_type_name(info.parameter_types[index]) + " "
                    + parameter_name(function.parameters[index].name);
            }
        }
        output += ")";
        return output;
    }

    std::string emit_function(const FunctionDeclarationStmt& function)
    {
        current_location_ = function.location;
        scopes_.clear();
        owned_class_values_.clear();
        push_scope();
        const auto& info = functions_.at(function.name);
        current_return_type_ = info.return_type;
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            const bool owns_parameter =
                info.parameter_types[index].kind == CValueKind::Class;
            scopes_.back().emplace(
                function.parameters[index].name,
                ValueInfo{
                    info.parameter_types[index],
                    parameter_name(function.parameters[index].name),
                    false,
                    owns_parameter,
                });
            if (owns_parameter) {
                owned_class_values_.back().push_back(ValueInfo{
                    info.parameter_types[index],
                    parameter_name(function.parameters[index].name),
                    false,
                    true,
                });
            }
        }

        std::string output = function_declaration(function) + "\n{\n";
        for (const auto& parameter : owned_class_values_.back()) {
            output += indent(1) + retain_name(parameter.type.nominal_name)
                + "(" + parameter.c_name + ");\n";
        }
        output += emit_statement_list(function.body->statements, 1);
        if (!statements_guarantee_return(function.body->statements)) {
            output += scope_cleanup(scopes_.size() - 1, 1);
        }
        output += "}\n";
        pop_scope();
        current_return_type_.reset();
        return output;
    }

    std::string emit_method(
        const std::string& owner,
        CValueKind owner_kind,
        const MethodDeclaration& method,
        const MethodInfo& info)
    {
        current_location_ = method.location;
        scopes_.clear();
        owned_class_values_.clear();
        push_scope();
        current_return_type_ = info.return_type;
        scopes_.back().emplace(
            "self",
            ValueInfo{
                CValueType{owner_kind, owner},
                "toro_self",
                owner_kind == CValueKind::Struct,
            });
        for (std::size_t index = 0; index < method.parameters.size(); ++index) {
            const bool owns_parameter =
                info.parameter_types[index].kind == CValueKind::Class;
            scopes_.back().emplace(
                method.parameters[index].name,
                ValueInfo{
                    info.parameter_types[index],
                    parameter_name(method.parameters[index].name),
                    false,
                    owns_parameter,
                });
            if (owns_parameter) {
                owned_class_values_.back().push_back(ValueInfo{
                    info.parameter_types[index],
                    parameter_name(method.parameters[index].name),
                    false,
                    true,
                });
            }
        }

        std::string output = method_declaration_text(
            owner, owner_kind, method, info) + "\n{\n";
        for (const auto& parameter : owned_class_values_.back()) {
            output += indent(1) + retain_name(parameter.type.nominal_name)
                + "(" + parameter.c_name + ");\n";
        }
        output += emit_statement_list(method.body->statements, 1);
        if (!statements_guarantee_return(method.body->statements)) {
            output += scope_cleanup(scopes_.size() - 1, 1);
        }
        output += "}\n";
        pop_scope();
        current_return_type_.reset();
        return output;
    }

    std::string emit_statement_list(
        const std::vector<std::unique_ptr<Stmt>>& statements,
        std::size_t depth)
    {
        std::string output;
        for (const auto& statement : statements) {
            output += emit_statement(*statement, depth);
        }
        return output;
    }

    std::string emit_statement(const Stmt& statement, std::size_t depth)
    {
        current_location_ = statement.location;
        const std::string prefix = indent(depth);
        switch (statement.kind) {
        case StmtKind::VariableDeclaration: {
            const auto& declaration =
                static_cast<const VariableDeclarationStmt&>(statement);
            const std::optional<CValueType> declared_type = declaration.explicit_type
                ? std::optional<CValueType>{lower_type(*declaration.explicit_type)}
                : std::nullopt;
            const auto initializer = emit_expression(
                *declaration.initializer, declared_type);
            const CValueType type = declared_type.value_or(initializer.type);
            const std::string c_name = variable_name(declaration.name);
            std::string output = indent_prelude(initializer.prelude, depth);
            output += prefix + c_type_name(type) + " " + c_name + " = "
                + initializer.code + ";\n";
            if (type.kind == CValueKind::Class) {
                if (!initializer.owned) {
                    output += prefix + retain_name(type.nominal_name)
                        + "(" + c_name + ");\n";
                }
                owned_class_values_.back().push_back(
                    ValueInfo{type, c_name, false, true});
            }
            scopes_.back().emplace(
                declaration.name,
                ValueInfo{type, c_name, false, type.kind == CValueKind::Class});
            return output;
        }
        case StmtKind::Assignment: {
            const auto& assignment = static_cast<const AssignmentStmt&>(statement);
            const auto& target = find_value(assignment.name);
            const auto value = emit_expression(*assignment.value, target.type);
            if (target.type.kind == CValueKind::Class) {
                if (!target.owned_local) {
                    throw_backend_error(
                        statement.location,
                        "cannot reassign borrowed class reference '"
                            + assignment.name + "' in the C backend");
                }
                const std::string temporary =
                    "toro_class_value_" + std::to_string(temporary_index_++);
                std::string output = indent_prelude(value.prelude, depth);
                output += prefix + c_type_name(target.type) + " " + temporary
                    + " = " + value.code + ";\n";
                if (!value.owned) {
                    output += prefix + retain_name(target.type.nominal_name)
                        + "(" + temporary + ");\n";
                }
                output += prefix + release_name(target.type.nominal_name)
                    + "(" + target.c_name + ");\n";
                output += prefix + target.c_name + " = " + temporary + ";\n";
                return output;
            }
            return indent_prelude(value.prelude, depth)
                + prefix + target.c_name + " = "
                + value.code + ";\n";
        }
        case StmtKind::MemberAssignment: {
            const auto& assignment =
                static_cast<const MemberAssignmentStmt&>(statement);
            if (const auto object = emit_expression(*assignment.target->object);
                object.type.kind == CValueKind::Class) {
                return emit_class_member_assignment(
                    assignment, std::move(object), depth);
            }
            const auto target = emit_member_access(*assignment.target);
            if (!target.addressable) {
                throw_backend_error(
                    statement.location,
                    "struct field assignment requires an addressable receiver");
            }
            const auto value = emit_expression(*assignment.value, target.type);
            return indent_prelude(target.prelude + value.prelude, depth)
                + prefix + target.code + " = " + value.code + ";\n";
        }
        case StmtKind::Expression: {
            const auto& expression = static_cast<const ExpressionStmt&>(statement);
            const auto value = emit_expression(*expression.expression);
            if (value.type.kind == CValueKind::Class && value.owned) {
                const std::string temporary =
                    "toro_unused_class_" + std::to_string(temporary_index_++);
                return indent_prelude(value.prelude, depth)
                    + prefix + c_type_name(value.type) + " " + temporary
                    + " = " + value.code + ";\n"
                    + prefix + release_name(value.type.nominal_name)
                    + "(" + temporary + ");\n";
            }
            return indent_prelude(value.prelude, depth)
                + prefix + value.code + ";\n";
        }
        case StmtKind::Return: {
            const auto& return_statement = static_cast<const ReturnStmt&>(statement);
            if (!return_statement.value) {
                return all_scope_cleanup(depth) + prefix + "return;\n";
            }
            const auto value = emit_expression(
                *return_statement.value, current_return_type_);
            if (value.type.kind == CValueKind::Class) {
                const std::string temporary =
                    "toro_return_class_" + std::to_string(temporary_index_++);
                std::string output = indent_prelude(value.prelude, depth);
                output += prefix + c_type_name(value.type) + " " + temporary
                    + " = " + value.code + ";\n";
                if (!value.owned) {
                    output += prefix + retain_name(value.type.nominal_name)
                        + "(" + temporary + ");\n";
                }
                output += all_scope_cleanup(depth);
                output += prefix + "return " + temporary + ";\n";
                return output;
            }
            const bool has_owned_references = std::ranges::any_of(
                owned_class_values_,
                [](const auto& values) { return !values.empty(); });
            if (!has_owned_references) {
                return indent_prelude(value.prelude, depth)
                    + prefix + "return " + value.code + ";\n";
            }
            const std::string temporary =
                "toro_return_value_" + std::to_string(temporary_index_++);
            return indent_prelude(value.prelude, depth)
                + prefix + c_type_name(value.type) + " " + temporary
                + " = " + value.code + ";\n"
                + all_scope_cleanup(depth)
                + prefix + "return " + temporary + ";\n";
        }
        case StmtKind::Block:
            return emit_block(static_cast<const BlockStmt&>(statement), depth);
        case StmtKind::If:
            return emit_if(static_cast<const IfStmt&>(statement), depth);
        case StmtKind::While: {
            const auto& loop = static_cast<const WhileStmt&>(statement);
            const auto condition = emit_expression(*loop.condition);
            if (condition.prelude.empty()) {
                std::string output = prefix + "while (" + condition.code + ") ";
                output += emit_braced_block(*loop.body, depth);
                return output;
            }
            push_scope();
            std::string output = prefix + "while (true)\n" + prefix + "{\n";
            output += indent_prelude(condition.prelude, depth + 1);
            output += indent(depth + 1) + "if (!(" + condition.code + "))\n";
            output += indent(depth + 1) + "{\n";
            output += indent(depth + 2) + "break;\n";
            output += indent(depth + 1) + "}\n";
            output += emit_statement_list(loop.body->statements, depth + 1);
            output += scope_cleanup(scopes_.size() - 1, depth + 1);
            output += prefix + "}\n";
            pop_scope();
            return output;
        }
        case StmtKind::FunctionDeclaration:
            throw_backend_error(
                statement.location,
                "nested functions are not supported by the C backend");
        case StmtKind::ForIn:
        case StmtKind::Stop:
        case StmtKind::Continue:
        case StmtKind::EnumDeclaration:
        case StmtKind::StructDeclaration:
        case StmtKind::ClassDeclaration:
        case StmtKind::InterfaceDeclaration:
            throw_backend_error(
                statement.location,
                "statement is not supported by the C backend");
        case StmtKind::Handle:
            return emit_handle(static_cast<const HandleStmt&>(statement), depth);
        }
        throw_backend_error(statement.location, "unknown statement");
    }

    std::string emit_block(const BlockStmt& block, std::size_t depth)
    {
        return indent(depth) + emit_braced_block(block, depth);
    }

    std::string emit_braced_block(const BlockStmt& block, std::size_t depth)
    {
        push_scope();
        std::string output = "{\n";
        output += emit_statement_list(block.statements, depth + 1);
        if (!statements_guarantee_return(block.statements)) {
            output += scope_cleanup(scopes_.size() - 1, depth + 1);
        }
        output += indent(depth) + "}\n";
        pop_scope();
        return output;
    }

    std::string emit_if(const IfStmt& conditional, std::size_t depth)
    {
        const auto condition = emit_expression(*conditional.condition);
        std::string output = indent_prelude(condition.prelude, depth);
        output += indent(depth) + "if (" + condition.code + ") ";
        output += emit_braced_block(*conditional.then_block, depth);
        if (conditional.else_branch) {
            output.resize(output.size() - 1);
            output += " else ";
            if (conditional.else_branch->kind == StmtKind::If) {
                output += "{\n";
                output += emit_if(
                    static_cast<const IfStmt&>(*conditional.else_branch),
                    depth + 1);
                output += indent(depth) + "}\n";
            } else {
                const auto& block =
                    static_cast<const BlockStmt&>(*conditional.else_branch);
                output += emit_braced_block(block, depth);
            }
        }
        return output;
    }

    std::string emit_handle(const HandleStmt& handle, std::size_t depth)
    {
        const auto handled = emit_expression(*handle.expression);
        if (handled.type.kind != CValueKind::Enum
            && handled.type.kind != CValueKind::Result) {
            throw_backend_error(
                handle.location,
                "handle lowering requires a supported enum or Result value");
        }
        const std::string temporary =
            "toro_handle_value_" + std::to_string(temporary_index_++);
        std::string output = indent_prelude(handled.prelude, depth);
        output += indent(depth) + "{\n";
        output += indent(depth + 1) + c_type_name(handled.type) + " "
            + temporary + " = " + handled.code + ";\n";
        output += indent(depth + 1) + "switch (" + temporary
            + ".toro_tag)\n" + indent(depth + 1) + "{\n";

        for (const auto& handle_case : handle.cases) {
            current_location_ = handle_case.location;
            std::optional<CValueType> payload_type;
            std::string tag;
            std::string payload;
            if (handled.type.kind == CValueKind::Enum) {
                const auto& info = enums_.at(handled.type.nominal_name);
                const auto variant =
                    info.variant_indices.find(handle_case.variant_name);
                if (variant == info.variant_indices.end()) {
                    throw_backend_error(
                        handle_case.location,
                        "enum '" + handled.type.nominal_name
                            + "' has no backend variant named '"
                            + handle_case.variant_name + "'");
                }
                payload_type = info.variants[variant->second].payload_type;
                tag = enum_tag_name(
                    handled.type.nominal_name, handle_case.variant_name);
                payload = enum_payload_name(handle_case.variant_name);
            } else {
                const bool is_ok = handle_case.variant_name == "ok";
                const bool is_error = handle_case.variant_name == "error";
                if (!is_ok && !is_error) {
                    throw_backend_error(
                        handle_case.location,
                        "Result has no backend variant named '"
                            + handle_case.variant_name + "'");
                }
                payload_type = handled.type.arguments[is_ok ? 0 : 1];
                tag = result_name(handled.type)
                    + (is_ok ? "_tag_ok" : "_tag_error");
                payload = is_ok ? "toro_ok" : "toro_error";
            }
            output += indent(depth + 1) + "case " + tag + ": {\n";
            push_scope();
            if (handle_case.binding_name && payload_type) {
                const std::string binding = variable_name(*handle_case.binding_name);
                scopes_.back().emplace(
                    *handle_case.binding_name,
                    ValueInfo{*payload_type, binding});
                output += indent(depth + 2) + c_type_name(*payload_type)
                    + " " + binding + " = " + temporary
                    + ".toro_payload." + payload + ";\n";
            }
            output += emit_statement_list(handle_case.body->statements, depth + 2);
            if (!statements_guarantee_return(handle_case.body->statements)) {
                output += scope_cleanup(scopes_.size() - 1, depth + 2);
            }
            output += indent(depth + 2) + "break;\n";
            pop_scope();
            output += indent(depth + 1) + "}\n";
        }
        output += indent(depth + 1) + "}\n";
        output += indent(depth) + "}\n";
        return output;
    }

    GeneratedExpression emit_expression(
        const Expr& expression,
        std::optional<CValueType> expected_type = std::nullopt)
    {
        switch (expression.kind) {
        case ExprKind::Integer:
            return {static_cast<const IntegerExpr&>(expression).value, int_type};
        case ExprKind::Decimal:
            return {static_cast<const DecimalExpr&>(expression).value, dec_type};
        case ExprKind::String:
            return {
                escape_c_string(static_cast<const StringExpr&>(expression).value),
                string_type,
            };
        case ExprKind::Bool:
            return {
                static_cast<const BoolExpr&>(expression).value ? "true" : "false",
                bool_type,
            };
        case ExprKind::Identifier: {
            const auto& identifier = static_cast<const IdentifierExpr&>(expression);
            const auto& value = find_value(identifier.name);
            return {value.c_name, value.type, true, value.pointer};
        }
        case ExprKind::Unary: {
            const auto& unary = static_cast<const UnaryExpr&>(expression);
            const auto operand = emit_expression(*unary.operand);
            return {
                "(-" + operand.code + ")",
                operand.type,
                false,
                false,
                operand.prelude,
            };
        }
        case ExprKind::Binary:
            return emit_binary(static_cast<const BinaryExpr&>(expression));
        case ExprKind::Call:
            return emit_call(
                static_cast<const CallExpr&>(expression), expected_type);
        case ExprKind::MemberAccess:
            return emit_member_access(
                static_cast<const MemberAccessExpr&>(expression));
        case ExprKind::TypeAccess:
            return emit_enum_variant_access(
                static_cast<const TypeAccessExpr&>(expression));
        case ExprKind::Grouping: {
            const auto inner = emit_expression(
                *static_cast<const GroupingExpr&>(expression).expression);
            return {
                "(" + inner.code + ")",
                inner.type,
                inner.addressable,
                inner.pointer,
                inner.prelude,
            };
        }
        case ExprKind::Null:
            if (expected_type && expected_type->kind == CValueKind::Class
                && expected_type->nullable) {
                return {"NULL", *expected_type};
            }
            throw_backend_error(
                current_location_,
                "null values are not supported by the C backend");
        case ExprKind::Propagation:
            return emit_propagation(
                static_cast<const PropagationExpr&>(expression));
        case ExprKind::Cast:
            throw_backend_error(
                current_location_,
                "expression is not supported by the C backend");
        }
        throw_backend_error(current_location_, "unknown expression");
    }

    GeneratedExpression emit_binary(const BinaryExpr& binary)
    {
        GeneratedExpression left{"", void_type};
        GeneratedExpression right{"", void_type};
        if (binary.left->kind == ExprKind::Null) {
            right = emit_expression(*binary.right);
            left = emit_expression(*binary.left, right.type);
        } else {
            left = emit_expression(*binary.left);
            right = emit_expression(*binary.right, left.type);
        }
        if (!right.prelude.empty()
            && (binary.operator_token.type == TokenType::And
                || binary.operator_token.type == TokenType::Or)) {
            const bool is_and = binary.operator_token.type == TokenType::And;
            const std::string temporary =
                "toro_logical_value_" + std::to_string(temporary_index_++);
            std::string prelude = left.prelude;
            prelude += "bool " + temporary + " = "
                + (is_and ? "false" : "true") + ";\n";
            prelude += "if (" + std::string(is_and ? "" : "!") + "("
                + left.code + ")) {\n";
            prelude += indent_prelude(right.prelude, 1);
            prelude += "    " + temporary + " = " + right.code + ";\n";
            prelude += "}\n";
            return {
                temporary,
                bool_type,
                false,
                false,
                std::move(prelude),
            };
        }
        if ((binary.operator_token.type == TokenType::Equal
                || binary.operator_token.type == TokenType::NotEqual)
            && left.type.kind == CValueKind::Class
            && right.type.kind == CValueKind::Class) {
            return {
                "(" + left.code
                    + (binary.operator_token.type == TokenType::Equal
                            ? " == "
                            : " != ")
                    + right.code + ")",
                bool_type,
                false,
                false,
                left.prelude + right.prelude,
            };
        }
        if (left.type.kind == CValueKind::Struct
            || left.type.kind == CValueKind::Class
            || left.type.kind == CValueKind::Enum
            || left.type.kind == CValueKind::Result
            || right.type.kind == CValueKind::Struct
            || right.type.kind == CValueKind::Class
            || right.type.kind == CValueKind::Enum
            || right.type.kind == CValueKind::Result) {
            throw_backend_error(
                current_location_,
                "operators on aggregate values are not supported by the C backend");
        }
        if ((binary.operator_token.type == TokenType::Equal
                || binary.operator_token.type == TokenType::NotEqual)
            && left.type == string_type) {
            const std::string comparison = binary.operator_token.type
                    == TokenType::Equal
                ? " == 0"
                : " != 0";
            return {
                "(strcmp(" + left.code + ", " + right.code + ")" + comparison + ")",
                bool_type,
                false,
                false,
                left.prelude + right.prelude,
            };
        }

        std::string operation;
        CValueType result_type = left.type;
        switch (binary.operator_token.type) {
        case TokenType::Plus: operation = "+"; break;
        case TokenType::Minus: operation = "-"; break;
        case TokenType::Star: operation = "*"; break;
        case TokenType::Slash: operation = "/"; break;
        case TokenType::Equal: operation = "=="; result_type = bool_type; break;
        case TokenType::NotEqual: operation = "!="; result_type = bool_type; break;
        case TokenType::Less: operation = "<"; result_type = bool_type; break;
        case TokenType::LessEqual: operation = "<="; result_type = bool_type; break;
        case TokenType::Greater: operation = ">"; result_type = bool_type; break;
        case TokenType::GreaterEqual: operation = ">="; result_type = bool_type; break;
        case TokenType::And: operation = "&&"; result_type = bool_type; break;
        case TokenType::Or: operation = "||"; result_type = bool_type; break;
        default:
            throw_backend_error(
                current_location_, "operator '" + binary.operator_token.lexeme
                    + "' is not supported by the C backend");
        }
        return {
            "(" + left.code + " " + operation + " " + right.code + ")",
            result_type,
            false,
            false,
            left.prelude + right.prelude,
        };
    }

    GeneratedExpression emit_call(
        const CallExpr& call,
        std::optional<CValueType> expected_type)
    {
        if (call.callee->kind == ExprKind::Identifier) {
            const auto& callee = static_cast<const IdentifierExpr&>(*call.callee);
            if (callee.name == "ok" || callee.name == "error") {
                return emit_result_construction(callee.name, call, expected_type);
            }
        }
        if (!call.generic_arguments.empty()) {
            throw_backend_error(
                current_location_,
                "generic calls are not supported by the C backend");
        }
        if (call.callee->kind == ExprKind::TypeAccess) {
            const auto& access =
                static_cast<const TypeAccessExpr&>(*call.callee);
            return emit_enum_variant_construction(
                access.type_name, access.member, call);
        }
        if (call.callee->kind == ExprKind::MemberAccess) {
            const auto& member =
                static_cast<const MemberAccessExpr&>(*call.callee);
            return emit_method_call(
                call, member);
        }
        if (call.callee->kind != ExprKind::Identifier) {
            throw_backend_error(current_location_, "unsupported call target");
        }
        const auto& callee = static_cast<const IdentifierExpr&>(*call.callee);
        if (callee.name == "print") {
            return emit_print(call);
        }
        if (structs_.contains(callee.name)) {
            return emit_struct_construction(callee.name, call);
        }
        if (classes_.contains(callee.name)) {
            return emit_class_construction(callee.name, call);
        }
        const auto function = functions_.find(callee.name);
        if (function == functions_.end()) {
            throw_backend_error(
                current_location_, "unknown backend function '" + callee.name + "'");
        }

        const auto ordered = order_arguments(
            call, function->second.declaration->parameters);

        std::string code = function_name(callee.name) + "(";
        std::string prelude;
        std::vector<ValueInfo> owned_arguments;
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            if (index != 0) {
                code += ", ";
            }
            const auto argument = emit_expression(
                *ordered[index]->value, function->second.parameter_types[index]);
            prelude += argument.prelude;
            if (argument.type.kind == CValueKind::Class && argument.owned) {
                const std::string temporary =
                    "toro_argument_class_" + std::to_string(temporary_index_++);
                prelude += c_type_name(argument.type) + " " + temporary
                    + " = " + argument.code + ";\n";
                owned_arguments.push_back(
                    ValueInfo{argument.type, temporary, false, true});
                code += temporary;
            } else {
                code += argument.code;
            }
        }
        code += ")";
        return complete_call(
            std::move(code),
            function->second.return_type,
            std::move(prelude),
            std::move(owned_arguments));
    }

    GeneratedExpression emit_result_construction(
        const std::string& constructor,
        const CallExpr& call,
        const std::optional<CValueType>& expected_type)
    {
        if (!expected_type || expected_type->kind != CValueKind::Result) {
            throw_backend_error(
                current_location_,
                "cannot lower '" + constructor
                    + "' without an expected Result<T, E> type");
        }
        if (call.arguments.size() != 1) {
            throw_backend_error(
                current_location_,
                "Result constructor '" + constructor + "' requires one payload");
        }
        const bool is_ok = constructor == "ok";
        const CValueType payload_type = expected_type->arguments[is_ok ? 0 : 1];
        const auto payload = emit_expression(
            *call.arguments.front().value, payload_type);
        const std::string result = result_name(*expected_type);
        return {
            "(" + result + "){.toro_tag = " + result
                + (is_ok ? "_tag_ok, .toro_payload.toro_ok = "
                         : "_tag_error, .toro_payload.toro_error = ")
                + payload.code + "}",
            *expected_type,
            false,
            false,
            payload.prelude,
        };
    }

    GeneratedExpression emit_propagation(const PropagationExpr& propagation)
    {
        const auto value = emit_expression(*propagation.expression);
        if (value.type.kind != CValueKind::Result) {
            throw_backend_error(
                current_location_, "operator '?' requires a Result value");
        }
        if (!current_return_type_
            || current_return_type_->kind != CValueKind::Result) {
            throw_backend_error(
                current_location_,
                "operator '?' requires a Result-returning function");
        }
        const std::string temporary =
            "toro_result_value_" + std::to_string(temporary_index_++);
        const std::string source_result = result_name(value.type);
        const std::string target_result = result_name(*current_return_type_);
        std::string prelude = value.prelude;
        prelude += c_type_name(value.type) + " " + temporary + " = "
            + value.code + ";\n";
        prelude += "if (" + temporary + ".toro_tag == " + source_result
            + "_tag_error) {\n";
        prelude += "    return (" + target_result + "){.toro_tag = "
            + target_result + "_tag_error, .toro_payload.toro_error = "
            + temporary + ".toro_payload.toro_error};\n";
        prelude += "}\n";
        return {
            temporary + ".toro_payload.toro_ok",
            value.type.arguments[0],
            true,
            false,
            std::move(prelude),
        };
    }

    GeneratedExpression emit_enum_variant_construction(
        const std::string& enum_type,
        const std::string& variant_name,
        const CallExpr& call)
    {
        const auto& info = enums_.at(enum_type);
        const auto variant = info.variant_indices.find(variant_name);
        if (variant == info.variant_indices.end()) {
            throw_backend_error(
                current_location_,
                "enum '" + enum_type + "' has no variant named '"
                    + variant_name + "'");
        }
        const auto& variant_info = info.variants[variant->second];
        if (!variant_info.payload_type) {
            if (!call.arguments.empty()) {
                throw_backend_error(
                    current_location_,
                    "payload-free enum variant received a payload");
            }
            return {
                "(" + enum_name(enum_type) + "){.toro_tag = "
                    + enum_tag_name(enum_type, variant_name) + "}",
                CValueType{CValueKind::Enum, enum_type},
            };
        }
        if (call.arguments.size() != 1) {
            throw_backend_error(
                current_location_,
                "enum variant '" + enum_type + "::" + variant_name
                    + "' has an unsupported payload layout");
        }
        std::string code = "(" + enum_name(enum_type) + "){.toro_tag = "
            + enum_tag_name(enum_type, variant_name) + ", .toro_payload."
            + enum_payload_name(variant_name) + " = ";
        const auto payload = emit_expression(
            *call.arguments.front().value, *variant_info.payload_type);
        code += payload.code + "}";
        return {
            std::move(code),
            CValueType{CValueKind::Enum, enum_type},
            false,
            false,
            payload.prelude,
        };
    }

    std::vector<const CallArgument*> order_arguments(
        const CallExpr& call,
        const std::vector<Parameter>& parameters) const
    {
        std::vector<const CallArgument*> ordered(parameters.size(), nullptr);
        std::size_t positional_index = 0;
        for (const auto& argument : call.arguments) {
            std::size_t parameter_index = positional_index;
            if (argument.name) {
                const auto parameter = std::find_if(
                    parameters.begin(),
                    parameters.end(),
                    [&](const Parameter& candidate) {
                        return candidate.name == *argument.name;
                    });
                if (parameter == parameters.end()) {
                    throw_backend_error(
                        current_location_,
                        "unknown named argument '" + *argument.name + "'");
                }
                parameter_index = static_cast<std::size_t>(
                    parameter - parameters.begin());
            } else {
                while (positional_index < ordered.size()
                    && ordered[positional_index] != nullptr) {
                    ++positional_index;
                }
                parameter_index = positional_index++;
            }
            if (parameter_index >= ordered.size()
                || ordered[parameter_index] != nullptr) {
                throw_backend_error(current_location_, "invalid call argument layout");
            }
            ordered[parameter_index] = &argument;
        }
        if (std::ranges::find(ordered, nullptr) != ordered.end()) {
            throw_backend_error(current_location_, "missing call argument");
        }
        return ordered;
    }

    GeneratedExpression emit_struct_construction(
        const std::string& name,
        const CallExpr& call)
    {
        const auto& info = structs_.at(name);
        std::vector<const CallArgument*> arguments(info.fields.size(), nullptr);
        std::size_t positional_index = 0;
        for (const auto& argument : call.arguments) {
            std::size_t field_index = positional_index;
            if (argument.name) {
                const auto found = info.field_indices.find(*argument.name);
                if (found == info.field_indices.end()) {
                    throw_backend_error(
                        current_location_,
                        "struct '" + name + "' has no field named '"
                            + *argument.name + "'");
                }
                field_index = found->second;
            } else {
                while (positional_index < arguments.size()
                    && arguments[positional_index] != nullptr) {
                    ++positional_index;
                }
                field_index = positional_index++;
            }
            if (field_index >= arguments.size() || arguments[field_index] != nullptr) {
                throw_backend_error(
                    current_location_, "invalid struct construction argument layout");
            }
            arguments[field_index] = &argument;
        }

        std::string prelude;
        std::string code = "(" + struct_name(name) + "){";
        if (info.fields.empty()) {
            code += ".toro_empty = 0";
        }
        for (std::size_t index = 0; index < info.fields.size(); ++index) {
            if (index != 0) {
                code += ", ";
            }
            const auto& field = info.fields[index];
            code += "." + field_name(field.declaration->name) + " = ";
            if (arguments[index]) {
                const auto value = emit_expression(
                    *arguments[index]->value, field.type);
                prelude += value.prelude;
                code += value.code;
            } else if (field.declaration->default_value) {
                const auto value = emit_expression(
                    *field.declaration->default_value, field.type);
                prelude += value.prelude;
                code += value.code;
            } else {
                code += zero_value(field.type, field.declaration->location);
            }
        }
        code += "}";
        return {
            std::move(code),
            CValueType{CValueKind::Struct, name},
            false,
            false,
            std::move(prelude),
        };
    }

    GeneratedExpression emit_class_construction(
        const std::string& name,
        const CallExpr& call)
    {
        const auto& info = classes_.at(name);
        if (const auto initializer = info.methods.find("init");
            initializer != info.methods.end()) {
            return emit_initialized_class_construction(
                name, call, info, initializer->second);
        }
        std::vector<const CallArgument*> arguments(info.fields.size(), nullptr);
        std::size_t positional_index = 0;
        for (const auto& argument : call.arguments) {
            std::size_t field_index = positional_index;
            if (argument.name) {
                const auto found = info.field_indices.find(*argument.name);
                if (found == info.field_indices.end()) {
                    throw_backend_error(
                        current_location_,
                        "class '" + name + "' has no field named '"
                            + *argument.name + "'");
                }
                field_index = found->second;
            } else {
                while (positional_index < arguments.size()
                    && arguments[positional_index] != nullptr) {
                    ++positional_index;
                }
                field_index = positional_index++;
            }
            if (field_index >= arguments.size() || arguments[field_index] != nullptr) {
                throw_backend_error(
                    current_location_, "invalid class construction argument layout");
            }
            arguments[field_index] = &argument;
        }

        const CValueType type{CValueKind::Class, name};
        const std::string temporary =
            "toro_new_class_" + std::to_string(temporary_index_++);
        std::string prelude = c_type_name(type) + " " + temporary
            + " = malloc(sizeof(*" + temporary + "));\n";
        prelude += "if (" + temporary + " == NULL) { abort(); }\n";
        prelude += temporary + "->toro_strong_count = 1;\n";
        prelude += temporary + "->toro_weak_control = malloc(sizeof(*"
            + temporary + "->toro_weak_control));\n";
        prelude += "if (" + temporary + "->toro_weak_control == NULL) { abort(); }\n";
        prelude += temporary + "->toro_weak_control->toro_object = "
            + temporary + ";\n";
        prelude += temporary + "->toro_weak_control->toro_weak_count = 1;\n";
        for (std::size_t index = 0; index < info.fields.size(); ++index) {
            const auto& field = info.fields[index];
            GeneratedExpression value{"", field.type};
            if (arguments[index]) {
                value = emit_expression(*arguments[index]->value, field.type);
            } else if (field.declaration->default_value) {
                value = emit_expression(*field.declaration->default_value, field.type);
            } else {
                value = GeneratedExpression{
                    zero_value(field.type, field.declaration->location), field.type};
            }
            prelude += value.prelude;
            const std::string access = temporary + "->"
                + field_name(field.declaration->name);
            if (field.declaration->is_weak) {
                const std::string weak_value =
                    "toro_weak_value_" + std::to_string(temporary_index_++);
                prelude += c_type_name(field.type) + " " + weak_value
                    + " = " + value.code + ";\n";
                prelude += access + ".toro_control = NULL;\n";
                prelude += "toro_weak_set(&" + access + ", " + weak_value
                    + " == NULL ? NULL : " + weak_value
                    + "->toro_weak_control);\n";
                if (value.owned) {
                    prelude += release_name(value.type.nominal_name)
                        + "(" + weak_value + ");\n";
                }
            } else {
                prelude += access + " = " + value.code + ";\n";
                if (field.type.kind == CValueKind::Class && !value.owned) {
                    prelude += retain_name(field.type.nominal_name)
                        + "(" + access + ");\n";
                }
            }
        }
        return {temporary, type, true, false, std::move(prelude), true};
    }

    GeneratedExpression emit_initialized_class_construction(
        const std::string& name,
        const CallExpr& call,
        const ClassInfo& info,
        const MethodInfo& initializer)
    {
        const CValueType type{CValueKind::Class, name};
        const std::string temporary =
            "toro_new_class_" + std::to_string(temporary_index_++);
        std::string prelude = c_type_name(type) + " " + temporary
            + " = malloc(sizeof(*" + temporary + "));\n";
        prelude += "if (" + temporary + " == NULL) { abort(); }\n";
        prelude += temporary + "->toro_strong_count = 1;\n";
        prelude += temporary + "->toro_weak_control = malloc(sizeof(*"
            + temporary + "->toro_weak_control));\n";
        prelude += "if (" + temporary + "->toro_weak_control == NULL) { abort(); }\n";
        prelude += temporary + "->toro_weak_control->toro_object = "
            + temporary + ";\n";
        prelude += temporary + "->toro_weak_control->toro_weak_count = 1;\n";

        for (const auto& field : info.fields) {
            const std::string access = temporary + "->"
                + field_name(field.declaration->name);
            if (field.declaration->is_weak) {
                prelude += access + ".toro_control = NULL;\n";
            }
            if (!field.declaration->default_value) {
                if (field.declaration->is_weak) {
                    continue;
                }
                if (field.type.kind == CValueKind::Class) {
                    prelude += access + " = NULL;\n";
                } else {
                    prelude += access + " = "
                        + (field.type.kind == CValueKind::Int
                                || field.type.kind == CValueKind::Dec
                                || field.type.kind == CValueKind::Bool
                                || field.type.kind == CValueKind::String
                            ? zero_value(field.type, field.declaration->location)
                            : "(" + c_type_name(field.type) + "){0}")
                        + ";\n";
                }
                continue;
            }

            const auto value = emit_expression(
                *field.declaration->default_value, field.type);
            prelude += value.prelude;
            if (field.declaration->is_weak) {
                const std::string weak_value =
                    "toro_weak_value_" + std::to_string(temporary_index_++);
                prelude += c_type_name(field.type) + " " + weak_value
                    + " = " + value.code + ";\n";
                prelude += "toro_weak_set(&" + access + ", " + weak_value
                    + " == NULL ? NULL : " + weak_value
                    + "->toro_weak_control);\n";
                if (value.owned) {
                    prelude += release_name(value.type.nominal_name)
                        + "(" + weak_value + ");\n";
                }
            } else {
                prelude += access + " = " + value.code + ";\n";
                if (field.type.kind == CValueKind::Class && !value.owned) {
                    prelude += retain_name(field.type.nominal_name)
                        + "(" + access + ");\n";
                }
            }
        }

        const auto ordered = order_arguments(call, initializer.declaration->parameters);
        std::string invocation = method_name(name, "init") + "(" + temporary;
        std::vector<ValueInfo> owned_arguments;
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            const auto argument = emit_expression(
                *ordered[index]->value, initializer.parameter_types[index]);
            prelude += argument.prelude;
            invocation += ", ";
            if (argument.type.kind == CValueKind::Class && argument.owned) {
                const std::string argument_temporary =
                    "toro_argument_class_" + std::to_string(temporary_index_++);
                prelude += c_type_name(argument.type) + " " + argument_temporary
                    + " = " + argument.code + ";\n";
                owned_arguments.push_back(
                    ValueInfo{argument.type, argument_temporary, false, true});
                invocation += argument_temporary;
            } else {
                invocation += argument.code;
            }
        }
        invocation += ");\n";
        prelude += invocation;
        for (auto argument = owned_arguments.rbegin();
             argument != owned_arguments.rend(); ++argument) {
            prelude += release_name(argument->type.nominal_name)
                + "(" + argument->c_name + ");\n";
        }
        return {temporary, type, true, false, std::move(prelude), true};
    }

    std::string zero_value(CValueType type, SourceLocation location) const
    {
        switch (type.kind) {
        case CValueKind::Int: return "0";
        case CValueKind::Dec: return "0.0";
        case CValueKind::Bool: return "false";
        case CValueKind::String: return "\"\"";
        case CValueKind::Class:
            if (type.nullable) {
                return "NULL";
            }
            throw_backend_error(
                location, "class-reference field has no backend zero value");
        case CValueKind::Struct:
            throw_backend_error(
                location,
                "omitted struct field '" + type.nominal_name
                    + "' has no backend zero value");
        case CValueKind::Enum:
            throw_backend_error(
                location,
                "omitted enum field '" + type.nominal_name
                    + "' has no backend zero value");
        case CValueKind::Result:
            throw_backend_error(
                location, "omitted Result field has no backend zero value");
        case CValueKind::Void:
            break;
        }
        throw_backend_error(location, "field has no backend zero value");
    }

    GeneratedExpression emit_member_access(const MemberAccessExpr& member)
    {
        const auto object = emit_expression(*member.object);
        if (object.type.kind != CValueKind::Struct
            && object.type.kind != CValueKind::Class) {
            throw_backend_error(
                current_location_, "field access requires a struct or class value");
        }
        if (object.type.kind == CValueKind::Class) {
            if (object.owned) {
                throw_backend_error(
                    current_location_,
                    "field access through a temporary class reference is not supported by the C backend");
            }
            const auto& info = classes_.at(object.type.nominal_name);
            const auto field = info.field_indices.find(member.member);
            if (field == info.field_indices.end()) {
                throw_backend_error(
                    current_location_,
                    "class '" + object.type.nominal_name + "' has no field named '"
                        + member.member + "'");
            }
            const auto& field_info = info.fields[field->second];
            const std::string access = "(" + object.code + ")->"
                + field_name(member.member);
            if (field_info.declaration->is_weak) {
                return {
                    "((" + c_type_name(field_info.type)
                        + ")toro_weak_load(&(" + access + ")))",
                    field_info.type,
                    false,
                    false,
                    object.prelude,
                };
            }
            return {
                access,
                field_info.type,
                object.addressable,
                false,
                object.prelude,
            };
        }
        const auto& info = structs_.at(object.type.nominal_name);
        const auto field = info.field_indices.find(member.member);
        if (field == info.field_indices.end()) {
            throw_backend_error(
                current_location_,
                "struct '" + object.type.nominal_name + "' has no field named '"
                    + member.member + "'");
        }
        return {
            "(" + object.code + ")" + (object.pointer ? "->" : ".")
                + field_name(member.member),
            info.fields[field->second].type,
            object.addressable,
            false,
            object.prelude,
        };
    }

    std::string emit_class_member_assignment(
        const MemberAssignmentStmt& assignment,
        GeneratedExpression object,
        std::size_t depth)
    {
        if (object.owned) {
            throw_backend_error(
                assignment.location,
                "field assignment through a temporary class reference is not supported by the C backend");
        }
        const auto& info = classes_.at(object.type.nominal_name);
        const auto field = info.field_indices.find(assignment.target->member);
        if (field == info.field_indices.end()) {
            throw_backend_error(
                assignment.location,
                "class '" + object.type.nominal_name + "' has no field named '"
                    + assignment.target->member + "'");
        }
        const auto& field_info = info.fields[field->second];
        const auto value = emit_expression(*assignment.value, field_info.type);
        const std::string prefix = indent(depth);
        const std::string access = "(" + object.code + ")->"
            + field_name(assignment.target->member);
        std::string output = indent_prelude(object.prelude + value.prelude, depth);
        if (field_info.declaration->is_weak) {
            const std::string temporary =
                "toro_weak_value_" + std::to_string(temporary_index_++);
            output += prefix + c_type_name(field_info.type) + " " + temporary
                + " = " + value.code + ";\n";
            output += prefix + "toro_weak_set(&" + access + ", " + temporary
                + " == NULL ? NULL : " + temporary + "->toro_weak_control);\n";
            if (value.owned) {
                output += prefix + release_name(value.type.nominal_name)
                    + "(" + temporary + ");\n";
            }
            return output;
        }
        if (field_info.type.kind == CValueKind::Class) {
            const std::string temporary =
                "toro_field_class_" + std::to_string(temporary_index_++);
            output += prefix + c_type_name(field_info.type) + " " + temporary
                + " = " + value.code + ";\n";
            if (!value.owned) {
                output += prefix + retain_name(field_info.type.nominal_name)
                    + "(" + temporary + ");\n";
            }
            output += prefix + release_name(field_info.type.nominal_name)
                + "(" + access + ");\n";
            output += prefix + access + " = " + temporary + ";\n";
            return output;
        }
        output += prefix + access + " = " + value.code + ";\n";
        return output;
    }

    GeneratedExpression emit_enum_variant_access(const TypeAccessExpr& access)
    {
        const auto found_enum = enums_.find(access.type_name);
        if (found_enum == enums_.end()) {
            throw_backend_error(
                current_location_,
                "'::' type-scoped access currently supports enum variants only");
        }
        const auto variant =
            found_enum->second.variant_indices.find(access.member);
        if (variant == found_enum->second.variant_indices.end()) {
            throw_backend_error(
                current_location_,
                "enum '" + access.type_name + "' has no variant named '"
                    + access.member + "'");
        }
        const auto& variant_info = found_enum->second.variants[variant->second];
        if (variant_info.payload_type) {
            throw_backend_error(
                current_location_,
                "enum variant '" + access.type_name + "::" + access.member
                    + "' requires a payload");
        }
        return {
            "(" + enum_name(access.type_name) + "){.toro_tag = "
                + enum_tag_name(access.type_name, access.member) + "}",
            CValueType{CValueKind::Enum, access.type_name},
        };
    }

    GeneratedExpression emit_method_call(
        const CallExpr& call,
        const MemberAccessExpr& member)
    {
        const auto receiver = emit_expression(*member.object);
        if (member.member == "destroy") {
            throw_backend_error(
                current_location_,
                "destroy() cannot be invoked directly");
        }
        if (receiver.type.kind != CValueKind::Struct
            && receiver.type.kind != CValueKind::Class) {
            throw_backend_error(
                current_location_, "method calls require a struct or class receiver");
        }
        if (receiver.type.kind == CValueKind::Class && member.member == "init") {
            throw_backend_error(
                current_location_,
                "init cannot be invoked directly");
        }
        const auto* methods = receiver.type.kind == CValueKind::Struct
            ? &structs_.at(receiver.type.nominal_name).methods
            : &classes_.at(receiver.type.nominal_name).methods;
        const auto method = methods->find(member.member);
        if (method == methods->end()) {
            throw_backend_error(
                current_location_,
                "type '" + receiver.type.nominal_name + "' has no method named '"
                    + member.member + "'");
        }
        if (receiver.type.kind == CValueKind::Struct
            && !receiver.pointer && !receiver.addressable) {
            throw_backend_error(
                current_location_,
                "struct method receiver must be an addressable value");
        }
        const auto ordered = order_arguments(
            call, method->second.declaration->parameters);
        std::string code = method_name(receiver.type.nominal_name, member.member) + "(";
        std::string prelude = receiver.prelude;
        std::vector<ValueInfo> owned_arguments;
        if (receiver.type.kind == CValueKind::Class && receiver.owned) {
            const std::string temporary =
                "toro_receiver_class_" + std::to_string(temporary_index_++);
            prelude += c_type_name(receiver.type) + " " + temporary
                + " = " + receiver.code + ";\n";
            owned_arguments.push_back(
                ValueInfo{receiver.type, temporary, false, true});
            code += temporary;
        } else {
            code += receiver.type.kind == CValueKind::Class || receiver.pointer
                ? receiver.code
                : "&(" + receiver.code + ")";
        }
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            const auto argument = emit_expression(
                *ordered[index]->value, method->second.parameter_types[index]);
            prelude += argument.prelude;
            code += ", ";
            if (argument.type.kind == CValueKind::Class && argument.owned) {
                const std::string temporary =
                    "toro_argument_class_" + std::to_string(temporary_index_++);
                prelude += c_type_name(argument.type) + " " + temporary
                    + " = " + argument.code + ";\n";
                owned_arguments.push_back(
                    ValueInfo{argument.type, temporary, false, true});
                code += temporary;
            } else {
                code += argument.code;
            }
        }
        code += ")";
        return complete_call(
            std::move(code),
            method->second.return_type,
            std::move(prelude),
            std::move(owned_arguments));
    }

    GeneratedExpression complete_call(
        std::string code,
        const CValueType& result_type,
        std::string prelude,
        std::vector<ValueInfo> owned_arguments)
    {
        if (owned_arguments.empty()) {
            return {
                std::move(code),
                result_type,
                false,
                false,
                std::move(prelude),
                result_type.kind == CValueKind::Class,
            };
        }

        std::string result_code = "(void)0";
        if (result_type.kind == CValueKind::Void) {
            prelude += code + ";\n";
        } else {
            result_code = "toro_call_value_" + std::to_string(temporary_index_++);
            prelude += c_type_name(result_type) + " " + result_code
                + " = " + code + ";\n";
        }
        for (auto argument = owned_arguments.rbegin();
             argument != owned_arguments.rend(); ++argument) {
            prelude += release_name(argument->type.nominal_name)
                + "(" + argument->c_name + ");\n";
        }
        return {
            std::move(result_code),
            result_type,
            false,
            false,
            std::move(prelude),
            result_type.kind == CValueKind::Class,
        };
    }

    GeneratedExpression emit_print(const CallExpr& call)
    {
        const auto value = emit_expression(*call.arguments.front().value);
        switch (value.type.kind) {
        case CValueKind::Int:
            return {
                "printf(\"%lld\\n\", (long long)(" + value.code + "))",
                void_type,
                false,
                false,
                value.prelude,
            };
        case CValueKind::Dec:
            return {
                "printf(\"%g\\n\", " + value.code + ")",
                void_type,
                false,
                false,
                value.prelude,
            };
        case CValueKind::Bool:
            return {
                "printf(\"%s\\n\", (" + value.code
                    + ") ? \"true\" : \"false\")",
                void_type,
                false,
                false,
                value.prelude,
            };
        case CValueKind::String:
            return {
                "printf(\"%s\\n\", " + value.code + ")",
                void_type,
                false,
                false,
                value.prelude,
            };
        case CValueKind::Struct:
            throw_backend_error(
                current_location_, "printing struct values is not supported by the C backend");
        case CValueKind::Class:
            throw_backend_error(
                current_location_, "printing class references is not supported by the C backend");
        case CValueKind::Enum:
            throw_backend_error(
                current_location_, "printing enum values is not supported by the C backend");
        case CValueKind::Result:
            throw_backend_error(
                current_location_, "printing Result values is not supported by the C backend");
        case CValueKind::Void:
            throw_backend_error(
                current_location_, "cannot print an expression without a value");
        }
        throw_backend_error(current_location_, "unsupported print value");
    }

    void push_scope()
    {
        scopes_.emplace_back();
        owned_class_values_.emplace_back();
    }

    void pop_scope()
    {
        scopes_.pop_back();
        owned_class_values_.pop_back();
    }

    std::string scope_cleanup(std::size_t scope_index, std::size_t depth) const
    {
        std::string output;
        const auto& values = owned_class_values_.at(scope_index);
        for (auto value = values.rbegin(); value != values.rend(); ++value) {
            output += indent(depth) + release_name(value->type.nominal_name)
                + "(" + value->c_name + ");\n";
        }
        return output;
    }

    std::string all_scope_cleanup(std::size_t depth) const
    {
        std::string output;
        for (std::size_t index = owned_class_values_.size(); index > 0; --index) {
            output += scope_cleanup(index - 1, depth);
        }
        return output;
    }

    const ValueInfo& find_value(const std::string& name) const
    {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            if (const auto value = scope->find(name); value != scope->end()) {
                return value->second;
            }
        }
        throw_backend_error(
            current_location_, "cannot resolve value '" + name + "' in C backend");
    }

    std::unordered_map<std::string, StructInfo> structs_;
    std::vector<const StructDeclarationStmt*> struct_order_;
    std::unordered_map<std::string, ClassInfo> classes_;
    std::vector<const ClassDeclarationStmt*> class_order_;
    std::unordered_map<std::string, EnumInfo> enums_;
    std::vector<const EnumDeclarationStmt*> enum_order_;
    std::vector<CValueType> nominal_order_;
    std::vector<ResultInfo> result_order_;
    std::unordered_map<std::string, FunctionInfo> functions_;
    std::vector<std::unordered_map<std::string, ValueInfo>> scopes_;
    std::vector<std::vector<ValueInfo>> owned_class_values_;
    std::size_t temporary_index_{0};
    std::optional<CValueType> current_return_type_;
    SourceLocation current_location_{1, 1};
};

} // namespace

std::string CGenerator::generate(const Program& program)
{
    return Generator{}.generate(program);
}

} // namespace toro
