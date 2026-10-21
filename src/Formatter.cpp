#include "toro/Formatter.hpp"

#include "toro/AST.hpp"
#include "toro/Lexer.hpp"
#include "toro/Parser.hpp"
#include "toro/SourceFile.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace toro {
namespace {

class AstFormatter {
public:
    std::string format(const Program& program)
    {
        for (std::size_t index = 0; index < program.statements.size(); ++index) {
            if (index != 0) {
                const bool adjacent_imports =
                    program.statements[index - 1]->kind == StmtKind::ImportDeclaration
                    && program.statements[index]->kind == StmtKind::ImportDeclaration;
                output_ << (adjacent_imports ? "\n" : "\n\n");
            }
            write_statement(*program.statements[index], 0);
        }
        if (!program.statements.empty()) {
            output_ << '\n';
        }
        return output_.str();
    }

private:
    static int precedence(const Expr& expression)
    {
        switch (expression.kind) {
        case ExprKind::Binary: {
            const auto type = static_cast<const BinaryExpr&>(expression).operator_token.type;
            if (type == TokenType::Or) return 1;
            if (type == TokenType::And) return 2;
            if (type == TokenType::Equal || type == TokenType::NotEqual) return 3;
            if (type == TokenType::Less || type == TokenType::LessEqual
                || type == TokenType::Greater || type == TokenType::GreaterEqual) return 4;
            if (type == TokenType::Plus || type == TokenType::Minus) return 5;
            return 6;
        }
        case ExprKind::Cast: return 7;
        case ExprKind::Unary: return 8;
        case ExprKind::Call:
        case ExprKind::Index:
        case ExprKind::MemberAccess:
        case ExprKind::TypeAccess:
        case ExprKind::Propagation:
            return 9;
        case ExprKind::Integer:
        case ExprKind::Decimal:
        case ExprKind::String:
        case ExprKind::Bool:
        case ExprKind::Null:
        case ExprKind::Identifier:
        case ExprKind::Grouping:
            return 10;
        }
        return 10;
    }

    static std::string type_text(const TypeReference& type)
    {
        std::string result = type.name;
        if (!type.arguments.empty()) {
            result += '<';
            for (std::size_t index = 0; index < type.arguments.size(); ++index) {
                if (index != 0) result += ", ";
                result += type_text(type.arguments[index]);
            }
            result += '>';
        }
        if (type.nullable) result += '?';
        return result;
    }

    static std::string generic_parameters_text(
        const std::vector<GenericParameter>& parameters)
    {
        if (parameters.empty()) return {};
        std::string result = "<";
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index != 0) result += ", ";
            result += parameters[index].name;
            if (!parameters[index].constraints.empty()) {
                result += ": ";
                for (std::size_t constraint = 0;
                     constraint < parameters[index].constraints.size(); ++constraint) {
                    if (constraint != 0) result += " + ";
                    result += type_text(parameters[index].constraints[constraint]);
                }
            }
        }
        result += '>';
        return result;
    }

    static std::string parameters_text(const std::vector<Parameter>& parameters)
    {
        std::string result;
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index != 0) result += ", ";
            result += parameters[index].name + ": "
                + type_text(parameters[index].type);
        }
        return result;
    }

    static std::string type_list_text(const std::vector<TypeReference>& types)
    {
        std::string result;
        for (std::size_t index = 0; index < types.size(); ++index) {
            if (index != 0) result += ", ";
            result += type_text(types[index]);
        }
        return result;
    }

    std::string expression_text(const Expr& expression, int parent = 0) const
    {
        const int own = precedence(expression);
        std::string result;
        switch (expression.kind) {
        case ExprKind::Integer:
            result = static_cast<const IntegerExpr&>(expression).value;
            break;
        case ExprKind::Decimal:
            result = static_cast<const DecimalExpr&>(expression).value;
            break;
        case ExprKind::String:
            result = '"' + static_cast<const StringExpr&>(expression).value + '"';
            break;
        case ExprKind::Bool:
            result = static_cast<const BoolExpr&>(expression).value ? "true" : "false";
            break;
        case ExprKind::Null:
            result = "null";
            break;
        case ExprKind::Identifier:
            result = static_cast<const IdentifierExpr&>(expression).name;
            break;
        case ExprKind::Unary: {
            const auto& unary = static_cast<const UnaryExpr&>(expression);
            result = unary.operator_token.lexeme + expression_text(*unary.operand, own);
            break;
        }
        case ExprKind::Binary: {
            const auto& binary = static_cast<const BinaryExpr&>(expression);
            result = expression_text(*binary.left, own) + " "
                + binary.operator_token.lexeme + " "
                + expression_text(*binary.right, own + 1);
            break;
        }
        case ExprKind::Call: {
            const auto& call = static_cast<const CallExpr&>(expression);
            result = expression_text(*call.callee, own);
            if (!call.generic_arguments.empty()) {
                result += '<' + type_list_text(call.generic_arguments) + '>';
            }
            result += '(';
            for (std::size_t index = 0; index < call.arguments.size(); ++index) {
                if (index != 0) result += ", ";
                if (call.arguments[index].name) {
                    result += *call.arguments[index].name + ": ";
                }
                result += expression_text(*call.arguments[index].value);
            }
            result += ')';
            break;
        }
        case ExprKind::Index: {
            const auto& index = static_cast<const IndexExpr&>(expression);
            result = expression_text(*index.object, own) + "["
                + expression_text(*index.index) + "]";
            break;
        }
        case ExprKind::MemberAccess: {
            const auto& member = static_cast<const MemberAccessExpr&>(expression);
            result = expression_text(*member.object, own) + "." + member.member;
            break;
        }
        case ExprKind::TypeAccess: {
            const auto& access = static_cast<const TypeAccessExpr&>(expression);
            result = access.type_name + "::" + access.member;
            break;
        }
        case ExprKind::Propagation: {
            const auto& propagation = static_cast<const PropagationExpr&>(expression);
            result = expression_text(*propagation.expression, own) + '?';
            break;
        }
        case ExprKind::Cast: {
            const auto& cast = static_cast<const CastExpr&>(expression);
            result = expression_text(*cast.expression, own) + " as "
                + type_text(cast.target_type);
            break;
        }
        case ExprKind::Grouping:
            result = "(" + expression_text(
                *static_cast<const GroupingExpr&>(expression).expression) + ")";
            break;
        }
        return own < parent ? "(" + result + ")" : result;
    }

    void indent(std::size_t depth)
    {
        output_ << std::string(depth * 4, ' ');
    }

    void write_block(const BlockStmt& block, std::size_t depth)
    {
        output_ << "{\n";
        for (std::size_t index = 0; index < block.statements.size(); ++index) {
            write_statement(*block.statements[index], depth + 1);
            output_ << '\n';
        }
        indent(depth);
        output_ << '}';
    }

    void write_function_signature(
        std::string_view prefix,
        std::string_view name,
        const std::vector<GenericParameter>& generic_parameters,
        const std::vector<Parameter>& parameters,
        const std::optional<TypeReference>& return_type)
    {
        output_ << prefix << name << generic_parameters_text(generic_parameters)
            << '(' << parameters_text(parameters) << ')';
        if (return_type) output_ << " -> " << type_text(*return_type);
    }

    void write_if(const IfStmt& statement, std::size_t depth, bool continuation)
    {
        if (!continuation) indent(depth);
        output_ << "if " << expression_text(*statement.condition) << ' ';
        write_block(*statement.then_block, depth);
        if (statement.else_branch) {
            output_ << " else ";
            if (statement.else_branch->kind == StmtKind::If) {
                write_if(static_cast<const IfStmt&>(*statement.else_branch), depth, true);
            } else {
                write_block(
                    static_cast<const BlockStmt&>(*statement.else_branch), depth);
            }
        }
    }

    void write_method(const MethodDeclaration& method, std::size_t depth, bool in_class)
    {
        indent(depth);
        if (in_class && method.visibility == Visibility::Public) output_ << "public ";
        if (method.is_virtual) output_ << "virtual ";
        if (method.is_override) output_ << "override ";
        if (method.performance_contract == PerformanceContract::Performance) {
            output_ << "performance ";
        }
        const bool lifecycle = in_class
            && (method.name == "init" || method.name == "destroy");
        write_function_signature(
            lifecycle ? "" : "function ", method.name,
            method.generic_parameters, method.parameters, method.return_type);
        if (method.body) {
            output_ << ' ';
            write_block(*method.body, depth);
        }
    }

    void write_conversion(const ConversionOverload& conversion, std::size_t depth)
    {
        indent(depth);
        output_ << "overload as " << type_text(conversion.target_type) << ' ';
        write_block(*conversion.body, depth);
    }

    void write_statement(const Stmt& statement, std::size_t depth)
    {
        switch (statement.kind) {
        case StmtKind::ImportDeclaration:
            indent(depth);
            output_ << "import "
                << static_cast<const ImportDeclarationStmt&>(statement).module_path();
            return;
        case StmtKind::VariableDeclaration: {
            const auto& declaration = static_cast<const VariableDeclarationStmt&>(statement);
            indent(depth);
            output_ << declaration.name;
            if (declaration.explicit_type) {
                output_ << ": " << type_text(*declaration.explicit_type) << " = ";
            } else {
                output_ << " := ";
            }
            output_ << expression_text(*declaration.initializer);
            return;
        }
        case StmtKind::Assignment: {
            const auto& assignment = static_cast<const AssignmentStmt&>(statement);
            indent(depth);
            output_ << assignment.name << " = " << expression_text(*assignment.value);
            return;
        }
        case StmtKind::MemberAssignment: {
            const auto& assignment = static_cast<const MemberAssignmentStmt&>(statement);
            indent(depth);
            output_ << expression_text(*assignment.target) << " = "
                << expression_text(*assignment.value);
            return;
        }
        case StmtKind::IndexAssignment: {
            const auto& assignment = static_cast<const IndexAssignmentStmt&>(statement);
            indent(depth);
            output_ << expression_text(*assignment.target) << " = "
                << expression_text(*assignment.value);
            return;
        }
        case StmtKind::Expression:
            indent(depth);
            output_ << expression_text(
                *static_cast<const ExpressionStmt&>(statement).expression);
            return;
        case StmtKind::FunctionDeclaration: {
            const auto& function = static_cast<const FunctionDeclarationStmt&>(statement);
            indent(depth);
            if (statement.is_public) output_ << "public ";
            if (function.is_external) {
                output_ << "extern \"" << function.calling_convention << "\" ";
            }
            if (function.performance_contract == PerformanceContract::Performance) {
                output_ << "performance ";
            }
            write_function_signature(
                "function ", function.name, function.generic_parameters,
                function.parameters, function.return_type);
            if (function.is_external) return;
            output_ << ' ';
            write_block(*function.body, depth);
            return;
        }
        case StmtKind::Return: {
            const auto& result = static_cast<const ReturnStmt&>(statement);
            indent(depth);
            output_ << "return";
            if (result.value) output_ << ' ' << expression_text(*result.value);
            return;
        }
        case StmtKind::Block:
            indent(depth);
            write_block(static_cast<const BlockStmt&>(statement), depth);
            return;
        case StmtKind::If:
            write_if(static_cast<const IfStmt&>(statement), depth, false);
            return;
        case StmtKind::While: {
            const auto& loop = static_cast<const WhileStmt&>(statement);
            indent(depth);
            output_ << "while " << expression_text(*loop.condition) << ' ';
            write_block(*loop.body, depth);
            return;
        }
        case StmtKind::ForIn: {
            const auto& loop = static_cast<const ForInStmt&>(statement);
            indent(depth);
            output_ << "for " << loop.variable_name << " in "
                << expression_text(*loop.collection) << ' ';
            write_block(*loop.body, depth);
            return;
        }
        case StmtKind::Stop:
            indent(depth);
            output_ << "stop";
            return;
        case StmtKind::Continue:
            indent(depth);
            output_ << "continue";
            return;
        case StmtKind::EnumDeclaration: {
            const auto& declaration = static_cast<const EnumDeclarationStmt&>(statement);
            indent(depth);
            if (statement.is_public) output_ << "public ";
            output_ << "enum " << declaration.name << " {\n";
            for (const auto& variant : declaration.variants) {
                indent(depth + 1);
                output_ << variant.name;
                if (variant.payload_type) {
                    output_ << '(' << type_text(*variant.payload_type) << ')';
                }
                output_ << '\n';
            }
            indent(depth);
            output_ << '}';
            return;
        }
        case StmtKind::Handle: {
            const auto& handle = static_cast<const HandleStmt&>(statement);
            indent(depth);
            output_ << "handle " << expression_text(*handle.expression) << " {\n";
            for (std::size_t index = 0; index < handle.cases.size(); ++index) {
                if (index != 0) output_ << '\n';
                const auto& handle_case = handle.cases[index];
                indent(depth + 1);
                output_ << handle_case.variant_name;
                if (handle_case.binding_name) {
                    output_ << '(' << *handle_case.binding_name << ')';
                }
                output_ << ' ';
                write_block(*handle_case.body, depth + 1);
                output_ << '\n';
            }
            indent(depth);
            output_ << '}';
            return;
        }
        case StmtKind::StructDeclaration: {
            const auto& declaration = static_cast<const StructDeclarationStmt&>(statement);
            indent(depth);
            if (statement.is_public) output_ << "public ";
            output_ << "struct " << declaration.name
                << generic_parameters_text(declaration.generic_parameters);
            if (!declaration.interfaces.empty()) {
                output_ << " implements " << type_list_text(declaration.interfaces);
            }
            output_ << " {\n";
            bool first = true;
            const auto separator = [&] {
                if (!first) output_ << "\n";
                first = false;
            };
            for (const auto& field : declaration.fields) {
                separator();
                indent(depth + 1);
                output_ << field.name << ": " << type_text(field.type);
                if (field.default_value) {
                    output_ << " = " << expression_text(*field.default_value);
                }
                output_ << '\n';
            }
            for (const auto& method : declaration.methods) {
                separator();
                write_method(
                    static_cast<const MethodDeclaration&>(*method), depth + 1, false);
                output_ << '\n';
            }
            for (const auto& conversion : declaration.conversions) {
                separator();
                write_conversion(*conversion, depth + 1);
                output_ << '\n';
            }
            indent(depth);
            output_ << '}';
            return;
        }
        case StmtKind::ClassDeclaration: {
            const auto& declaration = static_cast<const ClassDeclarationStmt&>(statement);
            indent(depth);
            if (statement.is_public) output_ << "public ";
            if (declaration.is_abstract) output_ << "abstract ";
            output_ << "class " << declaration.name
                << generic_parameters_text(declaration.generic_parameters);
            if (declaration.base_type) {
                output_ << " : " << type_text(*declaration.base_type);
            }
            if (!declaration.interfaces.empty()) {
                output_ << " implements " << type_list_text(declaration.interfaces);
            }
            output_ << " {\n";
            for (std::size_t index = 0; index < declaration.members.size(); ++index) {
                if (index != 0) output_ << '\n';
                const auto& member = *declaration.members[index];
                if (member.kind == ClassMemberKind::Field) {
                    const auto& field = static_cast<const ClassField&>(member);
                    indent(depth + 1);
                    if (field.visibility == Visibility::Public) output_ << "public ";
                    if (field.is_weak) output_ << "weak ";
                    output_ << field.name << ": " << type_text(field.type);
                    if (field.default_value) {
                        output_ << " = " << expression_text(*field.default_value);
                    }
                } else if (member.kind == ClassMemberKind::Method) {
                    write_method(
                        static_cast<const MethodDeclaration&>(member), depth + 1, true);
                } else {
                    write_conversion(
                        static_cast<const ConversionOverload&>(member), depth + 1);
                }
                output_ << '\n';
            }
            indent(depth);
            output_ << '}';
            return;
        }
        case StmtKind::InterfaceDeclaration: {
            const auto& declaration = static_cast<const InterfaceDeclarationStmt&>(statement);
            indent(depth);
            if (statement.is_public) output_ << "public ";
            output_ << "interface " << declaration.name
                << generic_parameters_text(declaration.generic_parameters) << " {\n";
            for (const auto& method : declaration.methods) {
                indent(depth + 1);
                write_function_signature(
                    "function ", method.name, method.generic_parameters,
                    method.parameters, method.return_type);
                output_ << '\n';
            }
            indent(depth);
            output_ << '}';
            return;
        }
        }
    }

    std::ostringstream output_;
};

} // namespace

std::string Formatter::format(std::string_view source) const
{
    auto tokens = Lexer(source).tokenize();
    const auto program = Parser(std::move(tokens)).parse_program();
    return AstFormatter{}.format(program);
}

bool Formatter::format_file(
    const std::filesystem::path& path,
    bool check_only) const
{
    return !format_files({path}, check_only).empty();
}

std::vector<std::filesystem::path> Formatter::format_files(
    const std::vector<std::filesystem::path>& paths,
    bool check_only) const
{
    struct PendingFile {
        std::filesystem::path path;
        std::string contents;
    };
    std::vector<PendingFile> pending;
    std::vector<std::filesystem::path> changed;
    for (const auto& path : paths) {
        const SourceFile source = load_source_file(path);
        std::string formatted;
        try {
            formatted = format(source.contents);
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "cannot format '" + path.string() + "': " + error.what());
        }
        if (formatted != source.contents) {
            changed.push_back(path);
            pending.push_back(PendingFile{path, std::move(formatted)});
        }
    }
    if (check_only) return changed;

    for (const auto& file : pending) {
        const std::filesystem::path temporary =
            file.path.string() + ".toro-format.tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error(
                    "could not create formatter temporary file: "
                        + temporary.string());
            }
            output << file.contents;
            if (!output) {
                std::filesystem::remove(temporary);
                throw std::runtime_error(
                    "could not write formatted source: " + file.path.string());
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, file.path, error);
        if (error) {
            std::filesystem::remove(temporary);
            throw std::runtime_error(
                "could not replace formatted source '" + file.path.string()
                    + "': " + error.message());
        }
    }
    return changed;
}

} // namespace toro
