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

} // namespace Semantic
