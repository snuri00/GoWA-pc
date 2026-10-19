cmake_minimum_required(VERSION 3.28)
project(renjob_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(GOWA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(EDGE_DIR ${GOWA_ROOT}/tests/edge)
set(RJ_FLAGS /clang:-march=x86-64-v3 /clang:-ffp-contract=off /clang:-fno-strict-aliasing /clang:-frounding-math /EHsc /O2)

add_library(rj_spu STATIC
    ${GOWA_ROOT}/runtime/src/spu/interpreter.cpp
    ${GOWA_ROOT}/runtime/src/ppu_ops.cpp
    ${GOWA_ROOT}/runtime/src/reservation.cpp
    ${GOWA_ROOT}/runtime/src/memory.cpp
    ${GOWA_ROOT}/runtime/src/log.cpp)
target_include_directories(rj_spu PUBLIC ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(rj_spu PRIVATE ${RJ_FLAGS})

find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB RJ_NATIVE_SRC CONFIGURE_DEPENDS ${GOWA_ROOT}/runtime/src/spu/renjob/*.cpp)
if(RJ_PROF)
    list(REMOVE_ITEM RJ_NATIVE_SRC ${GOWA_ROOT}/runtime/src/spu/renjob/renjob_gen.cpp)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/prof/renjob_gen.cpp ${CMAKE_CURRENT_BINARY_DIR}/prof/renjob_gen.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/prof
        COMMAND ${CMAKE_COMMAND} -E env RJ_PROF=1 ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/rj_cgen.py @${CMAKE_CURRENT_SOURCE_DIR}/roots.txt ${CMAKE_CURRENT_BINARY_DIR}/prof/renjob_gen.cpp ${CMAKE_CURRENT_BINARY_DIR}/prof/renjob_gen.h
        WORKING_DIRECTORY ${GOWA_ROOT}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rj_cgen.py ${CMAKE_CURRENT_SOURCE_DIR}/rj_kernels.py ${CMAKE_CURRENT_SOURCE_DIR}/roots.txt)
    list(APPEND RJ_NATIVE_SRC ${CMAKE_CURRENT_BINARY_DIR}/prof/renjob_gen.cpp)
endif()
add_library(rj_native STATIC ${RJ_NATIVE_SRC})
if(RJ_PROF)
    target_include_directories(rj_native BEFORE PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/prof ${CMAKE_CURRENT_SOURCE_DIR})
endif()
target_include_directories(rj_native PUBLIC ${GOWA_ROOT}/runtime/src/spu/renjob ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(rj_native PRIVATE ${RJ_FLAGS})

set(RJ_GEN_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/rj_cgen.py ${CMAKE_CURRENT_SOURCE_DIR}/rj_kernels.py)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fz_gen.cpp ${CMAKE_CURRENT_BINARY_DIR}/fz_gen.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/rj_cgen.py 0 ${CMAKE_CURRENT_BINARY_DIR}/fz_gen.cpp ${CMAKE_CURRENT_BINARY_DIR}/fz_gen.h renjob::fz table
    DEPENDS ${RJ_GEN_DEPS})
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/op_gen.cpp ${CMAKE_CURRENT_BINARY_DIR}/op_gen.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/rj_cgen.py ops ${CMAKE_CURRENT_BINARY_DIR}/op_gen.cpp ${CMAKE_CURRENT_BINARY_DIR}/op_gen.h
    DEPENDS ${RJ_GEN_DEPS})

add_executable(rj_harness harness.cpp fuzz.cpp hbench.cpp ${CMAKE_CURRENT_BINARY_DIR}/fz_gen.cpp ${CMAKE_CURRENT_BINARY_DIR}/op_gen.cpp ${EDGE_DIR}/capture.cpp ${EDGE_DIR}/unit.cpp)
target_include_directories(rj_harness PRIVATE ${EDGE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(rj_harness PRIVATE rj_native rj_spu)
target_compile_options(rj_harness PRIVATE ${RJ_FLAGS})
target_compile_definitions(rj_harness PRIVATE GOWA_ROOT="${GOWA_ROOT}" _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
