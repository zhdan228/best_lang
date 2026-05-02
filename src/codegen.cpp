#include "codegen.hpp"
#include <unordered_map>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <cassert>
#include <cmath>

/*
 * Генератор байткода BL-VM.
 *
 * Транслирует IR-инструкции в последовательность байт формата .blc.
 * Стековая машина: все промежуточные значения живут на стеке, локальные
 * переменные и временные — в именованных слотах текущего стека вызова.
 *
 * Ключевые механизмы:
 *   - Пул констант с дедупликацией (строки, числа, имена функций)
 *   - Обратная засыпка (backpatching) для прыжков: сначала placeholder,
 *     потом вписываем реальное смещение когда узнаём адрес метки
 *   - Мангленные имена для перегруженных функций
 */

namespace Codegen {

static void write_u8(Bytecode& b, uint8_t v)  { b.push_back(v); }
static void write_u16(Bytecode& b, uint16_t v) {
    b.push_back(v & 0xFF);
    b.push_back((v >> 8) & 0xFF);
}
static void write_i16(Bytecode& b, int16_t v)  { write_u16(b, (uint16_t)v); }
static void write_u32(Bytecode& b, uint32_t v) {
    b.push_back(v & 0xFF);
    b.push_back((v >> 8) & 0xFF);
    b.push_back((v >> 16) & 0xFF);
    b.push_back((v >> 24) & 0xFF);
}
static void write_i64(Bytecode& b, int64_t v) {
    uint64_t u = (uint64_t)v;
    for (int i=0;i<8;++i) b.push_back((u>>(i*8))&0xFF);
}
static void write_u64(Bytecode& b, uint64_t v) {
    for (int i=0;i<8;++i) b.push_back((v>>(i*8))&0xFF);
}
static void write_f64(Bytecode& b, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    write_u64(b, u);
}
static void write_str_entry(Bytecode& b, const std::string& s) {
    write_u32(b, (uint32_t)s.size());
    for (char c : s) b.push_back((uint8_t)c);
}
static void patch_u16(Bytecode& b, size_t off, uint16_t v) {
    b[off]   = v & 0xFF;
    b[off+1] = (v >> 8) & 0xFF;
}
static void patch_u32(Bytecode& b, size_t off, uint32_t v) {
    b[off]   = v & 0xFF;
    b[off+1] = (v>>8) & 0xFF;
    b[off+2] = (v>>16) & 0xFF;
    b[off+3] = (v>>24) & 0xFF;
}
static void patch_i16(Bytecode& b, size_t off, int16_t v) {
    patch_u16(b, off, (uint16_t)v);
}

// пул констант
struct ConstPool {
    Bytecode data;
    int      count = 0;
    // Карты дедупликации
    std::unordered_map<int64_t,  uint16_t> ints;
    std::unordered_map<uint64_t, uint16_t> uints;
    std::unordered_map<double,   uint16_t> floats;
    std::unordered_map<std::string, uint16_t> strings;
    std::unordered_map<std::string, uint16_t> fnames;

    uint16_t add_int(int64_t v) {
        auto it = ints.find(v);
        if (it != ints.end()) return it->second;
        uint16_t idx = count++;
        write_u8(data, CONST_INT64);
        write_i64(data, v);
        ints[v] = idx;
        return idx;
    }
    uint16_t add_uint(uint64_t v) {
        auto it = uints.find(v);
        if (it != uints.end()) return it->second;
        uint16_t idx = count++;
        write_u8(data, CONST_UINT64);
        write_u64(data, v);
        uints[v] = idx;
        return idx;
    }
    uint16_t add_float(double v) {
        // Сравниваем по битовому образу (NaN-безопасно)
        uint64_t bits; memcpy(&bits, &v, 8);
        auto it = floats.find(v);
        if (it != floats.end()) return it->second;
        uint16_t idx = count++;
        write_u8(data, CONST_FLOAT64);
        write_f64(data, v);
        floats[v] = idx;
        return idx;
    }
    uint16_t add_bool(bool v) {
        uint16_t idx = count++;
        write_u8(data, CONST_BOOL);
        write_u8(data, v ? 1 : 0);
        return idx;
    }
    uint16_t add_string(const std::string& s) {
        auto it = strings.find(s);
        if (it != strings.end()) return it->second;
        uint16_t idx = count++;
        write_u8(data, CONST_STRING);
        write_str_entry(data, s);
        strings[s] = idx;
        return idx;
    }
    uint16_t add_fname(const std::string& s) {
        auto it = fnames.find(s);
        if (it != fnames.end()) return it->second;
        uint16_t idx = count++;
        write_u8(data, CONST_FNAME);
        write_str_entry(data, s);
        fnames[s] = idx;
        return idx;
    }
};

struct FnGen {
    const IR::IRFunction& fn;
    ConstPool&            pool;
    Bytecode              code;

    // Распределение слотов: временные идут после локальных
    // локальные: слоты 0..num_locals-1
    // временные: слоты начиная с num_locals
    int next_temp_slot;
    std::unordered_map<int, int> temp_slots; // temp_id → индекс слота

    // Обратная засыпка прыжков: метка → смещение в байтах
    std::unordered_map<std::string, size_t> label_offsets;
    struct Backpatch { size_t code_off; std::string label; };
    std::vector<Backpatch> backpatches;

    FnGen(const IR::IRFunction& f, ConstPool& p) : fn(f), pool(p), next_temp_slot(f.num_locals) {}

    int temp_slot(int temp_id) {
        auto it = temp_slots.find(temp_id);
        if (it != temp_slots.end()) return it->second;
        int s = next_temp_slot++;
        temp_slots[temp_id] = s;
        return s;
    }

    int total_slots() const { return next_temp_slot; }

    // Вспомогательные методы генерации
    void emit(uint8_t op) { write_u8(code, op); }
    void emit_u16(uint16_t v) { write_u16(code, v); }
    void emit_i16(int16_t v)  { write_i16(code, v); }
    void emit_u8(uint8_t v)   { write_u8(code, v); }
    void emit_u32(uint32_t v) { write_u32(code, v); }

    // Загружает операнд на стек VM
    void push_operand(const IR::Operand& o) {
        std::visit([&](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, IR::TempVar>) {
                emit(OP_LOAD);
                emit_u16((uint16_t)temp_slot(v.id));
            } else if constexpr (std::is_same_v<T, IR::LocalVar>) {
                emit(OP_LOAD);
                emit_u16((uint16_t)v.slot);
            } else if constexpr (std::is_same_v<T, IR::GlobalVar>) {
                emit(OP_LOAD_GLOB);
                emit_u16((uint16_t)v.slot);
            } else if constexpr (std::is_same_v<T, IR::ConstInt>) {
                uint16_t idx = pool.add_int(v.v);
                emit(OP_PUSH_CONST);
                emit_u16(idx);
            } else if constexpr (std::is_same_v<T, IR::ConstUInt>) {
                uint16_t idx = pool.add_uint(v.v);
                emit(OP_PUSH_CONST);
                emit_u16(idx);
            } else if constexpr (std::is_same_v<T, IR::ConstFloat>) {
                uint16_t idx = pool.add_float(v.v);
                emit(OP_PUSH_CONST);
                emit_u16(idx);
            } else if constexpr (std::is_same_v<T, IR::ConstBool>) {
                emit(v.v ? OP_PUSH_TRUE : OP_PUSH_FALSE);
            } else if constexpr (std::is_same_v<T, IR::ConstString>) {
                uint16_t idx = pool.add_string(v.v);
                emit(OP_PUSH_CONST);
                emit_u16(idx);
            } else {
                emit(OP_PUSH_UNIT);
            }
        }, o);
    }

    // Снимает вершину стека и сохраняет в dest
    void pop_to(const IR::Operand& dst) {
        std::visit([&](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, IR::TempVar>) {
                emit(OP_STORE);
                emit_u16((uint16_t)temp_slot(v.id));
            } else if constexpr (std::is_same_v<T, IR::LocalVar>) {
                emit(OP_STORE);
                emit_u16((uint16_t)v.slot);
            } else if constexpr (std::is_same_v<T, IR::GlobalVar>) {
                emit(OP_STORE_GLOB);
                emit_u16((uint16_t)v.slot);
            }
            else { emit(OP_POP); }
        }, dst);
    }

    // Генерирует прыжок с обратной засыпкой
    void emit_jump(uint8_t jmp_op, const std::string& label) {
        emit(jmp_op);
        size_t patch_off = code.size();
        emit_i16(0); 
        backpatches.push_back({patch_off, label});
    }

    // Применяет все накопленные засыпки
    void apply_backpatches() {
        for (auto& bp : backpatches) {
            auto it = label_offsets.find(bp.label);
            if (it == label_offsets.end())
                throw std::runtime_error("undefined label: " + bp.label);
            int64_t target  = (int64_t)it->second;
            int64_t cur_pos = (int64_t)(bp.code_off + 2);
            int64_t off     = target - cur_pos;
            if (off < -32768 || off > 32767)

} // namespace Codegen
