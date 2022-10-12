cmake_minimum_required(VERSION 3.10)
project(DoryRpcTest CXX)

include(${CMAKE_BINARY_DIR}/setup.cmake)
dory_setup_cmake()

enable_testing()
include(GoogleTest)

add_executable(rpc_test rpc-test.cpp)
target_link_libraries(rpc_test ${CONAN_LIBS})
gtest_discover_tests(rpc_test)
