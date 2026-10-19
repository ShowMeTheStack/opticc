add_executable(opticcd main.cpp objcache.cpp)
target_link_libraries(opticcd PRIVATE opticc_core opticc_elf opticc_warnings)
