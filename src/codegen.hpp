#pragma once
#include "ir.hpp"
#include <vector>
#include <string>
#include <ostream>

namespace Codegen {
constexpr uint8_t OP_PUSH_CONST  = 0x01;
constexpr uint8_t OP_PUSH_TRUE   = 0x02;
constexpr uint8_t OP_PUSH_FALSE  = 0x03;
constexpr uint8_t OP_PUSH_UNIT   = 0x04;

constexpr uint8_t OP_LOAD        = 0x10;
constexpr uint8_t OP_STORE       = 0x11;
constexpr uint8_t OP_LOAD_GLOB   = 0x12;
constexpr uint8_t OP_STORE_GLOB  = 0x13;

constexpr uint8_t OP_IADD        = 0x20;
constexpr uint8_t OP_ISUB        = 0x21;
constexpr uint8_t OP_IMUL        = 0x22;
constexpr uint8_t OP_IDIV        = 0x23;
constexpr uint8_t OP_IMOD        = 0x24;
constexpr uint8_t OP_INEG        = 0x25;
constexpr uint8_t OP_FADD        = 0x28;
constexpr uint8_t OP_FSUB        = 0x29;
constexpr uint8_t OP_FMUL        = 0x2A;
constexpr uint8_t OP_FDIV        = 0x2B;
constexpr uint8_t OP_FNEG        = 0x2C;

constexpr uint8_t OP_BAND        = 0x30;
constexpr uint8_t OP_BOR         = 0x31;
constexpr uint8_t OP_BNOT        = 0x32;

constexpr uint8_t OP_IEQ         = 0x40;
constexpr uint8_t OP_INEQ        = 0x41;
constexpr uint8_t OP_ILT         = 0x42;
constexpr uint8_t OP_IGT         = 0x43;
constexpr uint8_t OP_ILTE        = 0x44;
constexpr uint8_t OP_IGTE        = 0x45;
constexpr uint8_t OP_FEQ         = 0x48;
constexpr uint8_t OP_FNEQ        = 0x49;
constexpr uint8_t OP_FLT         = 0x4A;
constexpr uint8_t OP_FGT         = 0x4B;
constexpr uint8_t OP_FLTE        = 0x4C;
constexpr uint8_t OP_FGTE        = 0x4D;
constexpr uint8_t OP_SEQ         = 0x4E;
constexpr uint8_t OP_SNEQ        = 0x4F;

constexpr uint8_t OP_JMP         = 0x50;
constexpr uint8_t OP_JMP_FALSE   = 0x51;
constexpr uint8_t OP_JMP_TRUE    = 0x52;
constexpr uint8_t OP_CALL        = 0x53;
constexpr uint8_t OP_RET         = 0x54;
constexpr uint8_t OP_RET_VAL     = 0x55;

constexpr uint8_t OP_POP         = 0x60;
constexpr uint8_t OP_DUP         = 0x61;

constexpr uint8_t OP_NEW_ARRAY   = 0x70;
constexpr uint8_t OP_ARRAY_GET   = 0x71;
constexpr uint8_t OP_ARRAY_SET   = 0x72;
constexpr uint8_t OP_ARRAY_LEN   = 0x73;
constexpr uint8_t OP_ARRAY_PUSH  = 0x74;  // [arr, val] → [] (dynamic push)
constexpr uint8_t OP_ARRAY_POP   = 0x75;  // [arr] → [val] (dynamic pop)

constexpr uint8_t OP_NEW_STRUCT  = 0x80;
constexpr uint8_t OP_FIELD_GET   = 0x81;
constexpr uint8_t OP_FIELD_SET   = 0x82;

constexpr uint8_t OP_STR_CONCAT  = 0x90;
constexpr uint8_t OP_STR_LEN     = 0x91;

constexpr uint8_t OP_I2F         = 0xA0;
constexpr uint8_t OP_F2I         = 0xA1;
constexpr uint8_t OP_F32_F64     = 0xA2;
constexpr uint8_t OP_F64_F32     = 0xA3;
constexpr uint8_t OP_ITRUNC      = 0xA4;
constexpr uint8_t OP_IEXT_S      = 0xA5;
constexpr uint8_t OP_IEXT_U      = 0xA6;

constexpr uint8_t OP_PRINT       = 0xB0;
constexpr uint8_t OP_INPUT       = 0xB1;
constexpr uint8_t OP_EXIT        = 0xB2;
constexpr uint8_t OP_PANIC       = 0xB3;
constexpr uint8_t OP_PRINT_END   = 0xB4;
constexpr uint8_t OP_INPUT_INT   = 0xB5; // input_int()   → int32
constexpr uint8_t OP_INPUT_FLOAT = 0xB6; // input_float() → float64
constexpr uint8_t OP_TO_INT      = 0xB7; // [str] → [int32]
constexpr uint8_t OP_TO_FLOAT    = 0xB8; // [str] → [float64]

// Теги элементов пула констант
constexpr uint8_t CONST_INT64    = 0x01;
constexpr uint8_t CONST_UINT64   = 0x02;
constexpr uint8_t CONST_FLOAT64  = 0x03;
constexpr uint8_t CONST_BOOL     = 0x04;
constexpr uint8_t CONST_STRING   = 0x05;
constexpr uint8_t CONST_FNAME    = 0x06;

// Результат компиляции — байты файла .blc
using Bytecode = std::vector<uint8_t>;

// Компилирует IR-программу в байт-вектор .blc
Bytecode compile(const IR::IRProgram& prog);

// Записывает байткод в файл
bool write_blc(const Bytecode& bc, const std::string& path);

} // namespace Codegen
