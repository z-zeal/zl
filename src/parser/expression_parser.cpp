#include "zl/parser/expression_parser.hpp"
#include <optional>
#include <functional>
#include "zl/parser/parser.hpp"

namespace zl {

NodePtr ExpressionParser::parseConditionExpression() {
    bool saved = parser_.noDataLiteral_;
    parser_.noDataLiteral_ = true;
    NodePtr expr = parseExpression();
    parser_.noDataLiteral_ = saved;
    return expr;
}

namespace {

struct InfixBinding {
    int leftBindingPower;
    int rightBindingPower;
};

std::optional<InfixBinding> infixBinding(TokenType type) {
    // Higher numbers bind tighter. All operators below are left-associative
    // except assignment and power, which are handled specially by the Pratt
    // loop with a lower right binding power.
    switch (type) {
        case TokenType::OR:      return InfixBinding{10, 11};
        case TokenType::AND:     return InfixBinding{20, 21};
        case TokenType::BIT_OR:  return InfixBinding{30, 31};
        case TokenType::BIT_XOR: return InfixBinding{40, 41};
        case TokenType::BIT_AND: return InfixBinding{50, 51};
        case TokenType::EQ:
        case TokenType::NEQ:     return InfixBinding{60, 61};
        case TokenType::LT:
        case TokenType::GT:
        case TokenType::LTE:
        case TokenType::GTE:     return InfixBinding{70, 71};
        case TokenType::SHL:
        case TokenType::SHR:
        case TokenType::USHR:    return InfixBinding{80, 81};
        case TokenType::PLUS:
        case TokenType::MINUS:   return InfixBinding{90, 91};
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::PERCENT: return InfixBinding{100, 101};
        case TokenType::POW:     return InfixBinding{120, 120}; // right-associative
        case TokenType::ASSIGN:  return InfixBinding{1, 1};     // right-associative
        default:                 return std::nullopt;
    }
}

bool isUnary(TokenType type) {
    // Unary `+` is the identity. It is accepted here so `+5` parses; the type
    // checker (`OperatorRules::unaryResult`) still restricts it to int and
    // double, and both backends lower it to nothing at all.
    return type == TokenType::PLUS || type == TokenType::MINUS ||
           type == TokenType::NOT || type == TokenType::BIT_NOT;
}

// Ternary binds tighter than assignment (`a = c ? b : d` assigns the whole
// ternary) and looser than every other operator, and it is right-associative.
constexpr int kTernaryBindingPower = 2;

NodePtr makeBinary(TokenType op, NodePtr left, NodePtr right, std::size_t line) {
    auto node = std::make_unique<BinaryExpr>();
    node->op = op;
    node->left = std::move(left);
    node->right = std::move(right);
    node->line = line;
    return node;
}

// `cond ? whenTrue : whenFalse` becomes an exhaustive bool match.
//
// Desugaring rather than adding a node keeps one implementation of everything a
// ternary needs: the subject is evaluated once, exactly one arm runs, both arms
// must agree on a type (the checker's existing match rule, which is the same
// rule a ternary has), and bool coverage is already checked. No backend learns
// anything new.
NodePtr makeTernaryExpr(NodePtr condition, NodePtr whenTrue, NodePtr whenFalse, std::size_t line) {
    auto node = std::make_unique<MatchExpr>();
    node->line = line;
    node->isTernarySugar = true;
    node->subject = std::move(condition);

    MatchExpr::Arm trueArm;
    trueArm.line = line;
    trueArm.patternKind = MatchExpr::PatternKind::Literal;
    trueArm.literalType = TokenType::BOOL_LITERAL;
    trueArm.raw = "true";
    trueArm.result = std::move(whenTrue);

    MatchExpr::Arm falseArm;
    falseArm.line = line;
    falseArm.patternKind = MatchExpr::PatternKind::Literal;
    falseArm.literalType = TokenType::BOOL_LITERAL;
    falseArm.raw = "false";
    falseArm.result = std::move(whenFalse);

    node->arms.push_back(std::move(trueArm));
    node->arms.push_back(std::move(falseArm));
    return node;
}

} // namespace

NodePtr ExpressionParser::parseExpression() {
    return parsePratt(0);
}

NodePtr ExpressionParser::parsePratt(int minBindingPower) {
    // Save on entry: the loop below folds flat operator chains iteratively
    // and must count those folds against the same budget (see below).
    const int savedDepth = parser_.expressionDepth_;
    // Every nested expression (parens, unary chains, right operands, call
    // arguments, collection literals, lambdas, match arms) funnels through
    // here, so counting entries bounds the parser's C++ recursion depth.
    // Parenthesized/unary nesting is otherwise unbounded and overflows the
    // stack on hostile input; this turns it into an ordinary syntax error.
    // The depth counter lives on Parser so it survives the nested
    // ExpressionParser instances a statement's sub-expressions create.
    if (++parser_.expressionDepth_ > Parser::kMaxExpressionDepth) {
        parser_.error("expression nesting exceeds the maximum depth of " +
                      std::to_string(Parser::kMaxExpressionDepth));
    }

    NodePtr left;

    if (parser_.check(TokenType::KW_AWAIT)) {
        Token awaitTok = parser_.advance();
        auto node = std::make_unique<AwaitExpr>();
        node->line = awaitTok.line;
        node->operand = parsePratt(110);
        left = std::move(node);
    } else if (isUnary(parser_.peek().type)) {
        Token op = parser_.advance();
        // Unary minus deliberately binds looser than power so -2^^2 becomes
        // -(2^^2), while the right-hand side of power can still begin with a
        // unary expression such as 2^^-2.
        NodePtr operand = parsePratt(110);
        // Fold -9223372036854775808 (INT64_MIN) into a single literal. It is
        // the only int whose positive half exceeds INT64_MAX, so without this
        // fold the literal range check rejects the only possible spelling of
        // the most-negative int64. Nothing else folds: every other negative
        // number keeps its UnaryExpr shape exactly as before.
        if (op.type == TokenType::MINUS && operand->kind == NodeKind::Literal) {
            auto* literal = static_cast<Literal*>(operand.get());
            if (literal->literalType == TokenType::INT_LITERAL && literal->raw == "9223372036854775808") {
                literal->raw = "-9223372036854775808";
                literal->line = op.line;
                left = std::move(operand);
            } else {
                auto node = std::make_unique<UnaryExpr>();
                node->op = op.type;
                node->operand = std::move(operand);
                node->line = op.line;
                left = std::move(node);
            }
        } else {
            auto node = std::make_unique<UnaryExpr>();
            node->op = op.type;
            node->operand = std::move(operand);
            node->line = op.line;
            left = std::move(node);
        }
    } else {
        left = parseCall();
    }

    while (true) {
        // Ternary, just above assignment and right-associative. The branches are
        // parsed at full expression depth; the else branch is parsed at this
        // level so `a ? b : c ? d : e` groups to the right.
        if (parser_.check(TokenType::QUESTION) && minBindingPower <= kTernaryBindingPower) {
            Token question = parser_.advance();
            NodePtr whenTrue = parseExpression();
            parser_.expect(TokenType::COLON, "Expected ':' before the alternative of a '?' expression");
            NodePtr whenFalse = parsePratt(kTernaryBindingPower);
            left = makeTernaryExpr(std::move(left), std::move(whenTrue), std::move(whenFalse), question.line);
            if (++parser_.expressionDepth_ > Parser::kMaxExpressionDepth) {
                parser_.error("expression nesting exceeds the maximum depth of " +
                              std::to_string(Parser::kMaxExpressionDepth));
            }
            continue;
        }

        const TokenType type = parser_.peek().type;
        auto binding = infixBinding(type);
        if (!binding || binding->leftBindingPower < minBindingPower) {
            break;
        }

        Token op = parser_.advance();

        if (type == TokenType::ASSIGN) {
            std::string name;
            NodePtr objectTarget = nullptr;
            if (left->kind == NodeKind::Identifier) {
                name = static_cast<Identifier*>(left.get())->name;
            } else if (left->kind == NodeKind::FieldAccessExpr) {
                auto fieldAccess = static_cast<FieldAccessExpr*>(left.get());
                name = fieldAccess->fieldName;
                objectTarget = std::move(fieldAccess->object);
            } else {
                parser_.error("Left side of '=' must be a variable name or field access");
            }

            NodePtr value = parsePratt(binding->rightBindingPower);
            if (objectTarget) {
                auto node = std::make_unique<FieldAssignExpr>();
                node->line = op.line;
                node->object = std::move(objectTarget);
                node->fieldName = name;
                node->value = std::move(value);
                left = std::move(node);
            } else {
                auto node = std::make_unique<AssignExpr>();
                node->line = op.line;
                node->name = name;
                node->value = std::move(value);
                left = std::move(node);
            }
            // Flat chains (`a+b+c+...`) fold iteratively: without counting each
            // fold, a 100k-term chain parses fine and then overflows the stack in
            // the checker/compiler that walks the left-nested AST recursively.
            // Save/restore (not ++/--) keeps sibling sub-expressions from
            // consuming each other's budget.
            if (++parser_.expressionDepth_ > Parser::kMaxExpressionDepth) {
                parser_.error("expression nesting exceeds the maximum depth of " +
                              std::to_string(Parser::kMaxExpressionDepth));
            }
            continue;
        }

        NodePtr right = parsePratt(binding->rightBindingPower);
        left = makeBinary(type, std::move(left), std::move(right), op.line);
        // Flat chains (`a+b+c+...`) fold iteratively: without counting each
        // fold, a 100k-term chain parses fine and then overflows the stack in
        // the checker/compiler that walks the left-nested AST recursively.
        // Save/restore (not ++/--) keeps sibling sub-expressions from
        // consuming each other's budget.
        if (++parser_.expressionDepth_ > Parser::kMaxExpressionDepth) {
            parser_.error("expression nesting exceeds the maximum depth of " +
                          std::to_string(Parser::kMaxExpressionDepth));
        }
    }

    parser_.expressionDepth_ = savedDepth;
    return left;
}

std::vector<TypeAnnotation> ExpressionParser::parseOptionalCallTypeArgs() {
    if (!(parser_.check(TokenType::LT) &&
          parser_.typeLookahead_.looksLikeGenericCallArgs(parser_.pos_))) {
        return {};
    }
    parser_.advance(); // consume '<'
    std::vector<TypeAnnotation> args;
    args.push_back(parser_.parseTypeAnnotation());
    while (parser_.match({TokenType::COMMA})) {
        args.push_back(parser_.parseTypeAnnotation());
    }
    parser_.expectTypeAngleClose("Expected '>' to close type argument list");
    return args;
}

NodePtr ExpressionParser::parseCall() {
    // Save on entry: the loop below folds flat operator chains iteratively
    // and must count those folds against the same budget (see below).
    const int savedDepth = parser_.expressionDepth_;
    NodePtr expr = parsePrimary();

    while (true) {
        if (parser_.check(TokenType::LBRACKET)) {
            Token openTok = parser_.advance();
            auto node = std::make_unique<IndexAccessExpr>();
            node->line = openTok.line;
            node->object = std::move(expr);
            node->index = parseExpression();
            parser_.expect(TokenType::RBRACKET, "Expected ']' after index");
            expr = std::move(node);
        } else if (parser_.check(TokenType::DOT)) {
            Token dotTok = parser_.advance();
            // Property/method names are ordinarily IDENTIFIER, but a few
            // builtin namespace members intentionally reuse words that are
            // otherwise lexed as type keywords - e.g. `Collection.set(...)`
            // collides with the `set<T>` type keyword (KW_SET). Only these
            // collection-type keywords are accepted here; anything else
            // still requires a plain IDENTIFIER.
            Token nameTok;
            if (parser_.check(TokenType::KW_ARRAY) || parser_.check(TokenType::KW_LIST) ||
                parser_.check(TokenType::KW_SET) || parser_.check(TokenType::KW_MAP) ||
                parser_.check(TokenType::KW_LOG)) {
                nameTok = parser_.advance();
            } else {
                nameTok = parser_.expect(TokenType::IDENTIFIER, "Expected property name after '.'");
            }

            std::vector<TypeAnnotation> typeArgs = parseOptionalCallTypeArgs();
            if (parser_.check(TokenType::LPAREN)) {
                // Method call: obj.method(args) - UNLESS `expr` is a bare
                // Identifier starting with an uppercase letter, in which case
                // it's a namespace/library call like Math.sqrt(...) or
                // IO.readLine(...), not a call on some instance variable.
                // This mirrors the same uppercase convention constructor
                // calls already use to distinguish `Point(1, 2)` (a class)
                // from `add(1, 2)` (a func) without needing `new`.
                parser_.advance(); // consume '('

                bool isNamespaceCall = expr->kind == NodeKind::Identifier &&
                                       !static_cast<Identifier*>(expr.get())->name.empty() &&
                                       std::isupper(static_cast<unsigned char>(
                                           static_cast<Identifier*>(expr.get())->name[0]));

                if (isNamespaceCall) {
                    auto call = std::make_unique<CallExpr>();
                    call->line = dotTok.line;
                    call->namespaceName = static_cast<Identifier*>(expr.get())->name;
                    call->calleeName = nameTok.lexeme;
                    call->typeArgs = std::move(typeArgs);
                    if (!parser_.check(TokenType::RPAREN)) {
                        call->arguments.push_back(parseExpression());
                        while (parser_.match({TokenType::COMMA})) {
                            call->arguments.push_back(parseExpression());
                        }
                    }
                    parser_.expect(TokenType::RPAREN, "Expected ')' after arguments");
                    expr = std::move(call);
                } else {
                    auto node = std::make_unique<MethodCallExpr>();
                    node->line = dotTok.line;
                    node->object = std::move(expr);
                    node->methodName = nameTok.lexeme;
                    node->typeArgs = std::move(typeArgs);
                    if (!parser_.check(TokenType::RPAREN)) {
                        node->arguments.push_back(parseExpression());
                        while (parser_.match({TokenType::COMMA})) {
                            node->arguments.push_back(parseExpression());
                        }
                    }
                    parser_.expect(TokenType::RPAREN, "Expected ')' after method arguments");
                    expr = std::move(node);
                }
            } else {
                // Field access: obj.field
                auto node = std::make_unique<FieldAccessExpr>();
                node->line = dotTok.line;
                node->object = std::move(expr);
                node->fieldName = nameTok.lexeme;
                expr = std::move(node);
            }
        } else if (parser_.check(TokenType::KW_WITH)) {
            Token withTok = parser_.advance();
            parser_.expect(TokenType::LBRACE, "Expected '{' after 'with'");
            auto node = std::make_unique<DataUpdateExpr>();
            node->line = withTok.line;
            node->base = std::move(expr);
            if (!parser_.check(TokenType::RBRACE)) {
                do {
                    Token fieldTok = parser_.expect(TokenType::IDENTIFIER, "Expected a field name in 'with' update");
                    parser_.expect(TokenType::COLON, "Expected ':' after field name in 'with' update");
                    node->fields.emplace_back(fieldTok.lexeme, parseExpression());
                } while (parser_.match({TokenType::COMMA}));
            }
            parser_.expect(TokenType::RBRACE, "Expected '}' after 'with' update");
            expr = std::move(node);
        } else if (parser_.check(TokenType::LPAREN) ||
                   (parser_.check(TokenType::LT) &&
                    parser_.typeLookahead_.looksLikeGenericCallArgs(parser_.pos_))) {
            // Function call: func(args) or func<T>(args). Intercept '<' here
            // so Pratt does not treat it as less-than (`firstOf<int>(...)`).
            std::vector<TypeAnnotation> typeArgs = parseOptionalCallTypeArgs();
            Token parenTok = parser_.advance(); // consume '('

            if (expr->kind == NodeKind::Identifier) {
                auto call = std::make_unique<CallExpr>();
                call->line = parenTok.line;
                call->calleeName = static_cast<Identifier*>(expr.get())->name;
                call->typeArgs = std::move(typeArgs);
                if (!parser_.check(TokenType::RPAREN)) {
                    call->arguments.push_back(parseExpression());
                    while (parser_.match({TokenType::COMMA})) {
                        call->arguments.push_back(parseExpression());
                    }
                }
                parser_.expect(TokenType::RPAREN, "Expected ')' after arguments");
                expr = std::move(call);
            } else {
                parser_.error("Dynamic func calls (calling a complex expression) are not supported yet");
            }
        } else {
            break;
        }
        // Every iteration that didn't break wrapped `expr` one level deeper.
        if (++parser_.expressionDepth_ > Parser::kMaxExpressionDepth) {
            parser_.error("expression nesting exceeds the maximum depth of " +
                          std::to_string(Parser::kMaxExpressionDepth));
        }
    }
    parser_.expressionDepth_ = savedDepth;
    return expr;
}

NodePtr ExpressionParser::parsePrimary() {
    if (parser_.check(TokenType::KW_LOG)) {
        Token logTok = parser_.advance();
        parser_.expect(TokenType::LPAREN, "Expected '(' after log");
        auto node = std::make_unique<LogExpr>();
        node->line = logTok.line;
        node->argument = parseExpression();
        parser_.expect(TokenType::RPAREN, "Expected ')' after log argument");
        return node;
    }
    if (parser_.check(TokenType::KW_MATCH)) {
        return parseMatchExpr();
    }
    if (parser_.check(TokenType::KW_ASYNC)) {
        // Expression-level async lambda: `async func(...) => ...`.
        return parseLambdaExpr();
    }
    if (parser_.check(TokenType::KW_FUNC)) {
        return parseLambdaExpr();
    }

    if (parser_.check(TokenType::KW_MOVE)) {
        Token moveTok = parser_.advance();
        Token nameTok = parser_.expect(TokenType::IDENTIFIER, "Expected a variable name after 'move'");
        auto node = std::make_unique<MoveExpr>();
        node->line = moveTok.line;
        node->name = nameTok.lexeme;
        return node;
    }

    if (parser_.check(TokenType::KW_NEW)) {
        Token newTok = parser_.advance();
        Token classTok = parser_.expect(TokenType::IDENTIFIER, "Expected class name after 'new'");
        auto node = std::make_unique<NewExpr>();
        node->line = newTok.line;
        node->className = classTok.lexeme;
        // Generic instantiation, e.g. `new Box<int>(1)` - unambiguous here
        // ('new ClassName' is always immediately followed by '(' or a type
        // argument list, never a comparison expression), unlike the bare
        // `ClassName(...)` form below, where '<' could otherwise be read as
        // less-than. This is why generic classes require `new` explicitly -
        // see ExpressionParser::parsePrimary's bare-call branch for the non-generic case.
        if (parser_.match({TokenType::LT})) {
            node->typeArgs.push_back(parser_.parseTypeAnnotation());
            while (parser_.match({TokenType::COMMA})) {
                node->typeArgs.push_back(parser_.parseTypeAnnotation());
            }
            parser_.expectTypeAngleClose("Expected '>' to close type argument list");
        }
        parser_.expect(TokenType::LPAREN, "Expected '(' after class name in 'new'");
        if (!parser_.check(TokenType::RPAREN)) {
            node->arguments.push_back(parseExpression());
            while (parser_.match({TokenType::COMMA})) {
                node->arguments.push_back(parseExpression());
            }
        }
        parser_.expect(TokenType::RPAREN, "Expected ')' after constructor arguments");
        return node;
    }

    if (parser_.check(TokenType::KW_THIS)) {
        Token thisTok = parser_.advance();
        auto node = std::make_unique<ThisExpr>();
        node->line = thisTok.line;
        return node;
    }

    if (parser_.check(TokenType::KW_SUPER)) {
        Token superTok = parser_.advance();
        if (parser_.check(TokenType::LPAREN)) {
            // super(args) - parent constructor call. Handled entirely here
            // (not deferred to parseCall's trailing-.method loop) since it's
            // its own distinct node, not a general call on some expression.
            parser_.advance(); // consume '('
            auto node = std::make_unique<SuperCallExpr>();
            node->line = superTok.line;
            if (!parser_.check(TokenType::RPAREN)) {
                node->arguments.push_back(parseExpression());
                while (parser_.match({TokenType::COMMA})) {
                    node->arguments.push_back(parseExpression());
                }
            }
            parser_.expect(TokenType::RPAREN, "Expected ')' after super(...) arguments");
            return node;
        }
        parser_.expect(TokenType::DOT, "Expected '(' or '.' after 'super'");
        Token methodTok = parser_.expect(TokenType::IDENTIFIER, "Expected a method name after 'super.'");
        std::vector<TypeAnnotation> typeArgs = parseOptionalCallTypeArgs();
        parser_.expect(TokenType::LPAREN, "Expected '(' after 'super." + methodTok.lexeme + "'");
        auto node = std::make_unique<SuperMethodCallExpr>();
        node->line = superTok.line;
        node->methodName = methodTok.lexeme;
        node->typeArgs = std::move(typeArgs);
        if (!parser_.check(TokenType::RPAREN)) {
            node->arguments.push_back(parseExpression());
            while (parser_.match({TokenType::COMMA})) {
                node->arguments.push_back(parseExpression());
            }
        }
        parser_.expect(TokenType::RPAREN, "Expected ')' after 'super." + methodTok.lexeme + "(...)' arguments");
        return node;
    }
    if (parser_.check(TokenType::REGEX_LITERAL)) {
        Token tok = parser_.advance();
        auto node = std::make_unique<NewExpr>();
        node->line = tok.line;
        node->className = "Regex";
        auto arg = std::make_unique<Literal>();
        arg->line = tok.line;
        arg->literalType = TokenType::STRING_LITERAL;
        arg->raw = tok.lexeme;
        node->arguments.push_back(std::move(arg));
        return node;
    }

    if (parser_.check(TokenType::INT_LITERAL) || parser_.check(TokenType::DECIMAL_LITERAL) ||
        parser_.check(TokenType::FLOAT_LITERAL) || parser_.check(TokenType::STRING_LITERAL)) {
        Token tok = parser_.advance();
        auto lit = std::make_unique<Literal>();
        lit->line = tok.line; lit->literalType = tok.type; lit->raw = tok.lexeme;
        return lit;
    }

    if (parser_.check(TokenType::KW_TRUE) || parser_.check(TokenType::KW_FALSE) || parser_.check(TokenType::KW_NULL)) {
        Token tok = parser_.advance();
        auto lit = std::make_unique<Literal>();
        lit->line = tok.line;
        lit->literalType = (tok.type == TokenType::KW_NULL) ? TokenType::KW_NULL : TokenType::BOOL_LITERAL;
        lit->raw = tok.lexeme;
        return lit;
    }

    // `shared` doubles as an ownership modifier and a plain identifier; the
    // statement parser only treats it as a modifier when a type follows, so by
    // the time it reaches expression position it names a variable. The same
    // holds for `list`, `set` and `map`, which are type spellings but legal
    // names: no type can appear here, so the token names the variable.
    if (parser_.checkName()) {
        Token tok = parser_.advance();
        // Check if it's ClassName(args...) for constructor call without 'new'
        if (parser_.check(TokenType::LPAREN) && tok.lexeme.length() > 0 && std::isupper(tok.lexeme[0])) {
            parser_.expect(TokenType::LPAREN, "Expected '(' after class name");
            auto node = std::make_unique<NewExpr>();
            node->line = tok.line;
            node->className = tok.lexeme;
            if (!parser_.check(TokenType::RPAREN)) {
                node->arguments.push_back(parseExpression());
                while (parser_.match({TokenType::COMMA})) {
                    node->arguments.push_back(parseExpression());
                }
            }
            parser_.expect(TokenType::RPAREN, "Expected ')' after constructor arguments");
            return node;
        }

        // TypeName { field: expr, ... } for a `data` type - same
        // uppercase-leading convention that already distinguishes
        // `ClassName(args)` from `funcCall(args)` above, guarded off in
        // condition/range-bound position (see parser_.noDataLiteral_'s comment).
        if (parser_.check(TokenType::LBRACE) && !parser_.noDataLiteral_ && tok.lexeme.length() > 0 &&
            std::isupper(static_cast<unsigned char>(tok.lexeme[0]))) {
            return parseDataLiteral(tok);
        }

        auto id = std::make_unique<Identifier>();
        id->line = tok.line; id->name = tok.lexeme;
        return id;
    }

    if (parser_.check(TokenType::LBRACE) || parser_.check(TokenType::LBRACKET)) {
        return parseCollectionLiteral();
    }

    if (parser_.match({TokenType::LPAREN})) {
        NodePtr expr = parseExpression();
        parser_.expect(TokenType::RPAREN, "Expected ')' after expression");
        return expr;
    }

    parser_.error("Expected expression");
}

// match subject { pattern [when guard] => expression, ... }
NodePtr ExpressionParser::parseMatchExpr() {
    Token matchTok = parser_.advance();
    auto node = std::make_unique<MatchExpr>();
    node->line = matchTok.line;
    node->subject = parseExpression();
    parser_.expect(TokenType::LBRACE, "Expected '{' after match subject");
    if (parser_.check(TokenType::RBRACE)) parser_.error("match requires at least one arm");

    // Match patterns nest (List[...] of List[...] of ..., map{...}, data
    // patterns) through direct recursion that never passes through
    // parsePratt, so they need their own depth budget - hostile input is
    // otherwise a stack overflow (SIGSEGV). RAII-scoped: the many early
    // returns below make a manual decrement infeasible. (A ParseError
    // discards the whole parser, so the counter needs no unwind discipline
    // beyond the guard.)
    int patternDepth = 0;
    struct PatternDepthGuard {
        int& depth;
        ~PatternDepthGuard() { --depth; }
    };
    static constexpr int kMaxPatternDepth = 500;
    std::function<std::unique_ptr<MatchExpr::Pattern>()> parsePattern = [&]() -> std::unique_ptr<MatchExpr::Pattern> {
        if (++patternDepth > kMaxPatternDepth) {
            parser_.error("match pattern nesting exceeds the maximum depth of " +
                          std::to_string(kMaxPatternDepth));
        }
        PatternDepthGuard patternGuard{patternDepth};
        auto pattern = std::make_unique<MatchExpr::Pattern>();
        pattern->line = parser_.peek().line;
        if (parser_.check(TokenType::IDENTIFIER) && parser_.peek().lexeme == "_") {
            parser_.advance();
            pattern->kind = MatchExpr::PatternKind::Wildcard;
            return pattern;
        }
    if (parser_.check(TokenType::INT_LITERAL) || parser_.check(TokenType::DECIMAL_LITERAL) ||
            parser_.check(TokenType::FLOAT_LITERAL) || parser_.check(TokenType::STRING_LITERAL) ||
            parser_.check(TokenType::KW_TRUE) || parser_.check(TokenType::KW_FALSE) || parser_.check(TokenType::KW_NULL)) {
            Token tok = parser_.advance();
            pattern->kind = MatchExpr::PatternKind::Literal;
            pattern->literalType = (tok.type == TokenType::KW_TRUE || tok.type == TokenType::KW_FALSE)
                ? TokenType::BOOL_LITERAL : tok.type;
            pattern->raw = tok.lexeme;
            return pattern;
        }
        if (parser_.check(TokenType::KW_FUNC) || parser_.check(TokenType::KW_ARRAY)) {
            pattern->kind = MatchExpr::PatternKind::Type;
            pattern->typePattern = parser_.parseTypeAnnotation();
            pattern->bindingName = parser_.expect(TokenType::IDENTIFIER, "Expected binding after type pattern").lexeme;
            return pattern;
        }
        if (parser_.check(TokenType::IDENTIFIER) || parser_.check(TokenType::KW_LIST) || parser_.check(TokenType::KW_SET) || parser_.check(TokenType::KW_MAP)) {
            Token first = parser_.advance();
            if (parser_.match({TokenType::DOT})) {
                Token member = parser_.expect(TokenType::IDENTIFIER, "Expected enum member name after '.'");
                pattern->kind = MatchExpr::PatternKind::EnumMember;
                pattern->enumTypeName = first.lexeme;
                pattern->enumMemberName = member.lexeme;
                return pattern;
            }
            if (first.lexeme == "map" && parser_.check(TokenType::LBRACE)) {
                parser_.advance();
                pattern->kind = MatchExpr::PatternKind::Map;
                pattern->containerKind = "map";
                if (!parser_.check(TokenType::RBRACE)) {
                    while (true) {
                        MatchExpr::MapEntryPattern entry;
                        entry.line = parser_.peek().line;
                        entry.key = parsePattern();
                        parser_.expect(TokenType::COLON, "Expected ':' between map key pattern and value pattern");
                        entry.value = parsePattern();
                        pattern->mapEntries.push_back(std::move(entry));
                        if (!parser_.match({TokenType::COMMA})) break;
                        if (parser_.check(TokenType::RBRACE)) break;
                    }
                }
                parser_.expect(TokenType::RBRACE, "Expected '}' after map match pattern");
                return pattern;
            }
            if ((first.lexeme == "List" || first.lexeme == "list" || first.lexeme == "Set" || first.lexeme == "set") && parser_.check(TokenType::LBRACKET)) {
                parser_.advance();
                pattern->kind = MatchExpr::PatternKind::List;
                pattern->containerKind = first.lexeme;
                if (!parser_.check(TokenType::RBRACKET)) {
                    pattern->elements.push_back(parsePattern());
                    while (parser_.match({TokenType::COMMA})) pattern->elements.push_back(parsePattern());
                }
                parser_.expect(TokenType::RBRACKET, "Expected ']' after container match pattern");
                return pattern;
            }
            if (parser_.check(TokenType::LPAREN)) {
                parser_.advance();
                pattern->kind = MatchExpr::PatternKind::Data;
                pattern->positional = true;
                pattern->typePattern.name = first.lexeme;
                if (!parser_.check(TokenType::RPAREN)) {
                    while (true) {
                        MatchExpr::DataFieldPattern fp;
                        fp.line = parser_.peek().line;
                        fp.pattern = parsePattern();
                        pattern->fields.push_back(std::move(fp));
                        if (!parser_.match({TokenType::COMMA})) break;
                    }
                }
                parser_.expect(TokenType::RPAREN, "Expected ')' after positional data match pattern");
                return pattern;
            }
            if (parser_.check(TokenType::LBRACE)) {
                parser_.advance();
                pattern->kind = MatchExpr::PatternKind::Data;
                pattern->typePattern.name = first.lexeme;
                while (!parser_.check(TokenType::RBRACE)) {
                    Token field = parser_.expect(TokenType::IDENTIFIER, "Expected field name in data match pattern");
                    MatchExpr::DataFieldPattern fp;
                    fp.fieldName = field.lexeme;
                    fp.line = field.line;
                    if (parser_.match({TokenType::COLON})) {
                        fp.pattern = parsePattern();
                    } else {
                        fp.pattern = std::make_unique<MatchExpr::Pattern>();
                        fp.pattern->kind = MatchExpr::PatternKind::Variable;
                        fp.pattern->bindingName = field.lexeme;
                        fp.pattern->line = field.line;
                    }
                    pattern->fields.push_back(std::move(fp));
                    if (!parser_.match({TokenType::COMMA})) break;
                }
                parser_.expect(TokenType::RBRACE, "Expected '}' after data match pattern");
                return pattern;
            }
            if (parser_.check(TokenType::LT)) {
                // Generic type pattern, e.g. `Box<int> value`. We are already
                // in pattern/type position, so '<' unambiguously starts the
                // type argument list. Reuse the normal type parser for nested
                // generic/function arguments so the AST shape stays identical
                // to ordinary type annotations.
                pattern->kind = MatchExpr::PatternKind::Type;
                pattern->typePattern.name = first.lexeme;
                pattern->typePattern.line = first.line;
                parser_.advance();
                pattern->typePattern.typeArgs.push_back(parser_.parseTypeAnnotation());
                while (parser_.match({TokenType::COMMA}))
                    pattern->typePattern.typeArgs.push_back(parser_.parseTypeAnnotation());
                parser_.expectTypeAngleClose("Expected '>' to close generic type pattern");
                if (parser_.check(TokenType::IDENTIFIER) && parser_.peek().lexeme != "when") {
                    Token binding = parser_.advance();
                    pattern->bindingName = binding.lexeme;
                } else {
                    pattern->bindingName = "_";
                }
                return pattern;
            }
            if (parser_.check(TokenType::IDENTIFIER) && parser_.peek().lexeme != "when") {
                Token binding = parser_.advance();
                pattern->kind = MatchExpr::PatternKind::Type;
                pattern->typePattern.name = first.lexeme;
                pattern->bindingName = binding.lexeme;
                pattern->typePattern.line = first.line;
                return pattern;
            }
            pattern->kind = MatchExpr::PatternKind::Variable;
            pattern->bindingName = first.lexeme;
            return pattern;
        }
        parser_.error("Expected literal, _, variable/type name, Enum.Member, or data pattern");
        return pattern;
    };

    while (!parser_.check(TokenType::RBRACE)) {
        MatchExpr::Arm arm;
        arm.line = parser_.peek().line;
        auto pattern = parsePattern();
        arm.patternKind = pattern->kind;
        arm.raw = pattern->raw;
        arm.literalType = pattern->literalType;
        arm.enumTypeName = pattern->enumTypeName;
        arm.enumMemberName = pattern->enumMemberName;
        arm.typePattern = pattern->typePattern;
        arm.bindingName = pattern->bindingName;
        arm.dataFields = std::move(pattern->fields);
        arm.listElements = std::move(pattern->elements);
        arm.mapEntries = std::move(pattern->mapEntries);
        arm.containerKind = std::move(pattern->containerKind);
        arm.positional = pattern->positional;
        if (parser_.check(TokenType::IDENTIFIER) && parser_.peek().lexeme == "when") {
            parser_.advance();
            arm.guard = parser_.parseExpression();
        }
        parser_.expect(TokenType::FAT_ARROW, "Expected '=>' after match pattern");
        arm.result = parseExpression();
        node->arms.push_back(std::move(arm));
        // Match arms may be separated either by a comma or by a line/brace
        // boundary. Newlines are not tokens in ZL, so when there is no comma
        // we continue parsing another arm as long as the next token is not the
        // closing brace. This supports the canonical multi-line form:
        //
        //   match value {
        //       Some<int> item => log(item.value)
        //       None<int>      => log(0)
        //       _              => log(-1)
        //   }
        if (parser_.match({TokenType::COMMA})) continue;
        if (!parser_.check(TokenType::RBRACE)) continue;
    }
    parser_.expect(TokenType::RBRACE, "Expected '}' after match arms");
    return node;
}

// func(params) => expr   OR   func(params) { block }
NodePtr ExpressionParser::parseLambdaExpr() {
    bool isAsync = false;
    if (parser_.check(TokenType::KW_ASYNC)) {
        parser_.advance();
        isAsync = true;
    }
    Token fnTok = parser_.advance(); // consume 'func'
    auto node = std::make_unique<LambdaExpr>();
    node->line = fnTok.line;
    node->isAsync = isAsync;

    parser_.expect(TokenType::LPAREN, "Expected '(' after 'func' in a lambda expression");
    if (!parser_.check(TokenType::RPAREN)) {
        node->params.push_back(parser_.parseFunctionParam());
        while (parser_.match({TokenType::COMMA})) {
            node->params.push_back(parser_.parseFunctionParam());
        }
    }
    parser_.expect(TokenType::RPAREN, "Expected ')' after lambda parameters");

    // Optional return-type annotation, mirroring named func declarations.
    if (parser_.match({TokenType::COLON})) {
        node->hasDeclaredReturnType = true;
        node->declaredReturnType = parser_.parseTypeAnnotation();
    }

    if (parser_.match({TokenType::FAT_ARROW})) {
        if (parser_.check(TokenType::LBRACE)) {
            node->hasExprBody = false;
            node->blockBody = parser_.parseBlock();
        } else {
            node->hasExprBody = true;
            node->exprBody = parseExpression();
        }
    } else if (parser_.check(TokenType::LBRACE)) {
        node->hasExprBody = false;
        node->blockBody = parser_.parseBlock();
    } else {
        parser_.error("Expected '=>', '{', or ': ReturnType' after a lambda's parameter list");
    }
    return node;
}

// {}  {1, 2, 3}  {"a": 1, "b": 2}
// Whether the first entry has a ':' after it decides map vs. array/list/set.
NodePtr ExpressionParser::parseCollectionLiteral() {
    const bool brackets = parser_.check(TokenType::LBRACKET);
    Token openTok = parser_.advance(); // consume '{' or '['
    auto node = std::make_unique<CollectionLiteral>();
    node->line = openTok.line;
    node->bracketSyntax = brackets;

    const TokenType closeToken = brackets ? TokenType::RBRACKET : TokenType::RBRACE;

    if (parser_.check(closeToken)) {
        parser_.advance();
        return node; // empty literal - type is resolved later from context
    }

    NodePtr first = parseExpression();
    if (!brackets && parser_.match({TokenType::COLON})) {
        node->isMap = true;
        NodePtr firstValue = parseExpression();
        node->entries.emplace_back(std::move(first), std::move(firstValue));
        while (parser_.match({TokenType::COMMA})) {
            if (parser_.check(closeToken)) break; // allow a trailing comma
            NodePtr key = parseExpression();
            parser_.expect(TokenType::COLON, "Expected ':' between map key and value");
            NodePtr value = parseExpression();
            node->entries.emplace_back(std::move(key), std::move(value));
        }
    } else {
        node->elements.push_back(std::move(first));
        while (parser_.match({TokenType::COMMA})) {
            if (parser_.check(closeToken)) break; // allow a trailing comma
            node->elements.push_back(parseExpression());
        }
    }

    parser_.expect(closeToken, brackets
        ? "Expected ']' to close list literal"
        : "Expected '}' to close collection literal");
    return node;
}



// TypeName { field: expr, field: expr, ... } - `nameTok` (the type name)
// is already consumed; this starts at '{'. Field names must be plain
// identifiers here (unlike map-literal keys, which are arbitrary
// expressions) - order and exhaustiveness against the type's declared
// fields are validated later, by TypeChecker::inferDataLiteral, not here.
NodePtr ExpressionParser::parseDataLiteral(const Token& nameTok) {
    parser_.expect(TokenType::LBRACE, "Expected '{' to start a data literal");

    auto node = std::make_unique<DataLiteralExpr>();
    node->line = nameTok.line;
    node->typeName = nameTok.lexeme;

    if (!parser_.check(TokenType::RBRACE)) {
        do {
            if (parser_.check(TokenType::RBRACE)) break; // trailing comma
            Token fieldTok = parser_.expect(TokenType::IDENTIFIER, "Expected a field name in '" + nameTok.lexeme + "' literal");
            parser_.expect(TokenType::COLON, "Expected ':' after field name '" + fieldTok.lexeme + "'");
            node->fields.emplace_back(fieldTok.lexeme, parseExpression());
        } while (parser_.match({TokenType::COMMA}));
    }

    parser_.expect(TokenType::RBRACE, "Expected '}' to close '" + nameTok.lexeme + "' literal");
    return node;
}

} // namespace zl
