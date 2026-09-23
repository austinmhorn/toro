#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace toro {

enum class TokenType {
    Identifier,
    Integer,
    Decimal,
    String,

    Function,
    Return,
    If,
    Else,
    While,
    For,
    In,
    Stop,
    Continue,
    Handle,
    Public,
    Private,
    Class,
    Struct,
    Enum,
    Interface,
    Virtual,
    Override,
    Abstract,
    Implements,
    True,
    False,
    Null,
    Self,
    And,
    Or,
    As,
    Weak,
    Performance,

    Plus,
    Minus,
    Star,
    Slash,
    Assign,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Declare,
    Arrow,

    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Colon,
    Comma,
    Dot,
    Question,

    EndOfFile,
};

struct Token {
    TokenType type;
    std::string lexeme;
    std::size_t line;
    std::size_t column;
};

[[nodiscard]] constexpr std::string_view token_type_name(TokenType type)
{
    switch (type) {
    case TokenType::Identifier: return "Identifier";
    case TokenType::Integer: return "Integer";
    case TokenType::Decimal: return "Decimal";
    case TokenType::String: return "String";
    case TokenType::Function: return "Function";
    case TokenType::Return: return "Return";
    case TokenType::If: return "If";
    case TokenType::Else: return "Else";
    case TokenType::While: return "While";
    case TokenType::For: return "For";
    case TokenType::In: return "In";
    case TokenType::Stop: return "Stop";
    case TokenType::Continue: return "Continue";
    case TokenType::Handle: return "Handle";
    case TokenType::Public: return "Public";
    case TokenType::Private: return "Private";
    case TokenType::Class: return "Class";
    case TokenType::Struct: return "Struct";
    case TokenType::Enum: return "Enum";
    case TokenType::Interface: return "Interface";
    case TokenType::Virtual: return "Virtual";
    case TokenType::Override: return "Override";
    case TokenType::Abstract: return "Abstract";
    case TokenType::Implements: return "Implements";
    case TokenType::True: return "True";
    case TokenType::False: return "False";
    case TokenType::Null: return "Null";
    case TokenType::Self: return "Self";
    case TokenType::And: return "And";
    case TokenType::Or: return "Or";
    case TokenType::As: return "As";
    case TokenType::Weak: return "Weak";
    case TokenType::Performance: return "Performance";
    case TokenType::Plus: return "Plus";
    case TokenType::Minus: return "Minus";
    case TokenType::Star: return "Star";
    case TokenType::Slash: return "Slash";
    case TokenType::Assign: return "Assign";
    case TokenType::Equal: return "Equal";
    case TokenType::NotEqual: return "NotEqual";
    case TokenType::Less: return "Less";
    case TokenType::LessEqual: return "LessEqual";
    case TokenType::Greater: return "Greater";
    case TokenType::GreaterEqual: return "GreaterEqual";
    case TokenType::Declare: return "Declare";
    case TokenType::Arrow: return "Arrow";
    case TokenType::LeftParen: return "LeftParen";
    case TokenType::RightParen: return "RightParen";
    case TokenType::LeftBrace: return "LeftBrace";
    case TokenType::RightBrace: return "RightBrace";
    case TokenType::LeftBracket: return "LeftBracket";
    case TokenType::RightBracket: return "RightBracket";
    case TokenType::Colon: return "Colon";
    case TokenType::Comma: return "Comma";
    case TokenType::Dot: return "Dot";
    case TokenType::Question: return "Question";
    case TokenType::EndOfFile: return "EndOfFile";
    }

    return "Unknown";
}

} // namespace toro
