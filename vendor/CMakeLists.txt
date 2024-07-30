# we have a bunch of header only libraries in the "include" directory
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(${APPNAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(${LIBNAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
MESSAGE(STATUS "Path to include directory: ${CMAKE_CURRENT_SOURCE_DIR}/include")

# other libraries
add_subdirectory(fmt)

# libs
target_link_libraries(${LIBNAME} fmt::fmt)
target_link_libraries(${APPNAME} fmt::fmt)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/fmt/include)