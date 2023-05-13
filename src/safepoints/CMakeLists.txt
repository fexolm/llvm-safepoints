add_executable(safepoints-example PlaceSafepoints.cpp main.cpp)

target_link_libraries(safepoints-example ${llvm_libs})