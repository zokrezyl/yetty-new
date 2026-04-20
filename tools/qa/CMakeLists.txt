# QA/Static analysis tools
# These tools require LLVM/Clang development libraries
#
# LLVM/Clang discovery:
#   - In nix shell: LLVM is in PATH, CMake finds it automatically
#   - On system: Set LLVM_DIR and Clang_DIR externally if not in standard paths
#     e.g., cmake -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm -DClang_DIR=/usr/lib/cmake/clang-18

# Only set default system paths if not already provided and not in nix
if(NOT DEFINED LLVM_DIR AND NOT DEFINED ENV{NIX_CC})
    # Try common system locations for Ubuntu/Debian
    foreach(LLVM_VER 21 20 19 18 17 16 15)
        if(EXISTS "/usr/lib/llvm-${LLVM_VER}/lib/cmake/llvm")
            set(LLVM_DIR "/usr/lib/llvm-${LLVM_VER}/lib/cmake/llvm")
            break()
        endif()
    endforeach()
endif()

if(NOT DEFINED Clang_DIR AND NOT DEFINED ENV{NIX_CC})
    # Try common system locations for Ubuntu/Debian
    foreach(LLVM_VER 21 20 19 18 17 16 15)
        if(EXISTS "/usr/lib/cmake/clang-${LLVM_VER}")
            set(Clang_DIR "/usr/lib/cmake/clang-${LLVM_VER}")
            break()
        endif()
    endforeach()
endif()

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)


message(STATUS "QA tools: Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "QA tools: Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "QA tools: Using ClangConfig.cmake in: ${Clang_DIR}")

# result-checker: ensures functions calling result-returning functions also return results
add_executable(result-checker
    result-checker.cpp
)

target_include_directories(result-checker PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)

target_compile_definitions(result-checker PRIVATE
    ${LLVM_DEFINITIONS}
)

target_link_libraries(result-checker PRIVATE
    clangTooling
    clangBasic
    clangFrontend
    clangAST
    clangASTMatchers
)
