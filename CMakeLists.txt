cmake_minimum_required(VERSION 3.20)

set(APPNAME echo)
project(${APPNAME})

set(LIBNAME ${APPNAME}_lib)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")

message(STATUS "Using compiler: ${CMAKE_CXX_COMPILER}")

# LLVM
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})

# Echo 
file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(${APPNAME} ${SOURCES})
add_library(${LIBNAME} STATIC ${SOURCES})

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
target_include_directories(${LIBNAME} PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Unit tests
Include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.4.0
)

FetchContent_MakeAvailable(Catch2)

file(GLOB TEST_SOURCES "tests/*.cpp")

add_executable(tests ${TEST_SOURCES})
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain)
target_link_libraries(tests PRIVATE ${LIBNAME})