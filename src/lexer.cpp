#include "lexer.hpp"
#include <unordered_map>
#include <sstream>
#include <cctype>
#include <stdexcept>

namespace Lexer {

// Таблица ключевых слов — идентификаторы из этого набора не могут быть именами
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
    // type names
    {"int8",      TokenKind::Int8},     {"int16",     TokenKind::Int16},
    {"int32",     TokenKind::Int32},    {"int64",     TokenKind::Int64},
    {"uint8",     TokenKind::UInt8},    {"uint16",    TokenKind::UInt16},
    {"uint32",    TokenKind::UInt32},   {"uint64",    TokenKind::UInt64},
    {"float32",   TokenKind::Float32},  {"float64",   TokenKind::Float64},
    {"bool",      TokenKind::Bool},     {"string",    TokenKind::String},
};

// Внутреннее состояние лексера: позиция в строке + текущая строка/столбец
struct Lexer {
    const std::string& src;
    const std::string& filename;
    size_t   pos  = 0;
    uint32_t line = 1;
    uint32_t col  = 1;

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

    bool at_end() const { return pos >= src.size(); }

    LexError error(const std::string& msg) const {
        return {filename, line, col, msg};
    }
};

// перевод из hex 
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Сканирование строкового литерала (открывающая " уже поглощена)
// Обрабатывает escape-последовательности: \n \t \r \" \\ \0 \xHH
static std::variant<std::string, LexError>
scan_string(Lexer& l) {
    std::string result;
    while (true) {
        if (l.at_end() || l.peek() == '\n')
            return l.error("unterminated string literal");
        char c = l.advance();
        if (c == '"') return result;
        if (c != '\\') {
            result += c;
            continue;
        }
        
        if (l.at_end()) return l.error("unterminated escape sequence");
        char e = l.advance();
        switch (e) {
        case 'n':  result += '\n'; break;
        case 't':  result += '\t'; break;
        case 'r':  result += '\r'; break;
        case '"':  result += '"';  break;
        case '\\': result += '\\'; break;
        case '0':  result += '\0'; break;
        case 'x': {
            if (l.at_end()) return l.error("incomplete \\xHH escape");
            char h1 = l.advance();
            if (l.at_end()) return l.error("incomplete \\xHH escape");
            char h2 = l.advance();
            int v1 = hex_val(h1), v2 = hex_val(h2);
            if (v1 < 0 || v2 < 0)
                return l.error("invalid hex digit in \\xHH escape");
            result += (char)(v1 * 16 + v2);
            break;
        }
        default:
            return l.error(std::string("unknown escape sequence '\\") + e + "'");
        }
    }
}

// Сканирование числового литерала: десятичный, шестнадцатеричный (0x), двоичный (0b).
// Поддерживает разделители разрядов (_) и суффиксы типов (i32, u8, f64, ...)
static std::variant<Token, LexError>
scan_number(Lexer& l, uint32_t start_line, uint32_t start_col) {
    Token tok;
    tok.line = start_line;
    tok.col  = start_col;

    std::string raw;
    raw += l.src[l.pos - 1]; 

    bool is_float = false;
    int  base     = 10;

    // определение префикса
    if (raw == "0" && !l.at_end()) {
        if (l.peek() == 'x' || l.peek() == 'X') {
            raw += l.advance();
            base = 16;
            if (l.at_end() || hex_val(l.peek()) < 0)
                return l.error("expected hex digit after '0x'");
        } else if (l.peek() == 'b' || l.peek() == 'B') {
            raw += l.advance();
            base = 2;
            if (l.at_end() || (l.peek() != '0' && l.peek() != '1'))
                return l.error("expected binary digit after '0b'");
        }
    }

    while (!l.at_end()) {
        char c = l.peek();
        if (c == '_') {
            if (raw.back() == '_') return l.error("consecutive '_' in number literal");
            if (l.pos + 1 >= l.src.size()) return l.error("trailing '_' in number literal");
            raw += l.advance();
            continue;
        }
        if (base == 16 && hex_val(c) >= 0) { raw += l.advance(); continue; }
        if (base == 2  && (c == '0' || c == '1')) { raw += l.advance(); continue; }
        if (base == 10 && std::isdigit(c))  { raw += l.advance(); continue; }
        break;
    }
    if (!raw.empty() && raw.back() == '_')
        return l.error("trailing '_' in number literal");

    if (base == 10 && !l.at_end() && l.peek() == '.' &&
        !(l.pos + 1 < l.src.size() && l.src[l.pos + 1] == '.')) {
        is_float = true;
        raw += l.advance(); // consume '.'
        while (!l.at_end() && std::isdigit(l.peek()))
            raw += l.advance();
    }
    
    if (base == 10 && !l.at_end() && (l.peek() == 'e' || l.peek() == 'E')) {
        is_float = true;
        raw += l.advance();
        if (!l.at_end() && (l.peek() == '+' || l.peek() == '-'))
            raw += l.advance();
        if (l.at_end() || !std::isdigit(l.peek()))
            return l.error("expected digits after exponent");
        while (!l.at_end() && std::isdigit(l.peek()))
            raw += l.advance();
    }

    tok.lexeme = raw;

    if (is_float) {
        
        std::string suf;
        if (!l.at_end()) {
            
            if (l.peek() == 'f' &&
                l.pos + 2 < l.src.size() &&
                (l.src.substr(l.pos, 3) == "f32" || l.src.substr(l.pos, 3) == "f64")) {
                suf += l.advance(); suf += l.advance(); suf += l.advance();
            }
        }
        tok.float_suffix = suf;
        tok.kind         = TokenKind::FloatLit;
        try {
            tok.float_val = std::stod(raw);
        } catch (...) {
            return l.error("invalid float literal '" + raw + "'");
        }
        tok.lexeme = raw + suf;
        return tok;
    }

    static const std::vector<std::pair<std::string,std::string>> ISUF = {
        {"i64","i64"},{"i32","i32"},{"i16","i16"},{"i8","i8"},
        {"u64","u64"},{"u32","u32"},{"u16","u16"},{"u8","u8"},
    };
    std::string suf;
    for (auto& [sfx, _] : ISUF) {
        if (l.pos + sfx.size() <= l.src.size() &&
            l.src.substr(l.pos, sfx.size()) == sfx) {
            
            size_t end = l.pos + sfx.size();
            if (end >= l.src.size() || !std::isalnum(l.src[end]) && l.src[end] != '_') {
                for (size_t k = 0; k < sfx.size(); ++k) l.advance();
                suf = sfx;
                break;
            }
        }
    }
    tok.int_suffix = suf;
    tok.kind       = TokenKind::IntLit;

    std::string clean;
    for (char c : raw)
        if (c != '_') clean += c;

    try {
        if (base == 10)
            tok.int_val = (int64_t)std::stoull(clean, nullptr, 10);
        else if (base == 16) {
            tok.int_val = (int64_t)std::stoull(clean.substr(2), nullptr, 16);
        } else {
            tok.int_val = (int64_t)std::stoull(clean.substr(2), nullptr, 2);
        }
    } catch (...) {
        return l.error("integer literal out of range: '" + raw + "'");
    }
    tok.lexeme = raw + suf;
    return tok;
}

// Главный цикл лексера: читает символы и выдаёт поток токенов.
// Правило максимального совпадения (maximal munch): >= это один токен, а не > и =.
std::variant<std::vector<Token>, LexError>
tokenize(const std::string& source, const std::string& filename) {
    Lexer l{source, filename};
    std::vector<Token> tokens;

    while (!l.at_end()) {
        while (!l.at_end() && std::isspace((unsigned char)l.peek()))
            l.advance();
        if (l.at_end()) break;

        uint32_t tline = l.line;
        uint32_t tcol  = l.col;
        char c = l.advance();

        if ((unsigned char)c > 127)
            return l.error("non-ASCII character in source");

        if (c == '/' && l.peek() == '/') {
            while (!l.at_end() && l.peek() != '\n') l.advance();
            continue;
        }
        //многострочные комментарии
        if (c == '/' && l.peek() == '*') {
            l.advance(); 
            while (!l.at_end()) {
                char cc = l.advance();
                if (cc == '*' && l.peek() == '/') { l.advance(); break; }
            }
            continue;
        }

        Token tok;
        tok.line   = tline;
        tok.col    = tcol;
        tok.lexeme = std::string(1, c);

        // строковый литерал
        if (c == '"') {
            auto r = scan_string(l);
            if (auto* e = std::get_if<LexError>(&r)) return *e;
            tok.kind    = TokenKind::StringLit;
            tok.str_val = std::get<std::string>(r);
            tok.lexeme  = '"' + tok.str_val + '"'; 
            tokens.push_back(tok);
            continue;
        }
        if (std::isdigit(c)) {
            auto r = scan_number(l, tline, tcol);
            if (auto* e = std::get_if<LexError>(&r)) return *e;
            tokens.push_back(std::get<Token>(r));
            continue;
        }

        if (std::isalpha(c) || c == '_') {
            std::string ident(1, c);
            while (!l.at_end() && (std::isalnum(l.peek()) || l.peek() == '_'))
                ident += l.advance();
            auto it = KEYWORDS.find(ident);
            tok.kind   = (it != KEYWORDS.end()) ? it->second : TokenKind::Ident;
            tok.lexeme = ident;
            tokens.push_back(tok);
            continue;
        }
        auto peek2 = [&](char next) {
            return !l.at_end() && l.peek() == next;
        };

        switch (c) {
        case '+': tok.kind = TokenKind::Plus;     break;
        case '-': tok.kind = TokenKind::Minus;    break;
        case '*': tok.kind = TokenKind::Star;     break;
        case '/': tok.kind = TokenKind::Slash;    break;
        case '%': tok.kind = TokenKind::Percent;  break;
        case ',': tok.kind = TokenKind::Comma;    break;
        case ';': tok.kind = TokenKind::Semicolon; break;
        case '(': tok.kind = TokenKind::LParen;   break;
        case ')': tok.kind = TokenKind::RParen;   break;
        case '[': tok.kind = TokenKind::LBracket; break;
        case ']': tok.kind = TokenKind::RBracket; break;
        case '{': tok.kind = TokenKind::LBrace;   break;
        case '}': tok.kind = TokenKind::RBrace;   break;
        case '.':
            if (peek2('.')) { l.advance(); tok.kind = TokenKind::DotDot; tok.lexeme = ".."; }
            else             {              tok.kind = TokenKind::Dot;                        }
            break;
        case '?': tok.kind = TokenKind::Question; break;
        case '=':
            if (peek2('=')) { l.advance(); tok.kind = TokenKind::EqEq;  tok.lexeme = "=="; }
            else             {              tok.kind = TokenKind::Assign;               }
            break;
        case '!':
            if (peek2('=')) { l.advance(); tok.kind = TokenKind::BangEq; tok.lexeme = "!="; }
            else return l.error(std::string("unexpected character '") + c + "'");
            break;
        case '<':
            if (peek2('=')) { l.advance(); tok.kind = TokenKind::LtEq; tok.lexeme = "<="; }
            else             {              tok.kind = TokenKind::Lt;                       }
            break;
        case '>':
            if (peek2('=')) { l.advance(); tok.kind = TokenKind::GtEq; tok.lexeme = ">="; }
            else             {              tok.kind = TokenKind::Gt;                       }
            break;
        case ':':
            if (peek2(':')) { l.advance(); tok.kind = TokenKind::ColonColon; tok.lexeme = "::"; }
            else             {              tok.kind = TokenKind::Colon;                         }
            break;
        default:
            return l.error(std::string("unexpected character '") + c + "'");
        }
        tokens.push_back(tok);
    }

    Token eof;
    eof.kind = TokenKind::Eof;
    eof.line = l.line;
    eof.col  = l.col;
    eof.lexeme = "<eof>";
    tokens.push_back(eof);
    return tokens;
}

// отладка для просмотра токенов
std::string token_kind_to_string(TokenKind k) {
    switch (k) {
#define C(x) case TokenKind::x: return #x;
    C(IntLit) C(FloatLit) C(StringLit)
    C(Var) C(Val) C(Fun) C(Return) C(If) C(Else) C(While) C(For) C(In) C(Break) C(Continue)
    C(Struct) C(Impl) C(Type) C(Namespace) C(As) C(True) C(False) C(Void) C(And) C(Or) C(Not)
    C(Null)
    C(Int8) C(Int16) C(Int32) C(Int64)
    C(UInt8) C(UInt16) C(UInt32) C(UInt64)
    C(Float32) C(Float64) C(Bool) C(String)
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
    for (const auto& t : tokens) {
        out << t.line << ":" << t.col << "\t"
            << token_kind_to_string(t.kind)
            << "\t'" << t.lexeme << "'\n";
    }
}

} 
