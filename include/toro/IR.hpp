#pragma once

#include "toro/SourceLocation.hpp"
#include "toro/Type.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace toro {

enum class IRValueCategory {
    Primitive,
    Value,
    ManagedReference,
    InterfaceValue,
    CollectionValue,
    ResultValue,
    Unknown,
};

struct IRType {
    Type type;
    IRValueCategory category{IRValueCategory::Unknown};
};

struct IROrigin {
    std::string module_name;
    SourceLocation location{1, 1};
};

enum class IROperationKind {
    Block,
    Constant,
    Load,
    Declare,
    Assign,
    MemberAssign,
    IndexAssign,
    Unary,
    Binary,
    Call,
    ForeignCall,
    VirtualCall,
    InterfaceCall,
    Construction,
    EnumConstruction,
    ResultConstruction,
    CollectionOperation,
    MemberAccess,
    IndexAccess,
    Conversion,
    PropagateResult,
    Branch,
    Loop,
    ForEach,
    Handle,
    Return,
    Stop,
    Continue,
};

struct IROperation {
    IROperationKind kind{IROperationKind::Block};
    IROrigin origin;
    std::optional<IRType> result_type;
    std::string name;
    std::string selected_declaration;
    std::vector<std::pair<std::string, IRType>> substitutions;
    std::vector<std::string> argument_names;
    std::vector<IROperation> children;
};

struct IRGenericParameter {
    std::string name;
    std::vector<IRType> constraints;
    IROrigin origin;
};

struct IRField {
    std::string name;
    IRType type;
    IROrigin origin;
    std::optional<IROperation> default_value;
    bool is_public{false};
    bool is_weak{false};
};

struct IREnumVariant {
    std::string name;
    std::optional<IRType> payload_type;
    IROrigin origin;
};

enum class IRDeclarationKind {
    Function,
    ForeignFunction,
    Struct,
    Class,
    Interface,
    Enum,
    Method,
    Conversion,
};

struct IRParameter {
    std::string name;
    IRType type;
    IROrigin origin;
};

struct IRDeclaration {
    IRDeclarationKind kind{IRDeclarationKind::Function};
    IROrigin origin;
    std::string name;
    std::string identity;
    std::string owner_identity;
    std::vector<IRGenericParameter> generic_parameters;
    std::vector<IRParameter> parameters;
    std::vector<IRField> fields;
    std::vector<IREnumVariant> variants;
    std::optional<IRType> return_type;
    std::vector<IRType> related_types;
    std::optional<IROperation> body;
    std::string calling_convention;
    bool is_virtual{false};
    bool is_override{false};
    bool is_abstract{false};
    bool is_public{false};
};

struct IRModule {
    std::string name;
    std::vector<std::string> imports;
};

enum class IRLifetimeModel {
    LegacyArc,
};

enum class IREffect : std::uint8_t {
    Allocate,
    Retain,
    ReleaseDestroy,
    Escape,
    Block,
    FilesystemIO,
    EnvironmentAccess,
    TimeQuery,
    ProcessIO,
    Synchronize,
    ForeignCall,
    NativeCall,
    DynamicDispatch,
    Terminate,
    UnknownExternal,
    Count,
};

struct IREffectCause {
    IREffect effect{IREffect::Allocate};
    IROrigin origin;
    std::string via_callable;
    bool transitive{false};
};

struct IRCallableEffectSummary {
    std::string callable_identity;
    std::vector<IREffect> effects;
    std::vector<IREffectCause> causes;
};

struct IRProgram {
    std::vector<IRModule> modules;
    std::vector<IRDeclaration> declarations;
    std::vector<IROperation> top_level_operations;
    std::string entry_module;
    IRLifetimeModel lifetime_model{IRLifetimeModel::LegacyArc};
    std::vector<IRCallableEffectSummary> effect_summaries;

};

[[nodiscard]] std::string_view ir_effect_name(IREffect effect);
[[nodiscard]] std::string dump_ir(
    const IRProgram& program,
    bool include_effects = false);

} // namespace toro
