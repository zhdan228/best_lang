#include "vm.hpp"
#include "codegen.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <cassert>
#include <sstream>

/*
 * Виртуальная машина BL-VM — стековая архитектура.
 *
 * Каждый вызов функции создаёт новый фрейм со слотами для параметров
 * и локальных переменных. Операнды вычислений живут на отдельном стеке значений.
 *
 * Соглашение о вызовах:
 *   1) Вызывающая сторона кладёт аргументы на стек слева направо
 *   2) CALL создаёт новый фрейм, параметры занимают слоты 0..N-1
 *   3) RET_VAL кладёт результат на стек вызывающего фрейма
 *
 * Значения Value хранят тег типа (Int/Float/Bool/Str/Array/Struct/Unit)
 * и соответствующее поле. Строки, массивы и структуры — через shared_ptr.
 */

namespace VM {

using namespace Codegen;

// Глубокое копирование значения (value semantics для массивов и структур)
static Value deep_copy(const Value& v) {
    if (v.kind == Value::Kind::Array && v.arr) {
        Value r; r.kind = Value::Kind::Array;
        r.arr = std::make_shared<ArrayObj>();
        r.arr->elems.reserve(v.arr->elems.size());
        for (auto& e : v.arr->elems) r.arr->elems.push_back(deep_copy(e));
        return r;
    }
    if (v.kind == Value::Kind::Struct && v.obj) {
        Value r; r.kind = Value::Kind::Struct;
        r.obj = std::make_shared<StructObj>();
        r.obj->fields.reserve(v.obj->fields.size());
        for (auto& f : v.obj->fields) r.obj->fields.push_back(deep_copy(f));
        return r;
    }
    return v;
}
static uint8_t  rd_u8 (const uint8_t* d, size_t& p) { return d[p++]; }
static uint16_t rd_u16(const uint8_t* d, size_t& p) {
    uint16_t v = (uint16_t)d[p] | ((uint16_t)d[p+1] << 8); p += 2; return v;
}
static uint32_t rd_u32(const uint8_t* d, size_t& p) {
    uint32_t v = (uint32_t)d[p]|((uint32_t)d[p+1]<<8)|
                 ((uint32_t)d[p+2]<<16)|((uint32_t)d[p+3]<<24);
    p += 4; return v;
}
static int64_t rd_i64(const uint8_t* d, size_t& p) {
    uint64_t v = 0;
    for (int i=0;i<8;++i) v |= ((uint64_t)d[p+i] << (i*8));
    p += 8; return (int64_t)v;
}
static uint64_t rd_u64(const uint8_t* d, size_t& p) {
    uint64_t v = 0;
    for (int i=0;i<8;++i) v |= ((uint64_t)d[p+i] << (i*8));
    p += 8; return v;
}
static double rd_f64(const uint8_t* d, size_t& p) {
    uint64_t u = rd_u64(d, p);
    double v; memcpy(&v, &u, 8); return v;
}
static std::string rd_str_entry(const uint8_t* d, size_t& p) {
    uint32_t len = rd_u32(d, p);
    std::string s(d+p, d+p+len); p += len; return s;
}

struct FnRecord {
    std::string name;
    uint16_t    num_params;
    uint16_t    num_slots;
    const uint8_t* code;
    uint32_t    code_size;
};

struct PoolEntry {
    enum class Tag { Int64, UInt64, Float64, Bool, Char, String, FName } tag;
    int64_t  i = 0;
    uint64_t u = 0;
    double   f = 0.0;
    bool     b = false;
    std::string s;

    Value to_value() const {
        Value v;
        switch (tag) {
        case Tag::Int64:   v.kind = Value::Kind::Int;   v.i = i; break;
        case Tag::UInt64:  v.kind = Value::Kind::UInt;  v.u = u; break;
        case Tag::Float64: v.kind = Value::Kind::Float; v.f = f; break;
        case Tag::Bool:    v.kind = Value::Kind::Bool;  v.b = b; break;
        case Tag::Char:    v.kind = Value::Kind::Char;  v.i = i; break;
        case Tag::String:  case Tag::FName:
            v.kind = Value::Kind::Str;
            v.str  = std::make_shared<std::string>(s);
            break;
        }
        return v;
    }
};

struct Module {
    std::vector<PoolEntry>  pool;
    std::vector<FnRecord>   functions;
    std::vector<Value>      globals;
    uint32_t                main_idx = 0;
    const uint8_t*          data     = nullptr;
    size_t                  size     = 0;
};

static Module load(const std::vector<uint8_t>& bc) {
    const uint8_t* d = bc.data();
    if (bc.size() < 24)        throw std::runtime_error("bytecode too small");
    if (d[0] != 0x42 || d[1] != 0x4C) throw std::runtime_error("invalid magic number");
    if (d[2] != 0x01)          throw std::runtime_error("unsupported bytecode version");

    size_t p = 4;
    uint32_t pool_off  = rd_u32(d, p);
    uint32_t fn_off    = rd_u32(d, p);
    rd_u32(d, p);
    uint32_t main_idx  = rd_u32(d, p);
    uint32_t glob_off  = rd_u32(d, p);

    size_t pp = pool_off;
    uint32_t pool_count = rd_u32(d, pp);
    std::vector<PoolEntry> pool;
    pool.reserve(pool_count);
    for (uint32_t i = 0; i < pool_count; ++i) {
        PoolEntry e;
        uint8_t tag = rd_u8(d, pp);
        switch (tag) {
        case CtTag::INT64:   e.tag = PoolEntry::Tag::Int64;   e.i = rd_i64(d, pp); break;
        case CtTag::UINT64:  e.tag = PoolEntry::Tag::UInt64;  e.u = rd_u64(d, pp); break;
        case CtTag::FLOAT64: e.tag = PoolEntry::Tag::Float64; e.f = rd_f64(d, pp); break;
        case CtTag::BOOL:    e.tag = PoolEntry::Tag::Bool;    e.b = (rd_u8(d, pp) != 0); break;
        case CtTag::CHAR:    e.tag = PoolEntry::Tag::Char;    e.i = (int64_t)rd_u8(d, pp); break;
        case CtTag::STRING:  e.tag = PoolEntry::Tag::String;  e.s = rd_str_entry(d, pp); break;
        case CtTag::FNAME:   e.tag = PoolEntry::Tag::FName;   e.s = rd_str_entry(d, pp); break;
        default: throw std::runtime_error("unknown constant tag");
        }
        pool.push_back(e);
    }

    size_t fp = fn_off;
    uint32_t fn_count = rd_u32(d, fp);
    std::vector<FnRecord> functions;
    functions.reserve(fn_count);
    for (uint32_t i = 0; i < fn_count; ++i) {
        FnRecord fn;
        uint16_t name_idx = rd_u16(d, fp);
        fn.name = pool[name_idx].s; fn.num_params = rd_u16(d, fp);
        fn.num_slots = rd_u16(d, fp); fn.code_size = rd_u32(d, fp);
        fn.code = d + fp; fp += fn.code_size;
        functions.push_back(fn);
    }

    std::vector<Value> globals;
    if (glob_off < bc.size()) {
        size_t gp = glob_off;
        uint32_t ng = rd_u32(d, gp);
        globals.reserve(ng);
        for (uint32_t i = 0; i < ng; ++i) {
            uint16_t cidx = rd_u16(d, gp);
            globals.push_back(pool[cidx].to_value());
        }
    }

    Module m;
    m.pool = std::move(pool); m.functions = std::move(functions);
    m.main_idx = main_idx;   m.data = d; m.size = bc.size();
    m.globals = std::move(globals);
    return m;
}

struct Frame {
    const FnRecord* fn;
    size_t          pc;
    std::vector<Value> slots;
};

// ─────────────────────────────────────────────────────────────────────────────
// class VMState
// ─────────────────────────────────────────────────────────────────────────────

class VMState {
public:
    Module             mod;
    std::vector<Value> stack;
    std::vector<Frame> frames;
    std::vector<Value>& globals = mod.globals;
    uint32_t           current_line = 0;

    explicit VMState(Module m) : mod(std::move(m)) {}

    Value&   top();
    Value    pop_val();
    void     push(Value v);
    Frame&   cur_frame();
    void     runtime_error(const std::string& msg);
    Value    call_function(uint16_t fn_idx, std::vector<Value> args);
    VMResult run();

private:
    void exec_var_op    (uint8_t op, const uint8_t* code, size_t& pc);
    void exec_array_op  (uint8_t op, const uint8_t* code, size_t& pc);
    void exec_struct_op (uint8_t op, const uint8_t* code, size_t& pc);
    void exec_cast_op   (uint8_t op, const uint8_t* code, size_t& pc);
    void exec_builtin_op(uint8_t op);
};

Value&   VMState::top()         { return stack.back(); }
Value    VMState::pop_val()     { auto v = stack.back(); stack.pop_back(); return v; }
void     VMState::push(Value v) { stack.push_back(std::move(v)); }
Frame&   VMState::cur_frame()   { return frames.back(); }

void VMState::runtime_error(const std::string& msg) {
    throw std::runtime_error("runtime error: " + msg + " at line " + std::to_string(current_line));
}

//переменные и константы 

void VMState::exec_var_op(uint8_t op, const uint8_t* code, size_t& pc) {
    auto rd2 = [&]{ uint16_t i=(uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2; return i; };
    switch (op) {
    case Op::PUSH_CONST: push(mod.pool[rd2()].to_value()); break;
    case Op::LOAD:       push(frames.back().slots[rd2()]); break;
    case Op::STORE: {
        uint16_t idx = rd2(); auto& s = frames.back().slots;
        if (idx >= s.size()) s.resize(idx+1);
        s[idx] = deep_copy(pop_val()); break;
    }
    case Op::STORE_TEMP: {
        // Без deep_copy: IR-временные разделяют shared_ptr с оригиналом
        uint16_t idx = rd2(); auto& s = frames.back().slots;
        if (idx >= s.size()) s.resize(idx+1);
        s[idx] = pop_val(); break;
    }
    case Op::LOAD_GLOB:  push(globals[rd2()]); break;
    case Op::STORE_GLOB: {
        uint16_t idx = rd2();
        if (idx >= globals.size()) globals.resize(idx+1);
        globals[idx] = deep_copy(pop_val()); break;
    }
    }
}

// массивы 

void VMState::exec_array_op(uint8_t op, const uint8_t* code, size_t& pc) {
    switch (op) {
    case Op::NEW_ARRAY: {
        uint16_t len = (uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2;
        Value v; v.kind=Value::Kind::Array; v.arr=std::make_shared<ArrayObj>(); v.arr->elems.resize(len);
        push(v); break;
    }
    case Op::ARRAY_GET: {
        auto idx_v=pop_val(); auto arr_v=pop_val();
        int64_t idx = idx_v.kind==Value::Kind::Int ? idx_v.i : (int64_t)idx_v.u;
        if (!arr_v.arr) runtime_error("null array dereference");
        auto& el = arr_v.arr->elems;
        if (idx<0||idx>=(int64_t)el.size())
            runtime_error("index out of bounds: index "+std::to_string(idx)+", size "+std::to_string(el.size()));
        push(el[idx]); break;
    }
    case Op::ARRAY_SET: {
        auto val=pop_val(); auto idx_v=pop_val(); auto arr_v=pop_val();
        int64_t idx = idx_v.kind==Value::Kind::Int ? idx_v.i : (int64_t)idx_v.u;
        if (!arr_v.arr) runtime_error("null array dereference");
        auto& el = arr_v.arr->elems;
        if (idx<0||idx>=(int64_t)el.size())
            runtime_error("index out of bounds: index "+std::to_string(idx)+", size "+std::to_string(el.size()));
        el[idx]=std::move(val); break;
    }
    case Op::ARRAY_LEN: {
        auto a=pop_val(); Value r; r.kind=Value::Kind::Int;
        r.i=(int64_t)(a.arr ? a.arr->elems.size() : 0); push(r); break;
    }
    case Op::ARRAY_PUSH: {
        auto val=pop_val(); auto arr_v=pop_val();
        if (!arr_v.arr) runtime_error("push on null array");
        arr_v.arr->elems.push_back(std::move(val)); break;
    }
    case Op::ARRAY_POP: {
        auto arr_v=pop_val();
        if (!arr_v.arr||arr_v.arr->elems.empty()) runtime_error("pop on empty array");
        push(arr_v.arr->elems.back()); arr_v.arr->elems.pop_back(); break;
    }
    }
}

//структуры 

void VMState::exec_struct_op(uint8_t op, const uint8_t* code, size_t& pc) {
    auto rd2 = [&]{ uint16_t i=(uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2; return i; };
    switch (op) {
    case Op::NEW_STRUCT: {
        uint16_t n=rd2(); Value v; v.kind=Value::Kind::Struct;
        v.obj=std::make_shared<StructObj>(); v.obj->fields.resize(n); push(v); break;
    }
    case Op::FIELD_GET: { uint16_t idx=rd2(); auto s=pop_val(); push(s.obj->fields[idx]); break; }
    case Op::FIELD_SET: {
        uint16_t idx=rd2(); auto val=pop_val(); auto s=pop_val();
        s.obj->fields[idx]=std::move(val); break;
    }
    }
}

// приведения типов 

void VMState::exec_cast_op(uint8_t op, const uint8_t* code, size_t& pc) {
    switch (op) {
    case Op::I2F:     { auto a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=(double)a.i;  push(r); break; }
    case Op::U2F:     { auto a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=(double)a.u;  push(r); break; }
    case Op::F64_F32: { auto a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=(float)a.f;   push(r); break; }
    case Op::F32_F64: break;
    case Op::F2I: {
        auto a=pop_val();
        if (std::isnan(a.f)||std::isinf(a.f)) runtime_error("invalid cast: NaN or Inf to integer");
        Value r; r.kind=Value::Kind::Int;  r.i=(int64_t)a.f;  push(r); break;
    }
    case Op::F2U: {
        auto a=pop_val();
        if (std::isnan(a.f)||std::isinf(a.f)) runtime_error("invalid cast: NaN or Inf to integer");
        Value r; r.kind=Value::Kind::UInt; r.u=(uint64_t)a.f; push(r); break;
    }
    case Op::TO_CHAR:     { auto a=pop_val(); Value r; r.kind=Value::Kind::Char; r.i=(int64_t)((uint8_t)a.i); push(r); break; }
    case Op::CHAR_TO_INT: { auto a=pop_val(); Value r; r.kind=Value::Kind::Int;  r.i=a.i; push(r); break; }
    case Op::ITRUNC: {
        uint8_t bits=code[pc++]; auto a=pop_val();
        uint64_t raw=(a.kind==Value::Kind::UInt)?a.u:(uint64_t)a.i;
        uint64_t mask=(bits>=64)?~0ULL:((1ULL<<bits)-1);
        Value r; r.kind=Value::Kind::Int; r.i=(int64_t)(raw&mask); push(r); break;
    }
    case Op::IEXT_S: {
        uint8_t bits=code[pc++]; auto a=pop_val();
        int64_t v=(a.kind==Value::Kind::UInt)?(int64_t)a.u:a.i;
        if (bits<64) { uint64_t sign=((uint64_t)v>>(bits-1))&1;
                       if(sign) v|=(int64_t)(~0ULL<<bits); else v&=((1LL<<bits)-1); }
        Value r; r.kind=Value::Kind::Int; r.i=v; push(r); break;
    }
    case Op::IEXT_U: {
        uint8_t bits=code[pc++]; auto a=pop_val();
        uint64_t v=(a.kind==Value::Kind::UInt)?a.u:(uint64_t)a.i;
        if (bits<64) v&=((1ULL<<bits)-1);
        Value r; r.kind=Value::Kind::UInt; r.u=v; push(r); break;
    }
    case Op::IWRAP_S: {
        uint8_t bits=code[pc++]; auto a=pop_val();
        int64_t v=(a.kind==Value::Kind::UInt)?(int64_t)a.u:a.i;
        if (bits<64) { uint64_t mask=(1ULL<<bits)-1; v&=(int64_t)mask;
                       if(v&(int64_t)(1ULL<<(bits-1))) v|=(int64_t)(~mask); }
        Value r; r.kind=Value::Kind::Int; r.i=v; push(r); break;
    }
    case Op::UWRAP: {
        uint8_t bits=code[pc++]; auto a=pop_val();
        uint64_t v=(a.kind==Value::Kind::UInt)?a.u:(uint64_t)a.i;
        if (bits<64) v&=((1ULL<<bits)-1);
        Value r; r.kind=Value::Kind::UInt; r.u=v; push(r); break;
    }
    }
}

//встроенные функции 

static void print_value(const Value& v) {
    switch (v.kind) {
    case Value::Kind::Int:   std::cout << v.i;                      break;
    case Value::Kind::UInt:  std::cout << v.u;                      break;
    case Value::Kind::Float: std::cout << v.f;                      break;
    case Value::Kind::Bool:  std::cout << (v.b ? "true" : "false"); break;
    case Value::Kind::Char:  std::cout << (char)v.i;                break;
    case Value::Kind::Str:   std::cout << *v.str;                   break;
    case Value::Kind::Unit:  std::cout << "()";                     break;
    case Value::Kind::Array:
        std::cout << "[";
        if (v.arr) {
            for (size_t i = 0; i < v.arr->elems.size(); ++i) {
                if (i > 0) std::cout << ", ";
                print_value(v.arr->elems[i]);
            }
        }
        std::cout << "]";
        break;
    case Value::Kind::Struct:
        std::cout << "{";
        if (v.obj) {
            for (size_t i = 0; i < v.obj->fields.size(); ++i) {
                if (i > 0) std::cout << ", ";
                print_value(v.obj->fields[i]);
            }
        }
        std::cout << "}";
        break;
    default: std::cout << "<value>"; break;
    }
}

void VMState::exec_builtin_op(uint8_t op) {
    switch (op) {
    case Op::PRINT:     { auto v=pop_val(); print_value(v); std::cout<<"\n"; break; }
    case Op::PRINT_END: {
        auto end_v=pop_val(); auto v=pop_val(); print_value(v);
        if (end_v.kind==Value::Kind::Str) std::cout<<*end_v.str;
        break;
    }
    case Op::INPUT: {
        std::string line; std::getline(std::cin, line);
        Value v; v.kind=Value::Kind::Str; v.str=std::make_shared<std::string>(std::move(line));
        push(v); break;
    }
    case Op::INPUT_INT: {
        std::string line; std::getline(std::cin, line);
        try { Value v; v.kind=Value::Kind::Int; v.i=(int64_t)std::stol(line); push(v); }
        catch(...) { runtime_error("input_int(): cannot parse \""+line+"\" as integer"); }
        break;
    }
    case Op::INPUT_FLOAT: {
        std::string line; std::getline(std::cin, line);
        try { Value v; v.kind=Value::Kind::Float; v.f=std::stod(line); push(v); }
        catch(...) { runtime_error("input_float(): cannot parse \""+line+"\" as float"); }
        break;
    }
    case Op::TO_INT: {
        auto s=pop_val(); if(!s.str) runtime_error("to_int(): null string");
        try { Value v; v.kind=Value::Kind::Int; v.i=(int64_t)std::stol(*s.str); push(v); }
        catch(...) { runtime_error("to_int(): cannot parse \""+*s.str+"\" as integer"); }
        break;
    }
    case Op::TO_FLOAT: {
        auto s=pop_val(); if(!s.str) runtime_error("to_float(): null string");
        try { Value v; v.kind=Value::Kind::Float; v.f=std::stod(*s.str); push(v); }
        catch(...) { runtime_error("to_float(): cannot parse \""+*s.str+"\" as float"); }
        break;
    }
    case Op::EXIT: {
        auto cv=pop_val(); int ec=(cv.kind==Value::Kind::Int)?static_cast<int>(cv.i):0;
        frames.clear(); throw std::runtime_error("__exit__:"+std::to_string(ec));
    }
    case Op::PANIC: {
        auto msg=pop_val(); runtime_error(msg.str?*msg.str:"panic"); break;
    }
    }
}

//главный цикл интерпретации 

Value VMState::call_function(uint16_t fn_idx, std::vector<Value> args) {
    const FnRecord& fn = mod.functions[fn_idx];
    Frame frame; frame.fn=&fn; frame.pc=0; frame.slots.resize(fn.num_slots);
    for (int i=0; i<(int)args.size()&&i<(int)fn.num_params; ++i) frame.slots[i]=std::move(args[i]);
    frames.push_back(std::move(frame));

    const uint8_t* code = fn.code;
    while (true) {
        Frame& f  = cur_frame();
        size_t& pc = f.pc;
        if (pc >= f.fn->code_size) { frames.pop_back(); return Value{}; }

        uint8_t op = code[pc++];
        switch (op) {
        case Op::PUSH_TRUE:  { Value v; v.kind=Value::Kind::Bool;  v.b=true;  push(v); break; }
        case Op::PUSH_FALSE: { Value v; v.kind=Value::Kind::Bool;  v.b=false; push(v); break; }
        case Op::PUSH_UNIT:  { Value v; v.kind=Value::Kind::Unit;             push(v); break; }
        case Op::PUSH_CONST: case Op::LOAD: case Op::STORE: case Op::STORE_TEMP: case Op::LOAD_GLOB: case Op::STORE_GLOB:
            exec_var_op(op, code, pc); break;

        // Целочисленная арифметика (знаковая)
        case Op::IADD: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Int;   r.i=a.i+b.i; push(r); break; }
        case Op::ISUB: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Int;   r.i=a.i-b.i; push(r); break; }
        case Op::IMUL: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Int;   r.i=a.i*b.i; push(r); break; }
        case Op::IDIV: { auto b=pop_val(),a=pop_val(); if(b.i==0) runtime_error("division by zero"); Value r; r.kind=Value::Kind::Int;   r.i=a.i/b.i; push(r); break; }
        case Op::IMOD: { auto b=pop_val(),a=pop_val(); if(b.i==0) runtime_error("division by zero"); Value r; r.kind=Value::Kind::Int;   r.i=a.i%b.i; push(r); break; }
        case Op::INEG: { auto a=pop_val();              Value r; r.kind=Value::Kind::Int;   r.i=-a.i;    push(r); break; }
        // Целочисленная арифметика (беззнаковая)
        case Op::UADD: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::UInt;  r.u=a.u+b.u; push(r); break; }
        case Op::USUB: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::UInt;  r.u=a.u-b.u; push(r); break; }
        case Op::UMUL: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::UInt;  r.u=a.u*b.u; push(r); break; }
        case Op::UDIV: { auto b=pop_val(),a=pop_val(); if(b.u==0) runtime_error("division by zero"); Value r; r.kind=Value::Kind::UInt;  r.u=a.u/b.u; push(r); break; }
        case Op::UMOD: { auto b=pop_val(),a=pop_val(); if(b.u==0) runtime_error("division by zero"); Value r; r.kind=Value::Kind::UInt;  r.u=a.u%b.u; push(r); break; }
        // Вещественная арифметика
        case Op::FADD: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=a.f+b.f; push(r); break; }
        case Op::FSUB: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=a.f-b.f; push(r); break; }
        case Op::FMUL: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=a.f*b.f; push(r); break; }
        case Op::FDIV: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=a.f/b.f; push(r); break; }
        case Op::FNEG: { auto a=pop_val();              Value r; r.kind=Value::Kind::Float; r.f=-a.f;    push(r); break; }
        // Логика
        case Op::BAND: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool;  r.b=a.b&&b.b; push(r); break; }
        case Op::BOR:  { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool;  r.b=a.b||b.b; push(r); break; }
        case Op::BNOT: { auto a=pop_val();              Value r; r.kind=Value::Kind::Bool;  r.b=!a.b;     push(r); break; }
        // Сравнения целых (знаковые)
        case Op::IEQ:  { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool;
            r.b=(a.kind==Value::Kind::Unit||b.kind==Value::Kind::Unit)?(a.kind==b.kind):(a.i==b.i); push(r); break; }
        case Op::INEQ: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool;
            r.b=(a.kind==Value::Kind::Unit||b.kind==Value::Kind::Unit)?(a.kind!=b.kind):(a.i!=b.i); push(r); break; }
        case Op::ILT:  { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.i< b.i); push(r); break; }
        case Op::IGT:  { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.i> b.i); push(r); break; }
        case Op::ILTE: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.i<=b.i); push(r); break; }
        case Op::IGTE: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.i>=b.i); push(r); break; }
        // Сравнения целых (беззнаковые)
        case Op::ULTS: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.u< b.u); push(r); break; }
        case Op::UGTS: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.u> b.u); push(r); break; }
        case Op::ULTE: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.u<=b.u); push(r); break; }
        case Op::UGTE: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.u>=b.u); push(r); break; }
        // Сравнения вещественных
        case Op::FEQ:  { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.f==b.f); push(r); break; }
        case Op::FNEQ: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.f!=b.f); push(r); break; }
        case Op::FLT:  { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.f< b.f); push(r); break; }
        case Op::FGT:  { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.f> b.f); push(r); break; }
        case Op::FLTE: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.f<=b.f); push(r); break; }
        case Op::FGTE: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.f>=b.f); push(r); break; }
        // Сравнения строк
        case Op::SEQ:  { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(*a.str==*b.str); push(r); break; }
        case Op::SNEQ: { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(*a.str!=*b.str); push(r); break; }
        case Op::SLT:  { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(*a.str< *b.str); push(r); break; }
        case Op::SGT:  { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(*a.str> *b.str); push(r); break; }
        case Op::SLE:  { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(*a.str<=*b.str); push(r); break; }
        case Op::SGE:  { auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(*a.str>=*b.str); push(r); break; }
        // Управление потоком
        case Op::JMP: {
            int16_t off=(int16_t)((uint16_t)code[pc]|((uint16_t)code[pc+1]<<8)); pc+=2;
            pc=(size_t)((int64_t)pc+off); break;
        }
        case Op::JMP_FALSE: {
            int16_t off=(int16_t)((uint16_t)code[pc]|((uint16_t)code[pc+1]<<8)); pc+=2;
            if (!pop_val().b) pc=(size_t)((int64_t)pc+off);
            break;
        }
        case Op::JMP_TRUE: {
            int16_t off=(int16_t)((uint16_t)code[pc]|((uint16_t)code[pc+1]<<8)); pc+=2;
            if (pop_val().b)  pc=(size_t)((int64_t)pc+off);
            break;
        }
        case Op::CALL: {
            uint16_t idx=(uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2;
            const FnRecord& callee=mod.functions[idx];
            std::vector<Value> cargs(callee.num_params);
            for (int i=callee.num_params-1;i>=0;--i) cargs[i]=pop_val();
            Frame nf; nf.fn=&callee; nf.pc=0; nf.slots.resize(callee.num_slots);
            for (int i=0;i<callee.num_params;++i) nf.slots[i]=deep_copy(cargs[i]);
            frames.push_back(std::move(nf)); code=callee.code; break;
        }
        case Op::CALL_METHOD: {
            // Как CALL, но первый аргумент (self) передаётся без deep_copy,
            // чтобы мутации полей структуры были видны вызывающей стороне.
            uint16_t idx=(uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2;
            const FnRecord& callee=mod.functions[idx];
            std::vector<Value> cargs(callee.num_params);
            for (int i=callee.num_params-1;i>=0;--i) cargs[i]=pop_val();
            Frame nf; nf.fn=&callee; nf.pc=0; nf.slots.resize(callee.num_slots);
            if (callee.num_params > 0) nf.slots[0]=std::move(cargs[0]); // self — без копии
            for (int i=1;i<callee.num_params;++i) nf.slots[i]=deep_copy(cargs[i]);
            frames.push_back(std::move(nf)); code=callee.code; break;
        }
        case Op::RET:     { frames.pop_back(); if(frames.empty()) return Value{}; code=frames.back().fn->code; break; }
        case Op::RET_VAL: {
            Value ret=pop_val(); frames.pop_back();
            if(frames.empty()) return ret;
            code=frames.back().fn->code; push(ret); break;
        }
        case Op::POP: pop_val(); break;
        case Op::DUP: push(top()); break;
        case Op::STR_CONCAT: {
            auto b=pop_val(),a=pop_val(); Value r; r.kind=Value::Kind::Str;
            r.str=std::make_shared<std::string>(*a.str+*b.str); push(r); break;
        }
        case Op::STR_LEN: {
            auto s=pop_val(); Value r; r.kind=Value::Kind::UInt;
            r.u=s.str?s.str->size():0; push(r); break;
        }
        case Op::NEW_ARRAY: case Op::ARRAY_GET: case Op::ARRAY_SET:
        case Op::ARRAY_LEN: case Op::ARRAY_PUSH: case Op::ARRAY_POP:
            exec_array_op(op, code, pc); break;
        case Op::NEW_STRUCT: case Op::FIELD_GET: case Op::FIELD_SET:
            exec_struct_op(op, code, pc); break;
        case Op::I2F: case Op::U2F: case Op::F2I: case Op::F2U:
        case Op::F32_F64: case Op::F64_F32: case Op::TO_CHAR: case Op::CHAR_TO_INT:
        case Op::ITRUNC: case Op::IEXT_S: case Op::IEXT_U: case Op::IWRAP_S: case Op::UWRAP:
            exec_cast_op(op, code, pc); break;
        case Op::PRINT: case Op::PRINT_END: case Op::INPUT:
        case Op::INPUT_INT: case Op::INPUT_FLOAT:
        case Op::TO_INT: case Op::TO_FLOAT: case Op::EXIT: case Op::PANIC:
            exec_builtin_op(op); break;
        default:
            throw std::runtime_error("unknown opcode 0x" +
                [&]{ std::ostringstream ss; ss<<std::hex<<(int)op; return ss.str(); }());
        }
    }
    return Value{};
}

VMResult VMState::run() {
    try {
        Value ret = call_function(mod.main_idx, {});
        int ec = (ret.kind == Value::Kind::Int) ? static_cast<int>(ret.i) : 0;
        return {ec, true, ""};
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        if (msg.substr(0, 8) == "__exit__")
            return {std::stoi(msg.substr(8)), true, ""};
        std::cerr << msg << "\n";
        return {1, false, msg};
    }
}

VMResult run_bytecode(const std::vector<uint8_t>& bc) {
    try {
        Module m = load(bc);
        VMState state(std::move(m));
        return state.run();
    } catch (const std::exception& e) {
        std::cerr << "runtime error: " << e.what() << "\n";
        return {1, false, e.what()};
    }
}

VMResult run_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "error: cannot open '" << path << "'\n";
        return {1, false, "cannot open file"};
    }
    std::vector<uint8_t> bc((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    return run_bytecode(bc);
}

}
