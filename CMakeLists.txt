cmake_minimum_required(VERSION 3.20)

set(APPNAME echoc)
project(${APPNAME})

set(LIBNAME ${APPNAME}_lib)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -g")
set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fsanitize=address")

set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
# set(CMAKE_BUILD_TYPE Debug)

set(VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")

message(STATUS "Using compiler: ${CMAKE_CXX_COMPILER}")

# LLVM
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

# DO NOT INCLUDE! THIS CAUSED AN ERROR THAT TOOK ME 3 FULL DAYS TO PIN DOWN
# I leave this here as a warning to future me and as a totem to ward off evil spirits
# honestly I do not even rember why I added this in the first place, it in combination 
# with some other stupid mistakes of mine caused a segfault / bad access in the libunwind on ARM64 Mac's 
# on Intel Mac's it worked fine which making this an absolute nightmare to debug
# link_directories(${LLVM_LIBRARY_DIRS}) 

# Echo 
file(GLOB_RECURSE SOURCES "src/*.cpp")
set_source_files_properties(${SOURCES} PROPERTIES COMPILE_FLAGS "-Wall -Wextra -pedantic -Wno-unused-parameter")

add_executable(${APPNAME} ${SOURCES})
add_library(${LIBNAME} STATIC ${SOURCES})

# where the stdlib sources live. PRIVATE on each target that needs it rather than PUBLIC on the
# library, because a consumer of echoc_lib is not thereby a consumer of the source tree - so the three
# targets that do need it name it, and this variable is the one place the path is spelled
set(ECO_STDLIB_DEFINITION "STDLIB_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}/stdlib\"")

target_compile_definitions(${APPNAME} PRIVATE ${ECO_STDLIB_DEFINITION})
target_compile_definitions(${LIBNAME} PRIVATE ${ECO_STDLIB_DEFINITION})

# create an install target
install(TARGETS ${APPNAME} DESTINATION bin)

message(STATUS "AVAIL COMPONENETS: ${LLVM_AVAILABLE_LIBS}")
message(STATUS "LLVM_TARGETS_TO_BUILD: ${LLVM_TARGETS_TO_BUILD}")
set(LLVM_COMPONENTS
    #AArch64
    # ARM
    Core
    ExecutionEngine
    IRReader
    MCJIT
    Support
    Target
    Analysis
    Passes
)

# add the native architecture to the list of components
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(NATIVE_ARCH "X86")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    set(NATIVE_ARCH "AArch64")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    set(NATIVE_ARCH "ARM")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
    set(NATIVE_ARCH "RISCV")
else()
    message(FATAL_ERROR "Unknown architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

message(STATUS "NATIVE_ARCH: ${NATIVE_ARCH}")
list(APPEND LLVM_COMPONENTS ${NATIVE_ARCH})

llvm_map_components_to_libnames(LLVM_LIBS ${LLVM_COMPONENTS})
# llvm_map_components_to_libnames(LLVM_LIBS support core irreader mcjit native)
# llvm_map_components_to_libnames(LLVM_LIBS ${LLVM_TARGETS_TO_BUILD} mcjit)
target_link_libraries(${APPNAME} ${LLVM_LIBS})

include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/stdlib/build)
target_include_directories(${LIBNAME} PUBLIC ${CMAKE_SOURCE_DIR}/include)

# vendor
add_subdirectory(vendor)

# Unit tests
Include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.6.0
)

FetchContent_MakeAvailable(Catch2)

file(GLOB TEST_SOURCES "tests/*.cpp")

add_executable(tests ${TEST_SOURCES})
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain)
target_link_libraries(tests PRIVATE ${LIBNAME})

# the e2e corpus spawns its ~400 echoc subprocesses from a worker pool - the assertions that judge
# them stay on the main thread, but the spawns do not
find_package(Threads REQUIRED)
target_link_libraries(tests PRIVATE Threads::Threads)

# e2e (tests_eco): always run against a freshly built compiler and tell the runner
# where the binary and the corpus live
add_dependencies(tests ${APPNAME})
#
# ECO_E2E_TMP_DIR is the scratch root: one directory per case under it, holding that case's binary, the
# objects emitted beside it and its module cache. inside the build tree and absolute, because the tests
# binary's working directory is not fixed (ctest and CI run it from `build/`, a developer runs
# `./build/tests` from the repo root), so a relative path is wrong in one of the two
#
# the stdlib definition is PRIVATE on the other two targets, so linking the library does not carry it
# here. The tests need it to assert that nothing is written into the compiler's own source tree
target_compile_definitions(tests PRIVATE
    "ECHOC_BINARY=\"$<TARGET_FILE:${APPNAME}>\""
    "ECO_E2E_TESTS_DIR=\"${CMAKE_SOURCE_DIR}/tests_eco\""
    "ECO_E2E_TMP_DIR=\"${CMAKE_BINARY_DIR}/e2e_tmp\""
    ${ECO_STDLIB_DEFINITION})

# ctest integration
enable_testing()
list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(tests)
