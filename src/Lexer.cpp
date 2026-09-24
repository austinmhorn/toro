#include "toro/Lexer.hpp"

#include <array>
#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>

namespace toro {
namespace {

using Keyword = std::pair<std::string_view, TokenType>;

constexpr std::array<Keyword, 30> keywords{{
    {"function", TokenType::Function},
    {"return", TokenType::Return},
    {"if", TokenType::If},
    {"else", TokenType::Else},
    {"while", TokenType::While},
    {"for", TokenType::For},
    {"in", TokenType::In},
    {"stop", TokenType::Stop},
    {"continue", TokenType::Continue},
    {"handle", TokenType::Handle},
    {"public", TokenType::Public},
    {"private", TokenType::Private},
    {"class", TokenType::Class},
    {"struct", TokenType::Struct},
    {"enum", TokenType::Enum},
    {"interface", TokenType::Interface},
    {"virtual", TokenType::Virtual},
    {"override", TokenType::Override},
    {"abstract", TokenType::Abstract},
    {"implements", TokenType::Implements},
    {"overload", TokenType::Overload},
    {"true", TokenType::True},
    {"false", TokenType::False},
    {"null", TokenType::Null},
    {"self", TokenType::Self},
    {"and", TokenType::And},
    {"or", TokenType::Or},
    {"as", TokenType::As},
    {"weak", TokenType::Weak},
    {"performance", TokenType::Performance},
}};

bool is_identifier_start(char character)
{
    const auto value = static_cast<unsigned char>(character);
    return std::isalpha(value) != 0 || character == '_';
}

bool is_identifier_part(char character)
{
    const auto value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_';
}

bool is_digit(char character)
{
    return std::isdigit(static_cast<unsigned char>(character)) != 0;
}

TokenType identifier_type(std::string_view lexeme)
{
    for (const auto& [keyword, type] : keywords) {
        if (lexeme == keyword) {
            return type;
        }
    }

    return TokenType::Identifier;
}

} // namespace

Lexer::Lexer(std::string_view source)
    : source_(source)
{
}

std::vector<Token> Lexer::tokenize()
{
    start_ = 0;
    current_ = 0;
    line_ = 1;
    column_ = 1;
    token_line_ = 1;
    token_column_ = 1;
    tokens_.clear();

    while (!at_end()) {
        start_ = current_;
        token_line_ = line_;
        token_column_ = column_;
        scan_token();
    }

    tokens_.push_back(Token{TokenType::EndOfFile, {}, line_, column_});
    return tokens_;
}

bool Lexer::at_end() const
{
    return current_ >= source_.size();
}

char Lexer::advance()
{
    const char character = source_[current_++];
    if (character == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return character;
}

char Lexer::peek() const
{
    return at_end() ? '\0' : source_[current_];
}

char Lexer::peek_next() const
{
    return current_ + 1 >= source_.size() ? '\0' : source_[current_ + 1];
}

bool Lexer::match(char expected)
{
    if (at_end() || source_[current_] != expected) {
        return false;
    }

    advance();
    return true;
}

void Lexer::scan_token()
{
    const char character = advance();

    if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
        return;
    }

    if (is_identifier_start(character)) {
        scan_identifier();
        return;
    }

    if (is_digit(character)) {
        scan_number();
        return;
    }

    switch (character) {
    case '"': scan_string(); break;
    case '+': add_token(TokenType::Plus); break;
    case '-': add_token(match('>') ? TokenType::Arrow : TokenType::Minus); break;
    case '*': add_token(TokenType::Star); break;
    case '/': add_token(TokenType::Slash); break;
    case '=': add_token(match('=') ? TokenType::Equal : TokenType::Assign); break;
    case '!':
        if (match('=')) {
            add_token(TokenType::NotEqual);
            break;
        }
        throw std::runtime_error(
            "line " + std::to_string(token_line_) + ", column "
            + std::to_string(token_column_) + ": invalid character '!'");
    case '<': add_token(match('=') ? TokenType::LessEqual : TokenType::Less); break;
    case '>': add_token(match('=') ? TokenType::GreaterEqual : TokenType::Greater); break;
    case ':':
        if (match(':')) {
            add_token(TokenType::DoubleColon);
        } else {
            add_token(match('=') ? TokenType::Declare : TokenType::Colon);
        }
        break;
    case '(': add_token(TokenType::LeftParen); break;
    case ')': add_token(TokenType::RightParen); break;
    case '{': add_token(TokenType::LeftBrace); break;
    case '}': add_token(TokenType::RightBrace); break;
    case '[': add_token(TokenType::LeftBracket); break;
    case ']': add_token(TokenType::RightBracket); break;
    case ',': add_token(TokenType::Comma); break;
    case '.': add_token(TokenType::Dot); break;
    case '?': add_token(TokenType::Question); break;
    default:
        throw std::runtime_error(
            "line " + std::to_string(token_line_) + ", column "
            + std::to_string(token_column_) + ": invalid character '"
            + std::string(1, character) + "'");
    }
}

void Lexer::scan_identifier()
{
    while (is_identifier_part(peek())) {
        advance();
    }

    const auto lexeme = source_.substr(start_, current_ - start_);
    add_token(identifier_type(lexeme), lexeme);
}

void Lexer::scan_number()
{
    while (is_digit(peek())) {
        advance();
    }

    auto type = TokenType::Integer;
    if (peek() == '.' && is_digit(peek_next())) {
        type = TokenType::Decimal;
        advance();
        while (is_digit(peek())) {
            advance();
        }
    }

    add_token(type);
}

void Lexer::scan_string()
{
    while (!at_end() && peek() != '"') {
        advance();
    }

    if (at_end()) {
        throw std::runtime_error(
            "line " + std::to_string(token_line_) + ", column "
            + std::to_string(token_column_) + ": unterminated string");
    }

    advance();
    add_token(TokenType::String, source_.substr(start_ + 1, current_ - start_ - 2));
}

void Lexer::add_token(TokenType type)
{
    add_token(type, source_.substr(start_, current_ - start_));
}

void Lexer::add_token(TokenType type, std::string_view lexeme)
{
    tokens_.push_back(Token{type, std::string(lexeme), token_line_, token_column_});
}

} // namespace toro
