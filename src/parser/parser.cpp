#include "zl/parser/parser.hpp"
#include "zl/parser/annotation_rules.hpp"
#include "zl/parser/expression_parser.hpp"

#include <cctype>
#include <limits>
#include <string>

namespace zl {

namespace {
// Token types that may appear in a name position. `list`, `set` and `map` are
// type spellings first, but the lexer runs without context, so a variable may
// carry the same spelling and the parser decides by position (see
// Parser::checkName and Parser::looksLikeTypedDeclStart).
bool isNameToken(TokenType type) {
    return type == TokenType::IDENTIFIER || type == TokenType::KW_SHARED ||
           type == TokenType::KW_LIST || type == TokenType::KW_SET || type == TokenType::KW_MAP;
}
} // namespace

Parser::Parser(std::vector<Token> tokens)
    : tokens_(std::move(tokens)), typeLookahead_(tokens_) {}

// ---------- cursor helpers ----------

const Token& Parser::peek() const { return tokens_[pos_]; }

const Token& Parser::peekNext() const {
    if (pos_ + 1 >= tokens_.size()) return tokens_.back();
    return tokens_[pos_ + 1];
}

const Token& Parser::previous() const { return tokens_[pos_ - 1]; }

bool Parser::isAtEnd() const { return peek().type == TokenType::END_OF_FILE; }

bool Parser::check(TokenType type) const { return !isAtEnd() && peek().type == type; }

bool Parser::match(std::initializer_list<TokenType> types) {
    for (TokenType t : types) {
        if (check(t)) { advance(); return true; }
    }
    return false;
}

const Token& Parser::advance() {
    if (!isAtEnd()) pos_++;
    return previous();
}

const Token& Parser::expect(TokenType type, const std::string& errorMessage) {
    if (check(type)) return advance();
    error(errorMessage);
}

bool Parser::checkName() const {
    return !isAtEnd() && isNameToken(peek().type);
}

const Token& Parser::expectName(const std::string& errorMessage) {
    if (checkName()) return advance();
    error(errorMessage);
}

const Token& Parser::expectTypeAngleClose(const std::string& errorMessage) {
    if (check(TokenType::GT)) return advance();

    // `List<List<int>>` lexes as LT int SHR. Split the fused token: overwrite
    // it with a single '>' that this call consumes, and insert the remainder
    // just after it so the enclosing list can close on the next pass. `>>>`
    // leaves a `>>` behind for the same reason.
    if (check(TokenType::SHR) || check(TokenType::USHR)) {
        const bool wasTriple = peek().type == TokenType::USHR;
        const std::size_t line = peek().line;
        const std::size_t column = peek().column;
        tokens_[pos_] = Token{TokenType::GT, ">", line, column};
        tokens_.insert(tokens_.begin() + static_cast<std::ptrdiff_t>(pos_) + 1,
                       Token{wasTriple ? TokenType::SHR : TokenType::GT,
                             wasTriple ? ">>" : ">", line, column + 1});
        return advance();
    }

    error(errorMessage);
}

void Parser::error(const std::string& message) const {
    const Token& tok = peek();
    throw ParseError(message + " -- got \"" + tok.lexeme + "\" at line " + std::to_string(tok.line));
}

// ---------- entry point ----------

std::unique_ptr<Program> Parser::parse() {
    auto program = std::make_unique<Program>();

    // Imports are Java-like: they must all come before any class/data
    // declaration. Once a declaration has started, `import` is just a
    // regular (undeclared) identifier again - not specially handled - so
    // there's no ambiguity in only accepting it right here, up front.
    while (check(TokenType::KW_IMPORT)) {
        program->imports.push_back(parseImportDecl());
    }

    while (!isAtEnd()) {
        NodePtr decl = parseDeclaration();
        program->declarations.push_back(std::move(decl));
    }
    return program;
}

NodePtr Parser::parseImportDecl() {
    Token importTok = advance(); // consume 'import'

    auto node = std::make_unique<ImportDecl>();
    node->line = importTok.line;

    auto expectImportSegment = [this]() -> Token {
        if (check(TokenType::IDENTIFIER) || check(TokenType::KW_SHARED)) return advance();
        error("Expected a package/class name after 'import'");
    };

    Token first = expectImportSegment();
    node->pathSegments.push_back(first.lexeme);
    node->dottedName = first.lexeme;

    while (match({TokenType::DOT})) {
        Token seg;
        if (check(TokenType::IDENTIFIER) || check(TokenType::KW_SHARED)) seg = advance();
        else error("Expected an identifier after '.' in import path");
        node->pathSegments.push_back(seg.lexeme);
        node->dottedName += "." + seg.lexeme;
    }

    return node;
}

NodePtr Parser::parseDeclaration() {
    // Class-level annotations come before the 'class' keyword itself, e.g.
    // `@Deprecated class Old { }`. The names are checked against the registry
    // (zl/parser/annotation_rules.hpp) inside parseAnnotations - an unknown
    // annotation is an error, not a silent no-op. Interfaces/data types don't
    // support annotations - not part of the spec, so any '@' before them
    // falls through to the normal error below.
    if (check(TokenType::AT)) {
        std::vector<Annotation> annotations = parseAnnotations();
        if (!check(TokenType::KW_CLASS)) {
            error("Expected 'class' after annotation(s) at file scope");
        }
        return parseClassDecl(std::move(annotations));
    }
    if (check(TokenType::KW_CLASS)) return parseClassDecl();
    if (check(TokenType::KW_INTERFACE)) return parseInterfaceDecl();
    if (check(TokenType::KW_DATA)) return parseDataDecl();
    if (check(TokenType::KW_ENUM)) return parseEnumDecl();
    // `memory` is a contextual keyword (docs/memory-domains.md §5.1): it
    // opens a declaration only here, at file scope, when followed by a name -
    // so a program where `memory` is a plain identifier still parses. The
    // identifier path below is unchanged for every other spelling.
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "memory" &&
        peekNext().type == TokenType::IDENTIFIER) {
        return parseMemoryDecl();
    }
    error("Expected a 'class', 'interface', 'data', 'enum', or 'memory' declaration at file scope");
}

// ---------- annotations ----------

std::vector<Annotation> Parser::parseAnnotations() {
    std::vector<Annotation> annotations;
    while (check(TokenType::AT)) {
        Token atTok = advance(); // consume '@'
        Token nameTok = expect(TokenType::IDENTIFIER, "Expected an annotation name after '@'");

        // Unknown names are an error, not a silent no-op: an annotation the
        // compiler does not know cannot be validated, target-checked or
        // consumed, so accepting it would bury the typo (`@memroy(Pool)`)
        // until the program behaves inexplicably. See
        // include/zl/parser/annotation_rules.hpp for the table.
        if (!annotations::isKnown(nameTok.lexeme)) {
            throw ParseError("unknown annotation '@" + nameTok.lexeme + "' at line " +
                             std::to_string(nameTok.line) + " - known annotations are: " +
                             annotations::knownNames());
        }

        Annotation ann;
        ann.name = nameTok.lexeme;
        ann.line = atTok.line;

        // General optional annotation argument list. Arguments are preserved
        // as raw lexemes; semantic validation belongs to the annotation user,
        // not the core parser.
        if (match({TokenType::LPAREN})) {
            if (!check(TokenType::RPAREN)) {
                while (true) {
                    const Token& argTok = peek();
                    switch (argTok.type) {
                    case TokenType::STRING_LITERAL:
                    case TokenType::INT_LITERAL:
                    case TokenType::DECIMAL_LITERAL:
                    case TokenType::FLOAT_LITERAL:
                    case TokenType::BOOL_LITERAL:
                    case TokenType::IDENTIFIER:
                    case TokenType::KW_TRUE:
                    case TokenType::KW_FALSE:
                    case TokenType::KW_NULL:
                        ann.arguments.push_back(argTok.lexeme);
                        advance();
                        break;
                    default:
                        error("Expected a literal or identifier annotation argument to @" + ann.name);
                    }
                    if (!match({TokenType::COMMA})) break;
                }
            }
            expect(TokenType::RPAREN, "Expected ')' after @" + ann.name + " arguments");
            if (ann.arguments.size() == 1) ann.argument = ann.arguments.front();
        }

        annotations.push_back(std::move(ann));
    }
    return annotations;
}

// ---------- class / interface / data / func declarations ----------

NodePtr Parser::parseClassDecl() { return parseClassDecl({}); }

NodePtr Parser::parseClassDecl(std::vector<Annotation> annotations) {
    Token classTok = advance(); // consume 'class'
    Token nameTok = expect(TokenType::IDENTIFIER, "Expected a class name after 'class'");

    auto node = std::make_unique<ClassDecl>();
    node->line = classTok.line;
    node->name = nameTok.lexeme;
    node->annotations = std::move(annotations);

    // class Box<T> { ... } / class Pair<A, B> { ... } - unambiguous right
    // after the class name (a class declaration is always followed by '<',
    // 'extends', 'implements', or '{', never a comparison expression).
    if (match({TokenType::LT})) {
        Token first = expect(TokenType::IDENTIFIER, "Expected a type parameter name");
        node->typeParams.push_back(first.lexeme);
        if (match({TokenType::COLON})) {
            Token iface = expect(TokenType::IDENTIFIER, "Expected an interface name after type parameter ':'");
            node->typeParamInterfaceConstraints[first.lexeme].push_back(iface.lexeme);
            while (match({TokenType::BIT_AND})) {
                Token more = expect(TokenType::IDENTIFIER, "Expected an interface name after '&'");
                node->typeParamInterfaceConstraints[first.lexeme].push_back(more.lexeme);
            }
        }
        while (match({TokenType::COMMA})) {
            Token next = expect(TokenType::IDENTIFIER, "Expected a type parameter name after ','");
            node->typeParams.push_back(next.lexeme);
            if (match({TokenType::COLON})) {
                Token iface = expect(TokenType::IDENTIFIER, "Expected an interface name after type parameter ':'");
                node->typeParamInterfaceConstraints[next.lexeme].push_back(iface.lexeme);
                while (match({TokenType::BIT_AND})) {
                    Token more = expect(TokenType::IDENTIFIER, "Expected an interface name after '&'");
                    node->typeParamInterfaceConstraints[next.lexeme].push_back(more.lexeme);
                }
            }
        }
        expectTypeAngleClose("Expected '>' to close type parameter list");
    }

    if (match({TokenType::KW_EXTENDS})) {
        Token parentTok = expect(TokenType::IDENTIFIER, "Expected a parent class name after 'extends'");
        if (parentTok.lexeme == node->name) {
            error("class '" + node->name + "' cannot extend itself");
        }
        node->extendsName = parentTok.lexeme;
        // extends Container<T> - the parent's own type arguments, kept (not
        // discarded) so TypeChecker::instantiateGenericClass can thread
        // substitution through to the parent too, e.g. SpecialContainer<int>
        // extending Container<T> needs to resolve to Container<int> as ITS
        // parent, not the bare unsubstituted "Container" template.
        if (match({TokenType::LT})) {
            node->extendsTypeArgs.push_back(parseTypeAnnotation());
            while (match({TokenType::COMMA})) {
                node->extendsTypeArgs.push_back(parseTypeAnnotation());
            }
            expectTypeAngleClose("Expected '>' to close type argument list");
        }
    }

    if (match({TokenType::KW_IMPLEMENTS})) {
        Token first = expect(TokenType::IDENTIFIER, "Expected an interface name after 'implements'");
        node->implementsNames.push_back(first.lexeme);
        while (match({TokenType::COMMA})) {
            Token next = expect(TokenType::IDENTIFIER, "Expected an interface name after ',' in 'implements' list");
            node->implementsNames.push_back(next.lexeme);
        }
    }

    expect(TokenType::LBRACE, "Expected '{' after class name");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        node->members.push_back(parseClassMember());
    }

    // Now that we know the class's own name, stamp every func member with
    // it (ownerClassName) and flag the constructor (name == class name) -
    // these were declared on FunctionDecl but never actually set anywhere.
    bool hasConstructor = false;
    for (auto& member : node->members) {
        if (member->kind != NodeKind::FunctionDecl) continue;
        auto* fn = static_cast<FunctionDecl*>(member.get());
        fn->ownerClassName = node->name;
        fn->isConstructor = (fn->name == node->name);
        if (fn->isConstructor) hasConstructor = true;
    }

    // `new Foo()` / `Foo()` must always resolve to *some* constructor, even
    // for a class that never declares one explicitly - the compiler and VM
    // both require every class to have a registered constructor func
    // (see Compiler::compile / VM's InvokeMethod). Synthesizing a no-arg,
    // empty-body constructor here - exactly what a user would get from
    // writing `func Foo() { }` by hand - means every later pass (type
    // checker, compiler) can treat "does this class have a constructor?" as
    // always true, with no special-casing. Mirrors Java/C#'s implicit
    // default constructor.
    if (!hasConstructor) {
        auto ctor = std::make_unique<FunctionDecl>();
        ctor->line = node->line;
        ctor->name = node->name;
        ctor->returnType.name = "void";
        ctor->body = std::make_unique<BlockStmt>();
        ctor->isConstructor = true;
        ctor->ownerClassName = node->name;
        ctor->access = AccessModifier::DEFAULT;
        node->members.push_back(std::move(ctor));
    }

    expect(TokenType::RBRACE, "Expected '}' to close class '" + nameTok.lexeme + "'");
    return node;
}

// interface Name (extends Base1, Base2, ...)? { methodSig methodSig ... }
NodePtr Parser::parseInterfaceDecl() {
    Token ifaceTok = advance(); // consume 'interface'
    Token nameTok = expect(TokenType::IDENTIFIER, "Expected an interface name after 'interface'");

    auto node = std::make_unique<InterfaceDecl>();
    node->line = ifaceTok.line;
    node->name = nameTok.lexeme;

    if (match({TokenType::KW_EXTENDS})) {
        Token first = expect(TokenType::IDENTIFIER, "Expected an interface name after 'extends'");
        if (first.lexeme == node->name) {
            error("interface '" + node->name + "' cannot extend itself");
        }
        node->extendsNames.push_back(first.lexeme);
        while (match({TokenType::COMMA})) {
            Token next = expect(TokenType::IDENTIFIER, "Expected an interface name after ',' in 'extends' list");
            if (next.lexeme == node->name) {
                error("interface '" + node->name + "' cannot extend itself");
            }
            node->extendsNames.push_back(next.lexeme);
        }
    }

    expect(TokenType::LBRACE, "Expected '{' after interface name");
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        node->methods.push_back(parseInterfaceMethodSig());
    }
    expect(TokenType::RBRACE, "Expected '}' to close interface '" + nameTok.lexeme + "'");
    return node;
}

// name(params): ReturnType   - a bare signature, no body, no access modifier
// (interface methods are implicitly public - that's the whole point of an
// interface), no trailing separator required between signatures.
InterfaceMethodSig Parser::parseInterfaceMethodSig() {
    InterfaceMethodSig sig;
    std::string operatorLexeme;

    if (check(TokenType::KW_OPERATOR)) {
        Token opTok = advance();
        Token symbolTok = advance();
        auto isSupported = [](TokenType t) {
            switch (t) {
                case TokenType::PLUS: case TokenType::MINUS: case TokenType::STAR:
                case TokenType::SLASH: case TokenType::PERCENT: case TokenType::POW:
                case TokenType::BIT_AND: case TokenType::BIT_OR: case TokenType::BIT_XOR:
                case TokenType::SHL: case TokenType::SHR: case TokenType::USHR:
                case TokenType::EQ: case TokenType::NEQ: case TokenType::LT: case TokenType::GT:
                case TokenType::LTE: case TokenType::GTE: case TokenType::NOT: case TokenType::BIT_NOT:
                    return true;
                default: return false;
            }
        };
        if (!isSupported(symbolTok.type)) {
            error("Expected a supported operator after 'operator'");
        }
        sig.name = "operator" + symbolTok.lexeme;
        operatorLexeme = symbolTok.lexeme;
        sig.isOperator = true;
        sig.operatorToken = symbolTok.type;
        sig.line = opTok.line;
    } else {
        Token nameTok = expect(TokenType::IDENTIFIER, "Expected a method name inside interface body");
        sig.name = nameTok.lexeme;
        sig.line = nameTok.line;
    }

    // interface method type parameters: `firstOf<T>(...)`. No constraints in v1.
    if (!sig.isOperator && match({TokenType::LT})) {
        Token first = expect(TokenType::IDENTIFIER, "Expected a type parameter name");
        sig.typeParams.push_back(first.lexeme);
        while (match({TokenType::COMMA})) {
            Token next = expect(TokenType::IDENTIFIER, "Expected a type parameter name after ','");
            sig.typeParams.push_back(next.lexeme);
        }
        expectTypeAngleClose("Expected '>' to close type parameter list");
    }

    expect(TokenType::LPAREN, "Expected '(' after interface method/operator name");
    if (!check(TokenType::RPAREN)) {
        sig.params.push_back(parseFunctionParam());
        while (match({TokenType::COMMA})) sig.params.push_back(parseFunctionParam());
    }
    expect(TokenType::RPAREN, "Expected ')' after parameters");

    if (match({TokenType::COLON})) sig.returnType = parseTypeAnnotation();
    else sig.returnType.name = "void";

    if (sig.isOperator) {
        if (sig.returnType.name == "void") {
            error("operator '" + operatorLexeme + "' must declare a non-void return type");
        }
        const bool unaryOnly = sig.operatorToken == TokenType::NOT || sig.operatorToken == TokenType::BIT_NOT;
        const bool unaryOrBinary = sig.operatorToken == TokenType::MINUS || sig.operatorToken == TokenType::PLUS;
        const std::size_t count = sig.params.size();
        if (unaryOnly && count != 0) {
            error("operator '" + operatorLexeme + "' expects 0 explicit parameter(s)");
        }
        if (!unaryOnly && !unaryOrBinary && count != 1) {
            error("operator '" + operatorLexeme + "' expects 1 explicit parameter(s)");
        }
        if (unaryOrBinary && count > 1) {
            error("operator '" + operatorLexeme + "' expects 0 or 1 explicit parameter(s)");
        }
    }
    return sig;
}

NodePtr Parser::parseDataDecl() {
    Token dataTok = advance(); // consume 'data'
    Token nameTok = expect(TokenType::IDENTIFIER, "Expected a data type name after 'data'");
    std::string extendsName;
    std::vector<TypeAnnotation> extendsTypeArgs;
    if (match({TokenType::KW_EXTENDS})) {
        extendsName = expect(TokenType::IDENTIFIER, "Expected parent data type name after 'extends'").lexeme;
        if (match({TokenType::LT})) {
            do { extendsTypeArgs.push_back(parseTypeAnnotation()); } while (match({TokenType::COMMA}));
            expectTypeAngleClose("Expected ' > ' after data parent type arguments");
        }
    }
    expect(TokenType::LBRACE, "Expected '{' after data type name");

    auto node = std::make_unique<DataDecl>();
    node->line = dataTok.line;
    node->name = nameTok.lexeme;
    node->extendsName = std::move(extendsName);
    node->extendsTypeArgs = std::move(extendsTypeArgs);

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        if (check(TokenType::KW_ASYNC)) {
            advance();
            if (!check(TokenType::KW_FUNC)) error("Expected 'func' after 'async' in data method");
            auto fn = parseFunctionDecl(AccessModifier::PUBLIC, {});
            auto* f = static_cast<FunctionDecl*>(fn.get());
            f->ownerClassName = node->name;
            f->isAsync = true;
            node->members.push_back(std::move(fn));
            continue;
        }
        if (check(TokenType::KW_FUNC)) {
            auto fn = parseFunctionDecl(AccessModifier::PUBLIC, {});
            auto* f = static_cast<FunctionDecl*>(fn.get());
            f->ownerClassName = node->name;
            node->members.push_back(std::move(fn));
            continue;
        }
        if (check(TokenType::KW_STATIC) || check(TokenType::KW_PUBLIC) ||
            check(TokenType::KW_PRIVATE) || check(TokenType::KW_PROTECTED) ||
            check(TokenType::KW_OPERATOR)) {
            error("data types allow only fields and instance func methods; access modifiers, static members, and operators are not permitted");
        }
        node->fields.push_back(parseParam());
    }

    expect(TokenType::RBRACE, "Expected '}' to close data type '" + nameTok.lexeme + "'");
    return node;
}

// enum Name { MEMBER MEMBER ... } - members are newline-separated (no
// commas), same convention as a data type's field list (parseDataDecl,
// just above) rather than a collection literal's comma-separated one.
NodePtr Parser::parseEnumDecl() {
    Token enumTok = advance(); // consume 'enum'
    Token nameTok = expect(TokenType::IDENTIFIER, "Expected an enum name after 'enum'");
    expect(TokenType::LBRACE, "Expected '{' after enum name");

    auto node = std::make_unique<EnumDecl>();
    node->line = enumTok.line;
    node->name = nameTok.lexeme;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        Token memberTok = expect(TokenType::IDENTIFIER, "Expected a member name in enum '" + nameTok.lexeme + "'");
        node->members.push_back(memberTok.lexeme);
    }

    expect(TokenType::RBRACE, "Expected '}' to close enum '" + nameTok.lexeme + "'");
    return node;
}

// memory Name { ... } - a user-written memory-domain declaration
// (docs/memory-domains.md §5.1). The contract itself (required public
// acquire/release, their signatures) is validated by
// TypeChecker::registerMemoryDeclaration; the parser only enforces the
// declaration *shape*: members are fields and instance funcs - the forms a
// class body takes, minus the ones a domain has no use for. Each rejection
// names the form, per §5.1's "a specific error, not a general 'not allowed
// here'".
NodePtr Parser::parseMemoryDecl() {
    Token memoryTok = advance(); // consume the contextual 'memory'
    Token nameTok = expect(TokenType::IDENTIFIER, "Expected a memory declaration name after 'memory'");
    expect(TokenType::LBRACE, "Expected '{' after memory declaration name");

    auto node = std::make_unique<MemoryDecl>();
    node->line = memoryTok.line;
    node->name = nameTok.lexeme;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        if (check(TokenType::AT)) {
            error("Annotations are not allowed on memory declaration members");
        }
        if (check(TokenType::KW_STATIC)) {
            error("'static' is not allowed in memory declaration '" + nameTok.lexeme +
                  "' - a domain's state and contract belong to the domain object");
        }
        if (check(TokenType::KW_ASYNC)) {
            error("'async' is not allowed in memory declaration '" + nameTok.lexeme +
                  "' - contract methods are called by the compiler, never awaited");
        }
        if (check(TokenType::KW_OPERATOR)) {
            error("Operators are not allowed in memory declaration '" + nameTok.lexeme + "'");
        }
        if (check(TokenType::KW_CLASS) || check(TokenType::KW_INTERFACE) ||
            check(TokenType::KW_DATA) || check(TokenType::KW_ENUM)) {
            error("Nested declarations are not allowed inside memory declaration '" + nameTok.lexeme + "'");
        }
        AccessModifier access = parseAccessModifier();
        if (check(TokenType::KW_STATIC)) {
            error("'static' is not allowed in memory declaration '" + nameTok.lexeme +
                  "' - a domain's state and contract belong to the domain object");
        }
        if (check(TokenType::KW_ASYNC)) {
            error("'async' is not allowed in memory declaration '" + nameTok.lexeme +
                  "' - contract methods are called by the compiler, never awaited");
        }
        if (check(TokenType::KW_FUNC)) {
            // `func(...): Return name` is a function-typed field, not a named
            // function - same disambiguation as parseClassMember.
            if (peekNext().type == TokenType::LPAREN) {
                node->members.push_back(parseTypedVarDecl(access));
                continue;
            }
            node->members.push_back(parseFunctionDecl(access, {}));
            continue;
        }
        if (check(TokenType::KW_VAR) || check(TokenType::KW_LET)) {
            node->members.push_back(parseVarDecl(access));
            continue;
        }
        if (looksLikeTypedDeclStart()) {
            node->members.push_back(parseTypedVarDecl(access));
            continue;
        }
        error("Expected a func or a field inside memory declaration '" + nameTok.lexeme + "'");
    }

    expect(TokenType::RBRACE, "Expected '}' to close memory declaration '" + nameTok.lexeme + "'");

    // Stamp ownership, and flag a constructor-shaped func by its name the way
    // parseClassDecl does - registerMemoryDeclaration rejects it explicitly
    // (a domain is never instantiated by user code), so the flag must exist.
    for (auto& member : node->members) {
        if (member->kind != NodeKind::FunctionDecl) continue;
        auto* fn = static_cast<FunctionDecl*>(member.get());
        fn->ownerClassName = node->name;
        fn->isConstructor = (fn->name == node->name);
    }

    return node;
}

AccessModifier Parser::parseAccessModifier() {
    if (match({TokenType::KW_PUBLIC})) return AccessModifier::PUBLIC;
    if (match({TokenType::KW_PRIVATE})) return AccessModifier::PRIVATE;
    if (match({TokenType::KW_PROTECTED})) return AccessModifier::PROTECTED;
    return AccessModifier::DEFAULT;
}

NodePtr Parser::parseClassMember() {
    std::vector<Annotation> annotations = parseAnnotations();
    bool isStatic = false;
    if (match({TokenType::KW_STATIC})) isStatic = true;
    AccessModifier access = parseAccessModifier();
    if (match({TokenType::KW_STATIC})) isStatic = true;
    if (check(TokenType::KW_ASYNC)) {
        advance();
        if (!check(TokenType::KW_FUNC)) error("Expected 'func' after 'async'");
        auto fn = parseFunctionDecl(access, std::move(annotations));
        static_cast<FunctionDecl*>(fn.get())->isStatic = isStatic;
        static_cast<FunctionDecl*>(fn.get())->isAsync = true;
        return fn;
    }
    if (check(TokenType::KW_FUNC)) {
        // `func(...) : Return name` is a function-typed field. A named
        // function starts `func Name(`, so the immediate `(` cleanly
        // disambiguates the two forms.
        if (peekNext().type == TokenType::LPAREN) {
            return parseTypedVarDecl(access);
        }
        auto fn = parseFunctionDecl(access, std::move(annotations));
        static_cast<FunctionDecl*>(fn.get())->isStatic = isStatic;
        return fn;
    }
    if (check(TokenType::KW_OPERATOR)) {
        if (isStatic) error("'static' is not allowed on operators");
        return parseOperatorDecl(access, std::move(annotations));
    }
    if (!annotations.empty()) {
        error("Annotations are only allowed on functions/methods, not fields");
    }
    if (check(TokenType::KW_VAR) || check(TokenType::KW_LET)) {
        auto field = parseVarDecl(access);
        static_cast<VarDecl*>(field.get())->isStatic = isStatic;
        return field;
    }
    if (looksLikeTypedDeclStart()) {
        auto field = parseTypedVarDecl(access);
        static_cast<VarDecl*>(field.get())->isStatic = isStatic;
        return field;
    }
    error("Expected a func or a typed field inside class body");
}

NodePtr Parser::parseOperatorDecl(AccessModifier access, std::vector<Annotation> annotations) {
    Token opTok = advance(); // consume 'operator'
    Token symbolTok = advance();

    auto isSupported = [](TokenType t) {
        switch (t) {
            case TokenType::PLUS: case TokenType::MINUS: case TokenType::STAR:
            case TokenType::SLASH: case TokenType::PERCENT: case TokenType::POW:
            case TokenType::BIT_AND: case TokenType::BIT_OR: case TokenType::BIT_XOR:
            case TokenType::SHL: case TokenType::SHR: case TokenType::USHR:
            case TokenType::EQ: case TokenType::NEQ: case TokenType::LT: case TokenType::GT:
            case TokenType::LTE: case TokenType::GTE:
            case TokenType::NOT: case TokenType::BIT_NOT:
                return true;
            default: return false;
        }
    };
    if (!isSupported(symbolTok.type)) {
        error("Expected a supported operator after 'operator'");
    }

    auto node = std::make_unique<FunctionDecl>();
    node->line = opTok.line;
    node->name = "operator" + symbolTok.lexeme;
    node->isOperator = true;
    node->operatorToken = symbolTok.type;
    // Operators are part of a type's public surface by default. They may
    // still be explicitly marked private/protected when intentionally
    // restricted to the declaring class hierarchy.
    node->access = (access == AccessModifier::DEFAULT) ? AccessModifier::PUBLIC : access;
    node->annotations = std::move(annotations);

    expect(TokenType::LPAREN, "Expected '(' after operator symbol");
    if (!check(TokenType::RPAREN)) {
        node->params.push_back(parseFunctionParam());
        while (match({TokenType::COMMA})) node->params.push_back(parseFunctionParam());
    }
    expect(TokenType::RPAREN, "Expected ')' after operator parameters");

    if (match({TokenType::COLON})) node->returnType = parseTypeAnnotation();
    else node->returnType.name = "void";
    if (node->returnType.name == "void") {
        error("operator '" + symbolTok.lexeme + "' must declare a non-void return type");
    }

    // Binary operators take one explicit parameter. `+` and `-` may also
    // be declared as unary operators with zero parameters; the call site
    // determines which overload is selected.
    const bool unaryOnly = symbolTok.type == TokenType::NOT || symbolTok.type == TokenType::BIT_NOT;
    const bool unaryOrBinary = symbolTok.type == TokenType::MINUS || symbolTok.type == TokenType::PLUS;
    const std::size_t count = node->params.size();
    if (unaryOnly && count != 0) {
        error("operator '" + symbolTok.lexeme + "' expects 0 explicit parameter(s)");
    }
    if (!unaryOnly && !unaryOrBinary && count != 1) {
        error("operator '" + symbolTok.lexeme + "' expects 1 explicit parameter(s)");
    }
    if (unaryOrBinary && count > 1) {
        error("operator '" + symbolTok.lexeme + "' expects 0 or 1 explicit parameter(s)");
    }

    node->body = parseBlock();
    return node;
}

NodePtr Parser::parseFunctionDecl(AccessModifier access, std::vector<Annotation> annotations) {
    Token fnTok = advance(); // consume 'func' or 'func' - both map to KW_FUNC
    Token nameTok = expect(TokenType::IDENTIFIER, "Expected a func name");

    auto node = std::make_unique<FunctionDecl>();
    node->line = fnTok.line;
    node->name = nameTok.lexeme;
    node->access = access;
    node->annotations = std::move(annotations);

    // Method type parameters: `func firstOf<T>(...)`. Unambiguous here — a
    // named declaration is followed by '<' or '(', never a comparison.
    if (match({TokenType::LT})) {
        Token first = expect(TokenType::IDENTIFIER, "Expected a type parameter name");
        node->typeParams.push_back(first.lexeme);
        while (match({TokenType::COMMA})) {
            Token next = expect(TokenType::IDENTIFIER, "Expected a type parameter name after ','");
            node->typeParams.push_back(next.lexeme);
        }
        expectTypeAngleClose("Expected '>' to close type parameter list");
    }

    expect(TokenType::LPAREN, "Expected '(' after func name");

    if (!check(TokenType::RPAREN)) {
        node->params.push_back(parseFunctionParam());
        while (match({TokenType::COMMA})) {
            node->params.push_back(parseFunctionParam());
        }
    }
    expect(TokenType::RPAREN, "Expected ')' after parameters");

    if (match({TokenType::COLON})) {
        node->returnType = parseTypeAnnotation();
    } else {
        node->returnType.name = "void"; // no ': Type' written -> defaults to void
    }

    node->body = parseBlock();
    return node;
}

// Data fields retain the established `name: Type` syntax.
Param Parser::parseParam() {
    Token nameTok = expect(TokenType::IDENTIFIER, "Expected a name");
    Param p;
    p.name = nameTok.lexeme;
    if (match({TokenType::COLON})) {
        p.type = parseTypeAnnotation();
    }
    return p;
}

// Function-like parameters use `Type Name`; a bare name remains valid for
// untyped lambda parameters.
Param Parser::parseFunctionParam() {
    Param p;
    if (match({TokenType::KW_OWNED})) p.ownership = OwnershipKind::OWNED;
    else if (match({TokenType::KW_BORROW})) p.ownership = OwnershipKind::BORROW;
    else if (match({TokenType::KW_SHARED})) p.ownership = OwnershipKind::SHARED;
    else if (match({TokenType::KW_GC})) p.ownership = OwnershipKind::GC;
    // A parameter name may also be `list`, `map` or `set`: the bare-name form
    // is only taken when the name is the whole parameter, so a type keyword
    // that is followed by a type is still read as a type.
    if (checkName() &&
        (peekNext().type == TokenType::COMMA || peekNext().type == TokenType::RPAREN)) {
        p.name = advance().lexeme;
        return p;
    }

    if (startsTypeAnnotation()) {
        p.type = parseTypeAnnotation();
        Token nameTok = expectName("Expected a parameter name after its type");
        p.name = nameTok.lexeme;
        if (check(TokenType::COLON)) {
            error("Parameter types use `Type Name`; remove ':' from this parameter");
        }
        return p;
    }

    p.name = expectName("Expected a parameter name").lexeme;
    if (check(TokenType::COLON)) {
        error("Parameter types use `Type Name`; remove ':' from this parameter");
    }
    return p;
}

// ---------- types ----------

bool Parser::startsTypeAnnotation() const {
    return check(TokenType::KW_GC) || check(TokenType::KW_OWNED) || check(TokenType::KW_BORROW) || check(TokenType::KW_SHARED) ||
           check(TokenType::KW_ARRAY) || check(TokenType::KW_LIST) ||
           check(TokenType::KW_SET) || check(TokenType::KW_MAP) ||
           check(TokenType::KW_VOID) || check(TokenType::KW_FUNC) ||
           check(TokenType::IDENTIFIER);
}

// int
// array<int>            array[10]<int>
// list<string>
// set<int>
// map<string, int>
// int|string             (union - repeatable: int|string|bool)
TypeAnnotation Parser::parseTypeAnnotation() {
    // Generic type arguments recurse through here (`list<list<list<...>>>`),
    // so nesting is counted like blocks and expressions. Both returns below
    // decrement; a thrown ParseError discards the whole parser, so a missing
    // decrement on that path cannot be observed.
    if (++typeDepth_ > kMaxTypeDepth) {
        error("type nesting exceeds the maximum depth of " + std::to_string(kMaxTypeDepth));
    }
    TypeAnnotation type;
    type.line = peek().line;

    if (check(TokenType::KW_ARRAY)) {
        advance();
        type.name = "array";
        if (match({TokenType::LBRACKET})) {
            Token sizeTok = expect(TokenType::INT_LITERAL, "Expected an integer size in array[N]");
            try {
                std::size_t pos = 0;
                const unsigned long long rawSize = std::stoull(sizeTok.lexeme, &pos);
                if (pos != sizeTok.lexeme.size() ||
                    rawSize > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
                    error("array size is out of range; expected a non-negative integer no larger than "+
                          std::to_string(std::numeric_limits<int>::max()));
                }
                type.fixedSize = static_cast<int>(rawSize);
            } catch (...) {
                error("array size is out of range; expected a non-negative integer");
            }
            expect(TokenType::RBRACKET, "Expected ']' after array size");
        }
        expect(TokenType::LT, "Expected '<' to start array's element type, e.g. array<int>");
        type.typeArgs.push_back(parseTypeAnnotation());
        expectTypeAngleClose("Expected '>' to close array<...>");
    } else if (check(TokenType::KW_LIST)) {
        advance();
        type.name = "list";
        expect(TokenType::LT, "Expected '<' to start list's element type, e.g. list<int>");
        type.typeArgs.push_back(parseTypeAnnotation());
        expectTypeAngleClose("Expected '>' to close list<...>");
    } else if (check(TokenType::KW_SET)) {
        advance();
        type.name = "set";
        expect(TokenType::LT, "Expected '<' to start set's element type, e.g. set<int>");
        type.typeArgs.push_back(parseTypeAnnotation());
        expectTypeAngleClose("Expected '>' to close set<...>");
    } else if (check(TokenType::KW_MAP)) {
        advance();
        type.name = "map";
        expect(TokenType::LT, "Expected '<' to start map's key/value types, e.g. map<string, int>");
        type.typeArgs.push_back(parseTypeAnnotation()); // key type
        expect(TokenType::COMMA, "Expected ',' between map's key and value types");
        type.typeArgs.push_back(parseTypeAnnotation()); // value type
        expectTypeAngleClose("Expected '>' to close map<...>");
    } else if (check(TokenType::KW_VOID)) {
        advance();
        type.name = "void";
    } else if (check(TokenType::KW_FUNC)) {
        // Bare `func` remains the legacy untyped function slot. When followed
        // by parentheses it is a full function type: func(int, string): bool.
        advance();
        type.name = "func";
        if (match({TokenType::LPAREN})) {
            type.functionHasSignature = true;
            if (!check(TokenType::RPAREN)) {
                type.functionParamTypes.push_back(parseTypeAnnotation());
                while (match({TokenType::COMMA})) {
                    type.functionParamTypes.push_back(parseTypeAnnotation());
                }
            }
            expect(TokenType::RPAREN, "Expected ')' after function-type parameters");
            if (match({TokenType::COLON})) {
                type.functionReturnType = std::make_shared<TypeAnnotation>(parseTypeAnnotation());
            } else {
                TypeAnnotation ret;
                ret.name = "void";
                ret.line = type.line;
                type.functionReturnType = std::make_shared<TypeAnnotation>(std::move(ret));
            }
        }
    } else {
        Token nameTok = expect(TokenType::IDENTIFIER, "Expected a type name");
        type.name = nameTok.lexeme; // primitive (int, string, ...) or a custom class/data name

        // Box<int>, Pair<string, int>, etc. - a generic class name used as a
        // type annotation. Unambiguous here: we're already in type-annotation
        // position (this func is never called mid-expression), so '<'
        // can only mean "generic type arguments follow", the same reasoning
        // that already lets list<T>/map<K,V>/etc. above use LT/GT directly.
        if (match({TokenType::LT})) {
            type.typeArgs.push_back(parseTypeAnnotation());
            while (match({TokenType::COMMA})) {
                type.typeArgs.push_back(parseTypeAnnotation());
            }
            expectTypeAngleClose("Expected '>' to close type argument list");
        }
    }

    // Union types: int|string|bool. BIT_XOR reuses '^' elsewhere but '|' here
    // is BIT_OR - same token the bitwise-or operator uses; unambiguous because
    // we're in type-annotation position, not expression position.
    if (check(TokenType::BIT_OR)) {
        TypeAnnotation unionType;
        unionType.unionOf.push_back(std::move(type));
        while (match({TokenType::BIT_OR})) {
            unionType.unionOf.push_back(parseTypeAnnotation());
        }
        --typeDepth_;
        return unionType;
    }

    --typeDepth_;
    return type;
}

bool Parser::looksLikeTypedDeclStart() const {
    if (check(TokenType::KW_GC) || check(TokenType::KW_OWNED) ||
        check(TokenType::KW_BORROW)) {
        return true;
    }
    // `shared` is both an ownership modifier and a legal identifier (see
    // parseVarDecl, which accepts it as a variable name). A declaration needs
    // a type after the modifier, so `shared int x` is a declaration while
    // `shared.push(1)` or `shared = 2` is ordinary use of a variable called
    // `shared`. Requiring the following token to start a type keeps both
    // readings working.
    if (check(TokenType::KW_SHARED)) {
        if (pos_ + 1 >= tokens_.size()) return false;
        const auto next = tokens_[pos_ + 1].type;
        if (next == TokenType::IDENTIFIER || next == TokenType::KW_ARRAY ||
            next == TokenType::KW_LIST || next == TokenType::KW_SET ||
            next == TokenType::KW_MAP || next == TokenType::KW_FUNC) {
            return true;
        }
        return false;
    }
    if (check(TokenType::KW_ARRAY)) {
        return true;
    }
    // `list`, `set` and `map` are also legal names, so the keyword alone does
    // not decide: `list<int> xs = []` is a declaration, while `list = []` and
    // `list.push(1)` are ordinary use of a variable called `list`. A
    // declaration is the reading that has a complete type annotation followed
    // by the name - the same one-token-of-lookahead rule `shared` uses above.
    if (check(TokenType::KW_LIST) || check(TokenType::KW_SET) || check(TokenType::KW_MAP)) {
        const long end = typeLookahead_.scanAnnotation(pos_);
        return end >= 0 && static_cast<std::size_t>(end) < tokens_.size() &&
               isNameToken(tokens_[static_cast<std::size_t>(end)].type);
    }
    if (check(TokenType::KW_FUNC)) {
        const long end = typeLookahead_.scanAnnotation(pos_);
        return end >= 0 && static_cast<std::size_t>(end) < tokens_.size() &&
               isNameToken(tokens_[static_cast<std::size_t>(end)].type);
    }
    // "int x" (type name, then variable name) vs "foo(x)" (call) or "foo = x"
    // (assignment - not supported yet) or just "foo" alone: the only case
    // that's unambiguously a typed declaration is IDENTIFIER immediately
    // followed by another IDENTIFIER - OR a full type annotation (which may
    // be a union chain like `int|string`) immediately followed by another
    // IDENTIFIER, e.g. `int|string unionValue`. The name may itself be a
    // contextual type spelling: `int list = 3` declares an int called `list`.
    if (!check(TokenType::IDENTIFIER)) return false;
    long end = typeLookahead_.scanAnnotation(pos_);
    return end >= 0 && static_cast<std::size_t>(end) < tokens_.size() &&
           isNameToken(tokens_[static_cast<std::size_t>(end)].type);
}

// ---------- statements ----------

NodePtr Parser::parseStatement() {
    if (check(TokenType::KW_VAR) || check(TokenType::KW_LET)) return parseVarDecl();
    if (looksLikeTypedDeclStart()) return parseTypedVarDecl();
    if (check(TokenType::KW_LOG)) return parseLogStmt();
    if (check(TokenType::KW_IF)) return parseIfStmt();
    if (check(TokenType::KW_RETURN)) return parseReturnStmt();
    if (check(TokenType::KW_FOR)) return parseForStmt();
    if (check(TokenType::KW_WHILE)) return parseWhileStmt();
    if (check(TokenType::KW_REPEAT)) return parseRepeatStmt();
    if (check(TokenType::KW_BREAK)) return parseBreakStmt();
    if (check(TokenType::KW_CONTINUE)) return parseContinueStmt();
    if (check(TokenType::KW_TRY)) return parseTryStmt();
    if (check(TokenType::KW_THROW)) return parseThrowStmt();
    if (check(TokenType::LBRACE)) return parseBlock();
    return parseExprStmt();
}

NodePtr Parser::parseVarDecl(AccessModifier access) {
    bool isConst = check(TokenType::KW_LET);
    advance(); // consume 'var' or 'let'

    // `list`, `set` and `map` are type spellings, but a variable may share the
    // spelling: `var list = [1, 2]` is unambiguous because `var` has already
    // said that a name follows.
    Token nameTok;
    if (checkName()) nameTok = advance();
    else error("Expected variable name");

    auto node = std::make_unique<VarDecl>();
    node->line = nameTok.line;
    node->isConst = isConst;
    node->name = nameTok.lexeme;
    node->access = access;

    if (match({TokenType::ASSIGN})) {
        node->initializer = parseExpression();
    }
    return node;
}

NodePtr Parser::parseTypedVarDecl(AccessModifier access) {
    OwnershipKind ownership = OwnershipKind::GC;
    if (match({TokenType::KW_OWNED})) ownership = OwnershipKind::OWNED;
    else if (match({TokenType::KW_BORROW})) ownership = OwnershipKind::BORROW;
    else if (match({TokenType::KW_SHARED})) ownership = OwnershipKind::SHARED;
    else if (match({TokenType::KW_GC})) ownership = OwnershipKind::GC;
    TypeAnnotation type = parseTypeAnnotation();
    // The name may share a spelling with a type keyword: `list<int> list = []`
    // declares a list called `list`, and `int map = 3` a field called `map`.
    Token nameTok = expectName("Expected a variable name after the type");

    auto node = std::make_unique<VarDecl>();
    node->line = nameTok.line;
    node->hasExplicitType = true;
    node->type = std::move(type);
    node->ownership = ownership;
    node->name = nameTok.lexeme;
    node->access = access;

    if (match({TokenType::ASSIGN})) {
        node->initializer = parseExpression();
    }
    return node;
}

NodePtr Parser::parseLogStmt() {
    Token logTok = advance(); // consume 'log'
    expect(TokenType::LPAREN, "Expected '(' after log");

    auto node = std::make_unique<LogStmt>();
    node->line = logTok.line;
    node->argument = parseExpression();

    expect(TokenType::RPAREN, "Expected ')' after log argument");
    return node;
}

NodePtr Parser::parseIfStmt() {
    Token ifTok = advance(); // consume 'if'

    auto node = std::make_unique<IfStmt>();
    node->line = ifTok.line;

    IfStmt::Branch first;
    first.condition = parseConditionExpression();
    first.body = parseBlock();
    node->branches.push_back(std::move(first));

    while (check(TokenType::KW_ELSEIF) ||
           (check(TokenType::KW_ELSE) && peekNext().type == TokenType::KW_IF)) {
        if (check(TokenType::KW_ELSEIF)) {
            advance();
        } else {
            advance(); // 'else'
            advance(); // 'if'
        }
        IfStmt::Branch branch;
        branch.condition = parseConditionExpression();
        branch.body = parseBlock();
        node->branches.push_back(std::move(branch));
    }

    if (check(TokenType::KW_ELSE)) {
        advance();
        node->elseBody = parseBlock();
    }

    return node;
}

NodePtr Parser::parseReturnStmt() {
    Token retTok = advance(); // consume 'return'

    auto node = std::make_unique<ReturnStmt>();
    node->line = retTok.line;

    // A value is present only if the next token is still on the SAME line as
    // 'return'. Otherwise (e.g. 'return' immediately followed by '}' on the
    // next line) it's a bare return. We rely on token line numbers for this
    // since the language has no semicolons to mark statement boundaries.
    if (!isAtEnd() && peek().line == retTok.line && !check(TokenType::RBRACE)) {
        node->value = parseExpression();
    }
    return node;
}

// for i in start..end step s { body }   -- 'step s' is optional (defaults to 1)
NodePtr Parser::parseForStmt() {
    Token forTok = advance(); // consume 'for'
    Token varTok = expectName("Expected a loop variable name after 'for'");
    expect(TokenType::KW_IN, "Expected 'in' after the loop variable, e.g. for i in 0..10");

    auto node = std::make_unique<ForStmt>();
    node->line = forTok.line;
    node->varName = varTok.lexeme;
    node->start = parseConditionExpression();
    expect(TokenType::DOT_DOT, "Expected '..' between the start and end of a for-range");
    node->end = parseConditionExpression();

    if (match({TokenType::KW_STEP})) {
        node->step = parseConditionExpression();
    } else {
        auto one = std::make_unique<Literal>();
        one->literalType = TokenType::INT_LITERAL;
        one->raw = "1";
        node->step = std::move(one);
    }

    node->body = parseBlock();
    return node;
}

NodePtr Parser::parseWhileStmt() {
    Token whileTok = advance(); // consume 'while'
    auto node = std::make_unique<WhileStmt>();
    node->line = whileTok.line;
    node->condition = parseConditionExpression();
    node->body = parseBlock();
    return node;
}

// repeat { body } while cond  - body runs once before the condition is ever checked
NodePtr Parser::parseRepeatStmt() {
    Token repeatTok = advance(); // consume 'repeat'
    auto node = std::make_unique<RepeatStmt>();
    node->line = repeatTok.line;
    node->body = parseBlock();
    expect(TokenType::KW_WHILE, "Expected 'while' after a repeat block, e.g. repeat { ... } while cond");
    node->condition = parseConditionExpression();
    return node;
}

NodePtr Parser::parseBreakStmt() {
    Token tok = advance();
    auto node = std::make_unique<BreakStmt>();
    node->line = tok.line;
    return node;
}

NodePtr Parser::parseContinueStmt() {
    Token tok = advance();
    auto node = std::make_unique<ContinueStmt>();
    node->line = tok.line;
    return node;
}

// try { tryBlock } catch [ExceptionType] e { catchBlock } ... finally { finallyBlock }
NodePtr Parser::parseTryStmt() {
    Token tryTok = advance(); // consume 'try'
    auto node = std::make_unique<TryStmt>();
    node->line = tryTok.line;
    node->tryBlock = parseBlock();

    while (match({TokenType::KW_CATCH})) {
        CatchClause clause;
        clause.line = previous().line;
        // `catch Exception e { }` types the caught value; the bound name may
        // be a contextual type spelling (`catch Exception list { }`).
        if (check(TokenType::IDENTIFIER) && isNameToken(peekNext().type)) {
            clause.type = parseTypeAnnotation();
        }
        Token varTok = expectName(
            "Expected a variable name to bind the caught exception to, e.g. catch Exception e { }");
        clause.varName = varTok.lexeme;
        clause.block = parseBlock();
        node->catches.push_back(std::move(clause));
    }

    if (match({TokenType::KW_FINALLY})) {
        node->finallyBlock = parseBlock();
    }

    if (node->catches.empty() && !node->finallyBlock) {
        throw ParseError("Expected 'catch' or 'finally' after a try block");
    }
    return node;
}

NodePtr Parser::parseThrowStmt() {
    Token throwTok = advance(); // consume 'throw'
    auto node = std::make_unique<ThrowStmt>();
    node->line = throwTok.line;
    node->value = parseExpression();
    return node;
}

std::unique_ptr<BlockStmt> Parser::parseBlock() {
    // Every '{' recurses through here (a block's statements can contain
    // blocks), so this is where nesting is counted. The error is a normal
    // ParseError: hostile input gets a syntax error, not a stack overflow.
    if (++blockDepth_ > kMaxBlockDepth) {
        error("block nesting exceeds the maximum depth of " + std::to_string(kMaxBlockDepth));
    }
    expect(TokenType::LBRACE, "Expected '{'");
    auto block = std::make_unique<BlockStmt>();
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        block->statements.push_back(parseStatement());
    }
    expect(TokenType::RBRACE, "Expected '}'");
    --blockDepth_;
    return block;
}

NodePtr Parser::parseExprStmt() {
    auto node = std::make_unique<ExprStmt>();
    node->expression = parseExpression();
    return node;
}

// ---------- expressions (lowest precedence to highest) ----------

NodePtr Parser::parseExpression() { return ExpressionParser(*this).parseExpression(); }

NodePtr Parser::parseConditionExpression() {
    return ExpressionParser(*this).parseConditionExpression();
}


} // namespace zl
