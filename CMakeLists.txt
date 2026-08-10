cmake_minimum_required(VERSION 3.20)

project(crepl VERSION 0.1.0 LANGUAGES C CXX)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake from ${LLVM_DIR}")
message(STATUS "Using ClangConfig.cmake from ${Clang_DIR}")

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")

add_executable(crepl crepl.cpp)
target_compile_features(crepl PRIVATE cxx_std_20)
set_target_properties(crepl PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
target_include_directories(crepl SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)
target_compile_definitions(crepl PRIVATE ${LLVM_DEFINITIONS_LIST})

if(TARGET clang-cpp)
    target_link_libraries(crepl PRIVATE clang-cpp)
else()
    find_library(CLANG_CPP_LIBRARY
        NAMES clang-cpp clang-cpp${LLVM_VERSION_MAJOR}
        HINTS ${LLVM_LIBRARY_DIRS}
    )

    if(CLANG_CPP_LIBRARY)
        target_link_libraries(crepl PRIVATE ${CLANG_CPP_LIBRARY})
    else()
        target_link_libraries(crepl PRIVATE
            clangInterpreter
            clangFrontend
            clangAST
            clangBasic
        )
    endif()
endif()

if(TARGET LLVM)
    target_link_libraries(crepl PRIVATE LLVM)
else()
    llvm_map_components_to_libnames(CREPL_LLVM_LIBRARIES
        Core
        ExecutionEngine
        LineEditor
        native
        OrcJIT
        Support
    )
    target_link_libraries(crepl PRIVATE
        ${CREPL_LLVM_LIBRARIES}
        ${LLVM_SYSTEM_LIBS}
    )
endif()

include(CTest)
if(BUILD_TESTING AND UNIX AND NOT APPLE)
    find_program(BASH_EXECUTABLE bash REQUIRED)
    add_test(
        NAME regression
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "CREPL_BINARY=$<TARGET_FILE:crepl>"
            "${BASH_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/regression.sh"
    )
endif()
