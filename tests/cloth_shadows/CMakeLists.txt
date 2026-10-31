cmake_minimum_required(VERSION 3.28)
project(cloth_shadows_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(GOWA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GOWA_GEN "${GOWA_ROOT}/out/gen/spu" CACHE PATH "Generated native SPU sources (tools/setup.py)")
set(EDGE_DIR ${GOWA_ROOT}/tests/edge)
if(MSVC)
    set(CS_FLAGS /clang:-march=x86-64-v3 /clang:-ffp-contract=off /clang:-fno-strict-aliasing /clang:-frounding-math /EHsc /O2)
else()
    set(CS_FLAGS -march=x86-64-v3 -ffp-contract=off -fno-strict-aliasing -frounding-math -O2)
endif()

add_library(cs_spu STATIC
    ${GOWA_ROOT}/runtime/src/spu/interpreter.cpp
    ${GOWA_ROOT}/runtime/src/ppu_ops.cpp
    ${GOWA_ROOT}/runtime/src/reservation.cpp
    ${GOWA_ROOT}/runtime/src/memory.cpp
    ${GOWA_ROOT}/runtime/src/log.cpp
    ${GOWA_ROOT}/runtime/src/host.cpp)
target_include_directories(cs_spu PUBLIC ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(cs_spu PRIVATE ${CS_FLAGS})

file(GLOB CS_NATIVE_SRC CONFIGURE_DEPENDS ${GOWA_ROOT}/runtime/src/spu/cloth/*.cpp ${GOWA_ROOT}/runtime/src/spu/shadows/*.cpp ${GOWA_GEN}/cloth/*.cpp ${GOWA_GEN}/shadows/*.cpp)
add_library(cs_native STATIC ${CS_NATIVE_SRC})
target_include_directories(cs_native PUBLIC ${GOWA_ROOT}/runtime/src/spu ${GOWA_ROOT}/runtime/src/spu/cloth ${GOWA_ROOT}/runtime/src/spu/shadows ${GOWA_GEN}/cloth ${GOWA_GEN}/shadows ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(cs_native PRIVATE ${CS_FLAGS})

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CS_GEN_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/cs_cgen.py ${CMAKE_CURRENT_SOURCE_DIR}/cs_kernels.py ${GOWA_ROOT}/tests/renjob/rj_cgen.py)
foreach(JOB cloth shadows)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fz_${JOB}.cpp ${CMAKE_CURRENT_BINARY_DIR}/fz_${JOB}.h
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cs_cgen.py ${JOB} fuzz ${CMAKE_CURRENT_BINARY_DIR}/fz_${JOB}.cpp ${CMAKE_CURRENT_BINARY_DIR}/fz_${JOB}.h
        DEPENDS ${CS_GEN_DEPS})
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/op_${JOB}.cpp ${CMAKE_CURRENT_BINARY_DIR}/op_${JOB}.h
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cs_cgen.py ${JOB} ops ${CMAKE_CURRENT_BINARY_DIR}/op_${JOB}.cpp ${CMAKE_CURRENT_BINARY_DIR}/op_${JOB}.h
        DEPENDS ${CS_GEN_DEPS})
endforeach()

add_executable(cs_harness harness.cpp fuzz.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/fz_cloth.cpp ${CMAKE_CURRENT_BINARY_DIR}/fz_shadows.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/op_cloth.cpp ${CMAKE_CURRENT_BINARY_DIR}/op_shadows.cpp
    ${EDGE_DIR}/capture.cpp ${EDGE_DIR}/unit.cpp)
target_include_directories(cs_harness PRIVATE ${EDGE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${GOWA_ROOT}/runtime/src/spu)
target_link_libraries(cs_harness PRIVATE cs_native cs_spu)
target_compile_options(cs_harness PRIVATE ${CS_FLAGS})
target_compile_definitions(cs_harness PRIVATE GOWA_ROOT="${GOWA_ROOT}" _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
