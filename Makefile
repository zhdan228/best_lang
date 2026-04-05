CXX      := g++
CXXFLAGS := -std=c++23 -O2 -Wall -Wextra -Wpedantic -Isrc

SRC := src/main.cpp src/lexer.cpp src/parser.cpp src/semantic.cpp \
       src/lowering.cpp src/optimizer.cpp src/codegen.cpp src/vm.cpp

SRC_VM := src/blvm_main.cpp src/vm.cpp src/codegen.cpp

.PHONY: build run debug clean

build: myc blvm

myc: $(SRC) src/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

blvm: $(SRC_VM) src/vm.hpp src/codegen.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_VM)

run: myc
	./myc examples/hello.bl -o /tmp/hello.blc --run

debug: myc
	./myc examples/hello.bl --dump-ir

clean:
	rm -f myc blvm /tmp/*.blc
