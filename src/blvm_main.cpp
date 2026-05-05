#include "vm.hpp"
#include <iostream>
#include <cstring>

// Автономный исполнитель байткода:blvm <файл.blc>
int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: blvm <file.blc>\n";
        return 1;
    }
    auto res = VM::run_file(argv[1]);
    return res.exit_code;
}
