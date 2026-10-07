#pragma once

#include "toro/AST.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
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
    std::string module_name;
    bool is_public;
};

class SemanticAnalyzer {
public:
    void analyze(const Program& program);

private:
    using Scope = std::unordered_map<std::string, std::vector<Symbol>>;

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
    void declare(
        const std::string& name,
        SymbolKind kind,
        SourceLocation location,
        std::string module_name = {},
        bool is_public = false);
    [[nodiscard]] bool resolve(const std::string& name) const;
    [[nodiscard]] bool is_accessible(const Symbol& symbol) const;

    std::vector<Scope> scopes_;
    std::unordered_map<std::string, std::unordered_set<std::string>> module_imports_;
    std::string current_module_;
    SourceLocation current_location_{1, 1};
    bool inside_class_method_{false};
};

} // namespace toro
