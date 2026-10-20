cmake_minimum_required(VERSION 3.28)
project(tk_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(GOWA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(EDGE_DIR ${GOWA_ROOT}/tests/edge)
set(TK_FLAGS /clang:-march=x86-64-v3 /clang:-ffp-contract=off /clang:-fno-strict-aliasing /clang:-frounding-math /EHsc /O2)

add_library(tk_spu STATIC
    ${GOWA_ROOT}/runtime/src/spu/interpreter.cpp
    ${GOWA_ROOT}/runtime/src/ppu_ops.cpp
    ${GOWA_ROOT}/runtime/src/reservation.cpp
    ${GOWA_ROOT}/runtime/src/memory.cpp
    ${GOWA_ROOT}/runtime/src/log.cpp)
target_include_directories(tk_spu PUBLIC ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(tk_spu PRIVATE ${TK_FLAGS})

file(GLOB TK_NATIVE_SRC CONFIGURE_DEPENDS ${GOWA_ROOT}/runtime/src/spu/tasks/*.cpp)
add_library(tk_native STATIC ${TK_NATIVE_SRC})
target_include_directories(tk_native PUBLIC ${GOWA_ROOT}/runtime/src/spu/tasks ${GOWA_ROOT}/runtime/src/spu/pm ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(tk_native PRIVATE ${TK_FLAGS})

add_executable(tk_harness tk_harness.cpp ${EDGE_DIR}/capture.cpp)
target_include_directories(tk_harness PRIVATE ${EDGE_DIR})
target_link_libraries(tk_harness PRIVATE tk_native tk_spu)
target_compile_options(tk_harness PRIVATE ${TK_FLAGS})
target_compile_definitions(tk_harness PRIVATE GOWA_ROOT="${GOWA_ROOT}" _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
