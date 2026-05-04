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

#ifdef DEBUG_VM
static void dbg(const std::vector<VM::Value>& s, const char* op) {
    std::cerr << op << " depth=" << s.size();
    if (!s.empty()) {
        auto& v=s.back();
        if (v.kind==VM::Value::Kind::Int) std::cerr << " top="<<v.i;
        else if (v.kind==VM::Value::Kind::Float) std::cerr << " top="<<v.f;
    }
    std::cerr << "\n";
}
#define DBG(op) dbg(stack, op)
#else
#define DBG(op)
#endif


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
    }
};

} // namespace VM
