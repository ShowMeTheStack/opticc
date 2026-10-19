# closure-path microbenchmark (not a ctest; run by hand, usage in bench_closure.cpp)
add_executable(opticc-bench-closure bench_closure.cpp incscan_head.cpp)
target_include_directories(opticc-bench-closure PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/third_party)
target_link_libraries(opticc-bench-closure PRIVATE opticc_core)
