#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "token.hpp"

namespace zl {

class Lexer {
public:
    // `explicit` blocks accidental implicit conversion, e.g. Lexer l = someString;
    // Passing std::string by value here because we intend to own a copy of it.
    explicit Lexer(std::string source);

    // [[nodiscard]] = compiler warns you if you call tokenize() and ignore the result.
    // No `const` after the () because this mutates internal position/line/column state.
    [[nodiscard]] std::vector<Token> tokenize();

private:
    // --- helper methods, all implemented in lexer.cpp ---
    [[nodiscard]] bool isAtEnd() const;
    char peek() const;        // look at current char without consuming it
    char peekNext() const;    // look one char ahead
    // True when the exponent following the current `e`/`E` has at least one
    // digit, allowing for an optional sign. Lookahead only - consumes nothing.
    [[nodiscard]] bool hasExponentDigits() const;
    char advance();           // consume and return the current char
    bool match(char expected); // consume current char IF it equals `expected`

    void skipWhitespaceAndComments();
    Token makeToken(TokenType type, const std::string& lexeme, std::size_t startLine, std::size_t startCol);
    Token scanNumber();
    Token scanString();
    Token scanRegexLiteral();
    Token scanIdentifierOrKeyword();

    std::string source_;
    std::size_t pos_{0};
    std::size_t line_{1};
    std::size_t column_{1};
};

} // namespace zl
