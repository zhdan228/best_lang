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

static std::optional<int64_t> as_int(const Operand& o) {
    if (auto* v = std::get_if<ConstInt>(&o))  return v->v;
    return std::nullopt;
}
static std::optional<double> as_float(const Operand& o) {
    if (auto* v = std::get_if<ConstFloat>(&o)) return v->v;
    return std::nullopt;
}
static std::optional<bool> as_bool(const Operand& o) {
    if (auto* v = std::get_if<ConstBool>(&o)) return v->v;
    return std::nullopt;
}

static bool is_const(const Operand& o) {
    return std::holds_alternative<ConstInt>(o)    ||
           std::holds_alternative<ConstUInt>(o)   ||
           std::holds_alternative<ConstFloat>(o)  ||
           std::holds_alternative<ConstBool>(o)   ||
           std::holds_alternative<ConstString>(o) ||
           std::holds_alternative<ConstUnit>(o);
}

static Operand resolve(const Operand& o, const std::unordered_map<int,Operand>& known) {
    if (auto* tv = std::get_if<TempVar>(&o)) {
        auto it = known.find(tv->id);
        if (it != known.end()) return it->second;
    }
    return o;
}

static void record_fold(const Operand& dst, Operand val, std::unordered_map<int,Operand>& known) {
    if (auto* tv = std::get_if<TempVar>(&dst))
        known[tv->id] = std::move(val);
}

static std::optional<Operand> fold_ibinop(IBinOp op, int64_t l, int64_t r) {
    switch (op) {
    case IBinOp::Add:  return ConstInt{l + r};
    case IBinOp::Sub:  return ConstInt{l - r};
    case IBinOp::Mul:  return ConstInt{l * r};
    case IBinOp::Div:  return r ? std::optional<Operand>{ConstInt{l / r}} : std::nullopt;
    case IBinOp::Mod:  return r ? std::optional<Operand>{ConstInt{l % r}} : std::nullopt;
    case IBinOp::IEq:  return ConstBool{l == r};
    case IBinOp::INeq: return ConstBool{l != r};
    case IBinOp::ILt:  return ConstBool{l  < r};
    case IBinOp::ILe:  return ConstBool{l <= r};
    case IBinOp::IGt:  return ConstBool{l  > r};
    case IBinOp::IGe:  return ConstBool{l >= r};
    default: return std::nullopt;
    }
}

static std::optional<Operand> fold_fbinop(FBinOp op, double l, double r) {
    switch (op) {
    case FBinOp::Add:  return ConstFloat{l + r};
    case FBinOp::Sub:  return ConstFloat{l - r};
    case FBinOp::Mul:  return ConstFloat{l * r};
    case FBinOp::Div:  return ConstFloat{l / r};
    case FBinOp::FEq:  return ConstBool{l == r};
    case FBinOp::FNeq: return ConstBool{l != r};
    case FBinOp::FLt:  return ConstBool{l  < r};
    case FBinOp::FLe:  return ConstBool{l <= r};
    case FBinOp::FGt:  return ConstBool{l  > r};
    case FBinOp::FGe:  return ConstBool{l >= r};
    default: return std::nullopt;
    }
}

static void fold_function(IRFunction& fn) {
    std::unordered_map<int,Operand> known;

    for (auto& instr : fn.body) {
        std::visit([&](auto& v) {
            using T = std::decay_t<decltype(v)>;

            if constexpr (std::is_same_v<T,IBinInstr>) {
                v.lhs = resolve(v.lhs, known);
                v.rhs = resolve(v.rhs, known);
                auto li = as_int(v.lhs), ri = as_int(v.rhs);
                if (li && ri)
                    if (auto res = fold_ibinop(v.op, *li, *ri))
                        record_fold(v.dst, *res, known);

            } else if constexpr (std::is_same_v<T,FBinInstr>) {
                v.lhs = resolve(v.lhs, known);
                v.rhs = resolve(v.rhs, known);
                auto lf = as_float(v.lhs), rf = as_float(v.rhs);
                if (lf && rf)
                    if (auto res = fold_fbinop(v.op, *lf, *rf))
                        record_fold(v.dst, *res, known);

            } else if constexpr (std::is_same_v<T,LBinInstr>) {
                v.lhs = resolve(v.lhs, known);
                v.rhs = resolve(v.rhs, known);
                auto lb = as_bool(v.lhs), rb = as_bool(v.rhs);
                if (lb && rb) {
                    bool res = (v.op == LBinOp::And) ? (*lb && *rb) : (*lb || *rb);
                    record_fold(v.dst, ConstBool{res}, known);
                }

            } else if constexpr (std::is_same_v<T,LUnInstr>) {
                v.src = resolve(v.src, known);
                if (auto b = as_bool(v.src))
                    record_fold(v.dst, ConstBool{!*b}, known);

            } else if constexpr (std::is_same_v<T,IUnInstr>) {
                v.src = resolve(v.src, known);
                if (auto i = as_int(v.src); i && v.op == IUnOp::Neg)
                    record_fold(v.dst, ConstInt{-*i}, known);

            } else if constexpr (std::is_same_v<T,Copy>) {
                v.src = resolve(v.src, known);
                if (is_const(v.src))
                    record_fold(v.dst, v.src, known);

            } else if constexpr (std::is_same_v<T,JumpFalse> ||
                                  std::is_same_v<T,JumpTrue>) {
                v.cond = resolve(v.cond, known);

            } else if constexpr (std::is_same_v<T,ReturnVal>) {
                v.val = resolve(v.val, known);
            }
        }, instr);
    }

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
                    if (it != known.end())
                        instr = Copy{v.dst, it->second};
                }
            }
        }, instr);
    }
}

void optimize(IRProgram& prog) {
    for (auto& fn : prog.functions)
        fold_function(fn);
}

}
