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
                throw std::runtime_error("jump too far for int16: " + bp.label);
            patch_i16(code, bp.code_off, (int16_t)off);
        }
    }

    // Генерирует опкоды приведения типов
    void emit_cast(TypePtr from, TypePtr to) {
        if (!from || !to) return;
        if (*from == *to) return;
        // int → float
        if (from->is_int() && to->is_float()) { emit(OP_I2F); return; }
        // float → int
        if (from->is_float() && to->is_int()) { emit(OP_F2I); return; }
        // float32 ↔ float64
        if (from->kind == Type::Kind::Float32 && to->kind == Type::Kind::Float64) {
            emit(OP_F32_F64); return;
        }
        if (from->kind == Type::Kind::Float64 && to->kind == Type::Kind::Float32) {
            emit(OP_F64_F32); return;
        }
        if (from->is_int() && to->is_int()) {
            int from_bits = from->int_bits();
            int to_bits   = to->int_bits();
            if (to_bits < from_bits) {
                emit(OP_ITRUNC); emit_u8((uint8_t)to_bits);
            } else if (to_bits > from_bits) {
                if (to->is_signed_int()) {
                    emit(OP_IEXT_S); emit_u8((uint8_t)to_bits);
                } else {
                    emit(OP_IEXT_U); emit_u8((uint8_t)to_bits);
                }
            }
        }
    }

    // Генерирует байткод для одной IR-инструкции
    void gen_instr(const IR::Instr& instr,
                   const std::unordered_map<std::string,uint16_t>& fn_index) {
        std::visit([&](auto&& v) {
            using T = std::decay_t<decltype(v)>;

            if constexpr (std::is_same_v<T, IR::Label>) {
                label_offsets[v.name] = code.size();
            }

            else if constexpr (std::is_same_v<T, IR::IBinInstr>) {
                push_operand(v.lhs);
                push_operand(v.rhs);
                switch (v.op) {
                case IR::IBinOp::Add:  emit(OP_IADD); break;
                case IR::IBinOp::Sub:  emit(OP_ISUB); break;
                case IR::IBinOp::Mul:  emit(OP_IMUL); break;
                case IR::IBinOp::Div:  emit(OP_IDIV); break;
                case IR::IBinOp::Mod:  emit(OP_IMOD); break;
                case IR::IBinOp::IEq:  emit(OP_IEQ);  break;
                case IR::IBinOp::INeq: emit(OP_INEQ); break;
                case IR::IBinOp::ILt:  emit(OP_ILT);  break;
                case IR::IBinOp::ILe:  emit(OP_ILTE); break;
                case IR::IBinOp::IGt:  emit(OP_IGT);  break;
                case IR::IBinOp::IGe:  emit(OP_IGTE); break;
                }
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::FBinInstr>) {
                push_operand(v.lhs);
                push_operand(v.rhs);
                switch (v.op) {
                case IR::FBinOp::Add:  emit(OP_FADD);  break;
                case IR::FBinOp::Sub:  emit(OP_FSUB);  break;
                case IR::FBinOp::Mul:  emit(OP_FMUL);  break;
                case IR::FBinOp::Div:  emit(OP_FDIV);  break;
                case IR::FBinOp::FEq:  emit(OP_FEQ);   break;
                case IR::FBinOp::FNeq: emit(OP_FNEQ);  break;
                case IR::FBinOp::FLt:  emit(OP_FLT);   break;
                case IR::FBinOp::FLe:  emit(OP_FLTE);  break;
                case IR::FBinOp::FGt:  emit(OP_FGT);   break;
                case IR::FBinOp::FGe:  emit(OP_FGTE);  break;
                }
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::LBinInstr>) {
                push_operand(v.lhs);
                push_operand(v.rhs);
                emit(v.op == IR::LBinOp::And ? OP_BAND : OP_BOR);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::IUnInstr>) {
                push_operand(v.src);
                emit(OP_INEG);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::FUnInstr>) {
                push_operand(v.src);
                emit(OP_FNEG);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::LUnInstr>) {
                push_operand(v.src);
                emit(OP_BNOT);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::Copy>) {
                push_operand(v.src);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::SBinInstr>) {
                push_operand(v.lhs);
                push_operand(v.rhs);
                emit(OP_STR_CONCAT);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::StrLen>) {
                push_operand(v.src);
                emit(OP_STR_LEN);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::ArrayLen>) {
                push_operand(v.src);
                emit(OP_ARRAY_LEN);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::SEqInstr>) {
                push_operand(v.lhs);
                push_operand(v.rhs);
                emit(v.eq ? OP_SEQ : OP_SNEQ);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::NewArray>) {
                emit(OP_NEW_ARRAY);
                emit_u16((uint16_t)v.size);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::NewDynArray>) {
                // size=0 → пустой динамический массив
                emit(OP_NEW_ARRAY);
                emit_u16(0);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::ArrayPush>) {
                push_operand(v.arr);
                push_operand(v.val);
                emit(OP_ARRAY_PUSH);
            }

            else if constexpr (std::is_same_v<T, IR::ArrayPop>) {
                push_operand(v.arr);
                emit(OP_ARRAY_POP);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::ArrayGet>) {
                push_operand(v.arr);
                push_operand(v.idx);
                emit(OP_ARRAY_GET);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::ArraySet>) {
                push_operand(v.arr);
                push_operand(v.idx);
                push_operand(v.val);
                emit(OP_ARRAY_SET);
            }

            else if constexpr (std::is_same_v<T, IR::NewStruct>) {
                emit(OP_NEW_STRUCT);
                emit_u16((uint16_t)v.n_fields);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::FieldGet>) {
                push_operand(v.obj);
                emit(OP_FIELD_GET);
                emit_u16((uint16_t)v.field_idx);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::FieldSet>) {
                push_operand(v.obj);
                push_operand(v.val);
                emit(OP_FIELD_SET);
                emit_u16((uint16_t)v.field_idx);
            }

            else if constexpr (std::is_same_v<T, IR::Call>) {
                for (auto& a : v.args) push_operand(a);
                auto it = fn_index.find(v.fname);
                if (it == fn_index.end())
                    throw std::runtime_error("undefined function: " + v.fname);
                emit(OP_CALL);
                emit_u16(it->second);
                if (v.dst) pop_to(*v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::Cast>) {
                push_operand(v.src);
                emit_cast(v.from_type, v.to_type);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::Jump>) {
                emit_jump(OP_JMP, v.label);
            }

            else if constexpr (std::is_same_v<T, IR::JumpFalse>) {
                push_operand(v.cond);
                emit_jump(OP_JMP_FALSE, v.label);
            }

            else if constexpr (std::is_same_v<T, IR::JumpTrue>) {
                push_operand(v.cond);
                emit_jump(OP_JMP_TRUE, v.label);
            }

            else if constexpr (std::is_same_v<T, IR::Return>) {
                emit(OP_RET);
            }

            else if constexpr (std::is_same_v<T, IR::ReturnVal>) {
                push_operand(v.val);
                emit(OP_RET_VAL);
            }

            else if constexpr (std::is_same_v<T, IR::Print>) {
                push_operand(v.val);
                emit(OP_PRINT);
            }

            else if constexpr (std::is_same_v<T, IR::PrintEnd>) {
                // стек: [val, end_str] — VM снимет end_str первым
                push_operand(v.val);
                push_operand(v.end);
                emit(OP_PRINT_END);
            }

            else if constexpr (std::is_same_v<T, IR::Input>) {
                emit(OP_INPUT);
                pop_to(v.dst);
            }
            else if constexpr (std::is_same_v<T, IR::InputInt>) {
                emit(OP_INPUT_INT);
                pop_to(v.dst);
            }
            else if constexpr (std::is_same_v<T, IR::InputFloat>) {
                emit(OP_INPUT_FLOAT);
                pop_to(v.dst);
            }
            else if constexpr (std::is_same_v<T, IR::ToInt>) {
                push_operand(v.src);
                emit(OP_TO_INT);
                pop_to(v.dst);
            }
            else if constexpr (std::is_same_v<T, IR::ToFloat>) {
                push_operand(v.src);
                emit(OP_TO_FLOAT);
                pop_to(v.dst);
            }

            else if constexpr (std::is_same_v<T, IR::Exit>) {
                push_operand(v.code);
                emit(OP_EXIT);
            }

            else if constexpr (std::is_same_v<T, IR::Panic>) {
                push_operand(v.msg);
                emit(OP_PANIC);
            }

        }, instr);
    }
};

Bytecode compile(const IR::IRProgram& prog) {
    ConstPool pool;
    Bytecode  header(24, 0); 

    // Строим индекс функций: имя → индекс
    std::unordered_map<std::string, uint16_t> fn_index;
    for (int i = 0; i < static_cast<int>(prog.functions.size()); ++i) {
        fn_index[prog.functions[i].name] = (uint16_t)i;
        pool.add_fname(prog.functions[i].name);
    }

    // Генерируем код для каждой функции
    std::vector<FnGen> generators;
    for (auto& fn : prog.functions)
        generators.emplace_back(fn, pool);

    for (auto& gen : generators) {
        for (auto& instr : gen.fn.body)
            const_cast<FnGen&>(gen).gen_instr(instr, fn_index);
        const_cast<FnGen&>(gen).apply_backpatches();
    }

    // Секция пула констант 
    uint32_t pool_offset = (uint32_t)header.size();
    Bytecode pool_bytes;
    write_u32(pool_bytes, (uint32_t)pool.count);
    pool_bytes.insert(pool_bytes.end(), pool.data.begin(), pool.data.end());

    // Секция функций 
    uint32_t fn_offset = pool_offset + (uint32_t)pool_bytes.size();
    Bytecode fn_bytes;
    write_u32(fn_bytes, (uint32_t)generators.size());
    for (auto& gen : generators) {
        uint16_t name_idx = pool.add_fname(gen.fn.name);
        write_u16(fn_bytes, name_idx);
        write_u16(fn_bytes, (uint16_t)gen.fn.num_params);
        write_u16(fn_bytes, (uint16_t)gen.total_slots());
        write_u32(fn_bytes, (uint32_t)gen.code.size());
        fn_bytes.insert(fn_bytes.end(), gen.code.begin(), gen.code.end());
    }

    // Секция глобальных переменных: num_globals + (const_pool_idx per global) 
    uint32_t fn_section_end = fn_offset + (uint32_t)fn_bytes.size();
    Bytecode glob_bytes;
    uint32_t n_globals = (uint32_t)prog.global_inits.size();
    write_u32(glob_bytes, n_globals);
    for (auto& init : prog.global_inits) {
        uint16_t cidx = std::visit([&](auto&& v) -> uint16_t {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T,IR::ConstInt>)    return pool.add_int(v.v);
            if constexpr (std::is_same_v<T,IR::ConstUInt>)   return pool.add_uint(v.v);
            if constexpr (std::is_same_v<T,IR::ConstFloat>)  return pool.add_float(v.v);
            if constexpr (std::is_same_v<T,IR::ConstBool>)   return pool.add_bool(v.v);
            if constexpr (std::is_same_v<T,IR::ConstString>) return pool.add_string(v.v);
            return pool.add_int(0); // unit → 0
        }, init);
        write_u16(glob_bytes, cidx);
    }

    // Таблица отладки 
    uint32_t glob_offset = fn_section_end;
    uint32_t dbg_offset  = glob_offset + (uint32_t)glob_bytes.size();
    Bytecode dbg_bytes;
    write_u32(dbg_bytes, 0); 

    // Пересчитываем пул после добавления инициализаторов 
    Bytecode pool_bytes2;
    write_u32(pool_bytes2, (uint32_t)pool.count);
    pool_bytes2.insert(pool_bytes2.end(), pool.data.begin(), pool.data.end());
    // Пересчитываем смещения
    pool_offset   = (uint32_t)header.size();
    fn_offset     = pool_offset + (uint32_t)pool_bytes2.size();
    glob_offset   = fn_offset   + (uint32_t)fn_bytes.size();
    dbg_offset    = glob_offset + (uint32_t)glob_bytes.size();

    // Собираем заголовок 
    header[0] = 0x42; header[1] = 0x4C;
    header[2] = 0x01; header[3] = 0x00;
    patch_u32(header, 4,  pool_offset);
    patch_u32(header, 8,  fn_offset);
    patch_u32(header, 12, dbg_offset);
    uint32_t main_idx = (uint32_t)(prog.main_idx >= 0 ? prog.main_idx : 0);
    patch_u32(header, 16, main_idx);
    patch_u32(header, 20, glob_offset); // смещение секции глобальных переменных

    // Финальный результат 
    Bytecode out;
    out.insert(out.end(), header.begin(),    header.end());
    out.insert(out.end(), pool_bytes2.begin(), pool_bytes2.end());
    out.insert(out.end(), fn_bytes.begin(),  fn_bytes.end());
    out.insert(out.end(), glob_bytes.begin(),glob_bytes.end());
    out.insert(out.end(), dbg_bytes.begin(), dbg_bytes.end());
    return out;
}

bool write_blc(const Bytecode& bc, const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bc.data()), (std::streamsize)bc.size());
    return f.good();
}

} 
