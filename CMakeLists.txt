cmake_minimum_required(VERSION 3.28)
project(GOWA_pc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(GOWA_GENERATED_DIR "${CMAKE_SOURCE_DIR}/out/ppu" CACHE PATH "Directory with generated PPU sources")
set(GOWA_GENERATED_OPT "-O1" CACHE STRING "Optimization level for generated code")

set(GOWA_ARCH_FLAGS /clang:-march=x86-64-v3 /clang:-ffp-contract=off /clang:-fno-strict-aliasing)

add_library(gowa_ppu_ops STATIC runtime/src/ppu_ops.cpp)
target_include_directories(gowa_ppu_ops PUBLIC runtime/include "${CMAKE_SOURCE_DIR}/out/include")
target_compile_options(gowa_ppu_ops PRIVATE ${GOWA_ARCH_FLAGS} /EHsc)

file(GLOB GOWA_RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/src/*.cpp runtime/src/lv2/*.cpp runtime/src/hle/*.cpp)
list(REMOVE_ITEM GOWA_RUNTIME_SOURCES "${CMAKE_SOURCE_DIR}/runtime/src/ppu_ops.cpp" "${CMAKE_SOURCE_DIR}/runtime/src/main.cpp")
add_library(gowa_runtime OBJECT ${GOWA_RUNTIME_SOURCES})
target_link_libraries(gowa_runtime PUBLIC gowa_ppu_ops)
target_compile_options(gowa_runtime PRIVATE ${GOWA_ARCH_FLAGS} /EHsc)

file(GLOB GOWA_GENERATED_SOURCES CONFIGURE_DEPENDS "${GOWA_GENERATED_DIR}/*_[0-9][0-9][0-9].cpp")
add_library(gowa_ppu STATIC ${GOWA_GENERATED_SOURCES} "${GOWA_GENERATED_DIR}/table.cpp")
target_include_directories(gowa_ppu PRIVATE "${GOWA_GENERATED_DIR}")
target_link_libraries(gowa_ppu PUBLIC gowa_ppu_ops)
target_compile_options(gowa_ppu PRIVATE ${GOWA_ARCH_FLAGS} /clang:${GOWA_GENERATED_OPT} /GR- /EHs-c- /clang:-Wno-unused-label /clang:-Wno-unused-variable)
set_source_files_properties(${GOWA_GENERATED_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

add_executable(gowa runtime/src/main.cpp $<TARGET_OBJECTS:gowa_runtime>)
target_link_libraries(gowa PRIVATE gowa_ppu gowa_ppu_ops)
target_include_directories(gowa PRIVATE runtime/include)
target_compile_options(gowa PRIVATE ${GOWA_ARCH_FLAGS} /EHsc)

if(EXISTS "${CMAKE_SOURCE_DIR}/out/conformance/tests.cpp")
    add_executable(ppu_conformance tests/ppu_conformance/harness.cpp "${CMAKE_SOURCE_DIR}/out/conformance/tests.cpp")
    target_include_directories(ppu_conformance PRIVATE tests/ppu_conformance)
    target_link_libraries(ppu_conformance PRIVATE gowa_ppu_ops)
    target_compile_options(ppu_conformance PRIVATE ${GOWA_ARCH_FLAGS} /clang:${GOWA_GENERATED_OPT})
endif()
