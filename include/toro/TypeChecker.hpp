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
    };

    struct FunctionSignature {
        std::vector<FunctionParameterType> parameters;
        Type return_type;
    };

    struct Scope {
        std::unordered_map<std::string, Type> values;
        std::unordered_map<std::string, FunctionSignature> functions;
    };

    void check_statement_list(const std::vector<std::unique_ptr<Stmt>>& statements);
    void predeclare(const std::vector<std::unique_ptr<Stmt>>& statements);
    void check_statement(const Stmt& statement);
    [[nodiscard]] Type check_expression(const Expr& expression);
    [[nodiscard]] Type check_call(const CallExpr& call);
    [[nodiscard]] Type check_binary(const BinaryExpr& binary);
    void check_function(const FunctionDeclarationStmt& function);
    void check_method(const MethodDeclaration& method);
    void check_block(const BlockStmt& block);

    [[nodiscard]] Type resolve_type(const TypeReference& reference) const;
    [[nodiscard]] bool is_generic_parameter(const std::string& name) const;
    [[nodiscard]] bool contains_generic_parameter(const TypeReference& reference) const;
    [[nodiscard]] Type require_value(Type type, SourceLocation location) const;
    void require_assignable(Type expected, Type actual, SourceLocation location) const;
    void require_condition(Type type, SourceLocation location) const;

    void push_scope();
    void pop_scope();
    void push_generic_parameters(const std::vector<GenericParameter>& parameters);
    void pop_generic_parameters();
    void declare_value(const std::string& name, Type type);
    [[nodiscard]] std::optional<Type> find_value(const std::string& name) const;
    [[nodiscard]] const FunctionSignature* find_function(const std::string& name) const;

    std::vector<Scope> scopes_;
    std::vector<std::unordered_set<std::string>> generic_parameter_scopes_;
    std::optional<Type> current_return_type_;
    SourceLocation current_location_{1, 1};
};

} // namespace toro
