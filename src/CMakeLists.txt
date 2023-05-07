add_executable(main PlaceSafepoints.cpp main.cpp)

target_link_libraries(main ${llvm_libs})