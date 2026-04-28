#pragma once
#include "ast.hpp"
#include "ir.hpp"
#include "semantic.hpp"
#include <string>
#include <vector>

namespace IR {

// Переводит семантически проверенный AST в трёхадресный IR.
IRProgram lower(Program& prog,
                const Semantic::AnalysisResult& sem,
                const std::string& filename);

} 
