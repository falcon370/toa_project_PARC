cmake_minimum_required(VERSION 3.20)
project(ParC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# The compiler phases, one folder each, shared by the parc program and the tests.
# Includes are written from compiler/, e.g. #include "scanner/scanner.hpp".
add_library(parc_core STATIC
    compiler/common/diagnostic.cpp
    compiler/common/json.cpp
    compiler/common/json_writer.cpp
    compiler/scanner/token.cpp
    compiler/scanner/scanner.cpp
    compiler/parser/ast.cpp
    compiler/parser/parser.cpp
    compiler/semantic/symbol_table.cpp
    compiler/semantic/semantic.cpp
    compiler/semantic/flow_checks.cpp
    compiler/semantic/value_analysis.cpp
    compiler/ir/ir.cpp
    compiler/ir/builder.cpp
    compiler/ir/cfg.cpp
    compiler/optimizer/analyses.cpp
    compiler/optimizer/dependence.cpp
    compiler/optimizer/passes_scalar.cpp
    compiler/optimizer/passes_cfg.cpp
    compiler/optimizer/passes_loop.cpp
    compiler/optimizer/optimizer.cpp
    compiler/codegen/codegen.cpp
    compiler/vm/bytecode.cpp
    compiler/vm/vm.cpp
    compiler/metrics/to_c.cpp
    compiler/metrics/measure.cpp
    compiler/metrics/clang_bridge.cpp
    compiler/metrics/complexity.cpp
    compiler/metrics/metrics.cpp
    compiler/cli/pipeline.cpp
)
target_include_directories(parc_core PUBLIC compiler)
if(WIN32)
    target_link_libraries(parc_core PUBLIC pdh psapi)  # the energy counter and process memory
endif()

# The metrics module hooks into an installed Clang/LLVM: libclang for the
# syntax tree and warnings, LLVM's remarks library for the optimizer's
# remarks, clang itself for the native program. Without them ParC still
# builds; "parc metrics" then says what is missing.
set(PARC_LLVM_ROOT "C:/Program Files/LLVM" CACHE PATH "An LLVM installation with libclang (optional)")
find_path(PARC_LIBCLANG_INCLUDE clang-c/Index.h HINTS "${PARC_LLVM_ROOT}/include")
find_library(PARC_LIBCLANG NAMES libclang clang HINTS "${PARC_LLVM_ROOT}/lib")
find_library(PARC_LLVM_REMARKS NAMES Remarks LLVMRemarks HINTS "${PARC_LLVM_ROOT}/lib")
find_program(PARC_CLANG_EXE clang HINTS "${PARC_LLVM_ROOT}/bin")
set(PARC_LLVM_DLLS "")
if(PARC_LIBCLANG_INCLUDE AND PARC_LIBCLANG)
    target_include_directories(parc_core SYSTEM PUBLIC "${PARC_LIBCLANG_INCLUDE}")
    target_link_libraries(parc_core PUBLIC "${PARC_LIBCLANG}")
    target_compile_definitions(parc_core PUBLIC PARC_HAVE_LIBCLANG)
    list(APPEND PARC_LLVM_DLLS "${PARC_LLVM_ROOT}/bin/libclang.dll")
    message(STATUS "ParC metrics: libclang ${PARC_LIBCLANG}")
else()
    message(STATUS "ParC metrics: libclang not found, building without Clang's syntax tree")
endif()
if(PARC_LIBCLANG_INCLUDE AND PARC_LLVM_REMARKS)
    if(NOT EXISTS "${PARC_LIBCLANG_INCLUDE}/llvm-c/ExternC.h")
        # The Windows installer has Remarks.h but not the three headers it includes.
        target_include_directories(parc_core SYSTEM PUBLIC compiler/metrics/llvm_shim)
    endif()
    target_link_libraries(parc_core PUBLIC "${PARC_LLVM_REMARKS}")
    target_compile_definitions(parc_core PUBLIC PARC_HAVE_LLVM_REMARKS)
    list(APPEND PARC_LLVM_DLLS "${PARC_LLVM_ROOT}/bin/Remarks.dll")
endif()
if(PARC_CLANG_EXE)
    target_compile_definitions(parc_core PUBLIC PARC_CLANG_EXE="${PARC_CLANG_EXE}")
    # libclang finds its own headers (stddef.h, ...) next to the DLL; the copy below is elsewhere, so tell it.
    execute_process(COMMAND "${PARC_CLANG_EXE}" -print-resource-dir OUTPUT_VARIABLE PARC_CLANG_RESOURCE_DIR
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(PARC_CLANG_RESOURCE_DIR)
        file(TO_CMAKE_PATH "${PARC_CLANG_RESOURCE_DIR}" PARC_CLANG_RESOURCE_DIR)
        target_compile_definitions(parc_core PUBLIC PARC_CLANG_RESOURCE_DIR="${PARC_CLANG_RESOURCE_DIR}")
    endif()
endif()

add_executable(parc compiler/cli/main.cpp)
target_link_libraries(parc PRIVATE parc_core)
# The DLLs go next to parc.exe and the tests, which are in the same folder.
foreach(dll ${PARC_LLVM_DLLS})
    if(EXISTS "${dll}")
        add_custom_command(TARGET parc POST_BUILD
                           COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:parc>")
    endif()
endforeach()

# One test program per phase, in that phase's tests/ folder.
enable_testing()
function(parc_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE parc_core)
    target_compile_definitions(${name} PRIVATE PARC_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

parc_test(test_scanner compiler/scanner/tests/test_scanner.cpp)
parc_test(test_parser compiler/parser/tests/test_parser.cpp)
parc_test(test_semantic compiler/semantic/tests/test_semantic.cpp)
parc_test(test_flow compiler/semantic/tests/test_flow.cpp)
parc_test(test_ir compiler/ir/tests/test_ir.cpp)
parc_test(test_optimizer compiler/optimizer/tests/test_optimizer.cpp)
parc_test(test_codegen compiler/codegen/tests/test_codegen.cpp)
parc_test(test_vm compiler/vm/tests/test_vm.cpp)
parc_test(test_metrics compiler/metrics/tests/test_metrics.cpp)
add_dependencies(test_metrics parc)  # for the Clang DLLs copied next to parc.exe
