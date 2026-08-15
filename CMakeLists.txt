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

# what a *released* binary is built with. the path above is the build machine's, so a binary someone
# downloaded can only find the standard library if it carries it - see the comment on
# ECO_USE_EMBEDDED_STDLIB in include/eco.h. Off by default: a local build wants the stdlib it can edit,
# and stdlib/build/stdlib_embedded.h is only as fresh as the last `--emit-stdlib-header`
option(ECO_EMBED_STDLIB "Embed the standard library into the echoc binary" OFF)

if(ECO_EMBED_STDLIB)
    target_compile_definitions(${APPNAME} PRIVATE ECO_USE_EMBEDDED_STDLIB=1)
    target_compile_definitions(${LIBNAME} PRIVATE ECO_USE_EMBEDDED_STDLIB=1)
endif()

# the same idea one layer down: a released binary carries its libraries the way it carries the
# standard library. LLVM's export set names zstd::libzstd_shared, which on a build machine with
# Homebrew resolves to an absolute /opt/homebrew path - a path that means nothing on the machine that
# downloaded the binary, where dyld then refuses to start the process at all rather than degrading. So
# a release built the ordinary way is not a slower compiler there, it is no compiler.
#
# off by default for ECO_EMBED_STDLIB's reason: a local build wants what the machine already has, and
# the static archive is not installed everywhere the shared library is
option(ECO_STATIC_ZSTD "Link zstd statically, so the binary carries it rather than naming a path" OFF)

if(ECO_STATIC_ZSTD)
    # rewritten where the dependency is *stated* - in the interface of whichever LLVM library names it
    # - rather than by adding the archive to our own link line, which would leave the dylib on the line
    # as well and change nothing. LLVM ships its own Findzstd.cmake and LLVMConfig resolves
    # find_package(zstd) through it, so the dependency is always a target name and never a path, and
    # the same module defines the static target whenever a libzstd.a is installed
    set(ECO_ZSTD_IS_LINKED FALSE)

    foreach(ECO_LLVM_LIB IN LISTS LLVM_AVAILABLE_LIBS)
        if(NOT TARGET ${ECO_LLVM_LIB})
            continue()
        endif()

        get_target_property(ECO_LLVM_LIB_DEPS ${ECO_LLVM_LIB} INTERFACE_LINK_LIBRARIES)

        if(NOT ECO_LLVM_LIB_DEPS OR NOT "zstd::libzstd_shared" IN_LIST ECO_LLVM_LIB_DEPS)
            continue()
        endif()

        set(ECO_ZSTD_IS_LINKED TRUE)

        if(TARGET zstd::libzstd_static)
            list(TRANSFORM ECO_LLVM_LIB_DEPS REPLACE "^zstd::libzstd_shared$" "zstd::libzstd_static")
            set_property(TARGET ${ECO_LLVM_LIB} PROPERTY INTERFACE_LINK_LIBRARIES "${ECO_LLVM_LIB_DEPS}")
        endif()
    endforeach()

    # an LLVM built without zstd names it nowhere and owes us nothing, so only a dependency we found
    # and could not answer is a failure. It is a hard one: the alternative is a release that carries a
    # dependency on the build machine's filesystem, which is the bug this option exists for
    if(ECO_ZSTD_IS_LINKED AND NOT TARGET zstd::libzstd_static)
        message(FATAL_ERROR
            "ECO_STATIC_ZSTD is on, but LLVM links zstd dynamically and there is no static zstd to "
            "put in its place (zstd_STATIC_LIBRARY is unset). Install one - libzstd-dev on Debian, "
            "brew install zstd on macOS - or configure without ECO_STATIC_ZSTD and accept a binary "
            "that only starts on a machine which already has the shared library.")
    endif()
endif()

# the patch number and the version suffix are CI's to supply, because the patch is a fact about the
# release history rather than about this tree. Both are added only when non-empty, so a plain configure
# leaves include/eco.h's own values alone rather than overriding them with a blank
set(ECO_VERSION_PATCH "" CACHE STRING "Patch version to stamp into the binary. Empty: use include/eco.h's.")
set(ECO_VERSION_SUFFIX "" CACHE STRING "Appended to the version verbatim, e.g. '-dev+abc1234'. Empty for a release.")

if(NOT ECO_VERSION_PATCH STREQUAL "")
    target_compile_definitions(${APPNAME} PRIVATE ECO_VERSION_PATCH=${ECO_VERSION_PATCH})
    target_compile_definitions(${LIBNAME} PRIVATE ECO_VERSION_PATCH=${ECO_VERSION_PATCH})
endif()

if(NOT ECO_VERSION_SUFFIX STREQUAL "")
    target_compile_definitions(${APPNAME} PRIVATE "ECO_VERSION_SUFFIX=\"${ECO_VERSION_SUFFIX}\"")
    target_compile_definitions(${LIBNAME} PRIVATE "ECO_VERSION_SUFFIX=\"${ECO_VERSION_SUFFIX}\"")
endif()

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
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    # Darwin says arm64, Linux says aarch64. both are AArch64 to LLVM
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

# the same LLVM components the compiler links, because the tests link the whole compiler library and any
# archive member they pull in may reach LLVM. This was latent until Compiler::TargetFacts - it reads the
# host triple, and ModuleParser's constructor resolves the host's facts, so every test that builds a parser
# now extracts that member. Deliberately `${LLVM_LIBS}` rather than the one component that happens to be
# missing today: a test binary linking a subset of what the library needs is a link error waiting for the
# next member to become reachable
#
# note this is target_link_libraries and *not* link_directories - see the loud comment at the top of this
# file for why the latter must never be added
target_link_libraries(tests PRIVATE ${LLVM_LIBS})

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
