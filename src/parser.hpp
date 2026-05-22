#pragma once
#include "lexer.hpp"
#include "ast.hpp"
#include <string>
#include <vector>
#include <stdexcept>
#include <expected>

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

std::expected<Program, ParseError> parse(const std::vector<Lexer::Token>& tokens, const std::string& filename);

void dump_ast(const Program& prog, std::ostream& out);

}
