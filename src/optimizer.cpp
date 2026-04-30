#include "optimizer.hpp"
#include <unordered_map>
#include <optional>

/*
 * Оптимизатор IR — свёртка констант (constant folding).
 *
 * Для каждой функции отслеживаем таблицу known: temp_id → константа.
 * Если оба операнда инструкции — известные константы, вычисляем результат
 * на этапе компиляции и заменяем инструкцию на Copy(dst, константа).
 *
 * Пример: t0 = 2 + 3  →  t0 = 5  (лишнее IADD не попадает в байткод)
 */

namespace IR {

// ── Constant folding ──────────────────────────────────────────────────────
// Для каждой инструкции в теле функции:
//   если оба операнда — известные константы, заменяем на Copy(dst, результат).
//
// Отслеживаем известные константы: temp_id → значение.

static std::optional<int64_t>  as_int(const Operand& o) {
    if (auto* v = std::get_if<ConstInt>(&o))  return v->v;
    return std::nullopt;
}
static std::optional<double>   as_float(const Operand& o) {
    if (auto* v = std::get_if<ConstFloat>(&o)) return v->v;
    return std::nullopt;
}
static std::optional<bool>     as_bool(const Operand& o) {
    if (auto* v = std::get_if<ConstBool>(&o)) return v->v;
    return std::nullopt;
}

static bool is_const(const Operand& o) {
    return std::holds_alternative<ConstInt>(o)   ||
           std::holds_alternative<ConstUInt>(o)  ||
           std::holds_alternative<ConstFloat>(o) ||
           std::holds_alternative<ConstBool>(o)  ||
           std::holds_alternative<ConstString>(o)||
           std::holds_alternative<ConstUnit>(o);
}

static Operand resolve(const Operand& o,
                       const std::unordered_map<int,Operand>& known) {
    if (auto* tv = std::get_if<TempVar>(&o)) {
        auto it = known.find(tv->id);
        if (it != known.end()) return it->second;
    }
    return o;
}

static void fold_function(IRFunction& fn) {
    std::unordered_map<int,Operand> known; // temp_id → const value

    for (auto& instr : fn.body) {
        std::visit([&](auto& v) {
            using T = std::decay_t<decltype(v)>;

            if constexpr (std::is_same_v<T,IBinInstr>) {
                v.lhs = resolve(v.lhs, known);
                v.rhs = resolve(v.rhs, known);
                auto li = as_int(v.lhs), ri = as_int(v.rhs);
                if (li && ri) {
                    int64_t res;
                    bool ok = true;
                    switch (v.op) {
                    case IBinOp::Add: res = *li + *ri; break;
                    case IBinOp::Sub: res = *li - *ri; break;
                    case IBinOp::Mul: res = *li * *ri; break;
                    case IBinOp::Div:
                        if (*ri == 0) { ok = false; break; }
                        res = *li / *ri; break;
                    case IBinOp::Mod:
                        if (*ri == 0) { ok = false; break; }
                        res = *li % *ri; break;
                    case IBinOp::IEq:  res = (*li == *ri)?1:0; break;
                    case IBinOp::INeq: res = (*li != *ri)?1:0; break;
                    case IBinOp::ILt:  res = (*li  < *ri)?1:0; break;
                    case IBinOp::ILe:  res = (*li <= *ri)?1:0; break;
                    case IBinOp::IGt:  res = (*li  > *ri)?1:0; break;
                    case IBinOp::IGe:  res = (*li >= *ri)?1:0; break;
                    default: ok = false;
                    }
                    if (ok) {
                        // сравнения дают bool
                        Operand folded;
                        if (v.op >= IBinOp::IEq)
                            folded = ConstBool{res != 0};
                        else
                            folded = ConstInt{res};
                        if (auto* tv = std::get_if<TempVar>(&v.dst))
                            known[tv->id] = folded;
                        // результат сохранён в known — второй проход заменит на Copy
                    }
                }
            } else if constexpr (std::is_same_v<T,FBinInstr>) {
                v.lhs = resolve(v.lhs, known);
                v.rhs = resolve(v.rhs, known);
                auto lf = as_float(v.lhs), rf = as_float(v.rhs);
                if (lf && rf) {
                    double res;
                    bool ok = true;
                    switch (v.op) {
                    case FBinOp::Add: res = *lf + *rf; break;
                    case FBinOp::Sub: res = *lf - *rf; break;
                    case FBinOp::Mul: res = *lf * *rf; break;
                    case FBinOp::Div: res = *lf / *rf; break;
                    case FBinOp::FEq: if (auto* tv = std::get_if<TempVar>(&v.dst)) known[tv->id] = ConstBool{*lf == *rf}; ok=false; break;
                    case FBinOp::FNeq:if (auto* tv = std::get_if<TempVar>(&v.dst)) known[tv->id] = ConstBool{*lf != *rf}; ok=false; break;
                    case FBinOp::FLt: if (auto* tv = std::get_if<TempVar>(&v.dst)) known[tv->id] = ConstBool{*lf  < *rf}; ok=false; break;
                    case FBinOp::FLe: if (auto* tv = std::get_if<TempVar>(&v.dst)) known[tv->id] = ConstBool{*lf <= *rf}; ok=false; break;
                    case FBinOp::FGt: if (auto* tv = std::get_if<TempVar>(&v.dst)) known[tv->id] = ConstBool{*lf  > *rf}; ok=false; break;
                    case FBinOp::FGe: if (auto* tv = std::get_if<TempVar>(&v.dst)) known[tv->id] = ConstBool{*lf >= *rf}; ok=false; break;
                    default: ok=false;
                    }
                    if (ok && std::get_if<TempVar>(&v.dst))
                        known[std::get<TempVar>(v.dst).id] = ConstFloat{res};
                }
            } else if constexpr (std::is_same_v<T,LBinInstr>) {
                v.lhs = resolve(v.lhs, known);
                v.rhs = resolve(v.rhs, known);
                auto lb = as_bool(v.lhs), rb = as_bool(v.rhs);
                if (lb && rb) {
                    bool res = (v.op == LBinOp::And) ? (*lb && *rb) : (*lb || *rb);
                    if (auto* tv = std::get_if<TempVar>(&v.dst))
                        known[tv->id] = ConstBool{res};
                }
            } else if constexpr (std::is_same_v<T,LUnInstr>) {
                v.src = resolve(v.src, known);
                auto b = as_bool(v.src);
                if (b) {
                    if (auto* tv = std::get_if<TempVar>(&v.dst))
                        known[tv->id] = ConstBool{!*b};
                }
            } else if constexpr (std::is_same_v<T,IUnInstr>) {
                v.src = resolve(v.src, known);
                auto i = as_int(v.src);
                if (i && v.op == IUnOp::Neg) {
                    if (auto* tv = std::get_if<TempVar>(&v.dst))
                        known[tv->id] = ConstInt{-*i};
                }
            } else if constexpr (std::is_same_v<T,Copy>) {
                v.src = resolve(v.src, known);
                if (is_const(v.src))
                    if (auto* tv = std::get_if<TempVar>(&v.dst))
                        known[tv->id] = v.src;
            } else if constexpr (std::is_same_v<T,JumpFalse>) {
                v.cond = resolve(v.cond, known);
            } else if constexpr (std::is_same_v<T,JumpTrue>) {
                v.cond = resolve(v.cond, known);
            } else if constexpr (std::is_same_v<T,ReturnVal>) {
                v.val = resolve(v.val, known);
            }
        }, instr);
    }

    // Второй проход: заменяем инструкции с известным результатом на Copy
    for (auto& instr : fn.body) {
        std::visit([&](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T,IBinInstr> ||
                          std::is_same_v<T,FBinInstr> ||
                          std::is_same_v<T,LBinInstr> ||
                          std::is_same_v<T,IUnInstr>  ||
                          std::is_same_v<T,FUnInstr>  ||
                          std::is_same_v<T,LUnInstr>) {
                if (auto* tv = std::get_if<TempVar>(&v.dst)) {
                    auto it = known.find(tv->id);
                    if (it != known.end()) {
                        instr = Copy{v.dst, it->second};
                    }
                }
            }
        }, instr);
    }
}


static void remove_dead_code(IRFunction& fn) {
    std::vector<Instr> live;
    bool reachable = true;
    for (auto& instr : fn.body) {
        if (std::holds_alternative<Label>(instr)) reachable = true;
        if (reachable) live.push_back(instr);
        if (std::holds_alternative<Jump>(instr) ||
            std::holds_alternative<Return>(instr) ||
            std::holds_alternative<ReturnVal>(instr))
            reachable = false;
    }
    fn.body = std::move(live);
}

void optimize(IRProgram& prog) {
    for (auto& fn : prog.functions)
        fold_function(fn);
}

} 
