#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace toro {

enum class TypeKind {
    Int,
    Dec,
    String,
    Bool,
    Null,
    Void,
    Unknown,
};

struct Type {
    explicit Type(
        TypeKind kind,
        bool nullable = false,
        bool deferred = false,
        std::string name = {},
        std::vector<Type> arguments = {})
        : kind(kind)
        , nullable(nullable)
        , deferred(deferred)
        , name(std::move(name))
        , arguments(std::move(arguments))
    {
    }

    TypeKind kind;
    bool nullable{false};
    bool deferred{false};
    std::string name;
    std::vector<Type> arguments;

    bool operator==(const Type&) const = default;
};

[[nodiscard]] constexpr bool is_numeric(const Type& type)
{
    return !type.nullable
        && (type.kind == TypeKind::Int || type.kind == TypeKind::Dec);
}

[[nodiscard]] constexpr bool is_unknown(const Type& type)
{
    return type.kind == TypeKind::Unknown;
}

[[nodiscard]] constexpr std::string_view base_type_name(const Type& type)
{
    switch (type.kind) {
    case TypeKind::Int: return "int";
    case TypeKind::Dec: return "dec";
    case TypeKind::String: return "string";
    case TypeKind::Bool: return "bool";
    case TypeKind::Null: return "null";
    case TypeKind::Void: return "no value";
    case TypeKind::Unknown: return "unknown";
    }
    return "unknown";
}

[[nodiscard]] inline std::string type_name(const Type& type)
{
    std::string result = is_unknown(type) && !type.name.empty()
        ? type.name
        : std::string(base_type_name(type));
    if (!type.arguments.empty()) {
        result += '<';
        for (std::size_t index = 0; index < type.arguments.size(); ++index) {
            if (index != 0) {
                result += ", ";
            }
            result += type_name(type.arguments[index]);
        }
        result += '>';
    }
    if (type.nullable) {
        result += '?';
    }
    return result;
}

} // namespace toro
