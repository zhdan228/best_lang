#pragma once
#include "ir.hpp"
#include <vector>
#include <string>
#include <ostream>

namespace Codegen {

namespace Op {
    constexpr uint8_t PUSH_CONST  = 0x01;
    constexpr uint8_t PUSH_TRUE   = 0x02;
    constexpr uint8_t PUSH_FALSE  = 0x03;
    constexpr uint8_t PUSH_UNIT   = 0x04;

    constexpr uint8_t LOAD        = 0x10;
    constexpr uint8_t STORE       = 0x11;
    constexpr uint8_t LOAD_GLOB   = 0x12;
    constexpr uint8_t STORE_GLOB  = 0x13;
    constexpr uint8_t STORE_TEMP  = 0x14; // без deep_copy (для IR-временных)

    constexpr uint8_t IADD        = 0x20;
    constexpr uint8_t ISUB        = 0x21;
    constexpr uint8_t IMUL        = 0x22;
    constexpr uint8_t IDIV        = 0x23;
    constexpr uint8_t IMOD        = 0x24;
    constexpr uint8_t INEG        = 0x25;
    constexpr uint8_t UADD        = 0x26;
    constexpr uint8_t USUB        = 0x27;
    constexpr uint8_t FADD        = 0x28;
    constexpr uint8_t FSUB        = 0x29;
    constexpr uint8_t FMUL        = 0x2A;
    constexpr uint8_t FDIV        = 0x2B;
    constexpr uint8_t FNEG        = 0x2C;
    constexpr uint8_t UMUL        = 0x2D;
    constexpr uint8_t UDIV        = 0x2E;
    constexpr uint8_t UMOD        = 0x2F;

    constexpr uint8_t BAND        = 0x30;
    constexpr uint8_t BOR         = 0x31;
    constexpr uint8_t BNOT        = 0x32;

    constexpr uint8_t IEQ         = 0x40;
    constexpr uint8_t INEQ        = 0x41;
    constexpr uint8_t ILT         = 0x42;
    constexpr uint8_t IGT         = 0x43;
    constexpr uint8_t ILTE        = 0x44;
    constexpr uint8_t IGTE        = 0x45;
    constexpr uint8_t ULTS        = 0x46;
    constexpr uint8_t UGTS        = 0x47;
    constexpr uint8_t FEQ         = 0x48;
    constexpr uint8_t FNEQ        = 0x49;
    constexpr uint8_t FLT         = 0x4A;
    constexpr uint8_t FGT         = 0x4B;
    constexpr uint8_t FLTE        = 0x4C;
    constexpr uint8_t FGTE        = 0x4D;
    constexpr uint8_t SEQ         = 0x4E;
    constexpr uint8_t SNEQ        = 0x4F;

    constexpr uint8_t JMP         = 0x50;
    constexpr uint8_t JMP_FALSE   = 0x51;
    constexpr uint8_t JMP_TRUE    = 0x52;
    constexpr uint8_t CALL        = 0x53;
    constexpr uint8_t RET         = 0x54;
    constexpr uint8_t RET_VAL     = 0x55;
    constexpr uint8_t CALL_METHOD = 0x56; // CALL без deep_copy первого аргумента (self)

    constexpr uint8_t POP         = 0x60;
    constexpr uint8_t DUP         = 0x61;

    constexpr uint8_t NEW_ARRAY   = 0x70;
    constexpr uint8_t ARRAY_GET   = 0x71;
    constexpr uint8_t ARRAY_SET   = 0x72;
    constexpr uint8_t ARRAY_LEN   = 0x73;
    constexpr uint8_t ARRAY_PUSH  = 0x74;
    constexpr uint8_t ARRAY_POP   = 0x75;

    constexpr uint8_t NEW_STRUCT  = 0x80;
    constexpr uint8_t FIELD_GET   = 0x81;
    constexpr uint8_t FIELD_SET   = 0x82;

    constexpr uint8_t STR_CONCAT  = 0x90;
    constexpr uint8_t STR_LEN     = 0x91;
    constexpr uint8_t SLT         = 0x92;
    constexpr uint8_t SLE         = 0x93;
    constexpr uint8_t SGT         = 0x94;
    constexpr uint8_t SGE         = 0x95;

    constexpr uint8_t I2F         = 0xA0;
    constexpr uint8_t F2I         = 0xA1;
    constexpr uint8_t F32_F64     = 0xA2;
    constexpr uint8_t F64_F32     = 0xA3;
    constexpr uint8_t ITRUNC      = 0xA4;
    constexpr uint8_t IEXT_S      = 0xA5;
    constexpr uint8_t IEXT_U      = 0xA6;
    constexpr uint8_t ULTE        = 0xA7;
    constexpr uint8_t UGTE        = 0xA8;
    constexpr uint8_t IWRAP_S     = 0xA9; // mask N bits + sign-extend (signed wrap-around)
    constexpr uint8_t UWRAP       = 0xAA; // mask N bits (unsigned wrap-around)
    constexpr uint8_t U2F         = 0xAB; // uint → float64
    constexpr uint8_t F2U         = 0xAC; // float64 → uint

    constexpr uint8_t PRINT       = 0xB0;
    constexpr uint8_t INPUT       = 0xB1;
    constexpr uint8_t EXIT        = 0xB2;
    constexpr uint8_t PANIC       = 0xB3;
    constexpr uint8_t PRINT_END   = 0xB4;
    constexpr uint8_t INPUT_INT   = 0xB5;
    constexpr uint8_t INPUT_FLOAT = 0xB6;
    constexpr uint8_t TO_INT      = 0xB7;
    constexpr uint8_t TO_FLOAT    = 0xB8;
    constexpr uint8_t TO_CHAR     = 0xB9;
    constexpr uint8_t CHAR_TO_INT = 0xBA;
    constexpr uint8_t LINE        = 0xBB; // LINE u32 — обновить текущую строку в VM
}

namespace CtTag {
    constexpr uint8_t INT64   = 0x01;
    constexpr uint8_t UINT64  = 0x02;
    constexpr uint8_t FLOAT64 = 0x03;
    constexpr uint8_t BOOL    = 0x04;
    constexpr uint8_t STRING  = 0x05;
    constexpr uint8_t FNAME   = 0x06;
    constexpr uint8_t CHAR    = 0x07;
}

using Bytecode = std::vector<uint8_t>;

Bytecode compile(const IR::IRProgram& prog);
bool write_blc(const Bytecode& bc, const std::string& path);

}
