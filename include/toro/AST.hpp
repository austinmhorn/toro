#pragma once

#include "toro/Token.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace toro {

struct SourceLocation {
    std::size_t line;
    std::size_t column;
};

struct TypeReference {
    std::string name;
    std::vector<TypeReference> arguments;
    SourceLocation location;
};

struct GenericParameter {
    std::string name;
    std::vector<TypeReference> constraints;
    SourceLocation location;
};

enum class ExprKind {
    Integer,
    Decimal,
    String,
    Bool,
    Null,
    Identifier,
    Unary,
    Binary,
    Call,
    MemberAccess,
    Grouping,
};

struct Expr {
    explicit Expr(ExprKind kind)
        : kind(kind)
    {
    }

    virtual ~Expr() = default;

    ExprKind kind;
};

struct IntegerExpr final : Expr {
    explicit IntegerExpr(std::string value)
        : Expr(ExprKind::Integer)
        , value(std::move(value))
    {
    }

    std::string value;
};

struct DecimalExpr final : Expr {
    explicit DecimalExpr(std::string value)
        : Expr(ExprKind::Decimal)
        , value(std::move(value))
    {
    }

    std::string value;
};

struct StringExpr final : Expr {
    explicit StringExpr(std::string value)
        : Expr(ExprKind::String)
        , value(std::move(value))
    {
    }

    std::string value;
};

struct BoolExpr final : Expr {
    explicit BoolExpr(bool value)
        : Expr(ExprKind::Bool)
        , value(value)
    {
    }

    bool value;
};

struct NullExpr final : Expr {
    NullExpr()
        : Expr(ExprKind::Null)
    {
    }
};

struct IdentifierExpr final : Expr {
    explicit IdentifierExpr(std::string name)
        : Expr(ExprKind::Identifier)
        , name(std::move(name))
    {
    }

    std::string name;
};

struct UnaryExpr final : Expr {
    UnaryExpr(Token operator_token, std::unique_ptr<Expr> operand)
        : Expr(ExprKind::Unary)
        , operator_token(std::move(operator_token))
        , operand(std::move(operand))
    {
    }

    Token operator_token;
    std::unique_ptr<Expr> operand;
};

struct BinaryExpr final : Expr {
    BinaryExpr(
        std::unique_ptr<Expr> left,
        Token operator_token,
        std::unique_ptr<Expr> right)
        : Expr(ExprKind::Binary)
        , left(std::move(left))
        , operator_token(std::move(operator_token))
        , right(std::move(right))
    {
    }

    std::unique_ptr<Expr> left;
    Token operator_token;
    std::unique_ptr<Expr> right;
};

struct CallArgument {
    std::optional<std::string> name;
    std::unique_ptr<Expr> value;
};

struct CallExpr final : Expr {
    CallExpr(
        std::unique_ptr<Expr> callee,
        std::vector<TypeReference> generic_arguments,
        std::vector<CallArgument> arguments)
        : Expr(ExprKind::Call)
        , callee(std::move(callee))
        , generic_arguments(std::move(generic_arguments))
        , arguments(std::move(arguments))
    {
    }

    std::unique_ptr<Expr> callee;
    std::vector<TypeReference> generic_arguments;
    std::vector<CallArgument> arguments;
};

struct MemberAccessExpr final : Expr {
    MemberAccessExpr(std::unique_ptr<Expr> object, std::string member)
        : Expr(ExprKind::MemberAccess)
        , object(std::move(object))
        , member(std::move(member))
    {
    }

    std::unique_ptr<Expr> object;
    std::string member;
};

struct GroupingExpr final : Expr {
    explicit GroupingExpr(std::unique_ptr<Expr> expression)
        : Expr(ExprKind::Grouping)
        , expression(std::move(expression))
    {
    }

    std::unique_ptr<Expr> expression;
};

enum class StmtKind {
    VariableDeclaration,
    Assignment,
    MemberAssignment,
    Expression,
    FunctionDeclaration,
    Return,
    Block,
    If,
    While,
    ForIn,
    Stop,
    Continue,
    EnumDeclaration,
    Handle,
    StructDeclaration,
    ClassDeclaration,
    InterfaceDeclaration,
};

struct Stmt {
    Stmt(StmtKind kind, SourceLocation location)
        : kind(kind)
        , location(location)
    {
    }

    virtual ~Stmt() = default;

    StmtKind kind;
    SourceLocation location;
};

struct VariableDeclarationStmt final : Stmt {
    VariableDeclarationStmt(
        SourceLocation location,
        std::string name,
        std::optional<TypeReference> explicit_type,
        std::unique_ptr<Expr> initializer)
        : Stmt(StmtKind::VariableDeclaration, location)
        , name(std::move(name))
        , explicit_type(std::move(explicit_type))
        , initializer(std::move(initializer))
    {
    }

    std::string name;
    std::optional<TypeReference> explicit_type;
    std::unique_ptr<Expr> initializer;
};

struct AssignmentStmt final : Stmt {
    AssignmentStmt(
        SourceLocation location,
        std::string name,
        std::unique_ptr<Expr> value)
        : Stmt(StmtKind::Assignment, location)
        , name(std::move(name))
        , value(std::move(value))
    {
    }

    std::string name;
    std::unique_ptr<Expr> value;
};

struct MemberAssignmentStmt final : Stmt {
    MemberAssignmentStmt(
        SourceLocation location,
        std::unique_ptr<MemberAccessExpr> target,
        std::unique_ptr<Expr> value)
        : Stmt(StmtKind::MemberAssignment, location)
        , target(std::move(target))
        , value(std::move(value))
    {
    }

    std::unique_ptr<MemberAccessExpr> target;
    std::unique_ptr<Expr> value;
};

struct ExpressionStmt final : Stmt {
    ExpressionStmt(SourceLocation location, std::unique_ptr<Expr> expression)
        : Stmt(StmtKind::Expression, location)
        , expression(std::move(expression))
    {
    }

    std::unique_ptr<Expr> expression;
};

struct Parameter {
    std::string name;
    TypeReference type;
    SourceLocation location;
};

struct BlockStmt final : Stmt {
    BlockStmt(SourceLocation location, std::vector<std::unique_ptr<Stmt>> statements)
        : Stmt(StmtKind::Block, location)
        , statements(std::move(statements))
    {
    }

    std::vector<std::unique_ptr<Stmt>> statements;
};

struct FunctionDeclarationStmt final : Stmt {
    FunctionDeclarationStmt(
        SourceLocation location,
        std::string name,
        std::vector<GenericParameter> generic_parameters,
        std::vector<Parameter> parameters,
        std::optional<TypeReference> return_type,
        std::unique_ptr<BlockStmt> body)
        : Stmt(StmtKind::FunctionDeclaration, location)
        , name(std::move(name))
        , generic_parameters(std::move(generic_parameters))
        , parameters(std::move(parameters))
        , return_type(std::move(return_type))
        , body(std::move(body))
    {
    }

    std::string name;
    std::vector<GenericParameter> generic_parameters;
    std::vector<Parameter> parameters;
    std::optional<TypeReference> return_type;
    std::unique_ptr<BlockStmt> body;
};

struct ReturnStmt final : Stmt {
    ReturnStmt(SourceLocation location, std::unique_ptr<Expr> value)
        : Stmt(StmtKind::Return, location)
        , value(std::move(value))
    {
    }

    std::unique_ptr<Expr> value;
};

struct IfStmt final : Stmt {
    IfStmt(
        SourceLocation location,
        std::unique_ptr<Expr> condition,
        std::unique_ptr<BlockStmt> then_block,
        std::unique_ptr<Stmt> else_branch)
        : Stmt(StmtKind::If, location)
        , condition(std::move(condition))
        , then_block(std::move(then_block))
        , else_branch(std::move(else_branch))
    {
    }

    std::unique_ptr<Expr> condition;
    std::unique_ptr<BlockStmt> then_block;
    std::unique_ptr<Stmt> else_branch;
};

struct WhileStmt final : Stmt {
    WhileStmt(
        SourceLocation location,
        std::unique_ptr<Expr> condition,
        std::unique_ptr<BlockStmt> body)
        : Stmt(StmtKind::While, location)
        , condition(std::move(condition))
        , body(std::move(body))
    {
    }

    std::unique_ptr<Expr> condition;
    std::unique_ptr<BlockStmt> body;
};

struct ForInStmt final : Stmt {
    ForInStmt(
        SourceLocation location,
        std::string variable_name,
        std::unique_ptr<Expr> collection,
        std::unique_ptr<BlockStmt> body)
        : Stmt(StmtKind::ForIn, location)
        , variable_name(std::move(variable_name))
        , collection(std::move(collection))
        , body(std::move(body))
    {
    }

    std::string variable_name;
    std::unique_ptr<Expr> collection;
    std::unique_ptr<BlockStmt> body;
};

struct StopStmt final : Stmt {
    explicit StopStmt(SourceLocation location)
        : Stmt(StmtKind::Stop, location)
    {
    }
};

struct ContinueStmt final : Stmt {
    explicit ContinueStmt(SourceLocation location)
        : Stmt(StmtKind::Continue, location)
    {
    }
};

struct EnumVariant {
    std::string name;
    std::optional<TypeReference> payload_type;
    SourceLocation location;
};

struct EnumDeclarationStmt final : Stmt {
    EnumDeclarationStmt(
        SourceLocation location,
        std::string name,
        std::vector<EnumVariant> variants)
        : Stmt(StmtKind::EnumDeclaration, location)
        , name(std::move(name))
        , variants(std::move(variants))
    {
    }

    std::string name;
    std::vector<EnumVariant> variants;
};

struct HandleCase {
    std::string variant_name;
    std::optional<std::string> binding_name;
    std::unique_ptr<BlockStmt> body;
    SourceLocation location;
};

struct HandleStmt final : Stmt {
    HandleStmt(
        SourceLocation location,
        std::unique_ptr<Expr> expression,
        std::vector<HandleCase> cases)
        : Stmt(StmtKind::Handle, location)
        , expression(std::move(expression))
        , cases(std::move(cases))
    {
    }

    std::unique_ptr<Expr> expression;
    std::vector<HandleCase> cases;
};

struct StructField {
    std::string name;
    TypeReference type;
    std::unique_ptr<Expr> default_value;
    SourceLocation location;
};

struct StructDeclarationStmt final : Stmt {
    StructDeclarationStmt(
        SourceLocation location,
        std::string name,
        std::vector<GenericParameter> generic_parameters,
        std::vector<TypeReference> interfaces,
        std::vector<StructField> fields)
        : Stmt(StmtKind::StructDeclaration, location)
        , name(std::move(name))
        , generic_parameters(std::move(generic_parameters))
        , interfaces(std::move(interfaces))
        , fields(std::move(fields))
    {
    }

    std::string name;
    std::vector<GenericParameter> generic_parameters;
    std::vector<TypeReference> interfaces;
    std::vector<StructField> fields;
};

enum class Visibility {
    Private,
    Public,
};

enum class ClassMemberKind {
    Field,
    Method,
};

struct ClassMember {
    ClassMember(ClassMemberKind kind, Visibility visibility, SourceLocation location)
        : kind(kind)
        , visibility(visibility)
        , location(location)
    {
    }

    virtual ~ClassMember() = default;

    ClassMemberKind kind;
    Visibility visibility;
    SourceLocation location;
};

struct ClassField final : ClassMember {
    ClassField(
        Visibility visibility,
        SourceLocation location,
        std::string name,
        TypeReference type,
        std::unique_ptr<Expr> default_value)
        : ClassMember(ClassMemberKind::Field, visibility, location)
        , name(std::move(name))
        , type(std::move(type))
        , default_value(std::move(default_value))
    {
    }

    std::string name;
    TypeReference type;
    std::unique_ptr<Expr> default_value;
};

struct MethodDeclaration final : ClassMember {
    MethodDeclaration(
        Visibility visibility,
        SourceLocation location,
        std::string name,
        std::vector<GenericParameter> generic_parameters,
        std::vector<Parameter> parameters,
        std::optional<TypeReference> return_type,
        bool is_virtual,
        bool is_override,
        std::unique_ptr<BlockStmt> body)
        : ClassMember(ClassMemberKind::Method, visibility, location)
        , name(std::move(name))
        , generic_parameters(std::move(generic_parameters))
        , parameters(std::move(parameters))
        , return_type(std::move(return_type))
        , is_virtual(is_virtual)
        , is_override(is_override)
        , body(std::move(body))
    {
    }

    std::string name;
    std::vector<GenericParameter> generic_parameters;
    std::vector<Parameter> parameters;
    std::optional<TypeReference> return_type;
    bool is_virtual;
    bool is_override;
    std::unique_ptr<BlockStmt> body;
};

struct ClassDeclarationStmt final : Stmt {
    ClassDeclarationStmt(
        SourceLocation location,
        std::string name,
        bool is_abstract,
        std::vector<GenericParameter> generic_parameters,
        std::optional<TypeReference> base_type,
        std::vector<TypeReference> interfaces,
        std::vector<std::unique_ptr<ClassMember>> members)
        : Stmt(StmtKind::ClassDeclaration, location)
        , name(std::move(name))
        , is_abstract(is_abstract)
        , generic_parameters(std::move(generic_parameters))
        , base_type(std::move(base_type))
        , interfaces(std::move(interfaces))
        , members(std::move(members))
    {
    }

    std::string name;
    bool is_abstract;
    std::vector<GenericParameter> generic_parameters;
    std::optional<TypeReference> base_type;
    std::vector<TypeReference> interfaces;
    std::vector<std::unique_ptr<ClassMember>> members;
};

struct InterfaceMethod {
    std::string name;
    std::vector<GenericParameter> generic_parameters;
    std::vector<Parameter> parameters;
    std::optional<TypeReference> return_type;
    SourceLocation location;
};

struct InterfaceDeclarationStmt final : Stmt {
    InterfaceDeclarationStmt(
        SourceLocation location,
        std::string name,
        std::vector<GenericParameter> generic_parameters,
        std::vector<InterfaceMethod> methods)
        : Stmt(StmtKind::InterfaceDeclaration, location)
        , name(std::move(name))
        , generic_parameters(std::move(generic_parameters))
        , methods(std::move(methods))
    {
    }

    std::string name;
    std::vector<GenericParameter> generic_parameters;
    std::vector<InterfaceMethod> methods;
};

struct Program {
    std::vector<std::unique_ptr<Stmt>> statements;
};

[[nodiscard]] std::string dump_expression(const Expr& expression);
[[nodiscard]] std::string dump_program(const Program& program);

} // namespace toro
