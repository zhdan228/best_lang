#include "lexer.hpp"
#include "parser.hpp"
#include "semantic.hpp"
#include "lowering.hpp"
#include "optimizer.hpp"
#include "codegen.hpp"
#include "vm.hpp"

#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <cstring>

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "error: cannot open '" << path << "'\n";
        return "";
    }
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static std::string default_output(const std::string& src) {
    std::filesystem::path p(src);
    return (p.parent_path() / p.stem()).string() + ".blc";
}

static void usage(const char* prog) {
    std::cerr << "usage: " << prog
              << " <source.bl> [-o <output.blc>] [--dump-tokens] [--dump-ast] [--dump-ir] [--run]\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    std::string source_path;
    std::string output_path;
    bool dump_tokens = false;
    bool dump_ast    = false;
    bool dump_ir     = false;
    bool run_after   = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-tokens") == 0) dump_tokens = true;
        else if (std::strcmp(argv[i], "--dump-ast") == 0)    dump_ast    = true;
        else if (std::strcmp(argv[i], "--dump-ir")  == 0)    dump_ir     = true;
        else if (std::strcmp(argv[i], "--run")      == 0)    run_after   = true;
        else if (std::strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) { std::cerr << "error: -o requires an argument\n"; return 1; }
            output_path = argv[i];
        } else if (source_path.empty()) {
            source_path = argv[i];
        } else {
            std::cerr << "error: unexpected argument '" << argv[i] << "'\n";
            return 1;
        }
    }

    if (source_path.empty()) { usage(argv[0]); return 1; }
    if (output_path.empty()) output_path = default_output(source_path);

    std::string source = read_file(source_path);
    if (source.empty() && !std::filesystem::exists(source_path)) return 1;

    auto lex_result = Lexer::tokenize(source, source_path);
    if (!lex_result) {
        std::cerr << lex_result.error().format() << "\n";
        return 1;
    }

    if (dump_tokens) {
        Lexer::dump_tokens(lex_result.value(), std::cout);
        return 0;
    }

    auto parse_result = Parser::parse(lex_result.value(), source_path);
    if (!parse_result) {
        std::cerr << parse_result.error().format() << "\n";
        return 1;
    }
    auto& prog = parse_result.value();

    if (dump_ast) {
        Parser::dump_ast(prog, std::cout);
        return 0;
    }

    auto sem = Semantic::analyze(prog, source_path);
    if (!sem.errors.empty()) {
        for (auto& e : sem.errors)
            std::cerr << e.format() << "\n";
        return 1;
    }

    auto ir_prog = IR::lower(prog, sem, source_path);
    IR::optimize(ir_prog);

    if (dump_ir) {
        IR::dump_ir(ir_prog, std::cout);
        return 0;
    }

    auto bytecode = Codegen::compile(ir_prog);
    if (!Codegen::write_blc(bytecode, output_path)) {
        std::cerr << "error: cannot write output file '" << output_path << "'\n";
        return 1;
    }

    std::cerr << "compiled: " << source_path << " -> " << output_path << "\n";

    if (run_after) {
        auto res = VM::run_bytecode(bytecode);
        return res.exit_code;
    }

    return 0;
}
