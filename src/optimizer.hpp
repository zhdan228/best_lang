#pragma once
#include "ir.hpp"

namespace IR {

// Применяет свёртку констант к IR-программе (изменяет на месте).
// Выражения с двумя константными операндами вычисляются на этапе компиляции.
void optimize(IRProgram& prog);

} 
