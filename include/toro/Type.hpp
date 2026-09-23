#pragma once

#include <string_view>

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
    TypeKind kind;

    bool operator==(const Type&) const = default;
};

[[nodiscard]] constexpr bool is_numeric(Type type)
{
    return type.kind == TypeKind::Int || type.kind == TypeKind::Dec;
}

[[nodiscard]] constexpr bool is_unknown(Type type)
{
    return type.kind == TypeKind::Unknown;
}

[[nodiscard]] constexpr std::string_view type_name(Type type)
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

} // namespace toro
