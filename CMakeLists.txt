cmake_minimum_required(VERSION 3.15.4)
project(psm C CXX ASM)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_subdirectory(src)
