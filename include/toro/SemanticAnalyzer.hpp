#pragma once

#include "toro/AST.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace toro {

enum class SymbolKind {
    Variable,
    Function,
    Parameter,
    Struct,
    Class,
    Interface,
    Enum,
    LoopVariable,
    HandleBinding,
    Builtin,
};

struct Symbol {
    SymbolKind kind;
    SourceLocation location;
};

class SemanticAnalyzer {
public:
    void analyze(const Program& program);

private:
    using Scope = std::unordered_map<std::string, Symbol>;

    void analyze_statement_list(const std::vector<std::unique_ptr<Stmt>>& statements);
    void predeclare(const std::vector<std::unique_ptr<Stmt>>& statements);
    void analyze_statement(const Stmt& statement);
    void analyze_expression(const Expr& expression);
    void analyze_function(const FunctionDeclarationStmt& function);
    void analyze_method(const MethodDeclaration& method);
    void analyze_conversion(const ConversionOverload& conversion);
    void analyze_block(const BlockStmt& block);

    void push_scope();
    void pop_scope();
    void declare(const std::string& name, SymbolKind kind, SourceLocation location);
    [[nodiscard]] bool resolve(const std::string& name) const;

    std::vector<Scope> scopes_;
    SourceLocation current_location_{1, 1};
    bool inside_class_method_{false};
};

} // namespace toro
