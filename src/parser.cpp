/*
 * Синтаксический анализатор BestLang — рекурсивный спуск.
 *
 * Грамматика реализована набором взаимно рекурсивных функций:
 *   parse_expr -> parse_bin -> parse_unary -> parse_postfix -> parse_primary
 * Приоритет операторов задаётся через числовые уровни (precedence climbing).
 */

#include "parser.hpp"
#include <sstream>
#include <cassert>
#include <stdexcept>

namespace Parser {

using TK = Lexer::TokenKind;

// внутреннее состояние парсера
struct P {
    const std::vector<Lexer::Token>& toks;
    const std::string& filename;
    size_t pos = 0;
    std::optional<ParseError> err;

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

    bool has_error() const { return err.has_value(); }

    void set_error(const Lexer::Token& t, const std::string& msg) {
        if (!err) err = ParseError{filename, t.line, t.col, msg};
    }

    Lexer::Token expect(TK k) {
        if (!at(k)) {
            auto msg = "expected '" + Lexer::token_kind_to_string(k) +
                       "' but got '" + cur().lexeme + "'";
            set_error(cur(), msg);
            return cur();
        }
        return advance();
    }

    SrcLoc loc() const { return {filename, cur().line, cur().col}; }
};

static ExprPtr    parse_expr(P& p);
static StmtPtr    parse_stmt(P& p);
static std::unique_ptr<BlockStmt> parse_block(P& p);
static TypePtr    parse_type(P& p);
static TopDeclPtr parse_top_decl(P& p);

// разбор типов

static TypePtr token_to_base_type(P& p, const Lexer::Token& tok) {
    switch (tok.kind) {
    case TK::Int8:    return TYPE_INT8;
    case TK::Int16:   return TYPE_INT16;
    case TK::Int32:   return TYPE_INT32;
    case TK::Int64:   return TYPE_INT64;
    case TK::UInt8:   return TYPE_UINT8;
    case TK::UInt16:  return TYPE_UINT16;
    case TK::UInt32:  return TYPE_UINT32;
    case TK::UInt64:  return TYPE_UINT64;
    case TK::Float32: return TYPE_FLOAT32;
    case TK::Float64: return TYPE_FLOAT64;
    case TK::Bool:    return TYPE_BOOL;
    case TK::Char:    return TYPE_CHAR;
    case TK::String:  return TYPE_STRING;
    case TK::Void:    return TYPE_VOID;
    case TK::Ident:
        if (p.at(TK::Lt)) {
            std::string base = tok.lexeme;
            p.advance();
            std::vector<TypePtr> args;
            args.push_back(parse_type(p));
            if (p.has_error()) return nullptr;
            while (p.at(TK::Comma)) {
                p.advance();
                args.push_back(parse_type(p));
                if (p.has_error()) return nullptr;
            }
            p.expect(TK::Gt);
            if (p.has_error()) return nullptr;
            return Type::make_generic_inst(base, std::move(args));
        }
        return Type::make_struct(tok.lexeme);
    default:
        p.set_error(tok, "expected type, got '" + tok.lexeme + "'");
        return nullptr;
    }
}

static TypePtr parse_array_or_dynarray_type(P& p) {
    if (p.has_error()) return nullptr;
    p.advance(); // '['
    auto elem = parse_type(p);
    if (p.has_error()) return nullptr;
    if (p.at(TK::Semicolon)) {
        p.advance();
        auto n_tok = p.expect(TK::IntLit);
        if (p.has_error()) return nullptr;
        p.expect(TK::RBracket);
        if (p.has_error()) return nullptr;
        return Type::make_array(std::move(elem), n_tok.num.i32);
    }
    p.expect(TK::RBracket);
    if (p.has_error()) return nullptr;
    return Type::make_dynarray(std::move(elem));
}

static TypePtr parse_tuple_type(P& p) {
    if (p.has_error()) return nullptr;
    p.advance(); // '('
    std::vector<TypePtr> elems;
    if (!p.at(TK::RParen)) {
        elems.push_back(parse_type(p));
        if (p.has_error()) return nullptr;
        while (p.at(TK::Comma)) {
            p.advance();
            if (p.at(TK::RParen)) break;
            elems.push_back(parse_type(p));
            if (p.has_error()) return nullptr;
        }
    }
    p.expect(TK::RParen);
    if (p.has_error()) return nullptr;
    return Type::make_tuple(std::move(elems));
}

static TypePtr parse_type(P& p) {
    if (p.has_error()) return nullptr;
    TypePtr base;
    switch (p.cur().kind) {
    case TK::LBracket: base = parse_array_or_dynarray_type(p); break;
    case TK::LParen:   base = parse_tuple_type(p); break;
    default: {
        auto tok = p.cur(); p.advance();
        base = token_to_base_type(p, tok);
        break;
    }
    }
    if (p.has_error()) return nullptr;
    while (p.at(TK::Question)) {
        p.advance();
        base = Type::make_nullable(std::move(base));
    }
    return base;
}

// первичные выражения

static ExprPtr parse_int_lit(P& p, SrcLoc loc) {
    auto tok = p.advance();
    auto e = std::make_unique<IntLitExpr>();
    e->kind = Expr::Kind::IntLit; e->loc = loc;
    e->value = tok.as_int(); e->suffix = tok.suffix;
    return e;
}

static ExprPtr parse_float_lit(P& p, SrcLoc loc) {
    auto tok = p.advance();
    auto e = std::make_unique<FloatLitExpr>();
    e->kind = Expr::Kind::FloatLit; e->loc = loc;
    e->value = tok.as_float(); e->suffix = tok.suffix;
    return e;
}

static ExprPtr parse_bool_lit(P& p, SrcLoc loc) {
    bool v = p.at(TK::True);
    p.advance();
    auto e = std::make_unique<BoolLitExpr>();
    e->kind = Expr::Kind::BoolLit; e->loc = loc; e->value = v;
    return e;
}

static ExprPtr parse_null_lit(P& p, SrcLoc loc) {
    p.advance();
    auto e = std::make_unique<NullLitExpr>();
    e->kind = Expr::Kind::NullLit; e->loc = loc;
    return e;
}

static ExprPtr parse_string_lit(P& p, SrcLoc loc) {
    auto tok = p.advance();
    auto e = std::make_unique<StringLitExpr>();
    e->kind = Expr::Kind::StringLit; e->loc = loc; e->value = tok.str_val;
    return e;
}

static ExprPtr parse_char_lit(P& p, SrcLoc loc) {
    auto tok = p.advance();
    auto e = std::make_unique<CharLitExpr>();
    e->kind = Expr::Kind::CharLit; e->loc = loc; e->value = (char)tok.num.u8;
    return e;
}

static ExprPtr parse_array_lit(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    p.advance(); // '['
    auto e = std::make_unique<ArrayLitExpr>();
    e->kind = Expr::Kind::ArrayLit; e->loc = loc;
    if (!p.at(TK::RBracket)) {
        e->elements.push_back(parse_expr(p));
        if (p.has_error()) return nullptr;
        while (p.at(TK::Comma)) {
            p.advance();
            if (p.at(TK::RBracket)) break;
            e->elements.push_back(parse_expr(p));
            if (p.has_error()) return nullptr;
        }
    }
    p.expect(TK::RBracket);
    if (p.has_error()) return nullptr;
    return e;
}

static ExprPtr parse_struct_lit(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    auto name_tok = p.advance(); // IDENT
    std::vector<TypePtr> type_args;
    if (p.at(TK::Lt)) {
        p.advance();
        type_args.push_back(parse_type(p));
        if (p.has_error()) return nullptr;
        while (p.at(TK::Comma)) {
            p.advance();
            type_args.push_back(parse_type(p));
            if (p.has_error()) return nullptr;
        }
        p.expect(TK::Gt);
        if (p.has_error()) return nullptr;
    }
    p.advance(); // '{'
    auto e = std::make_unique<StructLitExpr>();
    e->kind = Expr::Kind::StructLit; e->loc = loc;
    e->type_name = name_tok.lexeme; e->type_args = std::move(type_args);
    if (!p.at(TK::RBrace)) {
        while (true) {
            auto fn_tok = p.expect(TK::Ident);
            if (p.has_error()) return nullptr;
            p.expect(TK::Colon);
            if (p.has_error()) return nullptr;
            StructLitField f;
            f.name = fn_tok.lexeme; f.value = parse_expr(p);
            if (p.has_error()) return nullptr;
            e->fields.push_back(std::move(f));
            if (!p.at(TK::Comma)) break;
            p.advance();
            if (p.at(TK::RBrace)) break;
        }
    }
    p.expect(TK::RBrace);
    if (p.has_error()) return nullptr;
    return e;
}

static ExprPtr parse_ident_expr(P& p, SrcLoc loc) {
    auto name_tok = p.advance();
    if (p.at(TK::ColonColon)) {
        p.advance();
        auto mem_tok = p.expect(TK::Ident);
        if (p.has_error()) return nullptr;
        auto e = std::make_unique<NamespaceAccessExpr>();
        e->kind = Expr::Kind::NamespaceAccess; e->loc = loc;
        e->ns_name = name_tok.lexeme; e->member = mem_tok.lexeme;
        return e;
    }
    auto e = std::make_unique<IdentExpr>();
    e->kind = Expr::Kind::Ident; e->loc = loc; e->name = name_tok.lexeme;
    return e;
}

static ExprPtr parse_paren_or_tuple(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    p.advance(); // '('
    if (p.at(TK::RParen)) {
        p.advance();
        auto e = std::make_unique<TupleLitExpr>();
        e->kind = Expr::Kind::TupleLit; e->loc = loc;
        return e;
    }
    auto first = parse_expr(p);
    if (p.has_error()) return nullptr;
    if (!p.at(TK::Comma)) {
        p.expect(TK::RParen);
        if (p.has_error()) return nullptr;
        return first;
    }
    auto e = std::make_unique<TupleLitExpr>();
    e->kind = Expr::Kind::TupleLit; e->loc = loc;
    e->elements.push_back(std::move(first));
    while (p.at(TK::Comma)) {
        p.advance();
        if (p.at(TK::RParen)) break;
        e->elements.push_back(parse_expr(p));
        if (p.has_error()) return nullptr;
    }
    p.expect(TK::RParen);
    if (p.has_error()) return nullptr;
    return e;
}

static bool looks_like_generic_struct_lit(const P& p) {
    size_t i = p.pos + 2;
    int depth = 0;
    while (i < p.toks.size()) {
        auto k = p.toks[i].kind;
        if (k == TK::Lt)  { ++depth; ++i; continue; }
        if (k == TK::Gt)  {
            if (depth == 0) {
                return i + 1 < p.toks.size() && p.toks[i + 1].kind == TK::LBrace;
            }
            --depth; ++i; continue;
        }
        if (k == TK::Eof || k == TK::Semicolon || k == TK::LBrace) break;
        ++i;
    }
    return false;
}

static ExprPtr parse_primary(P& p) {
    if (p.has_error()) return nullptr;
    auto loc = p.loc();
    switch (p.cur().kind) {
    case TK::IntLit:    return parse_int_lit(p, loc);
    case TK::FloatLit:  return parse_float_lit(p, loc);
    case TK::True:
    case TK::False:     return parse_bool_lit(p, loc);
    case TK::Null:      return parse_null_lit(p, loc);
    case TK::StringLit: return parse_string_lit(p, loc);
    case TK::CharLit:   return parse_char_lit(p, loc);
    case TK::LBracket:  return parse_array_lit(p, loc);
    case TK::Ident:
        if (p.peek(1).kind == TK::LBrace &&
            (p.peek(2).kind == TK::RBrace ||
             (p.peek(2).kind == TK::Ident && p.peek(3).kind == TK::Colon)))
            return parse_struct_lit(p, loc);
        if (p.peek(1).kind == TK::Lt && looks_like_generic_struct_lit(p))
            return parse_struct_lit(p, loc);
        return parse_ident_expr(p, loc);
    case TK::LParen:    return parse_paren_or_tuple(p, loc);
    default:
        p.set_error(p.cur(), "expected expression, got '" + p.cur().lexeme + "'");
        return nullptr;
    }
}

// постфиксные операции

static ExprPtr parse_call_postfix(P& p, SrcLoc loc, ExprPtr callee) {
    if (p.has_error()) return nullptr;
    p.advance(); // '('
    auto call = std::make_unique<CallExpr>();
    call->kind = Expr::Kind::Call; call->loc = loc; call->callee = std::move(callee);
    auto parse_one_arg = [&]() {
        if (p.at(TK::Ident) && p.peek(1).kind == TK::Assign) {
            auto name_tok = p.advance();
            p.advance();
            NamedArg na;
            na.name = name_tok.lexeme; na.value = parse_expr(p);
            call->named_args.push_back(std::move(na));
        } else {
            call->args.push_back(parse_expr(p));
        }
    };
    if (!p.at(TK::RParen)) {
        parse_one_arg();
        if (p.has_error()) return nullptr;
        while (p.at(TK::Comma)) {
            p.advance();
            if (p.at(TK::RParen)) break;
            parse_one_arg();
            if (p.has_error()) return nullptr;
        }
    }
    p.expect(TK::RParen);
    if (p.has_error()) return nullptr;
    return call;
}

static ExprPtr parse_index_postfix(P& p, SrcLoc loc, ExprPtr arr) {
    if (p.has_error()) return nullptr;
    p.advance(); // '['
    auto idx = std::make_unique<IndexExpr>();
    idx->kind = Expr::Kind::Index; idx->loc = loc;
    idx->arr = std::move(arr); idx->idx = parse_expr(p);
    if (p.has_error()) return nullptr;
    p.expect(TK::RBracket);
    if (p.has_error()) return nullptr;
    return idx;
}

static ExprPtr parse_dot_postfix(P& p, SrcLoc loc, ExprPtr obj) {
    if (p.has_error()) return nullptr;
    p.advance(); // '.'
    if (p.at(TK::IntLit)) {
        auto idx_tok = p.advance();
        auto te = std::make_unique<TupleIndexExpr>();
        te->kind = Expr::Kind::TupleIndex; te->loc = loc;
        te->object = std::move(obj); te->index = static_cast<int>(idx_tok.as_int());
        return te;
    }
    auto fn_tok = p.expect(TK::Ident);
    if (p.has_error()) return nullptr;
    auto fe = std::make_unique<FieldExpr>();
    fe->kind = Expr::Kind::Field; fe->loc = loc;
    fe->object = std::move(obj); fe->field_name = fn_tok.lexeme;
    return fe;
}

static ExprPtr parse_postfix(P& p) {
    if (p.has_error()) return nullptr;
    auto e = parse_primary(p);
    while (!p.has_error()) {
        auto loc = p.loc();
        switch (p.cur().kind) {
        case TK::LParen:   e = parse_call_postfix(p, loc, std::move(e));  break;
        case TK::LBracket: e = parse_index_postfix(p, loc, std::move(e)); break;
        case TK::Dot:      e = parse_dot_postfix(p, loc, std::move(e));   break;
        default: return e;
        }
    }
    return nullptr;
}

static ExprPtr parse_unary(P& p) {
    if (p.has_error()) return nullptr;
    if (!p.at(TK::Minus))
        return parse_postfix(p);
    auto loc = p.loc();
    p.advance();
    auto e = std::make_unique<UnaryOpExpr>();
    e->kind = Expr::Kind::UnaryOp; e->loc = loc;
    e->op = UnaryOpKind::Neg; e->operand = parse_unary(p);
    if (p.has_error()) return nullptr;
    return e;
}

enum class Prec : int { As=1, Or=2, And=3, Not=4, Cmp=5, Add=6, Mul=7 };

static int bin_prec(TK k) {
    switch (k) {
    case TK::Star: case TK::Slash: case TK::Percent: return int(Prec::Mul);
    case TK::Plus: case TK::Minus:                   return int(Prec::Add);
    case TK::EqEq: case TK::BangEq:
    case TK::Lt:   case TK::LtEq:
    case TK::Gt:   case TK::GtEq:                   return int(Prec::Cmp);
    case TK::And:                                    return int(Prec::And);
    case TK::Or:                                     return int(Prec::Or);
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
    default: throw std::runtime_error("bad bin op"); // недостижимая ветка
    }
}

static ExprPtr parse_bin(P& p, int min_prec) {
    if (p.has_error()) return nullptr;
    if (p.at(TK::Not) && min_prec <= int(Prec::Not)) {
        auto loc = p.loc();
        p.advance();
        auto e = std::make_unique<UnaryOpExpr>();
        e->kind = Expr::Kind::UnaryOp; e->loc = loc;
        e->op = UnaryOpKind::Not; e->operand = parse_bin(p, int(Prec::Not));
        if (p.has_error()) return nullptr;
        return e;
    }
    auto lhs = parse_unary(p);
    while (!p.has_error()) {
        int prec = bin_prec(p.cur().kind);
        if (prec < min_prec) break;
        auto loc = p.loc();
        auto op  = bin_op_kind(p.cur().kind);
        p.advance();
        auto rhs = parse_bin(p, prec + 1);
        if (p.has_error()) return nullptr;
        auto e = std::make_unique<BinOpExpr>();
        e->kind = Expr::Kind::BinOp; e->loc = loc;
        e->op = op; e->lhs = std::move(lhs); e->rhs = std::move(rhs);
        lhs = std::move(e);
        if (prec == int(Prec::Cmp) && bin_prec(p.cur().kind) == int(Prec::Cmp)) {
            p.set_error(p.cur(), "comparison operators are non-chaining; use parentheses");
            return nullptr;
        }
    }
    return lhs;
}

static ExprPtr parse_expr(P& p) {
    if (p.has_error()) return nullptr;
    auto e = parse_bin(p, int(Prec::Or));
    while (!p.has_error() && p.at(TK::As)) {
        auto loc = p.loc();
        p.advance();
        auto cast = std::make_unique<CastExpr>();
        cast->kind = Expr::Kind::Cast; cast->loc = loc;
        cast->operand = std::move(e); cast->target = parse_type(p);
        if (p.has_error()) return nullptr;
        e = std::move(cast);
    }
    return e;
}

static std::unique_ptr<BlockStmt> parse_block(P& p) {
    if (p.has_error()) return nullptr;
    auto blk = std::make_unique<BlockStmt>();
    blk->kind = Stmt::Kind::Block; blk->loc = p.loc();
    p.expect(TK::LBrace);
    if (p.has_error()) return nullptr;
    while (!p.at(TK::RBrace) && !p.at(TK::Eof) && !p.has_error())
        blk->stmts.push_back(parse_stmt(p));
    if (p.has_error()) return nullptr;
    p.expect(TK::RBrace);
    if (p.has_error()) return nullptr;
    return blk;
}

static LValuePtr expr_to_lvalue(P& p, ExprPtr& e) {
    if (p.has_error()) return nullptr;
    switch (e->kind) {
    case Expr::Kind::Ident: {
        auto& id = static_cast<IdentExpr&>(*e);
        auto lv = std::make_unique<IdentLValue>();
        lv->kind = LValue::Kind::Ident; lv->loc = id.loc; lv->name = id.name;
        return lv;
    }
    case Expr::Kind::Index: {
        auto& ix = static_cast<IndexExpr&>(*e);
        auto lv = std::make_unique<IndexLValue>();
        lv->kind = LValue::Kind::Index; lv->loc = ix.loc;
        lv->base = expr_to_lvalue(p, ix.arr); lv->idx = std::move(ix.idx);
        return lv;
    }
    case Expr::Kind::Field: {
        auto& fx = static_cast<FieldExpr&>(*e);
        auto lv = std::make_unique<FieldLValue>();
        lv->kind = LValue::Kind::Field; lv->loc = fx.loc;
        lv->base = expr_to_lvalue(p, fx.object); lv->field = fx.field_name;
        return lv;
    }
    default:
        p.set_error(p.cur(), "invalid assignment target");
        return nullptr;
    }
}

// инструкции

static StmtPtr parse_var_decl(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    bool is_val = p.at(TK::Val);
    p.advance();
    auto name_tok = p.expect(TK::Ident);
    if (p.has_error()) return nullptr;
    TypePtr ann;
    if (p.at(TK::Colon)) { p.advance(); ann = parse_type(p); }
    if (p.has_error()) return nullptr;
    auto s = std::make_unique<VarDeclStmt>();
    s->kind = Stmt::Kind::VarDecl; s->loc = loc;
    s->name = name_tok.lexeme; s->ann_type = std::move(ann); s->is_val = is_val;
    if (p.at(TK::Assign)) {
        p.advance();
        s->init = parse_expr(p);
    } else {
        s->init = nullptr; // семантика заполнит нулями если тип позволяет
    }
    if (p.has_error()) return nullptr;
    p.expect(TK::Semicolon);
    if (p.has_error()) return nullptr;
    return s;
}

static StmtPtr parse_if(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    p.advance();
    auto cond = parse_expr(p);
    if (p.has_error()) return nullptr;
    auto then = parse_block(p);
    if (p.has_error()) return nullptr;
    StmtPtr els;
    if (p.at(TK::Else)) {
        p.advance();
        els = p.at(TK::If) ? parse_stmt(p) : StmtPtr(parse_block(p));
        if (p.has_error()) return nullptr;
    }
    auto s = std::make_unique<IfStmt>();
    s->kind = Stmt::Kind::If; s->loc = loc;
    s->cond = std::move(cond); s->then_branch = std::move(then); s->else_branch = std::move(els);
    return s;
}

static StmtPtr parse_while(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    p.advance();
    auto s = std::make_unique<WhileStmt>();
    s->kind = Stmt::Kind::While; s->loc = loc;
    s->cond = parse_expr(p);
    if (p.has_error()) return nullptr;
    s->body = parse_block(p);
    if (p.has_error()) return nullptr;
    return s;
}

static StmtPtr parse_assign_or_expr_nosemi(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    auto e = parse_expr(p);
    if (p.has_error()) return nullptr;
    if (!p.at(TK::Assign)) {
        auto s = std::make_unique<ExprStmt>();
        s->kind = Stmt::Kind::ExprStmt; s->loc = loc; s->expr = std::move(e);
        return s;
    }
    p.advance();
    auto lv = expr_to_lvalue(p, e);
    if (p.has_error()) return nullptr;
    auto rhs = parse_expr(p);
    if (p.has_error()) return nullptr;
    auto s = std::make_unique<AssignStmt>();
    s->kind = Stmt::Kind::Assign; s->loc = loc;
    s->target = std::move(lv); s->value = std::move(rhs);
    return s;
}

static StmtPtr parse_for_range(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    auto var_tok = p.advance();
    p.advance(); // 'in'
    auto s = std::make_unique<ForRangeStmt>();
    s->kind = Stmt::Kind::ForRange; s->loc = loc; s->var_name = var_tok.lexeme;
    s->start = parse_expr(p);
    if (p.has_error()) return nullptr;
    p.expect(TK::DotDot);
    if (p.has_error()) return nullptr;
    s->end = parse_expr(p);
    if (p.has_error()) return nullptr;
    s->body = parse_block(p);
    if (p.has_error()) return nullptr;
    return s;
}

static StmtPtr parse_for_c(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    StmtPtr init;
    if (p.at(TK::Var) || p.at(TK::Val)) {
        bool iv = p.at(TK::Val); p.advance();
        auto nm = p.expect(TK::Ident);
        if (p.has_error()) return nullptr;
        TypePtr ann;
        if (p.at(TK::Colon)) { p.advance(); ann = parse_type(p); }
        if (p.has_error()) return nullptr;
        p.expect(TK::Assign);
        if (p.has_error()) return nullptr;
        auto vd = std::make_unique<VarDeclStmt>();
        vd->kind = Stmt::Kind::VarDecl; vd->loc = loc;
        vd->name = nm.lexeme; vd->ann_type = std::move(ann);
        vd->init = parse_expr(p); vd->is_val = iv;
        if (p.has_error()) return nullptr;
        init = std::move(vd);
    } else {
        init = parse_assign_or_expr_nosemi(p, loc);
        if (p.has_error()) return nullptr;
    }
    p.expect(TK::Semicolon);
    if (p.has_error()) return nullptr;
    auto cond = parse_expr(p);
    if (p.has_error()) return nullptr;
    p.expect(TK::Semicolon);
    if (p.has_error()) return nullptr;
    auto step = parse_assign_or_expr_nosemi(p, loc);
    if (p.has_error()) return nullptr;
    auto s = std::make_unique<ForCStmt>();
    s->kind = Stmt::Kind::ForC; s->loc = loc;
    s->init = std::move(init); s->cond = std::move(cond);
    s->step = std::move(step); s->body = parse_block(p);
    if (p.has_error()) return nullptr;
    return s;
}

static StmtPtr parse_for(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    p.advance(); // 'for'
    if (p.at(TK::Ident) && p.peek(1).kind == TK::In)
        return parse_for_range(p, loc);
    return parse_for_c(p, loc);
}

static StmtPtr parse_return(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    p.advance();
    auto s = std::make_unique<ReturnStmt>();
    s->kind = Stmt::Kind::Return; s->loc = loc;
    if (!p.at(TK::Semicolon)) {
        s->value = parse_expr(p);
        if (p.has_error()) return nullptr;
    }
    p.expect(TK::Semicolon);
    if (p.has_error()) return nullptr;
    return s;
}

static StmtPtr parse_assign_or_expr(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    auto s = parse_assign_or_expr_nosemi(p, loc);
    if (p.has_error()) return nullptr;
    p.expect(TK::Semicolon);
    if (p.has_error()) return nullptr;
    return s;
}

static StmtPtr parse_stmt(P& p) {
    if (p.has_error()) return nullptr;
    auto loc = p.loc();
    switch (p.cur().kind) {
    case TK::Semicolon: {
        p.advance();
        auto s = std::make_unique<EmptyStmt>();
        s->kind = Stmt::Kind::Empty; s->loc = loc;
        return s;
    }
    case TK::LBrace:    return parse_block(p);
    case TK::Var:
    case TK::Val:       return parse_var_decl(p, loc);
    case TK::If:        return parse_if(p, loc);
    case TK::While:     return parse_while(p, loc);
    case TK::Break: {
        p.advance(); p.expect(TK::Semicolon);
        if (p.has_error()) return nullptr;
        auto s = std::make_unique<BreakStmt>();
        s->kind = Stmt::Kind::Break; s->loc = loc;
        return s;
    }
    case TK::Continue: {
        p.advance(); p.expect(TK::Semicolon);
        if (p.has_error()) return nullptr;
        auto s = std::make_unique<ContinueStmt>();
        s->kind = Stmt::Kind::Continue; s->loc = loc;
        return s;
    }
    case TK::For:       return parse_for(p, loc);
    case TK::Return:    return parse_return(p, loc);
    default:            return parse_assign_or_expr(p, loc);
    }
}

// объявления верхнего уровня

static TopDeclPtr parse_struct_decl(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    p.advance();
    auto name_tok = p.expect(TK::Ident);
    if (p.has_error()) return nullptr;
    auto sd = std::make_unique<StructDecl>();
    sd->kind = TopDecl::Kind::Struct; sd->loc = loc; sd->name = name_tok.lexeme;
    if (p.at(TK::Lt)) {
        p.advance();
        sd->type_params.push_back(p.expect(TK::Ident).lexeme);
        if (p.has_error()) return nullptr;
        while (p.at(TK::Comma)) {
            p.advance();
            sd->type_params.push_back(p.expect(TK::Ident).lexeme);
            if (p.has_error()) return nullptr;
        }
        p.expect(TK::Gt);
        if (p.has_error()) return nullptr;
    }
    p.expect(TK::LBrace);
    if (p.has_error()) return nullptr;
    while (!p.at(TK::RBrace) && !p.at(TK::Eof) && !p.has_error()) {
        auto fn_tok = p.expect(TK::Ident);
        if (p.has_error()) return nullptr;
        p.expect(TK::Colon);
        if (p.has_error()) return nullptr;
        FieldDecl fd;
        fd.name = fn_tok.lexeme; fd.type = parse_type(p);
        if (p.has_error()) return nullptr;
        fd.loc  = {p.filename, fn_tok.line, fn_tok.col};
        p.expect(TK::Semicolon);
        if (p.has_error()) return nullptr;
        sd->fields.push_back(std::move(fd));
    }
    p.expect(TK::RBrace);
    if (p.has_error()) return nullptr;
    return sd;
}

static TopDeclPtr parse_type_alias(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    p.advance();
    auto name_tok = p.expect(TK::Ident);
    if (p.has_error()) return nullptr;
    p.expect(TK::Assign);
    if (p.has_error()) return nullptr;
    auto ta = std::make_unique<TypeAliasDecl>();
    ta->kind = TopDecl::Kind::TypeAlias; ta->loc = loc;
    ta->name = name_tok.lexeme; ta->aliased = parse_type(p);
    if (p.has_error()) return nullptr;
    p.expect(TK::Semicolon);
    if (p.has_error()) return nullptr;
    return ta;
}

static TopDeclPtr parse_namespace_decl(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    p.advance();
    auto name_tok = p.expect(TK::Ident);
    if (p.has_error()) return nullptr;
    p.expect(TK::LBrace);
    if (p.has_error()) return nullptr;
    auto nd = std::make_unique<NamespaceDecl>();
    nd->kind = TopDecl::Kind::Namespace; nd->loc = loc; nd->name = name_tok.lexeme;
    while (!p.at(TK::RBrace) && !p.at(TK::Eof) && !p.has_error()) {
        if (p.at(TK::Namespace)) {
            p.set_error(p.cur(), "вложенные пространства имён не поддерживаются");
            return nullptr;
        }
        nd->members.push_back(parse_top_decl(p));
    }
    if (p.has_error()) return nullptr;
    p.expect(TK::RBrace);
    if (p.has_error()) return nullptr;
    return nd;
}

static TopDeclPtr parse_global_var(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    bool is_val = p.at(TK::Val);
    p.advance();
    auto name_tok = p.expect(TK::Ident);
    if (p.has_error()) return nullptr;
    TypePtr ann;
    if (p.at(TK::Colon)) { p.advance(); ann = parse_type(p); }
    if (p.has_error()) return nullptr;
    p.expect(TK::Assign);
    if (p.has_error()) return nullptr;
    auto gv = std::make_unique<GlobalVarDecl>();
    gv->kind = TopDecl::Kind::GlobalVar; gv->loc = loc;
    gv->name = name_tok.lexeme; gv->ann_type = std::move(ann);
    gv->init = parse_expr(p); gv->is_val = is_val;
    if (p.has_error()) return nullptr;
    p.expect(TK::Semicolon);
    if (p.has_error()) return nullptr;
    return gv;
}

static TopDeclPtr parse_impl_decl(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    p.advance();
    auto name_tok = p.expect(TK::Ident);
    if (p.has_error()) return nullptr;
    p.expect(TK::LBrace);
    if (p.has_error()) return nullptr;
    auto id = std::make_unique<ImplDecl>();
    id->kind = TopDecl::Kind::Impl; id->loc = loc; id->type_name = name_tok.lexeme;
    while (!p.at(TK::RBrace) && !p.at(TK::Eof) && !p.has_error()) {
        auto d = parse_top_decl(p);
        if (p.has_error()) return nullptr;
        if (d->kind != TopDecl::Kind::Fun) {
            p.set_error(p.cur(), "only fun declarations allowed inside impl");
            return nullptr;
        }
        id->methods.push_back(std::unique_ptr<FunDecl>(static_cast<FunDecl*>(d.release())));
    }
    if (p.has_error()) return nullptr;
    p.expect(TK::RBrace);
    if (p.has_error()) return nullptr;
    return id;
}

static std::vector<Param> parse_params(P& p) {
    if (p.has_error()) return {};
    std::vector<Param> params;
    p.expect(TK::LParen);
    if (p.has_error()) return {};
    if (!p.at(TK::RParen)) {
        while (true) {
            auto pname = p.expect(TK::Ident);
            if (p.has_error()) return {};
            p.expect(TK::Colon);
            if (p.has_error()) return {};
            Param param;
            param.name = pname.lexeme; param.type = parse_type(p);
            if (p.has_error()) return {};
            param.loc  = {p.filename, pname.line, pname.col};
            params.push_back(std::move(param));
            if (!p.at(TK::Comma)) break;
            p.advance();
        }
    }
    p.expect(TK::RParen);
    if (p.has_error()) return {};
    return params;
}

static TopDeclPtr parse_fun_decl(P& p, SrcLoc loc) {
    if (p.has_error()) return nullptr;
    p.advance();
    auto name_tok = p.expect(TK::Ident);
    if (p.has_error()) return nullptr;
    auto fd = std::make_unique<FunDecl>();
    fd->kind = TopDecl::Kind::Fun; fd->loc = loc; fd->name = name_tok.lexeme;
    if (p.at(TK::Lt)) {
        p.advance();
        fd->type_params.push_back(p.expect(TK::Ident).lexeme);
        if (p.has_error()) return nullptr;
        while (p.at(TK::Comma)) {
            p.advance();
            fd->type_params.push_back(p.expect(TK::Ident).lexeme);
            if (p.has_error()) return nullptr;
        }
        p.expect(TK::Gt);
        if (p.has_error()) return nullptr;
    }
    fd->params = parse_params(p);
    if (p.has_error()) return nullptr;
    p.expect(TK::Colon);
    if (p.has_error()) return nullptr;
    fd->ret_type = parse_type(p);
    if (p.has_error()) return nullptr;
    fd->body = parse_block(p);
    if (p.has_error()) return nullptr;
    return fd;
}

static TopDeclPtr parse_top_decl(P& p) {
    if (p.has_error()) return nullptr;
    auto loc = p.loc();
    switch (p.cur().kind) {
    case TK::Struct:    return parse_struct_decl(p, loc);
    case TK::Type:      return parse_type_alias(p, loc);
    case TK::Namespace: return parse_namespace_decl(p, loc);
    case TK::Var:
    case TK::Val:       return parse_global_var(p, loc);
    case TK::Impl:      return parse_impl_decl(p, loc);
    case TK::Fun:       return parse_fun_decl(p, loc);
    default:
        p.set_error(p.cur(), "expected top-level declaration, got '" + p.cur().lexeme + "'");
        return nullptr;
    }
}

// точка входа
std::expected<Program, ParseError> parse(const std::vector<Lexer::Token>& tokens, const std::string& filename) {
    P p{tokens, filename};
    Program prog;
    while (!p.at(TK::Eof) && !p.has_error())
        prog.decls.push_back(parse_top_decl(p));
    if (p.err) return std::unexpected(*p.err);
    return prog;
}

// AST печать (для --dump-ast)

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
    case Stmt::Kind::Assign:
        out << "Assign\n";
        dump_expr(out, static_cast<AssignStmt&>(*s).value, d+1); break;
    case Stmt::Kind::ExprStmt:
        out << "ExprStmt\n";
        dump_expr(out, static_cast<ExprStmt&>(*s).expr, d+1); break;
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
        dump_expr(out, w.cond, d+1); dump_stmt(out, w.body, d+1); break;
    }
    case Stmt::Kind::Block:
        out << "Block\n";
        for (auto& st : static_cast<BlockStmt&>(*s).stmts)
            dump_stmt(out, st, d+1);
        break;
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
        dump_stmt(out, fr.body, d+1); break;
    }
    case Stmt::Kind::ForC:
        out << "ForC\n";
        dump_stmt(out, static_cast<ForCStmt&>(*s).body, d+1); break;
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
        case TopDecl::Kind::Namespace:
            out << "Namespace " << static_cast<NamespaceDecl&>(*d).name << "\n"; break;
        case TopDecl::Kind::GlobalVar: {
            auto& gv = static_cast<GlobalVarDecl&>(*d);
            out << (gv.is_val?"Val":"Var") << "Decl " << gv.name << "\n";
            dump_expr(out, gv.init, 1); break;
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
