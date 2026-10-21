#include "toro/IRLowerer.hpp"

#include "toro/ModuleIdentity.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace toro {
namespace {

const Type void_type{TypeKind::Void};

std::string type_identity(const Type& type)
{
    std::string result = type.kind == TypeKind::Unknown && !type.name.empty()
        ? type.name
        : std::string(base_type_name(type));
    if (!type.arguments.empty()) {
        result += '<';
        for (std::size_t index = 0; index < type.arguments.size(); ++index) {
            if (index != 0) result += ',';
            result += type_identity(type.arguments[index]);
        }
        result += '>';
    }
    if (type.nullable) result += '?';
    return result;
}

std::string callable_identity(
    std::string_view module,
    std::string_view owner,
    std::string_view name,
    const std::vector<Parameter>& parameters)
{
    std::string result = owner.empty()
        ? declaration_identity(module, name)
        : std::string(owner) + "::" + std::string(name);
    result += '(';
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        if (index != 0) result += ',';
        const auto& reference = parameters[index].type;
        result += reference.resolved_type
            ? type_identity(*reference.resolved_type)
            : reference.name;
    }
    result += ')';
    return result;
}

[[noreturn]] void throw_ir_error(
    const IROrigin& origin,
    const std::string& message)
{
    throw std::runtime_error(
        "IR lowering error at " + std::to_string(origin.location.line) + ':'
        + std::to_string(origin.location.column) + ": " + message);
}

class LoweringContext {
public:
    IRProgram lower(const Program& program)
    {
        program_ = &program;
        collect_nominal_kinds();
        collect_callable_identities();

        IRProgram result;
        result.entry_module = program.entry_module;
        result.modules.reserve(program.modules.size());
        for (const auto& module : program.modules) {
            result.modules.push_back(IRModule{module.name, module.imports});
        }
        for (const auto& statement : program.statements) {
            lower_top_level(*statement, result);
        }
        return result;
    }

private:
    IROrigin origin(
        std::string_view module,
        SourceLocation location) const
    {
        return IROrigin{std::string(module), location};
    }

    IRValueCategory category(const Type& type) const
    {
        switch (type.kind) {
        case TypeKind::Int:
        case TypeKind::Dec:
        case TypeKind::String:
        case TypeKind::Bool:
        case TypeKind::CInt:
        case TypeKind::CInt64:
        case TypeKind::CDouble:
        case TypeKind::CBool:
        case TypeKind::CString:
        case TypeKind::Null:
        case TypeKind::Void:
            return IRValueCategory::Primitive;
        case TypeKind::Unknown:
            break;
        }
        if (type.name == "Result") return IRValueCategory::ResultValue;
        if (type.name == "Array" || type.name == "List" || type.name == "Map") {
            return IRValueCategory::CollectionValue;
        }
        if (class_types_.contains(type.name)) {
            return IRValueCategory::ManagedReference;
        }
        if (interface_types_.contains(type.name)) {
            return IRValueCategory::InterfaceValue;
        }
        if (struct_types_.contains(type.name) || enum_types_.contains(type.name)) {
            return IRValueCategory::Value;
        }
        return IRValueCategory::Unknown;
    }

    IRType lower_type(const Type& type) const
    {
        return IRType{type, category(type)};
    }

    IRType lower_type(
        const TypeReference& reference,
        const IROrigin& value_origin) const
    {
        if (reference.resolved_type) {
            return lower_type(*reference.resolved_type);
        }
        std::vector<Type> arguments;
        arguments.reserve(reference.arguments.size());
        for (const auto& argument : reference.arguments) {
            arguments.push_back(lower_type(argument, value_origin).type);
        }
        // Generic parameter references intentionally remain deferred in the
        // typed AST. Preserve that frontend decision rather than resolving it
        // again in the IR layer.
        return lower_type(Type{
            TypeKind::Unknown,
            reference.nullable,
            true,
            reference.name,
            std::move(arguments),
        });
    }

    IRType expression_type(
        const Expr& expression,
        const IROrigin& value_origin) const
    {
        if (!expression.resolved_type) {
            throw_ir_error(value_origin,
                "expression was not assigned a concrete frontend type");
        }
        return lower_type(*expression.resolved_type);
    }

    void collect_nominal_kinds()
    {
        for (const auto& statement : program_->statements) {
            const auto identity_for = [&](std::string_view name) {
                return declaration_identity(statement->module_name, name);
            };
            switch (statement->kind) {
            case StmtKind::StructDeclaration:
                struct_types_.insert(identity_for(
                    static_cast<const StructDeclarationStmt&>(*statement).name));
                break;
            case StmtKind::ClassDeclaration:
                class_types_.insert(identity_for(
                    static_cast<const ClassDeclarationStmt&>(*statement).name));
                break;
            case StmtKind::InterfaceDeclaration:
                interface_types_.insert(identity_for(
                    static_cast<const InterfaceDeclarationStmt&>(*statement).name));
                break;
            case StmtKind::EnumDeclaration:
                enum_types_.insert(identity_for(
                    static_cast<const EnumDeclarationStmt&>(*statement).name));
                break;
            default:
                break;
            }
        }
    }

    void collect_callable_identities()
    {
        for (const auto& statement : program_->statements) {
            const std::string& module = statement->module_name;
            if (statement->kind == StmtKind::FunctionDeclaration) {
                const auto& function =
                    static_cast<const FunctionDeclarationStmt&>(*statement);
                callable_identities_[&function] = callable_identity(
                    module, {}, function.name, function.parameters);
                continue;
            }
            if (statement->kind == StmtKind::StructDeclaration) {
                const auto& declaration =
                    static_cast<const StructDeclarationStmt&>(*statement);
                const std::string owner = declaration_identity(module, declaration.name);
                collect_struct_callables(declaration, module, owner);
                continue;
            }
            if (statement->kind == StmtKind::ClassDeclaration) {
                const auto& declaration =
                    static_cast<const ClassDeclarationStmt&>(*statement);
                const std::string owner = declaration_identity(module, declaration.name);
                collect_class_callables(declaration, module, owner);
                continue;
            }
            if (statement->kind == StmtKind::InterfaceDeclaration) {
                const auto& declaration =
                    static_cast<const InterfaceDeclarationStmt&>(*statement);
                const std::string owner = declaration_identity(module, declaration.name);
                for (const auto& method : declaration.methods) {
                    callable_identities_[&method] = callable_identity(
                        module, owner, method.name, method.parameters);
                }
            }
        }
    }

    void collect_struct_callables(
        const StructDeclarationStmt& declaration,
        std::string_view module,
        const std::string& owner)
    {
        for (const auto& member : declaration.methods) {
            if (member->kind != ClassMemberKind::Method) continue;
            const auto& method = static_cast<const MethodDeclaration&>(*member);
            callable_identities_[&method] = callable_identity(
                module, owner, method.name, method.parameters);
        }
        for (const auto& conversion : declaration.conversions) {
            conversion_identities_[conversion.get()] = owner + "::as("
                + (conversion->target_type.resolved_type
                        ? type_identity(*conversion->target_type.resolved_type)
                        : conversion->target_type.name)
                + ')';
        }
    }

    void collect_class_callables(
        const ClassDeclarationStmt& declaration,
        std::string_view module,
        const std::string& owner)
    {
        for (const auto& member : declaration.members) {
            if (member->kind == ClassMemberKind::Method) {
                const auto& method = static_cast<const MethodDeclaration&>(*member);
                callable_identities_[&method] = callable_identity(
                    module, owner, method.name, method.parameters);
            } else if (member->kind == ClassMemberKind::Conversion) {
                const auto& conversion =
                    static_cast<const ConversionOverload&>(*member);
                conversion_identities_[&conversion] = owner + "::as("
                    + (conversion.target_type.resolved_type
                            ? type_identity(*conversion.target_type.resolved_type)
                            : conversion.target_type.name)
                    + ')';
            }
        }
    }

    std::vector<IRGenericParameter> lower_generic_parameters(
        const std::vector<GenericParameter>& parameters,
        std::string_view module) const
    {
        std::vector<IRGenericParameter> result;
        result.reserve(parameters.size());
        for (const auto& parameter : parameters) {
            const auto parameter_origin = origin(module, parameter.location);
            IRGenericParameter lowered;
            lowered.name = parameter.name;
            lowered.origin = parameter_origin;
            for (const auto& constraint : parameter.constraints) {
                lowered.constraints.push_back(
                    lower_type(constraint, parameter_origin));
            }
            result.push_back(std::move(lowered));
        }
        return result;
    }

    std::vector<IRParameter> lower_parameters(
        const std::vector<Parameter>& parameters,
        std::string_view module) const
    {
        std::vector<IRParameter> result;
        result.reserve(parameters.size());
        for (const auto& parameter : parameters) {
            const auto parameter_origin = origin(module, parameter.location);
            result.push_back(IRParameter{
                parameter.name,
                lower_type(parameter.type, parameter_origin),
                parameter_origin,
            });
        }
        return result;
    }

    std::optional<IRType> lower_return_type(
        const std::optional<TypeReference>& reference,
        const IROrigin& declaration_origin) const
    {
        return reference
            ? std::optional<IRType>(lower_type(*reference, declaration_origin))
            : std::optional<IRType>(lower_type(void_type));
    }

    IROperation lower_expression(
        const Expr& expression,
        const IROrigin& inherited_origin)
    {
        IROperation operation;
        operation.origin = inherited_origin;
        operation.result_type = expression_type(expression, inherited_origin);
        switch (expression.kind) {
        case ExprKind::Integer:
            operation.kind = IROperationKind::Constant;
            operation.name = static_cast<const IntegerExpr&>(expression).value;
            break;
        case ExprKind::Decimal:
            operation.kind = IROperationKind::Constant;
            operation.name = static_cast<const DecimalExpr&>(expression).value;
            break;
        case ExprKind::String:
            operation.kind = IROperationKind::Constant;
            operation.name = '"' + static_cast<const StringExpr&>(expression).value + '"';
            break;
        case ExprKind::Bool:
            operation.kind = IROperationKind::Constant;
            operation.name = static_cast<const BoolExpr&>(expression).value
                ? "true" : "false";
            break;
        case ExprKind::Null:
            operation.kind = IROperationKind::Constant;
            operation.name = "null";
            break;
        case ExprKind::Identifier:
            operation.kind = IROperationKind::Load;
            operation.name = static_cast<const IdentifierExpr&>(expression).name;
            break;
        case ExprKind::Unary: {
            const auto& unary = static_cast<const UnaryExpr&>(expression);
            operation.kind = IROperationKind::Unary;
            operation.name = unary.operator_token.lexeme;
            operation.origin.location = {
                unary.operator_token.line, unary.operator_token.column};
            operation.children.push_back(
                lower_expression(*unary.operand, operation.origin));
            break;
        }
        case ExprKind::Binary: {
            const auto& binary = static_cast<const BinaryExpr&>(expression);
            operation.kind = IROperationKind::Binary;
            operation.name = binary.operator_token.lexeme;
            operation.origin.location = {
                binary.operator_token.line, binary.operator_token.column};
            operation.children.push_back(
                lower_expression(*binary.left, operation.origin));
            operation.children.push_back(
                lower_expression(*binary.right, operation.origin));
            break;
        }
        case ExprKind::Call:
            return lower_call(static_cast<const CallExpr&>(expression), inherited_origin);
        case ExprKind::Index: {
            const auto& index = static_cast<const IndexExpr&>(expression);
            operation.kind = IROperationKind::IndexAccess;
            operation.children.push_back(
                lower_expression(*index.object, inherited_origin));
            operation.children.push_back(
                lower_expression(*index.index, inherited_origin));
            break;
        }
        case ExprKind::MemberAccess: {
            const auto& member = static_cast<const MemberAccessExpr&>(expression);
            operation.kind = IROperationKind::MemberAccess;
            operation.name = member.member;
            operation.children.push_back(
                lower_expression(*member.object, inherited_origin));
            break;
        }
        case ExprKind::TypeAccess: {
            const auto& access = static_cast<const TypeAccessExpr&>(expression);
            operation.kind = IROperationKind::EnumConstruction;
            operation.name = access.type_name + "::" + access.member;
            if (expression.resolved_type
                && expression.resolved_type->kind == TypeKind::Unknown) {
                operation.selected_declaration = expression.resolved_type->name;
            }
            break;
        }
        case ExprKind::Propagation: {
            const auto& propagation = static_cast<const PropagationExpr&>(expression);
            operation.kind = IROperationKind::PropagateResult;
            operation.origin.location = {
                propagation.question_token.line,
                propagation.question_token.column,
            };
            operation.children.push_back(
                lower_expression(*propagation.expression, inherited_origin));
            break;
        }
        case ExprKind::Cast: {
            const auto& cast = static_cast<const CastExpr&>(expression);
            operation.kind = IROperationKind::Conversion;
            operation.name = cast.target_type.name;
            if (cast.resolved_conversion) {
                operation.selected_declaration =
                    conversion_identities_.at(cast.resolved_conversion);
            }
            operation.children.push_back(
                lower_expression(*cast.expression, inherited_origin));
            break;
        }
        case ExprKind::Grouping:
            return lower_expression(
                *static_cast<const GroupingExpr&>(expression).expression,
                inherited_origin);
        }
        return operation;
    }

    IROperation lower_call(
        const CallExpr& call,
        const IROrigin& call_origin)
    {
        IROperation operation;
        operation.origin = call_origin;
        operation.result_type = expression_type(call, call_origin);
        operation.kind = IROperationKind::Call;

        if (call.resolved_function) {
            operation.selected_declaration =
                callable_identities_.at(call.resolved_function);
            operation.name = call.resolved_function->name;
            if (call.resolved_function->is_external) {
                operation.kind = IROperationKind::ForeignCall;
            }
        } else if (call.resolved_method) {
            operation.selected_declaration =
                callable_identities_.at(call.resolved_method);
            operation.name = call.resolved_method->name;
            if (call.resolved_method->is_virtual || call.resolved_method->is_override) {
                operation.kind = IROperationKind::VirtualCall;
            }
        } else if (call.resolved_interface_method) {
            operation.selected_declaration =
                callable_identities_.at(call.resolved_interface_method);
            operation.name = call.resolved_interface_method->name;
            operation.kind = IROperationKind::InterfaceCall;
        }

        if (call.callee->kind == ExprKind::Identifier) {
            const auto& callee = static_cast<const IdentifierExpr&>(*call.callee);
            if (operation.name.empty()) operation.name = callee.name;
            if (callee.name == "ok" || callee.name == "error") {
                operation.kind = IROperationKind::ResultConstruction;
            } else if (callee.name == "print") {
                operation.kind = IROperationKind::Call;
            } else if (!call.resolved_function && call.resolved_type) {
                const auto result = lower_type(*call.resolved_type);
                if (result.category == IRValueCategory::CollectionValue) {
                    operation.kind = IROperationKind::CollectionOperation;
                } else if (result.category == IRValueCategory::Value
                    || result.category == IRValueCategory::ManagedReference) {
                    operation.kind = IROperationKind::Construction;
                    operation.selected_declaration = result.type.name;
                }
            }
        } else if (call.callee->kind == ExprKind::TypeAccess) {
            const auto& access = static_cast<const TypeAccessExpr&>(*call.callee);
            operation.kind = IROperationKind::EnumConstruction;
            operation.name = access.type_name + "::" + access.member;
            if (call.resolved_type
                && call.resolved_type->kind == TypeKind::Unknown) {
                operation.selected_declaration = call.resolved_type->name;
            }
        } else if (call.callee->kind == ExprKind::MemberAccess) {
            const auto& member = static_cast<const MemberAccessExpr&>(*call.callee);
            if (operation.name.empty()) operation.name = member.member;
            operation.children.push_back(
                lower_expression(*member.object, call_origin));
            if (member.object->resolved_type
                && category(*member.object->resolved_type)
                    == IRValueCategory::CollectionValue) {
                operation.kind = IROperationKind::CollectionOperation;
            }
        }

        for (const auto& [name, type] : call.resolved_substitutions) {
            operation.substitutions.emplace_back(name, lower_type(type));
        }
        std::ranges::sort(operation.substitutions,
            [](const auto& left, const auto& right) {
                return left.first < right.first;
            });
        for (const auto& argument : call.arguments) {
            operation.argument_names.push_back(argument.name.value_or(""));
            operation.children.push_back(
                lower_expression(*argument.value, call_origin));
        }
        return operation;
    }

    IROperation lower_block(
        const BlockStmt& block,
        std::string_view module)
    {
        IROperation result;
        result.kind = IROperationKind::Block;
        result.origin = origin(module, block.location);
        for (const auto& statement : block.statements) {
            result.children.push_back(lower_statement(*statement, module));
        }
        return result;
    }

    IROperation lower_statement(
        const Stmt& statement,
        std::string_view module)
    {
        const auto statement_origin = origin(module, statement.location);
        IROperation operation;
        operation.origin = statement_origin;
        switch (statement.kind) {
        case StmtKind::VariableDeclaration: {
            const auto& declaration =
                static_cast<const VariableDeclarationStmt&>(statement);
            operation.kind = IROperationKind::Declare;
            operation.name = declaration.name;
            operation.result_type = declaration.explicit_type
                ? lower_type(*declaration.explicit_type, statement_origin)
                : expression_type(*declaration.initializer, statement_origin);
            operation.children.push_back(
                lower_expression(*declaration.initializer, statement_origin));
            return operation;
        }
        case StmtKind::Assignment: {
            const auto& assignment = static_cast<const AssignmentStmt&>(statement);
            operation.kind = IROperationKind::Assign;
            operation.name = assignment.name;
            operation.children.push_back(
                lower_expression(*assignment.value, statement_origin));
            return operation;
        }
        case StmtKind::MemberAssignment: {
            const auto& assignment =
                static_cast<const MemberAssignmentStmt&>(statement);
            operation.kind = IROperationKind::MemberAssign;
            operation.name = assignment.target->member;
            operation.children.push_back(
                lower_expression(*assignment.target->object, statement_origin));
            operation.children.push_back(
                lower_expression(*assignment.value, statement_origin));
            return operation;
        }
        case StmtKind::IndexAssignment: {
            const auto& assignment =
                static_cast<const IndexAssignmentStmt&>(statement);
            operation.kind = IROperationKind::IndexAssign;
            operation.children.push_back(
                lower_expression(*assignment.target->object, statement_origin));
            operation.children.push_back(
                lower_expression(*assignment.target->index, statement_origin));
            operation.children.push_back(
                lower_expression(*assignment.value, statement_origin));
            return operation;
        }
        case StmtKind::Expression:
            return lower_expression(
                *static_cast<const ExpressionStmt&>(statement).expression,
                statement_origin);
        case StmtKind::Return: {
            const auto& result = static_cast<const ReturnStmt&>(statement);
            operation.kind = IROperationKind::Return;
            if (result.value) {
                operation.children.push_back(
                    lower_expression(*result.value, statement_origin));
            }
            return operation;
        }
        case StmtKind::Block:
            return lower_block(static_cast<const BlockStmt&>(statement), module);
        case StmtKind::If: {
            const auto& branch = static_cast<const IfStmt&>(statement);
            operation.kind = IROperationKind::Branch;
            operation.children.push_back(
                lower_expression(*branch.condition, statement_origin));
            operation.children.push_back(lower_block(*branch.then_block, module));
            if (branch.else_branch) {
                operation.children.push_back(
                    lower_statement(*branch.else_branch, module));
            }
            return operation;
        }
        case StmtKind::While: {
            const auto& loop = static_cast<const WhileStmt&>(statement);
            operation.kind = IROperationKind::Loop;
            operation.children.push_back(
                lower_expression(*loop.condition, statement_origin));
            operation.children.push_back(lower_block(*loop.body, module));
            return operation;
        }
        case StmtKind::ForIn: {
            const auto& loop = static_cast<const ForInStmt&>(statement);
            operation.kind = IROperationKind::ForEach;
            operation.name = loop.variable_name;
            operation.children.push_back(
                lower_expression(*loop.collection, statement_origin));
            operation.children.push_back(lower_block(*loop.body, module));
            return operation;
        }
        case StmtKind::Handle: {
            const auto& handle = static_cast<const HandleStmt&>(statement);
            operation.kind = IROperationKind::Handle;
            operation.children.push_back(
                lower_expression(*handle.expression, statement_origin));
            for (const auto& handle_case : handle.cases) {
                auto case_block = lower_block(*handle_case.body, module);
                case_block.name = handle_case.variant_name;
                if (handle_case.binding_name) {
                    case_block.name += '(' + *handle_case.binding_name + ')';
                }
                operation.children.push_back(std::move(case_block));
            }
            return operation;
        }
        case StmtKind::Stop:
            operation.kind = IROperationKind::Stop;
            return operation;
        case StmtKind::Continue:
            operation.kind = IROperationKind::Continue;
            return operation;
        case StmtKind::ImportDeclaration:
        case StmtKind::FunctionDeclaration:
        case StmtKind::EnumDeclaration:
        case StmtKind::StructDeclaration:
        case StmtKind::ClassDeclaration:
        case StmtKind::InterfaceDeclaration:
            throw_ir_error(statement_origin,
                "declaration appeared in an executable statement region");
        }
        throw_ir_error(statement_origin, "unknown statement kind");
    }

    IRDeclaration lower_function(
        const FunctionDeclarationStmt& function,
        std::string_view module)
    {
        const auto declaration_origin = origin(module, function.location);
        IRDeclaration result;
        result.kind = function.is_external
            ? IRDeclarationKind::ForeignFunction
            : IRDeclarationKind::Function;
        result.origin = declaration_origin;
        result.name = function.name;
        result.identity = callable_identities_.at(&function);
        result.generic_parameters = lower_generic_parameters(
            function.generic_parameters, module);
        result.parameters = lower_parameters(function.parameters, module);
        result.return_type = lower_return_type(
            function.return_type, declaration_origin);
        result.is_public = function.is_public;
        result.calling_convention = function.calling_convention;
        result.performance_contract = function.performance_contract;
        if (!function.is_external) {
            result.body = lower_block(*function.body, module);
        }
        return result;
    }

    IRDeclaration lower_method(
        const MethodDeclaration& method,
        std::string_view module,
        const std::string& owner)
    {
        const auto declaration_origin = origin(module, method.location);
        IRDeclaration result;
        result.kind = IRDeclarationKind::Method;
        result.origin = declaration_origin;
        result.name = method.name;
        result.identity = callable_identities_.at(&method);
        result.owner_identity = owner;
        result.generic_parameters = lower_generic_parameters(
            method.generic_parameters, module);
        result.parameters = lower_parameters(method.parameters, module);
        result.return_type = lower_return_type(method.return_type, declaration_origin);
        result.is_virtual = method.is_virtual;
        result.is_override = method.is_override;
        result.is_abstract = !method.body;
        result.is_public = method.visibility == Visibility::Public;
        result.performance_contract = method.performance_contract;
        if (method.body) result.body = lower_block(*method.body, module);
        return result;
    }

    IRDeclaration lower_conversion(
        const ConversionOverload& conversion,
        std::string_view module,
        const std::string& owner)
    {
        const auto declaration_origin = origin(module, conversion.location);
        IRDeclaration result;
        result.kind = IRDeclarationKind::Conversion;
        result.origin = declaration_origin;
        result.name = "as";
        result.identity = conversion_identities_.at(&conversion);
        result.owner_identity = owner;
        result.return_type = lower_type(conversion.target_type, declaration_origin);
        result.body = lower_block(*conversion.body, module);
        return result;
    }

    void lower_top_level(
        const Stmt& statement,
        IRProgram& program)
    {
        auto& output = program.declarations;
        const std::string& module = statement.module_name;
        const auto declaration_origin = origin(module, statement.location);
        switch (statement.kind) {
        case StmtKind::FunctionDeclaration:
            output.push_back(lower_function(
                static_cast<const FunctionDeclarationStmt&>(statement), module));
            return;
        case StmtKind::EnumDeclaration: {
            const auto& declaration =
                static_cast<const EnumDeclarationStmt&>(statement);
            IRDeclaration result;
            result.kind = IRDeclarationKind::Enum;
            result.origin = declaration_origin;
            result.name = declaration.name;
            result.identity = declaration_identity(module, declaration.name);
            result.is_public = declaration.is_public;
            for (const auto& variant : declaration.variants) {
                const auto variant_origin = origin(module, variant.location);
                result.variants.push_back(IREnumVariant{
                    variant.name,
                    variant.payload_type
                        ? std::optional<IRType>(lower_type(
                            *variant.payload_type, variant_origin))
                        : std::nullopt,
                    variant_origin,
                });
            }
            output.push_back(std::move(result));
            return;
        }
        case StmtKind::StructDeclaration: {
            const auto& declaration =
                static_cast<const StructDeclarationStmt&>(statement);
            const std::string owner = declaration_identity(module, declaration.name);
            IRDeclaration result;
            result.kind = IRDeclarationKind::Struct;
            result.origin = declaration_origin;
            result.name = declaration.name;
            result.identity = owner;
            result.generic_parameters = lower_generic_parameters(
                declaration.generic_parameters, module);
            result.is_public = declaration.is_public;
            for (const auto& field : declaration.fields) {
                const auto field_origin = origin(module, field.location);
                result.fields.push_back(IRField{
                    field.name,
                    lower_type(field.type, field_origin),
                    field_origin,
                    field.default_value
                        ? std::optional<IROperation>(lower_expression(
                            *field.default_value, field_origin))
                        : std::nullopt,
                    true,
                    false,
                });
            }
            for (const auto& interface_type : declaration.interfaces) {
                result.related_types.push_back(
                    lower_type(interface_type, declaration_origin));
            }
            output.push_back(std::move(result));
            for (const auto& member : declaration.methods) {
                if (member->kind == ClassMemberKind::Method) {
                    output.push_back(lower_method(
                        static_cast<const MethodDeclaration&>(*member), module, owner));
                }
            }
            for (const auto& conversion : declaration.conversions) {
                output.push_back(lower_conversion(*conversion, module, owner));
            }
            return;
        }
        case StmtKind::ClassDeclaration: {
            const auto& declaration =
                static_cast<const ClassDeclarationStmt&>(statement);
            const std::string owner = declaration_identity(module, declaration.name);
            IRDeclaration result;
            result.kind = IRDeclarationKind::Class;
            result.origin = declaration_origin;
            result.name = declaration.name;
            result.identity = owner;
            result.generic_parameters = lower_generic_parameters(
                declaration.generic_parameters, module);
            result.is_abstract = declaration.is_abstract;
            result.is_public = declaration.is_public;
            if (declaration.base_type) {
                result.related_types.push_back(
                    lower_type(*declaration.base_type, declaration_origin));
            }
            for (const auto& interface_type : declaration.interfaces) {
                result.related_types.push_back(
                    lower_type(interface_type, declaration_origin));
            }
            for (const auto& member : declaration.members) {
                if (member->kind == ClassMemberKind::Field) {
                    const auto& field = static_cast<const ClassField&>(*member);
                    const auto field_origin = origin(module, field.location);
                    result.fields.push_back(IRField{
                        field.name,
                        lower_type(field.type, field_origin),
                        field_origin,
                        field.default_value
                            ? std::optional<IROperation>(lower_expression(
                                *field.default_value, field_origin))
                            : std::nullopt,
                        field.visibility == Visibility::Public,
                        field.is_weak,
                    });
                }
            }
            output.push_back(std::move(result));
            for (const auto& member : declaration.members) {
                if (member->kind == ClassMemberKind::Method) {
                    output.push_back(lower_method(
                        static_cast<const MethodDeclaration&>(*member), module, owner));
                } else if (member->kind == ClassMemberKind::Conversion) {
                    output.push_back(lower_conversion(
                        static_cast<const ConversionOverload&>(*member), module, owner));
                }
            }
            return;
        }
        case StmtKind::InterfaceDeclaration: {
            const auto& declaration =
                static_cast<const InterfaceDeclarationStmt&>(statement);
            const std::string owner = declaration_identity(module, declaration.name);
            IRDeclaration result;
            result.kind = IRDeclarationKind::Interface;
            result.origin = declaration_origin;
            result.name = declaration.name;
            result.identity = owner;
            result.generic_parameters = lower_generic_parameters(
                declaration.generic_parameters, module);
            result.is_public = declaration.is_public;
            output.push_back(std::move(result));
            for (const auto& method : declaration.methods) {
                IRDeclaration method_result;
                method_result.kind = IRDeclarationKind::Method;
                method_result.origin = origin(module, method.location);
                method_result.name = method.name;
                method_result.identity = callable_identities_.at(&method);
                method_result.owner_identity = owner;
                method_result.generic_parameters = lower_generic_parameters(
                    method.generic_parameters, module);
                method_result.parameters = lower_parameters(method.parameters, module);
                method_result.return_type = lower_return_type(
                    method.return_type, method_result.origin);
                method_result.is_abstract = true;
                method_result.is_public = true;
                output.push_back(std::move(method_result));
            }
            return;
        }
        case StmtKind::ImportDeclaration:
            return;
        default:
            program.top_level_operations.push_back(
                lower_statement(statement, module));
            return;
        }
    }

    const Program* program_{nullptr};
    std::unordered_set<std::string> struct_types_;
    std::unordered_set<std::string> class_types_;
    std::unordered_set<std::string> interface_types_;
    std::unordered_set<std::string> enum_types_;
    std::unordered_map<const void*, std::string> callable_identities_;
    std::unordered_map<const ConversionOverload*, std::string> conversion_identities_;
};

} // namespace

IRProgram IRLowerer::lower(const Program& program) const
{
    return LoweringContext{}.lower(program);
}

} // namespace toro
