#pragma once

#include "toro/Token.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace toro {

class Lexer {
public:
    explicit Lexer(std::string_view source);

    [[nodiscard]] std::vector<Token> tokenize();

private:
    [[nodiscard]] bool at_end() const;
    char advance();
    [[nodiscard]] char peek() const;
    [[nodiscard]] char peek_next() const;
    bool match(char expected);

    void scan_token();
    void scan_identifier();
    void scan_number();
    void scan_string();
    void add_token(TokenType type);
    void add_token(TokenType type, std::string_view lexeme);

    std::string_view source_;
    std::size_t start_{0};
    std::size_t current_{0};
    std::size_t line_{1};
    std::size_t column_{1};
    std::size_t token_line_{1};
    std::size_t token_column_{1};
    std::vector<Token> tokens_;
};

} // namespace toro
