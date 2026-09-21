#include "header_parser.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace zl_bind {
namespace {

enum class TokenKind { Identifier, String, Symbol, End };
struct Token { TokenKind kind; std::string text; };

class Lexer {
public:
    explicit Lexer(std::string_view text) : text_(text) {}
    std::vector<Token> all() {
        std::vector<Token> out;
        for (;;) {
            auto t = next();
            out.push_back(t);
            if (t.kind == TokenKind::End) return out;
        }
    }
private:
    std::string_view text_; size_t pos_{0};
    Token next() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        if (pos_ >= text_.size()) return {TokenKind::End, {}};
        char c = text_[pos_];
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t start = pos_++;
            while (pos_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '_')) ++pos_;
            return {TokenKind::Identifier, std::string(text_.substr(start, pos_ - start))};
        }
        if (c == '"') {
            size_t start = ++pos_;
            while (pos_ < text_.size() && text_[pos_] != '"') ++pos_;
            if (pos_ >= text_.size()) throw std::runtime_error("unterminated string literal in header");
            std::string value(text_.substr(start, pos_ - start)); ++pos_;
            return {TokenKind::String, std::move(value)};
        }
        if (c == ':' && pos_ + 1 < text_.size() && text_[pos_ + 1] == ':') { pos_ += 2; return {TokenKind::Symbol, "::"}; }
        ++pos_; return {TokenKind::Symbol, std::string(1, c)};
    }
};

class Cursor {
public:
    explicit Cursor(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}
    const Token& peek(size_t offset = 0) const { return tokens_[std::min(index_ + offset, tokens_.size() - 1)]; }
    bool take(const char* text) { if (peek().text == text) { ++index_; return true; } return false; }
    Token require(TokenKind kind, const char* what) { if (peek().kind != kind) throw std::runtime_error(std::string("expected ") + what + ", got " + peek().text); return tokens_[index_++]; }
    void requireText(const char* text) { if (!take(text)) throw std::runtime_error(std::string("expected '") + text + "', got '" + peek().text + "'"); }
    bool eof() const { return peek().kind == TokenKind::End; }
private:
    std::vector<Token> tokens_; size_t index_{0};
};

std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string parseType(Cursor& c) {
    std::string out = c.require(TokenKind::Identifier, "type").text;
    if (out == "const" || out == "unsigned" || out == "long") {
        out += " "; out += c.require(TokenKind::Identifier, "type name").text;
    }
    while (c.take("::")) { out += "::"; out += c.require(TokenKind::Identifier, "type name").text; }
    while (c.take("*")) out += "*";
    return out;
}

std::vector<Param> parseParams(Cursor& c) {
    std::vector<Param> out;
    if (c.take("void")) { c.requireText(")"); return out; }
    if (c.take(")")) return out;
    for (;;) {
        Param p;
        p.type = parseType(c);
        p.name = c.require(TokenKind::Identifier, "parameter name").text;
        out.push_back(std::move(p));
        if (c.take(")")) return out;
        c.requireText(",");
    }
}

void parseAnnotationLine(const std::string& line, std::string& ownership, std::string& errors) {
    auto body = trim(line.substr(2));
    if (body.rfind("zl:", 0) != 0) return;
    body = trim(body.substr(3));
    auto eq = body.find('=');
    if (eq == std::string::npos) throw std::runtime_error("invalid zl annotation: " + line);
    auto key = trim(body.substr(0, eq)); auto value = trim(body.substr(eq + 1));
    if (key == "ownership") ownership = value;
    else if (key == "errors") errors = value;
}

std::string mapType(const std::string& t) {
    auto x = trim(t);
    if (x == "void") return "void";
    // Fixed-width integers only: `long`, `long long` and their spellings are
    // deliberately absent because their width is target-dependent, and the
    // generator promises a stable mapping, not a platform guess.
    if (x == "int" || x == "int8_t" || x == "uint8_t" || x == "int16_t" || x == "uint16_t" ||
        x == "int32_t" || x == "int64_t" || x == "uint32_t" || x == "uint64_t") return "int";
    if (x == "double" || x == "float") return "double";
    if (x == "bool") return "bool";
    if (x == "const char*" || x == "char*") return "string";
    throw std::runtime_error("unsupported native type: " + x);
}

void validate(const std::vector<Param>& params, const std::string& ret) {
    mapType(ret); for (const auto& p : params) mapType(p.type);
}

bool isClassStart(const std::string& line) {
    return line.rfind("class ", 0) == 0 || line.rfind("struct ", 0) == 0;
}

NativeClass parseClassHeader(const std::string& line) {
    Cursor c(Lexer(line).all());
    auto kind = c.require(TokenKind::Identifier, "class or struct");
    if (kind.text != "class" && kind.text != "struct") throw std::runtime_error("invalid native class declaration");
    NativeClass out; out.kind = kind.text; out.name = c.require(TokenKind::Identifier, "class name").text;
    c.requireText("{"); return out;
}

void parseTopLevelDecl(const std::string& line, std::string& pendingOwnership, std::string& pendingErrors, std::vector<Fn>& out) {
    Cursor c(Lexer(line).all());
    if (c.take("extern")) { auto lang = c.require(TokenKind::String, "language string"); if (lang.text != "C") throw std::runtime_error("only extern \"C\" is supported"); }
    auto ret = parseType(c);
    auto name = c.require(TokenKind::Identifier, "func name").text;
    c.requireText("("); auto params = parseParams(c); c.requireText(";");
    validate(params, ret);
    if (pendingOwnership != "borrowed" && pendingOwnership != "owned") throw std::runtime_error("ownership must be borrowed or owned");
    if (pendingErrors != "exception" && pendingErrors != "errno") throw std::runtime_error("errors must be exception or errno");
    out.push_back({ret, name, std::move(params), pendingOwnership, pendingErrors});
    pendingOwnership = "borrowed"; pendingErrors = "exception";
}

}

std::string mapNativeType(const std::string& type) { return mapType(type); }

ParsedHeader parseHeaderFile(const std::string& path) {
    std::ifstream in(path); if (!in) throw std::runtime_error("cannot open header: " + path);
    std::vector<std::string> lines; std::string line; while (std::getline(in, line)) lines.push_back(line);
    ParsedHeader result;
    std::string pendingOwnership = "borrowed", pendingErrors = "exception";
    for (size_t i = 0; i < lines.size(); ++i) {
        auto t = trim(lines[i]); if (t.empty() || t[0] == '#') continue;
        if (t.rfind("//", 0) == 0) { parseAnnotationLine(t, pendingOwnership, pendingErrors); continue; }
        if (!isClassStart(t)) { if (t.back() == ';') parseTopLevelDecl(t, pendingOwnership, pendingErrors, result.functions); continue; }
        auto current = parseClassHeader(t);
        std::string classOwnership = pendingOwnership == "borrowed" ? "unique" : pendingOwnership;
        std::string classErrors = pendingErrors;
        pendingOwnership = "borrowed"; pendingErrors = "exception";
        if (classOwnership != "unique" && classOwnership != "shared") throw std::runtime_error("class ownership must be unique or shared");
        if (classErrors != "exception" && classErrors != "errno") throw std::runtime_error("class errors must be exception or errno");
        bool closed = false;
        while (++i < lines.size()) {
            t = trim(lines[i]); if (t.empty() || t[0] == '#') continue;
            if (t == "public:" || t == "private:" || t == "protected:") continue;
            if (t == "};" || t == "}") { closed = true; break; }
            if (t.rfind("//", 0) == 0) { parseAnnotationLine(t, pendingOwnership, pendingErrors); continue; }
            Cursor c(Lexer(t).all());
            if (c.take("~")) {
                auto name = c.require(TokenKind::Identifier, "destructor name").text; c.requireText("("); c.requireText(")"); c.requireText(";");
                if (name != current.name) throw std::runtime_error("destructor name mismatch in " + current.name); current.hasDestructor = true; continue;
            }
            auto first = c.require(TokenKind::Identifier, "declaration");
            if (first.text == current.name && c.peek().text == "(") {
                c.requireText("("); current.constructorParams = parseParams(c); c.requireText(";");
                if (current.hasConstructor) throw std::runtime_error("overloaded constructors are not supported for " + current.name);
                validate(current.constructorParams, "void"); current.hasConstructor = true; continue;
            }
            std::string ret = first.text;
            while (c.take("::")) { ret += "::"; ret += c.require(TokenKind::Identifier, "return type").text; }
            while (c.take("*")) ret += "*";
            auto name = c.require(TokenKind::Identifier, "member name").text;
            if (!c.take("(")) {
                c.requireText(";");
                // Data member: classes ignore them (methods only), but a plain
                // data struct's schema is exactly this field list.
                current.fields.push_back({ret, name});
                continue;
            }
            auto params = parseParams(c); bool isConst = c.take("const"); c.requireText(";");
            validate(params, ret); current.methods.push_back({ret, name, std::move(params), isConst, false});
        }
        if (!closed) throw std::runtime_error("unterminated native class " + current.name);
        if (current.kind == "struct" && !current.hasConstructor && !current.hasDestructor && current.methods.empty()) {
            // A plain-data C struct: bind it field by field. Pointers, arrays
            // and non-scalars never reach here as valid field types - they are
            // refused now, at the schema, rather than guessed at later.
            if (current.fields.empty())
                throw std::runtime_error("native struct " + current.name + " has no bindable fields");
            NativeStruct out;
            out.name = current.name;
            for (const auto& field : current.fields) {
                const auto mapped = mapType(field.type);
                if (mapped != "int" && mapped != "double" && mapped != "bool")
                    throw std::runtime_error("unsupported struct field type: " + field.type + " (struct fields must be scalar ints, doubles or bools)");
                out.fields.push_back(field);
            }
            result.structs.push_back(std::move(out));
            continue;
        }
        if (!current.hasConstructor) throw std::runtime_error("native class " + current.name + " requires an explicit constructor");
        current.ownership = classOwnership; current.errors = classErrors; result.classes.push_back(std::move(current));
    }
    if (result.functions.empty() && result.classes.empty() && result.structs.empty()) throw std::runtime_error("no bindable declarations found in " + path);
    return result;
}

} // namespace zl_bind
