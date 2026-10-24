cmake_minimum_required(VERSION 3.28)
project(pm_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(GOWA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GOWA_GEN "${GOWA_ROOT}/out/gen/spu" CACHE PATH "Generated native SPU sources (tools/setup.py)")
set(EDGE_DIR ${GOWA_ROOT}/tests/edge)
set(PM_FLAGS /clang:-march=x86-64-v3 /clang:-ffp-contract=off /clang:-fno-strict-aliasing /clang:-frounding-math /EHsc /O2)

add_library(pm_spu STATIC
    ${GOWA_ROOT}/runtime/src/spu/interpreter.cpp
    ${GOWA_ROOT}/runtime/src/ppu_ops.cpp
    ${GOWA_ROOT}/runtime/src/reservation.cpp
    ${GOWA_ROOT}/runtime/src/memory.cpp
    ${GOWA_ROOT}/runtime/src/log.cpp)
target_include_directories(pm_spu PUBLIC ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(pm_spu PRIVATE ${PM_FLAGS})

file(GLOB PM_NATIVE_SRC CONFIGURE_DEPENDS ${GOWA_ROOT}/runtime/src/spu/pm/*.cpp ${GOWA_ROOT}/runtime/src/spu/edgejob/*.cpp ${GOWA_ROOT}/runtime/src/spu/meshhier/*.cpp ${GOWA_ROOT}/runtime/src/spu/anim/*.cpp ${GOWA_GEN}/pm/*.cpp ${GOWA_GEN}/edgejob/*.cpp ${GOWA_GEN}/meshhier/*.cpp ${GOWA_GEN}/anim/*.cpp ${GOWA_ROOT}/runtime/src/spu/mpjob/*.cpp ${GOWA_GEN}/mpjob/*.cpp)
add_library(pm_native STATIC ${PM_NATIVE_SRC})
target_include_directories(pm_native PUBLIC ${GOWA_GEN}/pm ${GOWA_GEN}/edgejob ${GOWA_GEN}/meshhier ${GOWA_GEN}/anim ${GOWA_GEN}/mpjob ${GOWA_ROOT}/runtime/src/spu/pm ${GOWA_ROOT}/runtime/src/spu/edgejob ${GOWA_ROOT}/runtime/src/spu/meshhier ${GOWA_ROOT}/runtime/src/spu/anim ${GOWA_ROOT}/runtime/src/spu/mpjob ${GOWA_ROOT}/runtime/include ${GOWA_ROOT}/out/include)
target_compile_options(pm_native PRIVATE ${PM_FLAGS})

add_executable(pm_harness harness.cpp ${EDGE_DIR}/capture.cpp)
target_include_directories(pm_harness PRIVATE ${EDGE_DIR})
target_link_libraries(pm_harness PRIVATE pm_native pm_spu)
target_compile_options(pm_harness PRIVATE ${PM_FLAGS})
target_compile_definitions(pm_harness PRIVATE GOWA_ROOT="${GOWA_ROOT}" _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
