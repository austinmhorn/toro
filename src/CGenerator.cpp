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

enum class CValueKind { Int, Dec, Bool, String, Struct, Enum, Void };

struct CValueType {
    CValueKind kind;
    std::string nominal_name;

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

struct EnumVariantInfo {
    const EnumVariant* declaration;
    std::optional<CValueType> payload_type;
};

struct EnumInfo {
    const EnumDeclarationStmt* declaration;
    std::vector<EnumVariantInfo> variants;
    std::unordered_map<std::string, std::size_t> variant_indices;
};

struct GeneratedExpression {
    std::string code;
    CValueType type;
    bool addressable{false};
    bool pointer{false};
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

std::string function_name(std::string_view name)
{
    return "toro_fn_" + std::string(name);
}

std::string struct_name(std::string_view name)
{
    return "toro_struct_" + std::string(name);
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
    case CValueKind::Enum: return enum_name(type.nominal_name);
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

        std::string output =
            "#include <stdbool.h>\n"
            "#include <stdint.h>\n"
            "#include <stdio.h>\n"
            "#include <string.h>\n\n";

        for (const auto& struct_declaration : struct_order_) {
            output += "typedef struct " + struct_name(struct_declaration->name)
                + " " + struct_name(struct_declaration->name) + ";\n";
        }
        for (const auto& enum_declaration : enum_order_) {
            output += "typedef struct " + enum_name(enum_declaration->name)
                + " " + enum_name(enum_declaration->name) + ";\n";
        }
        if (!struct_order_.empty() || !enum_order_.empty()) {
            output += '\n';
        }

        std::unordered_map<std::string, int> definition_state;
        for (const auto& type : nominal_order_) {
            output += emit_type_definition(type, definition_state);
        }

        for (const auto& statement : program.statements) {
            if (statement->kind == StmtKind::StructDeclaration
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
                    method_declaration,
                    info.methods.at(method_declaration.name));
                output += ";\n";
            }
        }
        if (!program.statements.empty() || !struct_order_.empty()
            || !enum_order_.empty()) {
            output += '\n';
        }

        for (const auto& statement : program.statements) {
            if (statement->kind == StmtKind::StructDeclaration
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
                    method_declaration,
                    info.methods.at(method_declaration.name));
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
            }
        }

        for (const auto* declaration : struct_order_) {
            auto& info = structs_.at(declaration->name);
            for (const auto& field : declaration->fields) {
                info.field_indices.emplace(field.name, info.fields.size());
                info.fields.push_back(StructFieldInfo{&field, lower_type(field.type)});
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
                info.variant_indices.emplace(variant.name, info.variants.size());
                info.variants.push_back(EnumVariantInfo{
                    &variant,
                    variant.payload_type
                        ? std::optional<CValueType>{lower_type(*variant.payload_type)}
                        : std::nullopt,
                });
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

    CValueType lower_type(const TypeReference& type) const
    {
        if (type.nullable || !type.arguments.empty()) {
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
        if (enums_.contains(type.name)) {
            return CValueType{CValueKind::Enum, type.name};
        }
        throw_backend_error(
            type.location,
            "type '" + type.name + "' is not supported by the C backend");
    }

    std::string emit_type_definition(
        const CValueType& type,
        std::unordered_map<std::string, int>& state) const
    {
        const std::string& name = type.nominal_name;
        if (state[name] == 2) {
            return {};
        }
        if (state[name] == 1) {
            const SourceLocation location = type.kind == CValueKind::Struct
                ? structs_.at(name).declaration->location
                : enums_.at(name).declaration->location;
            throw_backend_error(
                location,
                "recursive by-value type layout is not supported by the C backend: '"
                    + name + "'");
        }
        state[name] = 1;
        std::string output;
        if (type.kind == CValueKind::Enum) {
            const auto& info = enums_.at(name);
            for (const auto& variant : info.variants) {
                if (variant.payload_type
                    && (variant.payload_type->kind == CValueKind::Struct
                        || variant.payload_type->kind == CValueKind::Enum)) {
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
            state[name] = 2;
            return output;
        }

        const auto& info = structs_.at(name);
        for (const auto& field : info.fields) {
            if (field.type.kind == CValueKind::Struct
                || field.type.kind == CValueKind::Enum) {
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
        state[name] = 2;
        return output;
    }

    std::string method_declaration_text(
        const std::string& owner,
        const MethodDeclaration& method,
        const MethodInfo& info) const
    {
        std::string output = "static " + c_type_name(info.return_type) + " "
            + method_name(owner, method.name) + "("
            + struct_name(owner) + "* toro_self";
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
        push_scope();
        const auto& info = functions_.at(function.name);
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            scopes_.back().emplace(
                function.parameters[index].name,
                ValueInfo{
                    info.parameter_types[index],
                    parameter_name(function.parameters[index].name),
                });
        }

        std::string output = function_declaration(function) + "\n{\n";
        output += emit_statement_list(function.body->statements, 1);
        output += "}\n";
        pop_scope();
        return output;
    }

    std::string emit_method(
        const std::string& owner,
        const MethodDeclaration& method,
        const MethodInfo& info)
    {
        current_location_ = method.location;
        scopes_.clear();
        push_scope();
        scopes_.back().emplace(
            "self",
            ValueInfo{
                CValueType{CValueKind::Struct, owner},
                "toro_self",
                true,
            });
        for (std::size_t index = 0; index < method.parameters.size(); ++index) {
            scopes_.back().emplace(
                method.parameters[index].name,
                ValueInfo{
                    info.parameter_types[index],
                    parameter_name(method.parameters[index].name),
                });
        }

        std::string output = method_declaration_text(owner, method, info) + "\n{\n";
        output += emit_statement_list(method.body->statements, 1);
        output += "}\n";
        pop_scope();
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
            const auto initializer = emit_expression(*declaration.initializer);
            const CValueType type = declaration.explicit_type
                ? lower_type(*declaration.explicit_type)
                : initializer.type;
            const std::string c_name = variable_name(declaration.name);
            scopes_.back().emplace(declaration.name, ValueInfo{type, c_name});
            return prefix + c_type_name(type) + " " + c_name + " = "
                + initializer.code + ";\n";
        }
        case StmtKind::Assignment: {
            const auto& assignment = static_cast<const AssignmentStmt&>(statement);
            const auto value = emit_expression(*assignment.value);
            return prefix + find_value(assignment.name).c_name + " = "
                + value.code + ";\n";
        }
        case StmtKind::MemberAssignment: {
            const auto& assignment =
                static_cast<const MemberAssignmentStmt&>(statement);
            const auto target = emit_member_access(*assignment.target);
            if (!target.addressable) {
                throw_backend_error(
                    statement.location,
                    "struct field assignment requires an addressable receiver");
            }
            const auto value = emit_expression(*assignment.value);
            return prefix + target.code + " = " + value.code + ";\n";
        }
        case StmtKind::Expression: {
            const auto& expression = static_cast<const ExpressionStmt&>(statement);
            return prefix + emit_expression(*expression.expression).code + ";\n";
        }
        case StmtKind::Return: {
            const auto& return_statement = static_cast<const ReturnStmt&>(statement);
            if (!return_statement.value) {
                return prefix + "return;\n";
            }
            return prefix + "return "
                + emit_expression(*return_statement.value).code + ";\n";
        }
        case StmtKind::Block:
            return emit_block(static_cast<const BlockStmt&>(statement), depth);
        case StmtKind::If:
            return emit_if(static_cast<const IfStmt&>(statement), depth);
        case StmtKind::While: {
            const auto& loop = static_cast<const WhileStmt&>(statement);
            std::string output = prefix + "while ("
                + emit_expression(*loop.condition).code + ") ";
            output += emit_braced_block(*loop.body, depth);
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
        output += indent(depth) + "}\n";
        pop_scope();
        return output;
    }

    std::string emit_if(const IfStmt& conditional, std::size_t depth)
    {
        std::string output = indent(depth) + "if ("
            + emit_expression(*conditional.condition).code + ") ";
        output += emit_braced_block(*conditional.then_block, depth);
        if (conditional.else_branch) {
            output.resize(output.size() - 1);
            output += " else ";
            if (conditional.else_branch->kind == StmtKind::If) {
                std::string nested = emit_if(
                    static_cast<const IfStmt&>(*conditional.else_branch), depth);
                nested.erase(0, indent(depth).size());
                output += nested;
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
        if (handled.type.kind != CValueKind::Enum) {
            throw_backend_error(
                handle.location,
                "handle lowering requires a supported enum value; Result is not supported by the C backend");
        }
        const auto& info = enums_.at(handled.type.nominal_name);
        const std::string temporary =
            "toro_handle_value_" + std::to_string(temporary_index_++);
        std::string output = indent(depth) + "{\n";
        output += indent(depth + 1) + c_type_name(handled.type) + " "
            + temporary + " = " + handled.code + ";\n";
        output += indent(depth + 1) + "switch (" + temporary
            + ".toro_tag)\n" + indent(depth + 1) + "{\n";

        for (const auto& handle_case : handle.cases) {
            current_location_ = handle_case.location;
            const auto variant = info.variant_indices.find(handle_case.variant_name);
            if (variant == info.variant_indices.end()) {
                throw_backend_error(
                    handle_case.location,
                    "enum '" + handled.type.nominal_name
                        + "' has no backend variant named '"
                        + handle_case.variant_name + "'");
            }
            const auto& variant_info = info.variants[variant->second];
            output += indent(depth + 1) + "case "
                + enum_tag_name(
                    handled.type.nominal_name, handle_case.variant_name)
                + ": {\n";
            push_scope();
            if (handle_case.binding_name && variant_info.payload_type) {
                const std::string binding = variable_name(*handle_case.binding_name);
                scopes_.back().emplace(
                    *handle_case.binding_name,
                    ValueInfo{*variant_info.payload_type, binding});
                output += indent(depth + 2) + c_type_name(*variant_info.payload_type)
                    + " " + binding + " = " + temporary
                    + ".toro_payload."
                    + enum_payload_name(handle_case.variant_name) + ";\n";
            }
            output += emit_statement_list(handle_case.body->statements, depth + 2);
            output += indent(depth + 2) + "break;\n";
            pop_scope();
            output += indent(depth + 1) + "}\n";
        }
        output += indent(depth + 1) + "}\n";
        output += indent(depth) + "}\n";
        return output;
    }

    GeneratedExpression emit_expression(const Expr& expression)
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
            return {"(-" + operand.code + ")", operand.type};
        }
        case ExprKind::Binary:
            return emit_binary(static_cast<const BinaryExpr&>(expression));
        case ExprKind::Call:
            return emit_call(static_cast<const CallExpr&>(expression));
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
            };
        }
        case ExprKind::Null:
        case ExprKind::Propagation:
        case ExprKind::Cast:
            throw_backend_error(
                current_location_,
                "expression is not supported by the C backend");
        }
        throw_backend_error(current_location_, "unknown expression");
    }

    GeneratedExpression emit_binary(const BinaryExpr& binary)
    {
        const auto left = emit_expression(*binary.left);
        const auto right = emit_expression(*binary.right);
        if (left.type.kind == CValueKind::Struct
            || left.type.kind == CValueKind::Enum
            || right.type.kind == CValueKind::Struct
            || right.type.kind == CValueKind::Enum) {
            throw_backend_error(
                current_location_,
                "operators on struct or enum values are not supported by the C backend");
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
        };
    }

    GeneratedExpression emit_call(const CallExpr& call)
    {
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
        const auto function = functions_.find(callee.name);
        if (function == functions_.end()) {
            throw_backend_error(
                current_location_, "unknown backend function '" + callee.name + "'");
        }

        const auto ordered = order_arguments(
            call, function->second.declaration->parameters);

        std::string code = function_name(callee.name) + "(";
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            if (index != 0) {
                code += ", ";
            }
            code += emit_expression(*ordered[index]->value).code;
        }
        code += ")";
        return {std::move(code), function->second.return_type};
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
            + enum_payload_name(variant_name) + " = "
            + emit_expression(*call.arguments.front().value).code + "}";
        return {
            std::move(code),
            CValueType{CValueKind::Enum, enum_type},
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
                code += emit_expression(*arguments[index]->value).code;
            } else if (field.declaration->default_value) {
                code += emit_expression(*field.declaration->default_value).code;
            } else {
                code += zero_value(field.type, field.declaration->location);
            }
        }
        code += "}";
        return {
            std::move(code),
            CValueType{CValueKind::Struct, name},
        };
    }

    std::string zero_value(CValueType type, SourceLocation location) const
    {
        switch (type.kind) {
        case CValueKind::Int: return "0";
        case CValueKind::Dec: return "0.0";
        case CValueKind::Bool: return "false";
        case CValueKind::String: return "\"\"";
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
        case CValueKind::Void:
            break;
        }
        throw_backend_error(location, "field has no backend zero value");
    }

    GeneratedExpression emit_member_access(const MemberAccessExpr& member)
    {
        const auto object = emit_expression(*member.object);
        if (object.type.kind != CValueKind::Struct) {
            throw_backend_error(
                current_location_, "field access requires a struct value");
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
        };
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
        if (receiver.type.kind != CValueKind::Struct) {
            throw_backend_error(
                current_location_, "method calls require a struct receiver");
        }
        const auto& struct_info = structs_.at(receiver.type.nominal_name);
        const auto method = struct_info.methods.find(member.member);
        if (method == struct_info.methods.end()) {
            throw_backend_error(
                current_location_,
                "struct '" + receiver.type.nominal_name + "' has no method named '"
                    + member.member + "'");
        }
        if (!receiver.pointer && !receiver.addressable) {
            throw_backend_error(
                current_location_,
                "struct method receiver must be an addressable value");
        }
        const auto ordered = order_arguments(
            call, method->second.declaration->parameters);
        std::string code = method_name(receiver.type.nominal_name, member.member) + "(";
        code += receiver.pointer ? receiver.code : "&(" + receiver.code + ")";
        for (const auto* argument : ordered) {
            code += ", " + emit_expression(*argument->value).code;
        }
        code += ")";
        return {std::move(code), method->second.return_type};
    }

    GeneratedExpression emit_print(const CallExpr& call)
    {
        const auto value = emit_expression(*call.arguments.front().value);
        switch (value.type.kind) {
        case CValueKind::Int:
            return {
                "printf(\"%lld\\n\", (long long)(" + value.code + "))",
                void_type,
            };
        case CValueKind::Dec:
            return {"printf(\"%g\\n\", " + value.code + ")", void_type};
        case CValueKind::Bool:
            return {
                "printf(\"%s\\n\", (" + value.code
                    + ") ? \"true\" : \"false\")",
                void_type,
            };
        case CValueKind::String:
            return {"printf(\"%s\\n\", " + value.code + ")", void_type};
        case CValueKind::Struct:
            throw_backend_error(
                current_location_, "printing struct values is not supported by the C backend");
        case CValueKind::Enum:
            throw_backend_error(
                current_location_, "printing enum values is not supported by the C backend");
        case CValueKind::Void:
            throw_backend_error(
                current_location_, "cannot print an expression without a value");
        }
        throw_backend_error(current_location_, "unsupported print value");
    }

    void push_scope()
    {
        scopes_.emplace_back();
    }

    void pop_scope()
    {
        scopes_.pop_back();
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
    std::unordered_map<std::string, EnumInfo> enums_;
    std::vector<const EnumDeclarationStmt*> enum_order_;
    std::vector<CValueType> nominal_order_;
    std::unordered_map<std::string, FunctionInfo> functions_;
    std::vector<std::unordered_map<std::string, ValueInfo>> scopes_;
    std::size_t temporary_index_{0};
    SourceLocation current_location_{1, 1};
};

} // namespace

std::string CGenerator::generate(const Program& program)
{
    return Generator{}.generate(program);
}

} // namespace toro
