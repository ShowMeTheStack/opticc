# Linker-domain components: ELF model, manifests, liveness, prelink, partitioner.
add_library(opticc_elf STATIC
  ${CMAKE_SOURCE_DIR}/src/libopticc/src/elf/elf.cpp
  ${CMAKE_SOURCE_DIR}/src/libopticc/src/elf/manifest.cpp
  ${CMAKE_SOURCE_DIR}/src/libopticc/src/elf/liveness.cpp
  ${CMAKE_SOURCE_DIR}/src/libopticc/src/elf/prelink.cpp
  ${CMAKE_SOURCE_DIR}/src/libopticc/src/elf/wire.cpp
  ${CMAKE_SOURCE_DIR}/src/libopticc/src/domains/partition.cpp)
target_include_directories(opticc_elf PUBLIC ${CMAKE_SOURCE_DIR}/src/libopticc/include)
target_link_libraries(opticc_elf PUBLIC nlohmann_json PRIVATE opticc_warnings)

add_executable(opticc-prelink opticc-prelink/main.cpp)
target_link_libraries(opticc-prelink PRIVATE opticc_elf opticc_warnings)
add_executable(opticc-domains opticc-domains/main.cpp)
target_link_libraries(opticc-domains PRIVATE opticc_elf opticc_warnings)

# replays the fresh-client rdirect validation over a real source tree (tests/perf/bench_rdirect_fresh.sh)
add_executable(opticc-rdirect-bench opticc-rdirect-bench/main.cpp)
target_link_libraries(opticc-rdirect-bench PRIVATE opticc_core opticc_warnings)
