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
                bool is_void = e.type && e.type->is_void();
                if (is_void) {
                    ctx.emit(Call{std::nullopt, qualified, std::move(args)});
                    return ConstUnit{};
                } else {
                    auto dst = ctx.new_temp();
                    ctx.emit(Call{dst, qualified, std::move(args)});
                    return dst;
                }
            }
        }

        if (c.callee->kind == Expr::Kind::Ident)
            fname = static_cast<const IdentExpr&>(*c.callee).name;
        else if (c.callee->kind == Expr::Kind::NamespaceAccess) {
            auto& na = static_cast<const NamespaceAccessExpr&>(*c.callee);
            fname = na.ns_name + "::" + na.member;
        }

        // Встроенные функции
        if (fname == "print") {
            auto arg = lower_expr(*c.args[0], ctx, prog);
            // Ищем именованный аргумент end=
            Operand end_op = ConstString{"\n"}; // по умолчанию перевод строки
            bool has_end = false;
            for (auto& na : c.named_args) {
                if (na.name == "end") {
                    end_op  = lower_expr(*na.value, ctx, prog);
                    has_end = true;
                }
            }
            if (has_end) {
                ctx.emit(PrintEnd{arg, c.args[0]->type, end_op});
            } else {
                ctx.emit(Print{arg, c.args[0]->type});
            }
            return ConstUnit{};
        }
        // Если есть подсказка — печатаем без \n
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
            auto code = lower_expr(*c.args[0], ctx, prog);
            ctx.emit(Exit{code});
            return ConstUnit{};
        }
        if (fname == "panic") {
            auto msg = lower_expr(*c.args[0], ctx, prog);
            ctx.emit(Panic{msg});
            return ConstUnit{};
        }

        std::vector<Operand> args;
        for (auto& a : c.args)
            args.push_back(lower_expr(*a, ctx, prog));

        bool is_void = e.type && e.type->is_void();
        if (is_void) {
            ctx.emit(Call{std::nullopt, fname, args});
            return ConstUnit{};
        } else {
            auto dst = ctx.new_temp();
            ctx.emit(Call{dst, fname, args});
            return dst;
        }
    }

    case Expr::Kind::Index: {
        auto& ix = static_cast<const IndexExpr&>(e);
        auto arr = lower_expr(*ix.arr, ctx, prog);
        auto idx = lower_expr(*ix.idx, ctx, prog);
        auto dst = ctx.new_temp();
        ctx.emit(ArrayGet{dst, arr, idx});
        return dst;
    }

    case Expr::Kind::Field: {
        auto& fe = static_cast<const FieldExpr&>(e);
        auto obj = lower_expr(*fe.object, ctx, prog);
        auto dst = ctx.new_temp();
        ctx.emit(FieldGet{dst, obj, fe.field_idx});
        return dst;
    }

    case Expr::Kind::NullLit:
        return IR::ConstUnit{};

    case Expr::Kind::TupleLit: {
        auto& tl = static_cast<const TupleLitExpr&>(e);
        auto dst = ctx.new_temp();
        ctx.emit(NewStruct{dst, "__tuple__", static_cast<int>(tl.elements.size())});
        for (int i = 0; i < static_cast<int>(tl.elements.size()); ++i) {
            auto val = lower_expr(*tl.elements[i], ctx, prog);
            ctx.emit(FieldSet{dst, i, val});
        }
        return dst;
    }

    case Expr::Kind::TupleIndex: {
        auto& ti = static_cast<const TupleIndexExpr&>(e);
        auto obj = lower_expr(*ti.object, ctx, prog);
        auto dst = ctx.new_temp();
        ctx.emit(FieldGet{dst, obj, ti.index});
        return dst;
    }

    case Expr::Kind::ArrayLit: {
        auto& al = static_cast<const ArrayLitExpr&>(e);
        bool is_dyn = e.type && e.type->is_dynarray();
        TypePtr elem = (e.type && (e.type->is_array()||e.type->is_dynarray()))
                       ? e.type->elem_type : TYPE_INT32;
        auto dst = ctx.new_temp();
        if (is_dyn) {
            ctx.emit(NewDynArray{dst, elem});
            for (auto& el : al.elements) {
                auto val = lower_expr(*el, ctx, prog);
                ctx.emit(ArrayPush{dst, val});
            }
        } else {
            ctx.emit(NewArray{dst, (int64_t)al.elements.size(), elem});
            for (int i = 0; i < static_cast<int>(al.elements.size()); ++i) {
                auto val = lower_expr(*al.elements[i], ctx, prog);
                ctx.emit(ArraySet{dst, ConstInt{i}, val});
            }
        }
        return dst;
    }

    case Expr::Kind::StructLit: {
        auto& sl = static_cast<const StructLitExpr&>(e);
        auto dst = ctx.new_temp();
        ctx.emit(NewStruct{dst, sl.type_name, static_cast<int>(sl.fields.size())});
        for (int i = 0; i < static_cast<int>(sl.fields.size()); ++i) {
            auto val = lower_expr(*sl.fields[i].value, ctx, prog);
            ctx.emit(FieldSet{dst, i, val});
        }
        return dst;
    }

    default:
        throw std::runtime_error("unhandled expr kind in lowering");
    }
}


static void lower_assign(const LValue& lv,
                          Operand val,
                          FnCtx& ctx,
                          IRProgram& prog) {
    switch (lv.kind) {
    case LValue::Kind::Ident: {
        if (auto lop = lookup_local(ctx, lv.name)) {
            ctx.emit(Copy{*lop, val});
        } else if (auto gs = lookup_global_slot(prog, lv.name)) {
            ctx.emit(Copy{GlobalVar{*gs, lv.name}, val});
        } else {
            throw std::runtime_error("undefined lvalue: " + lv.name);
        }
        break;
    }
    case LValue::Kind::Index: {
        std::function<Operand(const LValue&)> get_base = [&](const LValue& base_lv) -> Operand {
            if (base_lv.kind == LValue::Kind::Ident) {
                if (auto lop = lookup_local(ctx, base_lv.name)) return *lop;
                if (auto gs = lookup_global_slot(prog, base_lv.name))
                    return GlobalVar{*gs, base_lv.name};
                throw std::runtime_error("undefined: " + base_lv.name);
            }
            auto arr_op = get_base(*base_lv.base);
            auto idx_op = lower_expr(*base_lv.idx, ctx, prog);
            auto tmp = ctx.new_temp();
            ctx.emit(ArrayGet{tmp, arr_op, idx_op});
            return tmp;
        };
        auto arr_op = get_base(*lv.base);
        auto idx_op = lower_expr(*lv.idx, ctx, prog);
        ctx.emit(ArraySet{arr_op, idx_op, val});
        break;
    }
    case LValue::Kind::Field: {
        std::function<Operand(const LValue&)> get_struct = [&](const LValue& base_lv) -> Operand {
            if (base_lv.kind == LValue::Kind::Ident) {
                if (auto lop = lookup_local(ctx, base_lv.name)) return *lop;
                if (auto gs = lookup_global_slot(prog, base_lv.name))
                    return GlobalVar{*gs, base_lv.name};
                throw std::runtime_error("undefined: " + base_lv.name);
            }
            auto obj_op = get_struct(*base_lv.base);
            auto tmp = ctx.new_temp();
            ctx.emit(FieldGet{tmp, obj_op, base_lv.field_idx});
            return tmp;
        };
        auto obj_op = get_struct(*lv.base);
        ctx.emit(FieldSet{obj_op, lv.field_idx, val});
        break;
    }
    }
}

static void lower_stmt(const Stmt& s, FnCtx& ctx, IRProgram& prog) {
    switch (s.kind) {
    case Stmt::Kind::Empty: break;

    case Stmt::Kind::VarDecl: {
        auto& v = static_cast<const VarDeclStmt&>(s);
        // Если аннотация DynArray, а инициализатор — фиксированный массив,
        // переопределяем тип чтобы сгенерировать NewDynArray + push
        if (v.ann_type && v.ann_type->is_dynarray() &&
            v.init->type && v.init->type->is_array()) {
            const_cast<Expr*>(v.init.get())->type = v.ann_type;
        }
        auto val = lower_expr(*v.init, ctx, prog);
        int slot = ctx.fn.num_locals++;
        ctx.locals[v.name] = slot;
        ctx.fn.slot_names.resize(ctx.fn.num_locals, "");
        ctx.fn.slot_names[slot] = v.name;
        ctx.emit(Copy{LocalVar{slot, v.name}, val});
        break;
    }

    case Stmt::Kind::Assign: {
        auto& a = static_cast<const AssignStmt&>(s);
        auto val = lower_expr(*a.value, ctx, prog);
        lower_assign(*a.target, val, ctx, prog);
        break;
    }

    case Stmt::Kind::ExprStmt: {
        lower_expr(*static_cast<const ExprStmt&>(s).expr, ctx, prog);
        break;
    }

    case Stmt::Kind::If: {
        auto& i = static_cast<const IfStmt&>(s);
        auto cond = lower_expr(*i.cond, ctx, prog);
        std::string else_label = ctx.new_label("else");
        std::string end_label  = ctx.new_label("endif");
        ctx.emit(JumpFalse{cond, else_label});
        lower_stmt(*i.then_branch, ctx, prog);
        if (i.else_branch) ctx.emit(Jump{end_label});
        ctx.emit(Label{else_label});
        if (i.else_branch) {
            lower_stmt(*i.else_branch, ctx, prog);
            ctx.emit(Label{end_label});
        }
        break;
    }

    case Stmt::Kind::While: {
        auto& w = static_cast<const WhileStmt&>(s);
        std::string start  = ctx.new_label("while_start");
        std::string end_lb = ctx.new_label("while_end");
        ctx.emit(Label{start});
        auto cond = lower_expr(*w.cond, ctx, prog);
        ctx.emit(JumpFalse{cond, end_lb});
        ctx.loops.push_back({start, end_lb});
        lower_stmt(*w.body, ctx, prog);
        ctx.loops.pop_back();
        ctx.emit(Jump{start});
        ctx.emit(Label{end_lb});
        break;
    }

    case Stmt::Kind::ForRange: {
        auto& fr = static_cast<const ForRangeStmt&>(s);
        auto start_val = lower_expr(*fr.start, ctx, prog);
        int slot = ctx.fn.num_locals++;
        ctx.locals[fr.var_name] = slot;
        ctx.fn.slot_names.resize(ctx.fn.num_locals, "");
        ctx.fn.slot_names[slot] = fr.var_name;
        ctx.emit(Copy{LocalVar{slot, fr.var_name}, start_val});

        std::string lstart = ctx.new_label("for_start");
        std::string lend   = ctx.new_label("for_end");
        ctx.emit(Label{lstart});
        // условие: i < end
        auto i_op   = LocalVar{slot, fr.var_name};
        auto end_op = lower_expr(*fr.end, ctx, prog);
        auto cond   = ctx.new_temp();
        ctx.emit(IBinInstr{cond, IBinOp::ILt, i_op, end_op});
        ctx.emit(JumpFalse{cond, lend});
        ctx.loops.push_back({lstart, lend});
        lower_stmt(*fr.body, ctx, prog);
        ctx.loops.pop_back();
        // шаг: i = i + 1
        auto one  = ConstInt{1};
        auto next = ctx.new_temp();
        ctx.emit(IBinInstr{next, IBinOp::Add, i_op, one});
        ctx.emit(Copy{i_op, next});
        ctx.emit(Jump{lstart});
        ctx.emit(Label{lend});
        break;
    }

    case Stmt::Kind::ForC: {
        // for init; cond; step { тело }
        auto& fc = static_cast<const ForCStmt&>(s);
        lower_stmt(*fc.init, ctx, prog);
        std::string lstart = ctx.new_label("forc_start");
        std::string lend   = ctx.new_label("forc_end");
        ctx.emit(Label{lstart});
        auto cond_op = lower_expr(*fc.cond, ctx, prog);
        ctx.emit(JumpFalse{cond_op, lend});
        ctx.loops.push_back({lstart, lend});
        lower_stmt(*fc.body, ctx, prog);
        ctx.loops.pop_back();
        lower_stmt(*fc.step, ctx, prog);
        ctx.emit(Jump{lstart});
        ctx.emit(Label{lend});
        break;
    }

    case Stmt::Kind::Block: {
        for (auto& st : static_cast<const BlockStmt&>(s).stmts)
            lower_stmt(*st, ctx, prog);
        break;
    }

    case Stmt::Kind::Break:
        if (!ctx.loops.empty())
            ctx.emit(Jump{ctx.loops.back().break_label});
        break;

    case Stmt::Kind::Continue:
        if (!ctx.loops.empty())
            ctx.emit(Jump{ctx.loops.back().cont_label});
        break;

    case Stmt::Kind::Return: {
        auto& r = static_cast<const ReturnStmt&>(s);
        if (r.value) {
            auto val = lower_expr(*r.value, ctx, prog);
            ctx.emit(ReturnVal{val});
        } else {
            ctx.emit(Return{});
        }
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

IRProgram lower(Program& prog,
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
        if (gv.init->kind == Expr::Kind::IntLit)
            irp.global_inits[i] = ConstInt{static_cast<IntLitExpr&>(*gv.init).value};
        else if (gv.init->kind == Expr::Kind::FloatLit)
            irp.global_inits[i] = ConstFloat{static_cast<FloatLitExpr&>(*gv.init).value};
        else if (gv.init->kind == Expr::Kind::BoolLit)
            irp.global_inits[i] = ConstBool{static_cast<BoolLitExpr&>(*gv.init).value};
        else if (gv.init->kind == Expr::Kind::StringLit)
            irp.global_inits[i] = ConstString{static_cast<StringLitExpr&>(*gv.init).value};
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
    return std::visit([](auto&& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, TempVar>)
            return "t" + std::to_string(v.id);
        else if constexpr (std::is_same_v<T, LocalVar>)
            return v.name.empty() ? ("loc" + std::to_string(v.slot)) : v.name;
        else if constexpr (std::is_same_v<T, GlobalVar>)
            return "@" + v.name;
        else if constexpr (std::is_same_v<T, ConstInt>)
            return std::to_string(v.v);
        else if constexpr (std::is_same_v<T, ConstUInt>)
            return std::to_string(v.v) + "u";
        else if constexpr (std::is_same_v<T, ConstFloat>)
            return std::to_string(v.v);
        else if constexpr (std::is_same_v<T, ConstBool>)
            return v.v ? "true" : "false";
        else if constexpr (std::is_same_v<T, ConstString>)
            return "\"" + v.v + "\"";
        else
            return "unit";
    }, o);
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
            std::visit([&](auto&& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T,Label>)
                    out << v.name << ":\n";
                else if constexpr (std::is_same_v<T,IBinInstr>)
                    out << operand_to_str(v.dst)<<" = "
                        <<operand_to_str(v.lhs)<<" "<<ibin_str(v.op)<<" "<<operand_to_str(v.rhs);
                else if constexpr (std::is_same_v<T,FBinInstr>)
                    out << operand_to_str(v.dst)<<" = "
                        <<operand_to_str(v.lhs)<<" "<<fbin_str(v.op)<<" "<<operand_to_str(v.rhs);
                else if constexpr (std::is_same_v<T,LBinInstr>)
                    out << operand_to_str(v.dst)<<" = "
                        <<operand_to_str(v.lhs)<<(v.op==LBinOp::And?" and ":" or ")<<operand_to_str(v.rhs);
                else if constexpr (std::is_same_v<T,IUnInstr>)
                    out << operand_to_str(v.dst)<<" = -"<<operand_to_str(v.src);
                else if constexpr (std::is_same_v<T,FUnInstr>)
                    out << operand_to_str(v.dst)<<" = f-"<<operand_to_str(v.src);
                else if constexpr (std::is_same_v<T,LUnInstr>)
                    out << operand_to_str(v.dst)<<" = not "<<operand_to_str(v.src);
                else if constexpr (std::is_same_v<T,Copy>)
                    out << operand_to_str(v.dst)<<" = "<<operand_to_str(v.src);
                else if constexpr (std::is_same_v<T,SBinInstr>)
                    out << operand_to_str(v.dst)<<" = "<<operand_to_str(v.lhs)<<"++str"<<operand_to_str(v.rhs);
                else if constexpr (std::is_same_v<T,StrLen>)
                    out << operand_to_str(v.dst)<<" = strlen("<<operand_to_str(v.src)<<")";
                else if constexpr (std::is_same_v<T,ArrayLen>)
                    out << operand_to_str(v.dst)<<" = arrlen("<<operand_to_str(v.src)<<")";
                else if constexpr (std::is_same_v<T,SEqInstr>)
                    out << operand_to_str(v.dst)<<" = "<<operand_to_str(v.lhs)<<(v.eq?" seq ":" sneq ")<<operand_to_str(v.rhs);
                else if constexpr (std::is_same_v<T,NewArray>)
                    out << operand_to_str(v.dst)<<" = new_array["<<v.size<<"]";
                else if constexpr (std::is_same_v<T,ArrayGet>)
                    out << operand_to_str(v.dst)<<" = "<<operand_to_str(v.arr)<<"["<<operand_to_str(v.idx)<<"]";
                else if constexpr (std::is_same_v<T,ArraySet>)
                    out << operand_to_str(v.arr)<<"["<<operand_to_str(v.idx)<<"] = "<<operand_to_str(v.val);
                else if constexpr (std::is_same_v<T,NewStruct>)
                    out << operand_to_str(v.dst)<<" = new_struct "<<v.type_name;
                else if constexpr (std::is_same_v<T,FieldGet>)
                    out << operand_to_str(v.dst)<<" = "<<operand_to_str(v.obj)<<".f"<<v.field_idx;
                else if constexpr (std::is_same_v<T,FieldSet>)
                    out << operand_to_str(v.obj)<<".f"<<v.field_idx<<" = "<<operand_to_str(v.val);
                else if constexpr (std::is_same_v<T,Call>) {
                    if (v.dst) out << operand_to_str(*v.dst)<<" = ";
                    out << "call "<<v.fname<<"(";
                    for(size_t i=0;i<v.args.size();++i){if(i)out<<",";out<<operand_to_str(v.args[i]);}
                    out << ")";
                }
                else if constexpr (std::is_same_v<T,Cast>)
                    out << operand_to_str(v.dst)<<" = "<<operand_to_str(v.src)
                        <<" as "<<(v.to_type?v.to_type->to_string():"?");
                else if constexpr (std::is_same_v<T,Jump>)
                    out << "goto "<<v.label;
                else if constexpr (std::is_same_v<T,JumpFalse>)
                    out << "if_false "<<operand_to_str(v.cond)<<" goto "<<v.label;
                else if constexpr (std::is_same_v<T,JumpTrue>)
                    out << "if_true "<<operand_to_str(v.cond)<<" goto "<<v.label;
                else if constexpr (std::is_same_v<T,Return>)
                    out << "return";
                else if constexpr (std::is_same_v<T,ReturnVal>)
                    out << "return "<<operand_to_str(v.val);
                else if constexpr (std::is_same_v<T,Print>)
                    out << "print("<<operand_to_str(v.val)<<")";
                else if constexpr (std::is_same_v<T,Input>)
                    out << operand_to_str(v.dst)<<" = input()";
                else if constexpr (std::is_same_v<T,Exit>)
                    out << "exit("<<operand_to_str(v.code)<<")";
                else if constexpr (std::is_same_v<T,Panic>)
                    out << "panic("<<operand_to_str(v.msg)<<")";
                if constexpr (!std::is_same_v<T,Label>) out << "\n";
            }, instr);
        }
        out << "\n";
    }
}

} 
