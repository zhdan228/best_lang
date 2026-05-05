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
    uint16_t    num_slots; // всего слотов (включая параметры)
    const uint8_t* code;
    uint32_t    code_size;
};

struct PoolEntry {
    enum class Tag { Int64, UInt64, Float64, Bool, String, FName } tag;
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
    std::vector<Value>      globals;  // предынициализированные глобальные слоты
    uint32_t                main_idx = 0;
    const uint8_t*          data     = nullptr;
    size_t                  size     = 0;
};

static Module load(const std::vector<uint8_t>& bc) {
    const uint8_t* d = bc.data();
    if (bc.size() < 24)
        throw std::runtime_error("bytecode too small");
    if (d[0] != 0x42 || d[1] != 0x4C)
        throw std::runtime_error("invalid magic number");
    if (d[2] != 0x01)
        throw std::runtime_error("unsupported bytecode version");

    size_t p = 4;
    uint32_t pool_off = rd_u32(d, p);
    uint32_t fn_off   = rd_u32(d, p);
    rd_u32(d, p); // смещение таблицы отладки (пока не используется)
    uint32_t main_idx  = rd_u32(d, p);
    uint32_t glob_off  = rd_u32(d, p); // смещение секции глобальных переменных

    // Разбираем пул констант
    size_t pp = pool_off;
    uint32_t pool_count = rd_u32(d, pp);
    std::vector<PoolEntry> pool;
    pool.reserve(pool_count);
    for (uint32_t i = 0; i < pool_count; ++i) {
        PoolEntry e;
        uint8_t tag = rd_u8(d, pp);
        switch (tag) {
        case CONST_INT64:   e.tag = PoolEntry::Tag::Int64;   e.i = rd_i64(d, pp); break;
        case CONST_UINT64:  e.tag = PoolEntry::Tag::UInt64;  e.u = rd_u64(d, pp); break;
        case CONST_FLOAT64: e.tag = PoolEntry::Tag::Float64; e.f = rd_f64(d, pp); break;
        case CONST_BOOL:    e.tag = PoolEntry::Tag::Bool;    e.b = (rd_u8(d, pp) != 0); break;
        case CONST_STRING:  e.tag = PoolEntry::Tag::String;  e.s = rd_str_entry(d, pp); break;
        case CONST_FNAME:   e.tag = PoolEntry::Tag::FName;   e.s = rd_str_entry(d, pp); break;
        default: throw std::runtime_error("unknown constant tag");
        }
        pool.push_back(e);
    }

    // Разбираем секцию функций
    size_t fp = fn_off;
    uint32_t fn_count = rd_u32(d, fp);
    std::vector<FnRecord> functions;
    functions.reserve(fn_count);
    for (uint32_t i = 0; i < fn_count; ++i) {
        FnRecord fn;
        uint16_t name_idx = rd_u16(d, fp);
        fn.name       = pool[name_idx].s;
        fn.num_params = rd_u16(d, fp);
        fn.num_slots  = rd_u16(d, fp);
        fn.code_size  = rd_u32(d, fp);
        fn.code       = d + fp;
        fp += fn.code_size;
        functions.push_back(fn);
    }

    // Разбираем секцию глобальных переменных
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
    m.pool      = std::move(pool);
    m.functions = std::move(functions);
    m.main_idx  = main_idx;
    m.data      = d;
    m.size      = bc.size();
    m.globals   = std::move(globals);
    return m;
}

struct Frame {
    const FnRecord* fn;
    size_t          pc; // счётчик команд: следующая инструкция
    std::vector<Value> slots; // слоты локальных переменных
};

struct VMState {
    Module              mod;
    std::vector<Value>  stack;
    std::vector<Frame>  frames;
    // глобальные переменные хранятся в mod.globals
    std::vector<Value>& globals = mod.globals;
    uint32_t            current_line = 0;

    VMState(Module m) : mod(std::move(m)) {
        // глобальные уже инициализированы загрузчиком
    }

    Value& top()      { return stack.back(); }
    Value  pop_val()  { auto v = stack.back(); stack.pop_back(); return v; }
    void   push(Value v) { stack.push_back(std::move(v)); }

    Frame& cur_frame() { return frames.back(); }

    void runtime_error(const std::string& msg) {
        std::string full = "runtime error: " + msg + " at line " + std::to_string(current_line);
        throw std::runtime_error(full);
    }

    // Выполняет один вызов функции 
    Value call_function(uint16_t fn_idx, std::vector<Value> args) {
        const FnRecord& fn = mod.functions[fn_idx];
        Frame frame;
        frame.fn  = &fn;
        frame.pc  = 0;
        frame.slots.resize(fn.num_slots);
        // Копируем аргументы в первые num_params слотов
        for (int i = 0; i < static_cast<int>(args.size()) && i < static_cast<int>(fn.num_params); ++i)
            frame.slots[i] = std::move(args[i]);

        frames.push_back(std::move(frame));

        const uint8_t* code = fn.code;
        while (true) {
            Frame& f = cur_frame();
            size_t& pc = f.pc;

            if (pc >= f.fn->code_size) {
                frames.pop_back();
                return Value{};
            }

            uint8_t op = code[pc++];
            switch (op) {
            // Константы 
            case OP_PUSH_CONST: {
                uint16_t idx = (uint16_t)code[pc] | ((uint16_t)code[pc+1] << 8); pc += 2;
                push(mod.pool[idx].to_value());
                break;
            }
            case OP_PUSH_TRUE: {
                Value v; v.kind = Value::Kind::Bool; v.b = true; push(v);
                break;
            }
            case OP_PUSH_FALSE: {
                Value v; v.kind = Value::Kind::Bool; v.b = false; push(v);
                break;
            }
            case OP_PUSH_UNIT: {
                Value v; v.kind = Value::Kind::Unit; push(v);
                break;
            }
            // Локальные и глобальные переменные 
            case OP_LOAD: {
                uint16_t idx = (uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2;
                push(f.slots[idx]);
                break;
            }
            case OP_STORE: {
                uint16_t idx = (uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2;
                if (idx >= f.slots.size()) f.slots.resize(idx+1);
                f.slots[idx] = pop_val();
                break;
            }
            case OP_LOAD_GLOB: {
                uint16_t idx = (uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2;
                push(globals[idx]);
                break;
            }
            case OP_STORE_GLOB: {
                uint16_t idx = (uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2;
                if (idx >= globals.size()) globals.resize(idx+1);
                globals[idx] = pop_val();
                break;
            }
            // Целочисленная арифметика
            case OP_IADD: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Int; r.i=a.i+b.i; push(r); break; }
            case OP_ISUB: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Int; r.i=a.i-b.i; push(r); break; }
            case OP_IMUL: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Int; r.i=a.i*b.i; push(r); break; }
            case OP_IDIV: {
                auto b=pop_val(); auto a=pop_val();
                if (b.i == 0) runtime_error("division by zero");
                Value r; r.kind=Value::Kind::Int; r.i=a.i/b.i; push(r); break;
            }
            case OP_IMOD: {
                auto b=pop_val(); auto a=pop_val();
                if (b.i == 0) runtime_error("division by zero");
                Value r; r.kind=Value::Kind::Int; r.i=a.i%b.i; push(r); break;
            }
            case OP_INEG: { auto a=pop_val(); Value r; r.kind=Value::Kind::Int; r.i=-a.i; push(r); break; }
            // Вещественная арифметика 
            case OP_FADD: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=a.f+b.f; push(r); break; }
            case OP_FSUB: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=a.f-b.f; push(r); break; }
            case OP_FMUL: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=a.f*b.f; push(r); break; }
            case OP_FDIV: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=a.f/b.f; push(r); break; }
            case OP_FNEG: { auto a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=-a.f; push(r); break; }
            // Логические операции
            case OP_BAND: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=a.b&&b.b; push(r); break; }
            case OP_BOR:  { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=a.b||b.b; push(r); break; }
            case OP_BNOT: { auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=!a.b; push(r); break; }
            // Целочисленные сравнения
            case OP_IEQ: {
                auto b=pop_val(); auto a=pop_val();
                Value r; r.kind=Value::Kind::Bool;
                if (a.kind==Value::Kind::Unit || b.kind==Value::Kind::Unit)
                    r.b = (a.kind == b.kind);
                else
                    r.b = (a.i == b.i);
                push(r); break;
            }
            case OP_INEQ: {
                auto b=pop_val(); auto a=pop_val();
                Value r; r.kind=Value::Kind::Bool;
                if (a.kind==Value::Kind::Unit || b.kind==Value::Kind::Unit)
                    r.b = (a.kind != b.kind);
                else
                    r.b = (a.i != b.i);
                push(r); break;
            }
            case OP_ILT:  { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.i <b.i); push(r); break; }
            case OP_IGT:  { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.i >b.i); push(r); break; }
            case OP_ILTE: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.i<=b.i); push(r); break; }
            case OP_IGTE: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.i>=b.i); push(r); break; }
            // Вещественные сравнения 
            case OP_FEQ:  { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.f==b.f); push(r); break; }
            case OP_FNEQ: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.f!=b.f); push(r); break; }
            case OP_FLT:  { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.f <b.f); push(r); break; }
            case OP_FGT:  { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.f >b.f); push(r); break; }
            case OP_FLTE: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.f<=b.f); push(r); break; }
            case OP_FGTE: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(a.f>=b.f); push(r); break; }
            // Сравнения строк
            case OP_SEQ:  { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(*a.str==*b.str); push(r); break; }
            case OP_SNEQ: { auto b=pop_val(); auto a=pop_val(); Value r; r.kind=Value::Kind::Bool; r.b=(*a.str!=*b.str); push(r); break; }
            // Управление потоком 
            case OP_JMP: {
                int16_t off = (int16_t)((uint16_t)code[pc]|((uint16_t)code[pc+1]<<8));
                pc += 2;
                pc = (size_t)((int64_t)pc + off);
                break;
            }
            case OP_JMP_FALSE: {
                int16_t off = (int16_t)((uint16_t)code[pc]|((uint16_t)code[pc+1]<<8));
                pc += 2;
                auto cond = pop_val();
                if (!cond.b) pc = (size_t)((int64_t)pc + off);
                break;
            }
            case OP_JMP_TRUE: {
                int16_t off = (int16_t)((uint16_t)code[pc]|((uint16_t)code[pc+1]<<8));
                pc += 2;
                auto cond = pop_val();
                if (cond.b) pc = (size_t)((int64_t)pc + off);
                break;
            }
            case OP_CALL: {
                uint16_t fn_idx = (uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2;
                const FnRecord& callee = mod.functions[fn_idx];
                std::vector<Value> args(callee.num_params);
                for (int i = callee.num_params - 1; i >= 0; --i)
                    args[i] = pop_val();
                // Создаём новый фрейм
                Frame nf;
                nf.fn = &callee;
                nf.pc = 0;
                nf.slots.resize(callee.num_slots);
                for (int i = 0; i < callee.num_params; ++i)
                    nf.slots[i] = std::move(args[i]);
                frames.push_back(std::move(nf));
                code = callee.code;
                // Выполнение продолжается в новом фрейме
                break;
            }
            case OP_RET: {
                frames.pop_back();
                if (frames.empty()) return Value{};
                code = frames.back().fn->code;
                break;
            }
            case OP_RET_VAL: {
                Value ret = pop_val();
                frames.pop_back();
                if (frames.empty()) return ret;
                code = frames.back().fn->code;
                push(ret);
                break;
            }
            // Операции со стеком 
            case OP_POP: pop_val(); break;
            case OP_DUP: push(top()); break;
            // Массивы 
            case OP_NEW_ARRAY: {
                uint16_t len = (uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2;
                Value v; v.kind = Value::Kind::Array;
                v.arr = std::make_shared<ArrayObj>();
                v.arr->elems.resize(len);
                push(v);
                break;
            }
            case OP_ARRAY_GET: {
                auto idx_v = pop_val();
                auto arr_v = pop_val();
                int64_t idx = idx_v.kind==Value::Kind::Int ? idx_v.i : (int64_t)idx_v.u;
                if (!arr_v.arr)
                    runtime_error("null array dereference");
                auto& elems = arr_v.arr->elems;
                if (idx < 0 || idx >= (int64_t)elems.size())
                    runtime_error("index out of bounds: index " + std::to_string(idx) +
                                  ", size " + std::to_string(elems.size()));
                push(elems[idx]);
                break;
            }
            case OP_ARRAY_SET: {
                auto val = pop_val();
                auto idx_v = pop_val();
                auto arr_v = pop_val();
                int64_t idx = idx_v.kind==Value::Kind::Int ? idx_v.i : (int64_t)idx_v.u;
                if (!arr_v.arr) runtime_error("null array dereference");
                auto& elems = arr_v.arr->elems;
                if (idx < 0 || idx >= (int64_t)elems.size())
                    runtime_error("index out of bounds: index " + std::to_string(idx) +
                                  ", size " + std::to_string(elems.size()));
                elems[idx] = std::move(val);
                break;
            }
            case OP_ARRAY_LEN: {
                auto arr_v = pop_val();
                Value r; r.kind = Value::Kind::Int;
                r.i = (int64_t)(arr_v.arr ? arr_v.arr->elems.size() : 0);
                push(r);
                break;
            }
            case OP_ARRAY_PUSH: {
                auto val = pop_val();
                auto arr_v = pop_val();
                if (!arr_v.arr) runtime_error("push on null array");
                arr_v.arr->elems.push_back(std::move(val));
                break;
            }
            case OP_ARRAY_POP: {
                auto arr_v = pop_val();
                if (!arr_v.arr || arr_v.arr->elems.empty())
                    runtime_error("pop on empty array");
                push(arr_v.arr->elems.back());
                arr_v.arr->elems.pop_back();
                break;
            }
            // Структуры 
            case OP_NEW_STRUCT: {
                uint16_t n = (uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2;
                Value v; v.kind = Value::Kind::Struct;
                v.obj = std::make_shared<StructObj>();
                v.obj->fields.resize(n);
                push(v);
                break;
            }
            case OP_FIELD_GET: {
                uint16_t idx = (uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2;
                auto s = pop_val();
                push(s.obj->fields[idx]);
                break;
            }
            case OP_FIELD_SET: {
                uint16_t idx = (uint16_t)code[pc]|((uint16_t)code[pc+1]<<8); pc+=2;
                auto val = pop_val();
                auto s   = pop_val();
                s.obj->fields[idx] = std::move(val);
                break;
            }
            // Строки
            case OP_STR_CONCAT: {
                auto b = pop_val(); auto a = pop_val();
                Value r; r.kind = Value::Kind::Str;
                r.str = std::make_shared<std::string>(*a.str + *b.str);
                push(r);
                break;
            }
            case OP_STR_LEN: {
                auto s = pop_val();
                Value r; r.kind = Value::Kind::UInt;
                r.u = s.str ? s.str->size() : 0;
                push(r);
                break;
            }
            // Приведения типов 
            case OP_I2F: { auto a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=(double)a.i; push(r); break; }
            case OP_F2I: {
                auto a = pop_val();
                if (std::isnan(a.f)||std::isinf(a.f))
                    runtime_error("invalid cast: NaN or Inf to integer");
                Value r; r.kind=Value::Kind::Int; r.i=(int64_t)a.f; push(r); break;
            }
            case OP_F32_F64: {break; }
            case OP_F64_F32: { auto a=pop_val(); Value r; r.kind=Value::Kind::Float; r.f=(float)a.f; push(r); break; }
            case OP_ITRUNC: {
                uint8_t bits = code[pc++];
                auto a = pop_val();
                uint64_t raw = (a.kind==Value::Kind::UInt) ? a.u : (uint64_t)a.i;
                uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL<<bits)-1);
                Value r; r.kind=Value::Kind::Int; r.i = (int64_t)(raw & mask); push(r);
                break;
            }
            case OP_IEXT_S: {
                uint8_t bits = code[pc++];
                auto a = pop_val();
                int64_t v = (a.kind==Value::Kind::UInt) ? (int64_t)a.u : a.i;
                if (bits < 64) {
                    uint64_t sign = ((uint64_t)v >> (bits-1)) & 1;
                    if (sign) v |= (int64_t)(~0ULL << bits);
                    else      v &= ((1LL << bits)-1);
                }
                Value r; r.kind=Value::Kind::Int; r.i=v; push(r);
                break;
            }
            case OP_IEXT_U: {
                uint8_t bits = code[pc++];
                auto a = pop_val();
                uint64_t v = (a.kind==Value::Kind::UInt) ? a.u : (uint64_t)a.i;
                if (bits < 64) v &= ((1ULL<<bits)-1);
                Value r; r.kind=Value::Kind::UInt; r.u=v; push(r);
                break;
            }
            // Встроенные функции 
            case OP_PRINT: {
                auto v = pop_val();
                switch (v.kind) {
                case Value::Kind::Int:   std::cout << v.i;                      break;
                case Value::Kind::UInt:  std::cout << v.u;                      break;
                case Value::Kind::Float: std::cout << v.f;                      break;
                case Value::Kind::Bool:  std::cout << (v.b ? "true" : "false"); break;
                case Value::Kind::Str:   std::cout << *v.str;                   break;
                case Value::Kind::Unit:  std::cout << "()";                     break;
                default:                 std::cout << "<value>";                 break;
                }
                std::cout << "\n"; // print() всегда заканчивается переводом строки
                break;
            }

            case OP_PRINT_END: {
                // print(x, end="...") — окончание задаётся явно
                auto end_v = pop_val(); // end-строка
                auto v     = pop_val(); // значение
                switch (v.kind) {
                case Value::Kind::Int:   std::cout << v.i;                      break;
                case Value::Kind::UInt:  std::cout << v.u;                      break;
                case Value::Kind::Float: std::cout << v.f;                      break;
                case Value::Kind::Bool:  std::cout << (v.b ? "true" : "false"); break;
                case Value::Kind::Str:   std::cout << *v.str;                   break;
                case Value::Kind::Unit:  std::cout << "()";                     break;
                default:                 std::cout << "<value>";                 break;
                }
                // печатаем окончание из end= вместо "\n"
                if (end_v.kind == Value::Kind::Str) std::cout << *end_v.str;
                break;
            }
            case OP_INPUT: {
                std::string line;
                std::getline(std::cin, line);
                Value v; v.kind = Value::Kind::Str;
                v.str = std::make_shared<std::string>(std::move(line));
                push(v);
                break;
            }
            case OP_INPUT_INT: {
                // читаем строку и сразу парсим как целое число
                std::string line;
                std::getline(std::cin, line);
                try {
                    Value v; v.kind = Value::Kind::Int;
                    v.i = (int64_t)std::stol(line);
                    push(v);
                } catch (...) {
                    runtime_error("input_int(): cannot parse \"" + line + "\" as integer");
                }
                break;
            }
            case OP_INPUT_FLOAT: {
                // читаем строку и сразу парсим как вещественное
                std::string line;
                std::getline(std::cin, line);
                try {
                    Value v; v.kind = Value::Kind::Float;
                    v.f = std::stod(line);
                    push(v);
                } catch (...) {
                    runtime_error("input_float(): cannot parse \"" + line + "\" as float");
                }
                break;
            }
            case OP_TO_INT: {
                // преобразуем строку в int32
                auto s = pop_val();
                if (!s.str) runtime_error("to_int(): null string");
                try {
                    Value v; v.kind = Value::Kind::Int;
                    v.i = (int64_t)std::stol(*s.str);
                    push(v);
                } catch (...) {
                    runtime_error("to_int(): cannot parse \"" + *s.str + "\" as integer");
                }
                break;
            }
            case OP_TO_FLOAT: {
                // преобразуем строку в float64
                auto s = pop_val();
                if (!s.str) runtime_error("to_float(): null string");
                try {
                    Value v; v.kind = Value::Kind::Float;
                    v.f = std::stod(*s.str);
                    push(v);
                } catch (...) {
                    runtime_error("to_float(): cannot parse \"" + *s.str + "\" as float");
                }
                break;
            }
            case OP_EXIT: {
                auto code_v = pop_val();
                int ec = (code_v.kind == Value::Kind::Int) ? static_cast<int>(code_v.i) : 0;
                // Разматываем все фреймы
                frames.clear();
                Value exit_marker; exit_marker.kind = Value::Kind::Int; exit_marker.i = ec;
                throw std::runtime_error("__exit__:" + std::to_string(ec));
            }
            case OP_PANIC: {
                auto msg = pop_val();
                std::string m = msg.str ? *msg.str : "panic";
                runtime_error(m);
                break;
            }
            default:
                throw std::runtime_error("unknown opcode 0x" +
                    [&]{ std::ostringstream ss; ss << std::hex << static_cast<int>(op); return ss.str(); }());
            }

            // После вызова OP_CALL нам необходимо обновить указатель кода, чтобы он соответствовал новому
            if (op == OP_CALL) {
                code = frames.back().fn->code;
            }
        }
        return Value{};
    }

    VMResult run() {
        try {
            Value ret = call_function(mod.main_idx, {});
            int ec = (ret.kind == Value::Kind::Int) ? static_cast<int>(ret.i) : 0;
            return {ec, true, ""};
        } catch (const std::runtime_error& e) {
            std::string msg = e.what();
            // Маркер завершения через exit()
            if (msg.substr(0, 8) == "__exit__")
                return {std::stoi(msg.substr(8)), true, ""};
            std::cerr << msg << "\n";
            return {1, false, msg};
        }
    }
};

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
