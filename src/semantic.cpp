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

namespace Semantic {

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

struct Analyser {
    const std::string& filename;
    std::vector<SemanticError> errors;

    // таблица псевдонимов типов
    std::unordered_map<std::string, TypePtr> type_aliases;
    // таблица структур
    std::unordered_map<std::string, Symbol>  struct_table;
    // таблица пространств имён
    std::unordered_map<std::string, std::unordered_map<std::string, Symbol>> ns_table;

    // глобальные символы
    Scope global_scope;
    int   next_global = 0;

    // контекст текущей функции
    TypePtr  current_ret_type;
    bool     in_loop = false;
    int      next_slot = 0;   // следующий свободный слот
    std::vector<Scope*> scope_stack; // последний = самый внутренний

    // результат анализа
    std::vector<GlobalVarDecl*> globals_out;
    std::vector<FunDecl*>       functions_out;

    void err(SrcLoc l, const std::string& msg) {
        errors.push_back({l.file.empty() ? filename : l.file, l.line, l.col, msg});
    }

    // имя для перегруженных функций: "foo@int32,float64"
    // Позволяет codegen различать перегрузки
    static std::string mangle(const std::string& base,
                               const std::vector<TypePtr>& params) {
        std::string s = base + "@";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i) s += ",";
            s += params[i] ? params[i]->to_string() : "?";
        }
        return s;
    }

    TypePtr resolve(TypePtr t) {
        if (!t) return nullptr;
        if (t->kind == Type::Kind::Struct) {
            auto it = type_aliases.find(t->struct_name);
            if (it != type_aliases.end()) return resolve(it->second);
            if (!struct_table.count(t->struct_name)) return t; 
            return t; 
        }
        if (t->kind == Type::Kind::Array) {
            auto elem = resolve(t->elem_type);
            if (*elem != *t->elem_type)
                return Type::make_array(elem, t->array_size);
        }
        return t;
    }

    Scope& cur_scope() { return *scope_stack.back(); }

    void push_scope() {
        auto* s = new Scope();
        s->parent = scope_stack.empty() ? &global_scope : scope_stack.back();
        scope_stack.push_back(s);
    }
    void pop_scope() {
        delete scope_stack.back();
        scope_stack.pop_back();
    }

    int alloc_slot() { return next_slot++; }

    bool declare_local(const std::string& name, TypePtr type, bool is_val, SrcLoc loc) {
        if (cur_scope().has_local(name)) {
            err(loc, "redeclaration of '" + name + "' in the same scope");
            return false;
        }
        int slot = alloc_slot();
        cur_scope().syms[name] = Symbol{Symbol::Kind::LocalVar, type, is_val, false, slot};
        return true;
    }

    // Неявное числовое расширение: int8→int32, int→float64 и т.д.
    // Вставляет CastExpr в AST если нужно
    bool can_widen(TypePtr from, TypePtr to) {
        if (!from || !to || *from == *to) return false;
        if (!from->is_numeric() || !to->is_numeric()) return false;
        // float64 > float32 > целые
        if (to->kind == Type::Kind::Float64 && from->is_int())   return true;
        if (to->kind == Type::Kind::Float64 && from->kind == Type::Kind::Float32) return true;
        if (to->kind == Type::Kind::Float32 && from->is_int())   return true;
        // расширение знакового целого
        if (from->is_signed_int() && to->is_signed_int())
            return to->int_bits() > from->int_bits();
        // расширение беззнакового целого
        if (from->is_unsigned_int() && to->is_unsigned_int())
            return to->int_bits() > from->int_bits();
        return false;
    }

    // Вставляет неявное приведение если нужно
    void maybe_widen(ExprPtr& expr, TypePtr target) {
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

    // Вспомогательные методы проверки типов 
    bool same_type(TypePtr a, TypePtr b) {
        if (!a || !b) return false;
        return *a == *b;
    }

    TypePtr type_for_int_suffix(const std::string& suf) {
        if (suf == "i8")  return TYPE_INT8;
        if (suf == "i16") return TYPE_INT16;
        if (suf == "i32") return TYPE_INT32;
        if (suf == "i64") return TYPE_INT64;
        if (suf == "u8")  return TYPE_UINT8;
        if (suf == "u16") return TYPE_UINT16;
        if (suf == "u32") return TYPE_UINT32;
        if (suf == "u64") return TYPE_UINT64;
        return TYPE_INT32; // default
    }

    TypePtr type_for_float_suffix(const std::string& suf) {
        if (suf == "f32") return TYPE_FLOAT32;
        return TYPE_FLOAT64; 
    }

    // Проверка допустимости приведения 
    bool cast_ok(TypePtr from, TypePtr to) {
        if (!from || !to) return false;
        if (*from == *to) return true;
        if (from->is_int()   && to->is_int())   return true;
        if (from->is_float() && to->is_float()) return true;
        if (from->is_int()   && to->is_float()) return true;
        if (from->is_float() && to->is_int())   return true;
        return false;
    }

    // Проверка типов выражений 
    TypePtr check_expr(Expr& e) {
        switch (e.kind) {
        case Expr::Kind::IntLit: {
            auto& il = static_cast<IntLitExpr&>(e);
            e.type = type_for_int_suffix(il.suffix);
            return e.type;
        }
        case Expr::Kind::FloatLit: {
            auto& fl = static_cast<FloatLitExpr&>(e);
            e.type = type_for_float_suffix(fl.suffix);
            return e.type;
        }
        case Expr::Kind::BoolLit:
            e.type = TYPE_BOOL;
            return e.type;
        case Expr::Kind::StringLit:
            e.type = TYPE_STRING;
            return e.type;

        case Expr::Kind::Ident: {
            auto& id = static_cast<IdentExpr&>(e);
            Symbol* sym = cur_scope().lookup(id.name);
            if (!sym) {
                
                sym = global_scope.lookup(id.name);
            }
            if (!sym) {
                err(e.loc, "undefined identifier '" + id.name + "'");
                e.type = TYPE_INT32;
                return e.type;
            }
            if (sym->kind == Symbol::Kind::Function) {
                e.type = sym->type; 
                return e.type;
            }
            e.type = sym->type;
            return e.type;
        }

        case Expr::Kind::NullLit:
            e.type = Type::make_nullable(TYPE_VOID); 
            return e.type;

        case Expr::Kind::TupleLit: {
            auto& tl = static_cast<TupleLitExpr&>(e);
            std::vector<TypePtr> elem_types;
            for (auto& el : tl.elements)
                elem_types.push_back(check_expr(*el));
            e.type = Type::make_tuple(std::move(elem_types));
            return e.type;
        }

        case Expr::Kind::TupleIndex: {
            auto& ti = static_cast<TupleIndexExpr&>(e);
            auto ot = check_expr(*ti.object);
            if (!ot || !ot->is_tuple()) {
                err(e.loc, "tuple index on non-tuple type");
                e.type = TYPE_INT32; return e.type;
            }
            if (ti.index < 0 || ti.index >= static_cast<int>(ot->tuple_elems.size())) {
                err(e.loc, "tuple index " + std::to_string(ti.index) +
                    " out of range (size " + std::to_string(ot->tuple_elems.size()) + ")");
                e.type = TYPE_INT32; return e.type;
            }
            e.type = ot->tuple_elems[ti.index];
            return e.type;
        }

        case Expr::Kind::NamespaceAccess: {
            auto& na = static_cast<NamespaceAccessExpr&>(e);
            auto nit = ns_table.find(na.ns_name);
            if (nit == ns_table.end()) {
                err(e.loc, "unknown namespace '" + na.ns_name + "'");
                e.type = TYPE_INT32;
                return e.type;
            }
            auto mit = nit->second.find(na.member);
            if (mit == nit->second.end()) {
                err(e.loc, "'" + na.member + "' not found in namespace '" + na.ns_name + "'");
                e.type = TYPE_INT32;
                return e.type;
            }
            e.type = mit->second.type;
            return e.type;
        }

        case Expr::Kind::BinOp: {
            auto& b = static_cast<BinOpExpr&>(e);
            auto lt = check_expr(*b.lhs);
            auto rt = check_expr(*b.rhs);

            // Логические операторы: оба операнда должны быть bool
            if (b.op == BinOpKind::And || b.op == BinOpKind::Or) {
                if (!lt || !lt->is_bool())
                    err(b.lhs->loc, "logical operator requires bool, got " + (lt?lt->to_string():"?"));
                if (!rt || !rt->is_bool())
                    err(b.rhs->loc, "logical operator requires bool, got " + (rt?rt->to_string():"?"));
                e.type = TYPE_BOOL;
                return e.type;
            }
            // Операторы сравнения
            if (b.op==BinOpKind::Eq||b.op==BinOpKind::NEq||
                b.op==BinOpKind::Lt||b.op==BinOpKind::Le||
                b.op==BinOpKind::Gt||b.op==BinOpKind::Ge) {
                bool types_ok = (lt && rt) && (
                    *lt == *rt ||
                    (lt->is_nullable() && rt->is_nullable()) ||
                    can_widen(lt, rt) || can_widen(rt, lt)
                );
                if (lt && rt && !types_ok) {
                    err(e.loc, "type mismatch in comparison: " +
                        lt->to_string() + " vs " + rt->to_string());
                } else if (lt && lt->is_numeric() && rt && rt->is_numeric() && *lt != *rt) {
                    if (can_widen(lt, rt)) maybe_widen(b.lhs, rt);
                    else if (can_widen(rt, lt)) maybe_widen(b.rhs, lt);
                }
                if (lt && lt->is_string() &&
                    b.op != BinOpKind::Eq && b.op != BinOpKind::NEq)
                    err(e.loc, "only == and != are defined for string");
                e.type = TYPE_BOOL;
                return e.type;
            }
            // Арифметика: + для строк = конкатенация
            if (b.op == BinOpKind::Add && lt && lt->is_string()) {
                if (!rt || !rt->is_string())
                    err(e.loc, "string concatenation requires string on both sides");
                b.op   = BinOpKind::StrConcat;
                e.type = TYPE_STRING;
                return e.type;
            }
            // Арифметика: с неявным расширением
            if (lt && rt) {
                if (*lt != *rt) {
                    if (can_widen(lt, rt)) {
                        maybe_widen(b.lhs, rt);
                        lt = rt;
                    } else if (can_widen(rt, lt)) {
                        maybe_widen(b.rhs, lt);
                        rt = lt;
                    } else {
                        err(e.loc, "type mismatch: " + lt->to_string() + " vs " + rt->to_string());
                    }
                }
                if (!lt->is_numeric())
                    err(e.loc, "arithmetic not defined for type " + lt->to_string());
            }
            e.type = lt ? lt : TYPE_INT32;
            return e.type;
        }

        case Expr::Kind::UnaryOp: {
            auto& u = static_cast<UnaryOpExpr&>(e);
            auto ot = check_expr(*u.operand);
            if (u.op == UnaryOpKind::Not) {
                if (!ot || !ot->is_bool())
                    err(e.loc, "'not' requires bool operand");
                e.type = TYPE_BOOL;
            } else { 
                if (!ot || !ot->is_numeric())
                    err(e.loc, "unary '-' requires numeric operand");
                e.type = ot ? ot : TYPE_INT32;
            }
            return e.type;
        }

        case Expr::Kind::Cast: {
            auto& c = static_cast<CastExpr&>(e);
            auto ot = check_expr(*c.operand);
            c.target = resolve(c.target);
            if (!cast_ok(ot, c.target))
                err(e.loc, "invalid cast from " + (ot?ot->to_string():"?") +
                    " to " + c.target->to_string());
            e.type = c.target;
            return e.type;
        }

        case Expr::Kind::Call: {
            auto& c = static_cast<CallExpr&>(e);
            std::string fname;
            Symbol* fsym = nullptr;

            if (c.callee->kind == Expr::Kind::Field) {
                auto& fe = static_cast<FieldExpr&>(*c.callee);
                auto obj_type = check_expr(*fe.object);
                auto obj_rt   = resolve(obj_type);
                if (obj_rt && obj_rt->is_string()) {
                    auto& method = fe.field_name;
                    if (method == "len") {
                        if (!c.args.empty())
                            err(e.loc, "str.len() takes no arguments");
                        e.type = TYPE_UINT64; return e.type;
                    }
                    err(e.loc, "unknown method '" + method + "' on string");
                    e.type = TYPE_INT32; return e.type;
                }
                if (obj_rt && obj_rt->is_dynarray()) {
                    auto& method = fe.field_name;
                    for (auto& a : c.args) check_expr(*a);
                    if (method == "push") {
                        e.type = TYPE_VOID; return e.type;
                    } else if (method == "pop") {
                        e.type = obj_rt->elem_type; return e.type;
                    } else if (method == "len") {
                        e.type = TYPE_UINT64; return e.type;
                    } else if (method == "get") {
                        e.type = obj_rt->elem_type; return e.type;
                    } else {
                        err(e.loc, "unknown method '" + method + "' on dynamic array");
                        e.type = TYPE_INT32; return e.type;
                    }
                }
                if (obj_rt && obj_rt->is_struct()) {
                    std::string full = obj_rt->struct_name + "::" + fe.field_name;
                    fsym  = global_scope.lookup(full);
                    fname = full;
                }
                if (!fsym) {
                    err(e.loc, "unknown method '" + fe.field_name + "' on type " +
                        (obj_rt ? obj_rt->to_string() : "?"));
                    for (auto& a : c.args) check_expr(*a);
                    e.type = TYPE_INT32; return e.type;
                }
                // проверяем аргументы с self как первым
                // число аргументов = параметры - 1 (self неявный)
                if (c.args.size() + 1 != fsym->param_types.size()) {
                    err(e.loc, "method '" + fe.field_name + "' expects " +
                        std::to_string(fsym->param_types.size()-1) + " arguments, got " +
                        std::to_string(c.args.size()));
                }
                for (size_t i = 0; i < c.args.size(); ++i) {
                    auto at = check_expr(*c.args[i]);
                    size_t pi = i + 1; // пропускаем self
                    if (pi < fsym->param_types.size() && at && fsym->param_types[pi]) {
                        auto rp = resolve(fsym->param_types[pi]);
                        auto ra = resolve(at);
                        bool arr_compat = rp->is_dynarray() &&
                            (ra->is_array()||ra->is_dynarray()) &&
                            *rp->elem_type == *ra->elem_type;
                        if (*ra != *rp && !arr_compat) maybe_widen(c.args[i], rp);
                    }
                }
                e.type = fsym->type;
                return e.type;
            }

            // Обычный вызов: name(args) или ns::name(args) ────────────
            if (c.callee->kind == Expr::Kind::Ident) {
                fname = static_cast<IdentExpr&>(*c.callee).name;
                // Сначала проверяем перегрузки
                auto* ovl = global_scope.lookup_overloads(fname);
                if (ovl && !ovl->empty()) {
                    std::vector<TypePtr> arg_types;
                    for (auto& a : c.args) arg_types.push_back(check_expr(*a));
                    for (auto& sym : *ovl) {
                        if (sym.param_types.size() != arg_types.size()) continue;
                        bool match = true;
                        for (size_t i = 0; i < arg_types.size(); ++i) {
                            auto rp = resolve(sym.param_types[i]);
                            auto ra = resolve(arg_types[i]);
                            if (ra && rp && *ra != *rp && !can_widen(ra, rp))
                                { match = false; break; }
                        }
                        if (match) {
                            static_cast<IdentExpr&>(*c.callee).name =
                                mangle(fname, sym.param_types);
                            e.type = sym.type;
                            return e.type;
                        }
                    }
                    err(e.loc, "no matching overload of '" + fname + "' for given arguments");
                    e.type = TYPE_INT32; return e.type;
                }
                fsym = global_scope.lookup(fname);
                if (!fsym) fsym = cur_scope().lookup(fname);
            } else if (c.callee->kind == Expr::Kind::NamespaceAccess) {
                auto& na = static_cast<NamespaceAccessExpr&>(*c.callee);
                auto nit = ns_table.find(na.ns_name);
                if (nit != ns_table.end()) {
                    auto mit = nit->second.find(na.member);
                    if (mit != nit->second.end()) fsym = &mit->second;
                }
                fname = na.ns_name + "::" + na.member;
            }
            if (!fsym || fsym->kind != Symbol::Kind::Function) {
                err(e.loc, "'" + fname + "' is not a function");
                for (auto& a : c.args) check_expr(*a);
                e.type = TYPE_INT32;
                return e.type;
            }
            if (!fsym->is_builtin && c.args.size() != fsym->param_types.size()) {
                err(e.loc, "function '" + fname + "' expects " +
                    std::to_string(fsym->param_types.size()) + " arguments, got " +
                    std::to_string(c.args.size()));
            }
            // Проверяем число аргументов для конкретных встроенных
            if (fsym->is_builtin) {
                if (fname == "print") {
                    if (c.args.size() != 1)
                        err(e.loc, "print() expects 1 positional argument");
                    // Проверяем именованный аргумент end: только string
                    for (auto& na : c.named_args) {
                        if (na.name == "end") {
                            auto et = check_expr(*na.value);
                            if (!et || !et->is_string())
                                err(na.value->loc, "print() end= must be string");
                        } else {
                            err(e.loc, "print() unknown named argument '" + na.name + "'");
                        }
                    }
                }
                if (fname == "input" || fname == "input_int" || fname == "input_float") {
                    // 0 или 1 аргумент (необязательная строка-подсказка, как в Python)
                    if (c.args.size() > 1)
                        err(e.loc, fname + "() expects 0 or 1 argument (prompt string)");
                    if (c.args.size() == 1) {
                        auto pt = check_expr(*c.args[0]);
                        if (!pt || !pt->is_string())
                            err(c.args[0]->loc, fname + "() prompt must be string");
                    }
                }
                if ((fname == "to_int" || fname == "to_float") && c.args.size() != 1)
                    err(e.loc, fname + "() expects 1 argument");
                if (fname == "exit" && c.args.size() != 1)
                    err(e.loc, "exit() expects 1 argument");
                if (fname == "panic" && c.args.size() != 1)
                    err(e.loc, "panic() expects 1 argument");
            }
            for (size_t i = 0; i < c.args.size(); ++i) {
                auto at = check_expr(*c.args[i]);
                // Пропускаем строгую проверку типов для встроенных
                if (fsym->is_builtin) continue;
                if (i < fsym->param_types.size() && at && fsym->param_types[i]) {
                    auto resolved_param = resolve(fsym->param_types[i]);
                    auto resolved_arg   = resolve(at);
                    bool arr_compat = resolved_param->is_dynarray() &&
                        (resolved_arg->is_array()||resolved_arg->is_dynarray()) &&
                        *resolved_param->elem_type == *resolved_arg->elem_type;
                    bool null_compat = resolved_param->is_nullable() &&
                        (resolved_arg->is_nullable() ||
                         (resolved_param->elem_type && *resolved_param->elem_type == *resolved_arg));
                    if (*resolved_arg != *resolved_param && !arr_compat && !null_compat)
                        err(c.args[i]->loc, "argument " + std::to_string(i+1) +
                            " type mismatch: expected " + resolved_param->to_string() +
                            ", got " + resolved_arg->to_string());
                }
            }
            e.type = fsym->type; 
            return e.type;
        }

        case Expr::Kind::Index: {
            auto& ix = static_cast<IndexExpr&>(e);
            auto arrt = check_expr(*ix.arr);
            auto idxt = check_expr(*ix.idx);
            auto arrt_r = resolve(arrt);
            if (!arrt_r || (!arrt_r->is_array() && !arrt_r->is_dynarray()))
                err(e.loc, "indexing non-array type");
            if (!idxt || !idxt->is_int())
                err(ix.idx->loc, "array index must be integer");
            e.type = (arrt_r && (arrt_r->is_array()||arrt_r->is_dynarray()))
                     ? arrt_r->elem_type : TYPE_INT32;
            return e.type;
        }

        case Expr::Kind::Field: {
            auto& fe = static_cast<FieldExpr&>(e);
            auto ot = check_expr(*fe.object);
            if (!ot || !ot->is_struct()) {
                err(e.loc, "field access on non-struct type");
                e.type = TYPE_INT32;
                return e.type;
            }
            auto sit = struct_table.find(ot->struct_name);
            if (sit == struct_table.end()) {
                err(e.loc, "unknown struct type '" + ot->struct_name + "'");
                e.type = TYPE_INT32;
                return e.type;
            }
            auto& fields = sit->second.fields;
            for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
                if (fields[i].name == fe.field_name) {
                    fe.field_idx = i;
                    e.type = fields[i].type;
                    return e.type;
                }
            }
            err(e.loc, "struct '" + ot->struct_name + "' has no field '" + fe.field_name + "'");
            e.type = TYPE_INT32;
            return e.type;
        }

        case Expr::Kind::ArrayLit: {
            auto& al = static_cast<ArrayLitExpr&>(e);
            TypePtr elem;
            for (auto& el : al.elements) {
                auto t = check_expr(*el);
                if (!elem) elem = t;
                else if (t && *t != *elem)
                    err(el->loc, "array literal element type mismatch");
            }
            if (!elem) elem = TYPE_INT32; 
            e.type = Type::make_array(elem, (int64_t)al.elements.size());
            return e.type;
        }

        case Expr::Kind::StructLit: {
            auto& sl = static_cast<StructLitExpr&>(e);
            auto sit = struct_table.find(sl.type_name);
            if (sit == struct_table.end()) {
                err(e.loc, "unknown struct '" + sl.type_name + "'");
                for (auto& f : sl.fields) check_expr(*f.value);
                e.type = Type::make_struct(sl.type_name);
                return e.type;
            }
            auto& decl_fields = sit->second.fields;
            // проверяем каждое поле
            for (auto& sf : sl.fields) {
                auto vt = check_expr(*sf.value);
                bool found = false;
                for (auto& df : decl_fields) {
                    if (df.name == sf.name) {
                        found = true;
                        if (vt && *vt != *df.type) {
                            bool arr_compat = df.type->is_dynarray() &&
                                (vt->is_array()||vt->is_dynarray()) &&
                                *df.type->elem_type == *vt->elem_type;
                            bool null_compat = df.type->is_nullable() &&
                                (vt->is_nullable() ||
                                 (df.type->elem_type && *df.type->elem_type == *vt));
                            if (!arr_compat && !null_compat)
                                err(sf.value->loc,
                                    "field '" + sf.name + "' expects " + df.type->to_string() +
                                    ", got " + vt->to_string());
                        }
                        break;
                    }
                }
                if (!found)
                    err(e.loc, "struct '" + sl.type_name + "' has no field '" + sf.name + "'");
            }
            // все поля должны быть заполнены
            for (auto& df : decl_fields) {
                bool found = false;
                for (auto& sf : sl.fields) if (sf.name == df.name) { found = true; break; }
                if (!found)
                    err(e.loc, "missing field '" + df.name + "' in struct literal");
            }
            e.type = Type::make_struct(sl.type_name);
            return e.type;
        }

        default:
            e.type = TYPE_INT32;
            return e.type;
        }
    }

    // Проверяет lvalue и возвращает его тип ─────────────────────────────────
    TypePtr check_lvalue(LValue& lv, bool allow_mut) {
        switch (lv.kind) {
        case LValue::Kind::Ident: {
            Symbol* sym = cur_scope().lookup(lv.name);
            if (!sym) sym = global_scope.lookup(lv.name);
            if (!sym) {
                err(lv.loc, "undefined identifier '" + lv.name + "'");
                return TYPE_INT32;
            }
            // val запрещает только переприсваивание самой переменной.
            // Изменение полей/элементов разрешено.
            if (allow_mut && sym->is_val && lv.kind == LValue::Kind::Ident)
                err(lv.loc, "cannot assign to immutable binding '" + lv.name + "'");
            return sym->type;
        }
        case LValue::Kind::Index: {
            auto arrt = check_lvalue(*lv.base, false); // разрешаем индексирование val-массивов
            auto idxt = check_expr(*lv.idx);
            if (!arrt || !arrt->is_array())
                err(lv.loc, "indexing non-array");
            if (!idxt || !idxt->is_int())
                err(lv.idx->loc, "array index must be integer");
            return (arrt && arrt->is_array()) ? arrt->elem_type : TYPE_INT32;
        }
        case LValue::Kind::Field: {
            auto ot = check_lvalue(*lv.base, false); // разрешаем изменение полей val-структур
            if (!ot || !ot->is_struct()) {
                err(lv.loc, "field access on non-struct");
                return TYPE_INT32;

} // namespace Semantic
