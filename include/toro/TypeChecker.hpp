#pragma once

#include "toro/AST.hpp"
#include "toro/Type.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toro {

class TypeChecker {
public:
    void check(const Program& program);

private:
    struct FunctionParameterType {
        std::string name;
        Type type;
        TypeReference type_reference;
    };

    struct FunctionSignature {
        std::vector<FunctionParameterType> parameters;
        Type return_type;
        std::optional<TypeReference> return_type_reference;
        std::vector<GenericParameter> generic_parameters;
        std::unordered_map<std::string, Type> containing_substitutions;
        SourceLocation location;
    };

    struct OverloadResolution {
        const FunctionSignature* signature;
        Type return_type;
    };

    struct FieldInfo {
        Type type;
        Visibility visibility;
        bool required;
        bool has_explicit_default;
        std::string owner;
        TypeReference type_reference;
    };

    struct MethodInfo {
        FunctionSignature signature;
        Visibility visibility;
        std::string owner;
        bool is_virtual;
        bool is_abstract;
        bool is_override;
    };

    struct EnumVariantInfo {
        std::optional<Type> payload_type;
        SourceLocation location;
    };

    enum class NominalKind { Struct, Class, Interface, Enum };

    struct NominalTypeInfo {
        NominalKind kind;
        bool is_abstract{false};
        std::optional<std::string> base;
        std::vector<std::string> interfaces;
        std::vector<GenericParameter> generic_parameters;
        std::vector<std::string> field_order;
        std::unordered_map<std::string, FieldInfo> fields;
        std::unordered_map<std::string, std::vector<MethodInfo>> methods;
        std::vector<std::string> variant_order;
        std::unordered_map<std::string, EnumVariantInfo> variants;
        SourceLocation location;
    };

    struct Scope {
        std::unordered_map<std::string, Type> values;
        std::unordered_map<std::string, std::vector<FunctionSignature>> functions;
        std::unordered_map<std::string, NominalTypeInfo> nominal_types;
        std::unordered_map<std::string, std::unordered_set<std::string>> conversions;
    };

    void check_statement_list(const std::vector<std::unique_ptr<Stmt>>& statements);
    void predeclare(const std::vector<std::unique_ptr<Stmt>>& statements);
    void validate_nominal_types();
    void validate_inheritance_cycles() const;
    void validate_class(const std::string& name, const NominalTypeInfo& type_info) const;
    void validate_interfaces(const std::string& name, const NominalTypeInfo& type_info) const;
    void check_statement(const Stmt& statement);
    [[nodiscard]] Type check_expression(
        const Expr& expression,
        std::optional<Type> expected_type = std::nullopt);
    [[nodiscard]] Type check_call(
        const CallExpr& call,
        std::optional<Type> expected_type);
    [[nodiscard]] Type check_result_construction(
        const std::string& constructor_name,
        const CallExpr& call,
        const Type& expected_type);
    [[nodiscard]] Type check_propagation(const PropagationExpr& propagation);
    [[nodiscard]] Type check_construction(
        const std::string& name,
        const CallExpr& call,
        const std::vector<Type>& arguments,
        const NominalTypeInfo& type_info);
    [[nodiscard]] Type check_method_call(
        const MemberAccessExpr& callee,
        const CallExpr& call,
        const std::vector<Type>& arguments);
    [[nodiscard]] Type check_enum_variant_call(
        const std::string& enum_name,
        const std::string& variant_name,
        const CallExpr& call,
        const std::vector<Type>& arguments,
        const NominalTypeInfo& type_info) const;
    [[nodiscard]] OverloadResolution resolve_overload(
        const std::string& callable_kind,
        const std::string& callable_name,
        const std::vector<CallArgument>& call_arguments,
        const std::vector<Type>& arguments,
        const std::vector<TypeReference>& generic_arguments,
        const std::vector<const FunctionSignature*>& candidates) const;
    [[nodiscard]] std::optional<int> overload_score(
        const std::vector<CallArgument>& call_arguments,
        const std::vector<Type>& arguments,
        const std::vector<TypeReference>& generic_arguments,
        const FunctionSignature& signature,
        Type& return_type,
        std::string& failure_reason) const;
    [[nodiscard]] Type check_member_access(const MemberAccessExpr& member);
    [[nodiscard]] Type check_type_access(const TypeAccessExpr& access) const;
    [[nodiscard]] Type check_binary(const BinaryExpr& binary);
    [[nodiscard]] Type check_cast(const CastExpr& cast);
    void check_function(const FunctionDeclarationStmt& function);
    void check_method(const MethodDeclaration& method, const Type& containing_type);
    void check_conversion(const ConversionOverload& conversion, const Type& source_type);
    void check_block(const BlockStmt& block);
    [[nodiscard]] bool statements_guarantee_return(
        const std::vector<std::unique_ptr<Stmt>>& statements) const;
    [[nodiscard]] bool statement_guarantees_return(const Stmt& statement) const;

    [[nodiscard]] Type resolve_type(const TypeReference& reference) const;
    [[nodiscard]] Type substitute_type(
        const TypeReference& reference,
        const std::unordered_map<std::string, Type>& substitutions) const;
    [[nodiscard]] bool infer_type_arguments(
        const TypeReference& pattern,
        const Type& actual,
        const std::vector<GenericParameter>& generic_parameters,
        std::unordered_map<std::string, Type>& substitutions,
        const std::unordered_set<std::string>& explicit_parameters,
        std::string& failure_reason) const;
    [[nodiscard]] bool validate_constraints(
        const std::vector<GenericParameter>& generic_parameters,
        const std::unordered_map<std::string, Type>& substitutions,
        std::string& failure_reason) const;
    [[nodiscard]] bool is_generic_parameter(const std::string& name) const;
    [[nodiscard]] bool contains_generic_parameter(const TypeReference& reference) const;
    [[nodiscard]] Type require_value(Type type, SourceLocation location) const;
    void require_assignable(Type expected, Type actual, SourceLocation location) const;
    [[nodiscard]] bool is_assignable(const Type& expected, const Type& actual) const;
    [[nodiscard]] bool is_subtype(const std::string& actual, const std::string& expected) const;
    [[nodiscard]] bool signatures_match(
        const FunctionSignature& left,
        const FunctionSignature& right) const;
    [[nodiscard]] bool parameter_types_match(
        const FunctionSignature& left,
        const FunctionSignature& right) const;
    void require_condition(Type type, SourceLocation location) const;

    void push_scope();
    void pop_scope();
    void push_generic_parameters(const std::vector<GenericParameter>& parameters);
    void pop_generic_parameters();
    void declare_value(const std::string& name, Type type);
    void declare_conversion(const std::string& source_name, const Type& target_type);
    [[nodiscard]] std::optional<Type> find_value(const std::string& name) const;
    [[nodiscard]] std::vector<const FunctionSignature*> find_functions(
        const std::string& name) const;
    [[nodiscard]] const NominalTypeInfo* find_nominal_type(const std::string& name) const;
    [[nodiscard]] const FieldInfo* find_field(
        const std::string& type_name,
        const std::string& field_name) const;
    [[nodiscard]] std::vector<const MethodInfo*> find_methods(
        const std::string& type_name,
        const std::string& method_name) const;
    [[nodiscard]] bool can_access(Visibility visibility, const std::string& owner) const;
    [[nodiscard]] bool find_conversion(
        const Type& source_type,
        const Type& target_type) const;

    std::vector<Scope> scopes_;
    std::vector<std::unordered_set<std::string>> generic_parameter_scopes_;
    std::optional<Type> current_return_type_;
    std::optional<std::string> current_type_name_;
    SourceLocation current_location_{1, 1};
};

} // namespace toro
