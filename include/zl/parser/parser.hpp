#pragma once

#include <initializer_list>
#include <stdexcept>
#include <vector>

#include "ast.hpp"
#include "zl/lexer/token.hpp"
#include "zl/parser/type_lookahead.hpp"

namespace zl {
class ExpressionParser;
}


namespace zl {

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& message) : std::runtime_error(message) {}
};

class Parser {
    friend class ExpressionParser;
public:
    explicit Parser(std::vector<Token> tokens);

    // Entry point: consumes all tokens, returns the root Program node
    // (a list of top-level ClassDecl/DataDecl declarations).
    [[nodiscard]] std::unique_ptr<Program> parse();

private:
    // --- token stream cursor helpers ---
    [[nodiscard]] const Token& peek() const;
    [[nodiscard]] const Token& peekNext() const;
    [[nodiscard]] const Token& previous() const;
    [[nodiscard]] bool isAtEnd() const;
    [[nodiscard]] bool check(TokenType type) const;
    bool match(std::initializer_list<TokenType> types);
    const Token& advance();
    const Token& expect(TokenType type, const std::string& errorMessage);
    // A name position: an IDENTIFIER, or one of the spellings that are both a
    // type keyword and a legal name. `list`, `set` and `map` are type
    // annotations (`list<int> xs`), but nothing stops a program from calling a
    // variable `list`; the lexer cannot tell the two readings apart, so the
    // parser accepts them as a name wherever no type can appear and resolves
    // the statement-level ambiguity in looksLikeTypedDeclStart.
    [[nodiscard]] bool checkName() const;
    const Token& expectName(const std::string& errorMessage);
    // Consume the '>' that closes a type argument / parameter list.
    //
    // The lexer runs without context, so the `>>` at the end of
    // `List<List<int>>` arrives as a single SHR token and the parse fails.
    // This splits a fused shift token back into the '>' characters it was
    // made of, consuming exactly one and leaving the rest in the stream for
    // the enclosing list to consume - the same trick C++ and Java parsers
    // use. Shift expressions are unaffected: they are never parsed here.
    const Token& expectTypeAngleClose(const std::string& errorMessage);
    [[noreturn]] void error(const std::string& message) const;

    // --- top-level declarations ---
    NodePtr parseImportDecl();    // import a.b.C - must appear before any class/data decl
    NodePtr parseDeclaration();   // dispatches to class/interface/data/enum/memory declarations
    NodePtr parseClassDecl();     // class Name (extends P)? (implements I, ...)? { member member ... }
    NodePtr parseClassDecl(std::vector<Annotation> annotations); // same, with leading @annotations already parsed
    NodePtr parseInterfaceDecl(); // interface Name (extends I, ...)? { methodSig methodSig ... }
    NodePtr parseDataDecl();      // data Name { field field ... }
    NodePtr parseEnumDecl();      // enum Name { MEMBER MEMBER ... }
    NodePtr parseMemoryDecl();    // memory Name { contract methods, helpers, fields }
    AccessModifier parseAccessModifier();
    NodePtr parseClassMember();   // a FunctionDecl or a field VarDecl, inside a class body
    NodePtr parseFunctionDecl(AccessModifier access = AccessModifier::DEFAULT,
                               std::vector<Annotation> annotations = {});
    NodePtr parseOperatorDecl(AccessModifier access = AccessModifier::DEFAULT,
                               std::vector<Annotation> annotations = {});
    Param parseParam();           // data field: name: Type
    Param parseFunctionParam();   // function/lambda/operator parameter: Type Name (or bare name)
    InterfaceMethodSig parseInterfaceMethodSig(); // name(params): ReturnType   (no body, no ';' needed)

    // @Name or @Name("arg"), zero or more in a row. Used before a class
    // declaration and before a class member (func or field).
    std::vector<Annotation> parseAnnotations();

    // --- types ---
    // `array`/`list`/`set`/`map` start unambiguously on their own keyword;
    // anything else falls back to treating the identifier itself as a type name.
    [[nodiscard]] bool startsTypeAnnotation() const;
    TypeAnnotation parseTypeAnnotation();

    // Disambiguates `int x = 5` (typed declaration) from `foo(x)` (expression
    // statement): true only for "IDENTIFIER IDENTIFIER" or a collection
    // keyword, since a bare identifier followed by another identifier is never
    // valid as the start of an expression otherwise. Also recognizes union
    // type prefixes like `int|string x` by scanning past the full union
    // chain before checking for the trailing variable-name identifier.
    [[nodiscard]] bool looksLikeTypedDeclStart() const;

    // --- statements ---
    NodePtr parseStatement();
    NodePtr parseVarDecl(AccessModifier access = AccessModifier::DEFAULT);
    NodePtr parseTypedVarDecl(AccessModifier access = AccessModifier::DEFAULT);
    NodePtr parseLogStmt();       // log(...)
    NodePtr parseIfStmt();        // if / elif / else
    NodePtr parseReturnStmt();    // return expr   OR   return
    NodePtr parseForStmt();       // for i in a..b step s { }
    NodePtr parseWhileStmt();     // while cond { }
    NodePtr parseRepeatStmt();    // repeat { } while cond
    NodePtr parseBreakStmt();
    NodePtr parseContinueStmt();
    NodePtr parseTryStmt();       // try { } catch e { }
    NodePtr parseThrowStmt();     // throw expr
    std::unique_ptr<BlockStmt> parseBlock(); // { ... }
    NodePtr parseExprStmt();      // fallback: bare expression

    // --- expressions ---
    NodePtr parseExpression();
    // Same as parseExpression(), but with data-literal parsing disabled -
    // see noDataLiteral_'s comment below. Used for if/while/repeat-while
    // conditions and for-loop start/end/step, i.e. every expression
    // position immediately followed by that construct's own '{' block.
    NodePtr parseConditionExpression();
    // TypeName { field: expr, ... } - called from parsePrimary once it's
    // already consumed an uppercase-leading identifier and sees '{' next.
    // `nameTok` is that already-consumed identifier token.
    // func(params) => expr   OR   func(params) { block }  - a lambda VALUE,
    // unambiguous wherever an expression is expected (KW_FUNC never
    // starts an expression any other way - named func declarations are
    // only ever parsed at the statement/class-member level, via
    // parseFunctionDecl, not through parsePrimary).

    // A bare `Identifier {` is ambiguous with "condition/range-bound
    // expression immediately followed by the statement's own block" (`if
    // cond { ... }`, `while cond { ... }`, `for i in a..b { ... }`, `repeat
    // { } while cond` - none of these use parens around the
    // condition/bound, unlike C-family languages). noDataLiteral_ is set
    // true (see parseIfStmt/parseWhileStmt/parseRepeatStmt/parseForStmt)
    // for exactly the duration of parsing such a condition/bound
    // expression, so parsePrimary falls back to treating a bare
    // uppercase identifier as a plain Identifier there instead of trying
    // to consume a following '{' as a data literal - the same restriction
    // Go and Rust apply to struct literals in an `if`/`for` condition, and
    // for the same reason.
    bool noDataLiteral_{false};

    // Recursion guards. The parser is recursive descent, so pathological
    // input (thousands of nested parentheses, or thousands of nested '{'
    // blocks) is a stack overflow (SIGSEGV) rather than a syntax error
    // without a counted limit. Both counters are members because expression
    // nesting recurses through one ExpressionParser instance while block
    // nesting recurses through Parser::parseBlock -> parseStatement.
    int expressionDepth_{0};
    int blockDepth_{0};
    int typeDepth_{0};
    static constexpr int kMaxExpressionDepth = 1000;
    static constexpr int kMaxBlockDepth = 500;
    static constexpr int kMaxTypeDepth = 500;

    std::vector<Token> tokens_;
    std::size_t pos_{0};
    TypeLookahead typeLookahead_;
};

} // namespace zl
