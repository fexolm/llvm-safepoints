cmake_minimum_required(VERSION 3.2)

project(llvm-playground)

set(CMAKE_CXX_STANDARD 17)

find_package(LLVM REQUIRED 15 CONFIG)

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})

llvm_map_components_to_libnames(llvm_libs 
    Core
    ExecutionEngine
    Interpreter
	OrcJit
    Support
    nativecodegen
    codegen
    X86
)

add_subdirectory(src)