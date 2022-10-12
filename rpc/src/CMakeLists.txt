cmake_minimum_required(VERSION 3.9)
project(doryrpc)

include(${CMAKE_BINARY_DIR}/setup.cmake)
dory_setup_cmake()

if("$ENV{CLANG_GEN_TIDY_INFO}" STREQUAL "1")
  add_library(doryrpc ${HEADER_TIDER})
endif()
