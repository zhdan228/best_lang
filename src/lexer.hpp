/*
 * Лексический анализатор BestLang.
 *
 * Преобразует исходный текст (.bl) в поток токенов.
 * Каждый токен несёт тип, лексему и позицию в файле.
 * Пробелы и комментарии отфильтровываются — в поток не попадают.
 */
#pragma once
#include <string>
#include <vector>
#include <variant>
#include <cstdint>
#include <ostream>

namespace Lexer {

// Виды токенов языка BestLang
enum class TokenKind {
    // Литералы
    IntLit, FloatLit, StringLit,

    // Ключевые слова
    Var, Val, Fun, Return, If, Else, While, For, In, Break, Continue,
    Struct, Impl, Type, Namespace, As, True, False, Void, And, Or, Not,
    Null,  // литерал null

    // Имена встроенных типов (зарезервированы, нельзя использовать как идентификаторы)
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Float32, Float64, Bool, String,

    // Идентификатор
    Ident,

    // Арифметические операторы
    Plus, Minus, Star, Slash, Percent,

    // Операторы сравнения
    EqEq, BangEq, Lt, LtEq, Gt, GtEq,

    // Присваивание
    Assign,

    // Операторы доступа
    Dot, DotDot, ColonColon, Colon,

    // Суффикс nullable типа
    Question,

    // Разделители
    LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Comma, Semicolon,

    // Конец файла
    Eof
};

// Один токен из потока лексера
struct Token {
    TokenKind   kind;
    std::string lexeme;   // исходный текст как написан в коде

    // Числовое значение: int_val, uint_val и float_val делят одну память (8 байт).
    // Какое поле читать — определяет suffix.
    union {
        int8_t   i8;    // суффикс i8
        int16_t  i16;   // суффикс i16
        int32_t  i32;   // суффикс i32 или нет суффикса
        int64_t  i64;   // суффикс i64
        uint8_t  u8;    // суффикс u8
        uint16_t u16;   // суффикс u16
        uint32_t u32;   // суффикс u32
        uint64_t u64;   // суффикс u64
        float    f32;   // суффикс f32
        double   f64;   // суффикс f64 или нет суффикса
    } num = {};

    std::string str_val;  // заполняется для StringLit
    std::string suffix;   // суффикс типа: "i8", "u32", "f64", "" и т.д.

    uint32_t line = 1;
    uint32_t col  = 1;

    int64_t as_int() const {
        if (suffix == "i8")  return num.i8;
        if (suffix == "i16") return num.i16;
        if (suffix == "i32") return num.i32;
        return num.i64;
    }
    uint64_t as_uint() const {
        if (suffix == "u8")  return num.u8;
        if (suffix == "u16") return num.u16;
        if (suffix == "u32") return num.u32;
        return num.u64;
    }
    double as_float() const { return (suffix == "f32") ? (double)num.f32 : num.f64; }
};

// Ошибка лексера — всегда фатальная (одна ошибка → остановка)
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

// При успехе возвращает поток токенов (последний токен — всегда Eof).
// При ошибке — первую LexError.
std::variant<std::vector<Token>, LexError>
tokenize(const std::string& source, const std::string& filename);

std::string token_kind_to_string(TokenKind k);
void dump_tokens(const std::vector<Token>& tokens, std::ostream& out);

} 
