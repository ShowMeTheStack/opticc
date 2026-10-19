add_library(opticc_sim STATIC sim.cpp)
target_include_directories(opticc_sim PUBLIC include)
target_link_libraries(opticc_sim PUBLIC opticc PRIVATE opticc_warnings)

add_executable(opticc-sim main_sim.cpp)
target_link_libraries(opticc-sim PRIVATE opticc_sim opticc_warnings)

add_executable(opticc-gentrace gentrace.cpp)
target_link_libraries(opticc-gentrace PRIVATE opticc opticc_warnings)
