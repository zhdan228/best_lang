#pragma once
#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <cstdint>

namespace VM {

// Значение в стеке VM
struct ArrayObj;
struct StructObj;

struct Value {
    enum class Kind { Int, UInt, Float, Bool, Str, Array, Struct, Unit };
    Kind kind = Kind::Unit;

    int64_t  i = 0;
    uint64_t u = 0;
    double   f = 0.0;
    bool     b = false;
    std::shared_ptr<std::string>  str;
    std::shared_ptr<ArrayObj>     arr;
    std::shared_ptr<StructObj>    obj;
};

struct ArrayObj  { std::vector<Value> elems; };
struct StructObj { std::vector<Value> fields; };

// Результат выполнения программы
struct VMResult {
    int  exit_code = 0;
    bool ok        = true;
    std::string error_msg; // заполняется при ok=false
};

// Загружает .blc файл и выполняет программу
VMResult run_file(const std::string& path);

// Выполняет байткод из памяти
VMResult run_bytecode(const std::vector<uint8_t>& bc);

} 
