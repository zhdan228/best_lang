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

} // namespace Lexer
