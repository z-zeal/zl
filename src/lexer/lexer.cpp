#include <stdexcept>
#include "zl/lexer/lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace zl {

// This map is built once, at first use, like a static readonly Dictionary in C#.
static const std::unordered_map<std::string, TokenType>& keywords() {
    static const std::unordered_map<std::string, TokenType> table{
        {"var", TokenType::KW_VAR},
        {"let", TokenType::KW_LET},
        {"data", TokenType::KW_DATA},
        {"enum", TokenType::KW_ENUM},
        {"import", TokenType::KW_IMPORT},
        {"func", TokenType::KW_FUNC},
        {"async", TokenType::KW_ASYNC},
        {"await", TokenType::KW_AWAIT},
        {"operator", TokenType::KW_OPERATOR},
        {"return", TokenType::KW_RETURN},
        {"class", TokenType::KW_CLASS},
        {"static", TokenType::KW_STATIC},
        {"public", TokenType::KW_PUBLIC},
        {"private", TokenType::KW_PRIVATE},
        {"protected", TokenType::KW_PROTECTED},
        {"new", TokenType::KW_NEW},
        {"this", TokenType::KW_THIS},
        {"extends", TokenType::KW_EXTENDS},
        {"with", TokenType::KW_WITH},
        {"implements", TokenType::KW_IMPLEMENTS},
        {"interface", TokenType::KW_INTERFACE},
        {"super", TokenType::KW_SUPER},
        {"void", TokenType::KW_VOID},
        {"array", TokenType::KW_ARRAY},
        {"list", TokenType::KW_LIST},
        {"set", TokenType::KW_SET},
        {"map", TokenType::KW_MAP},
        {"gc", TokenType::KW_GC},
        {"owned", TokenType::KW_OWNED},
        {"borrow", TokenType::KW_BORROW},
        {"shared", TokenType::KW_SHARED},
        {"move", TokenType::KW_MOVE},
        {"if", TokenType::KW_IF},
        {"else", TokenType::KW_ELSE},
        {"elif", TokenType::KW_ELSEIF},        // "else if" (2 tokens) is combined later, in the parser
        {"for", TokenType::KW_FOR},
        {"in", TokenType::KW_IN},
        {"step", TokenType::KW_STEP},
        {"repeat", TokenType::KW_REPEAT},
        {"while", TokenType::KW_WHILE},
        {"break", TokenType::KW_BREAK},
        {"continue", TokenType::KW_CONTINUE},
        {"try", TokenType::KW_TRY},
        {"catch", TokenType::KW_CATCH},
        {"throw", TokenType::KW_THROW},
        {"finally", TokenType::KW_FINALLY},
        {"match", TokenType::KW_MATCH},
        {"true", TokenType::KW_TRUE},
        {"false", TokenType::KW_FALSE},
        {"null", TokenType::KW_NULL},
        {"log", TokenType::KW_LOG},

    };
    return table;
}

// Constructor: `source` (the parameter) is copied/moved into `source_` (the member).
// This colon syntax is a "member initializer list" - the idiomatic way to set
// members, done BEFORE the constructor body runs.
Lexer::Lexer(std::string source) : source_(std::move(source)) {}

bool Lexer::isAtEnd() const {
    return pos_ >= source_.size();
}

char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return source_[pos_];
}

char Lexer::peekNext() const {
    if (pos_ + 1 >= source_.size()) return '\0';
    return source_[pos_ + 1];
}

bool Lexer::hasExponentDigits() const {
    // Called with the current character being the `e`/`E` itself, so the
    // exponent's digits start one position on - past an optional sign.
    std::size_t i = pos_ + 1;
    if (i < source_.size() && (source_[i] == '+' || source_[i] == '-')) ++i;
    return i < source_.size() && std::isdigit(static_cast<unsigned char>(source_[i]));
}

char Lexer::advance() {
    char c = source_[pos_++];
    if (c == '\n') {
        line_++;
        column_ = 1;
    } else {
        column_++;
    }
    return c;
}

bool Lexer::match(char expected) {
    if (isAtEnd() || source_[pos_] != expected) return false;
    advance();
    return true;
}

void Lexer::skipWhitespaceAndComments() {
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peekNext() == '/') {
            // line comment - consume until newline
            while (!isAtEnd() && peek() != '\n') advance();
        } else {
            break;
        }
    }
}

Token Lexer::makeToken(TokenType type, const std::string& lexeme, std::size_t startLine, std::size_t startCol) {
    return Token{type, lexeme, startLine, startCol};
}

Token Lexer::scanNumber() {
    std::size_t startLine = line_, startCol = column_;
    std::string text;
    TokenType type = TokenType::INT_LITERAL;

    // Accept both `12.5` and `.5` as floating-point literals.
    // The caller only enters this routine for a leading `.` when the next
    // character is a digit, so member access (`object.field`) remains DOT.
    if (peek() == '.') {
        type = TokenType::FLOAT_LITERAL;
        text += advance(); // consume '.'
        while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
            text += advance();
        }
        if ((peek() == 'e' || peek() == 'E') && hasExponentDigits()) {
            text += advance();
            if (peek() == '+' || peek() == '-') text += advance();
            while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
                text += advance();
            }
        }
        return Token{type, text, startLine, startCol};
    }

    while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
        text += advance();
    }
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peekNext()))) {
        type = TokenType::FLOAT_LITERAL;
        text += advance(); // consume '.'
        while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
            text += advance();
        }
    }
    // Exponent part, as in `1e5`, `1E+21` or `1.5e-3`. The printer emits this
    // form (`log(1e21)` prints `1e+21`), so refusing to lex it would make the
    // language unable to read back its own output.
    //
    // Only taken when at least one digit follows, so `1e` stays an integer
    // literal followed by the identifier `e` and keeps its existing error.
    if ((peek() == 'e' || peek() == 'E') && hasExponentDigits()) {
        type = TokenType::FLOAT_LITERAL;
        text += advance(); // consume 'e'/'E'
        if (peek() == '+' || peek() == '-') text += advance();
        while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
            text += advance();
        }
    }
    return Token{type, text, startLine, startCol};
}

Token Lexer::scanString() {
    std::size_t startLine = line_, startCol = column_;
    advance(); // consume opening "
    std::string text;
    while (!isAtEnd() && peek() != '"') {
        char c = advance();
        if (c == '\\' && !isAtEnd()) {
            char esc = advance();
            switch (esc) {
                case 'n':  text += '\n'; break;
                case 't':  text += '\t'; break;
                case 'r':  text += '\r'; break;
                case '\\': text += '\\'; break;
                case '"':  text += '"';  break;
                case '0':  text += '\0'; break;
                default:   text += esc;  break; // unknown escape - just keep the char, e.g. \q -> q
            }
        } else {
            text += c;
        }
    }
    if (isAtEnd()) {
        throw std::runtime_error("Unterminated string literal");
    }
    advance(); // consume closing "
    return Token{TokenType::STRING_LITERAL, text, startLine, startCol};
}

Token Lexer::scanRegexLiteral() {
    std::size_t startLine = line_, startCol = column_;
    advance(); // consume opening '#'
    std::string pattern;
    while (!isAtEnd()) {
        char c = peek();
        if (c == '#') {
            advance();
            return Token{TokenType::REGEX_LITERAL, pattern, startLine, startCol};
        }
        if (c == '\\') {
            pattern += advance();
            if (isAtEnd()) throw std::runtime_error("Unterminated regex literal");
            pattern += advance();
            continue;
        }
        if (c == '/' && peekNext() == '/') {
            while (!isAtEnd() && peek() != '\n') advance();
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
            continue;
        }
        pattern += advance();
    }
    throw std::runtime_error("Unterminated regex literal");
}

Token Lexer::scanIdentifierOrKeyword() {
    std::size_t startLine = line_, startCol = column_;
    std::string text;
    while (!isAtEnd() &&
           (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
        text += advance();
    }
    auto it = keywords().find(text);
    TokenType type = (it != keywords().end()) ? it->second : TokenType::IDENTIFIER;
    return Token{type, text, startLine, startCol};
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    while (true) {
        skipWhitespaceAndComments();
        if (isAtEnd()) break;

        std::size_t startLine = line_, startCol = column_;
        char c = peek();

        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && std::isdigit(static_cast<unsigned char>(peekNext())))) {
            tokens.push_back(scanNumber());
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            tokens.push_back(scanIdentifierOrKeyword());
            continue;
        }
        if (c == '"') {
            tokens.push_back(scanString());
            continue;
        }

        advance(); // consume the symbol char
        switch (c) {
            case '(': tokens.push_back(makeToken(TokenType::LPAREN, "(", startLine, startCol)); break;
            case ')': tokens.push_back(makeToken(TokenType::RPAREN, ")", startLine, startCol)); break;
            case '{': tokens.push_back(makeToken(TokenType::LBRACE, "{", startLine, startCol)); break;
            case '}': tokens.push_back(makeToken(TokenType::RBRACE, "}", startLine, startCol)); break;
            case ',': tokens.push_back(makeToken(TokenType::COMMA, ",", startLine, startCol)); break;
            case '+': tokens.push_back(makeToken(TokenType::PLUS, "+", startLine, startCol)); break;
            case '-': tokens.push_back(makeToken(TokenType::MINUS, "-", startLine, startCol)); break;
            case '*': tokens.push_back(makeToken(TokenType::STAR, "*", startLine, startCol)); break;
            case '/': tokens.push_back(makeToken(TokenType::SLASH, "/", startLine, startCol)); break;
            case '%': tokens.push_back(makeToken(TokenType::PERCENT, "%", startLine, startCol)); break;
            case '^':
                if (match('^')) tokens.push_back(makeToken(TokenType::POW, "^^", startLine, startCol));
                else tokens.push_back(makeToken(TokenType::BIT_XOR, "^", startLine, startCol));
                break;
            case '[': tokens.push_back(makeToken(TokenType::LBRACKET, "[", startLine, startCol)); break;
            case ']': tokens.push_back(makeToken(TokenType::RBRACKET, "]", startLine, startCol)); break;
            case ';': tokens.push_back(makeToken(TokenType::SEMICOLON, ";", startLine, startCol)); break;
            case ':': tokens.push_back(makeToken(TokenType::COLON, ":", startLine, startCol)); break;
            case '@': tokens.push_back(makeToken(TokenType::AT, "@", startLine, startCol)); break;
            case '#':
                // '#' is the regex literal delimiter. scanRegexLiteral() expects
                // the opening delimiter still to be current, so rewind one char.
                --pos_; --column_;
                tokens.push_back(scanRegexLiteral());
                break;
            case '&':
                if (match('&')) tokens.push_back(makeToken(TokenType::AND, "&&", startLine, startCol));
                else tokens.push_back(makeToken(TokenType::BIT_AND, "&", startLine, startCol));
                break;
            case '|':
                if (match('|')) tokens.push_back(makeToken(TokenType::OR, "||", startLine, startCol));
                else tokens.push_back(makeToken(TokenType::BIT_OR, "|", startLine, startCol));
                break;
            case '~':
                tokens.push_back(makeToken(TokenType::BIT_NOT, "~", startLine, startCol));
                break;
            case '.':
                if (match('.')) tokens.push_back(makeToken(TokenType::DOT_DOT, "..", startLine, startCol));
                else tokens.push_back(makeToken(TokenType::DOT, ".", startLine, startCol));
                break;
            case '=':
                if (match('=')) tokens.push_back(makeToken(TokenType::EQ, "==", startLine, startCol));
                else if (match('>')) tokens.push_back(makeToken(TokenType::FAT_ARROW, "=>", startLine, startCol));
                else tokens.push_back(makeToken(TokenType::ASSIGN, "=", startLine, startCol));
                break;
            case '!':
                if (match('=')) tokens.push_back(makeToken(TokenType::NEQ, "!=", startLine, startCol));
                else tokens.push_back(makeToken(TokenType::NOT, "!", startLine, startCol));
                break;
            case '<':
                if (match('<')) tokens.push_back(makeToken(TokenType::SHL, "<<", startLine, startCol));
                else if (match('=')) tokens.push_back(makeToken(TokenType::LTE, "<=", startLine, startCol));
                else tokens.push_back(makeToken(TokenType::LT, "<", startLine, startCol));
                break;
            case '>':
                if (match('>')) {
                    if (match('>')) tokens.push_back(makeToken(TokenType::USHR, ">>>", startLine, startCol));
                    else tokens.push_back(makeToken(TokenType::SHR, ">>", startLine, startCol));
                } else if (match('=')) {
                    tokens.push_back(makeToken(TokenType::GTE, ">=", startLine, startCol));
                } else {
                    tokens.push_back(makeToken(TokenType::GT, ">", startLine, startCol));
                }
                break;
            default:
                tokens.push_back(Token{TokenType::UNKNOWN, std::string(1, c), startLine, startCol});
                break;
        }
    }

    tokens.push_back(Token{TokenType::END_OF_FILE, "", line_, column_});
    return tokens;
}

} // namespace zl
