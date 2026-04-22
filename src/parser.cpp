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
            auto call = std::make_unique<CallExpr>();
            call->kind   = Expr::Kind::Call;
            call->loc    = loc;
            call->callee = std::move(e);
            auto parse_one_arg = [&]() {
                // именованный аргумент: name = expr
                if (p.at(TK::Ident) && p.peek(1).kind == TK::Assign) {
                    auto name_tok = p.advance();
                    p.advance(); // '='
                    auto val = parse_expr(p);
                    NamedArg na;
                    na.name  = name_tok.lexeme;
                    na.value = std::move(val);
                    call->named_args.push_back(std::move(na));
                } else {
                    call->args.push_back(parse_expr(p));
                }
            };
            if (!p.at(TK::RParen)) {
                parse_one_arg();
                while (p.at(TK::Comma)) {
                    p.advance();
                    if (p.at(TK::RParen)) break;
                    parse_one_arg();
                }
            }
            p.expect(TK::RParen);
            e = std::move(call);
        } else if (p.at(TK::LBracket)) {
            auto loc = p.loc();
            p.advance();
            auto idx = std::make_unique<IndexExpr>();
            idx->kind = Expr::Kind::Index;
            idx->loc  = loc;
            idx->arr  = std::move(e);
            idx->idx  = parse_expr(p);
            p.expect(TK::RBracket);
            e = std::move(idx);
        } else if (p.at(TK::Dot)) {
            auto loc = p.loc();
            p.advance();
            // индекс кортежа: .0, .1, .2
            if (p.at(TK::IntLit)) {
                auto idx_tok = p.advance();
                auto te = std::make_unique<TupleIndexExpr>();
                te->kind   = Expr::Kind::TupleIndex;
                te->loc    = loc;
                te->object = std::move(e);
                te->index  = static_cast<int>(idx_tok.int_val);
                e = std::move(te);
            } else {
                auto fn_tok = p.expect(TK::Ident);
                auto fe = std::make_unique<FieldExpr>();
                fe->kind       = Expr::Kind::Field;
                fe->loc        = loc;
                fe->object     = std::move(e);
                fe->field_name = fn_tok.lexeme;
                e = std::move(fe);
            }
        } else {
            break;
        }
    }
    return e;
}

static ExprPtr parse_unary(P& p) {
    if (p.at(TK::Minus)) {
        auto loc = p.loc();
        p.advance();
        auto e = std::make_unique<UnaryOpExpr>();
        e->kind    = Expr::Kind::UnaryOp;
        e->loc     = loc;
        e->op      = UnaryOpKind::Neg;
        e->operand = parse_unary(p);
        return e;
    }
    return parse_postfix(p);
}

// Разбор бинарных выражений с приоритетом
// Операторы сравнения нецепочечные
//  7: * / %
//  6: + -
//  5: == != < > <= >=
//  4: not  
//  3: and
//  2: or
//  1: as   
static int bin_prec(TK k) {
    switch (k) {
    case TK::Star: case TK::Slash: case TK::Percent: return 7;
    case TK::Plus: case TK::Minus:                   return 6;
    case TK::EqEq: case TK::BangEq:
    case TK::Lt:   case TK::LtEq:
    case TK::Gt:   case TK::GtEq:                   return 5;
    case TK::And:                                    return 3;
    case TK::Or:                                     return 2;
    default: return -1;
    }
}
static BinOpKind bin_op_kind(TK k) {
    switch (k) {
    case TK::Plus:    return BinOpKind::Add;
    case TK::Minus:   return BinOpKind::Sub;
    case TK::Star:    return BinOpKind::Mul;
    case TK::Slash:   return BinOpKind::Div;
    case TK::Percent: return BinOpKind::Mod;
    case TK::EqEq:    return BinOpKind::Eq;
    case TK::BangEq:  return BinOpKind::NEq;
    case TK::Lt:      return BinOpKind::Lt;
    case TK::LtEq:    return BinOpKind::Le;
    case TK::Gt:      return BinOpKind::Gt;
    case TK::GtEq:    return BinOpKind::Ge;
    case TK::And:     return BinOpKind::And;
    case TK::Or:      return BinOpKind::Or;
    default: throw std::runtime_error("bad bin op");
    }
}

static ExprPtr parse_bin(P& p, int min_prec) {
    if (p.at(TK::Not) && min_prec <= 4) {
        auto loc = p.loc();
        p.advance();
        auto e = std::make_unique<UnaryOpExpr>();
        e->kind    = Expr::Kind::UnaryOp;
        e->loc     = loc;
        e->op      = UnaryOpKind::Not;
        e->operand = parse_bin(p, 4);
        return e;
    }

    auto lhs = parse_unary(p);
    while (true) {
        int prec = bin_prec(p.cur().kind);
        if (prec < min_prec) break;
            if (prec == 5) {
            auto loc = p.loc();
            auto op  = bin_op_kind(p.cur().kind);
            p.advance();
            auto rhs = parse_bin(p, prec + 1); // запрет цепочки: требуем строго выше
            auto e = std::make_unique<BinOpExpr>();
            e->kind = Expr::Kind::BinOp;
            e->loc  = loc;
            e->op   = op;
            e->lhs  = std::move(lhs);
            e->rhs  = std::move(rhs);
            lhs = std::move(e);
            // после одного сравнения — нельзя ещё одно
            if (bin_prec(p.cur().kind) == 5)
                throw p.error(p.cur(), "comparison operators are non-chaining; use parentheses");
        } else {
            auto loc = p.loc();
            auto op  = bin_op_kind(p.cur().kind);
            p.advance();
            auto rhs = parse_bin(p, prec + 1);
            auto e = std::make_unique<BinOpExpr>();
            e->kind = Expr::Kind::BinOp;
            e->loc  = loc;
            e->op   = op;
            e->lhs  = std::move(lhs);
            e->rhs  = std::move(rhs);
            lhs = std::move(e);
        }
    }
    return lhs;
}
static ExprPtr parse_expr(P& p) {
    auto e = parse_bin(p, 2);
    while (p.at(TK::As)) {
        auto loc = p.loc();
        p.advance();
        auto t = parse_type(p);
        auto cast = std::make_unique<CastExpr>();
        cast->kind    = Expr::Kind::Cast;
        cast->loc     = loc;
        cast->operand = std::move(e);
        cast->target  = std::move(t);
        e = std::move(cast);
    }
    return e;
}


static std::unique_ptr<BlockStmt> parse_block(P& p) {
    auto blk = std::make_unique<BlockStmt>();
    blk->kind = Stmt::Kind::Block;
    blk->loc  = p.loc();
    p.expect(TK::LBrace);
    while (!p.at(TK::RBrace) && !p.at(TK::Eof))
        blk->stmts.push_back(parse_stmt(p));
    p.expect(TK::RBrace);
    return blk;
}

static StmtPtr parse_stmt(P& p) {
    auto loc = p.loc();

    if (p.at(TK::Semicolon)) {
        p.advance();
        auto s = std::make_unique<EmptyStmt>();
        s->kind = Stmt::Kind::Empty;
        s->loc  = loc;
        return s;
    }
    
    if (p.at(TK::LBrace)) return parse_block(p);

    // объявление var/val
    if (p.at(TK::Var) || p.at(TK::Val)) {
        bool is_val = p.at(TK::Val);
        p.advance();
        auto name_tok = p.expect(TK::Ident);
        TypePtr ann;
        if (p.at(TK::Colon)) { p.advance(); ann = parse_type(p); }
        p.expect(TK::Assign);
        auto init = parse_expr(p);
        p.expect(TK::Semicolon);
        auto s = std::make_unique<VarDeclStmt>();
        s->kind     = Stmt::Kind::VarDecl;
        s->loc      = loc;
        s->name     = name_tok.lexeme;
        s->ann_type = std::move(ann);
        s->init     = std::move(init);
        s->is_val   = is_val;
        return s;
    }
    // if
    if (p.at(TK::If)) {
        p.advance();
        auto cond = parse_expr(p);
        auto then = parse_block(p);
        StmtPtr els;
        if (p.at(TK::Else)) {
            p.advance();
            if (p.at(TK::If)) els = parse_stmt(p);
            else               els = parse_block(p);
        }
        auto s = std::make_unique<IfStmt>();
        s->kind        = Stmt::Kind::If;
        s->loc         = loc;
        s->cond        = std::move(cond);
        s->then_branch = std::move(then);
        s->else_branch = std::move(els);
        return s;
    }
    // while
    if (p.at(TK::While)) {
        p.advance();
        auto cond = parse_expr(p);
        auto body = parse_block(p);
        auto s = std::make_unique<WhileStmt>();
        s->kind = Stmt::Kind::While;
        s->loc  = loc;
        s->cond = std::move(cond);
        s->body = std::move(body);
        return s;
    }
    // break
    if (p.at(TK::Break)) {
        p.advance(); p.expect(TK::Semicolon);
        auto s = std::make_unique<BreakStmt>();
        s->kind = Stmt::Kind::Break; s->loc = loc;
        return s;
    }
    // continue
    if (p.at(TK::Continue)) {
        p.advance(); p.expect(TK::Semicolon);
        auto s = std::make_unique<ContinueStmt>();
        s->kind = Stmt::Kind::Continue; s->loc = loc;
        return s;
    }
    // for i in start..end { }
    if (p.at(TK::For) && p.peek(1).kind == TK::Ident && p.peek(2).kind == TK::In) {
        p.advance(); // 'for'
        auto var_tok = p.advance(); // ident
        p.advance(); // 'in'
        auto start = parse_expr(p);
        p.expect(TK::DotDot);
        auto end  = parse_expr(p);
        auto body = parse_block(p);
        auto s = std::make_unique<ForRangeStmt>();
        s->kind     = Stmt::Kind::ForRange;
        s->loc      = loc;
        s->var_name = var_tok.lexeme;
        s->start    = std::move(start);
        s->end      = std::move(end);
        s->body     = std::move(body);
        return s;
    }
    // for init; cond; step { }
    if (p.at(TK::For)) {
        p.advance(); // 'for'
        // инициализация: var/val или присваивание или выражение
        StmtPtr init;
        if (p.at(TK::Var) || p.at(TK::Val)) {
            bool iv = p.at(TK::Val); p.advance();
            auto nm = p.expect(TK::Ident);
            TypePtr ann;
            if (p.at(TK::Colon)) { p.advance(); ann = parse_type(p); }
            p.expect(TK::Assign);
            auto iv_e = parse_expr(p);
            auto vd = std::make_unique<VarDeclStmt>();
            vd->kind = Stmt::Kind::VarDecl; vd->loc = loc;
            vd->name = nm.lexeme; vd->ann_type = std::move(ann);
            vd->init = std::move(iv_e); vd->is_val = iv;
            init = std::move(vd);
        } else {
            auto e = parse_expr(p);
            if (p.at(TK::Assign)) {
                p.advance();
                auto rhs = parse_expr(p);
                // строим lvalue from e
                auto build = [&](ExprPtr& ep) -> LValuePtr {
                    std::function<LValuePtr(ExprPtr&)> to_lv = [&](ExprPtr& x) -> LValuePtr {
                        if (x->kind == Expr::Kind::Ident) {
                            auto& id = static_cast<IdentExpr&>(*x);
                            auto lv = std::make_unique<LValue>();
                            lv->kind = LValue::Kind::Ident; lv->loc = id.loc; lv->name = id.name;
                            return lv;
                        }
                        throw p.error(p.cur(), "invalid for-loop init lvalue");
                    };
                    return to_lv(ep);
                };
                auto lv = build(e);
                auto as = std::make_unique<AssignStmt>();
                as->kind = Stmt::Kind::Assign; as->loc = loc;
                as->target = std::move(lv); as->value = std::move(rhs);
                init = std::move(as);
            } else {
                auto es = std::make_unique<ExprStmt>();
                es->kind = Stmt::Kind::ExprStmt; es->loc = loc; es->expr = std::move(e);
                init = std::move(es);
            }
        }
        p.expect(TK::Semicolon);
        auto cond = parse_expr(p);
        p.expect(TK::Semicolon);
        // шаг: присваивание или выражение
        StmtPtr step;
        {
            auto e = parse_expr(p);
            if (p.at(TK::Assign)) {
                p.advance();
                auto rhs = parse_expr(p);
                std::function<LValuePtr(ExprPtr&)> to_lv = [&](ExprPtr& x) -> LValuePtr {
                    if (x->kind == Expr::Kind::Ident) {
                        auto& id = static_cast<IdentExpr&>(*x);
                        auto lv = std::make_unique<LValue>();
                        lv->kind = LValue::Kind::Ident; lv->loc = id.loc; lv->name = id.name;
                        return lv;
                    }
                    throw p.error(p.cur(), "invalid for-loop step lvalue");
                };
                auto lv = to_lv(e);
                auto as = std::make_unique<AssignStmt>();
                as->kind = Stmt::Kind::Assign; as->loc = loc;
                as->target = std::move(lv); as->value = std::move(rhs);
                step = std::move(as);
            } else {
                auto es = std::make_unique<ExprStmt>();
                es->kind = Stmt::Kind::ExprStmt; es->loc = loc; es->expr = std::move(e);
                step = std::move(es);
            }
        }
        auto body = parse_block(p);
        auto s = std::make_unique<ForCStmt>();
        s->kind = Stmt::Kind::ForC; s->loc = loc;
        s->init = std::move(init); s->cond = std::move(cond);
        s->step = std::move(step); s->body = std::move(body);
        return s;
    }
    // return
    if (p.at(TK::Return)) {
        p.advance();
        ExprPtr val;
        if (!p.at(TK::Semicolon)) val = parse_expr(p);
        p.expect(TK::Semicolon);
        auto s = std::make_unique<ReturnStmt>();
        s->kind  = Stmt::Kind::Return;
        s->loc   = loc;
        s->value = std::move(val);
        return s;
    }
    // присваивание или выражение-инструкция
    auto e = parse_expr(p);
    if (p.at(TK::Assign)) {
        // строим lvalue из выражения
        // только определённые формы допустимы как lvalue
        auto build_lv = [&](ExprPtr& ep) -> LValuePtr {
            if (ep->kind == Expr::Kind::Ident) {
                auto& id = static_cast<IdentExpr&>(*ep);
                auto lv = std::make_unique<LValue>();
                lv->kind = LValue::Kind::Ident;
                lv->loc  = id.loc;
                lv->name = id.name;
                return lv;
            }
            if (ep->kind == Expr::Kind::Index) {
                auto base_lv = [&]() -> LValuePtr {
                    // рекурсивно конвертируем выражение в lvalue
                    std::function<LValuePtr(ExprPtr&)> to_lv = [&](ExprPtr& x) -> LValuePtr {
                        if (x->kind == Expr::Kind::Ident) {
                            auto& id = static_cast<IdentExpr&>(*x);
                            auto lv = std::make_unique<LValue>();
                            lv->kind = LValue::Kind::Ident;
                            lv->loc  = id.loc;
                            lv->name = id.name;
                            return lv;
                        }
                        if (x->kind == Expr::Kind::Index) {
                            auto& ix = static_cast<IndexExpr&>(*x);
                            auto outer = std::make_unique<LValue>();
                            outer->kind = LValue::Kind::Index;
                            outer->loc  = ix.loc;
                            outer->base = to_lv(ix.arr);
                            outer->idx  = std::move(ix.idx);
                            return outer;
                        }
                        if (x->kind == Expr::Kind::Field) {
                            auto& fx = static_cast<FieldExpr&>(*x);
                            auto outer = std::make_unique<LValue>();
                            outer->kind  = LValue::Kind::Field;
                            outer->loc   = fx.loc;
                            outer->base  = to_lv(fx.object);
                            outer->field = fx.field_name;
                            return outer;
                        }
                        throw p.error(p.cur(), "invalid assignment target");
                    };
                    return to_lv(ep);
                }();
                return base_lv;
            }
            // рекурсивное преобразование в lvalue
            std::function<LValuePtr(ExprPtr&)> to_lv = [&](ExprPtr& x) -> LValuePtr {
                if (x->kind == Expr::Kind::Ident) {
                    auto& id = static_cast<IdentExpr&>(*x);
                    auto lv = std::make_unique<LValue>();
                    lv->kind = LValue::Kind::Ident;
                    lv->loc  = id.loc;
                    lv->name = id.name;
                    return lv;
                }
                if (x->kind == Expr::Kind::Index) {
                    auto& ix = static_cast<IndexExpr&>(*x);
                    auto outer = std::make_unique<LValue>();
                    outer->kind = LValue::Kind::Index;
                    outer->loc  = ix.loc;
                    outer->base = to_lv(ix.arr);
                    outer->idx  = std::move(ix.idx);
                    return outer;
                }
                if (x->kind == Expr::Kind::Field) {
                    auto& fx = static_cast<FieldExpr&>(*x);
                    auto outer = std::make_unique<LValue>();
                    outer->kind  = LValue::Kind::Field;
                    outer->loc   = fx.loc;
                    outer->base  = to_lv(fx.object);
                    outer->field = fx.field_name;
                    return outer;
                }
                throw p.error(p.cur(), "invalid assignment target");
            };
            return to_lv(ep);
        };
        auto lv = build_lv(e);
        p.advance(); // потребляем '='
        auto rhs = parse_expr(p);
        p.expect(TK::Semicolon);
        auto s = std::make_unique<AssignStmt>();
        s->kind   = Stmt::Kind::Assign;
        s->loc    = loc;
        s->target = std::move(lv);
        s->value  = std::move(rhs);
        return s;
    }
    p.expect(TK::Semicolon);
    auto s = std::make_unique<ExprStmt>();
    s->kind = Stmt::Kind::ExprStmt;
    s->loc  = loc;
    s->expr = std::move(e);
    return s;
}

static TopDeclPtr parse_top_decl(P& p) {
    auto loc = p.loc();

    // структура
    if (p.at(TK::Struct)) {
        p.advance();
        auto name_tok = p.expect(TK::Ident);
        p.expect(TK::LBrace);
        auto sd = std::make_unique<StructDecl>();
        sd->kind = TopDecl::Kind::Struct;
        sd->loc  = loc;
        sd->name = name_tok.lexeme;
        while (!p.at(TK::RBrace) && !p.at(TK::Eof)) {
            auto fn_tok = p.expect(TK::Ident);
            p.expect(TK::Colon);
            auto ftype = parse_type(p);
            p.expect(TK::Semicolon);
            FieldDecl fd;
            fd.name = fn_tok.lexeme;
            fd.type = std::move(ftype);
            fd.loc  = {p.filename, fn_tok.line, fn_tok.col};
            sd->fields.push_back(std::move(fd));
        }
        p.expect(TK::RBrace);
        return sd;
    }
    // псевдоним типа
    if (p.at(TK::Type)) {
        p.advance();
        auto name_tok = p.expect(TK::Ident);
        p.expect(TK::Assign);
        auto aliased = parse_type(p);
        p.expect(TK::Semicolon);
        auto ta = std::make_unique<TypeAliasDecl>();
        ta->kind    = TopDecl::Kind::TypeAlias;
        ta->loc     = loc;
        ta->name    = name_tok.lexeme;
        ta->aliased = std::move(aliased);
        return ta;
    }
    // пространство имён
    if (p.at(TK::Namespace)) {
        p.advance();
        auto name_tok = p.expect(TK::Ident);
        p.expect(TK::LBrace);
        auto nd = std::make_unique<NamespaceDecl>();
        nd->kind = TopDecl::Kind::Namespace;
        nd->loc  = loc;
        nd->name = name_tok.lexeme;
        while (!p.at(TK::RBrace) && !p.at(TK::Eof)) {
            if (p.at(TK::Namespace))
                throw p.error(p.cur(), "вложенные пространства имён не поддерживаются");
            nd->members.push_back(parse_top_decl(p));
        }
        p.expect(TK::RBrace);
        return nd;
    }
    // глобальная переменная
    if (p.at(TK::Var) || p.at(TK::Val)) {
        bool is_val = p.at(TK::Val);
        p.advance();
        auto name_tok = p.expect(TK::Ident);
        TypePtr ann;
        if (p.at(TK::Colon)) { p.advance(); ann = parse_type(p); }
        p.expect(TK::Assign);
        auto init = parse_expr(p);
        p.expect(TK::Semicolon);
        auto gv = std::make_unique<GlobalVarDecl>();
        gv->kind     = TopDecl::Kind::GlobalVar;
        gv->loc      = loc;
        gv->name     = name_tok.lexeme;
        gv->ann_type = std::move(ann);
        gv->init     = std::move(init);
        gv->is_val   = is_val;
        return gv;
    }
    // impl TypeName { fun method(...) { ... } }
    if (p.at(TK::Impl)) {
        p.advance();
        auto name_tok = p.expect(TK::Ident);
        p.expect(TK::LBrace);
        auto id = std::make_unique<ImplDecl>();
        id->kind      = TopDecl::Kind::Impl;
        id->loc       = loc;
        id->type_name = name_tok.lexeme;
        while (!p.at(TK::RBrace) && !p.at(TK::Eof)) {
            auto d = parse_top_decl(p);
            if (d->kind != TopDecl::Kind::Fun)
                throw p.error(p.cur(), "only fun declarations allowed inside impl");
            id->methods.push_back(std::unique_ptr<FunDecl>(
                static_cast<FunDecl*>(d.release())));
        }
        p.expect(TK::RBrace);
        return id;
    }
    // функция
    if (p.at(TK::Fun)) {
        p.advance();
        auto name_tok = p.expect(TK::Ident);
        p.expect(TK::LParen);
        std::vector<Param> params;
        if (!p.at(TK::RParen)) {
            while (true) {
                auto pname = p.expect(TK::Ident);
                p.expect(TK::Colon);
                auto ptype = parse_type(p);
                Param param;
                param.name = pname.lexeme;
                param.type = std::move(ptype);
                param.loc  = {p.filename, pname.line, pname.col};
                params.push_back(std::move(param));
                if (!p.at(TK::Comma)) break;
                p.advance();
            }
        }
        p.expect(TK::RParen);
        p.expect(TK::Colon);
        auto ret = parse_type(p);
        auto body = parse_block(p);
        auto fd = std::make_unique<FunDecl>();
        fd->kind     = TopDecl::Kind::Fun;
        fd->loc      = loc;
        fd->name     = name_tok.lexeme;
        fd->params   = std::move(params);
        fd->ret_type = std::move(ret);
        fd->body     = std::move(body);
        return fd;
    }
    throw p.error(p.cur(), "expected top-level declaration, got '" + p.cur().lexeme + "'");
}

// ── entry point 
static void debug_tree(const Program& prog) {
    std::cerr << "[DBG] decls: " << prog.decls.size() << "\n";
    for (size_t i=0;i<prog.decls.size();++i)
        std::cerr << "[DBG]  " << i << " kind=" << (int)prog.decls[i]->kind << "\n";
}

std::variant<Program, ParseError>
parse(const std::vector<Lexer::Token>& tokens, const std::string& filename) {
    P p{tokens, filename};
    try {
        Program prog;
        while (!p.at(TK::Eof))
            prog.decls.push_back(parse_top_decl(p));
        return prog;
    } catch (const ParseError& e) {
        return e;
    }
}

// ── AST печать (для --dump-ast) 
static void indent(std::ostream& out, int d) {
    for (int i = 0; i < d; ++i) out << "  ";
}
static void dump_type(std::ostream& out, const TypePtr& t) {
    out << (t ? t->to_string() : "?");
}
static void dump_expr(std::ostream& out, const ExprPtr& e, int d);
static void dump_stmt(std::ostream& out, const StmtPtr& s, int d);

static void dump_expr(std::ostream& out, const ExprPtr& e, int d) {
    if (!e) { indent(out,d); out << "<null>\n"; return; }
    indent(out, d);
    switch (e->kind) {
    case Expr::Kind::IntLit:
        out << "IntLit(" << static_cast<IntLitExpr&>(*e).value << ")\n"; break;
    case Expr::Kind::FloatLit:
        out << "FloatLit(" << static_cast<FloatLitExpr&>(*e).value << ")\n"; break;
    case Expr::Kind::BoolLit:
        out << "BoolLit(" << (static_cast<BoolLitExpr&>(*e).value ? "true" : "false") << ")\n"; break;
    case Expr::Kind::StringLit:
        out << "StringLit(\"" << static_cast<StringLitExpr&>(*e).value << "\")\n"; break;
    case Expr::Kind::Ident:
        out << "Ident(" << static_cast<IdentExpr&>(*e).name << ")\n"; break;
    case Expr::Kind::NamespaceAccess: {
        auto& na = static_cast<NamespaceAccessExpr&>(*e);
        out << "NS(" << na.ns_name << "::" << na.member << ")\n"; break;
    }
    case Expr::Kind::BinOp: {
        auto& b = static_cast<BinOpExpr&>(*e);
        out << "BinOp(" << static_cast<int>(b.op) << ")\n";
        dump_expr(out, b.lhs, d+1); dump_expr(out, b.rhs, d+1); break;
    }
    case Expr::Kind::UnaryOp: {
        auto& u = static_cast<UnaryOpExpr&>(*e);
        out << "UnaryOp(" << (u.op==UnaryOpKind::Neg?"-":"not") << ")\n";
        dump_expr(out, u.operand, d+1); break;
    }
    case Expr::Kind::Cast: {
        auto& c = static_cast<CastExpr&>(*e);
        out << "Cast(as "; dump_type(out, c.target); out << ")\n";
        dump_expr(out, c.operand, d+1); break;
    }
    case Expr::Kind::Call: {
        auto& c = static_cast<CallExpr&>(*e);
        out << "Call\n";
        dump_expr(out, c.callee, d+1);
        for (auto& a : c.args) dump_expr(out, a, d+1);
        break;
    }
    case Expr::Kind::Index: {
        auto& i = static_cast<IndexExpr&>(*e);
        out << "Index\n";
        dump_expr(out, i.arr, d+1); dump_expr(out, i.idx, d+1); break;
    }
    case Expr::Kind::Field: {
        auto& f = static_cast<FieldExpr&>(*e);
        out << "Field(." << f.field_name << ")\n";
        dump_expr(out, f.object, d+1); break;
    }
    default: out << "Expr(?)\n"; break;
    }
}

static void dump_stmt(std::ostream& out, const StmtPtr& s, int d) {
    if (!s) return;
    indent(out, d);
    switch (s->kind) {
    case Stmt::Kind::Empty: out << "Empty\n"; break;
    case Stmt::Kind::VarDecl: {
        auto& v = static_cast<VarDeclStmt&>(*s);
        out << (v.is_val?"Val":"Var") << "(" << v.name << ")\n";
        dump_expr(out, v.init, d+1); break;
    }
    case Stmt::Kind::Assign: {
        auto& a = static_cast<AssignStmt&>(*s);
        out << "Assign\n";
        dump_expr(out, a.value, d+1); break;
    }
    case Stmt::Kind::ExprStmt: {
        out << "ExprStmt\n";
        dump_expr(out, static_cast<ExprStmt&>(*s).expr, d+1); break;
    }
    case Stmt::Kind::If: {
        auto& i = static_cast<IfStmt&>(*s);
        out << "If\n";
        dump_expr(out, i.cond, d+1);
        dump_stmt(out, i.then_branch, d+1);
        if (i.else_branch) dump_stmt(out, i.else_branch, d+1);
        break;
    }
    case Stmt::Kind::While: {
        auto& w = static_cast<WhileStmt&>(*s);
        out << "While\n";
        dump_expr(out, w.cond, d+1);
        dump_stmt(out, w.body, d+1); break;
    }
    case Stmt::Kind::Block: {
        out << "Block\n";
        for (auto& st : static_cast<BlockStmt&>(*s).stmts)
            dump_stmt(out, st, d+1);
        break;
    }
    case Stmt::Kind::Break:    out << "Break\n";    break;
    case Stmt::Kind::Continue: out << "Continue\n"; break;
    case Stmt::Kind::Return: {
        out << "Return\n";
        if (static_cast<ReturnStmt&>(*s).value)
            dump_expr(out, static_cast<ReturnStmt&>(*s).value, d+1);
        break;
    }
    case Stmt::Kind::ForRange: {
        auto& fr = static_cast<ForRangeStmt&>(*s);
        out << "ForRange(" << fr.var_name << ")\n";
        dump_stmt(out, fr.body, d+1);
        break;
    }
    case Stmt::Kind::ForC: {
        out << "ForC\n";
        dump_stmt(out, static_cast<ForCStmt&>(*s).body, d+1);
        break;
    }
    }
}

void dump_ast(const Program& prog, std::ostream& out) {
    for (auto& d : prog.decls) {
        switch (d->kind) {
        case TopDecl::Kind::Fun: {
            auto& f = static_cast<FunDecl&>(*d);
            out << "FunDecl " << f.name << "(";
            for (size_t i=0;i<f.params.size();++i) {
                if(i) out<<",";
                out << f.params[i].name << ":" << f.params[i].type->to_string();
            }
            out << "): " << f.ret_type->to_string() << "\n";
            indent(out,1); out << "Block\n";
            for (auto& st : f.body->stmts) dump_stmt(out, st, 2);
            break;
        }
        case TopDecl::Kind::Struct: {
            auto& s = static_cast<StructDecl&>(*d);
            out << "StructDecl " << s.name << "\n";
            for (auto& f : s.fields)
                out << "  " << f.name << ": " << f.type->to_string() << "\n";
            break;
        }
        case TopDecl::Kind::TypeAlias: {
            auto& ta = static_cast<TypeAliasDecl&>(*d);
            out << "TypeAlias " << ta.name << " = " << ta.aliased->to_string() << "\n";
            break;
        }
        case TopDecl::Kind::Namespace: {
            auto& ns = static_cast<NamespaceDecl&>(*d);
            out << "Namespace " << ns.name << "\n";
            break;
        }
        case TopDecl::Kind::GlobalVar: {
            auto& gv = static_cast<GlobalVarDecl&>(*d);
            out << (gv.is_val?"Val":"Var") << "Decl " << gv.name << "\n";
            dump_expr(out, gv.init, 1);
            break;
        }
        case TopDecl::Kind::Impl: {
            auto& id = static_cast<ImplDecl&>(*d);
            out << "Impl " << id.type_name << " (" << id.methods.size() << " методов)\n";
            break;
        }
        }
    }
}

} 
