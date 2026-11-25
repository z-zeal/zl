#pragma once

#include <cstddef>
#include <string>

namespace zl {
    enum class TokenType {
        // Literals
        INT_LITERAL,
        DECIMAL_LITERAL,
        FLOAT_LITERAL,
        STRING_LITERAL,
        REGEX_LITERAL,
        BOOL_LITERAL,
        IDENTIFIER,

        // Keywords - declarations
        KW_VAR,
        KW_LET,
        KW_DATA,
        KW_ENUM,
        KW_IMPORT,
        KW_FUNC,      // "func"
        KW_ASYNC,     // "async" modifier for async func declarations
        KW_AWAIT,     // "await" expression operator
        KW_OPERATOR,      // operator declaration keyword
        KW_RETURN,
        KW_CLASS,
        KW_STATIC,
        KW_PUBLIC,
        KW_PRIVATE,
        KW_PROTECTED,
        KW_NEW,
        KW_THIS,
        KW_EXTENDS,
        KW_WITH,
        KW_IMPLEMENTS,
        KW_INTERFACE,
        KW_SUPER,
        KW_VOID,
        KW_ARRAY,
        KW_LIST,
        KW_SET,
        KW_MAP,
        KW_GC,
        KW_OWNED,
        KW_BORROW,
        KW_SHARED,
        KW_MOVE,

        // Keywords - conditionals
        KW_IF,
        KW_ELSE,
        KW_ELSEIF,        // "elif" maps directly here; "else if" (two tokens) is combined by the parser

        // Keywords - loops
        KW_FOR,
        KW_IN,
        KW_STEP,
        KW_REPEAT,
        KW_WHILE,
        KW_BREAK,
        KW_CONTINUE,
        KW_TRY,
        KW_CATCH,
        KW_THROW,
        KW_FINALLY,   // "finally"

        // Keywords - match
        KW_MATCH,

        // Keywords - literals
        KW_TRUE,
        KW_FALSE,
        KW_NULL,

        // Keywords - builtins
        KW_LOG,           // log(...) - was "print" in the earlier draft

        // Symbols
        LBRACE,           // {
        RBRACE,           // }
        LPAREN,           // (
        RPAREN,           // )
        LBRACKET,         // [
        RBRACKET,         // ]
        SEMICOLON,        // ;
        COLON,            // :
        QUESTION,         // ?  (the `cond ? then : else` expression)
        COMMA,            // ,
        DOT,              // .
        DOT_DOT,          // ..
        AT,               // @

        // Operators - arithmetic
        PLUS,             // +
        MINUS,            // -
        STAR,             // *
        SLASH,            // /
        PERCENT,          // %
        POW,              // ^^  (2^^2, not 2**2)

        // Operators - bitwise (Java-style)
        BIT_AND,          // &
        BIT_OR,           // |
        BIT_XOR,          // ^
        BIT_NOT,          // ~
        SHL,              // <<
        SHR,              // >>   (signed/arithmetic right shift)
        USHR,             // >>>  (unsigned right shift)

        // Operators - assignment
        ASSIGN,           // =
        PLUS_EQ,          // +=  (compound assignment; desugared for a variable target)
        MINUS_EQ,         // -=
        STAR_EQ,          // *=
        SLASH_EQ,         // /=
        PERCENT_EQ,       // %=
        BIT_AND_EQ,       // &=
        BIT_OR_EQ,        // |=
        BIT_XOR_EQ,       // ^=
        SHL_EQ,           // <<=
        SHR_EQ,           // >>=
        USHR_EQ,          // >>>=
        FAT_ARROW,        // =>  (lambda expression bodies only, e.g. func(x) => x * x)

        // Operators - comparison
        EQ,               // ==
        NEQ,              // !=
        LT,               // <
        GT,               // >
        LTE,              // <=
        GTE,              // >=

        // Operators - logical
        AND,              // &&
        OR,               // ||
        NOT,              // !

        // Special
        END_OF_FILE,
        UNKNOWN
    };

    struct Token {
        TokenType type{TokenType::UNKNOWN};
        std::string lexeme;   // raw text, e.g. "age", "21", "step"
        std::size_t line{1};
        std::size_t column{1};
    };
}
