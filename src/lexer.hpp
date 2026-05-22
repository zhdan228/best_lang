#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <ostream>
#include <optional>
#include <expected>

namespace Lexer {

enum class TokenKind {
    IntLit, FloatLit, StringLit, CharLit,

    Var, Val, Fun, Return, If, Else, While, For, In, Break, Continue,
    Struct, Impl, Type, Namespace, As, True, False, Void, And, Or, Not,
    Null,

    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Float32, Float64, Bool, String, Char,

    Ident,

    Plus, Minus, Star, Slash, Percent,
    EqEq, BangEq, Lt, LtEq, Gt, GtEq,
    Assign,
    Dot, DotDot, ColonColon, Colon,
    Question,
    LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Comma, Semicolon,

    Eof
};

struct Token {
    TokenKind   kind;
    std::string lexeme;

    union {
        int8_t   i8;
        int16_t  i16;
        int32_t  i32;
        int64_t  i64;
        uint8_t  u8;
        uint16_t u16;
        uint32_t u32;
        uint64_t u64;
        float    f32;
        double   f64;
    } num = {};

    std::string str_val;
    std::string suffix;
    uint32_t line = 1;
    uint32_t col  = 1;

    int64_t  as_int()   const {
        if (suffix == "i8")  return num.i8;
        if (suffix == "i16") return num.i16;
        if (suffix == "i32") return num.i32;
        return num.i64;
    }
    uint64_t as_uint()  const {
        if (suffix == "u8")  return num.u8;
        if (suffix == "u16") return num.u16;
        if (suffix == "u32") return num.u32;
        return num.u64;
    }
    double   as_float() const { return (suffix == "f32") ? (double)num.f32 : num.f64; }
};

struct LexError {
    std::string filename;
    uint32_t    line;
    uint32_t    col;
    std::string message;

    std::string format() const {
        return filename + ":" + std::to_string(line) + ":" +
               std::to_string(col) + ": error: " + message;
    }
};

std::expected<std::vector<Token>, LexError> tokenize(const std::string& source, const std::string& filename);

std::string token_kind_to_string(TokenKind k);
void dump_tokens(const std::vector<Token>& tokens, std::ostream& out);

}
