#include "zl/compiler/native_compiler.hpp"
#include "zl/compiler/ir_optimizer.hpp"
#include "zl/compiler/ir_lowering.hpp"
#include "zl/compiler/native_abi.hpp"
#include "zl/compiler/native_catalog.hpp"
#include <sstream>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <limits>
#include <cmath>

namespace zl::native {
namespace {

bool annotatedNative(const FunctionDecl& fn) {
    for (const auto& a : fn.annotations) if (a.name == "native") return true;
    return false;
}

std::string cppType(const TypeAnnotation& t) {
    if (t.name == "int") return "std::int64_t";
    if (t.name == "float" || t.name == "decimal") return "double";
    if (t.name == "bool") return "bool";
    return {};
}


bool checkedAdd(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) return false;
    out = a + b; return true;
}

bool checkedSub(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if ((b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) ||
        (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b)) return false;
    out = a - b; return true;
}

bool checkedMul(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if (a == 0 || b == 0) { out = 0; return true; }
    if (a == -1) { if (b == std::numeric_limits<std::int64_t>::min()) return false; out = -b; return true; }
    if (b == -1) { if (a == std::numeric_limits<std::int64_t>::min()) return false; out = -a; return true; }
    if (a > 0) {
        if (b > 0) { if (a > std::numeric_limits<std::int64_t>::max() / b) return false; }
        else { if (b < std::numeric_limits<std::int64_t>::min() / a) return false; }
    } else {
        if (b > 0) { if (a < std::numeric_limits<std::int64_t>::min() / b) return false; }
        else { if (a != 0 && b < std::numeric_limits<std::int64_t>::max() / a) return false; }
    }
    out = a * b; return true;
}

std::optional<std::int64_t> constantInt(const AstNode* n) {
    if (!n) return std::nullopt;
    if (n->kind == NodeKind::Literal) {
        const auto* l = static_cast<const Literal*>(n);
        if (l->literalType != TokenType::INT_LITERAL) return std::nullopt;
        try { return static_cast<std::int64_t>(std::stoll(l->raw)); } catch (...) { return std::nullopt; }
    }
    if (n->kind == NodeKind::UnaryExpr) {
        const auto* u = static_cast<const UnaryExpr*>(n);
        auto v = constantInt(u->operand.get());
        if (!v) return std::nullopt;
        if (u->op == TokenType::MINUS) { if (*v == std::numeric_limits<std::int64_t>::min()) return std::nullopt; return -*v; }
        if (u->op == TokenType::BIT_NOT) return ~*v;
        return std::nullopt;
    }
    if (n->kind != NodeKind::BinaryExpr) return std::nullopt;
    const auto* b = static_cast<const BinaryExpr*>(n);
    auto l = constantInt(b->left.get());
    auto r = constantInt(b->right.get());
    if (!l || !r) return std::nullopt;
    const auto a=*l, c=*r;
    std::int64_t out=0;
    switch (b->op) {
        case TokenType::PLUS: if (!checkedAdd(a,c,out)) return std::nullopt; return out;
        case TokenType::MINUS: if (!checkedSub(a,c,out)) return std::nullopt; return out;
        case TokenType::STAR: if (!checkedMul(a,c,out)) return std::nullopt; return out;
        case TokenType::SLASH: if (c==0 || (a==std::numeric_limits<std::int64_t>::min() && c==-1)) return std::nullopt; return a/c;
        case TokenType::PERCENT: if (c==0 || (a==std::numeric_limits<std::int64_t>::min() && c==-1)) return std::nullopt; return a%c;
        case TokenType::BIT_AND: return a&c;
        case TokenType::BIT_OR: return a|c;
        case TokenType::BIT_XOR: return a^c;
        case TokenType::SHL:
            if (c<0 || c>=64 || a<0) return std::nullopt;
            if (a > (std::numeric_limits<std::int64_t>::max() >> c)) return std::nullopt;
            return a << c;
        case TokenType::SHR: if (c<0 || c>=64 || a<0) return std::nullopt; return a >> c;
        case TokenType::USHR: if (c<0 || c>=64) return std::nullopt; return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) >> c);
        default: return std::nullopt;
    }
}

bool constantBool(const AstNode* n, bool& outValue) {
    if (!n) return false;
    if (n->kind == NodeKind::Literal) {
        const auto* l = static_cast<const Literal*>(n);
        if (l->literalType != TokenType::BOOL_LITERAL) return false;
        outValue = (l->raw == "true"); return true;
    }
    if (n->kind != NodeKind::BinaryExpr) return false;
    const auto* b = static_cast<const BinaryExpr*>(n);
    if (b->op != TokenType::AND && b->op != TokenType::OR) return false;
    bool l=false, r=false;
    if (!constantBool(b->left.get(), l) || !constantBool(b->right.get(), r)) return false;
    outValue = (b->op == TokenType::AND) ? (l && r) : (l || r);
    return true;
}

std::string tokenCppType(const Literal* lit) {
    if (!lit) return {};
    if (lit->literalType == TokenType::INT_LITERAL) return "std::int64_t";
    if (lit->literalType == TokenType::DECIMAL_LITERAL || lit->literalType == TokenType::FLOAT_LITERAL) return "double";
    if (lit->literalType == TokenType::BOOL_LITERAL) return "bool";
    return {};
}

std::string expressionType(const AstNode* n, const std::unordered_map<std::string, std::string>& locals) {
    if (!n) return {};
    switch (n->kind) {
        case NodeKind::Literal:
            return tokenCppType(static_cast<const Literal*>(n));
        case NodeKind::Identifier: {
            const auto* id = static_cast<const Identifier*>(n);
            auto it = locals.find(id->name);
            return it == locals.end() ? std::string{} : it->second;
        }
        case NodeKind::UnaryExpr: {
            const auto* u = static_cast<const UnaryExpr*>(n);
            const auto operand = expressionType(u->operand.get(), locals);
            if (operand == "std::int64_t" || operand == "double") return operand;
            if (operand == "bool" && u->op == TokenType::NOT) return "bool";
            return {};
        }
        case NodeKind::BinaryExpr: {
            const auto* b = static_cast<const BinaryExpr*>(n);
            const auto l = expressionType(b->left.get(), locals);
            const auto r = expressionType(b->right.get(), locals);
            switch (b->op) {
                case TokenType::EQ: case TokenType::NEQ: case TokenType::LT: case TokenType::GT:
                case TokenType::LTE: case TokenType::GTE: case TokenType::AND: case TokenType::OR:
                    return "bool";
                default:
                    if ((l == "std::int64_t" || l == "double") && (r == "std::int64_t" || r == "double"))
                        return (l == "double" || r == "double") ? "double" : "std::int64_t";
                    return {};
            }
        }
        default:
            return {};
    }
}

bool expression(const AstNode* n, std::ostringstream& out, const std::unordered_map<std::string, std::string>& locals) {
    if (!n) return false;
    if (const auto folded = constantInt(n)) { out << *folded; return true; }
    bool foldedBool = false;
    if (constantBool(n, foldedBool)) { out << (foldedBool ? "true" : "false"); return true; }
    switch (n->kind) {
        case NodeKind::Literal: {
            const auto* l = static_cast<const Literal*>(n);
            if (l->literalType == TokenType::INT_LITERAL) { out << l->raw; return true; }
            if (l->literalType == TokenType::DECIMAL_LITERAL || l->literalType == TokenType::FLOAT_LITERAL) { out << l->raw; return true; }
            if (l->literalType == TokenType::BOOL_LITERAL) { out << ((l->raw == "true") ? "true" : "false"); return true; }
            return false;
        }
        case NodeKind::Identifier: {
            const auto* id = static_cast<const Identifier*>(n);
            if (!locals.count(id->name)) return false;
            out << id->name; return true;
        }
        case NodeKind::UnaryExpr: {
            const auto* u = static_cast<const UnaryExpr*>(n);
            std::string op;
            switch (u->op) { case TokenType::MINUS: op="-"; break; case TokenType::BIT_NOT: op="~"; break; case TokenType::NOT: op="!"; break; default: return false; }
            out << op << "("; bool ok = expression(u->operand.get(), out, locals); out << ")"; return ok;
        }
        case NodeKind::BinaryExpr: {
            const auto* b = static_cast<const BinaryExpr*>(n);
            std::string op;
            switch (b->op) {
                case TokenType::PLUS: op="+"; break; case TokenType::MINUS: op="-"; break; case TokenType::STAR: op="*"; break;
                case TokenType::SLASH: op="/"; break; case TokenType::PERCENT: op="%"; break;
                case TokenType::BIT_AND: op="&"; break; case TokenType::BIT_OR: op="|"; break; case TokenType::BIT_XOR: op="^"; break;
                case TokenType::SHL: op="<<"; break; case TokenType::SHR: op=">>"; break; case TokenType::USHR: op=">>"; break;
                case TokenType::EQ: op="=="; break; case TokenType::NEQ: op="!="; break; case TokenType::LT: op="<"; break; case TokenType::GT: op=">"; break;
                case TokenType::LTE: op="<="; break; case TokenType::GTE: op=">="; break;
                case TokenType::AND: op="&&"; break; case TokenType::OR: op="||"; break;
                default: return false;
            }
            out << "("; bool ok = expression(b->left.get(), out, locals); out << " " << op << " "; ok = expression(b->right.get(), out, locals) && ok; out << ")"; return ok;
        }
        default: return false;
    }
}

bool emitStatements(const AstNode* node, std::ostringstream& out,
                    std::unordered_map<std::string, std::string>& locals,
                    bool& returned, std::string& error, int indent);

std::string spaces(int indent) { return std::string(static_cast<std::size_t>(indent), ' '); }

bool emitBlock(const AstNode* node, std::ostringstream& out,
               std::unordered_map<std::string, std::string>& locals,
               bool& returned, std::string& error, int indent) {
    if (!node || node->kind != NodeKind::BlockStmt) return false;
    const auto* b = static_cast<const BlockStmt*>(node);
    for (const auto& stmt : b->statements) {
        if (!emitStatements(stmt.get(), out, locals, returned, error, indent)) return false;
        if (returned) break;
    }
    return true;
}

bool emitStatements(const AstNode* node, std::ostringstream& out,
                    std::unordered_map<std::string, std::string>& locals,
                    bool& returned, std::string& error, int indent) {
    if (!node) return true;
    const auto pad = spaces(indent);
    switch (node->kind) {
        case NodeKind::BlockStmt:
            return emitBlock(node, out, locals, returned, error, indent);
        case NodeKind::VarDecl: {
            const auto* v = static_cast<const VarDecl*>(node);
            if (!v->initializer) { error="@native requires initialized local variables: " + v->name; return false; }
            std::string type = v->hasExplicitType ? cppType(v->type) : expressionType(v->initializer.get(), locals);
            if (type.empty()) { error="@native cannot infer supported local type for " + v->name; return false; }
            if (locals.count(v->name)) { error="@native local redeclaration is unsupported: " + v->name; return false; }
            out << pad << type << " " << v->name << " = ";
            if (!expression(v->initializer.get(), out, locals)) { error="@native unsupported local initializer in " + v->name; return false; }
            out << ";\n";
            locals[v->name] = type;
            return true;
        }
        case NodeKind::ExprStmt: {
            const auto* e = static_cast<const ExprStmt*>(node);
            if (!e->expression || e->expression->kind != NodeKind::AssignExpr) {
                error="@native only supports assignment expression statements in this subset"; return false;
            }
            const auto* a = static_cast<const AssignExpr*>(e->expression.get());
            auto it = locals.find(a->name);
            if (it == locals.end()) { error="@native assignment targets unsupported local '" + a->name + "'"; return false; }
            out << pad << a->name << " = ";
            if (!expression(a->value.get(), out, locals)) { error="@native unsupported assignment expression"; return false; }
            out << ";\n";
            return true;
        }
        case NodeKind::ForStmt: {
            const auto* f = static_cast<const ForStmt*>(node);
            if (f->iterable) {
                error="@native does not yet support list for-in loops";
                return false;
            }
            if (locals.count(f->varName)) { error="@native loop variable redeclaration is unsupported: " + f->varName; return false; }
            if (expressionType(f->start.get(), locals) != "std::int64_t" ||
                expressionType(f->end.get(), locals) != "std::int64_t" ||
                expressionType(f->step.get(), locals) != "std::int64_t") {
                error="@native numeric loops currently require int ranges: " + f->varName; return false;
            }
            std::ostringstream startExpr, endExpr, stepExpr;
            if (!expression(f->start.get(), startExpr, locals) ||
                !expression(f->end.get(), endExpr, locals) ||
                !expression(f->step.get(), stepExpr, locals)) {
                error="@native unsupported loop range expression"; return false;
            }
            const auto cStart = constantInt(f->start.get());
            const auto cEnd = constantInt(f->end.get());
            const auto cStep = constantInt(f->step.get());
            if (cStart && cEnd && cStep && *cStep != 0) {
                std::int64_t cur = *cStart;
                std::vector<std::int64_t> values;
                values.reserve(16);
                for (std::size_t n = 0; n < 17; ++n) {
                    if ((*cStep > 0 && cur >= *cEnd) || (*cStep < 0 && cur <= *cEnd)) break;
                    if (n == 16) { values.clear(); break; }
                    values.push_back(cur);
                    std::int64_t next = 0;
                    if (!checkedAdd(cur, *cStep, next)) { values.clear(); break; }
                    cur = next;
                }
                if (!values.empty()) {
                    for (const auto iv : values) {
                        out << pad << "{\n";
                        auto nestedLocals = locals;
                        nestedLocals[f->varName] = "std::int64_t";
                        out << spaces(indent + 4) << "const std::int64_t " << f->varName << " = " << iv << ";\n";
                        bool nestedReturned = false;
                        if (!emitBlock(f->body.get(), out, nestedLocals, nestedReturned, error, indent + 4)) return false;
                        out << pad << "}\n";
                        if (nestedReturned) { returned = true; break; }
                    }
                    return true;
                }
            }
            out << pad << "for (std::int64_t " << f->varName << " = " << startExpr.str()
                << "; (" << stepExpr.str() << " > 0 ? " << f->varName << " < " << endExpr.str()
                << " : " << f->varName << " > " << endExpr.str() << "); "
                << f->varName << " += " << stepExpr.str() << ") {\n";
            auto nestedLocals = locals;
            nestedLocals[f->varName] = "std::int64_t";
            bool nestedReturned = false;
            if (!emitBlock(f->body.get(), out, nestedLocals, nestedReturned, error, indent + 4)) return false;
            out << pad << "}\n";
            return true;
        }
        case NodeKind::IfStmt: {
            const auto* i = static_cast<const IfStmt*>(node);
            if (i->branches.empty()) { error="@native empty if statement"; return false; }
            for (std::size_t bi = 0; bi < i->branches.size(); ++bi) {
                const auto& br = i->branches[bi];
                if (expressionType(br.condition.get(), locals) != "bool") { error="@native if condition must be bool"; return false; }
                out << pad << (bi == 0 ? "if" : "else if") << " (";
                if (!expression(br.condition.get(), out, locals)) return false;
                out << ") {\n";
                bool nestedReturned = false;
                if (!emitBlock(br.body.get(), out, locals, nestedReturned, error, indent + 4)) return false;
                out << pad << "}";
            }
            if (i->elseBody) {
                out << " else {\n";
                bool nestedReturned = false;
                if (!emitBlock(i->elseBody.get(), out, locals, nestedReturned, error, indent + 4)) return false;
                out << pad << "}";
            }
            out << "\n";
            return true;
        }
        case NodeKind::ReturnStmt: {
            const auto* r = static_cast<const ReturnStmt*>(node);
            if (!r->value) { out << pad << "return;\n"; returned = true; return true; }
            out << pad << "return ";
            if (!expression(r->value.get(), out, locals)) { error="@native return expression is outside the supported numeric subset"; return false; }
            out << ";\n";
            returned = true;
            return true;
        }
        default:
            error="@native statement is outside the supported numeric/data subset";
            return false;
    }
}

ZlNativeOwnershipTag ffiOwnershipForParam(const Param& param) {
    switch (param.ownership) {
        case OwnershipKind::BORROW: return ZL_NATIVE_OWNERSHIP_BORROWED;
        case OwnershipKind::OWNED: return ZL_NATIVE_OWNERSHIP_OWNED;
        case OwnershipKind::SHARED: return ZL_NATIVE_OWNERSHIP_BORROWED;
        case OwnershipKind::GC: return ZL_NATIVE_OWNERSHIP_NONE;
    }
    return ZL_NATIVE_OWNERSHIP_NONE;
}

bool validateFfiOwnership(const FunctionDecl& fn, std::string& error) {
    for (const auto& p : fn.params) {
        if (p.ownership == OwnershipKind::GC) continue;
        error = "@native ownership metadata for parameter '" + p.name +
                "' requires the opaque-handle/buffer ABI; primitive @native exports cannot carry ownership";
        return false;
    }
    return true;
}

void emitFunction(const FunctionDecl& fn, std::ostringstream& out, bool& ok, std::string& error) {
    if (!annotatedNative(fn) || !ok) return;
    if (!validateFfiOwnership(fn, error)) { ok=false; return; }
    if (!fn.ownerClassName.empty() && !fn.isStatic) { ok=false; error="@native currently supports only free or static functions: " + fn.name; return; }
    if (fn.isAsync) { ok=false; error="@native simple functions cannot be async: " + fn.name; return; }
    const std::string ret = cppType(fn.returnType);
    if (ret.empty()) { ok=false; error="@native unsupported return type for " + fn.name; return; }
    std::unordered_map<std::string, std::string> locals;
    out << "extern \"C\" " << ret << " zl_native_" << fn.name << "(";
    for (std::size_t i=0;i<fn.params.size();++i) {
        if (i) out << ", ";
        const std::string ct = cppType(fn.params[i].type);
        if (ct.empty()) { ok=false; error="@native unsupported parameter type in " + fn.name; return; }
        locals[fn.params[i].name] = ct;
        out << ct << " " << fn.params[i].name;
    }
    out << ") {\n";
    bool returned = false;
    if (!emitBlock(fn.body.get(), out, locals, returned, error, 4)) { ok=false; return; }
    if (!returned) { ok=false; error="@native requires a terminating return in " + fn.name; return; }
    out << "}\n\n";
}
}

CompileResult emitSimpleCpp(const Program& program) {
    CompileResult result;
    // The native backend starts from the stable semantic/IR lowering gate. The
    // restricted C++ emitter intentionally accepts only the subset it can prove
    // safe; ordinary compilation continues through the VM bytecode path.
    const auto irResult = zl::ir::lowerProgram(program);
    (void)irResult;

    std::ostringstream out;
    out << "#include <cstdint>\n#include <cmath>\n#include <cstring>\n#include <exception>\n#include \"zl/compiler/native_abi.hpp\"\n\n";
    bool ok = true; std::string error;
    std::vector<const FunctionDecl*> exports;
    for (const auto& d : program.declarations) {
        if (d->kind == NodeKind::ClassDecl) {
            const auto* cls = static_cast<const ClassDecl*>(d.get());
            for (const auto& m : cls->members) if (m->kind == NodeKind::FunctionDecl) {
                const auto* fn = static_cast<const FunctionDecl*>(m.get());
                if (annotatedNative(*fn)) exports.push_back(fn);
                emitFunction(*fn, out, ok, error);
            }
        } else if (d->kind == NodeKind::DataDecl) {
            const auto* data = static_cast<const DataDecl*>(d.get());
            for (const auto& m : data->members) if (m->kind == NodeKind::FunctionDecl && annotatedNative(*static_cast<const FunctionDecl*>(m.get()))) { ok=false; error="@native is not available on data methods yet"; }
        } else if (d->kind == NodeKind::FunctionDecl) {
            const auto* fn = static_cast<const FunctionDecl*>(d.get());
            if (annotatedNative(*fn)) exports.push_back(fn);
            emitFunction(*fn, out, ok, error);
        }
    }
    if (ok && !exports.empty()) {
        for (std::size_t i = 0; i < exports.size(); ++i) {
            const auto* fn = exports[i];
            out << "static const ZlNativeTypeTag zl_native_params_" << i << "[] = {";
            for (std::size_t p = 0; p < fn->params.size(); ++p) {
                if (p) out << ", ";
                const auto tag = cppType(fn->params[p].type);
                out << ((tag == "std::int64_t") ? "ZL_NATIVE_I64" : (tag == "double" ? "ZL_NATIVE_F64" : "ZL_NATIVE_BOOL"));
            }
            out << "};\n";
            out << "static const ZlNativeOwnershipTag zl_native_ownership_" << i << "[] = {";
            for (std::size_t p = 0; p < fn->params.size(); ++p) {
                if (p) out << ", ";
                switch (ffiOwnershipForParam(fn->params[p])) {
                    case ZL_NATIVE_OWNERSHIP_BORROWED: out << "ZL_NATIVE_OWNERSHIP_BORROWED"; break;
                    case ZL_NATIVE_OWNERSHIP_OWNED: out << "ZL_NATIVE_OWNERSHIP_OWNED"; break;
                    case ZL_NATIVE_OWNERSHIP_CONSUMED: out << "ZL_NATIVE_OWNERSHIP_CONSUMED"; break;
                    default: out << "ZL_NATIVE_OWNERSHIP_NONE"; break;
                }
            }
            out << "};\n";
        }
        for (std::size_t i = 0; i < exports.size(); ++i) {
            const auto* fn = exports[i];
            const auto ret = cppType(fn->returnType);
            const auto tag = ret == "std::int64_t" ? "ZL_NATIVE_I64" : (ret == "double" ? "ZL_NATIVE_F64" : "ZL_NATIVE_BOOL");
            out << "static std::int32_t zl_native_invoke_" << i << "(const ZlNativeValue* args, std::uint32_t argc, ZlNativeValue* result, char* errorMessage, std::uint32_t errorCapacity) {\n";
            out << "    if (argc != " << fn->params.size() << ") { const char* m=\"arity mismatch\"; if (errorMessage && errorCapacity) { std::strncpy(errorMessage,m,errorCapacity-1); errorMessage[errorCapacity-1]=0; } return 1; }\n";
            for (std::size_t p = 0; p < fn->params.size(); ++p) {
                const auto ptag = fn->params[p].type.name == "int" ? "ZL_NATIVE_I64" : (fn->params[p].type.name == "float" || fn->params[p].type.name == "decimal" ? "ZL_NATIVE_F64" : "ZL_NATIVE_BOOL");
                out << "    if (!args || args[" << p << "].tag != " << ptag << ") { const char* m=\"argument type mismatch\"; if (errorMessage && errorCapacity) { std::strncpy(errorMessage,m,errorCapacity-1); errorMessage[errorCapacity-1]=0; } return 1; }\n";
            }
            out << "    try {\n";
            out << "        result->tag = " << tag << ";\n";
            out << "        result->data." << (tag == std::string("ZL_NATIVE_I64") ? "i64" : (tag == "ZL_NATIVE_F64" ? "f64" : "boolean")) << " = zl_native_" << fn->name << "(";
            for (std::size_t p = 0; p < fn->params.size(); ++p) {
                if (p) out << ", ";
                const auto ptag = fn->params[p].type.name == "int" ? "i64" : (fn->params[p].type.name == "float" || fn->params[p].type.name == "decimal" ? "f64" : "boolean");
                out << "args[" << p << "].data." << ptag;
            }
            out << ");\n";
            out << "        return 0;\n    } catch (const std::exception& e) { if (errorMessage && errorCapacity) { std::strncpy(errorMessage,e.what(),errorCapacity-1); errorMessage[errorCapacity-1]=0; } return 1; }\n";
            out << "}\n";
        }
        out << "extern \"C\" const ZlNativeExport zl_native_exports[] = {\n";
        for (std::size_t i = 0; i < exports.size(); ++i) {
            const auto* fn = exports[i];
            const auto ret = cppType(fn->returnType);
            const auto tag = ret == "std::int64_t" ? "ZL_NATIVE_I64" : (ret == "double" ? "ZL_NATIVE_F64" : "ZL_NATIVE_BOOL");
            out << "    {\"" << fn->name << "\", " << fn->params.size() << ", zl_native_params_" << i << ", " << tag << ", zl_native_invoke_" << i << ", zl_native_ownership_" << i << ", ZL_NATIVE_OWNERSHIP_NONE}" << (i + 1 == exports.size() ? "\n" : ",\n");
        }
        out << "};\n";
        out << "extern \"C\" const std::uint32_t zl_native_export_count = " << exports.size() << ";\n";
        out << "extern \"C\" const std::uint32_t zl_native_abi_version = 2;\n\n";
    }
    result.success = ok;
    result.source = out.str();
    result.error = error;
    return result;
}


namespace {

std::string mirOpcodeName(zl::ir::Opcode op) {
    using zl::ir::Opcode;
    switch (op) {
        case Opcode::Const: return "Const";
        case Opcode::LoadLocal: return "LoadLocal";
        case Opcode::DefineLocal: return "DefineLocal";
        case Opcode::AssignLocal: return "AssignLocal";
        case Opcode::Unary: return "Unary";
        case Opcode::Binary: return "Binary";
        case Opcode::Return: return "Return";
        case Opcode::Branch: return "Branch";
        case Opcode::Jump: return "Jump";
        case Opcode::MoveLocal: return "MoveLocal";
        case Opcode::BorrowLocal: return "BorrowLocal";
        case Opcode::EndBorrow: return "EndBorrow";
        case Opcode::DropLocal: return "DropLocal";
        case Opcode::Call: return "Call";
        default: return "Unsupported";
    }
}

bool mirTypeSupported(const std::string& type) {
    return type == "int" || type == "float" || type == "decimal" || type == "bool";
}

std::string mirCppType(const std::string& type) {
    if (type == "int") return "std::int64_t";
    if (type == "float" || type == "decimal") return "double";
    if (type == "bool") return "bool";
    return {};
}

std::string mirBinaryOp(const std::string& symbol) {
    using T = zl::TokenType;
    int raw = 0;
    try { raw = std::stoi(symbol); } catch (...) { return {}; }
    switch (static_cast<T>(raw)) {
        case T::PLUS: return "+"; case T::MINUS: return "-"; case T::STAR: return "*";
        case T::SLASH: return "/"; case T::PERCENT: return "%"; case T::POW: return "pow";
        case T::BIT_AND: return "&"; case T::BIT_OR: return "|"; case T::BIT_XOR: return "^";
        case T::SHL: return "<<"; case T::SHR: return ">>"; case T::USHR: return ">>";
        case T::EQ: return "=="; case T::NEQ: return "!="; case T::LT: return "<"; case T::GT: return ">";
        case T::LTE: return "<="; case T::GTE: return ">="; case T::AND: return "&&"; case T::OR: return "||";
        default: return {};
    }
}

std::string mirUnaryOp(const std::string& symbol) {
    using T = zl::TokenType;
    int raw = 0;
    try { raw = std::stoi(symbol); } catch (...) { return {}; }
    switch (static_cast<T>(raw)) {
        case T::MINUS: return "-"; case T::BIT_NOT: return "~"; case T::NOT: return "!";
        default: return {};
    }
}

CompileResult emitMirFunction(const zl::ir::Function& fn, const std::unordered_map<std::string, const zl::ir::Function*>& nativeFns) {
    CompileResult result;
    if (fn.blocks.empty()) { result.error = "MIR native lowering requires at least one basic block"; return result; }
    if (fn.isAsync) { result.error = "MIR native lowering does not support async native functions: " + fn.name; return result; }
    if (!mirTypeSupported(fn.returnType)) { result.error = "MIR native lowering unsupported return type for " + fn.name; return result; }

    std::string safeName = fn.name;
    for (char& c : safeName) if (c == '.') c = '_';

    std::unordered_map<zl::ir::ValueId, std::string> localNames;
    std::unordered_map<zl::ir::ValueId, std::string> valueTypes;
    std::vector<std::pair<zl::ir::ValueId, std::string>> localsInOrder;
    std::vector<zl::ir::ValueId> tempIds;

    // Function parameters occupy the first DefineLocal instructions in block zero.
    std::size_t paramIndex = 0;
    if (!fn.blocks.front().instructions.empty()) {
        for (const auto& ins : fn.blocks.front().instructions) {
            if (ins.opcode != zl::ir::Opcode::DefineLocal || paramIndex >= fn.parameterTypes.size()) continue;
            localNames[ins.result] = "l" + std::to_string(ins.result);
            const auto ct = mirCppType(fn.parameterTypes[paramIndex]);
            if (ct.empty()) { result.error = "MIR native lowering unsupported parameter type in " + fn.name; return result; }
            valueTypes[ins.result] = ct;
            localsInOrder.emplace_back(ins.result, ct);
            ++paramIndex;
        }
    }
    if (paramIndex != fn.parameterTypes.size()) {
        result.error = "MIR native lowering could not resolve all parameter locals for " + fn.name;
        return result;
    }

    auto rememberTemp = [&](zl::ir::ValueId id) {
        if (!id || localNames.count(id)) return;
        if (std::find(tempIds.begin(), tempIds.end(), id) == tempIds.end()) tempIds.push_back(id);
    };

    auto inferConst = [](const std::string& raw) -> std::string {
        if (raw == "true" || raw == "false") return "bool";
        if (raw.find('.') != std::string::npos || raw.find('e') != std::string::npos || raw.find('E') != std::string::npos) return "double";
        return "std::int64_t";
    };

    // Establish local identities and infer value types. A short fixed-point pass is enough
    // for the straight-line MIR subset currently accepted by the native backend.
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block.instructions) {
            if (ins.opcode == zl::ir::Opcode::DefineLocal && !localNames.count(ins.result)) {
                localNames[ins.result] = "l" + std::to_string(ins.result);
                rememberTemp(ins.operand0);
                if (ins.operand0 && valueTypes.count(ins.operand0)) {
                    valueTypes[ins.result] = valueTypes[ins.operand0];
                }
                localsInOrder.emplace_back(ins.result, valueTypes.count(ins.result) ? valueTypes[ins.result] : "std::int64_t");
            }
        }
    }

    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (const auto& block : fn.blocks) {
            for (const auto& ins : block.instructions) {
                std::string inferred;
                switch (ins.opcode) {
                    case zl::ir::Opcode::Const: inferred = inferConst(ins.symbol); break;
                    case zl::ir::Opcode::LoadLocal:
                        if (valueTypes.count(ins.operand0)) inferred = valueTypes[ins.operand0];
                        break;
                    case zl::ir::Opcode::MoveLocal:
                    case zl::ir::Opcode::BorrowLocal:
                        if (valueTypes.count(ins.operand0)) inferred = valueTypes[ins.operand0];
                        break;
                    case zl::ir::Opcode::Unary: {
                        if (valueTypes.count(ins.operand0)) inferred = valueTypes[ins.operand0];
                        int raw = 0; try { raw = std::stoi(ins.symbol); } catch (...) { raw = 0; }
                        if (static_cast<zl::TokenType>(raw) == zl::TokenType::NOT) inferred = "bool";
                        break;
                    }
                    case zl::ir::Opcode::Binary: {
                        const auto l = valueTypes.find(ins.operand0), r = valueTypes.find(ins.operand1);
                        if (l != valueTypes.end() && r != valueTypes.end()) {
                            const auto op = mirBinaryOp(ins.symbol);
                            const int raw = [&]() { try { return std::stoi(ins.symbol); } catch (...) { return 0; } }();
                            const auto tok = static_cast<zl::TokenType>(raw);
                            const bool comparison = tok == zl::TokenType::EQ || tok == zl::TokenType::NEQ || tok == zl::TokenType::LT || tok == zl::TokenType::GT || tok == zl::TokenType::LTE || tok == zl::TokenType::GTE || tok == zl::TokenType::AND || tok == zl::TokenType::OR;
                            if (comparison) inferred = "bool";
                            else if (tok == zl::TokenType::POW) inferred =
                                (l->second == "std::int64_t" && r->second == "std::int64_t") ? "std::int64_t" : "double";
                            else if (op.find("/") != std::string::npos && (l->second == "double" || r->second == "double")) inferred = "double";
                            else inferred = (l->second == "double" || r->second == "double") ? "double" : l->second;
                        }
                        break;
                    }
                    case zl::ir::Opcode::DefineLocal:
                        if (ins.operand0 && valueTypes.count(ins.operand0)) inferred = valueTypes[ins.operand0];
                        break;
                    case zl::ir::Opcode::AssignLocal:
                        if (ins.operand0 && valueTypes.count(ins.operand0)) inferred = valueTypes[ins.operand0];
                        break;
                    case zl::ir::Opcode::Call: {
                        auto it = nativeFns.find(ins.symbol);
                        if (it != nativeFns.end()) inferred = mirCppType(it->second->returnType);
                        break;
                    }
                    default: break;
                }
                if (ins.result && !localNames.count(ins.result)) rememberTemp(ins.result);
                if (ins.result && !inferred.empty()) {
                    auto it = valueTypes.find(ins.result);
                    if (it == valueTypes.end() || it->second != inferred) { valueTypes[ins.result] = inferred; changed = true; }
                }
            }
        }
        for (auto& [slot, type] : localsInOrder) {
            if (valueTypes.count(slot)) type = valueTypes[slot];
        }
        if (!changed) break;
    }

    for (const auto id : tempIds) {
        if (!valueTypes.count(id)) { result.error = "MIR native lowering could not infer value type for " + std::to_string(id); return result; }
    }
    for (const auto& [slot, type] : localsInOrder) {
        if (type.empty() || type == "void") { result.error = "MIR native lowering could not infer local type"; return result; }
    }

    std::ostringstream out;
    out << "extern \"C\" " << mirCppType(fn.returnType) << " zl_mir_native_" << safeName << "(";
    for (std::size_t i = 0; i < fn.parameterTypes.size(); ++i) {
        if (i) out << ", ";
        out << mirCppType(fn.parameterTypes[i]) << " p" << i;
    }
    out << ") {\n";

    for (const auto& [slot, type] : localsInOrder) out << "    " << type << " " << localNames[slot] << "{};\n";
    for (const auto id : tempIds) out << "    " << valueTypes[id] << " v" << id << "{};\n";

    // Initialize parameter locals from the function parameters.
    paramIndex = 0;
    for (const auto& ins : fn.blocks.front().instructions) {
        if (ins.opcode == zl::ir::Opcode::DefineLocal && paramIndex < fn.parameterTypes.size()) {
            out << "    " << localNames[ins.result] << " = p" << paramIndex++ << ";\n";
        }
    }

    for (const auto& block : fn.blocks) {
        out << "L" << block.id << ":\n";
        for (const auto& ins : block.instructions) {
            switch (ins.opcode) {
                case zl::ir::Opcode::Nop:
                    break;
                case zl::ir::Opcode::Const:
                    out << "    v" << ins.result << " = " << ins.symbol << ";\n";
                    break;
                case zl::ir::Opcode::LoadLocal:
                    if (!localNames.count(ins.operand0)) { result.error = "MIR LoadLocal cannot resolve local"; return result; }
                    out << "    v" << ins.result << " = " << localNames[ins.operand0] << ";\n";
                    break;
                case zl::ir::Opcode::DefineLocal:
                    if (ins.operand0) out << "    " << localNames[ins.result] << " = v" << ins.operand0 << ";\n";
                    break;
                case zl::ir::Opcode::AssignLocal:
                    if (!localNames.count(ins.result)) { result.error = "MIR AssignLocal cannot resolve local"; return result; }
                    out << "    " << localNames[ins.result] << " = v" << ins.operand0 << ";\n";
                    break;
                case zl::ir::Opcode::Unary: {
                    const auto op = mirUnaryOp(ins.symbol);
                    if (op.empty()) { result.error = "MIR Unary unsupported"; return result; }
                    int raw = 0; try { raw = std::stoi(ins.symbol); } catch (...) { raw = 0; }
                    const auto tok = static_cast<zl::TokenType>(raw);
                    const auto operandType = valueTypes.count(ins.operand0) ? valueTypes.at(ins.operand0) : std::string{};
                    if (tok == zl::TokenType::BIT_NOT && operandType != "std::int64_t") {
                        result.error = "MIR bitwise-not requires int operand"; return result;
                    }
                    out << "    v" << ins.result << " = " << op << "(v" << ins.operand0 << ");\n";
                    break;
                }
                case zl::ir::Opcode::Binary: {
                    const auto op = mirBinaryOp(ins.symbol);
                    if (op.empty()) { result.error = "MIR Binary unsupported"; return result; }
                    const auto resultTypeIt = valueTypes.find(ins.result);
                    const std::string resultType = resultTypeIt == valueTypes.end() ? std::string{} : resultTypeIt->second;
                    const auto lhsTypeIt = valueTypes.find(ins.operand0);
                    const auto rhsTypeIt = valueTypes.find(ins.operand1);
                    const std::string lhsType = lhsTypeIt == valueTypes.end() ? std::string{} : lhsTypeIt->second;
                    const std::string rhsType = rhsTypeIt == valueTypes.end() ? std::string{} : rhsTypeIt->second;
                    int rawBinary = 0; try { rawBinary = std::stoi(ins.symbol); } catch (...) { rawBinary = 0; }
                    const auto binaryTok = static_cast<zl::TokenType>(rawBinary);
                    const bool bitwise = binaryTok == zl::TokenType::BIT_AND || binaryTok == zl::TokenType::BIT_OR || binaryTok == zl::TokenType::BIT_XOR || binaryTok == zl::TokenType::SHL || binaryTok == zl::TokenType::SHR || binaryTok == zl::TokenType::USHR;
                    if (bitwise && (lhsType != "std::int64_t" || rhsType != "std::int64_t")) {
                        result.error = "MIR bitwise operator requires int operands"; return result;
                    }
                    if (resultType == "std::int64_t" && lhsType == "std::int64_t" && rhsType == "std::int64_t" && op == "pow") {
                        out << "    v" << ins.result << " = zl_safe_pow_i64(v" << ins.operand0 << ", v" << ins.operand1 << ");\n";
                    } else if (resultType == "double" && op == "pow") {
                        out << "    v" << ins.result << " = zl_safe_pow_f64(static_cast<double>(v" << ins.operand0 << "), static_cast<double>(v" << ins.operand1 << "));\n";
                    } else if (resultType == "double" && lhsType == "double" && rhsType == "double" && op == "%") {
                        out << "    v" << ins.result << " = zl_safe_mod_f64(v" << ins.operand0 << ", v" << ins.operand1 << ");\n";
                    } else if (resultType == "std::int64_t" && lhsType == "std::int64_t" && rhsType == "std::int64_t") {
                        std::string helper;
                        if (op == "+") helper = "zl_safe_add_i64";
                        else if (op == "-") helper = "zl_safe_sub_i64";
                        else if (op == "*") helper = "zl_safe_mul_i64";
                        else if (op == "/") helper = "zl_safe_div_i64";
                        else if (op == "%") helper = "zl_safe_mod_i64";
                        else if (op == "<<") helper = "zl_safe_shl_i64";
                        else if (op == ">>") {
                            int rawOp = 0;
                            try { rawOp = std::stoi(ins.symbol); } catch (...) { rawOp = 0; }
                            helper = (static_cast<zl::TokenType>(rawOp) == zl::TokenType::USHR)
                                ? "zl_safe_ushr_i64" : "zl_safe_shr_i64";
                        }
                        else if (op == "&" || op == "|" || op == "^") helper.clear();
                        if (!helper.empty()) {
                            out << "    v" << ins.result << " = " << helper << "(v" << ins.operand0 << ", v" << ins.operand1 << ");\n";
                        } else {
                            out << "    v" << ins.result << " = (v" << ins.operand0 << " " << op << " v" << ins.operand1 << ");\n";
                        }
                    } else if ((op == "+" || op == "-" || op == "*" || op == "/" || op == "%") &&
                               (resultType == "double" || lhsType == "double" || rhsType == "double")) {
                        // Double (or mixed double/int) arithmetic raises where the VM
                        // does. The casts also fix mixed `%`, which C++ would reject
                        // outright as `int64 % double`. Comparisons and && / || stay
                        // on the raw path below: C++ promotion matches the VM there.
                        std::string f64helper;
                        if (op == "+") f64helper = "zl_safe_add_f64";
                        else if (op == "-") f64helper = "zl_safe_sub_f64";
                        else if (op == "*") f64helper = "zl_safe_mul_f64";
                        else if (op == "/") f64helper = "zl_safe_div_f64";
                        else f64helper = "zl_safe_mod_f64";
                        out << "    v" << ins.result << " = " << f64helper << "(static_cast<double>(v" << ins.operand0 << "), static_cast<double>(v" << ins.operand1 << "));\n";
                    } else {
                        out << "    v" << ins.result << " = (v" << ins.operand0 << " " << op << " v" << ins.operand1 << ");\n";
                    }
                    break;
                }
                case zl::ir::Opcode::Branch:
                    if (block.successors.size() != 2) { result.error = "MIR Branch requires two successors"; return result; }
                    out << "    if (v" << ins.operand0 << ") goto L" << block.successors[0] << "; else goto L" << block.successors[1] << ";\n";
                    break;
                case zl::ir::Opcode::Jump:
                    if (block.successors.size() != 1) { result.error = "MIR Jump requires one successor"; return result; }
                    out << "    goto L" << block.successors[0] << ";\n";
                    break;
                case zl::ir::Opcode::Call: {
                    if (ins.symbol.empty()) { result.error = "MIR Call missing target"; return result; }
                    std::string safeTarget = ins.symbol;
                    for (char& c : safeTarget) if (c == '.') c = '_';
                    out << "    v" << ins.result << " = zl_mir_native_" << safeTarget << "(";
                    for (std::size_t i = 0; i < ins.operands.size(); ++i) {
                        if (i) out << ", ";
                        out << "v" << ins.operands[i];
                    }
                    out << ");\n";
                    break;
                }
                case zl::ir::Opcode::Throw: {
                    if (!ins.operand0 || !valueTypes.count(ins.operand0)) { result.error = "MIR Throw missing typed value"; return result; }
                    const auto& t = valueTypes[ins.operand0];
                    if (t == "std::int64_t") {
                        out << "    throw std::runtime_error(std::to_string(v" << ins.operand0 << "));\n";
                    } else if (t == "double") {
                        out << "    throw std::runtime_error(std::to_string(v" << ins.operand0 << "));\n";
                    } else if (t == "bool") {
                        out << "    throw std::runtime_error(v" << ins.operand0 << " ? \"true\" : \"false\");\n";
                    } else {
                        result.error = "MIR native lowering only supports primitive throw values in " + fn.name;
                        return result;
                    }
                    break;
                }
                case zl::ir::Opcode::Return:
                    if (!ins.operand0) out << "    return {};\n";
                    else out << "    return v" << ins.operand0 << ";\n";
                    break;
                case zl::ir::Opcode::MoveLocal:
                case zl::ir::Opcode::BorrowLocal:
                    out << "    v" << ins.result << " = " << localNames.at(ins.operand0) << ";\n";
                    break;
                case zl::ir::Opcode::EndBorrow:
                case zl::ir::Opcode::DropLocal:
                    break;
                default:
                    result.error = "MIR native lowering unsupported opcode " + mirOpcodeName(ins.opcode) + " in " + fn.name;
                    return result;
            }
        }
    }
    out << "}\n";
    result.success = true;
    result.source = out.str();
    return result;
}

} // namespace

CompileResult emitMirCpp(const Program& program) {
    CompileResult result;
    auto lowered = zl::ir::lowerProgram(program, /*nativeOnly=*/true);
    if (!lowered.complete) {
        result.error = lowered.diagnostics.empty() ? "MIR lowering failed" : lowered.diagnostics.front();
        return result;
    }
    lowered.module = zl::ir::optimize(lowered.module);

    std::unordered_map<std::string, const zl::ir::Function*> nativeFns;
    for (const auto& fn : lowered.module.functions) {
        if (fn.isNative) {
            if (fn.isAsync) { result.error = "MIR native lowering does not support async native functions: " + fn.name; return result; }
            auto [it, inserted] = nativeFns.emplace(fn.name, &fn);
            if (!inserted) { result.error = "MIR native lowering found duplicate function name: " + fn.name; return result; }
        }
    }

    // Require every reachable native MIR block to terminate explicitly.
    // This prevents generated non-void C++ functions from falling through
    // a label after a branch/throw path.
    for (const auto& fn : lowered.module.functions) {
        if (!fn.isNative || fn.blocks.empty()) continue;
        std::unordered_map<zl::ir::BlockId, const zl::ir::BasicBlock*> byId;
        for (const auto& block : fn.blocks) byId.emplace(block.id, &block);
        std::unordered_set<zl::ir::BlockId> seen;
        std::vector<zl::ir::BlockId> work{fn.blocks.front().id};
        while (!work.empty()) {
            const auto id = work.back();
            work.pop_back();
            if (!seen.insert(id).second) continue;
            const auto it = byId.find(id);
            if (it == byId.end()) {
                result.error = "MIR native lowering references a missing basic block in " + fn.name;
                return result;
            }
            const auto& block = *it->second;
            if (block.instructions.empty()) {
                result.error = "MIR native lowering found an empty reachable basic block in " + fn.name;
                return result;
            }
            const auto opcode = block.instructions.back().opcode;
            const bool terminates = opcode == zl::ir::Opcode::Return || opcode == zl::ir::Opcode::Throw ||
                                    opcode == zl::ir::Opcode::Jump || opcode == zl::ir::Opcode::Branch;
            if (!terminates) {
                result.error = "MIR native lowering found an unterminated reachable basic block in " + fn.name;
                return result;
            }
            for (const auto succ : block.successors) work.push_back(succ);
        }
    }

    for (const auto& fn : lowered.module.functions) {
        if (!fn.isNative) continue;
        for (const auto& block : fn.blocks) {
            for (const auto& ins : block.instructions) {
                if (ins.opcode == zl::ir::Opcode::Call && !nativeFns.count(ins.symbol)) {
                    result.error = "MIR native lowering cannot call non-native or unresolved function '" + ins.symbol + "' from '" + fn.name + "'";
                    return result;
                }
                if (ins.opcode == zl::ir::Opcode::Call) {
                    const auto* callee = nativeFns.at(ins.symbol);
                    if (callee->parameterTypes.size() != ins.operands.size()) {
                        result.error = "MIR call arity mismatch for '" + ins.symbol + "'";
                        return result;
                    }
                }
            }
        }
    }

    std::ostringstream out;
    out << "#include <cstdint>\n#include <utility>\n#include <string>\n#include <stdexcept>\n#include <limits>\n#include <cmath>\n\n";
    out << "#if defined(__GNUC__) || defined(__clang__)\n#define ZL_NATIVE_ALWAYS_INLINE inline __attribute__((always_inline))\n#else\n#define ZL_NATIVE_ALWAYS_INLINE inline\n#endif\n";
    out << "[[noreturn]] inline void zl_native_overflow(const char* message) { throw std::runtime_error(message); }\n";
    out << "#if defined(__GNUC__) || defined(__clang__)\n"
           "ZL_NATIVE_ALWAYS_INLINE std::int64_t zl_safe_add_i64(std::int64_t a, std::int64_t b) { std::int64_t r = 0; if (__builtin_add_overflow(a, b, &r)) zl_native_overflow(\"integer overflow in addition\"); return r; }\n"
           "ZL_NATIVE_ALWAYS_INLINE std::int64_t zl_safe_sub_i64(std::int64_t a, std::int64_t b) { std::int64_t r = 0; if (__builtin_sub_overflow(a, b, &r)) zl_native_overflow(\"integer overflow in subtraction\"); return r; }\n"
           "ZL_NATIVE_ALWAYS_INLINE std::int64_t zl_safe_mul_i64(std::int64_t a, std::int64_t b) { std::int64_t r = 0; if (__builtin_mul_overflow(a, b, &r)) zl_native_overflow(\"integer overflow in multiplication\"); return r; }\n"
           "#else\n"
           "ZL_NATIVE_ALWAYS_INLINE std::int64_t zl_safe_add_i64(std::int64_t a, std::int64_t b) { if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) || (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) zl_native_overflow(\"integer overflow in addition\"); return a + b; }\n"
           "ZL_NATIVE_ALWAYS_INLINE std::int64_t zl_safe_sub_i64(std::int64_t a, std::int64_t b) { if ((b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) || (b < 0 && a > std::numeric_limits<std::int64_t>::max() + b)) zl_native_overflow(\"integer overflow in subtraction\"); return a - b; }\n"
           "ZL_NATIVE_ALWAYS_INLINE std::int64_t zl_safe_mul_i64(std::int64_t a, std::int64_t b) { if (a != 0 && b != 0) { if ((a == -1 && b == std::numeric_limits<std::int64_t>::min()) || (b == -1 && a == std::numeric_limits<std::int64_t>::min())) zl_native_overflow(\"integer overflow in multiplication\"); if (a > 0) { if (b > 0 && a > std::numeric_limits<std::int64_t>::max() / b) zl_native_overflow(\"integer overflow in multiplication\"); if (b < 0 && b < std::numeric_limits<std::int64_t>::min() / a) zl_native_overflow(\"integer overflow in multiplication\"); } else { if (b > 0 && a < std::numeric_limits<std::int64_t>::min() / b) zl_native_overflow(\"integer overflow in multiplication\"); if (b < 0 && a < std::numeric_limits<std::int64_t>::max() / b) zl_native_overflow(\"integer overflow in multiplication\"); } } return a * b; }\n"
           "#endif\n";
    out << "ZL_NATIVE_ALWAYS_INLINE std::int64_t zl_safe_div_i64(std::int64_t a, std::int64_t b) { if (b == 0) throw std::runtime_error(\"division by zero\"); if (a == std::numeric_limits<std::int64_t>::min() && b == -1) throw std::runtime_error(\"integer overflow in division\"); return a / b; }\n";
    out << "ZL_NATIVE_ALWAYS_INLINE std::int64_t zl_safe_mod_i64(std::int64_t a, std::int64_t b) { if (b == 0) throw std::runtime_error(\"modulo by zero\"); if (a == std::numeric_limits<std::int64_t>::min() && b == -1) return 0; return a % b; }\n";
    out << "ZL_NATIVE_ALWAYS_INLINE std::int64_t zl_safe_shl_i64(std::int64_t a, std::int64_t b) { if (b < 0 || b >= 64) throw std::runtime_error(\"shift count must be in the range 0..63\"); return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) << static_cast<unsigned>(b)); }\n";
    out << "ZL_NATIVE_ALWAYS_INLINE std::int64_t zl_safe_shr_i64(std::int64_t a, std::int64_t b) { if (b < 0 || b >= 64) throw std::runtime_error(\"shift count must be in the range 0..63\"); const auto s = static_cast<unsigned>(b); const auto ux = static_cast<std::uint64_t>(a); std::uint64_t v = ux >> s; if (a < 0 && s != 0) v |= (~std::uint64_t{0}) << (64 - s); return static_cast<std::int64_t>(v); }\n";
    out << "ZL_NATIVE_ALWAYS_INLINE std::int64_t zl_safe_ushr_i64(std::int64_t a, std::int64_t b) { if (b < 0 || b >= 64) throw std::runtime_error(\"shift count must be in the range 0..63\"); return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) >> static_cast<unsigned>(b)); }\n";
    out << "ZL_NATIVE_ALWAYS_INLINE std::int64_t zl_safe_pow_i64(std::int64_t base, std::int64_t exponent) { if (exponent < 0) throw std::runtime_error(\"negative integer exponent requires floating-point result\"); std::int64_t result = 1; std::int64_t factor = base; std::uint64_t n = static_cast<std::uint64_t>(exponent); while (n != 0) { if (n & 1u) result = zl_safe_mul_i64(result, factor); n >>= 1u; if (n != 0) factor = zl_safe_mul_i64(factor, factor); } return result; }\n\n";
    // Double arithmetic raises exactly where the VM does: a non-finite
    // result is an error, never a silent inf/NaN (see checkedDoubleResult
    // in src/vm/vm.cpp - the messages below must stay identical to the
    // VM's). fmod cannot overflow on finite inputs, so it only needs the
    // zero check.
    out << "ZL_NATIVE_ALWAYS_INLINE double zl_safe_add_f64(double a, double b) { const double r = a + b; if (std::isinf(r)) zl_native_overflow(\"floating-point overflow in addition\"); if (std::isnan(r)) zl_native_overflow(\"invalid floating-point result in addition\"); return r; }\n";
    out << "ZL_NATIVE_ALWAYS_INLINE double zl_safe_sub_f64(double a, double b) { const double r = a - b; if (std::isinf(r)) zl_native_overflow(\"floating-point overflow in subtraction\"); if (std::isnan(r)) zl_native_overflow(\"invalid floating-point result in subtraction\"); return r; }\n";
    out << "ZL_NATIVE_ALWAYS_INLINE double zl_safe_mul_f64(double a, double b) { const double r = a * b; if (std::isinf(r)) zl_native_overflow(\"floating-point overflow in multiplication\"); if (std::isnan(r)) zl_native_overflow(\"invalid floating-point result in multiplication\"); return r; }\n";
    out << "ZL_NATIVE_ALWAYS_INLINE double zl_safe_div_f64(double a, double b) { if (b == 0.0) throw std::runtime_error(\"division by zero\"); const double r = a / b; if (std::isinf(r)) zl_native_overflow(\"floating-point overflow in division\"); if (std::isnan(r)) zl_native_overflow(\"invalid floating-point result in division\"); return r; }\n";
    out << "ZL_NATIVE_ALWAYS_INLINE double zl_safe_mod_f64(double a, double b) { if (b == 0.0) throw std::runtime_error(\"modulo by zero\"); return std::fmod(a, b); }\n";
    out << "ZL_NATIVE_ALWAYS_INLINE double zl_safe_pow_f64(double a, double b) { const double r = std::pow(a, b); if (std::isinf(r)) zl_native_overflow(\"floating-point overflow in exponentiation\"); if (std::isnan(r)) zl_native_overflow(\"invalid floating-point result in exponentiation\"); return r; }\n";
    out << "#include \"zl/compiler/native_abi.hpp\"\n\n";
    std::size_t emitted = 0;
    // Emit prototypes first so MIR calls can target functions declared later.
    for (const auto& fn : lowered.module.functions) {
        if (!fn.isNative || !mirTypeSupported(fn.returnType)) continue;
        std::string safeName = fn.name;
        for (char& c : safeName) if (c == '.') c = '_';
        out << "extern \"C\" " << mirCppType(fn.returnType) << " zl_mir_native_" << safeName << "(";
        for (std::size_t i = 0; i < fn.parameterTypes.size(); ++i) {
            if (i) out << ", ";
            out << mirCppType(fn.parameterTypes[i]) << " p" << i;
        }
        out << ");\n";
    }
    out << "\n";
    for (const auto& fn : lowered.module.functions) {
        // Native backend currently accepts the primitive, synchronous MIR subset.
        if (fn.isNative) {
            auto one = emitMirFunction(fn, nativeFns);
            if (!one.success) { result.error = one.error; return result; }
            out << one.source << "\n";
            ++emitted;
        }
    }
    if (!emitted) { result.error = "MIR native lowering found no functions"; return result; }

    // Generate ABI wrappers for the MIR-native functions. Ownership tags are
    // preserved in the export descriptor and checked at the existing boundary.
    std::size_t exportIndex = 0;
    std::vector<const zl::ir::Function*> exported;
    for (const auto& fn : lowered.module.functions) if (fn.isNative) exported.push_back(&fn);
    for (const auto* fn : exported) {
        std::string safeName = fn->name;
        for (char& c : safeName) if (c == '.') c = '_';
        out << "static const ZlNativeTypeTag zl_mir_native_params_" << exportIndex << "[] = {";
        for (std::size_t i = 0; i < fn->parameterTypes.size(); ++i) {
            if (i) out << ", ";
            const auto& t = fn->parameterTypes[i];
            out << (t == "int" ? "ZL_NATIVE_I64" : (t == "float" || t == "decimal" ? "ZL_NATIVE_F64" : "ZL_NATIVE_BOOL"));
        }
        out << "};\n";
        out << "static const ZlNativeOwnershipTag zl_mir_native_ownership_" << exportIndex << "[] = {";
        for (std::size_t i = 0; i < fn->parameterOwnership.size(); ++i) {
            if (i) out << ", ";
            switch (fn->parameterOwnership[i]) {
                case OwnershipKind::BORROW: case OwnershipKind::SHARED: out << "ZL_NATIVE_OWNERSHIP_BORROWED"; break;
                case OwnershipKind::OWNED: out << "ZL_NATIVE_OWNERSHIP_OWNED"; break;
                default: out << "ZL_NATIVE_OWNERSHIP_NONE"; break;
            }
        }
        out << "};\n";
        const auto retTag = fn->returnType == "int" ? "ZL_NATIVE_I64" : (fn->returnType == "float" || fn->returnType == "decimal" ? "ZL_NATIVE_F64" : "ZL_NATIVE_BOOL");
        const auto retField = std::string(retTag) == "ZL_NATIVE_I64" ? "i64" : (std::string(retTag) == "ZL_NATIVE_F64" ? "f64" : "boolean");
        out << "static std::int32_t zl_mir_native_invoke_" << exportIndex << "(const ZlNativeValue* args, std::uint32_t argc, ZlNativeValue* result, char* errorMessage, std::uint32_t errorCapacity) {\n";
        out << "    if (!result || argc != " << fn->parameterTypes.size() << ") return 1;\n";
        for (std::size_t i = 0; i < fn->parameterTypes.size(); ++i) {
            const auto tag = fn->parameterTypes[i] == "int" ? "ZL_NATIVE_I64" : (fn->parameterTypes[i] == "float" || fn->parameterTypes[i] == "decimal" ? "ZL_NATIVE_F64" : "ZL_NATIVE_BOOL");
            out << "    if (!args || args[" << i << "].tag != " << tag << ") return 1;\n";
        }
        out << "    try { result->tag = " << retTag << "; result->data." << retField << " = zl_mir_native_" << safeName << "(";
        for (std::size_t i = 0; i < fn->parameterTypes.size(); ++i) {
            if (i) out << ", ";
            const auto field = fn->parameterTypes[i] == "int" ? "i64" : (fn->parameterTypes[i] == "float" || fn->parameterTypes[i] == "decimal" ? "f64" : "boolean");
            out << "args[" << i << "].data." << field;
        }
        out << "); return 0; } catch (const std::exception& e) { if (errorMessage && errorCapacity) { const std::string msg = e.what(); const std::size_t n = std::min<std::size_t>(msg.size(), errorCapacity - 1); for (std::size_t j = 0; j < n; ++j) errorMessage[j] = msg[j]; errorMessage[n] = 0; } return 1; } catch (...) { if (errorMessage && errorCapacity) errorMessage[0] = 0; return 1; }\n}" << "\n";
        ++exportIndex;
    }
    out << "extern \"C\" const ZlNativeExport zl_mir_native_exports[] = {\n";
    for (std::size_t i = 0; i < exported.size(); ++i) {
        const auto* fn = exported[i];
        const auto retTag = fn->returnType == "int" ? "ZL_NATIVE_I64" : (fn->returnType == "float" || fn->returnType == "decimal" ? "ZL_NATIVE_F64" : "ZL_NATIVE_BOOL");
        out << "    {\"" << fn->name << "\", " << fn->parameterTypes.size() << ", zl_mir_native_params_" << i << ", " << retTag << ", zl_mir_native_invoke_" << i << ", zl_mir_native_ownership_" << i << ", ";
        switch (fn->returnOwnership) { case OwnershipKind::BORROW: case OwnershipKind::SHARED: out << "ZL_NATIVE_OWNERSHIP_BORROWED"; break; case OwnershipKind::OWNED: out << "ZL_NATIVE_OWNERSHIP_OWNED"; break; default: out << "ZL_NATIVE_OWNERSHIP_NONE"; break; }
        out << "}" << (i + 1 == exported.size() ? "\n" : ",\n");
    }
    out << "};\nextern \"C\" const std::uint32_t zl_mir_native_export_count = " << exported.size() << ";\n";
    result.success = true;
    result.source = out.str();
    return result;
}

}
