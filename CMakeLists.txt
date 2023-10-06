cmake_minimum_required(VERSION 3.20)

project(MHP)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")

message(STATUS "Using compiler: ${CMAKE_CXX_COMPILER}")

include_directories(${CMAKE_SOURCE_DIR}/include)

file(GLOB SOURCES "src/*.cpp")

add_executable(MHP ${SOURCES})