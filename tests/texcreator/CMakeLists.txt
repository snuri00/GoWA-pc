cmake_minimum_required(VERSION 3.28)
project(texcreator_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(GOWA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(TC_FLAGS /clang:-march=x86-64-v3 /clang:-ffp-contract=off /clang:-fno-strict-aliasing /clang:-frounding-math /EHsc /O2)

add_library(tc_spu STATIC
    ${GOWA_ROOT}/runtime/src/spu/interpreter.cpp
    ${GOWA_ROOT}/runtime/src/ppu_ops.cpp
    ${GOWA_ROOT}/runtime/src/reservation.cpp
    ${GOWA_ROOT}/runtime/src/memory.cpp
    ${GOWA_ROOT}/runtime/src/log.cpp)
target_include_directories(tc_spu PUBLIC ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(tc_spu PRIVATE ${TC_FLAGS})

add_library(texcreator_native STATIC texcreator_native.cpp)
target_include_directories(texcreator_native PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(texcreator_native PRIVATE ${TC_FLAGS})

add_executable(tc_harness harness.cpp)
target_link_libraries(tc_harness PRIVATE texcreator_native tc_spu)
target_compile_options(tc_harness PRIVATE ${TC_FLAGS})
target_compile_definitions(tc_harness PRIVATE GOWA_ROOT="${GOWA_ROOT}" _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
