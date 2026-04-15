/*
 * Синтаксический анализатор BestLang — рекурсивный спуск.
 *
 * Грамматика реализована набором взаимно рекурсивных функций:
 *   parse_expr  → parse_bin → parse_unary → parse_postfix → parse_primary
 * Приоритет операторов задаётся через числовые уровни (precedence climbing).
 */

#include "parser.hpp"
#include <sstream>
#include <cassert>
#include <stdexcept>
#include <functional>

namespace Parser {

using TK = Lexer::TokenKind;

// ── внутреннее состояние парсера  ─────────────────────────────────────────────────
struct P {
    const std::vector<Lexer::Token>& toks;
    const std::string& filename;
    size_t pos = 0;

    const Lexer::Token& cur() const { return toks[pos]; }
    const Lexer::Token& peek(size_t n=1) const {
        return pos+n < toks.size() ? toks[pos+n] : toks.back();
    }
    bool at(TK k) const { return cur().kind == k; }

    Lexer::Token advance() {
        auto t = toks[pos];
        if (pos + 1 < toks.size()) ++pos;
        return t;
    }

    ParseError error(const Lexer::Token& t, const std::string& msg) const {
        return {filename, t.line, t.col, msg};
    }

    // Потребляет токен ожидаемого вида или бросает ошибку
    Lexer::Token expect(TK k) {
        if (!at(k)) {
            auto msg = "expected '" + Lexer::token_kind_to_string(k) +
                       "' but got '" + cur().lexeme + "'";
            throw error(cur(), msg);
        }
        return advance();
    }

    SrcLoc loc() const { return {filename, cur().line, cur().col}; }
    SrcLoc loc_tok(const Lexer::Token& t) const { return {filename, t.line, t.col}; }
};

static ExprPtr parse_expr(P& p);
static StmtPtr parse_stmt(P& p);
static std::unique_ptr<BlockStmt> parse_block(P& p);
static TypePtr parse_type(P& p);
static TopDeclPtr parse_top_decl(P& p);


} // namespace Parser
