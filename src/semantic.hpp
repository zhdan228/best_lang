#pragma once
#include "ast.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace Semantic {

struct SemanticError {
    std::string filename;
    uint32_t    line;
    uint32_t    col;
    std::string message;

    std::string format() const {
        return filename + ":" + std::to_string(line) + ":" +
               std::to_string(col) + ": error: " + message;
    }
};

// Информация о символе (переменная, функция, структура) в таблице имён
struct Symbol {
    enum class Kind { LocalVar, GlobalVar, Param, Function, StructType, Namespace };
    Kind    kind;
    TypePtr type;           // тип переменной / тип возврата для функций
    bool    is_val     = false; // иммутабельная?
    bool    is_builtin = false; // встроенная функция (пропускаем строгую проверку типов)
    int     slot       = -1;    // индекс слота для локальных/глобальных переменных
    // Для функций:
    std::vector<TypePtr> param_types;
    // Для структур: список полей (индекс = field_idx)
    std::vector<FieldDecl> fields;
};

// Результат семантического анализа
struct AnalysisResult {
    std::vector<SemanticError>   errors;
    // Глобальные переменные в порядке объявления
    std::vector<GlobalVarDecl*>  globals;
    // Функции в порядке объявления
    std::vector<FunDecl*>        functions;
};

AnalysisResult analyze(Program& prog, const std::string& filename);

} 
