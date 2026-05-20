#include "optimizer.hpp"
#include <unordered_map>
#include <optional>

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
    return std::get_if<ConstInt>(&o)    || std::get_if<ConstUInt>(&o)  ||
           std::get_if<ConstFloat>(&o)  || std::get_if<ConstBool>(&o)  ||
           std::get_if<ConstChar>(&o)   ||
           std::get_if<ConstString>(&o) || std::get_if<ConstUnit>(&o);
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

static void fold_pass1(IRFunction& fn, std::unordered_map<int,Operand>& known) {
    for (auto& instr : fn.body) {
        if (auto* v = std::get_if<IBinInstr>(&instr)) {
            v->lhs = resolve(v->lhs, known); v->rhs = resolve(v->rhs, known);
            if (auto li = as_int(v->lhs), ri = as_int(v->rhs); li && ri)
                if (auto res = fold_ibinop(v->op, *li, *ri)) record_fold(v->dst, *res, known);
        } else if (auto* v = std::get_if<FBinInstr>(&instr)) {
            v->lhs = resolve(v->lhs, known); v->rhs = resolve(v->rhs, known);
            if (auto lf = as_float(v->lhs), rf = as_float(v->rhs); lf && rf)
                if (auto res = fold_fbinop(v->op, *lf, *rf)) record_fold(v->dst, *res, known);
        } else if (auto* v = std::get_if<LBinInstr>(&instr)) {
            v->lhs = resolve(v->lhs, known); v->rhs = resolve(v->rhs, known);
            if (auto lb = as_bool(v->lhs), rb = as_bool(v->rhs); lb && rb)
                record_fold(v->dst, ConstBool{v->op==LBinOp::And ? (*lb&&*rb) : (*lb||*rb)}, known);
        } else if (auto* v = std::get_if<LUnInstr>(&instr)) {
            v->src = resolve(v->src, known);
            if (auto b = as_bool(v->src)) record_fold(v->dst, ConstBool{!*b}, known);
        } else if (auto* v = std::get_if<IUnInstr>(&instr)) {
            v->src = resolve(v->src, known);
            if (auto i = as_int(v->src); i && v->op == IUnOp::Neg)
                record_fold(v->dst, ConstInt{-*i}, known);
        } else if (auto* v = std::get_if<Copy>(&instr)) {
            v->src = resolve(v->src, known);
            if (is_const(v->src)) record_fold(v->dst, v->src, known);
        } else if (auto* v = std::get_if<JumpFalse>(&instr)) {
            v->cond = resolve(v->cond, known);
        } else if (auto* v = std::get_if<JumpTrue>(&instr)) {
            v->cond = resolve(v->cond, known);
        } else if (auto* v = std::get_if<ReturnVal>(&instr)) {
            v->val = resolve(v->val, known);
        }
    }
}

static void fold_pass2(IRFunction& fn, const std::unordered_map<int,Operand>& known) {
    for (auto& instr : fn.body) {
        const Operand* dst = nullptr;
        if (auto* v = std::get_if<IBinInstr>(&instr)) dst = &v->dst;
        else if (auto* v = std::get_if<FBinInstr>(&instr)) dst = &v->dst;
        else if (auto* v = std::get_if<LBinInstr>(&instr)) dst = &v->dst;
        else if (auto* v = std::get_if<IUnInstr>(&instr))  dst = &v->dst;
        else if (auto* v = std::get_if<FUnInstr>(&instr))  dst = &v->dst;
        else if (auto* v = std::get_if<LUnInstr>(&instr))  dst = &v->dst;
        if (!dst) continue;
        if (auto* tv = std::get_if<TempVar>(dst)) {
            auto it = known.find(tv->id);
            if (it != known.end()) instr = Copy{*dst, it->second};
        }
    }
}

static void fold_function(IRFunction& fn) {
    std::unordered_map<int,Operand> known;
    fold_pass1(fn, known);
    fold_pass2(fn, known);
}

void optimize(IRProgram& prog) {
    for (auto& fn : prog.functions)
        fold_function(fn);
}

}
