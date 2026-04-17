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

// Разбор типа: базовые типы, массивы, кортежи, nullable
static TypePtr parse_type(P& p) {
    TypePtr base;

    // массивы: '[' тип ';' N ']' или '[' тип ']' (динамический)
    if (p.at(TK::LBracket)) {
        p.advance();
        auto elem = parse_type(p);
        if (p.at(TK::Semicolon)) {
            p.advance();
            auto n_tok = p.expect(TK::IntLit);
            p.expect(TK::RBracket);
            base = Type::make_array(std::move(elem), n_tok.int_val);
        } else {
            p.expect(TK::RBracket);
            base = Type::make_dynarray(std::move(elem));
        }
    }
    // кортеж: '(' тип ',' тип ... ')'
    else if (p.at(TK::LParen)) {
        p.advance();
        std::vector<TypePtr> elems;
        if (!p.at(TK::RParen)) {
            elems.push_back(parse_type(p));
            while (p.at(TK::Comma)) {
                p.advance();
                if (p.at(TK::RParen)) break;
                elems.push_back(parse_type(p));
            }
        }
        p.expect(TK::RParen);
        base = Type::make_tuple(std::move(elems));
    }
    else {
        auto tok = p.cur();
        p.advance();
        switch (tok.kind) {
        case TK::Int8:    base = TYPE_INT8;    break;
        case TK::Int16:   base = TYPE_INT16;   break;
        case TK::Int32:   base = TYPE_INT32;   break;
        case TK::Int64:   base = TYPE_INT64;   break;
        case TK::UInt8:   base = TYPE_UINT8;   break;
        case TK::UInt16:  base = TYPE_UINT16;  break;
        case TK::UInt32:  base = TYPE_UINT32;  break;
        case TK::UInt64:  base = TYPE_UINT64;  break;
        case TK::Float32: base = TYPE_FLOAT32; break;
        case TK::Float64: base = TYPE_FLOAT64; break;
        case TK::Bool:    base = TYPE_BOOL;    break;
        case TK::String:  base = TYPE_STRING;  break;
        case TK::Void:    base = TYPE_VOID;    break;
        case TK::Ident:   base = Type::make_struct(tok.lexeme); break;
        default:
            throw p.error(tok, "expected type, got '" + tok.lexeme + "'");
        }
    }

    // необязательный суффикс nullable '?'
    while (p.at(TK::Question)) {
        p.advance();
        base = Type::make_nullable(std::move(base));
    }
    return base;
}

static ExprPtr parse_primary(P& p) {
    auto loc = p.loc();

    // целый литерал
    if (p.at(TK::IntLit)) {
        auto tok = p.advance();
        auto e = std::make_unique<IntLitExpr>();
        e->kind   = Expr::Kind::IntLit;
        e->loc    = loc;
        e->value  = tok.int_val;
        e->suffix = tok.int_suffix;
        return e;
    }
    // вещественный литерал
    if (p.at(TK::FloatLit)) {
        auto tok = p.advance();
        auto e = std::make_unique<FloatLitExpr>();
        e->kind   = Expr::Kind::FloatLit;
        e->loc    = loc;
        e->value  = tok.float_val;
        e->suffix = tok.float_suffix;
        return e;
    }
    // логический литерал
    if (p.at(TK::True) || p.at(TK::False)) {
        bool v = p.at(TK::True);
        p.advance();
        auto e = std::make_unique<BoolLitExpr>();
        e->kind  = Expr::Kind::BoolLit;
        e->loc   = loc;
        e->value = v;
        return e;
    }
    // null литерал
    if (p.at(TK::Null)) {
        p.advance();
        auto e = std::make_unique<NullLitExpr>();
        e->kind = Expr::Kind::NullLit;
        e->loc  = loc;
        return e;
    }
    // строковый литерал
    if (p.at(TK::StringLit)) {
        auto tok = p.advance();
        auto e = std::make_unique<StringLitExpr>();
        e->kind  = Expr::Kind::StringLit;
        e->loc   = loc;
        e->value = tok.str_val;
        return e;
    }
    // литерал массива: '[' exprs ']'
    if (p.at(TK::LBracket)) {
        p.advance();
        auto e = std::make_unique<ArrayLitExpr>();
        e->kind = Expr::Kind::ArrayLit;
        e->loc  = loc;
        if (!p.at(TK::RBracket)) {
            e->elements.push_back(parse_expr(p));
            while (p.at(TK::Comma)) {
                p.advance();
                if (p.at(TK::RBracket)) break;
                e->elements.push_back(parse_expr(p));
            }
        }
        p.expect(TK::RBracket);
        return e;
    }
    // структура literal: IDENT '{' field: expr, ... '}'
    // Отличаем от IDENT перед блоком кода (if/while body).
    // Внутри '{' должно быть поле или '}'
    if (p.at(TK::Ident) && p.peek(1).kind == TK::LBrace &&
        (p.peek(2).kind == TK::RBrace ||
         (p.peek(2).kind == TK::Ident && p.peek(3).kind == TK::Colon))) {
        auto name_tok = p.advance();
        p.advance(); // '{'
        auto e = std::make_unique<StructLitExpr>();
        e->kind      = Expr::Kind::StructLit;
        e->loc       = loc;
        e->type_name = name_tok.lexeme;
        if (!p.at(TK::RBrace)) {
            while (true) {
                auto fn_tok = p.expect(TK::Ident);
                p.expect(TK::Colon);
                auto val = parse_expr(p);
                StructLitField f;
                f.name  = fn_tok.lexeme;
                f.value = std::move(val);
                e->fields.push_back(std::move(f));
                if (!p.at(TK::Comma)) break;
                p.advance();
                if (p.at(TK::RBrace)) break;
            }
        }
        p.expect(TK::RBrace);
        return e;
    }
    // доступ через :: или просто идентификатор
    if (p.at(TK::Ident)) {
        auto name_tok = p.advance();
        // пространство имён::member
        if (p.at(TK::ColonColon)) {
            p.advance();
            auto mem_tok = p.expect(TK::Ident);
            auto e = std::make_unique<NamespaceAccessExpr>();
            e->kind      = Expr::Kind::NamespaceAccess;
            e->loc       = loc;
            e->ns_name   = name_tok.lexeme;
            e->member    = mem_tok.lexeme;
            return e;
        }
        auto e = std::make_unique<IdentExpr>();
        e->kind = Expr::Kind::Ident;
        e->loc  = loc;
        e->name = name_tok.lexeme;
        return e;
    }
    // выражение в скобках или кортеж
    if (p.at(TK::LParen)) {
        p.advance();
        if (p.at(TK::RParen)) {
            // empty tuple ()
            p.advance();
            auto e = std::make_unique<TupleLitExpr>();
            e->kind = Expr::Kind::TupleLit;
            e->loc  = loc;
            return e;
        }
        auto first = parse_expr(p);
        if (p.at(TK::Comma)) {
            // это кортеж
            auto e = std::make_unique<TupleLitExpr>();
            e->kind = Expr::Kind::TupleLit;
            e->loc  = loc;
            e->elements.push_back(std::move(first));
            while (p.at(TK::Comma)) {
                p.advance();
                if (p.at(TK::RParen)) break;
                e->elements.push_back(parse_expr(p));
            }
            p.expect(TK::RParen);
            return e;
        }
        p.expect(TK::RParen);
        return first; // просто скобки
    }
    throw p.error(p.cur(), "expected expression, got '" + p.cur().lexeme + "'");
}

// ── postfix: call, index, field ───────────────────────────────────────────
static ExprPtr parse_postfix(P& p) {
    auto e = parse_primary(p);
    while (true) {
        if (p.at(TK::LParen)) {
            // вызов функции — позиционные и именованные аргументы
            auto loc = p.loc();
            p.advance();

static int bin_prec(Lexer::TokenKind k) {
    using TK = Lexer::TokenKind;
    switch(k) {
    case TK::Or:      return 3;
    case TK::And:     return 2;
    case TK::EqEq: case TK::BangEq:
    case TK::Lt: case TK::LtEq:
    case TK::Gt: case TK::GtEq: return 5;
    case TK::Plus: case TK::Minus: return 6;
    case TK::Star: case TK::Slash: case TK::Percent: return 7;
    default: return -1;
    }
}
} // namespace Parser
