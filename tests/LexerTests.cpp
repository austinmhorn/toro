#include "toro/Lexer.hpp"
#include "toro/Token.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using toro::Token;
using toro::TokenType;

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::vector<Token> tokenize(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    expect(!tokens.empty(), "token list must not be empty");
    expect(tokens.back().type == TokenType::EndOfFile, "token list must end with EOF");

    std::size_t eof_count = 0;
    for (const auto& token : tokens) {
        if (token.type == TokenType::EndOfFile) {
            ++eof_count;
        }
    }
    expect(eof_count == 1, "token list must contain exactly one EOF token");
    return tokens;
}

void expect_token(
    const Token& token,
    TokenType type,
    std::string_view lexeme,
    std::size_t line = 1,
    std::size_t column = 1)
{
    expect(token.type == type, "unexpected token type");
    expect(token.lexeme == lexeme, "unexpected token lexeme");
    expect(token.line == line, "unexpected token line");
    expect(token.column == column, "unexpected token column");
}

void test_keywords_and_identifiers()
{
    constexpr std::string_view source =
        "function return if else while for in stop continue handle "
        "public private class struct enum interface virtual override abstract implements "
        "true false null self and or as weak performance function_name";
    constexpr std::array expected{
        TokenType::Function, TokenType::Return, TokenType::If, TokenType::Else,
        TokenType::While, TokenType::For, TokenType::In, TokenType::Stop,
        TokenType::Continue, TokenType::Handle, TokenType::Public, TokenType::Private,
        TokenType::Class, TokenType::Struct, TokenType::Enum, TokenType::Interface,
        TokenType::Virtual, TokenType::Override, TokenType::Abstract, TokenType::Implements,
        TokenType::True,
        TokenType::False, TokenType::Null, TokenType::Self, TokenType::And, TokenType::Or,
        TokenType::As, TokenType::Weak,
        TokenType::Performance, TokenType::Identifier,
    };

    const auto tokens = tokenize(source);
    expect(tokens.size() == expected.size() + 1, "unexpected keyword token count");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expect(tokens[index].type == expected[index], "keyword classified incorrectly");
    }
    expect(tokens[expected.size() - 1].lexeme == "function_name", "identifier lexeme changed");
}

void test_numbers()
{
    const auto tokens = tokenize("10 19.99 0.0 19. -10");
    expect_token(tokens[0], TokenType::Integer, "10");
    expect_token(tokens[1], TokenType::Decimal, "19.99", 1, 4);
    expect_token(tokens[2], TokenType::Decimal, "0.0", 1, 10);
    expect_token(tokens[3], TokenType::Integer, "19", 1, 14);
    expect_token(tokens[4], TokenType::Dot, ".", 1, 16);
    expect_token(tokens[5], TokenType::Minus, "-", 1, 18);
    expect_token(tokens[6], TokenType::Integer, "10", 1, 19);
}

void test_strings()
{
    const auto tokens = tokenize("\"Hello, toro!\"");
    expect_token(tokens[0], TokenType::String, "Hello, toro!");
}

void test_operators_and_punctuation()
{
    const auto tokens = tokenize(
        ":= -> = == != < <= > >= + - * / ( ) { } [ ] : , . ?");
    constexpr std::array expected{
        TokenType::Declare, TokenType::Arrow, TokenType::Assign, TokenType::Equal,
        TokenType::NotEqual, TokenType::Less, TokenType::LessEqual, TokenType::Greater,
        TokenType::GreaterEqual, TokenType::Plus, TokenType::Minus, TokenType::Star,
        TokenType::Slash, TokenType::LeftParen, TokenType::RightParen,
        TokenType::LeftBrace, TokenType::RightBrace, TokenType::LeftBracket,
        TokenType::RightBracket, TokenType::Colon, TokenType::Comma, TokenType::Dot,
        TokenType::Question,
    };

    expect(tokens.size() == expected.size() + 1, "unexpected operator token count");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expect(tokens[index].type == expected[index], "operator or punctuation classified incorrectly");
    }
}

void test_line_and_column_tracking()
{
    const auto tokens = tokenize("function\n  name := 10");
    expect_token(tokens[0], TokenType::Function, "function", 1, 1);
    expect_token(tokens[1], TokenType::Identifier, "name", 2, 3);
    expect_token(tokens[2], TokenType::Declare, ":=", 2, 8);
    expect_token(tokens[3], TokenType::Integer, "10", 2, 11);
    expect_token(tokens[4], TokenType::EndOfFile, "", 2, 13);
}

void expect_lexer_error(std::string_view source, std::string_view expected_message)
{
    try {
        static_cast<void>(toro::Lexer(source).tokenize());
    } catch (const std::runtime_error& error) {
        expect(std::string_view(error.what()).find(expected_message) != std::string_view::npos,
            "lexer error message was not descriptive");
        return;
    }
    throw std::runtime_error("expected lexer failure");
}

void test_failures()
{
    expect_lexer_error("@", "line 1, column 1: invalid character '@'");
    expect_lexer_error("\"unfinished", "line 1, column 1: unterminated string");
}

} // namespace

int main()
{
    try {
        test_keywords_and_identifiers();
        test_numbers();
        test_strings();
        test_operators_and_punctuation();
        test_line_and_column_tracking();
        test_failures();
    } catch (const std::exception& error) {
        std::cerr << "lexer test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "lexer tests passed\n";
    return 0;
}
