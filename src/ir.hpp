/*
 * Промежуточное представление (IR) BestLang — трёхадресный код.
 *
 * Каждая инструкция содержит не более одного оператора и трёх операндов.
 * Временные переменные (TempVar) — виртуальные регистры без ограничения по числу.
 * IR является входом для оптимизатора (свёртка констант) и генератора кода.
 *
 * Пример: a + b * c  →  t0 = b * c;  t1 = a + t0
 */

#pragma once
#include "types.hpp"
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <cstdint>
#include <ostream>

namespace IR {

// Операнд — то что может стоять слева/справа в IR инструкции
struct TempVar  { int id; };             // временная переменная t0, t1, ...
struct LocalVar { int slot; std::string name; };  // локальная переменная
struct GlobalVar{ int slot; std::string name; };  // глобальная переменная
struct ConstInt   { int64_t  v; };
struct ConstUInt  { uint64_t v; };
struct ConstFloat { double   v; };
struct ConstBool  { bool     v; };
struct ConstString{ std::string v; };
struct ConstUnit  {}; // значение void/null

using Operand = std::variant<
    TempVar, LocalVar, GlobalVar,
    ConstInt, ConstUInt, ConstFloat, ConstBool, ConstString, ConstUnit
>;

// Перечисления операций
enum class IBinOp { Add, Sub, Mul, Div, Mod, IEq, INeq, ILt, ILe, IGt, IGe };
enum class FBinOp { Add, Sub, Mul, Div, FEq, FNeq, FLt, FLe, FGt, FGe };
enum class LBinOp { And, Or };
enum class IUnOp  { Neg };
enum class FUnOp  { Neg };
enum class LUnOp  { Not };

// IR инструкции
struct Label    { std::string name; };

struct IBinInstr { Operand dst; IBinOp op; Operand lhs, rhs; };
struct FBinInstr { Operand dst; FBinOp op; Operand lhs, rhs; };
struct LBinInstr { Operand dst; LBinOp op; Operand lhs, rhs; };
struct IUnInstr  { Operand dst; IUnOp  op; Operand src; };
struct FUnInstr  { Operand dst; FUnOp  op; Operand src; };
struct LUnInstr  { Operand dst; LUnOp  op; Operand src; };

struct Copy { Operand dst; Operand src; }; // dst = src

struct SBinInstr { Operand dst; Operand lhs, rhs; }; // конкатенация строк
struct StrLen    { Operand dst; Operand src; };
struct ArrayLen  { Operand dst; Operand src; };

// Сравнение строк: eq=true → ==, eq=false → !=
struct SEqInstr  { Operand dst; Operand lhs, rhs; bool eq; };

struct NewArray    { Operand dst; int64_t size; TypePtr elem; };
struct NewDynArray { Operand dst; TypePtr elem; }; // пустой динамический массив
struct ArrayGet  { Operand dst; Operand arr; Operand idx; };
struct ArraySet  { Operand arr; Operand idx; Operand val; };
struct ArrayPush { Operand arr; Operand val; };
struct ArrayPop  { Operand dst; Operand arr; };

struct NewStruct { Operand dst; std::string type_name; int n_fields; };
struct FieldGet  { Operand dst; Operand obj; int field_idx; };
struct FieldSet  { Operand obj; int field_idx; Operand val; };

struct Call {
    std::optional<Operand> dst; // нет значения для void-вызовов
    std::string fname;          // полностью квалифицированное имя
    std::vector<Operand> args;
};

// Приведение числовых типов
struct Cast {
    Operand dst;
    Operand src;
    TypePtr from_type;
    TypePtr to_type;
};

struct Jump      { std::string label; };
struct JumpFalse { Operand cond; std::string label; };
struct JumpTrue  { Operand cond; std::string label; };

struct Return    {};
struct ReturnVal { Operand val; };

struct Print    { Operand val; TypePtr type; };           // print(x)
struct PrintEnd { Operand val; TypePtr type; Operand end; }; // print(x, end="...")
struct Input      { Operand dst; };           // input()       → string
struct InputInt   { Operand dst; };           // input_int()   → int32
struct InputFloat { Operand dst; };           // input_float() → float64
struct ToInt      { Operand dst; Operand src; }; // to_int(s)  → int32
struct ToFloat    { Operand dst; Operand src; }; // to_float(s) → float64
struct Exit  { Operand code; };
struct Panic { Operand msg;  };

using Instr = std::variant<
    Label,
    IBinInstr, FBinInstr, LBinInstr,
    IUnInstr, FUnInstr, LUnInstr,
    Copy,
    SBinInstr, StrLen, ArrayLen, SEqInstr,
    NewArray, NewDynArray, ArrayGet, ArraySet, ArrayPush, ArrayPop,
    NewStruct, FieldGet, FieldSet,
    Call, Cast,
    Jump, JumpFalse, JumpTrue,
    Return, ReturnVal,
    Print, PrintEnd, Input, InputInt, InputFloat, ToInt, ToFloat, Exit, Panic
>;

// IR одной функции
struct IRFunction {
    std::string        name;
    int                num_params;
    int                num_locals;  // включая параметры
    TypePtr            ret_type;
    std::vector<Instr> body;
    std::vector<std::string> slot_names; // имена слотов для отладки
};

// IR всей программы
struct IRProgram {
    std::vector<Operand>     global_inits; // начальные значения глобальных переменных
    std::vector<std::string> global_names;
    std::vector<TypePtr>     global_types;
    std::vector<IRFunction>  functions;
    int                      main_idx = -1;
};

// Вспомогательные функции для вывода IR
std::string operand_to_str(const Operand& o);
void dump_ir(const IRProgram& prog, std::ostream& out);

} 
