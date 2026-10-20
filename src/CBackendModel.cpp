#include "CBackendModel.hpp"

#include "toro/ModuleIdentity.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace toro::detail::c_backend {
namespace {

[[noreturn]] void fail(const IROrigin& origin, std::string_view message)
{
    throw std::runtime_error(
        "C backend IR error at " + std::to_string(origin.location.line) + ':'
        + std::to_string(origin.location.column) + ": " + std::string(message));
}

TypeReference type_reference(const IRType& value, SourceLocation location)
{
    TypeReference result;
    result.name = value.type.kind == TypeKind::Unknown && !value.type.name.empty()
        ? std::string(declaration_source_name(value.type.name))
        : std::string(base_type_name(value.type));
    result.location = location;
    result.nullable = value.type.nullable;
    result.resolved_type = value.type;
    for (const auto& argument : value.type.arguments) {
        result.arguments.push_back(type_reference(
            IRType{argument, IRValueCategory::Unknown}, location));
    }
    return result;
}

std::vector<GenericParameter> generic_parameters(
    const std::vector<IRGenericParameter>& values)
{
    std::vector<GenericParameter> result;
    for (const auto& value : values) {
        GenericParameter parameter;
        parameter.name = value.name;
        parameter.location = value.origin.location;
        for (const auto& constraint : value.constraints) {
            parameter.constraints.push_back(
                type_reference(constraint, value.origin.location));
        }
        result.push_back(std::move(parameter));
    }
    return result;
}

std::vector<Parameter> parameters(const std::vector<IRParameter>& values)
{
    std::vector<Parameter> result;
    for (const auto& value : values) {
        result.push_back(Parameter{
            value.name,
            type_reference(value.type, value.origin.location),
            value.origin.location,
        });
    }
    return result;
}

std::optional<TypeReference> return_type(const IRDeclaration& declaration)
{
    if (!declaration.return_type
        || declaration.return_type->type.kind == TypeKind::Void) {
        return std::nullopt;
    }
    return type_reference(*declaration.return_type, declaration.origin.location);
}

TokenType operator_type(std::string_view value)
{
    if (value == "+") return TokenType::Plus;
    if (value == "-") return TokenType::Minus;
    if (value == "*") return TokenType::Star;
    if (value == "/") return TokenType::Slash;
    if (value == "==") return TokenType::Equal;
    if (value == "!=") return TokenType::NotEqual;
    if (value == "<") return TokenType::Less;
    if (value == "<=") return TokenType::LessEqual;
    if (value == ">") return TokenType::Greater;
    if (value == ">=") return TokenType::GreaterEqual;
    if (value == "and") return TokenType::And;
    if (value == "or") return TokenType::Or;
    return TokenType::Identifier;
}

class Builder {
public:
    explicit Builder(const IRProgram& ir) : ir_(ir) {}

    Program build()
    {
        validate_arc_schedules();
        Program result;
        result.entry_module = ir_.entry_module;
        create_top_level(result);
        attach_members();
        lower_bodies();
        return result;
    }

private:
    void validate_arc_schedules() const
    {
        for (const auto& declaration : ir_.declarations) {
            const bool requires_schedule = declaration.body.has_value()
                || declaration.kind == IRDeclarationKind::Class;
            if (!requires_schedule) continue;
            const std::string identity = declaration.kind == IRDeclarationKind::Class
                ? declaration.identity + "#lifetime" : declaration.identity;
            const bool present = std::ranges::any_of(
                ir_.arc_schedules,
                [&](const auto& schedule) {
                    return schedule.callable_identity == identity;
                });
            if (!present) {
                fail(declaration.origin,
                    "materialized ARC schedule is missing for '" + identity + "'");
            }
        }
    }

    std::vector<std::string> borrowed_parameters(
        std::string_view callable_identity) const
    {
        if (ir_.arc_stage != IRArcStage::Optimized) return {};
        for (const auto& schedule : ir_.arc_schedules) {
            if (schedule.callable_identity != callable_identity) continue;
            std::vector<std::string> result;
            for (const auto& operation : schedule.optimized_operations) {
                if (operation.kind == IRArcOperationKind::BorrowedTemporary
                    && operation.reason
                        == "locally proven non-escaping parameter borrow") {
                    result.push_back(operation.value);
                }
            }
            return result;
        }
        return {};
    }

    void apply_statement_metadata(Stmt& statement, const IRDeclaration& declaration)
    {
        statement.module_name = declaration.origin.module_name;
        statement.is_public = declaration.is_public;
    }

    void create_top_level(Program& result)
    {
        for (const auto& declaration : ir_.declarations) {
            switch (declaration.kind) {
            case IRDeclarationKind::Function:
            case IRDeclarationKind::ForeignFunction: {
                auto value = std::make_unique<FunctionDeclarationStmt>(
                    declaration.origin.location,
                    declaration.name,
                    generic_parameters(declaration.generic_parameters),
                    parameters(declaration.parameters),
                    return_type(declaration),
                    declaration.kind == IRDeclarationKind::ForeignFunction
                        ? std::make_unique<BlockStmt>(
                            declaration.origin.location,
                            std::vector<std::unique_ptr<Stmt>>{})
                        : nullptr,
                    declaration.kind == IRDeclarationKind::ForeignFunction,
                    declaration.calling_convention);
                value->identity = declaration.identity;
                value->borrowed_parameters = borrowed_parameters(
                    declaration.identity);
                apply_statement_metadata(*value, declaration);
                functions_.emplace(declaration.identity, value.get());
                declarations_.emplace(declaration.identity, &declaration);
                result.statements.push_back(std::move(value));
                break;
            }
            case IRDeclarationKind::Struct: {
                std::vector<StructField> fields;
                for (const auto& field : declaration.fields) {
                    fields.push_back(StructField{
                        field.name,
                        type_reference(field.type, field.origin.location),
                        nullptr,
                        field.origin.location,
                    });
                }
                std::vector<TypeReference> interfaces;
                for (const auto& related : declaration.related_types) {
                    interfaces.push_back(type_reference(
                        related, declaration.origin.location));
                }
                auto value = std::make_unique<StructDeclarationStmt>(
                    declaration.origin.location,
                    declaration.name,
                    generic_parameters(declaration.generic_parameters),
                    std::move(interfaces),
                    std::move(fields),
                    std::vector<std::unique_ptr<ClassMember>>{},
                    std::vector<std::unique_ptr<ConversionOverload>>{});
                value->identity = declaration.identity;
                apply_statement_metadata(*value, declaration);
                structs_.emplace(declaration.identity, value.get());
                nominals_.emplace(declaration.identity, &declaration);
                result.statements.push_back(std::move(value));
                break;
            }
            case IRDeclarationKind::Class: {
                std::optional<TypeReference> base;
                std::vector<TypeReference> interfaces;
                for (const auto& related : declaration.related_types) {
                    if (related.category == IRValueCategory::ManagedReference
                        && !base) {
                        base = type_reference(related, declaration.origin.location);
                    } else {
                        interfaces.push_back(type_reference(
                            related, declaration.origin.location));
                    }
                }
                std::vector<std::unique_ptr<ClassMember>> members;
                for (const auto& field : declaration.fields) {
                    members.push_back(std::make_unique<ClassField>(
                        field.is_public ? Visibility::Public : Visibility::Private,
                        field.is_weak,
                        field.origin.location,
                        field.name,
                        type_reference(field.type, field.origin.location),
                        nullptr));
                }
                auto value = std::make_unique<ClassDeclarationStmt>(
                    declaration.origin.location,
                    declaration.name,
                    declaration.is_abstract,
                    generic_parameters(declaration.generic_parameters),
                    std::move(base),
                    std::move(interfaces),
                    std::move(members));
                value->identity = declaration.identity;
                apply_statement_metadata(*value, declaration);
                classes_.emplace(declaration.identity, value.get());
                nominals_.emplace(declaration.identity, &declaration);
                result.statements.push_back(std::move(value));
                break;
            }
            case IRDeclarationKind::Interface: {
                auto value = std::make_unique<InterfaceDeclarationStmt>(
                    declaration.origin.location,
                    declaration.name,
                    generic_parameters(declaration.generic_parameters),
                    std::vector<InterfaceMethod>{});
                value->identity = declaration.identity;
                apply_statement_metadata(*value, declaration);
                interfaces_.emplace(declaration.identity, value.get());
                nominals_.emplace(declaration.identity, &declaration);
                result.statements.push_back(std::move(value));
                break;
            }
            case IRDeclarationKind::Enum: {
                std::vector<EnumVariant> variants;
                for (const auto& variant : declaration.variants) {
                    variants.push_back(EnumVariant{
                        variant.name,
                        variant.payload_type
                            ? std::optional<TypeReference>(type_reference(
                                *variant.payload_type, variant.origin.location))
                            : std::nullopt,
                        variant.origin.location,
                    });
                }
                auto value = std::make_unique<EnumDeclarationStmt>(
                    declaration.origin.location,
                    declaration.name,
                    std::move(variants));
                value->identity = declaration.identity;
                apply_statement_metadata(*value, declaration);
                enums_.emplace(declaration.identity, value.get());
                nominals_.emplace(declaration.identity, &declaration);
                result.statements.push_back(std::move(value));
                break;
            }
            case IRDeclarationKind::Method:
            case IRDeclarationKind::Conversion:
                declarations_.emplace(declaration.identity, &declaration);
                break;
            }
        }
    }

    void attach_members()
    {
        for (auto& [identity, interface] : interfaces_) {
            std::size_t count = 0;
            for (const auto& declaration : ir_.declarations) {
                if (declaration.kind == IRDeclarationKind::Method
                    && declaration.owner_identity == identity) {
                    ++count;
                }
            }
            interface->methods.reserve(count);
        }
        for (const auto& declaration : ir_.declarations) {
            if (declaration.kind == IRDeclarationKind::Method) {
                attach_method(declaration);
            } else if (declaration.kind == IRDeclarationKind::Conversion) {
                attach_conversion(declaration);
            }
        }
    }

    void attach_method(const IRDeclaration& declaration)
    {
        if (auto interface = interfaces_.find(declaration.owner_identity);
            interface != interfaces_.end()) {
            InterfaceMethod value{
                declaration.name,
                generic_parameters(declaration.generic_parameters),
                parameters(declaration.parameters),
                return_type(declaration),
                declaration.origin.location,
                declaration.identity,
            };
            interface->second->methods.push_back(std::move(value));
            interface_methods_.emplace(
                declaration.identity, &interface->second->methods.back());
            return;
        }

        auto value = std::make_unique<MethodDeclaration>(
            declaration.is_public ? Visibility::Public : Visibility::Private,
            declaration.origin.location,
            declaration.name,
            generic_parameters(declaration.generic_parameters),
            parameters(declaration.parameters),
            return_type(declaration),
            declaration.is_virtual,
            declaration.is_override,
            nullptr);
        value->identity = declaration.identity;
        value->borrowed_parameters = borrowed_parameters(declaration.identity);
        auto* pointer = value.get();
        if (auto structure = structs_.find(declaration.owner_identity);
            structure != structs_.end()) {
            structure->second->methods.push_back(std::move(value));
        } else if (auto class_ = classes_.find(declaration.owner_identity);
            class_ != classes_.end()) {
            class_->second->members.push_back(std::move(value));
        } else {
            fail(declaration.origin, "method owner is missing from IR");
        }
        methods_.emplace(declaration.identity, pointer);
    }

    void attach_conversion(const IRDeclaration& declaration)
    {
        if (!declaration.return_type) {
            fail(declaration.origin, "conversion has no target type");
        }
        auto value = std::make_unique<ConversionOverload>(
            declaration.origin.location,
            type_reference(*declaration.return_type, declaration.origin.location),
            nullptr);
        value->identity = declaration.identity;
        auto* pointer = value.get();
        if (auto structure = structs_.find(declaration.owner_identity);
            structure != structs_.end()) {
            structure->second->conversions.push_back(std::move(value));
        } else if (auto class_ = classes_.find(declaration.owner_identity);
            class_ != classes_.end()) {
            class_->second->members.push_back(std::move(value));
        } else {
            fail(declaration.origin, "conversion owner is missing from IR");
        }
        conversions_.emplace(declaration.identity, pointer);
    }

    void lower_bodies()
    {
        for (const auto& declaration : ir_.declarations) {
            if (declaration.kind == IRDeclarationKind::Function
                && declaration.body) {
                functions_.at(declaration.identity)->body = block(*declaration.body);
            } else if (declaration.kind == IRDeclarationKind::Method
                && declaration.body) {
                methods_.at(declaration.identity)->body = block(*declaration.body);
            } else if (declaration.kind == IRDeclarationKind::Conversion
                && declaration.body) {
                conversions_.at(declaration.identity)->body = block(*declaration.body);
            }
        }
        for (const auto& declaration : ir_.declarations) {
            if (declaration.kind == IRDeclarationKind::Struct) {
                auto* target = structs_.at(declaration.identity);
                for (std::size_t index = 0; index < declaration.fields.size(); ++index) {
                    if (declaration.fields[index].default_value) {
                        target->fields[index].default_value = expression(
                            *declaration.fields[index].default_value);
                    }
                }
            } else if (declaration.kind == IRDeclarationKind::Class) {
                auto* target = classes_.at(declaration.identity);
                std::size_t field_index = 0;
                for (const auto& member : target->members) {
                    if (member->kind != ClassMemberKind::Field) continue;
                    const auto& source = declaration.fields[field_index++];
                    if (source.default_value) {
                        static_cast<ClassField&>(*member).default_value =
                            expression(*source.default_value);
                    }
                }
            }
        }
    }

    Token token(const IROperation& operation) const
    {
        return Token{
            operator_type(operation.name),
            operation.name,
            operation.origin.location.line,
            operation.origin.location.column,
        };
    }

    template <typename Value>
    std::unique_ptr<Value> typed(
        std::unique_ptr<Value> value,
        const IROperation& operation) const
    {
        if (operation.result_type) value->resolved_type = operation.result_type->type;
        return value;
    }

    std::unique_ptr<Expr> expression(const IROperation& operation)
    {
        switch (operation.kind) {
        case IROperationKind::Constant:
            if (!operation.result_type) fail(operation.origin, "constant has no type");
            switch (operation.result_type->type.kind) {
            case TypeKind::Int:
            case TypeKind::CInt:
            case TypeKind::CInt64:
                return typed(std::make_unique<IntegerExpr>(operation.name), operation);
            case TypeKind::Dec:
            case TypeKind::CDouble:
                return typed(std::make_unique<DecimalExpr>(operation.name), operation);
            case TypeKind::String:
            case TypeKind::CString: {
                std::string value = operation.name;
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                    value = value.substr(1, value.size() - 2);
                }
                return typed(std::make_unique<StringExpr>(std::move(value)), operation);
            }
            case TypeKind::Bool:
            case TypeKind::CBool:
                return typed(std::make_unique<BoolExpr>(operation.name == "true"), operation);
            case TypeKind::Null:
                return typed(std::make_unique<NullExpr>(), operation);
            case TypeKind::Void:
            case TypeKind::Unknown:
                break;
            }
            break;
        case IROperationKind::Load:
            return typed(std::make_unique<IdentifierExpr>(operation.name), operation);
        case IROperationKind::Unary:
            return typed(std::make_unique<UnaryExpr>(
                token(operation), expression(operation.children.at(0))), operation);
        case IROperationKind::Binary:
            return typed(std::make_unique<BinaryExpr>(
                expression(operation.children.at(0)), token(operation),
                expression(operation.children.at(1))), operation);
        case IROperationKind::MemberAccess:
            return typed(std::make_unique<MemberAccessExpr>(
                expression(operation.children.at(0)), operation.name), operation);
        case IROperationKind::IndexAccess:
            return typed(std::make_unique<IndexExpr>(
                expression(operation.children.at(0)),
                expression(operation.children.at(1))), operation);
        case IROperationKind::PropagateResult:
            return typed(std::make_unique<PropagationExpr>(
                expression(operation.children.at(0))), operation);
        case IROperationKind::Conversion: {
            if (!operation.result_type) fail(operation.origin, "conversion has no type");
            auto value = typed(std::make_unique<CastExpr>(
                expression(operation.children.at(0)),
                type_reference(*operation.result_type, operation.origin.location)), operation);
            if (!operation.selected_declaration.empty()) {
                value->resolved_conversion = conversions_.at(operation.selected_declaration);
            }
            return value;
        }
        case IROperationKind::EnumConstruction:
            if (operation.children.empty()) return enum_access(operation);
            return call(operation);
        case IROperationKind::Call:
        case IROperationKind::ForeignCall:
        case IROperationKind::VirtualCall:
        case IROperationKind::InterfaceCall:
        case IROperationKind::Construction:
        case IROperationKind::ResultConstruction:
        case IROperationKind::CollectionOperation:
            return call(operation);
        case IROperationKind::Declare:
        case IROperationKind::Assign:
        case IROperationKind::MemberAssign:
        case IROperationKind::IndexAssign:
        case IROperationKind::Block:
        case IROperationKind::Branch:
        case IROperationKind::Loop:
        case IROperationKind::ForEach:
        case IROperationKind::Handle:
        case IROperationKind::Return:
        case IROperationKind::Stop:
        case IROperationKind::Continue:
            break;
        }
        fail(operation.origin, "operation is not an expression");
    }

    std::unique_ptr<Expr> enum_access(const IROperation& operation)
    {
        const auto separator = operation.name.find("::");
        auto value = std::make_unique<TypeAccessExpr>(
            separator == std::string::npos
                ? operation.name : operation.name.substr(0, separator),
            separator == std::string::npos
                ? std::string{} : operation.name.substr(separator + 2));
        if (operation.result_type) value->resolved_type = operation.result_type->type;
        return value;
    }

    std::unique_ptr<Expr> call(const IROperation& operation)
    {
        std::unique_ptr<Expr> callee;
        std::size_t argument_start = 0;
        const bool selected_method = methods_.contains(operation.selected_declaration)
            || interface_methods_.contains(operation.selected_declaration);
        const bool collection_method = operation.kind == IROperationKind::CollectionOperation
            && operation.name != "Array" && operation.name != "List"
            && operation.name != "Map";
        if (operation.kind == IROperationKind::EnumConstruction) {
            callee = enum_access(operation);
        } else if (selected_method || collection_method) {
            if (operation.children.empty()) fail(operation.origin, "method call has no receiver");
            callee = std::make_unique<MemberAccessExpr>(
                expression(operation.children.front()), operation.name);
            argument_start = 1;
        } else {
            callee = std::make_unique<IdentifierExpr>(operation.name);
        }

        std::vector<TypeReference> generic_arguments;
        std::vector<CallArgument> arguments;
        for (std::size_t index = argument_start; index < operation.children.size(); ++index) {
            const std::size_t name_index = index - argument_start;
            const std::string name = name_index < operation.argument_names.size()
                ? operation.argument_names[name_index] : std::string{};
            arguments.push_back(CallArgument{
                name.empty() ? std::nullopt : std::optional<std::string>(name),
                expression(operation.children[index]),
            });
        }
        auto value = std::make_unique<CallExpr>(
            std::move(callee), std::move(generic_arguments), std::move(arguments));
        if (operation.ownership_transfer) {
            value->transfer_source = operation.ownership_transfer->source;
        }
        value->stack_storage = operation.storage == IRStorageClass::Stack;
        if (operation.result_type) value->resolved_type = operation.result_type->type;
        for (const auto& [name, type] : operation.substitutions) {
            value->resolved_substitutions.emplace_back(name, type.type);
        }
        if (auto found = functions_.find(operation.selected_declaration);
            found != functions_.end()) {
            value->resolved_function = found->second;
        } else if (auto found = methods_.find(operation.selected_declaration);
            found != methods_.end()) {
            value->resolved_method = found->second;
        } else if (auto found = interface_methods_.find(operation.selected_declaration);
            found != interface_methods_.end()) {
            value->resolved_interface_method = found->second;
        }
        return value;
    }

    std::unique_ptr<BlockStmt> block(const IROperation& operation)
    {
        if (operation.kind != IROperationKind::Block) {
            fail(operation.origin, "expected block operation");
        }
        std::vector<std::unique_ptr<Stmt>> statements;
        for (const auto& child : operation.children) {
            statements.push_back(statement(child));
        }
        auto value = std::make_unique<BlockStmt>(
            operation.origin.location, std::move(statements));
        value->module_name = operation.origin.module_name;
        return value;
    }

    std::unique_ptr<Stmt> statement(const IROperation& operation)
    {
        std::unique_ptr<Stmt> result;
        switch (operation.kind) {
        case IROperationKind::Declare:
            result = std::make_unique<VariableDeclarationStmt>(
                operation.origin.location,
                operation.name,
                operation.result_type
                    ? std::optional<TypeReference>(type_reference(
                        *operation.result_type, operation.origin.location))
                    : std::nullopt,
                expression(operation.children.at(0)));
            break;
        case IROperationKind::Assign:
            result = std::make_unique<AssignmentStmt>(
                operation.origin.location, operation.name,
                expression(operation.children.at(0)));
            break;
        case IROperationKind::MemberAssign: {
            auto target = std::make_unique<MemberAccessExpr>(
                expression(operation.children.at(0)), operation.name);
            if (operation.children.at(0).result_type) {
                target->object->resolved_type = operation.children.at(0).result_type->type;
            }
            result = std::make_unique<MemberAssignmentStmt>(
                operation.origin.location, std::move(target),
                expression(operation.children.at(1)));
            break;
        }
        case IROperationKind::IndexAssign:
            result = std::make_unique<IndexAssignmentStmt>(
                operation.origin.location,
                std::make_unique<IndexExpr>(
                    expression(operation.children.at(0)),
                    expression(operation.children.at(1))),
                expression(operation.children.at(2)));
            break;
        case IROperationKind::Return:
            result = std::make_unique<ReturnStmt>(
                operation.origin.location,
                operation.children.empty() ? nullptr : expression(operation.children[0]));
            break;
        case IROperationKind::Block:
            result = block(operation);
            break;
        case IROperationKind::Branch:
            result = std::make_unique<IfStmt>(
                operation.origin.location,
                expression(operation.children.at(0)),
                block(operation.children.at(1)),
                operation.children.size() > 2
                    ? statement(operation.children.at(2)) : nullptr);
            break;
        case IROperationKind::Loop:
            result = std::make_unique<WhileStmt>(
                operation.origin.location,
                expression(operation.children.at(0)),
                block(operation.children.at(1)));
            break;
        case IROperationKind::ForEach:
            result = std::make_unique<ForInStmt>(
                operation.origin.location,
                operation.name,
                expression(operation.children.at(0)),
                block(operation.children.at(1)));
            break;
        case IROperationKind::Handle: {
            std::vector<HandleCase> cases;
            for (std::size_t index = 1; index < operation.children.size(); ++index) {
                const auto& child = operation.children[index];
                const auto open = child.name.find('(');
                const auto close = child.name.rfind(')');
                cases.push_back(HandleCase{
                    open == std::string::npos ? child.name : child.name.substr(0, open),
                    open != std::string::npos && close > open
                        ? std::optional<std::string>(child.name.substr(open + 1, close - open - 1))
                        : std::nullopt,
                    block(child),
                    child.origin.location,
                });
            }
            result = std::make_unique<HandleStmt>(
                operation.origin.location,
                expression(operation.children.at(0)),
                std::move(cases));
            break;
        }
        case IROperationKind::Stop:
            result = std::make_unique<StopStmt>(operation.origin.location);
            break;
        case IROperationKind::Continue:
            result = std::make_unique<ContinueStmt>(operation.origin.location);
            break;
        case IROperationKind::Constant:
        case IROperationKind::Load:
        case IROperationKind::Unary:
        case IROperationKind::Binary:
        case IROperationKind::Call:
        case IROperationKind::ForeignCall:
        case IROperationKind::VirtualCall:
        case IROperationKind::InterfaceCall:
        case IROperationKind::Construction:
        case IROperationKind::EnumConstruction:
        case IROperationKind::ResultConstruction:
        case IROperationKind::CollectionOperation:
        case IROperationKind::MemberAccess:
        case IROperationKind::IndexAccess:
        case IROperationKind::Conversion:
        case IROperationKind::PropagateResult:
            result = std::make_unique<ExpressionStmt>(
                operation.origin.location, expression(operation));
            break;
        }
        result->module_name = operation.origin.module_name;
        if (operation.ownership_transfer) {
            result->transfer_source = operation.ownership_transfer->source;
        }
        return result;
    }

    const IRProgram& ir_;
    std::unordered_map<std::string, const IRDeclaration*> declarations_;
    std::unordered_map<std::string, const IRDeclaration*> nominals_;
    std::unordered_map<std::string, FunctionDeclarationStmt*> functions_;
    std::unordered_map<std::string, StructDeclarationStmt*> structs_;
    std::unordered_map<std::string, ClassDeclarationStmt*> classes_;
    std::unordered_map<std::string, InterfaceDeclarationStmt*> interfaces_;
    std::unordered_map<std::string, EnumDeclarationStmt*> enums_;
    std::unordered_map<std::string, MethodDeclaration*> methods_;
    std::unordered_map<std::string, InterfaceMethod*> interface_methods_;
    std::unordered_map<std::string, ConversionOverload*> conversions_;
};

} // namespace

Program make_emission_model(const IRProgram& program)
{
    return Builder(program).build();
}

} // namespace toro::detail::c_backend
