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

static std::optional<Operand> lower_builtin_call(
        const std::string& fname, const CallExpr& c, FnCtx& ctx, IRProgram& prog) {
    if (fname == "print") {
        auto arg = lower_expr(*c.args[0], ctx, prog);
        Operand end_op = ConstString{"\n"};
        bool has_end = false;
        for (auto& na : c.named_args) {
            if (na.name == "end") {
                end_op  = lower_expr(*na.value, ctx, prog);
                has_end = true;
            }
        }
        if (has_end)
            ctx.emit(PrintEnd{arg, c.args[0]->type, end_op});
        else
            ctx.emit(Print{arg, c.args[0]->type});
        return ConstUnit{};
    }

    auto emit_prompt = [&]() {
        if (!c.args.empty()) {
            auto prompt = lower_expr(*c.args[0], ctx, prog);
            ctx.emit(PrintEnd{prompt, TYPE_STRING, ConstString{""}});
        }
    };

    if (fname == "input") {
        emit_prompt();
        auto dst = ctx.new_temp();
        ctx.emit(Input{dst});
        return dst;
    }
    if (fname == "input_int") {
        emit_prompt();
        auto dst = ctx.new_temp();
        ctx.emit(InputInt{dst});
        return dst;
    }
    if (fname == "input_float") {
        emit_prompt();
        auto dst = ctx.new_temp();
        ctx.emit(InputFloat{dst});
        return dst;
    }
    if (fname == "to_int") {
        auto src = lower_expr(*c.args[0], ctx, prog);
        auto dst = ctx.new_temp();
        ctx.emit(ToInt{dst, src});
        return dst;
    }
    if (fname == "to_float") {
        auto src = lower_expr(*c.args[0], ctx, prog);
        auto dst = ctx.new_temp();
        ctx.emit(ToFloat{dst, src});
        return dst;
    }
    if (fname == "exit") {
        ctx.emit(Exit{lower_expr(*c.args[0], ctx, prog)});
        return ConstUnit{};
    }
    if (fname == "panic") {
        ctx.emit(Panic{lower_expr(*c.args[0], ctx, prog)});
        return ConstUnit{};
    }
    if (fname == "assert") {
        auto cond = lower_expr(*c.args[0], ctx, prog);
        auto ok_lbl = ctx.new_label("assert_ok");
        ctx.emit(JumpTrue{cond, ok_lbl});
        Operand msg = (c.args.size() > 1)
            ? lower_expr(*c.args[1], ctx, prog)
            : Operand{ConstString{"assertion failed"}};
        ctx.emit(Panic{msg});
        ctx.emit(Label{ok_lbl});
        return ConstUnit{};
    }
    return std::nullopt;
}

static bool is_float_type(TypePtr t) { return t && t->is_float(); }
static bool is_bool_type(TypePtr t)  { return t && t->is_bool(); }

static IBinOp binop_to_int(BinOpKind op) {
    switch (op) {
    case BinOpKind::Add: return IBinOp::Add;
    case BinOpKind::Sub: return IBinOp::Sub;
    case BinOpKind::Mul: return IBinOp::Mul;
    case BinOpKind::Div: return IBinOp::Div;
    case BinOpKind::Mod: return IBinOp::Mod;
    case BinOpKind::Eq:  return IBinOp::IEq;
    case BinOpKind::NEq: return IBinOp::INeq;
    case BinOpKind::Lt:  return IBinOp::ILt;
    case BinOpKind::Le:  return IBinOp::ILe;
    case BinOpKind::Gt:  return IBinOp::IGt;
    case BinOpKind::Ge:  return IBinOp::IGe;
    default: throw std::runtime_error("unexpected int op");
    }
}

static FBinOp binop_to_float(BinOpKind op) {
    switch (op) {
    case BinOpKind::Add: return FBinOp::Add;
    case BinOpKind::Sub: return FBinOp::Sub;
    case BinOpKind::Mul: return FBinOp::Mul;
    case BinOpKind::Div: return FBinOp::Div;
    case BinOpKind::Eq:  return FBinOp::FEq;
    case BinOpKind::NEq: return FBinOp::FNeq;
    case BinOpKind::Lt:  return FBinOp::FLt;
    case BinOpKind::Le:  return FBinOp::FLe;
    case BinOpKind::Gt:  return FBinOp::FGt;
    case BinOpKind::Ge:  return FBinOp::FGe;
    default: throw std::runtime_error("unexpected float op");
    }
}

static Operand lower_int_lit(const Expr& e) {
    auto& il = static_cast<const IntLitExpr&>(e);
    if (il.suffix.size() && il.suffix[0] == 'u')
        return ConstUInt{(uint64_t)il.value};
    return ConstInt{il.value};
}

static Operand lower_ident(const Expr& e, FnCtx& ctx, IRProgram& prog) {
    auto& id = static_cast<const IdentExpr&>(e);
    if (auto lop = lookup_local(ctx, id.name))      return *lop;
    if (auto gs  = lookup_global_slot(prog, id.name)) return GlobalVar{*gs, id.name};
    throw std::runtime_error("undefined ident in lowering: " + id.name);
}

static Operand lower_namespace_access(const Expr& e, IRProgram& prog) {
    auto& na = static_cast<const NamespaceAccessExpr&>(e);
    std::string full = na.ns_name + "::" + na.member;
    if (auto gs = lookup_global_slot(prog, full)) return GlobalVar{*gs, full};
    throw std::runtime_error("undefined namespace var: " + full);
}

static Operand lower_binop(const Expr& e, FnCtx& ctx, IRProgram& prog) {
    auto& b   = static_cast<const BinOpExpr&>(e);
    auto  lhs = lower_expr(*b.lhs, ctx, prog);
    auto  rhs = lower_expr(*b.rhs, ctx, prog);
    auto  dst = ctx.new_temp();

    if (b.op == BinOpKind::StrConcat) {
        ctx.emit(SBinInstr{dst, lhs, rhs});
        return dst;
    }
    if (b.lhs->type && b.lhs->type->is_string()) {
        if (b.op == BinOpKind::Eq || b.op == BinOpKind::NEq) {
            ctx.emit(SEqInstr{dst, lhs, rhs, b.op == BinOpKind::Eq});
        } else {
            SCmpOp sop = (b.op == BinOpKind::Lt) ? SCmpOp::Lt :
                         (b.op == BinOpKind::Le) ? SCmpOp::Le :
                         (b.op == BinOpKind::Gt) ? SCmpOp::Gt : SCmpOp::Ge;
            ctx.emit(SCmpInstr{dst, sop, lhs, rhs});
        }
        return dst;
    }

    bool fp = is_float_type(b.lhs->type);
    bool lg = is_bool_type(b.lhs->type) || b.op == BinOpKind::And || b.op == BinOpKind::Or;

    if (lg) {
        ctx.emit(LBinInstr{dst, (b.op == BinOpKind::And) ? LBinOp::And : LBinOp::Or, lhs, rhs});
    } else if (fp) {
        ctx.emit(FBinInstr{dst, binop_to_float(b.op), lhs, rhs});
    } else {
        uint8_t bits = 64; bool is_u = false;
        if (b.lhs->type) {
            int b_bits = b.lhs->type->int_bits();
            if (b_bits > 0) bits = (uint8_t)b_bits;
            is_u = b.lhs->type->is_unsigned_int();
        }
        ctx.emit(IBinInstr{dst, binop_to_int(b.op), lhs, rhs, bits, is_u});
    }
    return dst;
}

static Operand lower_unary(const Expr& e, FnCtx& ctx, IRProgram& prog) {
    auto& u   = static_cast<const UnaryOpExpr&>(e);
    auto  src = lower_expr(*u.operand, ctx, prog);
    auto  dst = ctx.new_temp();
    if (u.op == UnaryOpKind::Neg) {
        if (is_float_type(u.operand->type)) {
            ctx.emit(FUnInstr{dst, FUnOp::Neg, src});
        } else {
            uint8_t bits = 64; bool is_u = false;
            if (u.operand->type) {
                int b = u.operand->type->int_bits();
                if (b > 0) bits = (uint8_t)b;
                is_u = u.operand->type->is_unsigned_int();
            }
            ctx.emit(IUnInstr{dst, IUnOp::Neg, src, bits, is_u});
        }
    } else {
        ctx.emit(LUnInstr{dst, LUnOp::Not, src});
    }
    return dst;
}

static Operand lower_cast(const Expr& e, FnCtx& ctx, IRProgram& prog) {
    auto& c   = static_cast<const CastExpr&>(e);
    auto  src = lower_expr(*c.operand, ctx, prog);
    auto  dst = ctx.new_temp();
    ctx.emit(Cast{dst, src, c.operand->type, c.target});
    return dst;
}

static Operand emit_call_result(const Expr& e, const std::string& fname,
                                std::vector<Operand> args, FnCtx& ctx,
                                bool is_method = false) {
    bool is_void = e.type && e.type->is_void();
    if (is_void) {
        ctx.emit(Call{std::nullopt, fname, std::move(args), is_method});
        return ConstUnit{};
    }
    auto dst = ctx.new_temp();
    ctx.emit(Call{dst, fname, std::move(args), is_method});
    return dst;
}

static std::optional<Operand> lower_method_call(const Expr& e, const CallExpr& c,
                                                 FnCtx& ctx, IRProgram& prog) {
    if (c.callee->kind != Expr::Kind::Field) return std::nullopt;

    auto& fe  = static_cast<const FieldExpr&>(*c.callee);
    auto  obj = lower_expr(*fe.object, ctx, prog);

    if (fe.object->type && fe.object->type->is_string() && fe.field_name == "len") {
        auto dst = ctx.new_temp();
        ctx.emit(StrLen{dst, obj});
        return dst;
    }

    bool is_array = fe.object->type &&
        (fe.object->type->is_dynarray() || fe.object->type->is_array());
    if (is_array) {
        if (fe.field_name == "push") {
            ctx.emit(ArrayPush{obj, lower_expr(*c.args[0], ctx, prog)});
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
            auto dst = ctx.new_temp();
            ctx.emit(ArrayGet{dst, obj, lower_expr(*c.args[0], ctx, prog)});
            return dst;
        }
    }

    if (fe.object->type && fe.object->type->is_struct()) {
        std::string qualified = sname(*fe.object->type) + "::" + fe.field_name;
        std::vector<Operand> args;
        args.push_back(obj);
        for (auto& a : c.args)
            args.push_back(lower_expr(*a, ctx, prog));
        return emit_call_result(e, qualified, std::move(args), ctx, /*is_method=*/true);
    }

    return std::nullopt;
}

static Operand lower_call(const Expr& e, FnCtx& ctx, IRProgram& prog) {
    auto& c = static_cast<const CallExpr&>(e);

    if (auto res = lower_method_call(e, c, ctx, prog))
        return *res;

    std::string fname;
    switch (c.callee->kind) {
    case Expr::Kind::Ident:
        fname = static_cast<const IdentExpr&>(*c.callee).name;
        break;
    case Expr::Kind::NamespaceAccess: {
        auto& na = static_cast<const NamespaceAccessExpr&>(*c.callee);
        fname = na.ns_name + "::" + na.member;
        break;
    }
    default: break;
    }

    if (auto res = lower_builtin_call(fname, c, ctx, prog))
        return *res;

    std::vector<Operand> args;
    for (auto& a : c.args)
        args.push_back(lower_expr(*a, ctx, prog));
    return emit_call_result(e, fname, std::move(args), ctx);
}

static Operand lower_array_lit(const Expr& e, FnCtx& ctx, IRProgram& prog) {
    auto& al    = static_cast<const ArrayLitExpr&>(e);
    bool  is_dyn = e.type && e.type->is_dynarray();
    TypePtr elem = (e.type && (e.type->is_array() || e.type->is_dynarray()))
                   ? type_elem(*e.type) : TYPE_INT32;
    auto dst = ctx.new_temp();
    if (is_dyn) {
        ctx.emit(NewDynArray{dst, elem});
        for (auto& el : al.elements) {
            ctx.emit(ArrayPush{dst, lower_expr(*el, ctx, prog)});
        }
    } else {
        ctx.emit(NewArray{dst, (int64_t)al.elements.size(), elem});
        for (int i = 0; i < static_cast<int>(al.elements.size()); ++i)
            ctx.emit(ArraySet{dst, ConstInt{i}, lower_expr(*al.elements[i], ctx, prog)});
    }
    return dst;
}

static Operand lower_struct_lit(const Expr& e, FnCtx& ctx, IRProgram& prog) {
    auto& sl     = static_cast<const StructLitExpr&>(e);
    auto  dst    = ctx.new_temp();
    std::string st_name = (e.type && e.type->is_struct()) ? sname(*e.type) : sl.type_name;
    ctx.emit(NewStruct{dst, st_name, static_cast<int>(sl.fields.size())});
    for (int i = 0; i < static_cast<int>(sl.fields.size()); ++i)
        ctx.emit(FieldSet{dst, i, lower_expr(*sl.fields[i].value, ctx, prog)});
    return dst;
}

static Operand lower_tuple_lit(const Expr& e, FnCtx& ctx, IRProgram& prog) {
    auto& tl  = static_cast<const TupleLitExpr&>(e);
    auto  dst = ctx.new_temp();
    ctx.emit(NewStruct{dst, "__tuple__", static_cast<int>(tl.elements.size())});
    for (int i = 0; i < static_cast<int>(tl.elements.size()); ++i)
        ctx.emit(FieldSet{dst, i, lower_expr(*tl.elements[i], ctx, prog)});
    return dst;
}

static Operand lower_expr(const Expr& e, FnCtx& ctx, IRProgram& prog) {
    switch (e.kind) {
    case Expr::Kind::IntLit:          return lower_int_lit(e);
    case Expr::Kind::FloatLit:        return ConstFloat{static_cast<const FloatLitExpr&>(e).value};
    case Expr::Kind::BoolLit:         return ConstBool{static_cast<const BoolLitExpr&>(e).value};
    case Expr::Kind::CharLit:         return ConstChar{static_cast<const CharLitExpr&>(e).value};
    case Expr::Kind::StringLit:       return ConstString{static_cast<const StringLitExpr&>(e).value};
    case Expr::Kind::NullLit:         return ConstUnit{};
    case Expr::Kind::Ident:           return lower_ident(e, ctx, prog);
    case Expr::Kind::NamespaceAccess: return lower_namespace_access(e, prog);
    case Expr::Kind::BinOp:           return lower_binop(e, ctx, prog);
    case Expr::Kind::UnaryOp:         return lower_unary(e, ctx, prog);
    case Expr::Kind::Cast:            return lower_cast(e, ctx, prog);
    case Expr::Kind::Call:            return lower_call(e, ctx, prog);
    case Expr::Kind::Index: {
        auto& ix = static_cast<const IndexExpr&>(e);
        auto  dst = ctx.new_temp();
        ctx.emit(ArrayGet{dst, lower_expr(*ix.arr, ctx, prog), lower_expr(*ix.idx, ctx, prog)});
        return dst;
    }
    case Expr::Kind::Field: {
        auto& fe  = static_cast<const FieldExpr&>(e);
        auto  dst = ctx.new_temp();
        ctx.emit(FieldGet{dst, lower_expr(*fe.object, ctx, prog), fe.field_idx});
        return dst;
    }
    case Expr::Kind::TupleLit:        return lower_tuple_lit(e, ctx, prog);
    case Expr::Kind::TupleIndex: {
        auto& ti  = static_cast<const TupleIndexExpr&>(e);
        auto  dst = ctx.new_temp();
        ctx.emit(FieldGet{dst, lower_expr(*ti.object, ctx, prog), ti.index});
        return dst;
    }
    case Expr::Kind::ArrayLit:        return lower_array_lit(e, ctx, prog);
    case Expr::Kind::StructLit:       return lower_struct_lit(e, ctx, prog);
    default:
        throw std::runtime_error("unhandled expr kind in lowering");
    }
}


static Operand resolve_lvalue_base(const LValue& lv, FnCtx& ctx, IRProgram& prog);

static Operand resolve_ident_lvalue(const IdentLValue& il, FnCtx& ctx, IRProgram& prog) {
    if (auto lop = lookup_local(ctx, il.name))        return *lop;
    if (auto gs  = lookup_global_slot(prog, il.name)) return GlobalVar{*gs, il.name};
    throw std::runtime_error("undefined: " + il.name);
}

static Operand resolve_lvalue_base(const LValue& lv, FnCtx& ctx, IRProgram& prog) {
    if (lv.kind == LValue::Kind::Ident)
        return resolve_ident_lvalue(static_cast<const IdentLValue&>(lv), ctx, prog);
    if (lv.kind == LValue::Kind::Index) {
        auto& xl  = static_cast<const IndexLValue&>(lv);
        auto base = resolve_lvalue_base(*xl.base, ctx, prog);
        auto idx  = lower_expr(*xl.idx, ctx, prog);
        auto tmp  = ctx.new_temp();
        ctx.emit(ArrayGet{tmp, base, idx});
        return tmp;
    }
    auto& fl  = static_cast<const FieldLValue&>(lv);
    auto base = resolve_lvalue_base(*fl.base, ctx, prog);
    auto tmp  = ctx.new_temp();
    ctx.emit(FieldGet{tmp, base, fl.field_idx});
    return tmp;
}

static void lower_assign(const LValue& lv, Operand val, FnCtx& ctx, IRProgram& prog) {
    switch (lv.kind) {
    case LValue::Kind::Ident: {
        auto& il = static_cast<const IdentLValue&>(lv);
        if (auto lop = lookup_local(ctx, il.name))
            ctx.emit(Copy{*lop, val});
        else if (auto gs = lookup_global_slot(prog, il.name))
            ctx.emit(Copy{GlobalVar{*gs, il.name}, val});
        else
            throw std::runtime_error("undefined lvalue: " + il.name);
        break;
    }
    case LValue::Kind::Index: {
        auto& xl    = static_cast<const IndexLValue&>(lv);
        auto arr_op = resolve_lvalue_base(*xl.base, ctx, prog);
        auto idx_op = lower_expr(*xl.idx, ctx, prog);
        ctx.emit(ArraySet{arr_op, idx_op, val});
        break;
    }
    case LValue::Kind::Field: {
        auto& fl    = static_cast<const FieldLValue&>(lv);
        auto obj_op = resolve_lvalue_base(*fl.base, ctx, prog);
        ctx.emit(FieldSet{obj_op, fl.field_idx, val});
        break;
    }
    }
}

static void lower_vardecl(const Stmt& s, FnCtx& ctx, IRProgram& prog) {
    auto& v = static_cast<const VarDeclStmt&>(s);
    // аннотация DynArray + инициализатор фиксированный массив → генерируем NewDynArray+push
    if (v.ann_type && v.ann_type->is_dynarray() &&
        v.init->type && v.init->type->is_array())
        const_cast<Expr*>(v.init.get())->type = v.ann_type;
    auto val  = lower_expr(*v.init, ctx, prog);
    int  slot = ctx.fn.num_locals++;
    ctx.locals[v.name] = slot;
    ctx.fn.slot_names.resize(ctx.fn.num_locals, "");
    ctx.fn.slot_names[slot] = v.name;
    ctx.emit(Copy{LocalVar{slot, v.name}, val});
}

static void lower_if(const Stmt& s, FnCtx& ctx, IRProgram& prog) {
    auto& i          = static_cast<const IfStmt&>(s);
    auto  cond       = lower_expr(*i.cond, ctx, prog);
    auto  else_label = ctx.new_label("else");
    auto  end_label  = ctx.new_label("endif");
    ctx.emit(JumpFalse{cond, else_label});
    lower_stmt(*i.then_branch, ctx, prog);
    if (i.else_branch) ctx.emit(Jump{end_label});
    ctx.emit(Label{else_label});
    if (i.else_branch) {
        lower_stmt(*i.else_branch, ctx, prog);
        ctx.emit(Label{end_label});
    }
}

static void lower_while(const Stmt& s, FnCtx& ctx, IRProgram& prog) {
    auto& w     = static_cast<const WhileStmt&>(s);
    auto  start = ctx.new_label("while_start");
    auto  end   = ctx.new_label("while_end");
    ctx.emit(Label{start});
    ctx.emit(JumpFalse{lower_expr(*w.cond, ctx, prog), end});
    ctx.loops.push_back({start, end});
    lower_stmt(*w.body, ctx, prog);
    ctx.loops.pop_back();
    ctx.emit(Jump{start});
    ctx.emit(Label{end});
}

static void lower_for_range(const Stmt& s, FnCtx& ctx, IRProgram& prog) {
    auto& fr   = static_cast<const ForRangeStmt&>(s);
    int   slot = ctx.fn.num_locals++;
    ctx.locals[fr.var_name] = slot;
    ctx.fn.slot_names.resize(ctx.fn.num_locals, "");
    ctx.fn.slot_names[slot] = fr.var_name;
    ctx.emit(Copy{LocalVar{slot, fr.var_name}, lower_expr(*fr.start, ctx, prog)});

    auto lstart = ctx.new_label("for_start");
    auto lend   = ctx.new_label("for_end");
    ctx.emit(Label{lstart});
    auto i_op = LocalVar{slot, fr.var_name};
    auto cond = ctx.new_temp();
    ctx.emit(IBinInstr{cond, IBinOp::ILt, i_op, lower_expr(*fr.end, ctx, prog)});
    ctx.emit(JumpFalse{cond, lend});
    ctx.loops.push_back({lstart, lend});
    lower_stmt(*fr.body, ctx, prog);
    ctx.loops.pop_back();
    auto next = ctx.new_temp();
    ctx.emit(IBinInstr{next, IBinOp::Add, i_op, ConstInt{1}});
    ctx.emit(Copy{i_op, next});
    ctx.emit(Jump{lstart});
    ctx.emit(Label{lend});
}

static void lower_for_c(const Stmt& s, FnCtx& ctx, IRProgram& prog) {
    auto& fc     = static_cast<const ForCStmt&>(s);
    auto  lstart = ctx.new_label("forc_start");
    auto  lend   = ctx.new_label("forc_end");
    lower_stmt(*fc.init, ctx, prog);
    ctx.emit(Label{lstart});
    ctx.emit(JumpFalse{lower_expr(*fc.cond, ctx, prog), lend});
    ctx.loops.push_back({lstart, lend});
    lower_stmt(*fc.body, ctx, prog);
    ctx.loops.pop_back();
    lower_stmt(*fc.step, ctx, prog);
    ctx.emit(Jump{lstart});
    ctx.emit(Label{lend});
}

static void lower_stmt(const Stmt& s, FnCtx& ctx, IRProgram& prog) {
    if (s.loc.line > 0 && s.kind != Stmt::Kind::Empty && s.kind != Stmt::Kind::Block)
        ctx.emit(IR::LineInfo{s.loc.line});
    switch (s.kind) {
    case Stmt::Kind::Empty:    break;
    case Stmt::Kind::VarDecl:  lower_vardecl(s, ctx, prog); break;
    case Stmt::Kind::Assign: {
        auto& a = static_cast<const AssignStmt&>(s);
        lower_assign(*a.target, lower_expr(*a.value, ctx, prog), ctx, prog);
        break;
    }
    case Stmt::Kind::ExprStmt:
        lower_expr(*static_cast<const ExprStmt&>(s).expr, ctx, prog);
        break;
    case Stmt::Kind::If:       lower_if(s, ctx, prog);       break;
    case Stmt::Kind::While:    lower_while(s, ctx, prog);    break;
    case Stmt::Kind::ForRange: lower_for_range(s, ctx, prog); break;
    case Stmt::Kind::ForC:     lower_for_c(s, ctx, prog);    break;
    case Stmt::Kind::Block:
        for (auto& st : static_cast<const BlockStmt&>(s).stmts)
            lower_stmt(*st, ctx, prog);
        break;
    case Stmt::Kind::Break:
        if (!ctx.loops.empty()) ctx.emit(Jump{ctx.loops.back().break_label});
        break;
    case Stmt::Kind::Continue:
        if (!ctx.loops.empty()) ctx.emit(Jump{ctx.loops.back().cont_label});
        break;
    case Stmt::Kind::Return: {
        auto& r = static_cast<const ReturnStmt&>(s);
        if (r.value) ctx.emit(ReturnVal{lower_expr(*r.value, ctx, prog)});
        else         ctx.emit(Return{});
        break;
    }
    }
}

static IRFunction lower_function(FunDecl& fd, IRProgram& prog) {
    IRFunction fn;
    fn.name       = fd.name;
    fn.num_params = static_cast<int>(fd.params.size());
    fn.num_locals = static_cast<int>(fd.params.size()); 
    fn.ret_type   = fd.ret_type;

    int next_temp = 0;
    FnCtx ctx{fn, next_temp};

    // Регистрируем слоты параметров
    for (int i = 0; i < static_cast<int>(fd.params.size()); ++i) {
        ctx.locals[fd.params[i].name] = i;
        fn.slot_names.push_back(fd.params[i].name);
    }

    // Понижаем тело функции
    for (auto& st : fd.body->stmts)
        lower_stmt(*st, ctx, prog);

    // void-функции должны заканчиваться Return
    if (fn.body.empty() || !std::holds_alternative<ReturnVal>(fn.body.back()) &&
                           !std::holds_alternative<Return>(fn.body.back()))
        fn.body.push_back(Return{});

    return fn;
}

IRProgram lower(Program& /*prog*/,
                const Semantic::AnalysisResult& sem,
                const std::string& /*filename*/) {
    IRProgram irp;

    // Регистрируем глобальные переменные
    for (auto* gv : sem.globals) {
        irp.global_names.push_back(gv->name);
        irp.global_types.push_back(gv->ann_type);
        irp.global_inits.push_back(ConstUnit{});
    }

    // Понижаем каждую функцию
    for (auto* fd : sem.functions) {
        IRFunction irf = lower_function(*fd, irp);
        if (fd->name == "main") irp.main_idx = static_cast<int>(irp.functions.size());
        irp.functions.push_back(std::move(irf));
    }

    // Простой константный инициализатор?
    auto is_const_init = [](const Expr& e) {
        return e.kind == Expr::Kind::IntLit   ||
               e.kind == Expr::Kind::FloatLit ||
               e.kind == Expr::Kind::BoolLit  ||
               e.kind == Expr::Kind::StringLit;
    };

    // Константные инициализаторы → пул констант
    for (int i = 0; i < static_cast<int>(sem.globals.size()); ++i) {
        auto& gv = *sem.globals[i];
        switch (gv.init->kind) {
        case Expr::Kind::IntLit:
            irp.global_inits[i] = ConstInt{static_cast<IntLitExpr&>(*gv.init).value};    break;
        case Expr::Kind::FloatLit:
            irp.global_inits[i] = ConstFloat{static_cast<FloatLitExpr&>(*gv.init).value}; break;
        case Expr::Kind::BoolLit:
            irp.global_inits[i] = ConstBool{static_cast<BoolLitExpr&>(*gv.init).value};   break;
        case Expr::Kind::StringLit:
            irp.global_inits[i] = ConstString{static_cast<StringLitExpr&>(*gv.init).value}; break;
        default: break;
        }
    }

    // Нетривиальные инициализаторы (вызовы функций):
    // генерируем __init_globals__ и вызываем в начале main
    bool has_complex = false;
    for (auto* gv : sem.globals)
        if (!is_const_init(*gv->init)) { has_complex = true; break; }

    if (has_complex) {
        IRFunction init_fn;
        init_fn.name       = "__init_globals__";
        init_fn.num_params = 0;
        init_fn.num_locals = 0;
        init_fn.ret_type   = TYPE_VOID;

        int next_temp = 0;
        FnCtx init_ctx{init_fn, next_temp};

        for (int i = 0; i < static_cast<int>(sem.globals.size()); ++i) {
            auto& gv = *sem.globals[i];
            if (is_const_init(*gv.init)) continue; // константы уже обработаны
            auto val = lower_expr(*gv.init, init_ctx, irp);
            init_ctx.emit(Copy{GlobalVar{i, gv.name}, val});
        }
        init_ctx.emit(Return{});
        irp.functions.push_back(std::move(init_fn));

        // Вставляем CALL __init_globals__ в начало main
        if (irp.main_idx >= 0) {
            auto& main_fn = irp.functions[irp.main_idx];
            std::vector<Instr> prefix;
            prefix.push_back(Call{std::nullopt, "__init_globals__", {}});
            prefix.insert(prefix.end(), main_fn.body.begin(), main_fn.body.end());
            main_fn.body = std::move(prefix);
        }
    }

    return irp;
}

// IR печать
std::string operand_to_str(const Operand& o) {
    if (auto* v = std::get_if<TempVar>(&o))    return "t" + std::to_string(v->id);
    if (auto* v = std::get_if<LocalVar>(&o))   return v->name.empty() ? ("loc" + std::to_string(v->slot)) : v->name;
    if (auto* v = std::get_if<GlobalVar>(&o))  return "@" + v->name;
    if (auto* v = std::get_if<ConstInt>(&o))   return std::to_string(v->v);
    if (auto* v = std::get_if<ConstUInt>(&o))  return std::to_string(v->v) + "u";
    if (auto* v = std::get_if<ConstFloat>(&o)) return std::to_string(v->v);
    if (auto* v = std::get_if<ConstBool>(&o))  return v->v ? "true" : "false";
    if (auto* v = std::get_if<ConstChar>(&o))  return std::string("'") + v->v + "'";
    if (auto* v = std::get_if<ConstString>(&o)) return "\"" + v->v + "\"";
    return "unit";
}

static const char* ibin_str(IBinOp o) {
    switch(o){case IBinOp::Add:return"+";case IBinOp::Sub:return"-";
    case IBinOp::Mul:return"*";case IBinOp::Div:return"/";case IBinOp::Mod:return"%";
    case IBinOp::IEq:return"==";case IBinOp::INeq:return"!=";
    case IBinOp::ILt:return"<";case IBinOp::ILe:return"<=";
    case IBinOp::IGt:return">";case IBinOp::IGe:return">=";
    default:return"?";}
}
static const char* fbin_str(FBinOp o) {
    switch(o){case FBinOp::Add:return"f+";case FBinOp::Sub:return"f-";
    case FBinOp::Mul:return"f*";case FBinOp::Div:return"f/";
    case FBinOp::FEq:return"f==";case FBinOp::FNeq:return"f!=";
    case FBinOp::FLt:return"f<";case FBinOp::FLe:return"f<=";
    case FBinOp::FGt:return"f>";case FBinOp::FGe:return"f>=";
    default:return"?";}
}

static void dump_instr(const Instr& instr, std::ostream& out) {
    auto o = [](const Operand& op){ return operand_to_str(op); };
    if (auto* v = std::get_if<Label>(&instr))     { out << v->name << ":\n"; return; }
    if (auto* v = std::get_if<IBinInstr>(&instr)) { out<<o(v->dst)<<" = "<<o(v->lhs)<<" "<<ibin_str(v->op)<<" "<<o(v->rhs); }
    else if (auto* v = std::get_if<FBinInstr>(&instr)) { out<<o(v->dst)<<" = "<<o(v->lhs)<<" "<<fbin_str(v->op)<<" "<<o(v->rhs); }
    else if (auto* v = std::get_if<LBinInstr>(&instr)) { out<<o(v->dst)<<" = "<<o(v->lhs)<<(v->op==LBinOp::And?" and ":" or ")<<o(v->rhs); }
    else if (auto* v = std::get_if<IUnInstr>(&instr))  { out<<o(v->dst)<<" = -"<<o(v->src); }
    else if (auto* v = std::get_if<FUnInstr>(&instr))  { out<<o(v->dst)<<" = f-"<<o(v->src); }
    else if (auto* v = std::get_if<LUnInstr>(&instr))  { out<<o(v->dst)<<" = not "<<o(v->src); }
    else if (auto* v = std::get_if<Copy>(&instr))      { out<<o(v->dst)<<" = "<<o(v->src); }
    else if (auto* v = std::get_if<SBinInstr>(&instr)) { out<<o(v->dst)<<" = "<<o(v->lhs)<<"++str"<<o(v->rhs); }
    else if (auto* v = std::get_if<StrLen>(&instr))    { out<<o(v->dst)<<" = strlen("<<o(v->src)<<")"; }
    else if (auto* v = std::get_if<ArrayLen>(&instr))  { out<<o(v->dst)<<" = arrlen("<<o(v->src)<<")"; }
    else if (auto* v = std::get_if<SEqInstr>(&instr))  { out<<o(v->dst)<<" = "<<o(v->lhs)<<(v->eq?" seq ":" sneq ")<<o(v->rhs); }
    else if (auto* v = std::get_if<NewArray>(&instr))  { out<<o(v->dst)<<" = new_array["<<v->size<<"]"; }
    else if (auto* v = std::get_if<ArrayGet>(&instr))  { out<<o(v->dst)<<" = "<<o(v->arr)<<"["<<o(v->idx)<<"]"; }
    else if (auto* v = std::get_if<ArraySet>(&instr))  { out<<o(v->arr)<<"["<<o(v->idx)<<"] = "<<o(v->val); }
    else if (auto* v = std::get_if<NewStruct>(&instr)) { out<<o(v->dst)<<" = new_struct "<<v->type_name; }
    else if (auto* v = std::get_if<FieldGet>(&instr))  { out<<o(v->dst)<<" = "<<o(v->obj)<<".f"<<v->field_idx; }
    else if (auto* v = std::get_if<FieldSet>(&instr))  { out<<o(v->obj)<<".f"<<v->field_idx<<" = "<<o(v->val); }
    else if (auto* v = std::get_if<Call>(&instr)) {
        if (v->dst) out<<o(*v->dst)<<" = ";
        out<<"call "<<v->fname<<"(";
        for (size_t i=0;i<v->args.size();++i){if(i)out<<",";out<<o(v->args[i]);}
        out<<")";
    }
    else if (auto* v = std::get_if<Cast>(&instr))      { out<<o(v->dst)<<" = "<<o(v->src)<<" as "<<(v->to_type?v->to_type->to_string():"?"); }
    else if (auto* v = std::get_if<Jump>(&instr))      { out<<"goto "<<v->label; }
    else if (auto* v = std::get_if<JumpFalse>(&instr)) { out<<"if_false "<<o(v->cond)<<" goto "<<v->label; }
    else if (auto* v = std::get_if<JumpTrue>(&instr))  { out<<"if_true "<<o(v->cond)<<" goto "<<v->label; }
    else if (std::get_if<Return>(&instr))               { out<<"return"; }
    else if (auto* v = std::get_if<ReturnVal>(&instr))  { out<<"return "<<o(v->val); }
    else if (auto* v = std::get_if<Print>(&instr))      { out<<"print("<<o(v->val)<<")"; }
    else if (auto* v = std::get_if<Input>(&instr))      { out<<o(v->dst)<<" = input()"; }
    else if (auto* v = std::get_if<Exit>(&instr))       { out<<"exit("<<o(v->code)<<")"; }
    else if (auto* v = std::get_if<Panic>(&instr))      { out<<"panic("<<o(v->msg)<<")"; }
    out << "\n";
}

void dump_ir(const IRProgram& prog, std::ostream& out) {
    for (int i=0;i<static_cast<int>(prog.global_names.size());++i)
        out << "global @" << prog.global_names[i] << " = "
            << operand_to_str(prog.global_inits[i]) << "\n";
    out << "\n";

    for (auto& fn : prog.functions) {
        out << "fun " << fn.name << " (" << fn.num_params << " params, "
            << fn.num_locals << " locals):\n";
        for (auto& instr : fn.body) {
            out << "  ";
            dump_instr(instr, out);
        }
        out << "\n";
    }
}

} 
