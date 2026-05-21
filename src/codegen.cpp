#include "codegen.hpp"
#include <unordered_map>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <type_traits>

namespace Codegen {

using FnIndex = std::unordered_map<std::string, uint16_t>;

// запись little-endian значений в байт-вектор

template<typename T>
static void write_le(Bytecode& b, T v) {
    using U = std::make_unsigned_t<T>;
    U u = static_cast<U>(v);
    for (size_t i = 0; i < sizeof(U); ++i)
        b.push_back(static_cast<uint8_t>(u >> (i * 8)));
}

template<typename T>
static void patch_le(Bytecode& b, size_t off, T v) {
    using U = std::make_unsigned_t<T>;
    U u = static_cast<U>(v);
    for (size_t i = 0; i < sizeof(U); ++i)
        b[off + i] = static_cast<uint8_t>(u >> (i * 8));
}

static void write_u8 (Bytecode& b, uint8_t  v) { b.push_back(v); }
static void write_u16(Bytecode& b, uint16_t v) { write_le(b, v); }
static void write_i16(Bytecode& b, int16_t  v) { write_le(b, v); }
static void write_u32(Bytecode& b, uint32_t v) { write_le(b, v); }
static void write_i64(Bytecode& b, int64_t  v) { write_le(b, v); }
static void write_u64(Bytecode& b, uint64_t v) { write_le(b, v); }
static void write_f64(Bytecode& b, double   v) { uint64_t u; memcpy(&u, &v, 8); write_le(b, u); }
static void write_str(Bytecode& b, const std::string& s) {
    write_le(b, (uint32_t)s.size());
    for (char c : s) b.push_back((uint8_t)c);
}

static void patch_u16(Bytecode& b, size_t off, uint16_t v) { patch_le(b, off, v); }
static void patch_u32(Bytecode& b, size_t off, uint32_t v) { patch_le(b, off, v); }
static void patch_i16(Bytecode& b, size_t off, int16_t  v) { patch_le(b, off, v); }

// пул констант с дедупликацией

struct ConstPool {
    Bytecode data;
    int      count = 0;
    std::unordered_map<int64_t,     uint16_t> ints;
    std::unordered_map<uint64_t,    uint16_t> uints;
    std::unordered_map<double,      uint16_t> floats;
    std::unordered_map<uint8_t,     uint16_t> chars;
    std::unordered_map<std::string, uint16_t> strings;
    std::unordered_map<std::string, uint16_t> fnames;

    template<typename K, typename WriteFunc>
    uint16_t add_dedup(std::unordered_map<K, uint16_t>& map, K val, uint8_t tag, WriteFunc write_fn) {
        auto [it, ok] = map.emplace(val, (uint16_t)count);
        if (!ok) return it->second;
        write_u8(data, tag);
        write_fn(data, val);
        return count++;
    }

    uint16_t add_int   (int64_t  v) { return add_dedup(ints,    v, CtTag::INT64,   write_i64); }
    uint16_t add_uint  (uint64_t v) { return add_dedup(uints,   v, CtTag::UINT64,  write_u64); }
    uint16_t add_float (double   v) { return add_dedup(floats,  v, CtTag::FLOAT64, write_f64); }
    uint16_t add_string(const std::string& s) { return add_dedup(strings, s, CtTag::STRING, write_str); }
    uint16_t add_fname (const std::string& s) { return add_dedup(fnames,  s, CtTag::FNAME,  write_str); }

    uint16_t add_bool(bool v) {
        uint16_t idx = count++;
        write_u8(data, CtTag::BOOL); write_u8(data, v ? 1 : 0);
        return idx;
    }
    uint16_t add_char(uint8_t v) { return add_dedup(chars, v, CtTag::CHAR, [](Bytecode& b, uint8_t c){ write_u8(b, c); }); }
};

// генератор байткода одной функции

struct FnGen {
    const IR::IRFunction& fn;
    ConstPool&            pool;
    const FnIndex&        fn_index;
    Bytecode              code;
    int                   next_temp_slot;
    std::unordered_map<int, int>            temp_slots;
    std::unordered_map<std::string, size_t> label_offsets;
    struct Backpatch { size_t code_off; std::string label; };
    std::vector<Backpatch> backpatches;

    FnGen(const IR::IRFunction& f, ConstPool& p, const FnIndex& fi)
        : fn(f), pool(p), fn_index(fi), next_temp_slot(f.num_locals) {}

    int  temp_slot(int id) {
        auto [it, ok] = temp_slots.emplace(id, next_temp_slot);
        if (ok) ++next_temp_slot;
        return it->second;
    }
    int  total_slots() const { return next_temp_slot; }

    void emit    (uint8_t  op) { write_u8 (code, op); }
    void emit_u8 (uint8_t  v)  { write_u8 (code, v);  }
    void emit_u16(uint16_t v)  { write_u16(code, v);  }
    void emit_i16(int16_t  v)  { write_i16(code, v);  }

    void emit_jump(uint8_t op, const std::string& label) {
        emit(op);
        backpatches.push_back({code.size(), label});
        emit_i16(0);
    }

    void apply_backpatches() {
        for (auto& bp : backpatches) {
            auto it = label_offsets.find(bp.label);
            if (it == label_offsets.end())
                throw std::runtime_error("undefined label: " + bp.label);
            int64_t off = (int64_t)it->second - (int64_t)(bp.code_off + 2);
            if (off < -32768 || off > 32767)
                throw std::runtime_error("jump too far: " + bp.label);
            patch_i16(code, bp.code_off, (int16_t)off);
        }
    }

    void emit_ibin_op(IR::IBinOp op, bool is_unsigned);
    void emit_fbin_op(IR::FBinOp op);
    void emit_cast(TypePtr from, TypePtr to);
    void push_operand(const IR::Operand& o);
    void pop_to(const IR::Operand& dst);

    void gen(const IR::Label&      v) { label_offsets[v.name] = code.size(); }
    void gen(const IR::IBinInstr&  v) {
        push_operand(v.lhs); push_operand(v.rhs);
        emit_ibin_op(v.op, v.is_unsigned);
        // После арифметических операций применяем wrap-around если тип уже́ 64 бит
        bool is_arith = (v.op == IR::IBinOp::Add || v.op == IR::IBinOp::Sub ||
                         v.op == IR::IBinOp::Mul || v.op == IR::IBinOp::Div ||
                         v.op == IR::IBinOp::Mod);
        if (is_arith && v.bits < 64) {
            if (v.is_unsigned) { emit(Op::UWRAP);   emit_u8(v.bits); }
            else               { emit(Op::IWRAP_S); emit_u8(v.bits); }
        }
        pop_to(v.dst);
    }
    void gen(const IR::FBinInstr&  v) { push_operand(v.lhs); push_operand(v.rhs); emit_fbin_op(v.op); pop_to(v.dst); }
    void gen(const IR::LBinInstr&  v) { push_operand(v.lhs); push_operand(v.rhs); emit(v.op==IR::LBinOp::And?Op::BAND:Op::BOR); pop_to(v.dst); }
    void gen(const IR::IUnInstr&   v) {
        push_operand(v.src); emit(Op::INEG);
        if (v.bits < 64) { emit(Op::IWRAP_S); emit_u8(v.bits); }
        pop_to(v.dst);
    }
    void gen(const IR::FUnInstr&   v) { push_operand(v.src); emit(Op::FNEG); pop_to(v.dst); }
    void gen(const IR::LUnInstr&   v) { push_operand(v.src); emit(Op::BNOT); pop_to(v.dst); }
    void gen(const IR::Copy&       v) { push_operand(v.src); pop_to(v.dst); }
    void gen(const IR::SBinInstr&  v) { push_operand(v.lhs); push_operand(v.rhs); emit(Op::STR_CONCAT); pop_to(v.dst); }
    void gen(const IR::SEqInstr&   v) { push_operand(v.lhs); push_operand(v.rhs); emit(v.eq?Op::SEQ:Op::SNEQ); pop_to(v.dst); }
    void gen(const IR::SCmpInstr&  v) {
        push_operand(v.lhs); push_operand(v.rhs);
        switch (v.op) {
        case IR::SCmpOp::Lt: emit(Op::SLT); break;
        case IR::SCmpOp::Le: emit(Op::SLE); break;
        case IR::SCmpOp::Gt: emit(Op::SGT); break;
        case IR::SCmpOp::Ge: emit(Op::SGE); break;
        }
        pop_to(v.dst);
    }
    void gen(const IR::StrLen&     v) { push_operand(v.src); emit(Op::STR_LEN);   pop_to(v.dst); }
    void gen(const IR::ArrayLen&   v) { push_operand(v.src); emit(Op::ARRAY_LEN); pop_to(v.dst); }
    void gen(const IR::NewArray&   v) { emit(Op::NEW_ARRAY); emit_u16((uint16_t)v.size); pop_to(v.dst); }
    void gen(const IR::NewDynArray&v) { emit(Op::NEW_ARRAY); emit_u16(0);                pop_to(v.dst); }
    void gen(const IR::ArrayPush&  v) { push_operand(v.arr); push_operand(v.val); emit(Op::ARRAY_PUSH); }
    void gen(const IR::ArrayPop&   v) { push_operand(v.arr); emit(Op::ARRAY_POP); pop_to(v.dst); }
    void gen(const IR::ArrayGet&   v) { push_operand(v.arr); push_operand(v.idx); emit(Op::ARRAY_GET); pop_to(v.dst); }
    void gen(const IR::ArraySet&   v) { push_operand(v.arr); push_operand(v.idx); push_operand(v.val); emit(Op::ARRAY_SET); }
    void gen(const IR::NewStruct&  v) { emit(Op::NEW_STRUCT); emit_u16((uint16_t)v.n_fields); pop_to(v.dst); }
    void gen(const IR::FieldGet&   v) { push_operand(v.obj); emit(Op::FIELD_GET); emit_u16((uint16_t)v.field_idx); pop_to(v.dst); }
    void gen(const IR::FieldSet&   v) { push_operand(v.obj); push_operand(v.val); emit(Op::FIELD_SET); emit_u16((uint16_t)v.field_idx); }
    void gen(const IR::Call&       v) {
        for (auto& a : v.args) push_operand(a);
        auto it = fn_index.find(v.fname);
        if (it == fn_index.end()) throw std::runtime_error("undefined function: " + v.fname);
        emit(v.is_method ? Op::CALL_METHOD : Op::CALL); emit_u16(it->second);
        if (v.dst) pop_to(*v.dst);
    }
    void gen(const IR::Cast&       v) { push_operand(v.src); emit_cast(v.from_type, v.to_type); pop_to(v.dst); }
    void gen(const IR::Jump&       v) { emit_jump(Op::JMP,       v.label); }
    void gen(const IR::JumpFalse&  v) { push_operand(v.cond); emit_jump(Op::JMP_FALSE, v.label); }
    void gen(const IR::JumpTrue&   v) { push_operand(v.cond); emit_jump(Op::JMP_TRUE,  v.label); }
    void gen(const IR::Return&      ) { emit(Op::RET); }
    void gen(const IR::ReturnVal&  v) { push_operand(v.val); emit(Op::RET_VAL); }
    void gen(const IR::Print&      v) { push_operand(v.val); emit(Op::PRINT); }
    void gen(const IR::PrintEnd&   v) { push_operand(v.val); push_operand(v.end); emit(Op::PRINT_END); }
    void gen(const IR::Input&      v) { emit(Op::INPUT);       pop_to(v.dst); }
    void gen(const IR::InputInt&   v) { emit(Op::INPUT_INT);   pop_to(v.dst); }
    void gen(const IR::InputFloat& v) { emit(Op::INPUT_FLOAT); pop_to(v.dst); }
    void gen(const IR::ToInt&      v) { push_operand(v.src); emit(Op::TO_INT);   pop_to(v.dst); }
    void gen(const IR::ToFloat&    v) { push_operand(v.src); emit(Op::TO_FLOAT); pop_to(v.dst); }
    void gen(const IR::Exit&       v) { push_operand(v.code); emit(Op::EXIT); }
    void gen(const IR::Panic&      v) { push_operand(v.msg);  emit(Op::PANIC); }

    void gen_instr(const IR::Instr& instr) {
        std::visit([this](auto&& v) { gen(v); }, instr);
    }
};

// загрузка операнда на стек VM

struct Pusher {
    FnGen& g;
    void operator()(const IR::TempVar&    v) { g.emit(Op::LOAD);       g.emit_u16((uint16_t)g.temp_slot(v.id)); }
    void operator()(const IR::LocalVar&   v) { g.emit(Op::LOAD);       g.emit_u16((uint16_t)v.slot); }
    void operator()(const IR::GlobalVar&  v) { g.emit(Op::LOAD_GLOB);  g.emit_u16((uint16_t)v.slot); }
    void operator()(const IR::ConstInt&   v) { g.emit(Op::PUSH_CONST); g.emit_u16(g.pool.add_int(v.v));    }
    void operator()(const IR::ConstUInt&  v) { g.emit(Op::PUSH_CONST); g.emit_u16(g.pool.add_uint(v.v));   }
    void operator()(const IR::ConstFloat& v) { g.emit(Op::PUSH_CONST); g.emit_u16(g.pool.add_float(v.v));  }
    void operator()(const IR::ConstBool&  v) { g.emit(v.v ? Op::PUSH_TRUE : Op::PUSH_FALSE); }
    void operator()(const IR::ConstChar&  v) { g.emit(Op::PUSH_CONST); g.emit_u16(g.pool.add_char((uint8_t)v.v)); }
    void operator()(const IR::ConstString&v) { g.emit(Op::PUSH_CONST); g.emit_u16(g.pool.add_string(v.v)); }
    void operator()(const IR::ConstUnit&  ) { g.emit(Op::PUSH_UNIT); }
};

// снятие вершины стека в операнд

struct Popper {
    FnGen& g;
    void operator()(const IR::TempVar&   v) { g.emit(Op::STORE_TEMP); g.emit_u16((uint16_t)g.temp_slot(v.id)); }
    void operator()(const IR::LocalVar&  v) { g.emit(Op::STORE);      g.emit_u16((uint16_t)v.slot); }
    void operator()(const IR::GlobalVar& v) { g.emit(Op::STORE_GLOB); g.emit_u16((uint16_t)v.slot); }
    template<typename T> void operator()(const T&) { g.emit(Op::POP); }
};

void FnGen::push_operand(const IR::Operand& o)  { std::visit(Pusher{*this}, o); }
void FnGen::pop_to(const IR::Operand& dst)       { std::visit(Popper{*this}, dst); }

void FnGen::emit_ibin_op(IR::IBinOp op, bool is_unsigned) {
    if (is_unsigned) {
        switch (op) {
        case IR::IBinOp::Add:  emit(Op::UADD);  break;
        case IR::IBinOp::Sub:  emit(Op::USUB);  break;
        case IR::IBinOp::Mul:  emit(Op::UMUL);  break;
        case IR::IBinOp::Div:  emit(Op::UDIV);  break;
        case IR::IBinOp::Mod:  emit(Op::UMOD);  break;
        case IR::IBinOp::IEq:  emit(Op::IEQ);   break; // битовый паттерн одинаков
        case IR::IBinOp::INeq: emit(Op::INEQ);  break;
        case IR::IBinOp::ILt:  emit(Op::ULTS);  break;
        case IR::IBinOp::ILe:  emit(Op::ULTE);  break;
        case IR::IBinOp::IGt:  emit(Op::UGTS);  break;
        case IR::IBinOp::IGe:  emit(Op::UGTE);  break;
        }
    } else {
        switch (op) {
        case IR::IBinOp::Add:  emit(Op::IADD); break;
        case IR::IBinOp::Sub:  emit(Op::ISUB); break;
        case IR::IBinOp::Mul:  emit(Op::IMUL); break;
        case IR::IBinOp::Div:  emit(Op::IDIV); break;
        case IR::IBinOp::Mod:  emit(Op::IMOD); break;
        case IR::IBinOp::IEq:  emit(Op::IEQ);  break;
        case IR::IBinOp::INeq: emit(Op::INEQ); break;
        case IR::IBinOp::ILt:  emit(Op::ILT);  break;
        case IR::IBinOp::ILe:  emit(Op::ILTE); break;
        case IR::IBinOp::IGt:  emit(Op::IGT);  break;
        case IR::IBinOp::IGe:  emit(Op::IGTE); break;
        }
    }
}

void FnGen::emit_fbin_op(IR::FBinOp op) {
    switch (op) {
    case IR::FBinOp::Add:  emit(Op::FADD);  break;
    case IR::FBinOp::Sub:  emit(Op::FSUB);  break;
    case IR::FBinOp::Mul:  emit(Op::FMUL);  break;
    case IR::FBinOp::Div:  emit(Op::FDIV);  break;
    case IR::FBinOp::FEq:  emit(Op::FEQ);   break;
    case IR::FBinOp::FNeq: emit(Op::FNEQ);  break;
    case IR::FBinOp::FLt:  emit(Op::FLT);   break;
    case IR::FBinOp::FLe:  emit(Op::FLTE);  break;
    case IR::FBinOp::FGt:  emit(Op::FGT);   break;
    case IR::FBinOp::FGe:  emit(Op::FGTE);  break;
    }
}

void FnGen::emit_cast(TypePtr from, TypePtr to) {
    if (!from || !to || *from == *to) return;
    bool from_signed = from->is_signed_int();
    bool to_signed   = to->is_signed_int();
    bool from_uint   = from->is_unsigned_int();
    bool to_uint     = to->is_unsigned_int();
    if (from->is_char()  && to->is_int())   { emit(Op::CHAR_TO_INT); return; }
    if (from->is_int()   && to->is_char())  { emit(Op::TO_CHAR); return; }
    if (from_signed      && to->is_float()) { emit(Op::I2F);  return; }
    if (from_uint        && to->is_float()) { emit(Op::U2F);  return; }
    if (from->is_float() && to_signed)      { emit(Op::F2I);  return; }
    if (from->is_float() && to_uint)        { emit(Op::F2U);  return; }
    if (from->kind == Type::Kind::Float32 && to->kind == Type::Kind::Float64) { emit(Op::F32_F64); return; }
    if (from->kind == Type::Kind::Float64 && to->kind == Type::Kind::Float32) { emit(Op::F64_F32); return; }
    if (from->is_int() && to->is_int()) {
        int fb = from->int_bits(), tb = to->int_bits();
        if (tb < fb) {
            // усечение: маска
            emit(to_uint ? Op::ITRUNC : Op::ITRUNC);
            emit_u8((uint8_t)tb);
            if (to_signed) { emit(Op::IEXT_S); emit_u8((uint8_t)tb); }
        } else if (tb > fb) {
            // расширение
            emit(from_signed ? Op::IEXT_S : Op::IEXT_U);
            emit_u8((uint8_t)tb);
        }
        // смена знаковости при одинаковом размере — ничего не делаем (биты те же)
    }
}

// сборка секций файла .blc

static FnIndex build_fn_index(ConstPool& pool, const IR::IRProgram& prog) {
    FnIndex idx;
    for (int i = 0; i < (int)prog.functions.size(); ++i) {
        idx[prog.functions[i].name] = (uint16_t)i;
        pool.add_fname(prog.functions[i].name);
    }
    return idx;
}

static std::vector<FnGen> gen_all_functions(const IR::IRProgram& prog,
                                             ConstPool& pool, const FnIndex& fn_index) {
    std::vector<FnGen> gens;
    for (auto& fn : prog.functions) gens.emplace_back(fn, pool, fn_index);
    for (auto& g : gens) {
        for (auto& instr : g.fn.body) g.gen_instr(instr);
        g.apply_backpatches();
    }
    return gens;
}

static Bytecode build_fn_bytes(std::vector<FnGen>& gens, ConstPool& pool) {
    Bytecode b;
    write_u32(b, (uint32_t)gens.size());
    for (auto& g : gens) {
        write_u16(b, pool.add_fname(g.fn.name));
        write_u16(b, (uint16_t)g.fn.num_params);
        write_u16(b, (uint16_t)g.total_slots());
        write_u32(b, (uint32_t)g.code.size());
        b.insert(b.end(), g.code.begin(), g.code.end());
    }
    return b;
}

struct InitVisitor {
    ConstPool& pool;
    uint16_t operator()(const IR::ConstInt&    v) { return pool.add_int(v.v);    }
    uint16_t operator()(const IR::ConstUInt&   v) { return pool.add_uint(v.v);   }
    uint16_t operator()(const IR::ConstFloat&  v) { return pool.add_float(v.v);  }
    uint16_t operator()(const IR::ConstBool&   v) { return pool.add_bool(v.v);   }
    uint16_t operator()(const IR::ConstChar&   v) { return pool.add_char((uint8_t)v.v); }
    uint16_t operator()(const IR::ConstString& v) { return pool.add_string(v.v); }
    uint16_t operator()(const IR::TempVar&   )    { return pool.add_int(0); }
    uint16_t operator()(const IR::LocalVar&  )    { return pool.add_int(0); }
    uint16_t operator()(const IR::GlobalVar& )    { return pool.add_int(0); }
    uint16_t operator()(const IR::ConstUnit& )    { return pool.add_int(0); }
};

static Bytecode build_glob_bytes(ConstPool& pool, const IR::IRProgram& prog) {
    Bytecode b;
    write_u32(b, (uint32_t)prog.global_inits.size());
    for (auto& init : prog.global_inits)
        write_u16(b, std::visit(InitVisitor{pool}, init));
    return b;
}

static Bytecode build_pool_bytes(const ConstPool& pool) {
    Bytecode b;
    write_u32(b, (uint32_t)pool.count);
    b.insert(b.end(), pool.data.begin(), pool.data.end());
    return b;
}

Bytecode compile(const IR::IRProgram& prog) {
    ConstPool pool;
    auto fn_index   = build_fn_index(pool, prog);
    auto gens       = gen_all_functions(prog, pool, fn_index);
    auto fn_bytes   = build_fn_bytes(gens, pool);
    auto glob_bytes = build_glob_bytes(pool, prog);
    auto pool_bytes = build_pool_bytes(pool);

    Bytecode dbg_bytes; write_u32(dbg_bytes, 0);
    Bytecode header(24, 0);

    uint32_t pool_off = (uint32_t)header.size();
    uint32_t fn_off   = pool_off + (uint32_t)pool_bytes.size();
    uint32_t glob_off = fn_off   + (uint32_t)fn_bytes.size();
    uint32_t dbg_off  = glob_off + (uint32_t)glob_bytes.size();
    uint32_t main_idx = (uint32_t)(prog.main_idx >= 0 ? prog.main_idx : 0);

    header[0]=0x42; header[1]=0x4C; header[2]=0x01; header[3]=0x00;
    patch_u32(header,  4, pool_off);
    patch_u32(header,  8, fn_off);
    patch_u32(header, 12, dbg_off);
    patch_u32(header, 16, main_idx);
    patch_u32(header, 20, glob_off);

    Bytecode out;
    for (auto* s : {&header, &pool_bytes, &fn_bytes, &glob_bytes, &dbg_bytes})
        out.insert(out.end(), s->begin(), s->end());
    return out;
}

bool write_blc(const Bytecode& bc, const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bc.data()), (std::streamsize)bc.size());
    return f.good();
}

}
