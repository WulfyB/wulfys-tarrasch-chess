cmake_minimum_required(VERSION 3.16)
project(wolf_eval CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Tarrasch's chess rules library lives next door in ../src
set(TARRASCH_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_library(wolfcore STATIC
    core/accuracy.cpp core/pgn.cpp core/uci_engine.cpp core/analysis.cpp
    ${TARRASCH_SRC}/thc.cpp)
target_include_directories(wolfcore PUBLIC core ${TARRASCH_SRC})
if (MSVC)
    target_compile_options(wolfcore PUBLIC /utf-8 /FIcstdint /FIstring)
    target_compile_definitions(wolfcore PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(wolfcore PUBLIC "SHELL:-include cstdint" "SHELL:-include string")
endif()

add_executable(wolf-eval cli/main.cpp)
target_link_libraries(wolf-eval wolfcore)
if (MSVC)
    set_property(TARGET wolf-eval wolfcore PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")  # single .exe, no DLLs
endif()

enable_testing()
add_executable(wolf-tests tests/test_main.cpp)
target_link_libraries(wolf-tests wolfcore)
add_test(NAME wolf-tests COMMAND wolf-tests)
