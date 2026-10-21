cmake_minimum_required(VERSION 3.28)
project(meshprep_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(GOWA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(EDGE_DIR ${GOWA_ROOT}/tests/edge)
set(RJ_DIR ${GOWA_ROOT}/tests/renjob)
set(MP_FLAGS /clang:-march=x86-64-v3 /clang:-ffp-contract=off /clang:-fno-strict-aliasing /clang:-frounding-math /EHsc /O2)

add_library(mp_spu STATIC
    ${GOWA_ROOT}/runtime/src/spu/interpreter.cpp
    ${GOWA_ROOT}/runtime/src/ppu_ops.cpp
    ${GOWA_ROOT}/runtime/src/reservation.cpp
    ${GOWA_ROOT}/runtime/src/memory.cpp
    ${GOWA_ROOT}/runtime/src/log.cpp)
target_include_directories(mp_spu PUBLIC ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(mp_spu PRIVATE ${MP_FLAGS})

find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB MP_NATIVE_SRC CONFIGURE_DEPENDS ${GOWA_ROOT}/runtime/src/spu/meshprep/*.cpp)
add_library(mp_native STATIC ${MP_NATIVE_SRC})
target_include_directories(mp_native PUBLIC ${GOWA_ROOT}/runtime/src/spu/meshprep ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(mp_native PRIVATE ${MP_FLAGS})

set(MP_GEN_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/mp_cgen.py ${CMAKE_CURRENT_SOURCE_DIR}/mp_jobs.py ${CMAKE_CURRENT_SOURCE_DIR}/mp_kernels.py ${RJ_DIR}/rj_cgen.py)
set(MP_GEN_SRC)
foreach(pair meshprep:mp primbuilder:pb groupvis:gv sortprim:sp)
    string(REPLACE ":" ";" pl ${pair})
    list(GET pl 0 job)
    list(GET pl 1 short)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fz_gen_${job}.cpp ${CMAKE_CURRENT_BINARY_DIR}/fz_gen_${job}.h
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/mp_cgen.py ${job} 0 ${CMAKE_CURRENT_BINARY_DIR}/fz_gen_${job}.cpp ${CMAKE_CURRENT_BINARY_DIR}/fz_gen_${job}.h renjob::xo::fz_${short} table
        WORKING_DIRECTORY ${GOWA_ROOT}
        DEPENDS ${MP_GEN_DEPS})
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/op_gen_${job}.cpp ${CMAKE_CURRENT_BINARY_DIR}/op_gen_${job}.h
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/mp_cgen.py ${job} ops ${CMAKE_CURRENT_BINARY_DIR}/op_gen_${job}.cpp ${CMAKE_CURRENT_BINARY_DIR}/op_gen_${job}.h
        WORKING_DIRECTORY ${GOWA_ROOT}
        DEPENDS ${MP_GEN_DEPS})
    list(APPEND MP_GEN_SRC ${CMAKE_CURRENT_BINARY_DIR}/fz_gen_${job}.cpp ${CMAKE_CURRENT_BINARY_DIR}/op_gen_${job}.cpp)
endforeach()

add_executable(mp_harness harness.cpp fuzz.cpp hbench.cpp robust.cpp ${MP_GEN_SRC} ${EDGE_DIR}/capture.cpp ${EDGE_DIR}/unit.cpp)
target_include_directories(mp_harness PRIVATE ${EDGE_DIR} ${RJ_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(mp_harness PRIVATE mp_native mp_spu)
target_compile_options(mp_harness PRIVATE ${MP_FLAGS})
target_compile_definitions(mp_harness PRIVATE GOWA_ROOT="${GOWA_ROOT}" _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
