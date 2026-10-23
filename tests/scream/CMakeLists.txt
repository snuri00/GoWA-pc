cmake_minimum_required(VERSION 3.28)
project(scream_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(GOWA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GOWA_GEN "${GOWA_ROOT}/out/gen/spu" CACHE PATH "Generated native SPU sources (tools/setup.py)")
set(EDGE_DIR ${GOWA_ROOT}/tests/edge)
set(SC_FLAGS /clang:-march=x86-64-v3 /clang:-ffp-contract=off /clang:-fno-strict-aliasing /clang:-frounding-math /EHsc /O2)

add_library(sc_spu STATIC
    ${GOWA_ROOT}/runtime/src/spu/interpreter.cpp
    ${GOWA_ROOT}/runtime/src/ppu_ops.cpp
    ${GOWA_ROOT}/runtime/src/reservation.cpp
    ${GOWA_ROOT}/runtime/src/memory.cpp
    ${GOWA_ROOT}/runtime/src/log.cpp)
target_include_directories(sc_spu PUBLIC ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(sc_spu PRIVATE ${SC_FLAGS})

find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB SC_NATIVE_SRC CONFIGURE_DEPENDS ${GOWA_ROOT}/runtime/src/spu/scream/*.cpp ${GOWA_GEN}/scream/*.cpp)
add_library(sc_native STATIC ${SC_NATIVE_SRC})
target_include_directories(sc_native PUBLIC ${GOWA_ROOT}/runtime/src/spu/scream ${GOWA_GEN}/scream ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(sc_native PRIVATE ${SC_FLAGS})

set(SC_GEN_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/sc_cgen.py ${CMAKE_CURRENT_SOURCE_DIR}/sc_indirect.py ${CMAKE_CURRENT_SOURCE_DIR}/sc_kernels.py ${GOWA_ROOT}/tests/renjob/rj_cgen.py)
set(SC_GEN_SRC)
foreach(img mp3 aux)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fz_${img}.cpp ${CMAKE_CURRENT_BINARY_DIR}/fz_${img}.h
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/sc_cgen.py ${img} 0 ${CMAKE_CURRENT_BINARY_DIR}/fz_${img}.cpp ${CMAKE_CURRENT_BINARY_DIR}/fz_${img}.h renjob::xo::scream_${img}::fz table
        WORKING_DIRECTORY ${GOWA_ROOT}
        DEPENDS ${SC_GEN_DEPS})
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/op_${img}.cpp ${CMAKE_CURRENT_BINARY_DIR}/op_${img}.h
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/sc_cgen.py ${img} ops ${CMAKE_CURRENT_BINARY_DIR}/op_${img}.cpp ${CMAKE_CURRENT_BINARY_DIR}/op_${img}.h
        WORKING_DIRECTORY ${GOWA_ROOT}
        DEPENDS ${SC_GEN_DEPS})
    list(APPEND SC_GEN_SRC ${CMAKE_CURRENT_BINARY_DIR}/fz_${img}.cpp ${CMAKE_CURRENT_BINARY_DIR}/op_${img}.cpp)
endforeach()

add_executable(sc_harness harness.cpp images.cpp ext.cpp fuzz.cpp ${SC_GEN_SRC} ${EDGE_DIR}/capture.cpp ${EDGE_DIR}/unit.cpp)
target_include_directories(sc_harness PRIVATE ${EDGE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(sc_harness PRIVATE sc_native sc_spu)
target_compile_options(sc_harness PRIVATE ${SC_FLAGS})
target_compile_definitions(sc_harness PRIVATE GOWA_ROOT="${GOWA_ROOT}" _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
