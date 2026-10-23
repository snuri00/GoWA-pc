cmake_minimum_required(VERSION 3.28)
project(edge_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(GOWA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GOWA_GEN "${GOWA_ROOT}/out/gen/spu" CACHE PATH "Generated native SPU sources (tools/setup.py)")
set(EDGE_FLAGS /clang:-march=x86-64-v3 /clang:-ffp-contract=off /clang:-fno-strict-aliasing /clang:-frounding-math /EHsc /O2)

add_library(edge_spu STATIC
    ${GOWA_ROOT}/runtime/src/spu/interpreter.cpp
    ${GOWA_ROOT}/runtime/src/ppu_ops.cpp
    ${GOWA_ROOT}/runtime/src/reservation.cpp
    ${GOWA_ROOT}/runtime/src/memory.cpp
    ${GOWA_ROOT}/runtime/src/log.cpp)
target_include_directories(edge_spu PUBLIC ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(edge_spu PRIVATE ${EDGE_FLAGS})

file(GLOB EDGE_NATIVE_SRC CONFIGURE_DEPENDS ${GOWA_ROOT}/runtime/src/spu/edge/*.cpp ${GOWA_GEN}/edge/*.cpp)
file(GLOB EDGE_ADAPTER_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/nat_*.cpp)
add_library(edge_native STATIC ${EDGE_NATIVE_SRC})
target_include_directories(edge_native PUBLIC ${GOWA_ROOT}/runtime/src/spu/edge ${GOWA_GEN}/edge ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(edge_native PRIVATE ${EDGE_FLAGS})

add_executable(edge_harness harness.cpp capture.cpp unit.cpp ${EDGE_ADAPTER_SRC})
target_link_libraries(edge_harness PRIVATE edge_native edge_spu)
target_compile_options(edge_harness PRIVATE ${EDGE_FLAGS})
target_compile_definitions(edge_harness PRIVATE GOWA_ROOT="${GOWA_ROOT}" _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
