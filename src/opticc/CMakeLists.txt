# compile-cache index: standalone so tests can link it without the coordinator
add_library(opticc_cacheindex STATIC cache_index.cpp)
target_include_directories(opticc_cacheindex PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(opticc_cacheindex PUBLIC opticc_core PRIVATE opticc_warnings)

# header prefetch: standalone for unit tests
add_library(opticc_prefetch STATIC prefetch.cpp)
target_include_directories(opticc_prefetch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(opticc_prefetch PRIVATE opticc_warnings)

add_executable(opticc-client main.cpp coordinator.cpp link_pipeline.cpp ${CMAKE_SOURCE_DIR}/src/opticcd/objcache.cpp)
target_include_directories(opticc-client PRIVATE ${CMAKE_SOURCE_DIR}/src/opticcd)
set_target_properties(opticc-client PROPERTIES OUTPUT_NAME opticc)
target_link_libraries(opticc-client PRIVATE opticc_cacheindex opticc_prefetch opticc_elf opticc_warnings)
