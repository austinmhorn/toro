#pragma once

#include "toro/IR.hpp"
#include "toro/Token.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace toro::detail::c_backend {

struct TypeReference {
    std::string name;
    std::vector<TypeReference> arguments;
    SourceLocation location{1, 1};
    bool nullable{false};
    std::optional<Type> resolved_type;
};

struct GenericParameter {
    std::string name;
    std::vector<TypeReference> constraints;
    SourceLocation location{1, 1};
};

enum class ExprKind {
    Integer, Decimal, String, Bool, Null, Identifier, Unary, Binary, Call,
    Index, MemberAccess, TypeAccess, Propagation, Cast, Grouping,
};

struct Expr {
    explicit Expr(ExprKind value_kind) : kind(value_kind) {}
    virtual ~Expr() = default;
    ExprKind kind;
    std::optional<Type> resolved_type;
};

struct IntegerExpr final : Expr {
    explicit IntegerExpr(std::string value)
        : Expr(ExprKind::Integer), value(std::move(value)) {}
    std::string value;
};
struct DecimalExpr final : Expr {
    explicit DecimalExpr(std::string value)
        : Expr(ExprKind::Decimal), value(std::move(value)) {}
    std::string value;
};
struct StringExpr final : Expr {
    explicit StringExpr(std::string value)
        : Expr(ExprKind::String), value(std::move(value)) {}
    std::string value;
};
struct BoolExpr final : Expr {
    explicit BoolExpr(bool value) : Expr(ExprKind::Bool), value(value) {}
    bool value;
};
struct NullExpr final : Expr { NullExpr() : Expr(ExprKind::Null) {} };
struct IdentifierExpr final : Expr {
    explicit IdentifierExpr(std::string name)
        : Expr(ExprKind::Identifier), name(std::move(name)) {}
    std::string name;
};
struct UnaryExpr final : Expr {
    UnaryExpr(Token op, std::unique_ptr<Expr> operand)
        : Expr(ExprKind::Unary), operator_token(std::move(op)),
          operand(std::move(operand)) {}
    Token operator_token;
    std::unique_ptr<Expr> operand;
};
struct BinaryExpr final : Expr {
    BinaryExpr(std::unique_ptr<Expr> left, Token op,
        std::unique_ptr<Expr> right)
        : Expr(ExprKind::Binary), left(std::move(left)),
          operator_token(std::move(op)), right(std::move(right)) {}
    std::unique_ptr<Expr> left;
    Token operator_token;
    std::unique_ptr<Expr> right;
};

struct FunctionDeclarationStmt;
struct MethodDeclaration;
struct InterfaceMethod;
struct ConversionOverload;

struct CallArgument {
    std::optional<std::string> name;
    std::unique_ptr<Expr> value;
};
struct CallExpr final : Expr {
    CallExpr(std::unique_ptr<Expr> callee,
        std::vector<TypeReference> generic_arguments,
        std::vector<CallArgument> arguments)
        : Expr(ExprKind::Call), callee(std::move(callee)),
          generic_arguments(std::move(generic_arguments)),
          arguments(std::move(arguments)) {}
    std::unique_ptr<Expr> callee;
    std::vector<TypeReference> generic_arguments;
    std::vector<CallArgument> arguments;
    std::vector<std::pair<std::string, Type>> resolved_substitutions;
    const FunctionDeclarationStmt* resolved_function{nullptr};
    const MethodDeclaration* resolved_method{nullptr};
    const InterfaceMethod* resolved_interface_method{nullptr};
    std::optional<std::string> transfer_source;
    bool stack_storage{false};
};
struct IndexExpr final : Expr {
    IndexExpr(std::unique_ptr<Expr> object, std::unique_ptr<Expr> index)
        : Expr(ExprKind::Index), object(std::move(object)), index(std::move(index)) {}
    std::unique_ptr<Expr> object;
    std::unique_ptr<Expr> index;
};
struct MemberAccessExpr final : Expr {
    MemberAccessExpr(std::unique_ptr<Expr> object, std::string member)
        : Expr(ExprKind::MemberAccess), object(std::move(object)),
          member(std::move(member)) {}
    std::unique_ptr<Expr> object;
    std::string member;
};
struct TypeAccessExpr final : Expr {
    TypeAccessExpr(std::string type_name, std::string member)
        : Expr(ExprKind::TypeAccess), type_name(std::move(type_name)),
          member(std::move(member)) {}
    std::string type_name;
    std::string member;
};
struct PropagationExpr final : Expr {
    explicit PropagationExpr(std::unique_ptr<Expr> expression)
        : Expr(ExprKind::Propagation), expression(std::move(expression)) {}
    std::unique_ptr<Expr> expression;
};
struct CastExpr final : Expr {
    CastExpr(std::unique_ptr<Expr> expression, TypeReference target_type)
        : Expr(ExprKind::Cast), expression(std::move(expression)),
          target_type(std::move(target_type)) {}
    std::unique_ptr<Expr> expression;
    TypeReference target_type;
    const ConversionOverload* resolved_conversion{nullptr};
};
struct GroupingExpr final : Expr {
    explicit GroupingExpr(std::unique_ptr<Expr> expression)
        : Expr(ExprKind::Grouping), expression(std::move(expression)) {}
    std::unique_ptr<Expr> expression;
};

enum class StmtKind {
    ImportDeclaration, VariableDeclaration, Assignment, MemberAssignment,
    IndexAssignment, Expression, FunctionDeclaration, Return, Block, If,
    While, ForIn, Stop, Continue, EnumDeclaration, Handle,
    StructDeclaration, ClassDeclaration, InterfaceDeclaration,
};
struct Stmt {
    Stmt(StmtKind value_kind, SourceLocation value_location)
        : kind(value_kind), location(value_location) {}
    virtual ~Stmt() = default;
    StmtKind kind;
    SourceLocation location;
    std::string module_name;
    bool is_public{false};
    std::optional<std::string> transfer_source;
};
struct ImportDeclarationStmt final : Stmt {
    ImportDeclarationStmt(SourceLocation location, std::vector<std::string> path)
        : Stmt(StmtKind::ImportDeclaration, location), path(std::move(path)) {}
    std::vector<std::string> path;
};
struct VariableDeclarationStmt final : Stmt {
    VariableDeclarationStmt(SourceLocation location, std::string name,
        std::optional<TypeReference> explicit_type,
        std::unique_ptr<Expr> initializer)
        : Stmt(StmtKind::VariableDeclaration, location), name(std::move(name)),
          explicit_type(std::move(explicit_type)), initializer(std::move(initializer)) {}
    std::string name;
    std::optional<TypeReference> explicit_type;
    std::unique_ptr<Expr> initializer;
};
struct AssignmentStmt final : Stmt {
    AssignmentStmt(SourceLocation location, std::string name,
        std::unique_ptr<Expr> value)
        : Stmt(StmtKind::Assignment, location), name(std::move(name)),
          value(std::move(value)) {}
    std::string name;
    std::unique_ptr<Expr> value;
};
struct MemberAssignmentStmt final : Stmt {
    MemberAssignmentStmt(SourceLocation location,
        std::unique_ptr<MemberAccessExpr> target, std::unique_ptr<Expr> value)
        : Stmt(StmtKind::MemberAssignment, location), target(std::move(target)),
          value(std::move(value)) {}
    std::unique_ptr<MemberAccessExpr> target;
    std::unique_ptr<Expr> value;
};
struct IndexAssignmentStmt final : Stmt {
    IndexAssignmentStmt(SourceLocation location, std::unique_ptr<IndexExpr> target,
        std::unique_ptr<Expr> value)
        : Stmt(StmtKind::IndexAssignment, location), target(std::move(target)),
          value(std::move(value)) {}
    std::unique_ptr<IndexExpr> target;
    std::unique_ptr<Expr> value;
};
struct ExpressionStmt final : Stmt {
    ExpressionStmt(SourceLocation location, std::unique_ptr<Expr> expression)
        : Stmt(StmtKind::Expression, location), expression(std::move(expression)) {}
    std::unique_ptr<Expr> expression;
};
struct Parameter { std::string name; TypeReference type; SourceLocation location; };
struct BlockStmt final : Stmt {
    BlockStmt(SourceLocation location, std::vector<std::unique_ptr<Stmt>> statements)
        : Stmt(StmtKind::Block, location), statements(std::move(statements)) {}
    std::vector<std::unique_ptr<Stmt>> statements;
};
struct FunctionDeclarationStmt final : Stmt {
    FunctionDeclarationStmt(SourceLocation location, std::string name,
        std::vector<GenericParameter> generic_parameters,
        std::vector<Parameter> parameters,
        std::optional<TypeReference> return_type,
        std::unique_ptr<BlockStmt> body, bool is_external = false,
        std::string calling_convention = {})
        : Stmt(StmtKind::FunctionDeclaration, location), name(std::move(name)),
          generic_parameters(std::move(generic_parameters)),
          parameters(std::move(parameters)), return_type(std::move(return_type)),
          body(std::move(body)), is_external(is_external),
          calling_convention(std::move(calling_convention)) {}
    std::string name;
    std::vector<GenericParameter> generic_parameters;
    std::vector<Parameter> parameters;
    std::optional<TypeReference> return_type;
    std::unique_ptr<BlockStmt> body;
    bool is_external;
    std::string calling_convention;
    std::string identity;
    std::vector<std::string> borrowed_parameters;
};
struct ReturnStmt final : Stmt {
    ReturnStmt(SourceLocation location, std::unique_ptr<Expr> value)
        : Stmt(StmtKind::Return, location), value(std::move(value)) {}
    std::unique_ptr<Expr> value;
};
struct IfStmt final : Stmt {
    IfStmt(SourceLocation location, std::unique_ptr<Expr> condition,
        std::unique_ptr<BlockStmt> then_block, std::unique_ptr<Stmt> else_branch)
        : Stmt(StmtKind::If, location), condition(std::move(condition)),
          then_block(std::move(then_block)), else_branch(std::move(else_branch)) {}
    std::unique_ptr<Expr> condition;
    std::unique_ptr<BlockStmt> then_block;
    std::unique_ptr<Stmt> else_branch;
};
struct WhileStmt final : Stmt {
    WhileStmt(SourceLocation location, std::unique_ptr<Expr> condition,
        std::unique_ptr<BlockStmt> body)
        : Stmt(StmtKind::While, location), condition(std::move(condition)),
          body(std::move(body)) {}
    std::unique_ptr<Expr> condition;
    std::unique_ptr<BlockStmt> body;
};
struct ForInStmt final : Stmt {
    ForInStmt(SourceLocation location, std::string variable_name,
        std::unique_ptr<Expr> collection, std::unique_ptr<BlockStmt> body)
        : Stmt(StmtKind::ForIn, location), variable_name(std::move(variable_name)),
          collection(std::move(collection)), body(std::move(body)) {}
    std::string variable_name;
    std::unique_ptr<Expr> collection;
    std::unique_ptr<BlockStmt> body;
};
struct StopStmt final : Stmt { explicit StopStmt(SourceLocation location) : Stmt(StmtKind::Stop, location) {} };
struct ContinueStmt final : Stmt { explicit ContinueStmt(SourceLocation location) : Stmt(StmtKind::Continue, location) {} };
struct EnumVariant { std::string name; std::optional<TypeReference> payload_type; SourceLocation location; };
struct EnumDeclarationStmt final : Stmt {
    EnumDeclarationStmt(SourceLocation location, std::string name,
        std::vector<EnumVariant> variants)
        : Stmt(StmtKind::EnumDeclaration, location), name(std::move(name)),
          variants(std::move(variants)) {}
    std::string name;
    std::vector<EnumVariant> variants;
    std::string identity;
};
struct HandleCase {
    std::string variant_name;
    std::optional<std::string> binding_name;
    std::unique_ptr<BlockStmt> body;
    SourceLocation location;
};
struct HandleStmt final : Stmt {
    HandleStmt(SourceLocation location, std::unique_ptr<Expr> expression,
        std::vector<HandleCase> cases)
        : Stmt(StmtKind::Handle, location), expression(std::move(expression)),
          cases(std::move(cases)) {}
    std::unique_ptr<Expr> expression;
    std::vector<HandleCase> cases;
};
enum class Visibility { Private, Public };
enum class ClassMemberKind { Field, Method, Conversion };
struct ClassMember {
    ClassMember(ClassMemberKind kind, Visibility visibility, SourceLocation location)
        : kind(kind), visibility(visibility), location(location) {}
    virtual ~ClassMember() = default;
    ClassMemberKind kind;
    Visibility visibility;
    SourceLocation location;
};
struct ConversionOverload final : ClassMember {
    ConversionOverload(SourceLocation location, TypeReference target_type,
        std::unique_ptr<BlockStmt> body)
        : ClassMember(ClassMemberKind::Conversion, Visibility::Private, location),
          target_type(std::move(target_type)), body(std::move(body)) {}
    TypeReference target_type;
    std::unique_ptr<BlockStmt> body;
    std::string identity;
};
struct StructField { std::string name; TypeReference type; std::unique_ptr<Expr> default_value; SourceLocation location; };
struct MethodDeclaration final : ClassMember {
    MethodDeclaration(Visibility visibility, SourceLocation location,
        std::string name, std::vector<GenericParameter> generic_parameters,
        std::vector<Parameter> parameters,
        std::optional<TypeReference> return_type, bool is_virtual,
        bool is_override, std::unique_ptr<BlockStmt> body)
        : ClassMember(ClassMemberKind::Method, visibility, location),
          name(std::move(name)), generic_parameters(std::move(generic_parameters)),
          parameters(std::move(parameters)), return_type(std::move(return_type)),
          is_virtual(is_virtual), is_override(is_override), body(std::move(body)) {}
    std::string name;
    std::vector<GenericParameter> generic_parameters;
    std::vector<Parameter> parameters;
    std::optional<TypeReference> return_type;
    bool is_virtual;
    bool is_override;
    std::unique_ptr<BlockStmt> body;
    std::string identity;
    std::vector<std::string> borrowed_parameters;
};
struct InterfaceMethod {
    std::string name;
    std::vector<GenericParameter> generic_parameters;
    std::vector<Parameter> parameters;
    std::optional<TypeReference> return_type;
    SourceLocation location;
    std::string identity;
};
struct ClassField final : ClassMember {
    ClassField(Visibility visibility, bool is_weak, SourceLocation location,
        std::string name, TypeReference type, std::unique_ptr<Expr> default_value)
        : ClassMember(ClassMemberKind::Field, visibility, location),
          is_weak(is_weak), name(std::move(name)), type(std::move(type)),
          default_value(std::move(default_value)) {}
    bool is_weak;
    std::string name;
    TypeReference type;
    std::unique_ptr<Expr> default_value;
};
struct StructDeclarationStmt final : Stmt {
    StructDeclarationStmt(SourceLocation location, std::string name,
        std::vector<GenericParameter> generic_parameters,
        std::vector<TypeReference> interfaces, std::vector<StructField> fields,
        std::vector<std::unique_ptr<ClassMember>> methods,
        std::vector<std::unique_ptr<ConversionOverload>> conversions)
        : Stmt(StmtKind::StructDeclaration, location), name(std::move(name)),
          generic_parameters(std::move(generic_parameters)),
          interfaces(std::move(interfaces)), fields(std::move(fields)),
          methods(std::move(methods)), conversions(std::move(conversions)) {}
    std::string name;
    std::vector<GenericParameter> generic_parameters;
    std::vector<TypeReference> interfaces;
    std::vector<StructField> fields;
    std::vector<std::unique_ptr<ClassMember>> methods;
    std::vector<std::unique_ptr<ConversionOverload>> conversions;
    std::string identity;
};
struct ClassDeclarationStmt final : Stmt {
    ClassDeclarationStmt(SourceLocation location, std::string name,
        bool is_abstract, std::vector<GenericParameter> generic_parameters,
        std::optional<TypeReference> base_type,
        std::vector<TypeReference> interfaces,
        std::vector<std::unique_ptr<ClassMember>> members)
        : Stmt(StmtKind::ClassDeclaration, location), name(std::move(name)),
          is_abstract(is_abstract), generic_parameters(std::move(generic_parameters)),
          base_type(std::move(base_type)), interfaces(std::move(interfaces)),
          members(std::move(members)) {}
    std::string name;
    bool is_abstract;
    std::vector<GenericParameter> generic_parameters;
    std::optional<TypeReference> base_type;
    std::vector<TypeReference> interfaces;
    std::vector<std::unique_ptr<ClassMember>> members;
    std::string identity;
};
struct InterfaceDeclarationStmt final : Stmt {
    InterfaceDeclarationStmt(SourceLocation location, std::string name,
        std::vector<GenericParameter> generic_parameters,
        std::vector<InterfaceMethod> methods)
        : Stmt(StmtKind::InterfaceDeclaration, location), name(std::move(name)),
          generic_parameters(std::move(generic_parameters)), methods(std::move(methods)) {}
    std::string name;
    std::vector<GenericParameter> generic_parameters;
    std::vector<InterfaceMethod> methods;
    std::string identity;
};
struct Program {
    std::vector<std::unique_ptr<Stmt>> statements;
    std::string entry_module;
};

[[nodiscard]] Program make_emission_model(const IRProgram& program);

} // namespace toro::detail::c_backend
