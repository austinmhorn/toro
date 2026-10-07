#include "toro/CGenerator.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace toro {
namespace {

enum class CValueKind {
    Int,
    Dec,
    Bool,
    String,
    Struct,
    Class,
    Interface,
    Enum,
    Result,
    Array,
    List,
    Map,
    Void,
};

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
    std::string key;
    std::unordered_map<std::string, CValueType> substitutions;
    CValueType return_type;
    std::vector<CValueType> parameter_types;
};

struct StructFieldInfo {
    const StructField* declaration;
    CValueType type;
};

struct MethodInfo {
    const MethodDeclaration* declaration;
    std::string key;
    std::unordered_map<std::string, CValueType> substitutions;
    CValueType return_type;
    std::vector<CValueType> parameter_types;
};

struct ConversionInfo {
    const ConversionOverload* declaration;
    std::string key;
    CValueType target_type;
};

struct StructInfo {
    const StructDeclarationStmt* declaration;
    std::string key;
    std::unordered_map<std::string, CValueType> substitutions;
    std::vector<StructFieldInfo> fields;
    std::unordered_map<std::string, std::size_t> field_indices;
    std::unordered_map<std::string, MethodInfo> methods;
    std::vector<std::string> method_order;
    std::vector<ConversionInfo> conversions;
};

struct ClassFieldInfo {
    const ClassField* declaration;
    CValueType type;
};

struct ClassInfo {
    const ClassDeclarationStmt* declaration;
    std::string key;
    std::unordered_map<std::string, CValueType> substitutions;
    std::optional<std::string> base_name;
    std::vector<ClassFieldInfo> fields;
    std::unordered_map<std::string, std::size_t> field_indices;
    std::unordered_map<std::string, MethodInfo> methods;
    std::vector<std::string> method_order;
    std::vector<ConversionInfo> conversions;
};

struct InterfaceMethodInfo {
    const InterfaceMethod* declaration;
    std::string key;
    CValueType return_type;
    std::vector<CValueType> parameter_types;
};

struct InterfaceInfo {
    const InterfaceDeclarationStmt* declaration;
    std::vector<InterfaceMethodInfo> methods;
    std::unordered_map<std::string, std::size_t> method_indices;
};

struct VirtualSlot {
    std::string name;
    std::string declaration_owner;
    const MethodInfo* declaration;
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

std::string finalize_name(std::string_view name)
{
    return class_name(name) + "_finalize";
}

std::string vtable_name(std::string_view root)
{
    return "toro_vtable_" + std::to_string(root.size()) + "_"
        + std::string(root);
}

std::string vtable_instance_name(std::string_view name)
{
    return "toro_vtable_instance_" + std::to_string(name.size()) + "_"
        + std::string(name);
}

std::string interface_name(std::string_view name)
{
    return "toro_interface_" + std::to_string(name.size()) + "_"
        + std::string(name);
}

std::string interface_vtable_name(std::string_view name)
{
    return interface_name(name) + "_vtable";
}

std::string interface_slot_name(std::string_view interface, std::string_view method)
{
    return "toro_interface_slot_" + std::to_string(interface.size()) + "_"
        + std::string(interface) + "_" + std::string(method);
}

std::string interface_thunk_name(
    std::string_view interface,
    std::string_view implementer,
    std::string_view method)
{
    return "toro_interface_thunk_" + std::to_string(interface.size()) + "_"
        + std::string(interface) + "_" + std::to_string(implementer.size()) + "_"
        + std::string(implementer) + "_" + std::string(method);
}

std::string interface_vtable_instance_name(
    std::string_view interface,
    std::string_view implementer)
{
    return "toro_interface_vtable_instance_"
        + std::to_string(interface.size()) + "_" + std::string(interface) + "_"
        + std::to_string(implementer.size()) + "_" + std::string(implementer);
}

std::string interface_struct_storage_name(std::string_view implementer)
{
    return "toro_struct_value_" + std::to_string(implementer.size()) + "_"
        + std::string(implementer);
}

std::string interface_class_retain_name(
    std::string_view interface,
    std::string_view implementer)
{
    return "toro_interface_class_retain_" + std::to_string(interface.size())
        + "_" + std::string(interface) + "_"
        + std::to_string(implementer.size()) + "_" + std::string(implementer);
}

std::string interface_class_release_name(
    std::string_view interface,
    std::string_view implementer)
{
    return "toro_interface_class_release_" + std::to_string(interface.size())
        + "_" + std::string(interface) + "_"
        + std::to_string(implementer.size()) + "_" + std::string(implementer);
}

std::string virtual_slot_name(
    std::string_view owner,
    std::string_view name)
{
    return "toro_virtual_" + std::to_string(owner.size()) + "_"
        + std::string(owner) + "_" + std::string(name);
}

std::string virtual_thunk_name(
    std::string_view concrete,
    std::string_view method)
{
    return "toro_virtual_thunk_" + std::to_string(concrete.size()) + "_"
        + std::string(concrete) + "_" + std::string(method);
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
    std::string result;
    switch (type.kind) {
    case CValueKind::Int: result = "i"; break;
    case CValueKind::Dec: result = "d"; break;
    case CValueKind::Bool: result = "b"; break;
    case CValueKind::String: result = "s"; break;
    case CValueKind::Struct:
        result = "s" + std::to_string(type.nominal_name.size()) + "_"
            + type.nominal_name;
        break;
    case CValueKind::Class:
        result = "c" + std::to_string(type.nominal_name.size()) + "_"
            + type.nominal_name;
        break;
    case CValueKind::Interface:
        result = "i" + std::to_string(type.nominal_name.size()) + "_"
            + type.nominal_name;
        break;
    case CValueKind::Enum:
        result = "e" + std::to_string(type.nominal_name.size()) + "_"
            + type.nominal_name;
        break;
    case CValueKind::Result:
        result = "r" + std::to_string(type_mangle(type.arguments[0]).size()) + "_"
            + type_mangle(type.arguments[0]) + "_"
            + std::to_string(type_mangle(type.arguments[1]).size()) + "_"
            + type_mangle(type.arguments[1]);
        break;
    case CValueKind::Array:
        result = "a" + type_mangle(type.arguments[0]);
        break;
    case CValueKind::List:
        result = "l" + type_mangle(type.arguments[0]);
        break;
    case CValueKind::Map:
        result = "m" + type_mangle(type.arguments[0]) + "_"
            + type_mangle(type.arguments[1]);
        break;
    case CValueKind::Void: result = "v"; break;
    }
    if (type.nullable) {
        result += "n";
    }
    return result;
}

std::string specialization_key(
    std::string_view name,
    const std::vector<CValueType>& arguments)
{
    std::string key{name};
    for (const auto& argument : arguments) {
        const std::string mangled = type_mangle(argument);
        key += "__" + std::to_string(mangled.size()) + "_" + mangled;
    }
    return key;
}

std::string type_reference_mangle(const TypeReference& type)
{
    std::string result = "n" + std::to_string(type.name.size()) + "_" + type.name;
    if (!type.arguments.empty()) {
        result += "_g" + std::to_string(type.arguments.size());
        for (const auto& argument : type.arguments) {
            const std::string mangled = type_reference_mangle(argument);
            result += "_" + std::to_string(mangled.size()) + "_" + mangled;
        }
    }
    if (type.nullable) {
        result += "_nullable";
    }
    return result;
}

template <typename Parameters>
std::string source_signature_key(
    std::string_view name,
    const Parameters& parameters)
{
    std::string result{name};
    result += "__signature";
    for (const auto& parameter : parameters) {
        const std::string mangled = type_reference_mangle(parameter.type);
        result += "__" + std::to_string(mangled.size()) + "_" + mangled;
    }
    return result;
}

std::string result_name(const CValueType& type)
{
    return "toro_result_" + type_mangle(type);
}

std::string collection_name(const CValueType& type)
{
    std::string prefix;
    switch (type.kind) {
    case CValueKind::Array: prefix = "array"; break;
    case CValueKind::List: prefix = "list"; break;
    case CValueKind::Map: prefix = "map"; break;
    default: prefix = "collection"; break;
    }
    return "toro_" + prefix + "_" + type_mangle(type);
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

std::string conversion_name(std::string_view owner, std::string_view key)
{
    return "toro_conversion_" + std::to_string(owner.size()) + "_"
        + std::string(owner) + "_" + std::string(key);
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
    case CValueKind::Interface: return interface_name(type.nominal_name);
    case CValueKind::Enum: return enum_name(type.nominal_name);
    case CValueKind::Result: return result_name(type);
    case CValueKind::Array: return collection_name(type);
    case CValueKind::List:
    case CValueKind::Map:
        return collection_name(type) + "*";
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

        for (const auto& key : struct_order_) {
            output += "typedef struct " + struct_name(key)
                + " " + struct_name(key) + ";\n";
        }
        for (const auto& key : class_order_) {
            output += "typedef struct " + class_name(key)
                + " " + class_name(key) + ";\n";
        }
        for (const auto& interface_declaration : interface_order_) {
            output += "typedef struct " + interface_name(interface_declaration->name)
                + " " + interface_name(interface_declaration->name) + ";\n";
            output += "typedef struct "
                + interface_vtable_name(interface_declaration->name) + " "
                + interface_vtable_name(interface_declaration->name) + ";\n";
        }
        for (const auto& key : class_order_) {
            if (classes_.at(key).base_name
                || virtual_slots(key).empty()) {
                continue;
            }
            output += "typedef struct " + vtable_name(key)
                + " " + vtable_name(key) + ";\n";
        }
        for (const auto& enum_declaration : enum_order_) {
            output += "typedef struct " + enum_name(enum_declaration->name)
                + " " + enum_name(enum_declaration->name) + ";\n";
        }
        for (const auto& result : result_order_) {
            output += "typedef struct " + result_name(result.type) + " "
                + result_name(result.type) + ";\n";
        }
        for (const auto& collection : collection_order_) {
            output += "typedef struct " + collection_name(collection) + " "
                + collection_name(collection) + ";\n";
        }
        if (!struct_order_.empty() || !class_order_.empty()
            || !interface_order_.empty() || !enum_order_.empty()
            || !result_order_.empty() || !collection_order_.empty()) {
            output += '\n';
        }
        for (const auto& key : class_order_) {
            const auto& info = classes_.at(key);
            output += "static void " + finalize_name(key)
                + "(void* toro_raw_value);\n";
            output += "static void " + retain_name(key) + "("
                + class_name(key) + "* toro_value);\n";
            output += "static void " + release_name(key) + "("
                + class_name(key) + "* toro_value);\n";
            if (info.methods.contains("destroy")) {
                output += "static void " + method_name(key, "destroy")
                    + "(" + class_name(key)
                    + "* toro_self);\n";
            }
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
        for (const auto* declaration : interface_order_) {
            output += emit_interface_definition(*declaration);
        }
        for (const auto& collection : collection_order_) {
            output += emit_collection_definition(collection);
        }

        for (const auto& key : class_order_) {
            if (classes_.at(key).base_name) {
                continue;
            }
            output += emit_vtable_definition(key);
        }

        for (const auto& statement : program.statements) {
            if (statement->kind != StmtKind::FunctionDeclaration
                && statement->kind != StmtKind::StructDeclaration
                && statement->kind != StmtKind::ClassDeclaration
                && statement->kind != StmtKind::EnumDeclaration
                && statement->kind != StmtKind::InterfaceDeclaration) {
                throw_backend_error(
                    statement->location,
                    "top-level statements are not supported by the C backend");
            }
        }
        for (const auto& key : function_order_) {
            output += function_declaration(key);
            output += ";\n";
        }
        for (const auto& key : struct_order_) {
            const auto& info = structs_.at(key);
            for (const auto& method_key : info.method_order) {
                const auto& method_info = info.methods.at(method_key);
                const auto& method_declaration = *method_info.declaration;
                output += method_declaration_text(
                    key,
                    CValueKind::Struct,
                    method_declaration,
                    method_info);
                output += ";\n";
            }
            for (const auto& conversion : info.conversions) {
                output += conversion_declaration_text(
                    key, CValueKind::Struct, conversion) + ";\n";
            }
        }
        for (const auto& key : class_order_) {
            const auto& info = classes_.at(key);
            for (const auto& method_key : info.method_order) {
                const auto& method_info = info.methods.at(method_key);
                const auto& method = *method_info.declaration;
                if (!method.body) {
                    continue;
                }
                output += method_declaration_text(
                    key,
                    CValueKind::Class,
                    method,
                    method_info);
                output += ";\n";
            }
            for (const auto& conversion : info.conversions) {
                output += conversion_declaration_text(
                    key, CValueKind::Class, conversion) + ";\n";
            }
        }

        for (const auto& key : struct_order_) {
            const auto& info = structs_.at(key);
            for (const auto& implemented : info.declaration->interfaces) {
                output += emit_interface_thunks(
                    implemented.name, key, CValueKind::Struct);
                output += emit_interface_vtable_instance(
                    implemented.name, key, CValueKind::Struct);
            }
        }
        for (const auto& key : class_order_) {
            for (const auto& implemented : effective_class_interfaces(
                     key)) {
                output += emit_interface_thunks(
                    implemented, key, CValueKind::Class);
                output += emit_interface_vtable_instance(
                    implemented, key, CValueKind::Class);
            }
        }

        for (const auto& key : class_order_) {
            const auto& info = classes_.at(key);
            if (info.declaration->is_abstract
                || virtual_slots(class_root(key)).empty()) {
                continue;
            }
            output += emit_virtual_thunks(key);
            output += emit_vtable_instance(key);
        }
        if (!program.statements.empty() || !struct_order_.empty()
            || !class_order_.empty()
            || !enum_order_.empty()) {
            output += '\n';
        }

        for (const auto& key : function_order_) {
            output += emit_function(key);
            output += '\n';
        }
        for (const auto& key : struct_order_) {
            const auto& info = structs_.at(key);
            for (const auto& method_key : info.method_order) {
                const auto& method_info = info.methods.at(method_key);
                const auto& method_declaration = *method_info.declaration;
                output += emit_method(
                    key,
                    CValueKind::Struct,
                    method_declaration,
                    method_info);
                output += '\n';
            }
            for (const auto& conversion : info.conversions) {
                output += emit_conversion(
                    key, CValueKind::Struct, conversion) + '\n';
            }
        }
        for (const auto& key : class_order_) {
            const auto& info = classes_.at(key);
            for (const auto& method_key : info.method_order) {
                const auto& method_info = info.methods.at(method_key);
                const auto& method = *method_info.declaration;
                if (!method.body) {
                    continue;
                }
                output += emit_method(
                    key,
                    CValueKind::Class,
                    method,
                    method_info);
                output += '\n';
            }
            for (const auto& conversion : info.conversions) {
                output += emit_conversion(
                    key, CValueKind::Class, conversion) + '\n';
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
    static bool is_managed_reference(const CValueType& type)
    {
        return type.kind == CValueKind::Class
            || type.kind == CValueKind::Interface
            || type.kind == CValueKind::List
            || type.kind == CValueKind::Map;
    }

    static bool is_owned_runtime_value(const CValueType& type)
    {
        return is_managed_reference(type) || type.kind == CValueKind::Array;
    }

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

    static bool contains_interface_value(const CValueType& type)
    {
        if (type.kind == CValueKind::Interface) {
            return true;
        }
        return std::ranges::any_of(
            type.arguments,
            [](const CValueType& argument) {
                return contains_interface_value(argument);
            });
    }

    bool is_class_base_of(
        const std::string& base_name,
        const std::string& derived_name) const
    {
        if (base_name == derived_name) {
            return true;
        }
        auto current = classes_.at(derived_name).base_name;
        while (current) {
            if (*current == base_name) {
                return true;
            }
            current = classes_.at(*current).base_name;
        }
        return false;
    }

    std::vector<std::string> effective_class_interfaces(
        const std::string& name) const
    {
        std::vector<std::string> result;
        const auto& info = classes_.at(name);
        if (info.base_name) {
            result = effective_class_interfaces(*info.base_name);
        }
        for (const auto& interface_reference : info.declaration->interfaces) {
            if (std::ranges::find(result, interface_reference.name) == result.end()) {
                result.push_back(interface_reference.name);
            }
        }
        return result;
    }

    bool implements_interface(
        const CValueType& type,
        const std::string& interface) const
    {
        if (type.kind == CValueKind::Struct) {
            const auto& declarations = structs_.at(type.nominal_name)
                                           .declaration->interfaces;
            return std::ranges::any_of(
                declarations,
                [&](const TypeReference& candidate) {
                    return candidate.name == interface;
                });
        }
        if (type.kind == CValueKind::Class) {
            const auto implemented = effective_class_interfaces(type.nominal_name);
            return std::ranges::find(implemented, interface) != implemented.end();
        }
        return type.kind == CValueKind::Interface
            && type.nominal_name == interface;
    }

    std::string retain_call(
        const CValueType& type,
        const std::string& value) const
    {
        if (type.kind == CValueKind::Class) {
            return retain_name(type.nominal_name) + "(" + value + ")";
        }
        if (type.kind == CValueKind::Interface) {
            return interface_name(type.nominal_name) + "_retain(&" + value + ")";
        }
        if (type.kind == CValueKind::List || type.kind == CValueKind::Map) {
            return collection_name(type) + "_retain(" + value + ")";
        }
        throw_backend_error(current_location_, "cannot retain a non-reference value");
    }

    std::string release_call(
        const CValueType& type,
        const std::string& value) const
    {
        if (type.kind == CValueKind::Class) {
            return release_name(type.nominal_name) + "(" + value + ")";
        }
        if (type.kind == CValueKind::Interface) {
            return interface_name(type.nominal_name) + "_release(&" + value + ")";
        }
        if (type.kind == CValueKind::List || type.kind == CValueKind::Map) {
            return collection_name(type) + "_release(" + value + ")";
        }
        if (type.kind == CValueKind::Array) {
            return collection_name(type) + "_destroy(&" + value + ")";
        }
        throw_backend_error(current_location_, "cannot release a non-reference value");
    }

    std::string class_root(const std::string& name) const
    {
        std::string current = name;
        while (classes_.at(current).base_name) {
            current = *classes_.at(current).base_name;
        }
        return current;
    }

    std::vector<VirtualSlot> virtual_slots(const std::string& root) const
    {
        std::vector<VirtualSlot> slots;
        for (const auto& key : class_order_) {
            if (class_root(key) != root) {
                continue;
            }
            const auto& info = classes_.at(key);
            for (const auto& member : info.declaration->members) {
                if (member->kind != ClassMemberKind::Method) {
                    continue;
                }
                const auto& method =
                    static_cast<const MethodDeclaration&>(*member);
                if (!method.is_virtual || method.is_override) {
                    continue;
                }
                const auto runtime_method = std::ranges::find_if(
                    info.methods,
                    [&](const auto& entry) {
                        return entry.second.declaration == &method;
                    });
                if (runtime_method == info.methods.end()) {
                    throw_backend_error(
                        method.location, "missing virtual method runtime declaration");
                }
                slots.push_back(VirtualSlot{
                    method.name,
                    key,
                    &runtime_method->second,
                });
            }
        }
        return slots;
    }

    std::optional<VirtualSlot> find_virtual_slot(
        const std::string& class_type,
        const MethodInfo& method) const
    {
        const auto slots = virtual_slots(class_root(class_type));
        const auto slot = std::ranges::find_if(
            slots,
            [&](const VirtualSlot& candidate) {
                return candidate.name == method.declaration->name
                    && candidate.declaration->parameter_types
                        == method.parameter_types
                    && is_class_base_of(
                        candidate.declaration_owner, class_type);
            });
        return slot == slots.end()
            ? std::nullopt
            : std::optional<VirtualSlot>{*slot};
    }

    std::string class_path_to_base(
        const std::string& derived_name,
        const std::string& base_name) const
    {
        std::string path;
        std::string current = derived_name;
        while (current != base_name) {
            const auto& info = classes_.at(current);
            if (!info.base_name) {
                throw_backend_error(
                    current_location_,
                    "cannot form class upcast from '" + derived_name + "' to '"
                        + base_name + "'");
            }
            path += "toro_base.";
            current = *info.base_name;
        }
        return path;
    }

    std::string class_member_access(
        const std::string& expression,
        const std::string& static_type,
        const std::string& owner,
        const std::string& member) const
    {
        return "(" + expression + ")->"
            + class_path_to_base(static_type, owner) + member;
    }

    std::string class_header_access(
        const std::string& expression,
        const std::string& static_type,
        const std::string& member) const
    {
        std::string root = static_type;
        while (classes_.at(root).base_name) {
            root = *classes_.at(root).base_name;
        }
        return class_member_access(expression, static_type, root, member);
    }

    std::string class_upcast(
        std::string expression,
        const std::string& derived_name,
        const std::string& base_name) const
    {
        std::string current = derived_name;
        while (current != base_name) {
            const auto& info = classes_.at(current);
            if (!info.base_name) {
                throw_backend_error(
                    current_location_,
                    "cannot form class upcast from '" + derived_name + "' to '"
                        + base_name + "'");
            }
            expression = "(&(" + expression + ")->toro_base)";
            current = *info.base_name;
        }
        return expression;
    }

    std::vector<std::pair<std::string, const ClassFieldInfo*>>
    effective_class_fields(const std::string& name) const
    {
        std::vector<std::pair<std::string, const ClassFieldInfo*>> fields;
        const auto& info = classes_.at(name);
        if (info.base_name) {
            fields = effective_class_fields(*info.base_name);
        }
        for (const auto& field : info.fields) {
            const auto duplicate = std::ranges::find_if(
                fields,
                [&](const auto& inherited) {
                    return inherited.second->declaration->name
                        == field.declaration->name;
                });
            if (duplicate != fields.end()) {
                throw_backend_error(
                    field.declaration->location,
                    "inherited field shadowing is not supported by the C backend: '"
                        + name + "." + field.declaration->name + "'");
            }
            fields.emplace_back(name, &field);
        }
        return fields;
    }

    std::optional<std::pair<std::string, const ClassFieldInfo*>> find_class_field(
        const std::string& name,
        const std::string& field_name_value) const
    {
        const auto& info = classes_.at(name);
        if (const auto field = info.field_indices.find(field_name_value);
            field != info.field_indices.end()) {
            return std::pair<std::string, const ClassFieldInfo*>{
                name, &info.fields[field->second]};
        }
        return info.base_name
            ? find_class_field(*info.base_name, field_name_value)
            : std::nullopt;
    }

    std::optional<std::pair<std::string, const MethodInfo*>> find_class_method(
        const std::string& name,
        const std::string& method_name_value) const
    {
        const auto& info = classes_.at(name);
        if (const auto method = info.methods.find(method_name_value);
            method != info.methods.end()) {
            return std::pair<std::string, const MethodInfo*>{
                name, &method->second};
        }
        return info.base_name
            ? find_class_method(*info.base_name, method_name_value)
            : std::nullopt;
    }

    std::optional<std::pair<std::string, const MethodInfo*>> find_class_method(
        const std::string& name,
        const std::string& method_name_value,
        const std::vector<CValueType>& parameter_types) const
    {
        const auto& info = classes_.at(name);
        const auto method = std::ranges::find_if(
            info.methods,
            [&](const auto& entry) {
                return entry.second.declaration->name == method_name_value
                    && entry.second.parameter_types == parameter_types;
            });
        if (method != info.methods.end()) {
            return std::pair<std::string, const MethodInfo*>{
                name, &method->second};
        }
        return info.base_name
            ? find_class_method(
                  *info.base_name, method_name_value, parameter_types)
            : std::nullopt;
    }

    std::optional<std::pair<std::string, const MethodDeclaration*>>
    find_class_method_declaration(
        const std::string& name,
        const std::string& method_name_value) const
    {
        const auto& info = classes_.at(name);
        for (const auto& member : info.declaration->members) {
            if (member->kind != ClassMemberKind::Method) {
                continue;
            }
            const auto& method = static_cast<const MethodDeclaration&>(*member);
            if (method.name == method_name_value) {
                return std::pair<std::string, const MethodDeclaration*>{
                    name, &method};
            }
        }
        return info.base_name
            ? find_class_method_declaration(*info.base_name, method_name_value)
            : std::nullopt;
    }

    template <typename Members>
    static std::string method_source_key(
        const Members& members,
        const MethodDeclaration& selected,
        const std::vector<CValueType>& generic_arguments)
    {
        std::size_t overload_count = 0;
        for (const auto& member : members) {
            if (member->kind != ClassMemberKind::Method) {
                continue;
            }
            const auto& method = static_cast<const MethodDeclaration&>(*member);
            if (method.name != selected.name) {
                continue;
            }
            ++overload_count;
        }
        std::string base = selected.name;
        if (overload_count > 1) {
            base = source_signature_key(selected.name, selected.parameters);
        }
        return specialization_key(base, generic_arguments);
    }

    std::string method_source_key(
        const std::string& owner,
        CValueKind owner_kind,
        const MethodDeclaration& method,
        const std::vector<CValueType>& generic_arguments) const
    {
        if (owner_kind == CValueKind::Struct) {
            return method_source_key(
                structs_.at(owner).declaration->methods,
                method,
                generic_arguments);
        }
        return method_source_key(
            classes_.at(owner).declaration->members,
            method,
            generic_arguments);
    }

    std::optional<std::pair<std::string, const MethodInfo*>>
    find_class_method_by_declaration(
        const std::string& name,
        const MethodDeclaration& declaration) const
    {
        const auto& info = classes_.at(name);
        const auto method = std::ranges::find_if(
            info.methods,
            [&](const auto& entry) {
                return entry.second.declaration == &declaration;
            });
        if (method != info.methods.end()) {
            return std::pair<std::string, const MethodInfo*>{
                name, &method->second};
        }
        return info.base_name
            ? find_class_method_by_declaration(*info.base_name, declaration)
            : std::nullopt;
    }

    std::optional<std::string> find_class_method_owner(
        const std::string& name,
        const MethodDeclaration& declaration) const
    {
        const auto& info = classes_.at(name);
        const bool declared_here = std::ranges::any_of(
            info.declaration->members,
            [&](const auto& member) {
                return member->kind == ClassMemberKind::Method
                    && member.get() == &declaration;
            });
        if (declared_here) {
            return name;
        }
        return info.base_name
            ? find_class_method_owner(*info.base_name, declaration)
            : std::nullopt;
    }

    std::optional<std::string> base_initializer_owner(
        const std::string& name) const
    {
        auto current = classes_.at(name).base_name;
        while (current) {
            const auto& info = classes_.at(*current);
            if (info.methods.contains("init")) {
                return current;
            }
            current = info.base_name;
        }
        return std::nullopt;
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

    std::unordered_map<std::string, CValueType> substitutions_for(
        const std::vector<GenericParameter>& parameters,
        const std::vector<CValueType>& arguments,
        SourceLocation location) const
    {
        if (parameters.size() != arguments.size()) {
            throw_backend_error(
                location,
                "generic specialization expects "
                    + std::to_string(parameters.size()) + " type arguments, got "
                    + std::to_string(arguments.size()));
        }
        std::unordered_map<std::string, CValueType> substitutions;
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            substitutions.emplace(parameters[index].name, arguments[index]);
        }
        return substitutions;
    }

    CValueType instantiate_struct(
        const StructDeclarationStmt& declaration,
        const std::vector<CValueType>& arguments)
    {
        const std::string key = specialization_key(declaration.name, arguments);
        if (const auto existing = structs_.find(key); existing != structs_.end()) {
            if (existing->second.declaration != &declaration) {
                throw_backend_error(
                    declaration.location,
                    "C backend struct specialization identity collides with '"
                        + existing->second.declaration->name + "'");
            }
            return CValueType{CValueKind::Struct, key};
        }
        if (instantiating_types_.contains(key)) {
            return CValueType{CValueKind::Struct, key};
        }
        instantiating_types_.insert(key);
        const auto saved = current_substitutions_;
        current_substitutions_ = substitutions_for(
            declaration.generic_parameters, arguments, declaration.location);
        StructInfo info{
            &declaration, key, current_substitutions_, {}, {}, {}, {}, {}};
        for (const auto& field : declaration.fields) {
            const CValueType field_type = lower_type(field.type);
            if (field_type.kind == CValueKind::Interface) {
                throw_backend_error(
                    field.location,
                    "interface-valued struct fields are not supported by the C backend");
            }
            if (contains_class_reference(field_type)) {
                throw_backend_error(
                    field.location,
                    "class-reference struct fields are not supported by the C backend");
            }
            info.field_indices.emplace(field.name, info.fields.size());
            info.fields.push_back(StructFieldInfo{&field, field_type});
        }
        for (const auto& member : declaration.methods) {
            const auto& method = static_cast<const MethodDeclaration&>(*member);
            if (!method.generic_parameters.empty()) {
                if (method.is_virtual || method.is_override) {
                    throw_backend_error(
                        method.location,
                        "generic virtual methods are not supported by the C backend");
                }
                continue;
            }
            if (method.is_virtual || method.is_override || !method.body) {
                throw_backend_error(
                    method.location,
                    "virtual or bodyless struct methods are not supported by the C backend");
            }
            const std::string method_key = method_source_key(
                declaration.methods, method, {});
            MethodInfo method_info{
                &method, method_key, current_substitutions_, void_type, {}};
            if (method.return_type) {
                method_info.return_type = lower_type(*method.return_type);
            }
            for (const auto& parameter : method.parameters) {
                method_info.parameter_types.push_back(lower_type(parameter.type));
            }
            info.methods.emplace(method_key, std::move(method_info));
            info.method_order.push_back(method_key);
        }
        for (const auto& conversion : declaration.conversions) {
            const CValueType target_type = lower_type(conversion->target_type);
            info.conversions.push_back(ConversionInfo{
                conversion.get(),
                "as__" + type_mangle(target_type),
                target_type,
            });
        }
        current_substitutions_ = saved;
        instantiating_types_.erase(key);
        structs_.emplace(key, std::move(info));
        struct_order_.push_back(key);
        nominal_order_.push_back(CValueType{CValueKind::Struct, key});
        return CValueType{CValueKind::Struct, key};
    }

    CValueType instantiate_class(
        const ClassDeclarationStmt& declaration,
        const std::vector<CValueType>& arguments)
    {
        const std::string key = specialization_key(declaration.name, arguments);
        if (const auto existing = classes_.find(key); existing != classes_.end()) {
            if (existing->second.declaration != &declaration) {
                throw_backend_error(
                    declaration.location,
                    "C backend class specialization identity collides with '"
                        + existing->second.declaration->name + "'");
            }
            return CValueType{CValueKind::Class, key};
        }
        if (instantiating_types_.contains(key)) {
            return CValueType{CValueKind::Class, key};
        }
        instantiating_types_.insert(key);
        const auto saved = current_substitutions_;
        current_substitutions_ = substitutions_for(
            declaration.generic_parameters, arguments, declaration.location);
        std::optional<std::string> base_name;
        if (declaration.base_type) {
            base_name = lower_type(*declaration.base_type).nominal_name;
        }
        ClassInfo info{
            &declaration, key, current_substitutions_, base_name, {}, {}, {}, {}, {}};
        for (const auto& member : declaration.members) {
            if (member->kind == ClassMemberKind::Conversion) {
                const auto& conversion =
                    static_cast<const ConversionOverload&>(*member);
                const CValueType target_type = lower_type(conversion.target_type);
                info.conversions.push_back(ConversionInfo{
                    &conversion,
                    "as__" + type_mangle(target_type),
                    target_type,
                });
                continue;
            }
            if (member->kind == ClassMemberKind::Field) {
                const auto& field = static_cast<const ClassField&>(*member);
                const CValueType field_type = lower_type(field.type);
                if (field_type.kind == CValueKind::Interface) {
                    throw_backend_error(
                        field.location,
                        "interface-valued class fields are not supported by the C backend");
                }
                if (field.is_weak
                    && (field_type.kind != CValueKind::Class
                        || !field_type.nullable)) {
                    throw_backend_error(
                        field.location, "weak fields require a nullable class type");
                }
                info.field_indices.emplace(field.name, info.fields.size());
                info.fields.push_back(ClassFieldInfo{&field, field_type});
                continue;
            }
            const auto& method = static_cast<const MethodDeclaration&>(*member);
            if (!method.generic_parameters.empty()) {
                if (method.is_virtual || method.is_override) {
                    throw_backend_error(
                        method.location,
                        "generic virtual methods are not supported by the C backend");
                }
                continue;
            }
            if (!method.body && !method.is_virtual) {
                throw_backend_error(
                    method.location,
                    "only virtual class methods may omit a body in the C backend");
            }
            const std::string method_key = method_source_key(
                declaration.members, method, {});
            MethodInfo method_info{
                &method, method_key, current_substitutions_, void_type, {}};
            if (method.return_type) {
                method_info.return_type = lower_type(*method.return_type);
            }
            for (const auto& parameter : method.parameters) {
                method_info.parameter_types.push_back(lower_type(parameter.type));
            }
            info.methods.emplace(method_key, std::move(method_info));
            info.method_order.push_back(method_key);
        }
        current_substitutions_ = saved;
        instantiating_types_.erase(key);
        classes_.emplace(key, std::move(info));
        class_order_.push_back(key);
        nominal_order_.push_back(CValueType{CValueKind::Class, key});
        return CValueType{CValueKind::Class, key};
    }

    std::string instantiate_method(
        const std::string& owner,
        CValueKind owner_kind,
        const MethodDeclaration& method,
        const std::vector<CValueType>& arguments)
    {
        const std::string key = method_source_key(
            owner, owner_kind, method, arguments);
        auto add_to = [&](auto& info) {
            if (const auto existing = info.methods.find(key);
                existing != info.methods.end()) {
                if (existing->second.declaration != &method) {
                    throw_backend_error(
                        method.location,
                        "C backend method specialization identity collides with '"
                            + existing->second.declaration->name + "'");
                }
                return;
            }
            const auto saved = current_substitutions_;
            current_substitutions_ = info.substitutions;
            const auto method_substitutions = substitutions_for(
                method.generic_parameters, arguments, method.location);
            current_substitutions_.insert(
                method_substitutions.begin(), method_substitutions.end());
            MethodInfo method_info{
                &method, key, current_substitutions_, void_type, {}};
            if (method.return_type) {
                method_info.return_type = lower_type(*method.return_type);
            }
            for (const auto& parameter : method.parameters) {
                method_info.parameter_types.push_back(lower_type(parameter.type));
            }
            info.methods.emplace(key, std::move(method_info));
            info.method_order.push_back(key);
            if (method.body) {
                discover_statement(*method.body);
            }
            current_substitutions_ = saved;
        };
        if (owner_kind == CValueKind::Struct) {
            add_to(structs_.at(owner));
        } else {
            add_to(classes_.at(owner));
        }
        return key;
    }

    void collect_types(const Program& program)
    {
        for (const auto& statement : program.statements) {
            if (statement->kind == StmtKind::StructDeclaration) {
                const auto& declaration =
                    static_cast<const StructDeclarationStmt&>(*statement);
                struct_templates_.emplace(declaration.name, &declaration);
            } else if (statement->kind == StmtKind::ClassDeclaration) {
                const auto& declaration =
                    static_cast<const ClassDeclarationStmt&>(*statement);
                class_templates_.emplace(declaration.name, &declaration);
            }
        }
        for (const auto& statement : program.statements) {
            if (statement->kind != StmtKind::InterfaceDeclaration) {
                continue;
            }
            const auto& declaration =
                static_cast<const InterfaceDeclarationStmt&>(*statement);
            if (!declaration.generic_parameters.empty()) {
                throw_backend_error(
                    declaration.location,
                    "generic interfaces are not supported by the C backend");
            }
            interfaces_.emplace(
                declaration.name, InterfaceInfo{&declaration, {}, {}});
            interface_order_.push_back(&declaration);
        }

        for (const auto& statement : program.statements) {
            if (statement->kind == StmtKind::StructDeclaration) {
                const auto& declaration =
                    static_cast<const StructDeclarationStmt&>(*statement);
                if (declaration.generic_parameters.empty()) {
                    static_cast<void>(instantiate_struct(declaration, {}));
                }
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
                if (declaration.generic_parameters.empty()) {
                    static_cast<void>(instantiate_class(declaration, {}));
                }
            }
        }

        for (const auto* declaration : interface_order_) {
            auto& info = interfaces_.at(declaration->name);
            for (std::size_t method_index = 0;
                 method_index < declaration->methods.size(); ++method_index) {
                const auto& method = declaration->methods[method_index];
                if (!method.generic_parameters.empty()) {
                    throw_backend_error(
                        method.location,
                        "generic interface methods are not supported by the C backend");
                }
                std::size_t overload_count = 0;
                std::size_t overload_index = 0;
                for (std::size_t index = 0;
                     index < declaration->methods.size(); ++index) {
                    if (declaration->methods[index].name != method.name) {
                        continue;
                    }
                    if (index == method_index) {
                        overload_index = overload_count;
                    }
                    ++overload_count;
                }
                std::string method_key = method.name;
                if (overload_count > 1) {
                    method_key += "__overload_" + std::to_string(overload_index);
                }
                InterfaceMethodInfo method_info{
                    &method, method_key, void_type, {}};
                if (method.return_type) {
                    method_info.return_type = lower_type(*method.return_type);
                }
                for (const auto& parameter : method.parameters) {
                    method_info.parameter_types.push_back(lower_type(parameter.type));
                }
                info.method_indices.emplace(method_key, info.methods.size());
                info.methods.push_back(std::move(method_info));
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
                    if (contains_interface_value(*payload_type)) {
                        throw_backend_error(
                            variant.location,
                            "interface-valued enum payloads are not supported by the C backend");
                    }
                }
                info.variant_indices.emplace(variant.name, info.variants.size());
                info.variants.push_back(EnumVariantInfo{
                    &variant,
                    std::move(payload_type),
                });
            }
        }

    }

    std::string instantiate_function(
        const FunctionDeclarationStmt& function,
        const std::vector<CValueType>& arguments)
    {
        const auto templates = function_templates_.find(function.name);
        if (templates == function_templates_.end()) {
            throw_backend_error(function.location,
                "missing backend function template for '" + function.name + "'");
        }
        if (std::ranges::find(templates->second, &function)
            == templates->second.end()) {
            throw_backend_error(function.location,
                "missing backend overload declaration for '" + function.name + "'");
        }
        std::string base_key = function.name;
        if (templates->second.size() > 1) {
            base_key = source_signature_key(
                function.name, function.parameters);
        }
        const std::string key = specialization_key(base_key, arguments);
        if (const auto existing = functions_.find(key);
            existing != functions_.end()) {
            if (existing->second.declaration != &function) {
                throw_backend_error(
                    function.location,
                    "C backend function specialization identity collides with '"
                        + existing->second.declaration->name + "'");
            }
            return key;
        }
        const auto saved = current_substitutions_;
        current_substitutions_ = substitutions_for(
            function.generic_parameters, arguments, function.location);
        FunctionInfo info{
            &function, key, current_substitutions_, void_type, {}};
        if (function.return_type) {
            info.return_type = lower_type(*function.return_type);
        }
        for (const auto& parameter : function.parameters) {
            info.parameter_types.push_back(lower_type(parameter.type));
        }
        current_substitutions_ = saved;
        functions_.emplace(key, std::move(info));
        function_order_.push_back(key);
        return key;
    }

    void collect_functions(const Program& program)
    {
        for (const auto& statement : program.statements) {
            if (statement->kind == StmtKind::FunctionDeclaration) {
                const auto& function =
                    static_cast<const FunctionDeclarationStmt&>(*statement);
                function_templates_[function.name].push_back(&function);
            }
        }
        if (const auto main = function_templates_.find("main");
            main != function_templates_.end() && main->second.size() != 1) {
            throw_backend_error(
                main->second.front()->location,
                "toro entry function 'main' cannot be overloaded in the C backend");
        }
        for (const auto& statement : program.statements) {
            if (statement->kind != StmtKind::FunctionDeclaration) {
                continue;
            }
            const auto& function =
                static_cast<const FunctionDeclarationStmt&>(*statement);
            if (function.name == "main" && !function.generic_parameters.empty()) {
                throw_backend_error(
                    function.location,
                    "toro entry function 'main' cannot be generic in the C backend");
            }
            if (function.generic_parameters.empty()) {
                static_cast<void>(instantiate_function(function, {}));
            }
        }
    }

    void register_collection(
        const CValueType& type,
        SourceLocation location)
    {
        const std::size_t expected_arguments =
            type.kind == CValueKind::Map ? 2U : 1U;
        if ((type.kind != CValueKind::Array
                && type.kind != CValueKind::List
                && type.kind != CValueKind::Map)
            || type.arguments.size() != expected_arguments) {
            throw_backend_error(location,
                "invalid concrete collection type reached the C backend");
        }
        if (type.nullable) {
            throw_backend_error(location,
                "nullable collection values are not supported by the C backend");
        }
        if (type.kind == CValueKind::Map
            && type.arguments.front() != string_type) {
            throw_backend_error(location,
                "Map currently supports string keys only in the C backend");
        }
        for (const auto& argument : type.arguments) {
            if (argument.kind == CValueKind::Interface) {
                throw_backend_error(location,
                    "interface-valued collection elements are not supported by the C backend");
            }
            if (argument.kind == CValueKind::Array) {
                throw_backend_error(location,
                    "nested Array values are not supported by the C backend");
            }
        }
        if (std::ranges::none_of(collection_order_, [&](const CValueType& value) {
                return value == type;
            })) {
            collection_order_.push_back(type);
        }
    }

    void register_result(const CValueType& type, SourceLocation location)
    {
        if (type.kind != CValueKind::Result || type.arguments.size() != 2) {
            throw_backend_error(location,
                "invalid concrete Result type reached the C backend");
        }
        if (type.nullable) {
            throw_backend_error(location,
                "nullable Result values are not supported by the C backend");
        }
        if (contains_class_reference(type)) {
            throw_backend_error(location,
                "class-reference Result payloads are not supported by the C backend");
        }
        if (contains_interface_value(type)) {
            throw_backend_error(location,
                "interface-valued Result payloads are not supported by the C backend");
        }
        if (std::ranges::none_of(
                result_order_,
                [&](const ResultInfo& existing) { return existing.type == type; })) {
            result_order_.push_back(ResultInfo{type, location});
        }
    }

    CValueType lower_type(const TypeReference& type)
    {
        if (type.arguments.empty()) {
            if (const auto substitution = current_substitutions_.find(type.name);
                substitution != current_substitutions_.end()) {
                CValueType result = substitution->second;
                if (type.nullable) {
                    if (result.kind != CValueKind::Class) {
                        throw_backend_error(
                            type.location,
                            "nullable generic substitution is not a class reference");
                    }
                    result.nullable = true;
                }
                return result;
            }
        }
        if (type.nullable) {
            if (const auto found = class_templates_.find(type.name);
                found != class_templates_.end()) {
                std::vector<CValueType> arguments;
                for (const auto& argument : type.arguments) {
                    arguments.push_back(lower_type(argument));
                }
                auto result = instantiate_class(*found->second, arguments);
                result.nullable = true;
                return result;
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
            register_result(result, type.location);
            return result;
        }
        if (type.name == "Array" || type.name == "List" || type.name == "Map") {
            const std::size_t expected = type.name == "Map" ? 2U : 1U;
            if (type.arguments.size() != expected) {
                throw_backend_error(type.location, type.name + " requires "
                    + std::to_string(expected) + " type arguments");
            }
            std::vector<CValueType> arguments;
            for (const auto& argument : type.arguments) {
                arguments.push_back(lower_type(argument));
            }
            const CValueKind kind = type.name == "Array" ? CValueKind::Array
                : type.name == "List" ? CValueKind::List : CValueKind::Map;
            CValueType result{kind, {}, std::move(arguments)};
            register_collection(result, type.location);
            return result;
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
        if (const auto found = struct_templates_.find(type.name);
            found != struct_templates_.end()) {
            std::vector<CValueType> arguments;
            for (const auto& argument : type.arguments) {
                arguments.push_back(lower_type(argument));
            }
            if (arguments.empty() && !found->second->generic_parameters.empty()) {
                for (const auto& parameter : found->second->generic_parameters) {
                    if (const auto value = current_substitutions_.find(parameter.name);
                        value != current_substitutions_.end()) {
                        arguments.push_back(value->second);
                    }
                }
            }
            return instantiate_struct(*found->second, arguments);
        }
        if (const auto found = class_templates_.find(type.name);
            found != class_templates_.end()) {
            std::vector<CValueType> arguments;
            for (const auto& argument : type.arguments) {
                arguments.push_back(lower_type(argument));
            }
            if (arguments.empty() && !found->second->generic_parameters.empty()) {
                for (const auto& parameter : found->second->generic_parameters) {
                    if (const auto value = current_substitutions_.find(parameter.name);
                        value != current_substitutions_.end()) {
                        arguments.push_back(value->second);
                    }
                }
            }
            return instantiate_class(*found->second, arguments);
        }
        if (interfaces_.contains(type.name)) {
            return CValueType{CValueKind::Interface, type.name};
        }
        if (enums_.contains(type.name)) {
            return CValueType{CValueKind::Enum, type.name};
        }
        throw_backend_error(
            type.location,
            "type '" + type.name + "' is not supported by the C backend");
    }

    CValueType lower_type(const Type& type)
    {
        if (type.deferred && type.arguments.empty()) {
            if (const auto found = current_substitutions_.find(type.name);
                found != current_substitutions_.end()) {
                return found->second;
            }
        }
        switch (type.kind) {
        case TypeKind::Int: return int_type;
        case TypeKind::Dec: return dec_type;
        case TypeKind::String: return string_type;
        case TypeKind::Bool: return bool_type;
        case TypeKind::Void: return void_type;
        case TypeKind::Null:
            throw_backend_error(current_location_, "cannot lower an untyped null value");
        case TypeKind::Unknown:
            break;
        }
        std::vector<CValueType> arguments;
        arguments.reserve(type.arguments.size());
        for (const auto& argument : type.arguments) {
            arguments.push_back(lower_type(argument));
        }
        if (type.name == "Result") {
            CValueType result{CValueKind::Result, {}, arguments, type.nullable};
            register_result(result, current_location_);
            return result;
        }
        if (type.name == "Array" || type.name == "List" || type.name == "Map") {
            const CValueKind kind = type.name == "Array" ? CValueKind::Array
                : type.name == "List" ? CValueKind::List : CValueKind::Map;
            CValueType result{kind, {}, arguments, type.nullable};
            register_collection(result, current_location_);
            return result;
        }
        if (const auto found = struct_templates_.find(type.name);
            found != struct_templates_.end()) {
            if (arguments.empty() && !found->second->generic_parameters.empty()) {
                for (const auto& parameter : found->second->generic_parameters) {
                    if (const auto value = current_substitutions_.find(parameter.name);
                        value != current_substitutions_.end()) {
                        arguments.push_back(value->second);
                    }
                }
            }
            return instantiate_struct(*found->second, arguments);
        }
        if (const auto found = class_templates_.find(type.name);
            found != class_templates_.end()) {
            if (arguments.empty() && !found->second->generic_parameters.empty()) {
                for (const auto& parameter : found->second->generic_parameters) {
                    if (const auto value = current_substitutions_.find(parameter.name);
                        value != current_substitutions_.end()) {
                        arguments.push_back(value->second);
                    }
                }
            }
            auto result = instantiate_class(*found->second, arguments);
            result.nullable = type.nullable;
            return result;
        }
        if (interfaces_.contains(type.name)) {
            return CValueType{CValueKind::Interface, type.name, arguments, type.nullable};
        }
        if (enums_.contains(type.name)) {
            return CValueType{CValueKind::Enum, type.name, arguments, type.nullable};
        }
        throw_backend_error(
            current_location_,
            "type '" + type_name(type) + "' is not supported by the C backend");
    }

    void discover_expression(const Expr& expression)
    {
        if (expression.resolved_type
            && expression.resolved_type->kind != TypeKind::Null
            && (!expression.resolved_type->deferred
                || current_substitutions_.contains(
                    expression.resolved_type->name))) {
            static_cast<void>(lower_type(*expression.resolved_type));
        }
        switch (expression.kind) {
        case ExprKind::Unary:
            discover_expression(*static_cast<const UnaryExpr&>(expression).operand);
            return;
        case ExprKind::Binary: {
            const auto& binary = static_cast<const BinaryExpr&>(expression);
            discover_expression(*binary.left);
            discover_expression(*binary.right);
            return;
        }
        case ExprKind::Call: {
            const auto& call = static_cast<const CallExpr&>(expression);
            discover_expression(*call.callee);
            for (const auto& argument : call.arguments) {
                discover_expression(*argument.value);
            }
            if (call.callee->kind == ExprKind::Identifier) {
                const auto& callee =
                    static_cast<const IdentifierExpr&>(*call.callee);
                if (call.resolved_function
                    && !call.resolved_function->generic_parameters.empty()) {
                    std::vector<CValueType> arguments;
                    for (const auto& parameter :
                         call.resolved_function->generic_parameters) {
                        const auto substitution = std::ranges::find_if(
                            call.resolved_substitutions,
                            [&](const auto& value) {
                                return value.first == parameter.name;
                            });
                        if (substitution == call.resolved_substitutions.end()) {
                            throw_backend_error(
                                current_location_,
                                "missing resolved generic argument for function '"
                                    + callee.name + "'");
                        }
                        arguments.push_back(lower_type(substitution->second));
                    }
                    static_cast<void>(instantiate_function(
                        *call.resolved_function, arguments));
                }
            } else if (call.callee->kind == ExprKind::MemberAccess) {
                const auto& member =
                    static_cast<const MemberAccessExpr&>(*call.callee);
                if (member.object->resolved_type) {
                    const CValueType owner_type =
                        lower_type(*member.object->resolved_type);
                    if (owner_type.kind == CValueKind::Struct
                        || owner_type.kind == CValueKind::Class) {
                        const MethodDeclaration* method = call.resolved_method;
                        if (method && !method->generic_parameters.empty()) {
                            std::vector<CValueType> arguments;
                            for (const auto& parameter : method->generic_parameters) {
                                const auto substitution = std::ranges::find_if(
                                    call.resolved_substitutions,
                                    [&](const auto& value) {
                                        return value.first == parameter.name;
                                    });
                                if (substitution == call.resolved_substitutions.end()) {
                                    throw_backend_error(
                                        current_location_,
                                        "missing resolved generic argument for method '"
                                            + member.member + "'");
                                }
                                arguments.push_back(lower_type(substitution->second));
                            }
                            std::string method_owner = owner_type.nominal_name;
                            if (owner_type.kind == CValueKind::Class) {
                                const auto found = find_class_method_owner(
                                    owner_type.nominal_name, *method);
                                if (!found) {
                                    throw_backend_error(current_location_,
                                        "missing selected method declaration for '"
                                            + member.member + "'");
                                }
                                method_owner = *found;
                            }
                            static_cast<void>(instantiate_method(
                                method_owner, owner_type.kind, *method, arguments));
                        }
                    }
                }
            }
            return;
        }
        case ExprKind::MemberAccess:
            discover_expression(
                *static_cast<const MemberAccessExpr&>(expression).object);
            return;
        case ExprKind::Index: {
            const auto& index = static_cast<const IndexExpr&>(expression);
            discover_expression(*index.object);
            discover_expression(*index.index);
            return;
        }
        case ExprKind::Propagation:
            discover_expression(
                *static_cast<const PropagationExpr&>(expression).expression);
            return;
        case ExprKind::Cast:
            discover_expression(*static_cast<const CastExpr&>(expression).expression);
            return;
        case ExprKind::Grouping:
            discover_expression(
                *static_cast<const GroupingExpr&>(expression).expression);
            return;
        case ExprKind::Integer:
        case ExprKind::Decimal:
        case ExprKind::String:
        case ExprKind::Bool:
        case ExprKind::Null:
        case ExprKind::Identifier:
        case ExprKind::TypeAccess:
            return;
        }
    }

    void discover_statement(const Stmt& statement)
    {
        current_location_ = statement.location;
        switch (statement.kind) {
        case StmtKind::VariableDeclaration: {
            const auto& declaration =
                static_cast<const VariableDeclarationStmt&>(statement);
            if (declaration.explicit_type) {
                static_cast<void>(lower_type(*declaration.explicit_type));
            }
            discover_expression(*declaration.initializer);
            return;
        }
        case StmtKind::Assignment:
            discover_expression(*static_cast<const AssignmentStmt&>(statement).value);
            return;
        case StmtKind::MemberAssignment: {
            const auto& assignment =
                static_cast<const MemberAssignmentStmt&>(statement);
            discover_expression(*assignment.target);
            discover_expression(*assignment.value);
            return;
        }
        case StmtKind::IndexAssignment: {
            const auto& assignment =
                static_cast<const IndexAssignmentStmt&>(statement);
            discover_expression(*assignment.target);
            discover_expression(*assignment.value);
            return;
        }
        case StmtKind::Expression:
            discover_expression(
                *static_cast<const ExpressionStmt&>(statement).expression);
            return;
        case StmtKind::Return: {
            const auto& result = static_cast<const ReturnStmt&>(statement);
            if (result.value) {
                discover_expression(*result.value);
            }
            return;
        }
        case StmtKind::Block:
            for (const auto& nested :
                 static_cast<const BlockStmt&>(statement).statements) {
                discover_statement(*nested);
            }
            return;
        case StmtKind::If: {
            const auto& conditional = static_cast<const IfStmt&>(statement);
            discover_expression(*conditional.condition);
            discover_statement(*conditional.then_block);
            if (conditional.else_branch) {
                discover_statement(*conditional.else_branch);
            }
            return;
        }
        case StmtKind::While: {
            const auto& loop = static_cast<const WhileStmt&>(statement);
            discover_expression(*loop.condition);
            discover_statement(*loop.body);
            return;
        }
        case StmtKind::ForIn: {
            const auto& loop = static_cast<const ForInStmt&>(statement);
            discover_expression(*loop.collection);
            discover_statement(*loop.body);
            return;
        }
        case StmtKind::Handle: {
            const auto& handle = static_cast<const HandleStmt&>(statement);
            discover_expression(*handle.expression);
            for (const auto& handle_case : handle.cases) {
                discover_statement(*handle_case.body);
            }
            return;
        }
        case StmtKind::FunctionDeclaration:
        case StmtKind::Stop:
        case StmtKind::Continue:
        case StmtKind::EnumDeclaration:
        case StmtKind::StructDeclaration:
        case StmtKind::ClassDeclaration:
        case StmtKind::InterfaceDeclaration:
            return;
        }
    }

    void collect_local_types(const Program&)
    {
        for (std::size_t index = 0; index < function_order_.size(); ++index) {
            const std::string key = function_order_[index];
            const auto* declaration = functions_.at(key).declaration;
            current_substitutions_ = functions_.at(key).substitutions;
            discover_statement(*declaration->body);
        }
        for (std::size_t index = 0; index < struct_order_.size(); ++index) {
            const std::string key = struct_order_[index];
            const auto* declaration = structs_.at(key).declaration;
            current_substitutions_ = structs_.at(key).substitutions;
            for (const auto& field : declaration->fields) {
                if (field.default_value) {
                    discover_expression(*field.default_value);
                }
            }
            for (const auto& method : declaration->methods) {
                const auto& declaration =
                    static_cast<const MethodDeclaration&>(*method);
                if (declaration.body && declaration.generic_parameters.empty()) {
                    discover_statement(*declaration.body);
                }
            }
            for (const auto& conversion : declaration->conversions) {
                discover_statement(*conversion->body);
            }
        }
        for (std::size_t index = 0; index < class_order_.size(); ++index) {
            const std::string key = class_order_[index];
            const auto* declaration = classes_.at(key).declaration;
            current_substitutions_ = classes_.at(key).substitutions;
            for (const auto& member : declaration->members) {
                if (member->kind == ClassMemberKind::Field) {
                    const auto& field = static_cast<const ClassField&>(*member);
                    if (field.default_value) {
                        discover_expression(*field.default_value);
                    }
                    continue;
                }
                if (member->kind != ClassMemberKind::Method) {
                    if (member->kind == ClassMemberKind::Conversion) {
                        discover_statement(
                            *static_cast<const ConversionOverload&>(*member).body);
                    }
                    continue;
                }
                const auto& declaration =
                    static_cast<const MethodDeclaration&>(*member);
                if (declaration.body && declaration.generic_parameters.empty()) {
                    discover_statement(*declaration.body);
                }
            }
        }
        current_substitutions_.clear();
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
        case StmtKind::IndexAssignment:
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

    std::string collection_element_retain(
        const CValueType& type,
        std::string_view expression) const
    {
        if (type.kind == CValueKind::Class) {
            return retain_name(type.nominal_name) + "(" + std::string(expression) + ");\n";
        }
        if (type.kind == CValueKind::List || type.kind == CValueKind::Map) {
            return collection_name(type) + "_retain(" + std::string(expression) + ");\n";
        }
        return {};
    }

    std::string collection_element_release(
        const CValueType& type,
        std::string_view expression) const
    {
        if (type.kind == CValueKind::Class) {
            return release_name(type.nominal_name) + "(" + std::string(expression) + ");\n";
        }
        if (type.kind == CValueKind::List || type.kind == CValueKind::Map) {
            return collection_name(type) + "_release(" + std::string(expression) + ");\n";
        }
        return {};
    }

    std::string emit_collection_definition(const CValueType& type) const
    {
        const std::string name = collection_name(type);
        if (type.kind == CValueKind::Array) {
            const auto& element = type.arguments[0];
            std::string output = "struct " + name + "\n{\n"
                "    size_t toro_count;\n    " + c_type_name(element)
                + "* toro_data;\n};\n\n";
            output += "static " + name + " " + name
                + "_new(size_t toro_count)\n{\n    " + name
                + " toro_value;\n    toro_value.toro_count = toro_count;\n"
                  "    toro_value.toro_data = toro_count == 0 ? NULL : calloc(toro_count, sizeof(*toro_value.toro_data));\n"
                  "    if (toro_count != 0 && toro_value.toro_data == NULL) { abort(); }\n"
                  "    return toro_value;\n}\n\n";
            output += "static void " + name + "_destroy(" + name
                + "* toro_value)\n{\n    if (toro_value->toro_data == NULL) { return; }\n";
            if (const auto release = collection_element_release(
                    element, "toro_value->toro_data[toro_index]"); !release.empty()) {
                output += "    for (size_t toro_index = 0; toro_index < toro_value->toro_count; ++toro_index)\n    {\n        "
                    + release + "    }\n";
            }
            output += "    free(toro_value->toro_data);\n    toro_value->toro_data = NULL;\n    toro_value->toro_count = 0;\n}\n\n";
            output += "static " + name + " " + name + "_clone(" + name
                + " toro_source)\n{\n    " + name + " toro_copy = " + name
                + "_new(toro_source.toro_count);\n    for (size_t toro_index = 0; toro_index < toro_source.toro_count; ++toro_index)\n    {\n"
                  "        toro_copy.toro_data[toro_index] = toro_source.toro_data[toro_index];\n";
            if (const auto retain = collection_element_retain(
                    element, "toro_copy.toro_data[toro_index]"); !retain.empty()) {
                output += "        " + retain;
            }
            output += "    }\n    return toro_copy;\n}\n\n";
            output += "static " + c_type_name(element) + " " + name
                + "_get(" + name + " toro_value, int64_t toro_index)\n{\n"
                  "    if (toro_index < 0 || (size_t)toro_index >= toro_value.toro_count) { abort(); }\n"
                  "    return toro_value.toro_data[toro_index];\n}\n\n";
            output += "static void " + name + "_set(" + name
                + "* toro_value, int64_t toro_index, " + c_type_name(element)
                + " toro_element)\n{\n"
                  "    if (toro_index < 0 || (size_t)toro_index >= toro_value->toro_count) { abort(); }\n";
            output += "    " + collection_element_retain(element, "toro_element");
            output += "    " + collection_element_release(
                element, "toro_value->toro_data[toro_index]");
            output += "    toro_value->toro_data[toro_index] = toro_element;\n}\n\n";
            return output;
        }

        if (type.kind == CValueKind::List) {
            const auto& element = type.arguments[0];
            std::string output = "struct " + name + "\n{\n"
                "    uint64_t toro_ref_count;\n    size_t toro_count;\n"
                "    size_t toro_capacity;\n    " + c_type_name(element)
                + "* toro_data;\n};\n\n";
            output += "static " + name + "* " + name + "_new(void)\n{\n    "
                + name + "* toro_value = calloc(1, sizeof(*toro_value));\n"
                  "    if (toro_value == NULL) { abort(); }\n    toro_value->toro_ref_count = 1;\n    return toro_value;\n}\n\n";
            output += "static void " + name + "_retain(" + name
                + "* toro_value)\n{\n    if (toro_value != NULL) { ++toro_value->toro_ref_count; }\n}\n\n";
            output += "static void " + name + "_release(" + name
                + "* toro_value)\n{\n    if (toro_value == NULL || --toro_value->toro_ref_count != 0) { return; }\n";
            if (const auto release = collection_element_release(
                    element, "toro_value->toro_data[toro_index]"); !release.empty()) {
                output += "    for (size_t toro_index = 0; toro_index < toro_value->toro_count; ++toro_index)\n    {\n        "
                    + release + "    }\n";
            }
            output += "    free(toro_value->toro_data);\n    free(toro_value);\n}\n\n";
            output += "static void " + name + "_add(" + name + "* toro_value, "
                + c_type_name(element) + " toro_element)\n{\n"
                  "    if (toro_value->toro_count == toro_value->toro_capacity)\n    {\n"
                  "        size_t toro_capacity = toro_value->toro_capacity == 0 ? 4 : toro_value->toro_capacity * 2;\n"
                  "        void* toro_data = realloc(toro_value->toro_data, toro_capacity * sizeof(*toro_value->toro_data));\n"
                  "        if (toro_data == NULL) { abort(); }\n        toro_value->toro_data = toro_data;\n        toro_value->toro_capacity = toro_capacity;\n    }\n";
            output += "    " + collection_element_retain(element, "toro_element");
            output += "    toro_value->toro_data[toro_value->toro_count++] = toro_element;\n}\n\n";
            output += "static " + c_type_name(element) + " " + name
                + "_get(" + name + "* toro_value, int64_t toro_index)\n{\n"
                  "    if (toro_index < 0 || (size_t)toro_index >= toro_value->toro_count) { abort(); }\n"
                  "    return toro_value->toro_data[toro_index];\n}\n\n";
            output += "static void " + name + "_set(" + name
                + "* toro_value, int64_t toro_index, " + c_type_name(element)
                + " toro_element)\n{\n"
                  "    if (toro_index < 0 || (size_t)toro_index >= toro_value->toro_count) { abort(); }\n";
            output += "    " + collection_element_retain(element, "toro_element");
            output += "    " + collection_element_release(
                element, "toro_value->toro_data[toro_index]");
            output += "    toro_value->toro_data[toro_index] = toro_element;\n}\n\n";
            output += "static " + name + "* " + name + "_clone(" + name
                + "* toro_source)\n{\n    " + name + "* toro_copy = " + name
                + "_new();\n    for (size_t toro_index = 0; toro_index < toro_source->toro_count; ++toro_index)\n    {\n        "
                + name + "_add(toro_copy, toro_source->toro_data[toro_index]);\n    }\n    return toro_copy;\n}\n\n";
            return output;
        }

        const auto& value = type.arguments[1];
        std::string output = "struct " + name + "\n{\n"
            "    uint64_t toro_ref_count;\n    size_t toro_count;\n    size_t toro_capacity;\n"
            "    const char** toro_keys;\n    " + c_type_name(value)
            + "* toro_values;\n    bool* toro_occupied;\n};\n\n";
        output += "static uint64_t " + name + "_hash(const char* toro_key)\n{\n"
            "    uint64_t toro_hash = UINT64_C(1469598103934665603);\n"
            "    while (*toro_key != '\\0') { toro_hash ^= (unsigned char)*toro_key++; toro_hash *= UINT64_C(1099511628211); }\n"
            "    return toro_hash;\n}\n\n";
        output += "static " + name + "* " + name + "_new(void)\n{\n    "
            + name + "* toro_value = calloc(1, sizeof(*toro_value));\n"
              "    if (toro_value == NULL) { abort(); }\n    toro_value->toro_ref_count = 1;\n"
              "    toro_value->toro_capacity = 8;\n"
              "    toro_value->toro_keys = calloc(toro_value->toro_capacity, sizeof(*toro_value->toro_keys));\n"
              "    toro_value->toro_values = calloc(toro_value->toro_capacity, sizeof(*toro_value->toro_values));\n"
              "    toro_value->toro_occupied = calloc(toro_value->toro_capacity, sizeof(*toro_value->toro_occupied));\n"
              "    if (toro_value->toro_keys == NULL || toro_value->toro_values == NULL || toro_value->toro_occupied == NULL) { abort(); }\n"
              "    return toro_value;\n}\n\n";
        output += "static void " + name + "_retain(" + name
            + "* toro_value)\n{\n    if (toro_value != NULL) { ++toro_value->toro_ref_count; }\n}\n\n";
        output += "static void " + name + "_release(" + name
            + "* toro_value)\n{\n    if (toro_value == NULL || --toro_value->toro_ref_count != 0) { return; }\n";
        if (const auto release = collection_element_release(
                value, "toro_value->toro_values[toro_index]"); !release.empty()) {
            output += "    for (size_t toro_index = 0; toro_index < toro_value->toro_capacity; ++toro_index)\n    {\n        if (toro_value->toro_occupied[toro_index]) { "
                + release + "        }\n    }\n";
        }
        output += "    free(toro_value->toro_keys);\n    free(toro_value->toro_values);\n    free(toro_value->toro_occupied);\n    free(toro_value);\n}\n\n";
        output += "static size_t " + name + "_slot(" + name
            + "* toro_value, const char* toro_key)\n{\n    size_t toro_index = (size_t)("
            + name + "_hash(toro_key) % toro_value->toro_capacity);\n"
              "    while (toro_value->toro_occupied[toro_index] && strcmp(toro_value->toro_keys[toro_index], toro_key) != 0) { toro_index = (toro_index + 1) % toro_value->toro_capacity; }\n"
              "    return toro_index;\n}\n\n";
        output += "static void " + name + "_set(" + name
            + "* toro_value, const char* toro_key, " + c_type_name(value)
            + " toro_element);\n\n";
        output += "static void " + name + "_grow(" + name + "* toro_value)\n{\n"
            "    size_t toro_old_capacity = toro_value->toro_capacity;\n"
            "    const char** toro_old_keys = toro_value->toro_keys;\n"
            "    " + c_type_name(value) + "* toro_old_values = toro_value->toro_values;\n"
            "    bool* toro_old_occupied = toro_value->toro_occupied;\n"
            "    toro_value->toro_capacity *= 2; toro_value->toro_count = 0;\n"
            "    toro_value->toro_keys = calloc(toro_value->toro_capacity, sizeof(*toro_value->toro_keys));\n"
            "    toro_value->toro_values = calloc(toro_value->toro_capacity, sizeof(*toro_value->toro_values));\n"
            "    toro_value->toro_occupied = calloc(toro_value->toro_capacity, sizeof(*toro_value->toro_occupied));\n"
            "    if (toro_value->toro_keys == NULL || toro_value->toro_values == NULL || toro_value->toro_occupied == NULL) { abort(); }\n"
            "    for (size_t toro_index = 0; toro_index < toro_old_capacity; ++toro_index)\n    {\n"
            "        if (!toro_old_occupied[toro_index]) { continue; }\n"
            "        size_t toro_slot = " + name + "_slot(toro_value, toro_old_keys[toro_index]);\n"
            "        toro_value->toro_occupied[toro_slot] = true; toro_value->toro_keys[toro_slot] = toro_old_keys[toro_index]; toro_value->toro_values[toro_slot] = toro_old_values[toro_index]; ++toro_value->toro_count;\n"
            "    }\n    free(toro_old_keys); free(toro_old_values); free(toro_old_occupied);\n}\n\n";
        output += "static void " + name + "_set(" + name
            + "* toro_value, const char* toro_key, " + c_type_name(value)
            + " toro_element)\n{\n    if ((toro_value->toro_count + 1) * 10 >= toro_value->toro_capacity * 7) { "
            + name + "_grow(toro_value); }\n    size_t toro_slot = " + name
            + "_slot(toro_value, toro_key);\n";
        output += "    " + collection_element_retain(value, "toro_element");
        output += "    if (toro_value->toro_occupied[toro_slot]) { "
            + collection_element_release(value, "toro_value->toro_values[toro_slot]")
            + "    } else { toro_value->toro_occupied[toro_slot] = true; toro_value->toro_keys[toro_slot] = toro_key; ++toro_value->toro_count; }\n"
            "    toro_value->toro_values[toro_slot] = toro_element;\n}\n\n";
        output += "static bool " + name + "_contains(" + name
            + "* toro_value, const char* toro_key)\n{\n    size_t toro_slot = "
            + name + "_slot(toro_value, toro_key);\n    return toro_value->toro_occupied[toro_slot];\n}\n\n";
        output += "static " + c_type_name(value) + " " + name
            + "_get(" + name + "* toro_value, const char* toro_key)\n{\n    size_t toro_slot = "
            + name + "_slot(toro_value, toro_key);\n    if (!toro_value->toro_occupied[toro_slot]) { abort(); }\n    return toro_value->toro_values[toro_slot];\n}\n\n";
        output += "static " + name + "* " + name + "_clone(" + name
            + "* toro_source)\n{\n    " + name + "* toro_copy = " + name
            + "_new();\n    for (size_t toro_index = 0; toro_index < toro_source->toro_capacity; ++toro_index)\n    {\n        if (toro_source->toro_occupied[toro_index]) { "
            + name + "_set(toro_copy, toro_source->toro_keys[toro_index], toro_source->toro_values[toro_index]); }\n    }\n    return toro_copy;\n}\n\n";
        return output;
    }

    std::string emit_vtable_definition(const std::string& root) const
    {
        const auto slots = virtual_slots(root);
        if (slots.empty()) {
            return {};
        }
        std::string output = "struct " + vtable_name(root) + "\n{\n";
        for (const auto& slot : slots) {
            output += "    " + c_type_name(slot.declaration->return_type)
                + " (*" + virtual_slot_name(
                    slot.declaration_owner, slot.declaration->key)
                + ")(void* toro_object";
            for (std::size_t index = 0;
                 index < slot.declaration->parameter_types.size(); ++index) {
                output += ", "
                    + c_type_name(slot.declaration->parameter_types[index])
                    + " " + parameter_name(
                        slot.declaration->declaration->parameters[index].name);
            }
            output += ");\n";
        }
        output += "};\n\n";
        for (const auto& key : class_order_) {
            const auto& info = classes_.at(key);
            if (!info.declaration->is_abstract
                && class_root(key) == root) {
                output += "static const " + vtable_name(root) + " "
                    + vtable_instance_name(key) + ";\n";
            }
        }
        output += '\n';
        return output;
    }

    std::string emit_virtual_thunks(const std::string& concrete) const
    {
        const auto& concrete_info = classes_.at(concrete);
        const auto slots = virtual_slots(class_root(concrete));
        std::string output;
        for (const auto& slot : slots) {
            if (!is_class_base_of(slot.declaration_owner, concrete)) {
                continue;
            }
            const auto implementation = find_class_method(
                concrete, slot.name, slot.declaration->parameter_types);
            if (!implementation || !implementation->second->declaration->body) {
                throw_backend_error(
                    concrete_info.declaration->location,
                    "concrete class '" + concrete_info.declaration->name
                        + "' has no runtime implementation for virtual method '"
                        + slot.name + "'");
            }
            output += "static "
                + c_type_name(slot.declaration->return_type) + " "
                + virtual_thunk_name(concrete, slot.declaration->key)
                + "(void* toro_object";
            for (std::size_t index = 0;
                 index < slot.declaration->parameter_types.size(); ++index) {
                output += ", "
                    + c_type_name(slot.declaration->parameter_types[index])
                    + " " + parameter_name(
                        slot.declaration->declaration->parameters[index].name);
            }
            output += ")\n{\n";
            const std::string receiver = class_upcast(
                "(" + class_name(concrete) + "*)toro_object",
                concrete,
                implementation->first);
            output += "    ";
            if (slot.declaration->return_type != void_type) {
                output += "return ";
            }
            output += method_name(
                implementation->first, implementation->second->key) + "("
                + receiver;
            for (const auto& parameter :
                 slot.declaration->declaration->parameters) {
                output += ", " + parameter_name(parameter.name);
            }
            output += ");\n}\n\n";
        }
        return output;
    }

    std::string emit_vtable_instance(const std::string& concrete) const
    {
        const std::string root = class_root(concrete);
        const auto slots = virtual_slots(root);
        if (slots.empty()) {
            return {};
        }
        std::string output = "static const " + vtable_name(root) + " "
            + vtable_instance_name(concrete) + " =\n{\n";
        for (const auto& slot : slots) {
            output += "    ." + virtual_slot_name(
                slot.declaration_owner, slot.declaration->key) + " = ";
            if (is_class_base_of(slot.declaration_owner, concrete)) {
                output += virtual_thunk_name(
                    concrete, slot.declaration->key);
            } else {
                output += "NULL";
            }
            output += ",\n";
        }
        output += "};\n\n";
        return output;
    }

    std::string emit_interface_definition(
        const InterfaceDeclarationStmt& declaration) const
    {
        const auto& info = interfaces_.at(declaration.name);
        const std::string value_type = interface_name(declaration.name);
        const std::string table_type = interface_vtable_name(declaration.name);
        std::string output = "struct " + value_type + "\n{\n";
        output += "    const " + table_type + "* toro_vtable;\n";
        output += "    void* toro_object;\n";
        output += "    void (*toro_retain)(void*);\n";
        output += "    void (*toro_release)(void*);\n";
        output += "    union\n    {\n";
        bool has_struct_implementer = false;
        for (const auto& key : struct_order_) {
            const auto& implementer = structs_.at(key);
            const bool implements = std::ranges::any_of(
                implementer.declaration->interfaces,
                [&](const TypeReference& candidate) {
                    return candidate.name == declaration.name;
                });
            if (!implements) {
                continue;
            }
            has_struct_implementer = true;
            output += "        " + struct_name(key) + " "
                + interface_struct_storage_name(key) + ";\n";
        }
        if (!has_struct_implementer) {
            output += "        uint8_t toro_empty;\n";
        }
        output += "    } toro_value;\n";
        output += "};\n\n";

        output += "struct " + table_type + "\n{\n";
        if (info.methods.empty()) {
            output += "    uint8_t toro_empty;\n";
        }
        for (const auto& method : info.methods) {
            output += "    " + c_type_name(method.return_type) + " (*"
                + interface_slot_name(declaration.name, method.key)
                + ")(" + value_type + "* toro_interface_self";
            for (std::size_t index = 0;
                 index < method.parameter_types.size(); ++index) {
                output += ", " + c_type_name(method.parameter_types[index]) + " "
                    + parameter_name(method.declaration->parameters[index].name);
            }
            output += ");\n";
        }
        output += "};\n\n";
        output += "static void " + value_type + "_retain(" + value_type
            + "* toro_value)\n{\n"
            + "    if (toro_value->toro_retain != NULL) { "
            + "toro_value->toro_retain(toro_value->toro_object); }\n"
            + "}\n\n";
        output += "static void " + value_type + "_release(" + value_type
            + "* toro_value)\n{\n"
            + "    if (toro_value->toro_release != NULL) { "
            + "toro_value->toro_release(toro_value->toro_object); }\n"
            + "}\n\n";
        return output;
    }

    std::string emit_interface_thunks(
        const std::string& interface,
        const std::string& implementer,
        CValueKind implementer_kind) const
    {
        const auto& info = interfaces_.at(interface);
        const std::string value_type = interface_name(interface);
        std::string output;
        if (implementer_kind == CValueKind::Class) {
            output += "static void "
                + interface_class_retain_name(interface, implementer)
                + "(void* toro_object)\n{\n    " + retain_name(implementer)
                + "((" + class_name(implementer) + "*)toro_object);\n}\n\n";
            output += "static void "
                + interface_class_release_name(interface, implementer)
                + "(void* toro_object)\n{\n    " + release_name(implementer)
                + "((" + class_name(implementer) + "*)toro_object);\n}\n\n";
        }
        for (const auto& requirement : info.methods) {
            output += "static " + c_type_name(requirement.return_type) + " "
                + interface_thunk_name(
                    interface, implementer, requirement.key)
                + "(" + value_type + "* toro_interface_self";
            for (std::size_t index = 0;
                 index < requirement.parameter_types.size(); ++index) {
                output += ", " + c_type_name(requirement.parameter_types[index])
                    + " " + parameter_name(
                        requirement.declaration->parameters[index].name);
            }
            output += ")\n{\n    ";
            if (requirement.return_type != void_type) {
                output += "return ";
            }
            if (implementer_kind == CValueKind::Struct) {
                const auto& methods = structs_.at(implementer).methods;
                const auto method = std::ranges::find_if(
                    methods,
                    [&](const auto& entry) {
                        return entry.second.declaration->name
                                == requirement.declaration->name
                            && entry.second.parameter_types
                                == requirement.parameter_types;
                    });
                if (method == methods.end()) {
                    throw_backend_error(
                        requirement.declaration->location,
                        "missing runtime struct interface implementation");
                }
                output += method_name(implementer, method->second.key)
                    + "(&toro_interface_self->toro_value."
                    + interface_struct_storage_name(implementer);
            } else {
                const auto implementation = find_class_method(
                    implementer,
                    requirement.declaration->name,
                    requirement.parameter_types);
                if (!implementation) {
                    throw_backend_error(
                        requirement.declaration->location,
                        "missing runtime class interface implementation");
                }
                std::string receiver = "(" + class_name(implementer)
                    + "*)toro_interface_self->toro_object";
                const auto slot = find_virtual_slot(
                    implementer, *implementation->second);
                if (slot) {
                    const std::string table = class_header_access(
                        receiver, implementer, "toro_vtable");
                    const std::string control = class_header_access(
                        receiver, implementer, "toro_weak_control");
                    output += table + "->" + virtual_slot_name(
                        slot->declaration_owner, slot->declaration->key)
                        + "(" + control + "->toro_object";
                } else {
                    receiver = class_upcast(
                        receiver, implementer, implementation->first);
                    output += method_name(
                        implementation->first, implementation->second->key)
                        + "(" + receiver;
                }
            }
            for (const auto& parameter : requirement.declaration->parameters) {
                output += ", " + parameter_name(parameter.name);
            }
            output += ");\n}\n\n";
        }
        return output;
    }

    std::string emit_interface_vtable_instance(
        const std::string& interface,
        const std::string& implementer,
        CValueKind) const
    {
        const auto& info = interfaces_.at(interface);
        std::string output = "static const " + interface_vtable_name(interface)
            + " " + interface_vtable_instance_name(interface, implementer)
            + " =\n{\n";
        if (info.methods.empty()) {
            output += "    .toro_empty = 0,\n";
        }
        for (const auto& method : info.methods) {
            output += "    ." + interface_slot_name(
                interface, method.key) + " = "
                + interface_thunk_name(
                    interface, implementer, method.key) + ",\n";
        }
        output += "};\n\n";
        return output;
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
            const auto fields = effective_class_fields(name);
            if (info.base_name) {
                output += emit_type_definition(
                    CValueType{CValueKind::Class, *info.base_name}, state);
            }
            for (const auto& field : info.fields) {
                if (field.type.kind == CValueKind::Array
                    || field.type.kind == CValueKind::List
                    || field.type.kind == CValueKind::Map) {
                    throw_backend_error(field.declaration->location,
                        "collection-valued class fields are not supported by the C backend");
                }
                if (field.type.kind == CValueKind::Struct
                    || field.type.kind == CValueKind::Enum
                    || field.type.kind == CValueKind::Result) {
                    output += emit_type_definition(field.type, state);
                }
            }
            const std::string object = class_name(name);
            output += "struct " + object + "\n{\n";
            if (info.base_name) {
                output += "    " + class_name(*info.base_name) + " toro_base;\n";
            } else {
                output += "    uint64_t toro_strong_count;\n";
                output += "    toro_weak_control* toro_weak_control;\n";
                output += "    void (*toro_finalize)(void*);\n";
                if (!virtual_slots(name).empty()) {
                    output += "    const " + vtable_name(name)
                        + "* toro_vtable;\n";
                }
            }
            for (const auto& field : info.fields) {
                output += "    "
                    + std::string(field.declaration->is_weak
                            ? "toro_weak_ref"
                            : c_type_name(field.type))
                    + " "
                    + field_name(field.declaration->name) + ";\n";
            }
            output += "};\n\n";

            output += "static void " + finalize_name(name)
                + "(void* toro_raw_value)\n{\n";
            output += "    " + object + "* toro_value = (" + object
                + "*)toro_raw_value;\n";
            output += "    toro_weak_control* toro_control = "
                + class_header_access("toro_value", name, "toro_weak_control")
                + ";\n";
            output += "    toro_control->toro_object = NULL;\n";
            std::string lifecycle_owner = name;
            while (true) {
                const auto& lifecycle_info = classes_.at(lifecycle_owner);
                if (lifecycle_info.methods.contains("destroy")) {
                    output += "    " + method_name(lifecycle_owner, "destroy")
                        + "(" + class_upcast(
                            "toro_value", name, lifecycle_owner) + ");\n";
                }
                if (!lifecycle_info.base_name) {
                    break;
                }
                lifecycle_owner = *lifecycle_info.base_name;
            }
            for (auto field = fields.rbegin(); field != fields.rend(); ++field) {
                const std::string access = class_member_access(
                    "toro_value",
                    name,
                    field->first,
                    field_name(field->second->declaration->name));
                if (field->second->declaration->is_weak) {
                    output += "    toro_weak_clear(&" + access + ");\n";
                } else if (field->second->type.kind == CValueKind::Class) {
                    output += "    " + release_name(
                        field->second->type.nominal_name) + "(" + access + ");\n";
                }
            }
            output += "    if (--toro_control->toro_weak_count == 0) { free(toro_control); }\n";
            output += "    free(toro_value);\n";
            output += "}\n\n";

            output += "static void " + retain_name(name) + "(" + object
                + "* toro_value)\n{\n";
            output += "    if (toro_value != NULL)\n    {\n";
            output += "        if ("
                + class_header_access("toro_value", name, "toro_strong_count")
                + " == 0) { abort(); }\n";
            output += "        ++"
                + class_header_access("toro_value", name, "toro_strong_count")
                + ";\n    }\n";
            output += "}\n\n";
            output += "static void " + release_name(name) + "(" + object
                + "* toro_value)\n{\n";
            output += "    if (toro_value != NULL && --"
                + class_header_access("toro_value", name, "toro_strong_count")
                + " == 0)\n";
            output += "    {\n";
            output += "        "
                + class_header_access("toro_value", name, "toro_finalize")
                + "(" + class_header_access(
                    "toro_value", name, "toro_weak_control")
                + "->toro_object);\n";
            output += "    }\n";
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
            if (field.type.kind == CValueKind::Array
                || field.type.kind == CValueKind::List
                || field.type.kind == CValueKind::Map) {
                throw_backend_error(field.declaration->location,
                    "collection-valued struct fields are not supported by the C backend");
            }
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
            + method_name(owner, info.key) + "("
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

    std::string conversion_declaration_text(
        const std::string& owner,
        CValueKind owner_kind,
        const ConversionInfo& info) const
    {
        return "static " + c_type_name(info.target_type) + " "
            + conversion_name(owner, info.key) + "("
            + c_type_name(CValueType{owner_kind, owner})
            + (owner_kind == CValueKind::Struct ? "*" : "")
            + " toro_self)";
    }

    std::string function_declaration(const std::string& key) const
    {
        const auto& info = functions_.at(key);
        const auto& function = *info.declaration;
        std::string output = "static ";
        output += c_type_name(info.return_type) + " " + function_name(key) + "(";
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

    std::string emit_function(const std::string& key)
    {
        const auto& info = functions_.at(key);
        const auto& function = *info.declaration;
        current_location_ = function.location;
        scopes_.clear();
        owned_reference_values_.clear();
        push_scope();
        current_substitutions_ = info.substitutions;
        current_return_type_ = info.return_type;
        bind_parameters(function.parameters, info.parameter_types);

        std::string output = function_declaration(key) + "\n{\n";
        output += retain_owned_parameters(1);
        output += emit_statement_list(function.body->statements, 1);
        if (!statements_guarantee_return(function.body->statements)) {
            output += scope_cleanup(scopes_.size() - 1, 1);
        }
        output += "}\n";
        pop_scope();
        current_return_type_.reset();
        current_substitutions_.clear();
        return output;
    }

    std::string emit_method(
        const std::string& owner,
        CValueKind owner_kind,
        const MethodDeclaration& method,
        const MethodInfo& info)
    {
        current_location_ = method.location;
        current_substitutions_ = info.substitutions;
        scopes_.clear();
        owned_reference_values_.clear();
        push_scope();
        current_return_type_ = info.return_type;
        scopes_.back().emplace(
            "self",
            ValueInfo{
                CValueType{owner_kind, owner},
                "toro_self",
                owner_kind == CValueKind::Struct,
            });
        bind_parameters(method.parameters, info.parameter_types);

        std::string output = method_declaration_text(
            owner, owner_kind, method, info) + "\n{\n";
        output += retain_owned_parameters(1);
        output += emit_statement_list(method.body->statements, 1);
        if (!statements_guarantee_return(method.body->statements)) {
            output += scope_cleanup(scopes_.size() - 1, 1);
        }
        output += "}\n";
        pop_scope();
        current_return_type_.reset();
        current_substitutions_.clear();
        return output;
    }

    std::string emit_conversion(
        const std::string& owner,
        CValueKind owner_kind,
        const ConversionInfo& info)
    {
        const auto& conversion = *info.declaration;
        current_location_ = conversion.location;
        current_substitutions_ = owner_kind == CValueKind::Struct
            ? structs_.at(owner).substitutions
            : classes_.at(owner).substitutions;
        scopes_.clear();
        owned_reference_values_.clear();
        push_scope();
        current_return_type_ = info.target_type;
        scopes_.back().emplace(
            "self",
            ValueInfo{
                CValueType{owner_kind, owner},
                "toro_self",
                owner_kind == CValueKind::Struct,
            });
        std::string output = conversion_declaration_text(
            owner, owner_kind, info) + "\n{\n";
        output += emit_statement_list(conversion.body->statements, 1);
        output += "}\n";
        pop_scope();
        current_return_type_.reset();
        current_substitutions_.clear();
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
            auto initializer = emit_expression(
                *declaration.initializer, declared_type);
            const CValueType type = declared_type.value_or(initializer.type);
            const std::string c_name = variable_name(declaration.name);
            std::string output = indent_prelude(initializer.prelude, depth);
            if (type.kind == CValueKind::Array && !initializer.owned) {
                initializer.code = collection_name(type) + "_clone("
                    + initializer.code + ")";
                initializer.owned = true;
            }
            output += prefix + c_type_name(type) + " " + c_name + " = "
                + initializer.code + ";\n";
            if (is_owned_runtime_value(type)) {
                if (is_managed_reference(type) && !initializer.owned) {
                    output += prefix + retain_call(type, c_name) + ";\n";
                }
                owned_reference_values_.back().push_back(
                    ValueInfo{type, c_name, false, true});
            }
            scopes_.back().emplace(
                declaration.name,
                ValueInfo{type, c_name, false, is_owned_runtime_value(type)});
            return output;
        }
        case StmtKind::Assignment: {
            const auto& assignment = static_cast<const AssignmentStmt&>(statement);
            const auto& target = find_value(assignment.name);
            auto value = emit_expression(*assignment.value, target.type);
            if (target.type.kind == CValueKind::Array) {
                if (!target.owned_local) {
                    throw_backend_error(statement.location,
                        "cannot reassign borrowed Array value '" + assignment.name
                            + "' in the C backend");
                }
                const std::string temporary =
                    "toro_array_value_" + std::to_string(temporary_index_++);
                std::string output = indent_prelude(value.prelude, depth);
                output += prefix + c_type_name(target.type) + " " + temporary
                    + " = " + (value.owned ? value.code
                        : collection_name(target.type) + "_clone(" + value.code + ")")
                    + ";\n";
                output += prefix + release_call(target.type, target.c_name) + ";\n";
                output += prefix + target.c_name + " = " + temporary + ";\n";
                return output;
            }
            if (is_managed_reference(target.type)) {
                if (!target.owned_local) {
                    throw_backend_error(
                        statement.location,
                        "cannot reassign borrowed reference value '"
                            + assignment.name + "' in the C backend");
                }
                const std::string temporary =
                    "toro_class_value_" + std::to_string(temporary_index_++);
                std::string output = indent_prelude(value.prelude, depth);
                output += prefix + c_type_name(target.type) + " " + temporary
                    + " = " + value.code + ";\n";
                if (!value.owned) {
                    output += prefix + retain_call(target.type, temporary) + ";\n";
                }
                output += prefix + release_call(target.type, target.c_name) + ";\n";
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
        case StmtKind::IndexAssignment:
            return emit_index_assignment(
                static_cast<const IndexAssignmentStmt&>(statement), depth);
        case StmtKind::Expression: {
            const auto& expression = static_cast<const ExpressionStmt&>(statement);
            const auto value = emit_expression(*expression.expression);
            if (is_owned_runtime_value(value.type) && value.owned) {
                const std::string temporary =
                    "toro_unused_class_" + std::to_string(temporary_index_++);
                return indent_prelude(value.prelude, depth)
                    + prefix + c_type_name(value.type) + " " + temporary
                    + " = " + value.code + ";\n"
                    + prefix + release_call(value.type, temporary) + ";\n";
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
            if (is_managed_reference(value.type)) {
                const std::string temporary =
                    "toro_return_class_" + std::to_string(temporary_index_++);
                std::string output = indent_prelude(value.prelude, depth);
                output += prefix + c_type_name(value.type) + " " + temporary
                    + " = " + value.code + ";\n";
                if (!value.owned) {
                    output += prefix + retain_call(value.type, temporary) + ";\n";
                }
                output += all_scope_cleanup(depth);
                output += prefix + "return " + temporary + ";\n";
                return output;
            }
            if (value.type.kind == CValueKind::Array) {
                const std::string temporary =
                    "toro_return_array_" + std::to_string(temporary_index_++);
                std::string output = indent_prelude(value.prelude, depth);
                output += prefix + c_type_name(value.type) + " " + temporary
                    + " = " + (value.owned ? value.code
                        : collection_name(value.type) + "_clone(" + value.code + ")")
                    + ";\n";
                output += all_scope_cleanup(depth);
                output += prefix + "return " + temporary + ";\n";
                return output;
            }
            const bool has_owned_references = std::ranges::any_of(
                owned_reference_values_,
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
            push_scope();
            std::string output;
            if (condition.prelude.empty()) {
                output = prefix + "while (" + condition.code + ")\n"
                    + prefix + "{\n";
            } else {
                output = prefix + "while (true)\n" + prefix + "{\n";
                output += indent_prelude(condition.prelude, depth + 1);
                output += indent(depth + 1) + "if (!(" + condition.code + "))\n";
                output += indent(depth + 1) + "{\n";
                output += indent(depth + 2) + "break;\n";
                output += indent(depth + 1) + "}\n";
            }
            loop_scope_indices_.push_back(scopes_.size() - 1);
            output += emit_statement_list(loop.body->statements, depth + 1);
            loop_scope_indices_.pop_back();
            output += scope_cleanup(scopes_.size() - 1, depth + 1);
            output += prefix + "}\n";
            pop_scope();
            return output;
        }
        case StmtKind::ForIn: {
            const auto& loop = static_cast<const ForInStmt&>(statement);
            const auto collection = emit_expression(*loop.collection);
            if ((collection.type.kind != CValueKind::Array
                    && collection.type.kind != CValueKind::List)
                || collection.type.arguments.size() != 1) {
                throw_backend_error(statement.location,
                    "for ... in lowering requires Array<T> or List<T>");
            }
            push_scope();
            const std::string collection_temporary =
                "toro_for_collection_" + std::to_string(temporary_index_++);
            const std::string index =
                "toro_for_index_" + std::to_string(temporary_index_++);
            std::string output = indent_prelude(collection.prelude, depth);
            output += prefix + c_type_name(collection.type) + " "
                + collection_temporary + " = " + collection.code + ";\n";
            if (collection.owned) {
                owned_reference_values_.back().push_back(ValueInfo{
                    collection.type, collection_temporary, false, true});
            }
            const std::string count = collection.type.kind == CValueKind::Array
                ? collection_temporary + ".toro_count"
                : collection_temporary + "->toro_count";
            output += prefix + "for (size_t " + index + " = 0; " + index
                + " < " + count + "; ++" + index + ")\n" + prefix + "{\n";
            push_scope();
            const std::string variable = variable_name(loop.variable_name);
            const CValueType element = collection.type.arguments[0];
            output += indent(depth + 1) + c_type_name(element) + " " + variable
                + " = " + collection_name(collection.type) + "_get("
                + collection_temporary + ", (int64_t)" + index + ");\n";
            scopes_.back().emplace(
                loop.variable_name, ValueInfo{element, variable, false, false});
            loop_scope_indices_.push_back(scopes_.size() - 1);
            output += emit_statement_list(loop.body->statements, depth + 1);
            loop_scope_indices_.pop_back();
            output += scope_cleanup(scopes_.size() - 1, depth + 1);
            output += prefix + "}\n";
            pop_scope();
            output += scope_cleanup(scopes_.size() - 1, depth);
            pop_scope();
            return output;
        }
        case StmtKind::Stop:
        case StmtKind::Continue: {
            if (loop_scope_indices_.empty()) {
                throw_backend_error(statement.location,
                    "loop control is not inside a backend loop");
            }
            std::string output;
            for (std::size_t index = owned_reference_values_.size();
                 index > loop_scope_indices_.back(); --index) {
                output += scope_cleanup(index - 1, depth);
            }
            output += prefix
                + (statement.kind == StmtKind::Stop ? "break;\n" : "continue;\n");
            return output;
        }
        case StmtKind::FunctionDeclaration:
            throw_backend_error(
                statement.location,
                "nested functions are not supported by the C backend");
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

    GeneratedExpression convert_to_interface(
        GeneratedExpression value,
        const CValueType& target)
    {
        if (!implements_interface(value.type, target.nominal_name)) {
            throw_backend_error(
                current_location_,
                "type '" + value.type.nominal_name
                    + "' does not implement interface '" + target.nominal_name + "'");
        }
        const std::string table = interface_vtable_instance_name(
            target.nominal_name, value.type.nominal_name);
        std::string code = "(" + interface_name(target.nominal_name)
            + "){.toro_vtable = &" + table;
        if (value.type.kind == CValueKind::Class) {
            code += ", .toro_object = " + value.code
                + ", .toro_retain = "
                + interface_class_retain_name(
                    target.nominal_name, value.type.nominal_name)
                + ", .toro_release = "
                + interface_class_release_name(
                    target.nominal_name, value.type.nominal_name);
        } else {
            const std::string struct_value = value.pointer
                ? "*(" + value.code + ")"
                : value.code;
            code += ", .toro_object = NULL, .toro_retain = NULL, "
                ".toro_release = NULL, .toro_value."
                + interface_struct_storage_name(value.type.nominal_name)
                + " = " + struct_value;
        }
        code += "}";
        return {
            std::move(code),
            target,
            false,
            false,
            std::move(value.prelude),
            value.type.kind == CValueKind::Struct || value.owned,
        };
    }

    GeneratedExpression emit_expression(
        const Expr& expression,
        std::optional<CValueType> expected_type = std::nullopt)
    {
        auto result = emit_expression_impl(expression, expected_type);
        if (expected_type
            && expected_type->kind == CValueKind::Class
            && result.type.kind == CValueKind::Class
            && is_class_base_of(
                expected_type->nominal_name, result.type.nominal_name)) {
            if (expected_type->nominal_name != result.type.nominal_name) {
                result.code = class_upcast(
                    result.code,
                    result.type.nominal_name,
                    expected_type->nominal_name);
            }
            result.type = *expected_type;
        }
        if (expected_type
            && expected_type->kind == CValueKind::Interface
            && result.type.kind != CValueKind::Interface) {
            result = convert_to_interface(std::move(result), *expected_type);
        }
        return result;
    }

    GeneratedExpression emit_expression_impl(
        const Expr& expression,
        std::optional<CValueType> expected_type)
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
        case ExprKind::Index:
            return emit_index(static_cast<const IndexExpr&>(expression));
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
            return emit_cast(static_cast<const CastExpr&>(expression));
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
            || left.type.kind == CValueKind::Interface
            || left.type.kind == CValueKind::Enum
            || left.type.kind == CValueKind::Result
            || left.type.kind == CValueKind::Array
            || left.type.kind == CValueKind::List
            || left.type.kind == CValueKind::Map
            || right.type.kind == CValueKind::Struct
            || right.type.kind == CValueKind::Class
            || right.type.kind == CValueKind::Interface
            || right.type.kind == CValueKind::Enum
            || right.type.kind == CValueKind::Result
            || right.type.kind == CValueKind::Array
            || right.type.kind == CValueKind::List
            || right.type.kind == CValueKind::Map) {
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

    GeneratedExpression emit_cast(const CastExpr& cast)
    {
        if (!cast.resolved_type) {
            throw_backend_error(current_location_,
                "missing resolved type for explicit conversion");
        }
        const CValueType target = lower_type(*cast.resolved_type);
        auto source = emit_expression(*cast.expression);
        if (!cast.resolved_conversion) {
            if (source.type == target
                || (source.type.kind == CValueKind::Class
                    && source.type.nominal_name == target.nominal_name
                    && source.type.arguments == target.arguments)) {
                source.type = target;
                return source;
            }
            if (source.type.kind == CValueKind::Int
                && target.kind == CValueKind::Dec) {
                source.code = "((double)(" + source.code + "))";
                source.type = target;
                return source;
            }
            if (source.type.kind == CValueKind::Dec
                && target.kind == CValueKind::Int) {
                source.code = "((int64_t)(" + source.code + "))";
                source.type = target;
                return source;
            }
            throw_backend_error(current_location_,
                "explicit conversion is not supported by the C backend");
        }

        const ConversionInfo* conversion = nullptr;
        if (source.type.kind == CValueKind::Struct) {
            for (const auto& candidate :
                 structs_.at(source.type.nominal_name).conversions) {
                if (candidate.declaration == cast.resolved_conversion) {
                    conversion = &candidate;
                    break;
                }
            }
        } else if (source.type.kind == CValueKind::Class) {
            for (const auto& candidate :
                 classes_.at(source.type.nominal_name).conversions) {
                if (candidate.declaration == cast.resolved_conversion) {
                    conversion = &candidate;
                    break;
                }
            }
        }
        if (!conversion) {
            throw_backend_error(current_location_,
                "missing selected user-defined conversion in C backend");
        }

        std::string prelude = source.prelude;
        std::vector<ValueInfo> owned_arguments;
        std::string receiver;
        if (source.type.kind == CValueKind::Struct) {
            if (source.addressable || source.pointer) {
                receiver = source.pointer ? source.code : "&(" + source.code + ")";
            } else {
                const std::string temporary =
                    "toro_conversion_value_" + std::to_string(temporary_index_++);
                prelude += c_type_name(source.type) + " " + temporary
                    + " = " + source.code + ";\n";
                receiver = "&" + temporary;
            }
        } else if (source.type.kind == CValueKind::Class) {
            receiver = source.code;
            if (source.owned) {
                const std::string temporary =
                    "toro_conversion_class_" + std::to_string(temporary_index_++);
                prelude += c_type_name(source.type) + " " + temporary
                    + " = " + source.code + ";\n";
                receiver = temporary;
                owned_arguments.push_back(
                    ValueInfo{source.type, temporary, false, true});
            }
        } else {
            throw_backend_error(current_location_,
                "user-defined conversions require a struct or class value");
        }
        return complete_call(
            conversion_name(source.type.nominal_name, conversion->key)
                + "(" + receiver + ")",
            target,
            std::move(prelude),
            std::move(owned_arguments));
    }

    std::string prepare_call_argument(
        GeneratedExpression argument,
        const CValueType& parameter_type,
        std::string_view temporary_prefix,
        std::string& prelude,
        std::vector<ValueInfo>& owned_arguments)
    {
        prelude += argument.prelude;
        if (parameter_type.kind == CValueKind::Array) {
            const std::string temporary = std::string(temporary_prefix)
                + std::to_string(temporary_index_++);
            prelude += c_type_name(parameter_type) + " " + temporary + " = "
                + (argument.owned ? argument.code
                    : collection_name(parameter_type) + "_clone(" + argument.code + ")")
                + ";\n";
            owned_arguments.push_back(
                ValueInfo{parameter_type, temporary, false, true});
            return temporary;
        }
        if (is_managed_reference(argument.type) && argument.owned) {
            const std::string temporary = std::string(temporary_prefix)
                + std::to_string(temporary_index_++);
            prelude += c_type_name(argument.type) + " " + temporary
                + " = " + argument.code + ";\n";
            owned_arguments.push_back(
                ValueInfo{argument.type, temporary, false, true});
            return temporary;
        }
        return argument.code;
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
        if (callee.name == "Array" || callee.name == "List"
            || callee.name == "Map") {
            if (!call.resolved_type) {
                throw_backend_error(current_location_,
                    "missing resolved collection construction type");
            }
            return emit_collection_construction(
                lower_type(*call.resolved_type), call);
        }
        if (struct_templates_.contains(callee.name)) {
            if (!call.resolved_type) {
                throw_backend_error(
                    current_location_, "missing resolved generic construction type");
            }
            const auto type = lower_type(*call.resolved_type);
            return emit_struct_construction(type.nominal_name, call);
        }
        if (class_templates_.contains(callee.name)) {
            if (!call.resolved_type) {
                throw_backend_error(
                    current_location_, "missing resolved generic construction type");
            }
            const auto type = lower_type(*call.resolved_type);
            return emit_class_construction(type.nominal_name, call);
        }
        if (!call.resolved_function) {
            throw_backend_error(current_location_,
                "missing resolved overload for function '" + callee.name + "'");
        }
        std::vector<CValueType> generic_arguments;
        if (!call.resolved_function->generic_parameters.empty()) {
            for (const auto& parameter :
                 call.resolved_function->generic_parameters) {
                const auto substitution = std::ranges::find_if(
                    call.resolved_substitutions,
                    [&](const auto& value) {
                        return value.first == parameter.name;
                    });
                if (substitution == call.resolved_substitutions.end()) {
                    throw_backend_error(
                        current_location_,
                        "missing resolved generic argument for function '"
                            + callee.name + "'");
                }
                generic_arguments.push_back(lower_type(substitution->second));
            }
        }
        const std::string function_key = instantiate_function(
            *call.resolved_function, generic_arguments);
        const auto function = functions_.find(function_key);
        if (function == functions_.end()) {
            throw_backend_error(
                current_location_, "unknown backend function '" + callee.name + "'");
        }

        const auto ordered = order_arguments(
            call, function->second.declaration->parameters);

        std::string code = function_name(function_key) + "(";
        std::string prelude;
        std::vector<ValueInfo> owned_arguments;
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            if (index != 0) {
                code += ", ";
            }
            const auto argument = emit_expression(
                *ordered[index]->value, function->second.parameter_types[index]);
            code += prepare_call_argument(
                argument,
                function->second.parameter_types[index],
                "toro_call_argument_",
                prelude,
                owned_arguments);
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
                const auto saved = current_substitutions_;
                current_substitutions_ = info.substitutions;
                const auto value = emit_expression(
                    *field.declaration->default_value, field.type);
                current_substitutions_ = saved;
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
        if (const auto base_initializer = base_initializer_owner(name)) {
            throw_backend_error(
                current_location_,
                "construction of derived class '" + name
                    + "' requires unsupported base initializer chaining for '"
                    + *base_initializer + "'");
        }
        if (const auto initializer = info.methods.find("init");
            initializer != info.methods.end()) {
            return emit_initialized_class_construction(
                name, call, initializer->second);
        }
        const auto fields = effective_class_fields(name);
        std::vector<const CallArgument*> arguments(fields.size(), nullptr);
        std::size_t positional_index = 0;
        for (const auto& argument : call.arguments) {
            std::size_t field_index = positional_index;
            if (argument.name) {
                const auto found = std::ranges::find_if(
                    fields,
                    [&](const auto& field) {
                        return field.second->declaration->name == *argument.name;
                    });
                if (found == fields.end()) {
                    throw_backend_error(
                        current_location_,
                        "class '" + name + "' has no field named '"
                            + *argument.name + "'");
                }
                field_index = static_cast<std::size_t>(found - fields.begin());
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
        const std::string strong_count = class_header_access(
            temporary, name, "toro_strong_count");
        const std::string weak_control = class_header_access(
            temporary, name, "toro_weak_control");
        const std::string finalizer = class_header_access(
            temporary, name, "toro_finalize");
        prelude += strong_count + " = 1;\n";
        prelude += weak_control + " = malloc(sizeof(*" + weak_control + "));\n";
        prelude += "if (" + weak_control + " == NULL) { abort(); }\n";
        prelude += weak_control + "->toro_object = " + temporary + ";\n";
        prelude += weak_control + "->toro_weak_count = 1;\n";
        prelude += finalizer + " = " + finalize_name(name) + ";\n";
        if (!virtual_slots(class_root(name)).empty()) {
            prelude += class_header_access(temporary, name, "toro_vtable")
                + " = &" + vtable_instance_name(name) + ";\n";
        }
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto& field = *fields[index].second;
            GeneratedExpression value{"", field.type};
            if (arguments[index]) {
                value = emit_expression(*arguments[index]->value, field.type);
            } else if (field.declaration->default_value) {
                const auto saved = current_substitutions_;
                current_substitutions_ =
                    classes_.at(fields[index].first).substitutions;
                value = emit_expression(*field.declaration->default_value, field.type);
                current_substitutions_ = saved;
            } else {
                value = GeneratedExpression{
                    zero_value(field.type, field.declaration->location), field.type};
            }
            prelude += value.prelude;
            const std::string access = class_member_access(
                temporary,
                name,
                fields[index].first,
                field_name(field.declaration->name));
            if (field.declaration->is_weak) {
                const std::string weak_value =
                    "toro_weak_value_" + std::to_string(temporary_index_++);
                prelude += c_type_name(field.type) + " " + weak_value
                    + " = " + value.code + ";\n";
                prelude += access + ".toro_control = NULL;\n";
                prelude += "toro_weak_set(&" + access + ", " + weak_value
                    + " == NULL ? NULL : " + class_header_access(
                        weak_value, field.type.nominal_name, "toro_weak_control")
                    + ");\n";
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
        const MethodInfo& initializer)
    {
        const CValueType type{CValueKind::Class, name};
        const std::string temporary =
            "toro_new_class_" + std::to_string(temporary_index_++);
        std::string prelude = c_type_name(type) + " " + temporary
            + " = malloc(sizeof(*" + temporary + "));\n";
        prelude += "if (" + temporary + " == NULL) { abort(); }\n";
        const std::string strong_count = class_header_access(
            temporary, name, "toro_strong_count");
        const std::string weak_control = class_header_access(
            temporary, name, "toro_weak_control");
        const std::string finalizer = class_header_access(
            temporary, name, "toro_finalize");
        prelude += strong_count + " = 1;\n";
        prelude += weak_control + " = malloc(sizeof(*" + weak_control + "));\n";
        prelude += "if (" + weak_control + " == NULL) { abort(); }\n";
        prelude += weak_control + "->toro_object = " + temporary + ";\n";
        prelude += weak_control + "->toro_weak_count = 1;\n";
        prelude += finalizer + " = " + finalize_name(name) + ";\n";
        if (!virtual_slots(class_root(name)).empty()) {
            prelude += class_header_access(temporary, name, "toro_vtable")
                + " = &" + vtable_instance_name(name) + ";\n";
        }

        for (const auto& [owner, field_pointer] : effective_class_fields(name)) {
            const auto& field = *field_pointer;
            const std::string access = class_member_access(
                temporary,
                name,
                owner,
                field_name(field.declaration->name));
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

            const auto saved = current_substitutions_;
            current_substitutions_ = classes_.at(owner).substitutions;
            const auto value = emit_expression(
                *field.declaration->default_value, field.type);
            current_substitutions_ = saved;
            prelude += value.prelude;
            if (field.declaration->is_weak) {
                const std::string weak_value =
                    "toro_weak_value_" + std::to_string(temporary_index_++);
                prelude += c_type_name(field.type) + " " + weak_value
                    + " = " + value.code + ";\n";
                prelude += "toro_weak_set(&" + access + ", " + weak_value
                    + " == NULL ? NULL : " + class_header_access(
                        weak_value, field.type.nominal_name, "toro_weak_control")
                    + ");\n";
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
            invocation += ", ";
            invocation += prepare_call_argument(
                argument,
                initializer.parameter_types[index],
                "toro_init_argument_",
                prelude,
                owned_arguments);
        }
        invocation += ");\n";
        prelude += invocation;
        prelude += release_owned_values(owned_arguments);
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
        case CValueKind::Interface:
            throw_backend_error(
                location,
                "interface-valued field has no backend zero value");
        case CValueKind::Enum:
            throw_backend_error(
                location,
                "omitted enum field '" + type.nominal_name
                    + "' has no backend zero value");
        case CValueKind::Result:
            throw_backend_error(
                location, "omitted Result field has no backend zero value");
        case CValueKind::Array:
        case CValueKind::List:
        case CValueKind::Map:
            throw_backend_error(
                location, "omitted collection field has no backend zero value");
        case CValueKind::Void:
            break;
        }
        throw_backend_error(location, "field has no backend zero value");
    }

    GeneratedExpression emit_collection_construction(
        const CValueType& type,
        const CallExpr& call)
    {
        const std::string temporary =
            "toro_new_collection_" + std::to_string(temporary_index_++);
        std::string prelude;
        if (type.kind == CValueKind::Array) {
            prelude += c_type_name(type) + " " + temporary + " = "
                + collection_name(type) + "_new("
                + std::to_string(call.arguments.size()) + ");\n";
        } else if (type.kind == CValueKind::List || type.kind == CValueKind::Map) {
            prelude += c_type_name(type) + " " + temporary + " = "
                + collection_name(type) + "_new();\n";
        } else {
            throw_backend_error(current_location_, "invalid collection construction");
        }
        if (type.kind != CValueKind::Map) {
            const CValueType& element = type.arguments[0];
            for (std::size_t index = 0; index < call.arguments.size(); ++index) {
                const auto value = emit_expression(*call.arguments[index].value, element);
                prelude += value.prelude;
                std::string value_code = value.code;
                if (is_owned_runtime_value(value.type) && value.owned) {
                    const std::string value_temporary =
                        "toro_collection_element_"
                        + std::to_string(temporary_index_++);
                    prelude += c_type_name(value.type) + " " + value_temporary
                        + " = " + value.code + ";\n";
                    value_code = value_temporary;
                    prelude += collection_name(type)
                        + (type.kind == CValueKind::Array ? "_set(&" : "_add(")
                        + temporary + ", "
                        + (type.kind == CValueKind::Array
                                ? std::to_string(index) + ", " : "")
                        + value_code + ");\n";
                    prelude += release_call(value.type, value_temporary) + ";\n";
                    continue;
                }
                prelude += collection_name(type)
                    + (type.kind == CValueKind::Array ? "_set(&" : "_add(")
                    + temporary + ", "
                    + (type.kind == CValueKind::Array
                            ? std::to_string(index) + ", " : "")
                    + value_code + ");\n";
            }
        }
        return {temporary, type, true, false, std::move(prelude), true};
    }

    GeneratedExpression emit_index(const IndexExpr& index)
    {
        const auto object = emit_expression(*index.object);
        if (object.owned) {
            throw_backend_error(current_location_,
                "indexing a temporary collection is not supported by the C backend");
        }
        if (object.type.kind != CValueKind::Array
            && object.type.kind != CValueKind::List
            && object.type.kind != CValueKind::Map) {
            throw_backend_error(current_location_, "indexing requires a collection");
        }
        const CValueType index_type = object.type.kind == CValueKind::Map
            ? object.type.arguments[0] : int_type;
        const auto subscript = emit_expression(*index.index, index_type);
        const CValueType result_type = object.type.kind == CValueKind::Map
            ? object.type.arguments[1] : object.type.arguments[0];
        return {
            collection_name(object.type) + "_get(" + object.code + ", "
                + subscript.code + ")",
            result_type,
            false,
            false,
            object.prelude + subscript.prelude,
        };
    }

    std::string emit_index_assignment(
        const IndexAssignmentStmt& assignment,
        std::size_t depth)
    {
        const auto object = emit_expression(*assignment.target->object);
        if (object.owned) {
            throw_backend_error(assignment.location,
                "indexed assignment through a temporary collection is not supported");
        }
        if (object.type.kind != CValueKind::Array
            && object.type.kind != CValueKind::List
            && object.type.kind != CValueKind::Map) {
            throw_backend_error(assignment.location,
                "indexed assignment requires a collection");
        }
        if (object.type.kind == CValueKind::Array && !object.addressable) {
            throw_backend_error(assignment.location,
                "Array indexed assignment requires an addressable value");
        }
        const CValueType index_type = object.type.kind == CValueKind::Map
            ? object.type.arguments[0] : int_type;
        const CValueType element_type = object.type.kind == CValueKind::Map
            ? object.type.arguments[1] : object.type.arguments[0];
        const auto subscript = emit_expression(*assignment.target->index, index_type);
        const auto value = emit_expression(*assignment.value, element_type);
        const std::string prefix = indent(depth);
        std::string output = indent_prelude(
            object.prelude + subscript.prelude + value.prelude, depth);
        std::string value_code = value.code;
        std::optional<std::string> owned_temporary;
        if (is_owned_runtime_value(value.type) && value.owned) {
            owned_temporary = "toro_index_value_"
                + std::to_string(temporary_index_++);
            output += prefix + c_type_name(value.type) + " " + *owned_temporary
                + " = " + value.code + ";\n";
            value_code = *owned_temporary;
        }
        output += prefix + collection_name(object.type) + "_set("
            + (object.type.kind == CValueKind::Array ? "&" : "")
            + object.code + ", " + subscript.code + ", " + value_code + ");\n";
        if (owned_temporary) {
            output += prefix + release_call(value.type, *owned_temporary) + ";\n";
        }
        return output;
    }

    GeneratedExpression emit_collection_method_call(
        const CallExpr& call,
        const MemberAccessExpr& member,
        const GeneratedExpression& receiver)
    {
        if (receiver.owned) {
            throw_backend_error(current_location_,
                "calling a method on a temporary collection is not supported");
        }
        if (member.member == "clone") {
            if (!call.arguments.empty()) {
                throw_backend_error(current_location_, "clone() takes no arguments");
            }
            return {
                collection_name(receiver.type) + "_clone(" + receiver.code + ")",
                receiver.type,
                false,
                false,
                receiver.prelude,
                true,
            };
        }
        if (receiver.type.kind == CValueKind::List
            && (member.member == "add" || member.member == "append")) {
            const auto value = emit_expression(
                *call.arguments.front().value, receiver.type.arguments[0]);
            std::string prelude = receiver.prelude + value.prelude;
            std::string value_code = value.code;
            if (is_owned_runtime_value(value.type) && value.owned) {
                const std::string temporary =
                    "toro_list_element_" + std::to_string(temporary_index_++);
                prelude += c_type_name(value.type) + " " + temporary + " = "
                    + value.code + ";\n";
                prelude += collection_name(receiver.type) + "_add("
                    + receiver.code + ", " + temporary + ");\n";
                prelude += release_call(value.type, temporary) + ";\n";
                return {"(void)0", void_type, false, false, std::move(prelude)};
            }
            return {
                collection_name(receiver.type) + "_add(" + receiver.code + ", "
                    + value_code + ")",
                void_type,
                false,
                false,
                std::move(prelude),
            };
        }
        if (receiver.type.kind == CValueKind::Map && member.member == "contains") {
            const auto key = emit_expression(
                *call.arguments.front().value, receiver.type.arguments[0]);
            return {
                collection_name(receiver.type) + "_contains(" + receiver.code
                    + ", " + key.code + ")",
                bool_type,
                false,
                false,
                receiver.prelude + key.prelude,
            };
        }
        throw_backend_error(current_location_, "unsupported collection method '"
            + member.member + "'");
    }

    GeneratedExpression emit_member_access(const MemberAccessExpr& member)
    {
        const auto object = emit_expression(*member.object);
        if (object.type.kind == CValueKind::Array
            || object.type.kind == CValueKind::List
            || object.type.kind == CValueKind::Map) {
            if (object.owned) {
                throw_backend_error(current_location_,
                    "member access through a temporary collection is not supported");
            }
            if (member.member != "count" && member.member != "length") {
                throw_backend_error(current_location_, "collection type has no member named '"
                    + member.member + "'");
            }
            return {
                object.code + (object.type.kind == CValueKind::Array
                    ? ".toro_count" : "->toro_count"),
                int_type,
                false,
                false,
                object.prelude,
            };
        }
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
            const auto field = find_class_field(
                object.type.nominal_name, member.member);
            if (!field) {
                throw_backend_error(
                    current_location_,
                    "class '" + object.type.nominal_name + "' has no field named '"
                        + member.member + "'");
            }
            const auto& field_info = *field->second;
            const std::string access = class_member_access(
                object.code,
                object.type.nominal_name,
                field->first,
                field_name(member.member));
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
        const auto field = find_class_field(
            object.type.nominal_name, assignment.target->member);
        if (!field) {
            throw_backend_error(
                assignment.location,
                "class '" + object.type.nominal_name + "' has no field named '"
                    + assignment.target->member + "'");
        }
        const auto& field_info = *field->second;
        const auto value = emit_expression(*assignment.value, field_info.type);
        const std::string prefix = indent(depth);
        const std::string access = class_member_access(
            object.code,
            object.type.nominal_name,
            field->first,
            field_name(assignment.target->member));
        std::string output = indent_prelude(object.prelude + value.prelude, depth);
        if (field_info.declaration->is_weak) {
            const std::string temporary =
                "toro_weak_value_" + std::to_string(temporary_index_++);
            output += prefix + c_type_name(field_info.type) + " " + temporary
                + " = " + value.code + ";\n";
            output += prefix + "toro_weak_set(&" + access + ", " + temporary
                + " == NULL ? NULL : " + class_header_access(
                    temporary, field_info.type.nominal_name, "toro_weak_control")
                + ");\n";
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
        if (receiver.type.kind == CValueKind::Array
            || receiver.type.kind == CValueKind::List
            || receiver.type.kind == CValueKind::Map) {
            return emit_collection_method_call(call, member, receiver);
        }
        if (member.member == "destroy") {
            throw_backend_error(
                current_location_,
                "destroy() cannot be invoked directly");
        }
        if (receiver.type.kind == CValueKind::Interface) {
            return emit_interface_method_call(call, member, receiver);
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
        const MethodInfo* method_info = nullptr;
        std::string method_owner = receiver.type.nominal_name;
        std::string method_key = member.member;
        const auto resolved_method_key = [&](
            const std::string& owner,
            CValueKind owner_kind,
            const MethodDeclaration& method) {
            if (method.generic_parameters.empty()) {
                return method_source_key(owner, owner_kind, method, {});
            }
            std::vector<CValueType> arguments;
            for (const auto& parameter : method.generic_parameters) {
                const auto substitution = std::ranges::find_if(
                    call.resolved_substitutions,
                    [&](const auto& value) {
                        return value.first == parameter.name;
                    });
                if (substitution == call.resolved_substitutions.end()) {
                    throw_backend_error(
                        current_location_,
                        "missing resolved generic argument for method '"
                            + member.member + "'");
                }
                arguments.push_back(lower_type(substitution->second));
            }
            return method_source_key(owner, owner_kind, method, arguments);
        };
        if (receiver.type.kind == CValueKind::Struct) {
            const auto& struct_info = structs_.at(receiver.type.nominal_name);
            if (!call.resolved_method) {
                throw_backend_error(current_location_,
                    "missing resolved overload for method '" + member.member + "'");
            }
            method_key = resolved_method_key(
                receiver.type.nominal_name,
                CValueKind::Struct,
                *call.resolved_method);
            const auto& methods = struct_info.methods;
            const auto method = methods.find(method_key);
            if (method != methods.end()) {
                method_info = &method->second;
            }
        } else {
            if (!call.resolved_method) {
                throw_backend_error(current_location_,
                    "missing resolved overload for method '" + member.member + "'");
            }
            if (const auto selected = find_class_method_by_declaration(
                    receiver.type.nominal_name, *call.resolved_method)) {
                method_owner = selected->first;
                method_key = resolved_method_key(
                    method_owner,
                    CValueKind::Class,
                    *call.resolved_method);
            }
        }
        if (receiver.type.kind == CValueKind::Class) {
            if (call.resolved_method) {
                if (const auto method = find_class_method_by_declaration(
                        receiver.type.nominal_name, *call.resolved_method)) {
                    method_owner = method->first;
                    method_info = method->second;
                }
            }
        }
        if (!method_info) {
            throw_backend_error(
                current_location_,
                "type '" + receiver.type.nominal_name + "' has no method named '"
                    + member.member + "'");
        }
        const auto virtual_slot = receiver.type.kind == CValueKind::Class
            ? find_virtual_slot(receiver.type.nominal_name, *method_info)
            : std::nullopt;
        if (receiver.type.kind == CValueKind::Struct
            && !receiver.pointer && !receiver.addressable) {
            throw_backend_error(
                current_location_,
                "struct method receiver must be an addressable value");
        }
        const auto ordered = order_arguments(
            call, method_info->declaration->parameters);
        std::string prelude = receiver.prelude;
        std::vector<ValueInfo> owned_arguments;
        std::string receiver_code;
        if (is_managed_reference(receiver.type) && receiver.owned) {
            const std::string temporary =
                "toro_receiver_class_" + std::to_string(temporary_index_++);
            prelude += c_type_name(receiver.type) + " " + temporary
                + " = " + receiver.code + ";\n";
            owned_arguments.push_back(
                ValueInfo{receiver.type, temporary, false, true});
            receiver_code = temporary;
        } else {
            receiver_code = receiver.type.kind == CValueKind::Class || receiver.pointer
                ? receiver.code
                : "&(" + receiver.code + ")";
        }
        std::string code;
        if (virtual_slot) {
            const std::string vtable = class_header_access(
                receiver_code, receiver.type.nominal_name, "toro_vtable");
            const std::string control = class_header_access(
                receiver_code, receiver.type.nominal_name, "toro_weak_control");
            code = vtable + "->" + virtual_slot_name(
                virtual_slot->declaration_owner,
                virtual_slot->declaration->key) + "("
                + control + "->toro_object";
        } else {
            if (receiver.type.kind == CValueKind::Class
                && method_owner != receiver.type.nominal_name) {
                receiver_code = class_upcast(
                    receiver_code, receiver.type.nominal_name, method_owner);
            }
            code = method_name(method_owner, method_info->key) + "(" + receiver_code;
        }
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            const auto argument = emit_expression(
                *ordered[index]->value, method_info->parameter_types[index]);
            code += ", ";
            code += prepare_call_argument(
                argument,
                method_info->parameter_types[index],
                "toro_method_argument_",
                prelude,
                owned_arguments);
        }
        code += ")";
        return complete_call(
            std::move(code),
            method_info->return_type,
            std::move(prelude),
            std::move(owned_arguments));
    }

    GeneratedExpression emit_interface_method_call(
        const CallExpr& call,
        const MemberAccessExpr& member,
        const GeneratedExpression& receiver)
    {
        const auto& interface = interfaces_.at(receiver.type.nominal_name);
        const auto found = std::ranges::find_if(
            interface.methods,
            [&](const InterfaceMethodInfo& candidate) {
                return candidate.declaration == call.resolved_interface_method;
            });
        if (found == interface.methods.end()) {
            throw_backend_error(
                current_location_,
                "missing resolved interface overload for '"
                    + receiver.type.nominal_name + "." + member.member + "'");
        }
        const auto& method = *found;
        const auto ordered = order_arguments(call, method.declaration->parameters);
        std::string prelude = receiver.prelude;
        std::vector<ValueInfo> owned_arguments;
        std::string receiver_code = receiver.code;
        if (receiver.owned || !receiver.addressable) {
            receiver_code =
                "toro_interface_receiver_" + std::to_string(temporary_index_++);
            prelude += c_type_name(receiver.type) + " " + receiver_code
                + " = " + receiver.code + ";\n";
            if (receiver.owned) {
                owned_arguments.push_back(
                    ValueInfo{receiver.type, receiver_code, false, true});
            }
        }
        std::string code = receiver_code + ".toro_vtable->"
            + interface_slot_name(receiver.type.nominal_name, method.key)
            + "(&" + receiver_code;
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            const auto argument = emit_expression(
                *ordered[index]->value, method.parameter_types[index]);
            code += ", ";
            code += prepare_call_argument(
                argument,
                method.parameter_types[index],
                "toro_interface_argument_",
                prelude,
                owned_arguments);
        }
        code += ")";
        return complete_call(
            std::move(code),
            method.return_type,
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
                is_owned_runtime_value(result_type),
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
        prelude += release_owned_values(owned_arguments);
        return {
            std::move(result_code),
            result_type,
            false,
            false,
            std::move(prelude),
            is_owned_runtime_value(result_type),
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
        case CValueKind::Interface:
            throw_backend_error(
                current_location_, "printing interface values is not supported by the C backend");
        case CValueKind::Enum:
            throw_backend_error(
                current_location_, "printing enum values is not supported by the C backend");
        case CValueKind::Result:
            throw_backend_error(
                current_location_, "printing Result values is not supported by the C backend");
        case CValueKind::Array:
        case CValueKind::List:
        case CValueKind::Map:
            throw_backend_error(
                current_location_, "printing collection values is not supported by the C backend");
        case CValueKind::Void:
            throw_backend_error(
                current_location_, "cannot print an expression without a value");
        }
        throw_backend_error(current_location_, "unsupported print value");
    }

    void push_scope()
    {
        scopes_.emplace_back();
        owned_reference_values_.emplace_back();
    }

    void pop_scope()
    {
        scopes_.pop_back();
        owned_reference_values_.pop_back();
    }

    void bind_parameters(
        const std::vector<Parameter>& parameters,
        const std::vector<CValueType>& parameter_types)
    {
        if (parameters.size() != parameter_types.size()) {
            throw_backend_error(
                current_location_, "internal C backend parameter-type mismatch");
        }
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            const bool owns_parameter =
                is_managed_reference(parameter_types[index]);
            const std::string name = parameter_name(parameters[index].name);
            scopes_.back().emplace(
                parameters[index].name,
                ValueInfo{parameter_types[index], name, false, owns_parameter});
            if (owns_parameter) {
                owned_reference_values_.back().push_back(
                    ValueInfo{parameter_types[index], name, false, true});
            }
        }
    }

    std::string retain_owned_parameters(std::size_t depth) const
    {
        std::string output;
        for (const auto& parameter : owned_reference_values_.back()) {
            output += indent(depth)
                + retain_call(parameter.type, parameter.c_name) + ";\n";
        }
        return output;
    }

    std::string release_owned_values(
        const std::vector<ValueInfo>& values) const
    {
        std::string output;
        for (auto value = values.rbegin(); value != values.rend(); ++value) {
            output += release_call(value->type, value->c_name) + ";\n";
        }
        return output;
    }

    std::string scope_cleanup(std::size_t scope_index, std::size_t depth) const
    {
        std::string output;
        const auto& values = owned_reference_values_.at(scope_index);
        for (auto value = values.rbegin(); value != values.rend(); ++value) {
            output += indent(depth) + release_call(value->type, value->c_name)
                + ";\n";
        }
        return output;
    }

    std::string all_scope_cleanup(std::size_t depth) const
    {
        std::string output;
        for (std::size_t index = owned_reference_values_.size(); index > 0; --index) {
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
    std::vector<std::string> struct_order_;
    std::unordered_map<std::string, const StructDeclarationStmt*> struct_templates_;
    std::unordered_map<std::string, ClassInfo> classes_;
    std::vector<std::string> class_order_;
    std::unordered_map<std::string, const ClassDeclarationStmt*> class_templates_;
    std::unordered_map<std::string, InterfaceInfo> interfaces_;
    std::vector<const InterfaceDeclarationStmt*> interface_order_;
    std::unordered_map<std::string, EnumInfo> enums_;
    std::vector<const EnumDeclarationStmt*> enum_order_;
    std::vector<CValueType> nominal_order_;
    std::vector<ResultInfo> result_order_;
    std::vector<CValueType> collection_order_;
    std::unordered_map<std::string, FunctionInfo> functions_;
    std::unordered_map<std::string,
        std::vector<const FunctionDeclarationStmt*>> function_templates_;
    std::vector<std::string> function_order_;
    std::vector<std::unordered_map<std::string, ValueInfo>> scopes_;
    std::vector<std::vector<ValueInfo>> owned_reference_values_;
    std::size_t temporary_index_{0};
    std::optional<CValueType> current_return_type_;
    std::unordered_map<std::string, CValueType> current_substitutions_;
    std::unordered_set<std::string> instantiating_types_;
    std::vector<std::size_t> loop_scope_indices_;
    SourceLocation current_location_{1, 1};
};

} // namespace

std::string CGenerator::generate(const Program& program)
{
    return Generator{}.generate(program);
}

} // namespace toro
