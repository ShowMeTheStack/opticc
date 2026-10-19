add_executable(opticc-elf-tests
  ${CMAKE_SOURCE_DIR}/tests/test_main.cpp
  test_elf.cpp
  test_liveness.cpp
  test_prelink.cpp
  ${CMAKE_SOURCE_DIR}/tests/domains/test_partition.cpp
  ${CMAKE_SOURCE_DIR}/tests/domains/test_domainlink.cpp)
target_include_directories(opticc-elf-tests PRIVATE ${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/tests/elf)
target_link_libraries(opticc-elf-tests PRIVATE opticc_elf opticc_warnings)
target_compile_definitions(opticc-elf-tests PRIVATE OPTICC_PRELINK_BIN="$<TARGET_FILE:opticc-prelink>" OPTICC_DOMAINS_BIN="$<TARGET_FILE:opticc-domains>" OPTICC_BIN="$<TARGET_FILE:opticc-client>" OPTICCD_BIN="$<TARGET_FILE:opticcd>" OPTICC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set(OPTICC_ELF_TEST_NAMES
  elf_roundtrip_content_identical
  elf_roundtrip_links_identically
  manifest_classifies_relocs
  liveness_matches_lld_gc_sections
  liveness_comdat_and_start_stop
  liveness_condensed_equals_full
  liveness_wire_roundtrip
  prelink_residual_equals_cut
  prelink_differential_20_seeds
  prelink_fails_loudly
  prelink_cli_smoke
  partition_mincut_beats_random
  domainlink_e2e)
foreach(t ${OPTICC_ELF_TEST_NAMES})
  add_test(NAME ${t} COMMAND opticc-elf-tests ${t} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(${t} PROPERTIES TIMEOUT 600)
endforeach()
