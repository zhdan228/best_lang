#include "lowering.hpp"
#include <stdexcept>
#include <cassert>
#include <functional>
#include <unordered_map>

/*
 * Понижение AST → IR (lowering).
 *
 * Обходит дерево разбора рекурсивно. Для каждого выражения возвращает Operand —
 * временную переменную или константу, куда помещён результат.
 * Управляющие конструкции (if, while, for) разворачиваются в метки и прыжки.
 * Вызовы методов на динамических массивах и структурах (impl) транслируются
 * в соответствующие IR-инструкции или вызовы квалифицированных функций.
 */

namespace IR {

struct FnCtx {
    IRFunction&  fn;
    int&         next_temp;
    int          label_ctr = 0;

    // таблица символов: имя → слот
    std::unordered_map<std::string, int> locals;

    // метки цикла для break/continue
    struct LoopInfo { std::string cont_label; std::string break_label; };
    std::vector<LoopInfo> loops;

    Operand new_temp() {
        return TempVar{next_temp++};
    }
    std::string new_label(const std::string& hint = "L") {
        return hint + std::to_string(label_ctr++);
    }

    void emit(Instr i) { fn.body.push_back(std::move(i)); }

    Operand local_op(int slot, const std::string& name) {
        return LocalVar{slot, name};
    }
};

// Ищет слот по имени в локальной или глобальной таблице
// Возвращает nullopt если не найдено
static std::optional<Operand>
lookup_local(FnCtx& ctx, const std::string& name) {
    auto it = ctx.locals.find(name);
    if (it != ctx.locals.end())
        return LocalVar{it->second, name};
    return std::nullopt;
}

// Поиск глобальной переменной в IRProgram
static std::optional<int>
lookup_global_slot(const IRProgram& prog, const std::string& name) {
    for (int i = 0; i < static_cast<int>(prog.global_names.size()); ++i)
        if (prog.global_names[i] == name) return i;
    return std::nullopt;
}

static Operand lower_expr(const Expr& e, FnCtx& ctx, IRProgram& prog);
static void    lower_stmt(const Stmt& s, FnCtx& ctx, IRProgram& prog);

static bool is_float_type(TypePtr t) { return t && t->is_float(); }
static bool is_string_type(TypePtr t){ return t && t->is_string(); }
static bool is_bool_type(TypePtr t)  { return t && t->is_bool(); }

static Operand lower_expr(const Expr& e, FnCtx& ctx, IRProgram& prog) {
    switch (e.kind) {

    case Expr::Kind::IntLit: {
        auto& il = static_cast<const IntLitExpr&>(e);
        // Беззнаковый суффикс → ConstUInt
        if (il.suffix.size() && il.suffix[0]=='u')
            return ConstUInt{(uint64_t)il.value};
        return ConstInt{il.value};
    }
    case Expr::Kind::FloatLit:
        return ConstFloat{static_cast<const FloatLitExpr&>(e).value};
    case Expr::Kind::BoolLit:
        return ConstBool{static_cast<const BoolLitExpr&>(e).value};
    case Expr::Kind::StringLit:
        return ConstString{static_cast<const StringLitExpr&>(e).value};

    case Expr::Kind::Ident: {
        auto& id = static_cast<const IdentExpr&>(e);
        // локальная переменная?
        if (auto lop = lookup_local(ctx, id.name)) return *lop;
        // глобальная переменная?
        if (auto gs = lookup_global_slot(prog, id.name))
            return GlobalVar{*gs, id.name};
        throw std::runtime_error("undefined ident in lowering: " + id.name);
    }

    case Expr::Kind::NamespaceAccess: {
        auto& na = static_cast<const NamespaceAccessExpr&>(e);
        std::string full = na.ns_name + "::" + na.member;
        if (auto gs = lookup_global_slot(prog, full))
            return GlobalVar{*gs, full};
        throw std::runtime_error("undefined namespace var: " + full);
    }

    case Expr::Kind::BinOp: {
        auto& b = static_cast<const BinOpExpr&>(e);
        auto lhs = lower_expr(*b.lhs, ctx, prog);
        auto rhs = lower_expr(*b.rhs, ctx, prog);
        auto dst = ctx.new_temp();

        // Строковые операции
        if (b.op == BinOpKind::StrConcat) {
            ctx.emit(SBinInstr{dst, lhs, rhs});
            return dst;
        }
        if (b.op == BinOpKind::Eq || b.op == BinOpKind::NEq) {
            if (b.lhs->type && b.lhs->type->is_string()) {
                ctx.emit(SEqInstr{dst, lhs, rhs, b.op == BinOpKind::Eq});
                return dst;
            }
        }

        bool fp = is_float_type(b.lhs->type);
        bool lg = is_bool_type(b.lhs->type) ||
                  b.op == BinOpKind::And || b.op == BinOpKind::Or;

        if (lg) {
            LBinOp op = (b.op == BinOpKind::And) ? LBinOp::And : LBinOp::Or;
            ctx.emit(LBinInstr{dst, op, lhs, rhs});
        } else if (fp) {
            FBinOp op;
            switch (b.op) {
            case BinOpKind::Add: op=FBinOp::Add; break;
            case BinOpKind::Sub: op=FBinOp::Sub; break;
            case BinOpKind::Mul: op=FBinOp::Mul; break;
            case BinOpKind::Div: op=FBinOp::Div; break;
            case BinOpKind::Eq:  op=FBinOp::FEq;  break;
            case BinOpKind::NEq: op=FBinOp::FNeq; break;
            case BinOpKind::Lt:  op=FBinOp::FLt;  break;
            case BinOpKind::Le:  op=FBinOp::FLe;  break;
            case BinOpKind::Gt:  op=FBinOp::FGt;  break;
            case BinOpKind::Ge:  op=FBinOp::FGe;  break;
            default: throw std::runtime_error("unexpected float op");
            }
            ctx.emit(FBinInstr{dst, op, lhs, rhs});
        } else {
            IBinOp op;
            switch (b.op) {
            case BinOpKind::Add: op=IBinOp::Add; break;
            case BinOpKind::Sub: op=IBinOp::Sub; break;
            case BinOpKind::Mul: op=IBinOp::Mul; break;
            case BinOpKind::Div: op=IBinOp::Div; break;
            case BinOpKind::Mod: op=IBinOp::Mod; break;
            case BinOpKind::Eq:  op=IBinOp::IEq;  break;
            case BinOpKind::NEq: op=IBinOp::INeq; break;
            case BinOpKind::Lt:  op=IBinOp::ILt;  break;
            case BinOpKind::Le:  op=IBinOp::ILe;  break;
            case BinOpKind::Gt:  op=IBinOp::IGt;  break;
            case BinOpKind::Ge:  op=IBinOp::IGe;  break;
            default: throw std::runtime_error("unexpected int op");
            }
            ctx.emit(IBinInstr{dst, op, lhs, rhs});
        }
        return dst;
    }

    case Expr::Kind::UnaryOp: {
        auto& u = static_cast<const UnaryOpExpr&>(e);
        auto src = lower_expr(*u.operand, ctx, prog);
        auto dst = ctx.new_temp();
        if (u.op == UnaryOpKind::Neg) {
            if (is_float_type(u.operand->type))
                ctx.emit(FUnInstr{dst, FUnOp::Neg, src});
            else
                ctx.emit(IUnInstr{dst, IUnOp::Neg, src});
        } else {
            ctx.emit(LUnInstr{dst, LUnOp::Not, src});
        }
        return dst;
    }

    case Expr::Kind::Cast: {
        auto& c = static_cast<const CastExpr&>(e);
        auto src = lower_expr(*c.operand, ctx, prog);
        auto dst = ctx.new_temp();
        ctx.emit(Cast{dst, src, c.operand->type, c.target});
        return dst;
    }

    case Expr::Kind::Call: {
        auto& c = static_cast<const CallExpr&>(e);
        std::string fname;

        // Вызов метода на массиве или структуре 
        if (c.callee->kind == Expr::Kind::Field) {
            auto& fe = static_cast<const FieldExpr&>(*c.callee);
            auto obj = lower_expr(*fe.object, ctx, prog);
            // Методы строки
            if (fe.object->type && fe.object->type->is_string()) {
                if (fe.field_name == "len") {
                    auto dst = ctx.new_temp();
                    ctx.emit(StrLen{dst, obj});
                    return dst;
                }
            }
            // Сначала проверяем тип объекта
            bool obj_is_dynarray = fe.object->type &&
                (fe.object->type->is_dynarray() ||
                 (fe.object->type->is_array()));
            // Встроенные методы динамического массива
            if (obj_is_dynarray) {
                if (fe.field_name == "push") {
                    auto val = lower_expr(*c.args[0], ctx, prog);
                    ctx.emit(ArrayPush{obj, val});
                    return ConstUnit{};
                }
                if (fe.field_name == "pop") {
                    auto dst = ctx.new_temp();
                    ctx.emit(ArrayPop{dst, obj});
                    return dst;
                }
                if (fe.field_name == "len") {
                    auto dst = ctx.new_temp();
                    ctx.emit(ArrayLen{dst, obj});
                    return dst;
                }
                if (fe.field_name == "get") {
                    auto idx = lower_expr(*c.args[0], ctx, prog);
                    auto dst = ctx.new_temp();
                    ctx.emit(ArrayGet{dst, obj, idx});
                    return dst;
                }
            }
            // Вызов метода структуры: TypeName::method(self, ...)
            if (fe.object->type && fe.object->type->is_struct()) {
                std::string qualified = fe.object->type->struct_name + "::" + fe.field_name;
                std::vector<Operand> args;
                args.push_back(obj); 
                for (auto& a : c.args)
                    args.push_back(lower_expr(*a, ctx, prog));

} // namespace IR
