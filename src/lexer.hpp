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
    std::string lexeme;       // исходный текст как написан в коде
    int64_t     int_val   = 0;    // заполняется для IntLit
    double      float_val = 0.0;  // заполняется для FloatLit
    std::string str_val;          // заполняется для StringLit
    // суффиксы числовых литералов: "", "i8","i16","i32","i64","u8","u16","u32","u64"
    std::string int_suffix;
    // суффиксы вещественных литералов: "", "f32", "f64"
    std::string float_suffix;
    uint32_t line = 1;
    uint32_t col  = 1;
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
