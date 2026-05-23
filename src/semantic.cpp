/*
 * Семантический анализатор BestLang.
 *
 * Работает в два прохода:
 *   1) Сбор всех объявлений верхнего уровня (чтобы функции могли вызывать друг друга)
 *   2) Анализ тел функций: разрешение имён, проверка типов, контроль потока
 *
 * Результат — аннотированный AST: каждый узел-выражение получает свой тип.
 */

#include "semantic.hpp"
#include <sstream>
#include <algorithm>
#include <functional>
#include <unordered_set>

namespace Semantic {

// Вспомогательные функции для дженериков

using TypeBindings = std::unordered_map<std::string, TypePtr>;

// Рекурсивная подстановка TypeParam → конкретный тип.
// Также обрабатывает StructType("T") — парсер хранит type params как StructType.
static TypePtr subst_type(TypePtr t, const TypeBindings& b) {
    if (!t) return nullptr;
    switch (t->kind) {
    case Type::Kind::TypeParam: {
        auto& tp = static_cast<TypeParamType&>(*t);
        auto it = b.find(tp.name);
        return it != b.end() ? it->second : t;
    }
    case Type::Kind::Struct: {
        // Если имя структуры есть в bindings — это TypeParam из generic
        auto& sn = struct_name(*t);
        auto it = b.find(sn);
        if (it != b.end()) return it->second;
        return t;
    }
    case Type::Kind::Array: {
        auto& at = static_cast<ArrayType&>(*t);
        return Type::make_array(subst_type(at.elem, b), at.size);
    }
    case Type::Kind::DynArray: {
        auto& dt = static_cast<DynArrayType&>(*t);
        return Type::make_dynarray(subst_type(dt.elem, b));
    }
    case Type::Kind::Nullable: {
        auto& nt = static_cast<NullableType&>(*t);
        return Type::make_nullable(subst_type(nt.elem, b));
    }
    case Type::Kind::Tuple: {
        auto& tt = static_cast<TupleType&>(*t);
        std::vector<TypePtr> elems;
        for (auto& e : tt.elems) elems.push_back(subst_type(e, b));
        return Type::make_tuple(std::move(elems));
    }
    case Type::Kind::GenericInst: {
        auto& gi = static_cast<GenericInstType&>(*t);
        std::vector<TypePtr> args;
        for (auto& a : gi.args) args.push_back(subst_type(a, b));
        return Type::make_generic_inst(gi.base, std::move(args));
    }
    default: return t;
    }
}

// Конвертирует StructType("T") → TypeParamType("T") для имён из списка type_params.
// Нужно потому что парсер не знает, какие идентификаторы — type params, а какие — structs.
static TypePtr convert_type_params(TypePtr t, const std::vector<std::string>& tps) {
    if (!t) return nullptr;
    switch (t->kind) {
    case Type::Kind::Struct: {
        auto& sn = struct_name(*t);
        if (std::find(tps.begin(), tps.end(), sn) != tps.end())
            return Type::make_type_param(sn);
        return t;
    }
    case Type::Kind::Array: {
        auto& at = static_cast<ArrayType&>(*t);
        return Type::make_array(convert_type_params(at.elem, tps), at.size);
    }
    case Type::Kind::DynArray: {
        auto& dt = static_cast<DynArrayType&>(*t);
        return Type::make_dynarray(convert_type_params(dt.elem, tps));
    }
    case Type::Kind::Nullable: {
        auto& nt = static_cast<NullableType&>(*t);
        return Type::make_nullable(convert_type_params(nt.elem, tps));
    }
    case Type::Kind::Tuple: {
        auto& tt = static_cast<TupleType&>(*t);
        std::vector<TypePtr> elems;
        for (auto& e : tt.elems) elems.push_back(convert_type_params(e, tps));
        return Type::make_tuple(std::move(elems));
    }
    case Type::Kind::GenericInst: {
        auto& gi = static_cast<GenericInstType&>(*t);
        std::vector<TypePtr> args;
        for (auto& a : gi.args) args.push_back(convert_type_params(a, tps));
        return Type::make_generic_inst(gi.base, std::move(args));
    }
    default: return t;
    }
}

// Вывод типов: param — тип параметра шаблона (может содержать TypeParam),
// arg — конкретный тип аргумента. Заполняет bindings: T → int32.
static void infer_types(TypePtr param, TypePtr arg, TypeBindings& bindings) {
    if (!param || !arg) return;
    if (param->is_type_param()) {
        auto& name = static_cast<TypeParamType&>(*param).name;
        if (!bindings.count(name))
            bindings[name] = arg;
        return;
    }
    // Структурный разбор составных типов
    if (param->is_array() && arg->is_array())
        { infer_types(type_elem(*param), type_elem(*arg), bindings); return; }
    if (param->is_dynarray() && (arg->is_dynarray() || arg->is_array()))
        { infer_types(type_elem(*param), type_elem(*arg), bindings); return; }
    if (param->is_nullable())
        { infer_types(type_elem(*param), arg->is_nullable() ? type_elem(*arg) : arg, bindings); return; }
    if (param->is_tuple() && arg->is_tuple()) {
        auto& pe = tuple_elems(*param); auto& ae = tuple_elems(*arg);
        for (size_t i = 0; i < pe.size() && i < ae.size(); ++i)
            infer_types(pe[i], ae[i], bindings);
        return;
    }
    if (param->is_generic_inst() && arg->is_generic_inst()) {
        auto& pi = static_cast<GenericInstType&>(*param);
        auto& ai = static_cast<GenericInstType&>(*arg);
        if (pi.base == ai.base)
            for (size_t i = 0; i < pi.args.size() && i < ai.args.size(); ++i)
                infer_types(pi.args[i], ai.args[i], bindings);
    }
}

// Клонирование LValue (нужно для глубокого копирования тела функции)
static LValuePtr clone_lvalue(const LValue& lv, const TypeBindings& b);
static ExprPtr   clone_expr  (const Expr& e,    const TypeBindings& b);
static StmtPtr   clone_stmt  (const Stmt& s,    const TypeBindings& b);

template<typename T> static std::unique_ptr<T> make_enode(const Expr& e)
{ auto n = std::make_unique<T>(); n->kind = e.kind; n->loc = e.loc; return n; }

template<typename T> static std::unique_ptr<T> make_snode(const Stmt& s)
{ auto n = std::make_unique<T>(); n->kind = s.kind; n->loc = s.loc; return n; }

static LValuePtr clone_lvalue(const LValue& lv, const TypeBindings& b) {
    switch (lv.kind) {
    case LValue::Kind::Ident: {
        auto& src = static_cast<const IdentLValue&>(lv);
        auto n = std::make_unique<IdentLValue>();
        n->kind = lv.kind; n->loc = lv.loc; n->name = src.name; return n;
    }
    case LValue::Kind::Index: {
        auto& src = static_cast<const IndexLValue&>(lv);
        auto n = std::make_unique<IndexLValue>();
        n->kind = lv.kind; n->loc = lv.loc;
        n->base = clone_lvalue(*src.base, b); n->idx = clone_expr(*src.idx, b); return n;
    }
    case LValue::Kind::Field: {
        auto& src = static_cast<const FieldLValue&>(lv);
        auto n = std::make_unique<FieldLValue>();
        n->kind = lv.kind; n->loc = lv.loc;
        n->base = clone_lvalue(*src.base, b); n->field = src.field; return n;
    }
    }
    throw std::logic_error("clone_lvalue: unknown kind");
}

// clone_expr helpers — по одной функции на каждый вид выражения

static ExprPtr clone_int_lit(const Expr& e, const TypeBindings&) {
    auto& s = static_cast<const IntLitExpr&>(e);
    auto n = make_enode<IntLitExpr>(e);
    n->value = s.value; n->suffix = s.suffix;
    return n;
}
static ExprPtr clone_float_lit(const Expr& e, const TypeBindings&) {
    auto& s = static_cast<const FloatLitExpr&>(e);
    auto n = make_enode<FloatLitExpr>(e);
    n->value = s.value; n->suffix = s.suffix;
    return n;
}
static ExprPtr clone_bool_lit(const Expr& e, const TypeBindings&) {
    auto n = make_enode<BoolLitExpr>(e);
    n->value = static_cast<const BoolLitExpr&>(e).value;
    return n;
}
static ExprPtr clone_string_lit(const Expr& e, const TypeBindings&) {
    auto n = make_enode<StringLitExpr>(e);
    n->value = static_cast<const StringLitExpr&>(e).value;
    return n;
}
static ExprPtr clone_char_lit(const Expr& e, const TypeBindings&) {
    auto n = make_enode<CharLitExpr>(e);
    n->value = static_cast<const CharLitExpr&>(e).value;
    return n;
}
static ExprPtr clone_ident_expr(const Expr& e, const TypeBindings&) {
    auto n = make_enode<IdentExpr>(e);
    n->name = static_cast<const IdentExpr&>(e).name;
    return n;
}
static ExprPtr clone_ns_access(const Expr& e, const TypeBindings&) {
    auto& s = static_cast<const NamespaceAccessExpr&>(e);
    auto n = make_enode<NamespaceAccessExpr>(e);
    n->ns_name = s.ns_name; n->member = s.member;
    return n;
}
static ExprPtr clone_binop_expr(const Expr& e, const TypeBindings& b) {
    auto& s = static_cast<const BinOpExpr&>(e);
    auto n = make_enode<BinOpExpr>(e);
    n->op  = s.op;
    n->lhs = clone_expr(*s.lhs, b);
    n->rhs = clone_expr(*s.rhs, b);
    return n;
}
static ExprPtr clone_unary_expr(const Expr& e, const TypeBindings& b) {
    auto& s = static_cast<const UnaryOpExpr&>(e);
    auto n = make_enode<UnaryOpExpr>(e);
    n->op      = s.op;
    n->operand = clone_expr(*s.operand, b);
    return n;
}
static ExprPtr clone_cast_expr(const Expr& e, const TypeBindings& b) {
    auto& s = static_cast<const CastExpr&>(e);
    auto n = make_enode<CastExpr>(e);
    n->operand = clone_expr(*s.operand, b);
    n->target  = subst_type(s.target, b);
    return n;
}
static ExprPtr clone_call_expr(const Expr& e, const TypeBindings& b) {
    auto& s = static_cast<const CallExpr&>(e);
    auto n = make_enode<CallExpr>(e);
    n->callee = clone_expr(*s.callee, b);
    for (auto& a  : s.args)
        n->args.push_back(clone_expr(*a, b));
    for (auto& na : s.named_args) {
        NamedArg nn; nn.name = na.name; nn.value = clone_expr(*na.value, b);
        n->named_args.push_back(std::move(nn));
    }
    return n;
}
static ExprPtr clone_index_expr(const Expr& e, const TypeBindings& b) {
    auto& s = static_cast<const IndexExpr&>(e);
    auto n = make_enode<IndexExpr>(e);
    n->arr = clone_expr(*s.arr, b);
    n->idx = clone_expr(*s.idx, b);
    return n;
}
static ExprPtr clone_field_expr(const Expr& e, const TypeBindings& b) {
    auto& s = static_cast<const FieldExpr&>(e);
    auto n = make_enode<FieldExpr>(e);
    n->field_name = s.field_name;
    n->object     = clone_expr(*s.object, b);
    return n;
}
static ExprPtr clone_tuple_index_expr(const Expr& e, const TypeBindings& b) {
    auto& s = static_cast<const TupleIndexExpr&>(e);
    auto n = make_enode<TupleIndexExpr>(e);
    n->index  = s.index;
    n->object = clone_expr(*s.object, b);
    return n;
}
static ExprPtr clone_array_lit_expr(const Expr& e, const TypeBindings& b) {
    auto& s = static_cast<const ArrayLitExpr&>(e);
    auto n = make_enode<ArrayLitExpr>(e);
    for (auto& el : s.elements) n->elements.push_back(clone_expr(*el, b));
    return n;
}
static ExprPtr clone_struct_lit_expr(const Expr& e, const TypeBindings& b) {
    auto& s = static_cast<const StructLitExpr&>(e);
    auto n = make_enode<StructLitExpr>(e);
    n->type_name = s.type_name;
    for (auto& ta : s.type_args) n->type_args.push_back(subst_type(ta, b));
    for (auto& f  : s.fields) {
        StructLitField nf; nf.name = f.name; nf.value = clone_expr(*f.value, b);
        n->fields.push_back(std::move(nf));
    }
    return n;
}
static ExprPtr clone_tuple_lit_expr(const Expr& e, const TypeBindings& b) {
    auto& s = static_cast<const TupleLitExpr&>(e);
    auto n = make_enode<TupleLitExpr>(e);
    for (auto& el : s.elements) n->elements.push_back(clone_expr(*el, b));
    return n;
}

static ExprPtr clone_expr(const Expr& e, const TypeBindings& b) {
    switch (e.kind) {
    case Expr::Kind::IntLit:          return clone_int_lit(e, b);
    case Expr::Kind::FloatLit:        return clone_float_lit(e, b);
    case Expr::Kind::BoolLit:         return clone_bool_lit(e, b);
    case Expr::Kind::StringLit:       return clone_string_lit(e, b);
    case Expr::Kind::CharLit:         return clone_char_lit(e, b);
    case Expr::Kind::NullLit:         return make_enode<NullLitExpr>(e);
    case Expr::Kind::Ident:           return clone_ident_expr(e, b);
    case Expr::Kind::NamespaceAccess: return clone_ns_access(e, b);
    case Expr::Kind::BinOp:           return clone_binop_expr(e, b);
    case Expr::Kind::UnaryOp:         return clone_unary_expr(e, b);
    case Expr::Kind::Cast:            return clone_cast_expr(e, b);
    case Expr::Kind::Call:            return clone_call_expr(e, b);
    case Expr::Kind::Index:           return clone_index_expr(e, b);
    case Expr::Kind::Field:           return clone_field_expr(e, b);
    case Expr::Kind::TupleIndex:      return clone_tuple_index_expr(e, b);
    case Expr::Kind::ArrayLit:        return clone_array_lit_expr(e, b);
    case Expr::Kind::StructLit:       return clone_struct_lit_expr(e, b);
    case Expr::Kind::TupleLit:        return clone_tuple_lit_expr(e, b);
    }
    throw std::logic_error("clone_expr: unknown kind");
}

// clone_stmt helpers — по одной функции на каждый вид оператора

static StmtPtr clone_block_stmt(const Stmt& s, const TypeBindings& b) {
    auto& src = static_cast<const BlockStmt&>(s);
    auto n = make_snode<BlockStmt>(s);
    for (auto& st : src.stmts) n->stmts.push_back(clone_stmt(*st, b));
    return n;
}
static StmtPtr clone_vardecl_stmt(const Stmt& s, const TypeBindings& b) {
    auto& src = static_cast<const VarDeclStmt&>(s);
    auto n = make_snode<VarDeclStmt>(s);
    n->name     = src.name;
    n->is_val   = src.is_val;
    n->ann_type = subst_type(src.ann_type, b);
    n->init     = clone_expr(*src.init, b);
    return n;
}
static StmtPtr clone_assign_stmt(const Stmt& s, const TypeBindings& b) {
    auto& src = static_cast<const AssignStmt&>(s);
    auto n = make_snode<AssignStmt>(s);
    n->target = clone_lvalue(*src.target, b);
    n->value  = clone_expr(*src.value, b);
    return n;
}
static StmtPtr clone_if_stmt(const Stmt& s, const TypeBindings& b) {
    auto& src = static_cast<const IfStmt&>(s);
    auto n = make_snode<IfStmt>(s);
    n->cond        = clone_expr(*src.cond, b);
    n->then_branch = clone_stmt(*src.then_branch, b);
    if (src.else_branch) n->else_branch = clone_stmt(*src.else_branch, b);
    return n;
}
static StmtPtr clone_while_stmt(const Stmt& s, const TypeBindings& b) {
    auto& src = static_cast<const WhileStmt&>(s);
    auto n = make_snode<WhileStmt>(s);
    n->cond = clone_expr(*src.cond, b);
    n->body = clone_stmt(*src.body, b);
    return n;
}
static StmtPtr clone_for_range_stmt(const Stmt& s, const TypeBindings& b) {
    auto& src = static_cast<const ForRangeStmt&>(s);
    auto n = make_snode<ForRangeStmt>(s);
    n->var_name = src.var_name;
    n->var_type = subst_type(src.var_type, b);
    n->start    = clone_expr(*src.start, b);
    n->end      = clone_expr(*src.end, b);
    n->body     = clone_stmt(*src.body, b);
    return n;
}
static StmtPtr clone_for_c_stmt(const Stmt& s, const TypeBindings& b) {
    auto& src = static_cast<const ForCStmt&>(s);
    auto n = make_snode<ForCStmt>(s);
    n->init = clone_stmt(*src.init, b);
    n->cond = clone_expr(*src.cond, b);
    n->step = clone_stmt(*src.step, b);
    n->body = clone_stmt(*src.body, b);
    return n;
}
static StmtPtr clone_return_stmt(const Stmt& s, const TypeBindings& b) {
    auto& src = static_cast<const ReturnStmt&>(s);
    auto n = make_snode<ReturnStmt>(s);
    if (src.value) n->value = clone_expr(*src.value, b);
    return n;
}

static StmtPtr clone_stmt(const Stmt& s, const TypeBindings& b) {
    switch (s.kind) {
    case Stmt::Kind::Block:    return clone_block_stmt(s, b);
    case Stmt::Kind::VarDecl:  return clone_vardecl_stmt(s, b);
    case Stmt::Kind::Assign:   return clone_assign_stmt(s, b);
    case Stmt::Kind::ExprStmt: { auto n = make_snode<ExprStmt>(s); n->expr = clone_expr(*static_cast<const ExprStmt&>(s).expr, b); return n; }
    case Stmt::Kind::If:       return clone_if_stmt(s, b);
    case Stmt::Kind::While:    return clone_while_stmt(s, b);
    case Stmt::Kind::ForRange: return clone_for_range_stmt(s, b);
    case Stmt::Kind::ForC:     return clone_for_c_stmt(s, b);
    case Stmt::Kind::Return:   return clone_return_stmt(s, b);
    case Stmt::Kind::Break:    return make_snode<BreakStmt>(s);
    case Stmt::Kind::Continue: return make_snode<ContinueStmt>(s);
    case Stmt::Kind::Empty:    return make_snode<EmptyStmt>(s);
    }
    throw std::logic_error("clone_stmt: unknown kind");
}

// Цепочка областей видимости (лексическая вложенность).
// При входе в блок { } — push, при выходе — pop.
// Поиск имени идёт от внутренней области к внешней.
struct Scope {
    std::unordered_map<std::string, Symbol>              syms;
    // перегрузки: имя → список символов
    std::unordered_map<std::string, std::vector<Symbol>> overloads;
    Scope* parent = nullptr;

    Symbol* lookup(const std::string& name) {
        auto it = syms.find(name);
        if (it != syms.end()) return &it->second;
        if (parent) return parent->lookup(name);
        return nullptr;
    }
    std::vector<Symbol>* lookup_overloads(const std::string& name) {
        auto it = overloads.find(name);
        if (it != overloads.end()) return &it->second;
        if (parent) return parent->lookup_overloads(name);
        return nullptr;
    }

    bool has_local(const std::string& name) const {
        return syms.count(name) > 0 || overloads.count(name) > 0;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// class Analyser — семантический анализатор
// ─────────────────────────────────────────────────────────────────────────────

class Analyser {
public:
    const std::string& filename;
    std::vector<SemanticError> errors;
    std::unordered_map<std::string, TypePtr> type_aliases;
    std::unordered_map<std::string, Symbol>  struct_table;
    std::unordered_map<std::string, std::unordered_map<std::string, Symbol>> ns_table;
    Scope global_scope;
    int   next_global = 0;
    TypePtr  current_ret_type;
    bool     in_loop  = false;
    int      next_slot = 0;
    std::vector<Scope*>        scope_stack;
    std::vector<GlobalVarDecl*> globals_out;
    std::vector<FunDecl*>       functions_out;
    std::unordered_map<std::string, FunDecl*>    generic_funs;
    std::unordered_map<std::string, StructDecl*> generic_structs;
    std::unordered_map<std::string, FunDecl*>    instantiated_funs;
    std::vector<std::unique_ptr<FunDecl>>        owned_instances;

    explicit Analyser(const std::string& fn) : filename(fn) {}

    void     err(SrcLoc l, const std::string& msg);
    static std::string mangle(const std::string& base, const std::vector<TypePtr>& params);
    FunDecl* instantiate_generic_fun(const std::string& base_name, FunDecl* tmpl, const TypeBindings& bindings);
    static std::string mangle_generic(const std::string& base, const std::vector<std::string>& type_param_names, const TypeBindings& bindings);
    static std::string mangle_struct(const std::string& base, const std::vector<TypePtr>& args);
    TypePtr  resolve(TypePtr t);
    Scope&   cur_scope();
    void     push_scope();
    void     pop_scope();
    int      alloc_slot();
    bool     declare_local(const std::string& name, TypePtr type, bool is_val, SrcLoc loc);
    bool     can_widen(TypePtr from, TypePtr to);
    void     maybe_widen(ExprPtr& expr, TypePtr target);
    TypePtr  type_for_int_suffix(const std::string& suf);
    TypePtr  type_for_float_suffix(const std::string& suf);
    bool     cast_ok(TypePtr from, TypePtr to);
    TypePtr  check_expr(Expr& e);
    TypePtr  check_lvalue(LValue& lv, bool allow_mut);
    bool     always_returns(const Stmt& s);
    void     check_stmt(Stmt& s);
    void     collect_top(const std::vector<TopDeclPtr>& decls, const std::string& = "");
    void     analyse_fun(FunDecl& fd);
    void     analyse_global(GlobalVarDecl& gv);
    AnalysisResult run(Program& prog);

private:
    TypePtr check_ident_expr(Expr& e);
    TypePtr check_tuple_lit(Expr& e);
    TypePtr check_tuple_index(Expr& e);
    TypePtr check_ns_access(Expr& e);
    TypePtr check_binop_expr(Expr& e);
    TypePtr check_unary_expr(Expr& e);
    TypePtr check_cast_expr(Expr& e);
    TypePtr check_method_call(Expr& e, CallExpr& c);
    TypePtr check_generic_call(Expr& e, CallExpr& c, const std::string& fname);
    void    check_builtin_args(Expr& e, CallExpr& c, const std::string& fname);
    TypePtr check_call_expr(Expr& e);
    TypePtr check_index_expr(Expr& e);
    TypePtr check_field_expr(Expr& e);
    TypePtr check_array_lit(Expr& e);
    TypePtr check_struct_lit(Expr& e);
    void check_vardecl_stmt(Stmt& s);
    void check_assign_stmt(Stmt& s);
    void check_if_stmt(Stmt& s);
    void check_while_stmt(Stmt& s);
    void check_for_range_stmt(Stmt& s);
    void check_for_c_stmt(Stmt& s);
    void check_return_stmt(Stmt& s);
};

// ─────────────────────────────────────────────────────────────────────────────
// Реализации методов Analyser
// ─────────────────────────────────────────────────────────────────────────────

void Analyser::err(SrcLoc l, const std::string& msg) {
    errors.push_back({l.file.empty() ? filename : l.file, l.line, l.col, msg});
}

std::string Analyser::mangle(const std::string& base, const std::vector<TypePtr>& params) {
    std::string s = base + "@";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) s += ",";
        s += params[i] ? params[i]->to_string() : "?";
    }
    return s;
}

FunDecl* Analyser::instantiate_generic_fun(const std::string& base_name,
                                            FunDecl* tmpl,
                                            const TypeBindings& bindings) {
    std::string mangled = mangle_generic(base_name, tmpl->type_params, bindings);
    auto it = instantiated_funs.find(mangled);
    if (it != instantiated_funs.end()) return it->second;

    auto new_fd = std::make_unique<FunDecl>();
    new_fd->kind = TopDecl::Kind::Fun;
    new_fd->loc  = tmpl->loc;
    new_fd->name = mangled;
    for (auto& p : tmpl->params) {
        Param np; np.name = p.name; np.type = subst_type(p.type, bindings); np.loc = p.loc;
        new_fd->params.push_back(std::move(np));
    }
    new_fd->ret_type = subst_type(tmpl->ret_type, bindings);
    auto cloned_body = clone_stmt(*tmpl->body, bindings);
    new_fd->body = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(cloned_body.release()));

    Symbol sym;
    sym.kind = Symbol::Kind::Function;
    sym.type = new_fd->ret_type;
    for (auto& p : new_fd->params) sym.param_types.push_back(p.type);
    sym.slot = static_cast<int>(functions_out.size());
    global_scope.syms[mangled] = sym;

    FunDecl* ptr = new_fd.get();
    instantiated_funs[mangled] = ptr;
    owned_instances.push_back(std::move(new_fd));
    functions_out.push_back(ptr);

    auto saved_ret     = current_ret_type;
    auto saved_in_loop = in_loop;
    auto saved_slot    = next_slot;
    auto saved_scopes  = std::move(scope_stack);
    scope_stack.clear();

    analyse_fun(*ptr);

    scope_stack      = std::move(saved_scopes);
    current_ret_type = saved_ret;
    in_loop          = saved_in_loop;
    next_slot        = saved_slot;

    return ptr;
}

std::string Analyser::mangle_generic(const std::string& base,
                                      const std::vector<std::string>& type_param_names,
                                      const TypeBindings& bindings) {
    std::string s = base + "<";
    for (size_t i = 0; i < type_param_names.size(); ++i) {
        if (i) s += ", ";
        auto it = bindings.find(type_param_names[i]);
        s += (it != bindings.end()) ? it->second->to_string() : "?";
    }
    return s + ">";
}

std::string Analyser::mangle_struct(const std::string& base, const std::vector<TypePtr>& args) {
    std::string s = base + "<";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) s += ", ";
        s += args[i]->to_string();
    }
    return s + ">";
}

TypePtr Analyser::resolve(TypePtr t) {
    if (!t) return nullptr;
    if (t->kind == Type::Kind::TypeParam) return t;
    if (t->kind == Type::Kind::GenericInst) {
        auto& gi = static_cast<GenericInstType&>(*t);
        std::vector<TypePtr> resolved_args;
        for (auto& a : gi.args) resolved_args.push_back(resolve(a));
        std::string mangled = mangle_struct(gi.base, resolved_args);
        auto resolved_gi = Type::make_generic_inst(gi.base, resolved_args);
        if (struct_table.count(mangled)) return resolved_gi;
        auto sit = generic_structs.find(gi.base);
        if (sit == generic_structs.end()) return resolved_gi;
        auto* tmpl = sit->second;
        TypeBindings bindings;
        for (size_t i = 0; i < tmpl->type_params.size() && i < resolved_args.size(); ++i)
            bindings[tmpl->type_params[i]] = resolved_args[i];
        Symbol sym;
        sym.kind = Symbol::Kind::StructType;
        sym.type = resolved_gi;
        for (auto& f : tmpl->fields) {
            FieldDecl fd; fd.name = f.name; fd.type = subst_type(f.type, bindings); fd.loc = f.loc;
            sym.fields.push_back(std::move(fd));
        }
        struct_table[mangled] = sym;
        return resolved_gi;
    }
    if (t->kind == Type::Kind::Struct) {
        auto& sn = struct_name(*t);
        auto it = type_aliases.find(sn);
        if (it != type_aliases.end()) return resolve(it->second);
        if (!struct_table.count(sn)) return t;
        return t;
    }
    if (t->kind == Type::Kind::Array) {
        auto& at = static_cast<ArrayType&>(*t);
        auto elem = resolve(at.elem);
        if (*elem != *at.elem)
            return Type::make_array(elem, at.size);
    }
    return t;
}

Scope& Analyser::cur_scope() { return *scope_stack.back(); }

void Analyser::push_scope() {
    auto* s = new Scope();
    s->parent = scope_stack.empty() ? &global_scope : scope_stack.back();
    scope_stack.push_back(s);
}

void Analyser::pop_scope() {
    delete scope_stack.back();
    scope_stack.pop_back();
}

int Analyser::alloc_slot() { return next_slot++; }

bool Analyser::declare_local(const std::string& name, TypePtr type, bool is_val, SrcLoc loc) {
    if (cur_scope().has_local(name)) {
        err(loc, "redeclaration of '" + name + "' in the same scope");
        return false;
    }
    int slot = alloc_slot();
    cur_scope().syms[name] = Symbol{Symbol::Kind::LocalVar, type, is_val, false, slot};
    return true;
}

bool Analyser::can_widen(TypePtr from, TypePtr to) {
    if (!from || !to || *from == *to) return false;
    if (!from->is_numeric() || !to->is_numeric()) return false;
    if (to->kind == Type::Kind::Float64 && from->is_int())   return true;
    if (to->kind == Type::Kind::Float64 && from->kind == Type::Kind::Float32) return true;
    if (to->kind == Type::Kind::Float32 && from->is_int())   return true;
    if (from->is_signed_int() && to->is_signed_int())
        return to->int_bits() > from->int_bits();
    if (from->is_unsigned_int() && to->is_unsigned_int())
        return to->int_bits() > from->int_bits();
    return false;
}

void Analyser::maybe_widen(ExprPtr& expr, TypePtr target) {
    if (!expr || !expr->type || !target) return;
    if (*expr->type == *target) return;
    if (can_widen(expr->type, target)) {
        auto cast = std::make_unique<CastExpr>();
        cast->kind    = Expr::Kind::Cast;
        cast->loc     = expr->loc;
        cast->operand = std::move(expr);
        cast->target  = target;
        cast->type    = target;
        expr = std::move(cast);
    }
}

TypePtr Analyser::type_for_int_suffix(const std::string& suf) {
    if (suf == "i8")  return TYPE_INT8;
    if (suf == "i16") return TYPE_INT16;
    if (suf == "i32") return TYPE_INT32;
    if (suf == "i64") return TYPE_INT64;
    if (suf == "u8")  return TYPE_UINT8;
    if (suf == "u16") return TYPE_UINT16;
    if (suf == "u32") return TYPE_UINT32;
    if (suf == "u64") return TYPE_UINT64;
    return TYPE_INT32;
}

TypePtr Analyser::type_for_float_suffix(const std::string& suf) {
    if (suf == "f32") return TYPE_FLOAT32;
    return TYPE_FLOAT64;
}

bool Analyser::cast_ok(TypePtr from, TypePtr to) {
    if (!from || !to) return false;
    if (*from == *to) return true;
    if (from->is_int()   && to->is_int())   return true;
    if (from->is_float() && to->is_float()) return true;
    if (from->is_int()   && to->is_float()) return true;
    if (from->is_float() && to->is_int())   return true;
    if (from->is_char()  && to->is_int())   return true;
    if (from->is_int()   && to->is_char())  return true;
    return false;
}

// ── check_expr helpers ────────────────────────────────────────────────────────

TypePtr Analyser::check_ident_expr(Expr& e) {
    auto& id  = static_cast<IdentExpr&>(e);
    Symbol* s = cur_scope().lookup(id.name);
    if (!s) s = global_scope.lookup(id.name);
    if (!s) { err(e.loc, "undefined identifier '" + id.name + "'"); return e.type = TYPE_INT32; }
    return e.type = s->type;
}

TypePtr Analyser::check_tuple_lit(Expr& e) {
    auto& tl = static_cast<TupleLitExpr&>(e);
    std::vector<TypePtr> elem_types;
    for (auto& el : tl.elements) elem_types.push_back(check_expr(*el));
    return e.type = Type::make_tuple(std::move(elem_types));
}

TypePtr Analyser::check_tuple_index(Expr& e) {
    auto& ti = static_cast<TupleIndexExpr&>(e);
    auto ot  = check_expr(*ti.object);
    if (!ot || !ot->is_tuple()) { err(e.loc, "tuple index on non-tuple type"); return e.type = TYPE_INT32; }
    auto& te = tuple_elems(*ot);
    if (ti.index < 0 || ti.index >= static_cast<int>(te.size())) {
        err(e.loc, "tuple index " + std::to_string(ti.index) +
            " out of range (size " + std::to_string(te.size()) + ")");
        return e.type = TYPE_INT32;
    }
    return e.type = te[ti.index];
}

TypePtr Analyser::check_ns_access(Expr& e) {
    auto& na  = static_cast<NamespaceAccessExpr&>(e);
    auto  nit = ns_table.find(na.ns_name);
    if (nit == ns_table.end()) { err(e.loc, "unknown namespace '" + na.ns_name + "'"); return e.type = TYPE_INT32; }
    auto  mit = nit->second.find(na.member);
    if (mit == nit->second.end()) {
        err(e.loc, "'" + na.member + "' not found in namespace '" + na.ns_name + "'");
        return e.type = TYPE_INT32;
    }
    return e.type = mit->second.type;
}

TypePtr Analyser::check_binop_expr(Expr& e) {
    auto& b  = static_cast<BinOpExpr&>(e);
    auto  lt = check_expr(*b.lhs);
    auto  rt = check_expr(*b.rhs);

    if (b.op == BinOpKind::And || b.op == BinOpKind::Or) {
        if (!lt || !lt->is_bool()) err(b.lhs->loc, "logical operator requires bool, got " + (lt?lt->to_string():"?"));
        if (!rt || !rt->is_bool()) err(b.rhs->loc, "logical operator requires bool, got " + (rt?rt->to_string():"?"));
        return e.type = TYPE_BOOL;
    }
    if (b.op==BinOpKind::Eq||b.op==BinOpKind::NEq||
        b.op==BinOpKind::Lt||b.op==BinOpKind::Le||
        b.op==BinOpKind::Gt||b.op==BinOpKind::Ge) {
        bool ok = (lt && rt) && (*lt==*rt || (lt->is_nullable()&&rt->is_nullable()) ||
                                 (lt->is_char()&&rt->is_char()) || can_widen(lt,rt) || can_widen(rt,lt));
        if (lt && rt && !ok) {
            err(e.loc, "type mismatch in comparison: " + lt->to_string() + " vs " + rt->to_string());
        } else if (lt && lt->is_numeric() && rt && rt->is_numeric() && *lt != *rt) {
            if (can_widen(lt, rt)) maybe_widen(b.lhs, rt);
            else if (can_widen(rt, lt)) maybe_widen(b.rhs, lt);
        }
        return e.type = TYPE_BOOL;
    }
    if (b.op == BinOpKind::Add && lt && lt->is_string()) {
        if (!rt || !rt->is_string()) err(e.loc, "string concatenation requires string on both sides");
        b.op = BinOpKind::StrConcat;
        return e.type = TYPE_STRING;
    }
    if (lt && rt) {
        if (*lt != *rt) {
            if      (can_widen(lt, rt)) { maybe_widen(b.lhs, rt); lt = rt; }
            else if (can_widen(rt, lt)) { maybe_widen(b.rhs, lt); rt = lt; }
            else    err(e.loc, "type mismatch: " + lt->to_string() + " vs " + rt->to_string());
        }
        if (!lt->is_numeric()) err(e.loc, "arithmetic not defined for type " + lt->to_string());
    }
    return e.type = lt ? lt : TYPE_INT32;
}

TypePtr Analyser::check_unary_expr(Expr& e) {
    auto& u  = static_cast<UnaryOpExpr&>(e);
    auto  ot = check_expr(*u.operand);
    if (u.op == UnaryOpKind::Not) {
        if (!ot || !ot->is_bool()) err(e.loc, "'not' requires bool operand");
        return e.type = TYPE_BOOL;
    }
    if (!ot || !ot->is_numeric()) err(e.loc, "unary '-' requires numeric operand");
    return e.type = ot ? ot : TYPE_INT32;
}

TypePtr Analyser::check_cast_expr(Expr& e) {
    auto& c  = static_cast<CastExpr&>(e);
    auto  ot = check_expr(*c.operand);
    c.target = resolve(c.target);
    if (!cast_ok(ot, c.target))
        err(e.loc, "invalid cast from " + (ot?ot->to_string():"?") + " to " + c.target->to_string());
    return e.type = c.target;
}

TypePtr Analyser::check_method_call(Expr& e, CallExpr& c) {
    auto& fe    = static_cast<FieldExpr&>(*c.callee);
    auto  obj_rt = resolve(check_expr(*fe.object));

    if (obj_rt && obj_rt->is_string()) {
        if (fe.field_name == "len") {
            if (!c.args.empty()) err(e.loc, "str.len() takes no arguments");
            return e.type = TYPE_UINT64;
        }
        err(e.loc, "unknown method '" + fe.field_name + "' on string");
        return e.type = TYPE_INT32;
    }
    if (obj_rt && obj_rt->is_dynarray()) {
        for (auto& a : c.args) check_expr(*a);
        auto& m = fe.field_name;
        if (m == "push") return e.type = TYPE_VOID;
        if (m == "pop")  return e.type = type_elem(*obj_rt);
        if (m == "len")  return e.type = TYPE_UINT64;
        if (m == "get")  return e.type = type_elem(*obj_rt);
        err(e.loc, "unknown method '" + m + "' on dynamic array");
        return e.type = TYPE_INT32;
    }
    Symbol* fsym = nullptr;
    if (obj_rt && obj_rt->is_struct())
        fsym = global_scope.lookup(sname(*obj_rt) + "::" + fe.field_name);
    if (!fsym) {
        err(e.loc, "unknown method '" + fe.field_name + "' on type " + (obj_rt?obj_rt->to_string():"?"));
        for (auto& a : c.args) check_expr(*a);
        return e.type = TYPE_INT32;
    }
    if (c.args.size() + 1 != fsym->param_types.size())
        err(e.loc, "method '" + fe.field_name + "' expects " +
            std::to_string(fsym->param_types.size()-1) + " arguments, got " + std::to_string(c.args.size()));
    for (size_t i = 0; i < c.args.size(); ++i) {
        auto at = check_expr(*c.args[i]);
        size_t pi = i + 1;
        if (pi < fsym->param_types.size() && at && fsym->param_types[pi]) {
            auto rp = resolve(fsym->param_types[pi]);
            auto ra = resolve(at);
            bool arr_compat = rp->is_dynarray() && (ra->is_array()||ra->is_dynarray()) &&
                              *type_elem(*rp) == *type_elem(*ra);
            if (*ra != *rp && !arr_compat) maybe_widen(c.args[i], rp);
        }
    }
    return e.type = fsym->type;
}

TypePtr Analyser::check_generic_call(Expr& e, CallExpr& c, const std::string& fname) {
    auto git = generic_funs.find(fname);
    if (git == generic_funs.end()) {
        err(e.loc, "'" + fname + "' is not a function");
        for (auto& a : c.args) check_expr(*a);
        return e.type = TYPE_INT32;
    }
    auto* tmpl = git->second;
    std::vector<TypePtr> arg_types;
    for (auto& a : c.args) arg_types.push_back(check_expr(*a));
    TypeBindings bindings;
    bool ok = (c.args.size() == tmpl->params.size());
    if (!ok) {
        err(e.loc, "generic function '" + fname + "' expects " +
            std::to_string(tmpl->params.size()) + " argument(s), got " + std::to_string(c.args.size()));
    } else {
        for (size_t i = 0; i < c.args.size(); ++i)
            infer_types(tmpl->params[i].type, arg_types[i], bindings);
        for (auto& tp : tmpl->type_params)
            if (!bindings.count(tp)) {
                err(e.loc, "cannot infer type parameter '" + tp + "' for '" + fname + "'");
                ok = false;
            }
        if (ok) {
            for (size_t i = 0; i < c.args.size(); ++i) {
                auto expected = subst_type(tmpl->params[i].type, bindings);
                if (expected && arg_types[i] && *expected != *arg_types[i]) {
                    err(c.args[i]->loc, "argument " + std::to_string(i+1) + " of '" + fname +
                        "': expected " + expected->to_string() + ", got " + arg_types[i]->to_string());
                    ok = false;
                }
            }
        }
    }
    if (ok) {
        auto* inst = instantiate_generic_fun(fname, tmpl, bindings);
        if (inst) {
            if (c.callee->kind == Expr::Kind::Ident)
                static_cast<IdentExpr&>(*c.callee).name = inst->name;
            return e.type = inst->ret_type;
        }
    }
    return e.type = TYPE_INT32;
}

void Analyser::check_builtin_args(Expr& e, CallExpr& c, const std::string& fname) {
    if (fname == "print") {
        if (c.args.size() != 1) err(e.loc, "print() expects 1 positional argument");
        for (auto& na : c.named_args) {
            if (na.name == "end") {
                auto et = check_expr(*na.value);
                if (!et || !et->is_string()) err(na.value->loc, "print() end= must be string");
            } else {
                err(e.loc, "print() unknown named argument '" + na.name + "'");
            }
        }
    }
    if (fname == "input" || fname == "input_int" || fname == "input_float") {
        if (c.args.size() > 1) err(e.loc, fname + "() expects 0 or 1 argument (prompt string)");
        if (c.args.size() == 1) {
            auto pt = check_expr(*c.args[0]);
            if (!pt || !pt->is_string()) err(c.args[0]->loc, fname + "() prompt must be string");
        }
    }
    if ((fname == "to_int" || fname == "to_float") && c.args.size() != 1)
        err(e.loc, fname + "() expects 1 argument");
    if (fname == "exit"  && c.args.size() != 1) err(e.loc, "exit() expects 1 argument");
    if (fname == "panic" && c.args.size() != 1) err(e.loc, "panic() expects 1 argument");
    if (fname == "assert") {
        if (c.args.size() < 1 || c.args.size() > 2) err(e.loc, "assert() expects 1 or 2 arguments");
        if (c.args.size() == 2) {
            auto mt = check_expr(*c.args[1]);
            if (!mt || !mt->is_string()) err(c.args[1]->loc, "assert() message must be string");
        }
    }
}

TypePtr Analyser::check_call_expr(Expr& e) {
    auto& c = static_cast<CallExpr&>(e);

    if (c.callee->kind == Expr::Kind::Field)
        return check_method_call(e, c);

    std::string fname;
    Symbol*     fsym = nullptr;

    if (c.callee->kind == Expr::Kind::Ident) {
        fname     = static_cast<IdentExpr&>(*c.callee).name;
        auto* ovl = global_scope.lookup_overloads(fname);
        if (ovl && !ovl->empty()) {
            std::vector<TypePtr> arg_types;
            for (auto& a : c.args) arg_types.push_back(check_expr(*a));
            for (auto& sym : *ovl) {
                if (sym.param_types.size() != arg_types.size()) continue;
                bool match = true;
                for (size_t i = 0; i < arg_types.size(); ++i) {
                    auto rp = resolve(sym.param_types[i]), ra = resolve(arg_types[i]);
                    if (ra && rp && *ra != *rp && !can_widen(ra, rp)) { match = false; break; }
                }
                if (match) {
                    static_cast<IdentExpr&>(*c.callee).name = mangle(fname, sym.param_types);
                    return e.type = sym.type;
                }
            }
            err(e.loc, "no matching overload of '" + fname + "' for given arguments");
            return e.type = TYPE_INT32;
        }
        fsym = global_scope.lookup(fname);
        if (!fsym) fsym = cur_scope().lookup(fname);
    } else if (c.callee->kind == Expr::Kind::NamespaceAccess) {
        auto& na  = static_cast<NamespaceAccessExpr&>(*c.callee);
        auto  nit = ns_table.find(na.ns_name);
        if (nit != ns_table.end()) {
            auto mit = nit->second.find(na.member);
            if (mit != nit->second.end()) fsym = &mit->second;
        }
        fname = na.ns_name + "::" + na.member;
    }

    if (!fsym || fsym->kind != Symbol::Kind::Function)
        return check_generic_call(e, c, fname);

    if (!fsym->is_builtin && c.args.size() != fsym->param_types.size())
        err(e.loc, "function '" + fname + "' expects " +
            std::to_string(fsym->param_types.size()) + " arguments, got " + std::to_string(c.args.size()));
    if (fsym->is_builtin)
        check_builtin_args(e, c, fname);

    for (size_t i = 0; i < c.args.size(); ++i) {
        auto at = check_expr(*c.args[i]);
        if (fsym->is_builtin) continue;
        if (i < fsym->param_types.size() && at && fsym->param_types[i]) {
            auto rp = resolve(fsym->param_types[i]);
            auto ra = resolve(at);
            bool arr_compat  = rp->is_dynarray() && (ra->is_array()||ra->is_dynarray()) &&
                               *type_elem(*rp) == *type_elem(*ra);
            bool null_compat = rp->is_nullable() &&
                               (ra->is_nullable() || (type_elem(*rp) && *type_elem(*rp) == *ra));
            if (*ra != *rp && !arr_compat && !null_compat)
                err(c.args[i]->loc, "argument " + std::to_string(i+1) +
                    " type mismatch: expected " + rp->to_string() + ", got " + ra->to_string());
        }
    }
    return e.type = fsym->type;
}

TypePtr Analyser::check_index_expr(Expr& e) {
    auto& ix    = static_cast<IndexExpr&>(e);
    auto  arrt  = check_expr(*ix.arr);
    auto  idxt  = check_expr(*ix.idx);
    auto  arr_r = resolve(arrt);
    if (!arr_r || (!arr_r->is_array() && !arr_r->is_dynarray())) err(e.loc, "indexing non-array type");
    if (!idxt || !idxt->is_int()) err(ix.idx->loc, "array index must be integer");
    return e.type = (arr_r && (arr_r->is_array()||arr_r->is_dynarray())) ? type_elem(*arr_r) : TYPE_INT32;
}

TypePtr Analyser::check_field_expr(Expr& e) {
    auto& fe  = static_cast<FieldExpr&>(e);
    auto  ot  = check_expr(*fe.object);
    if (!ot || !ot->is_struct()) { err(e.loc, "field access on non-struct type"); return e.type = TYPE_INT32; }
    auto  sn  = sname(*ot);
    auto  sit = struct_table.find(sn);
    if (sit == struct_table.end()) { err(e.loc, "unknown struct type '" + sn + "'"); return e.type = TYPE_INT32; }
    auto& fields = sit->second.fields;
    for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
        if (fields[i].name == fe.field_name) { fe.field_idx = i; return e.type = fields[i].type; }
    }
    err(e.loc, "struct '" + sn + "' has no field '" + fe.field_name + "'");
    return e.type = TYPE_INT32;
}

TypePtr Analyser::check_array_lit(Expr& e) {
    auto& al = static_cast<ArrayLitExpr&>(e);
    TypePtr elem;
    for (auto& el : al.elements) {
        auto t = check_expr(*el);
        if (!elem) elem = t;
        else if (t && *t != *elem) err(el->loc, "array literal element type mismatch");
    }
    if (!elem) elem = TYPE_INT32;
    return e.type = Type::make_array(elem, (int64_t)al.elements.size());
}

TypePtr Analyser::check_struct_lit(Expr& e) {
    auto& sl = static_cast<StructLitExpr&>(e);
    std::string resolved_name = sl.type_name;
    TypePtr resolved_struct_type;
    if (!sl.type_args.empty()) {
        std::vector<TypePtr> resolved_args;
        for (auto& ta : sl.type_args) resolved_args.push_back(resolve(ta));
        resolved_struct_type = resolve(Type::make_generic_inst(sl.type_name, std::move(resolved_args)));
        resolved_name = sname(*resolved_struct_type);
    }
    auto sit = struct_table.find(resolved_name);
    if (sit == struct_table.end()) {
        err(e.loc, "unknown struct '" + resolved_name + "'");
        for (auto& f : sl.fields) check_expr(*f.value);
        return e.type = Type::make_struct(resolved_name);
    }
    auto& decl_fields = sit->second.fields;
    for (auto& sf : sl.fields) {
        auto vt = check_expr(*sf.value);
        bool found = false;
        for (auto& df : decl_fields) {
            if (df.name == sf.name) {
                found = true;
                if (vt && *vt != *df.type) {
                    bool arr_compat  = df.type->is_dynarray() && (vt->is_array()||vt->is_dynarray()) &&
                                       *type_elem(*df.type) == *type_elem(*vt);
                    bool null_compat = df.type->is_nullable() &&
                                       (vt->is_nullable() || (type_elem(*df.type) && *type_elem(*df.type) == *vt));
                    if (!arr_compat && !null_compat)
                        err(sf.value->loc, "field '" + sf.name + "' expects " +
                            df.type->to_string() + ", got " + vt->to_string());
                }
                break;
            }
        }
        if (!found) err(e.loc, "struct '" + resolved_name + "' has no field '" + sf.name + "'");
    }
    for (auto& df : decl_fields) {
        bool found = false;
        for (auto& sf : sl.fields) if (sf.name == df.name) { found = true; break; }
        if (!found) err(e.loc, "missing field '" + df.name + "' in struct literal");
    }
    return e.type = resolved_struct_type ? resolved_struct_type : Type::make_struct(resolved_name);
}

TypePtr Analyser::check_expr(Expr& e) {
    switch (e.kind) {
    case Expr::Kind::IntLit:          { auto& il = static_cast<IntLitExpr&>(e);   return e.type = type_for_int_suffix(il.suffix); }
    case Expr::Kind::FloatLit:        { auto& fl = static_cast<FloatLitExpr&>(e); return e.type = type_for_float_suffix(fl.suffix); }
    case Expr::Kind::BoolLit:         return e.type = TYPE_BOOL;
    case Expr::Kind::CharLit:         return e.type = TYPE_CHAR;
    case Expr::Kind::StringLit:       return e.type = TYPE_STRING;
    case Expr::Kind::NullLit:         return e.type = Type::make_nullable(TYPE_VOID);
    case Expr::Kind::Ident:           return check_ident_expr(e);
    case Expr::Kind::TupleLit:        return check_tuple_lit(e);
    case Expr::Kind::TupleIndex:      return check_tuple_index(e);
    case Expr::Kind::NamespaceAccess: return check_ns_access(e);
    case Expr::Kind::BinOp:           return check_binop_expr(e);
    case Expr::Kind::UnaryOp:         return check_unary_expr(e);
    case Expr::Kind::Cast:            return check_cast_expr(e);
    case Expr::Kind::Call:            return check_call_expr(e);
    case Expr::Kind::Index:           return check_index_expr(e);
    case Expr::Kind::Field:           return check_field_expr(e);
    case Expr::Kind::ArrayLit:        return check_array_lit(e);
    case Expr::Kind::StructLit:       return check_struct_lit(e);
    default:                          return e.type = TYPE_INT32;
    }
}

TypePtr Analyser::check_lvalue(LValue& lv, bool allow_mut) {
    switch (lv.kind) {
    case LValue::Kind::Ident: {
        auto& il = static_cast<IdentLValue&>(lv);
        Symbol* sym = cur_scope().lookup(il.name);
        if (!sym) sym = global_scope.lookup(il.name);
        if (!sym) { err(lv.loc, "undefined identifier '" + il.name + "'"); return TYPE_INT32; }
        if (allow_mut && sym->is_val)
            err(lv.loc, "cannot assign to immutable binding '" + il.name + "'");
        return sym->type;
    }
    case LValue::Kind::Index: {
        auto& xl = static_cast<IndexLValue&>(lv);
        auto arrt = check_lvalue(*xl.base, false);
        auto idxt = check_expr(*xl.idx);
        if (!arrt || !arrt->is_array()) err(lv.loc, "indexing non-array");
        if (!idxt || !idxt->is_int())   err(xl.idx->loc, "array index must be integer");
        return (arrt && arrt->is_array()) ? type_elem(*arrt) : TYPE_INT32;
    }
    case LValue::Kind::Field: {
        auto& fl = static_cast<FieldLValue&>(lv);
        auto ot = check_lvalue(*fl.base, false);
        if (!ot || !ot->is_struct()) { err(lv.loc, "field access on non-struct"); return TYPE_INT32; }
        auto sn = sname(*ot);
        auto sit = struct_table.find(sn);
        if (sit == struct_table.end()) { err(lv.loc, "unknown struct '" + sn + "'"); return TYPE_INT32; }
        for (int i = 0; i < static_cast<int>(sit->second.fields.size()); ++i) {
            if (sit->second.fields[i].name == fl.field) {
                fl.field_idx = i;
                return sit->second.fields[i].type;
            }
        }
        err(lv.loc, "struct has no field '" + fl.field + "'");
        return TYPE_INT32;
    }
    }
    return TYPE_INT32;
}

bool Analyser::always_returns(const Stmt& s) {
    switch (s.kind) {
    case Stmt::Kind::Return: return true;
    case Stmt::Kind::Block: {
        for (auto& st : static_cast<const BlockStmt&>(s).stmts)
            if (always_returns(*st)) return true;
        return false;
    }
    case Stmt::Kind::If: {
        auto& i = static_cast<const IfStmt&>(s);
        return i.else_branch &&
               always_returns(*i.then_branch) &&
               always_returns(*i.else_branch);
    }
    case Stmt::Kind::While:
        return false;
    default:
        return false;
    }
}

// check_stmt helpers 

void Analyser::check_vardecl_stmt(Stmt& s) {
    auto& v        = static_cast<VarDeclStmt&>(s);
    auto  init_type = check_expr(*v.init);
    TypePtr declared;
    if (v.ann_type) {
        declared = resolve(v.ann_type);
        bool dyn_compat  = declared->is_dynarray() && init_type &&
                           (init_type->is_array()||init_type->is_dynarray()) &&
                           *type_elem(*declared) == *type_elem(*init_type);
        bool null_compat = declared->is_nullable() && init_type &&
                           (init_type->is_nullable() || *type_elem(*declared) == *init_type);
        bool widen_ok    = init_type && can_widen(init_type, declared);
        if (init_type && *init_type != *declared && !dyn_compat && !null_compat && !widen_ok)
            err(v.loc, "variable '" + v.name + "': type annotation " +
                declared->to_string() + " doesn't match initializer " + init_type->to_string());
        if (widen_ok) maybe_widen(v.init, declared);
    } else {
        declared = init_type;
    }
    declare_local(v.name, declared, v.is_val, v.loc);
}

void Analyser::check_assign_stmt(Stmt& s) {
    auto& a     = static_cast<AssignStmt&>(s);
    auto  ltype = check_lvalue(*a.target, true);
    auto  rtype = check_expr(*a.value);
    if (ltype && rtype && *ltype != *rtype) {
        bool null_ok  = ltype->is_nullable() &&
                        (rtype->is_nullable() || (type_elem(*ltype) && *type_elem(*ltype) == *rtype));
        bool widen_ok = can_widen(rtype, ltype);
        if (!null_ok && !widen_ok)
            err(s.loc, "assignment type mismatch: " + ltype->to_string() + " vs " + rtype->to_string());
        if (widen_ok) maybe_widen(a.value, ltype);
    }
}

void Analyser::check_if_stmt(Stmt& s) {
    auto& i  = static_cast<IfStmt&>(s);
    auto  ct = check_expr(*i.cond);
    if (!ct || !ct->is_bool())
        err(i.cond->loc, "if condition must be bool, got " + (ct?ct->to_string():"?"));
    check_stmt(*i.then_branch);
    if (i.else_branch) check_stmt(*i.else_branch);
}

void Analyser::check_while_stmt(Stmt& s) {
    auto& w    = static_cast<WhileStmt&>(s);
    auto  ct   = check_expr(*w.cond);
    if (!ct || !ct->is_bool())
        err(w.cond->loc, "while condition must be bool, got " + (ct?ct->to_string():"?"));
    bool prev = in_loop; in_loop = true;
    check_stmt(*w.body);
    in_loop = prev;
}

void Analyser::check_for_range_stmt(Stmt& s) {
    auto& fr    = static_cast<ForRangeStmt&>(s);
    auto  st    = check_expr(*fr.start);
    check_expr(*fr.end);
    TypePtr var_t = fr.var_type ? resolve(fr.var_type) : (st ? st : TYPE_INT32);
    push_scope();
    int slot = alloc_slot();
    cur_scope().syms[fr.var_name] = Symbol{Symbol::Kind::LocalVar, var_t, false, false, slot};
    bool prev = in_loop; in_loop = true;
    check_stmt(*fr.body);
    in_loop = prev;
    pop_scope();
}

void Analyser::check_for_c_stmt(Stmt& s) {
    auto& fc = static_cast<ForCStmt&>(s);
    push_scope();
    check_stmt(*fc.init);
    auto ct = check_expr(*fc.cond);
    if (!ct || !ct->is_bool()) err(fc.cond->loc, "for condition must be bool");
    bool prev = in_loop; in_loop = true;
    check_stmt(*fc.body);
    check_stmt(*fc.step);
    in_loop = prev;
    pop_scope();
}

void Analyser::check_return_stmt(Stmt& s) {
    auto& r = static_cast<ReturnStmt&>(s);
    if (current_ret_type && current_ret_type->is_void()) {
        if (r.value) err(s.loc, "void function cannot return a value");
        return;
    }
    if (!r.value) { err(s.loc, "non-void function must return a value"); return; }
    auto vt = check_expr(*r.value);
    if (current_ret_type && vt && *vt != *current_ret_type) {
        bool ret_nullable = current_ret_type->is_nullable() &&
            (vt->is_nullable() || (type_elem(*current_ret_type) && *type_elem(*current_ret_type) == *vt));
        bool widen_ok = can_widen(vt, current_ret_type);
        if (!ret_nullable && !widen_ok)
            err(s.loc, "return type mismatch: expected " +
                current_ret_type->to_string() + ", got " + vt->to_string());
    }
}

void Analyser::check_stmt(Stmt& s) {
    switch (s.kind) {
    case Stmt::Kind::Empty:    break;
    case Stmt::Kind::VarDecl:  check_vardecl_stmt(s); break;
    case Stmt::Kind::Assign:   check_assign_stmt(s);  break;
    case Stmt::Kind::ExprStmt: check_expr(*static_cast<ExprStmt&>(s).expr); break;
    case Stmt::Kind::If:       check_if_stmt(s);       break;
    case Stmt::Kind::While:    check_while_stmt(s);    break;
    case Stmt::Kind::Block:
        push_scope();
        for (auto& st : static_cast<BlockStmt&>(s).stmts) check_stmt(*st);
        pop_scope();
        break;
    case Stmt::Kind::ForRange: check_for_range_stmt(s); break;
    case Stmt::Kind::ForC:     check_for_c_stmt(s);     break;
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
        if (!in_loop) err(s.loc, (s.kind==Stmt::Kind::Break?"break":"continue") +
            std::string(" outside of loop"));
        break;
    case Stmt::Kind::Return:   check_return_stmt(s); break;
    }
}

void Analyser::collect_top(const std::vector<TopDeclPtr>& decls, const std::string&) {
    for (auto& td : decls) {
        switch (td->kind) {
        case TopDecl::Kind::Struct: {
            auto& sd = static_cast<StructDecl&>(*td);
            if (!sd.type_params.empty()) {
                for (auto& f : sd.fields)
                    f.type = convert_type_params(f.type, sd.type_params);
                generic_structs[sd.name] = &sd;
                break;
            }
            Symbol sym;
            sym.kind   = Symbol::Kind::StructType;
            sym.type   = Type::make_struct(sd.name);
            sym.fields = sd.fields;
            struct_table[sd.name] = sym;
            break;
        }
        case TopDecl::Kind::TypeAlias: {
            auto& ta = static_cast<TypeAliasDecl&>(*td);
            type_aliases[ta.name] = ta.aliased;
            break;
        }
        case TopDecl::Kind::Namespace: {
            auto& ns = static_cast<NamespaceDecl&>(*td);
            for (auto& m : ns.members) {
                if (m->kind == TopDecl::Kind::GlobalVar) {
                    auto& gv = static_cast<GlobalVarDecl&>(*m);
                    std::string short_name = gv.name;
                    gv.name = ns.name + "::" + short_name;
                    Symbol sym;
                    sym.kind   = Symbol::Kind::GlobalVar;
                    sym.type   = gv.ann_type;
                    sym.is_val = gv.is_val;
                    sym.slot   = next_global++;
                    ns_table[ns.name][short_name] = sym;
                    globals_out.push_back(&gv);
                } else if (m->kind == TopDecl::Kind::Fun) {
                    auto& fd = static_cast<FunDecl&>(*m);
                    Symbol sym;
                    sym.kind = Symbol::Kind::Function;
                    sym.type = fd.ret_type;
                    for (auto& p : fd.params)
                        sym.param_types.push_back(p.type);
                    ns_table[ns.name][fd.name] = sym;
                    functions_out.push_back(&fd);
                }
            }
            break;
        }
        case TopDecl::Kind::GlobalVar: {
            auto& gv = static_cast<GlobalVarDecl&>(*td);
            Symbol sym;
            sym.kind   = Symbol::Kind::GlobalVar;
            sym.type   = gv.ann_type;
            sym.is_val = gv.is_val;
            sym.slot   = next_global++;
            global_scope.syms[gv.name] = sym;
            globals_out.push_back(&gv);
            break;
        }
        case TopDecl::Kind::Fun: {
            auto& fd = static_cast<FunDecl&>(*td);
            if (!fd.type_params.empty()) {
                for (auto& p : fd.params)
                    p.type = convert_type_params(p.type, fd.type_params);
                fd.ret_type = convert_type_params(fd.ret_type, fd.type_params);
                generic_funs[fd.name] = &fd;
                break;
            }
            Symbol sym;
            sym.kind = Symbol::Kind::Function;
            sym.type = fd.ret_type;
            for (auto& p : fd.params)
                sym.param_types.push_back(p.type);
            std::string base_name = fd.name;
            std::string mangled   = mangle(base_name, sym.param_types);
            sym.slot = static_cast<int>(functions_out.size());

            if (global_scope.syms.count(base_name)) {
                auto existing = global_scope.syms[base_name];
                global_scope.syms.erase(base_name);
                global_scope.overloads[base_name].push_back(existing);
                global_scope.overloads[base_name].push_back(sym);
                for (auto* f : functions_out) {
                    if (f->name == base_name) {
                        f->name = mangle(base_name, existing.param_types);
                        break;
                    }
                }
                fd.name = mangled;
            } else if (global_scope.overloads.count(base_name)) {
                global_scope.overloads[base_name].push_back(sym);
                fd.name = mangled;
            } else {
                global_scope.syms[base_name] = sym;
            }
            functions_out.push_back(&fd);
            break;
        }
        case TopDecl::Kind::Impl: {
            auto& id = static_cast<ImplDecl&>(*td);
            for (auto& mp : id.methods) {
                auto& m = *mp;
                std::string qualified = id.type_name + "::" + m.name;
                m.name = qualified;
                Symbol sym;
                sym.kind = Symbol::Kind::Function;
                sym.type = m.ret_type;
                for (auto& p : m.params)
                    sym.param_types.push_back(p.type);
                global_scope.syms[qualified] = sym;
                functions_out.push_back(mp.get());
            }
            break;
        }
        default: break;
        }
    }
}

void Analyser::analyse_fun(FunDecl& fd) {
    current_ret_type = resolve(fd.ret_type);
    in_loop  = false;
    next_slot = 0;

    push_scope();
    for (auto& p : fd.params) {
        auto pt = resolve(p.type);
        int slot = alloc_slot();
        cur_scope().syms[p.name] = Symbol{Symbol::Kind::Param, pt, true, false, slot};
    }
    for (auto& st : fd.body->stmts)
        check_stmt(*st);

    if (!current_ret_type->is_void() && !always_returns(*fd.body))
        err(fd.loc, "function '" + fd.name + "' does not return on all paths");

    fd.body->loc.col = (uint32_t)next_slot;

    pop_scope();
}

void Analyser::analyse_global(GlobalVarDecl& gv) {
    auto it = check_expr(*gv.init);
    if (gv.ann_type) {
        gv.ann_type = resolve(gv.ann_type);
        if (it && *it != *gv.ann_type)
            err(gv.loc, "global '" + gv.name + "': type mismatch");
    } else {
        gv.ann_type = it;
    }
    auto sym_it = global_scope.syms.find(gv.name);
    if (sym_it != global_scope.syms.end())
        sym_it->second.type = gv.ann_type;
}

AnalysisResult Analyser::run(Program& prog) {
    collect_top(prog.decls);

    {
        auto* main_sym = global_scope.lookup("main");
        if (!main_sym || main_sym->kind != Symbol::Kind::Function)
            errors.push_back({filename, 0, 0, "program must have a 'main' function"});
    }

    push_scope();
    for (auto* gv : globals_out)
        analyse_global(*gv);
    pop_scope();

    for (size_t i = 0; i < functions_out.size(); ++i) {
        auto* fd = functions_out[i];
        if (instantiated_funs.count(fd->name)) continue;
        analyse_fun(*fd);
    }

    AnalysisResult res;
    res.errors          = std::move(errors);
    res.globals         = std::move(globals_out);
    res.functions       = std::move(functions_out);
    res.owned_instances = std::move(owned_instances);
    return res;
}


static void register_builtins(Scope& gs) {
    auto reg_builtin = [&](const std::string& name,
                           std::vector<TypePtr> params,
                           TypePtr ret) {
        Symbol s;
        s.kind        = Symbol::Kind::Function;
        s.type        = ret;
        s.param_types = std::move(params);
        s.is_builtin  = true;
        gs.syms[name] = s;
    };

    reg_builtin("print",       {TYPE_INT32},  TYPE_VOID);
    reg_builtin("input",       {},            TYPE_STRING);
    reg_builtin("input_int",   {},            TYPE_INT32);
    reg_builtin("input_float", {},            TYPE_FLOAT64);
    reg_builtin("to_int",      {TYPE_STRING}, TYPE_INT32);
    reg_builtin("to_float",    {TYPE_STRING}, TYPE_FLOAT64);
    reg_builtin("exit",        {TYPE_INT32},  TYPE_VOID);
    reg_builtin("panic",       {TYPE_STRING}, TYPE_VOID);
    reg_builtin("assert",      {TYPE_BOOL},   TYPE_VOID);
}

AnalysisResult analyze(Program& prog, const std::string& filename) {
    Analyser a(filename);
    register_builtins(a.global_scope);
    return a.run(prog);
}

}
