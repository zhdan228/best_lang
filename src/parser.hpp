#pragma once
#include "lexer.hpp"
#include "ast.hpp"
#include <string>
#include <vector>
#include <optional>

namespace Parser {

struct ParseError {
    std::string filename;
    uint32_t    line;
    uint32_t    col;
    std::string message;

    std::string format() const {
        return filename + ":" + std::to_string(line) + ":" +
               std::to_string(col) + ": error: " + message;
    }
};

// Строит AST из потока токенов.
// Возвращает программу при успехе или первую синтаксическую ошибку.
std::variant<Program, ParseError>
parse(const std::vector<Lexer::Token>& tokens, const std::string& filename);

void dump_ast(const Program& prog, std::ostream& out);

} 
