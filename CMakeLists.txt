cmake_minimum_required(VERSION 3.20)

set(APPNAME echo)
project(${APPNAME})

set(LIBNAME ${APPNAME}_lib)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")

message(STATUS "Using compiler: ${CMAKE_CXX_COMPILER}")

file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(${APPNAME} ${SOURCES})
add_library(${LIBNAME} STATIC ${SOURCES})

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