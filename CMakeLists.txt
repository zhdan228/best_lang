cmake_minimum_required(VERSION 3.20)
project(BestLang CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(myc
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/semantic.cpp
    src/lowering.cpp
    src/optimizer.cpp
    src/codegen.cpp
    src/vm.cpp
)

target_include_directories(myc PRIVATE src)

add_executable(blvm
    src/blvm_main.cpp
    src/vm.cpp
    src/codegen.cpp   
)

target_include_directories(blvm PRIVATE src)

add_custom_target(build DEPENDS myc blvm)
add_custom_target(run
    COMMAND myc ${CMAKE_SOURCE_DIR}/examples/hello.bl -o /tmp/hello.blc --run
    DEPENDS myc
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(debug
    COMMAND myc ${CMAKE_SOURCE_DIR}/examples/hello.bl --dump-ir
    DEPENDS myc
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
