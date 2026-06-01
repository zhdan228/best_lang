#include "lexer.hpp"
#include <unordered_map>
#include <cctype>

namespace Lexer {

static const std::unordered_map<std::string, TokenKind> KEYWORDS = {
    {"var",       TokenKind::Var},      {"val",       TokenKind::Val},
    {"fun",       TokenKind::Fun},      {"return",    TokenKind::Return},
    {"if",        TokenKind::If},       {"else",      TokenKind::Else},
    {"while",     TokenKind::While},    {"for",       TokenKind::For},
    {"in",        TokenKind::In},       {"break",     TokenKind::Break},
    {"continue",  TokenKind::Continue}, {"struct",    TokenKind::Struct},
    {"impl",      TokenKind::Impl},     {"type",      TokenKind::Type},
    {"namespace", TokenKind::Namespace},{"as",        TokenKind::As},
    {"true",      TokenKind::True},     {"false",     TokenKind::False},
    {"void",      TokenKind::Void},     {"and",       TokenKind::And},
    {"or",        TokenKind::Or},       {"not",       TokenKind::Not},
    {"null",      TokenKind::Null},
    {"int8",      TokenKind::Int8},     {"int16",     TokenKind::Int16},
    {"int32",     TokenKind::Int32},    {"int64",     TokenKind::Int64},
    {"uint8",     TokenKind::UInt8},    {"uint16",    TokenKind::UInt16},
    {"uint32",    TokenKind::UInt32},   {"uint64",    TokenKind::UInt64},
    {"float32",   TokenKind::Float32},  {"float64",   TokenKind::Float64},
    {"bool",      TokenKind::Bool},     {"string",    TokenKind::String},
    {"char",      TokenKind::Char},
};

struct L {
    const std::string& src;
    const std::string& filename;
    size_t   pos  = 0;
    uint32_t line = 1;
    uint32_t col  = 1;
    std::optional<LexError> err;

    char peek(size_t off = 0) const {
        size_t i = pos + off;
        return i < src.size() ? src[i] : '\0';
    }
    char advance() {
        char c = src[pos++];
        if (c == '\n') { ++line; col = 1; }
        else            { ++col; }
        return c;
    }
    bool at_end()    const { return pos >= src.size(); }
    bool has_error() const { return err.has_value(); }

    void set_error(const std::string& msg) {
        if (!err) err = LexError{filename, line, col, msg};
    }
};

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char parse_escape(L& l) {
    if (l.at_end()) { l.set_error("unterminated escape sequence"); return '\0'; }
    char e = l.advance();
    switch (e) {
    case 'n':  return '\n';
    case 't':  return '\t';
    case 'r':  return '\r';
    case '"':  return '"';
    case '\\': return '\\';
    case '0':  return '\0';
    case 'x': {
        if (l.at_end()) { l.set_error("incomplete \\xHH escape"); return '\0'; }
        char h1 = l.advance();
        if (l.at_end()) { l.set_error("incomplete \\xHH escape"); return '\0'; }
        char h2 = l.advance();
        int v1 = hex_val(h1), v2 = hex_val(h2);
        if (v1 < 0 || v2 < 0) { l.set_error("invalid hex digit in \\xHH escape"); return '\0'; }
        return (char)(v1 * 16 + v2);
    }
    default:
        l.set_error(std::string("unknown escape sequence '\\") + e + "'");
        return '\0';
    }
}

static Token scan_string_token(L& l, uint32_t line, uint32_t col) {
    std::string result;
    while (true) {
        if (l.at_end() || l.peek() == '\n') { l.set_error("unterminated string literal"); return {}; }
        char c = l.advance();
        if (c == '"') break;
        char ec = (c == '\\') ? parse_escape(l) : c;
        if (l.has_error()) return {};
        result += ec;
    }
    Token tok;
    tok.kind    = TokenKind::StringLit;
    tok.line    = line; tok.col = col;
    tok.str_val = result;
    tok.lexeme  = '"' + result + '"';
    return tok;
}

enum class IntSuffix { None, I8, I16, I32, I64, U8, U16, U32, U64 };

static std::string int_suffix_str(IntSuffix s) {
    switch (s) {
    case IntSuffix::I8:  return "i8";  case IntSuffix::I16: return "i16";
    case IntSuffix::I32: return "i32"; case IntSuffix::I64: return "i64";
    case IntSuffix::U8:  return "u8";  case IntSuffix::U16: return "u16";
    case IntSuffix::U32: return "u32"; case IntSuffix::U64: return "u64";
    default: return "";
    }
}

static std::string scan_int_suffix(L& l) {
    struct Entry { const char* str; IntSuffix kind; };
    static const Entry TABLE[] = {
        {"i64", IntSuffix::I64}, {"i32", IntSuffix::I32},
        {"i16", IntSuffix::I16}, {"i8",  IntSuffix::I8},
        {"u64", IntSuffix::U64}, {"u32", IntSuffix::U32},
        {"u16", IntSuffix::U16}, {"u8",  IntSuffix::U8},
    };
    for (auto& e : TABLE) {
        size_t len = std::string_view(e.str).size();
        if (l.pos + len > l.src.size()) continue;
        if (l.src.substr(l.pos, len) != e.str) continue;
        size_t after = l.pos + len;
        if (after < l.src.size() && (std::isalnum(l.src[after]) || l.src[after] == '_'))
            continue;
        for (size_t k = 0; k < len; ++k) l.advance();
        return int_suffix_str(e.kind);
    }
    return "";
}

static int detect_base(L& l, std::string& raw) {
    if (raw != "0" || l.at_end()) return 10;
    if (l.peek() == 'x' || l.peek() == 'X') { raw += l.advance(); return 16; }
    if (l.peek() == 'b' || l.peek() == 'B') { raw += l.advance(); return 2; }
    return 10;
}

static void scan_digits(L& l, std::string& raw, int base) {
    char last = raw.back();
    while (!l.at_end()) {
        char c = l.peek();
        if (c == '_') {
            if (last == '_') { l.set_error("consecutive '_' in number literal"); return; }
            if (l.pos + 1 >= l.src.size()) { l.set_error("trailing '_' in number literal"); return; }
            l.advance(); last = '_'; continue;
        }
        if (base == 16 && hex_val(c) >= 0) { last = c; raw += l.advance(); continue; }
        if (base == 2  && (c == '0' || c == '1')) { last = c; raw += l.advance(); continue; }
        if (base == 10 && std::isdigit(c))  { last = c; raw += l.advance(); continue; }
        break;
    }
    if (last == '_') l.set_error("trailing '_' in number literal");
}

static bool scan_float_decimal(L& l, std::string& raw) {
    bool is_float = false;
    if (!l.at_end() && l.peek() == '.' &&
        !(l.pos + 1 < l.src.size() && l.src[l.pos + 1] == '.')) {
        is_float = true;
        raw += l.advance();
        while (!l.at_end() && std::isdigit(l.peek())) raw += l.advance();
    }
    if (!l.at_end() && (l.peek() == 'e' || l.peek() == 'E')) {
        is_float = true;
        raw += l.advance();
        if (!l.at_end() && (l.peek() == '+' || l.peek() == '-')) raw += l.advance();
        if (l.at_end() || !std::isdigit(l.peek())) {
            l.set_error("expected digits after exponent");
            return false;
        }
        while (!l.at_end() && std::isdigit(l.peek())) raw += l.advance();
    }
    return is_float;
}

static Token make_float_token(L& l, const std::string& raw, uint32_t line, uint32_t col) {
    std::string suf;
    if (!l.at_end() && l.peek() == 'f' && l.pos + 2 < l.src.size() &&
        (l.src.substr(l.pos, 3) == "f32" || l.src.substr(l.pos, 3) == "f64")) {
        suf += l.advance(); suf += l.advance(); suf += l.advance();
    }
    Token tok;
    tok.kind = TokenKind::FloatLit; tok.line = line; tok.col = col;
    tok.suffix = suf; tok.lexeme = raw + suf;
    try {
        double d = std::stod(raw);
        if (suf == "f32") tok.num.f32 = (float)d;
        else               tok.num.f64 = d;
    } catch (...) {
        l.set_error("invalid float literal '" + raw + "'");
        return {};
    }
    return tok;
}

static Token make_int_token(L& l, const std::string& raw, int base, uint32_t line, uint32_t col) {
    Token tok;
    tok.kind   = TokenKind::IntLit; tok.line = line; tok.col = col;
    tok.suffix = scan_int_suffix(l); tok.lexeme = raw + tok.suffix;
    try {
        uint64_t u = (base == 10) ? std::stoull(raw, nullptr, 10)
                   : (base == 16) ? std::stoull(raw.substr(2), nullptr, 16)
                   :                std::stoull(raw.substr(2), nullptr, 2);
        auto& s = tok.suffix;
        if      (s == "i8")  tok.num.i8  = (int8_t)u;
        else if (s == "i16") tok.num.i16 = (int16_t)u;
        else if (s == "i32") tok.num.i32 = (int32_t)u;
        else if (s == "i64") tok.num.i64 = (int64_t)u;
        else if (s == "u8")  tok.num.u8  = (uint8_t)u;
        else if (s == "u16") tok.num.u16 = (uint16_t)u;
        else if (s == "u32") tok.num.u32 = (uint32_t)u;
        else if (s == "u64") tok.num.u64 = u;
        else                 tok.num.i64 = (int64_t)u;
    } catch (...) {
        l.set_error("integer literal out of range: '" + raw + "'");
        return {};
    }
    return tok;
}

static Token scan_number(L& l, char first, uint32_t line, uint32_t col) {
    std::string raw(1, first);
    int base = detect_base(l, raw);
    if (base == 16 && (l.at_end() || hex_val(l.peek()) < 0)) {
        l.set_error("expected hex digit after '0x'");
        return {};
    }
    if (base == 2  && (l.at_end() || (l.peek() != '0' && l.peek() != '1'))) {
        l.set_error("expected binary digit after '0b'");
        return {};
    }
    scan_digits(l, raw, base);
    if (l.has_error()) return {};
    if (base == 10 && scan_float_decimal(l, raw)) {
        if (l.has_error()) return {};
        return make_float_token(l, raw, line, col);
    }
    return make_int_token(l, raw, base, line, col);
}

static Token scan_char_token(L& l, uint32_t line, uint32_t col) {
    if (l.at_end() || l.peek() == '\n') {
        l.set_error("unterminated char literal");
        return {};
    }
    char c = l.advance();
    char ch = (c == '\\') ? parse_escape(l) : c;
    if (l.has_error()) return {};
    if (l.at_end() || l.peek() != '\'') {
        l.set_error("char literal must contain exactly one character");
        return {};
    }
    l.advance();
    Token tok;
    tok.kind    = TokenKind::CharLit;
    tok.line    = line; tok.col = col;
    tok.num.u8  = (uint8_t)ch;
    tok.lexeme  = std::string("'") + ch + "'";
    return tok;
}

static bool skip_comment(L& l, char c) {
    if (c == '/' && l.peek() == '/') {
        while (!l.at_end() && l.peek() != '\n') l.advance();
        return true;
    }
    if (c == '/' && l.peek() == '*') {
        l.advance();
        while (!l.at_end()) {
            char cc = l.advance();
            if (cc == '*' && l.peek() == '/') { l.advance(); break; }
        }
        return true;
    }
    return false;
}

static Token scan_ident_or_keyword(L& l, char first, uint32_t line, uint32_t col) {
    std::string ident(1, first);
    while (!l.at_end() && (std::isalnum(l.peek()) || l.peek() == '_'))
        ident += l.advance();
    auto it = KEYWORDS.find(ident);
    Token tok;
    tok.line   = line; tok.col = col;
    tok.kind   = (it != KEYWORDS.end()) ? it->second : TokenKind::Ident;
    tok.lexeme = ident;
    return tok;
}

static Token scan_operator(L& l, char c, uint32_t line, uint32_t col) {
    auto peek2 = [&](char next) { return !l.at_end() && l.peek() == next; };
    Token tok;
    tok.line = line; tok.col = col; tok.lexeme = std::string(1, c);
    switch (c) {
    case '+': tok.kind = TokenKind::Plus;      break;
    case '-': tok.kind = TokenKind::Minus;     break;
    case '*': tok.kind = TokenKind::Star;      break;
    case '/': tok.kind = TokenKind::Slash;     break;
    case '%': tok.kind = TokenKind::Percent;   break;
    case ',': tok.kind = TokenKind::Comma;     break;
    case ';': tok.kind = TokenKind::Semicolon; break;
    case '(': tok.kind = TokenKind::LParen;    break;
    case ')': tok.kind = TokenKind::RParen;    break;
    case '[': tok.kind = TokenKind::LBracket;  break;
    case ']': tok.kind = TokenKind::RBracket;  break;
    case '{': tok.kind = TokenKind::LBrace;    break;
    case '}': tok.kind = TokenKind::RBrace;    break;
    case '?': tok.kind = TokenKind::Question;  break;
    case '.':
        if (peek2('.')) { l.advance(); tok.kind = TokenKind::DotDot;    tok.lexeme = ".."; }
        else             {              tok.kind = TokenKind::Dot;                           }
        break;
    case '=':
        if (peek2('=')) { l.advance(); tok.kind = TokenKind::EqEq;      tok.lexeme = "=="; }
        else             {              tok.kind = TokenKind::Assign;                        }
        break;
    case '!':
        if (peek2('=')) { l.advance(); tok.kind = TokenKind::BangEq;    tok.lexeme = "!="; }
        else { l.set_error(std::string("unexpected character '") + c + "'"); return {}; }
        break;
    case '<':
        if (peek2('=')) { l.advance(); tok.kind = TokenKind::LtEq;      tok.lexeme = "<="; }
        else             {              tok.kind = TokenKind::Lt;                            }
        break;
    case '>':
        if (peek2('=')) { l.advance(); tok.kind = TokenKind::GtEq;      tok.lexeme = ">="; }
        else             {              tok.kind = TokenKind::Gt;                            }
        break;
    case ':':
        if (peek2(':')) { l.advance(); tok.kind = TokenKind::ColonColon; tok.lexeme = "::"; }
        else             {              tok.kind = TokenKind::Colon;                          }
        break;
    default:
        l.set_error(std::string("unexpected character '") + c + "'");
        return {};
    }
    return tok;
}

std::expected<std::vector<Token>, LexError> tokenize(const std::string& source, const std::string& filename) {
    L l{source, filename};
    std::vector<Token> tokens;
    while (!l.at_end() && !l.has_error()) {
        while (!l.at_end() && std::isspace((unsigned char)l.peek())) l.advance();
        if (l.at_end()) break;
        uint32_t tline = l.line, tcol = l.col;
        char c = l.advance();
        if ((unsigned char)c > 127) { l.set_error("non-ASCII character in source"); break; }
        if (skip_comment(l, c)) continue;
        switch (c) {
        case '"':
            tokens.push_back(scan_string_token(l, tline, tcol)); break;
        case '\'':
            tokens.push_back(scan_char_token(l, tline, tcol)); break;
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            tokens.push_back(scan_number(l, c, tline, tcol)); break;
        default:
            if (std::isalpha((unsigned char)c) || c == '_')
                tokens.push_back(scan_ident_or_keyword(l, c, tline, tcol));
            else
                tokens.push_back(scan_operator(l, c, tline, tcol));
        }
    }
    if (l.err) return std::unexpected(*l.err);
    Token eof;
    eof.kind = TokenKind::Eof; eof.line = l.line; eof.col = l.col; eof.lexeme = "<eof>";
    tokens.push_back(eof);
    return tokens;
}

std::string token_kind_to_string(TokenKind k) {
    switch (k) {
#define C(x) case TokenKind::x: return #x;
    C(IntLit) C(FloatLit) C(StringLit) C(CharLit)
    C(Var) C(Val) C(Fun) C(Return) C(If) C(Else) C(While) C(For) C(In) C(Break) C(Continue)
    C(Struct) C(Impl) C(Type) C(Namespace) C(As) C(True) C(False) C(Void) C(And) C(Or) C(Not)
    C(Null)
    C(Int8) C(Int16) C(Int32) C(Int64)
    C(UInt8) C(UInt16) C(UInt32) C(UInt64)
    C(Float32) C(Float64) C(Bool) C(String) C(Char)
    C(Ident)
    C(Plus) C(Minus) C(Star) C(Slash) C(Percent)
    C(EqEq) C(BangEq) C(Lt) C(LtEq) C(Gt) C(GtEq)
    C(Assign) C(Dot) C(DotDot) C(ColonColon) C(Colon) C(Question)
    C(LParen) C(RParen) C(LBracket) C(RBracket) C(LBrace) C(RBrace)
    C(Comma) C(Semicolon) C(Eof)
#undef C
    default: return "?";
    }
}

void dump_tokens(const std::vector<Token>& tokens, std::ostream& out) {
    for (const auto& t : tokens)
        out << t.line << ":" << t.col << "\t"
            << token_kind_to_string(t.kind) << "\t'" << t.lexeme << "'\n";
}

}
