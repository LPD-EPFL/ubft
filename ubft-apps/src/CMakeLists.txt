cmake_minimum_required(VERSION 3.9)
project(doryubftapps CXX)

include(${CMAKE_BINARY_DIR}/setup.cmake)
dory_setup_cmake()

add_executable(mu-server ${HEADER_TIDER} mu-server.cpp)
target_link_libraries(mu-server ${CONAN_LIBS})

add_executable(mu-client ${HEADER_TIDER} client.cpp)
target_link_libraries(mu-client ${CONAN_LIBS})

add_executable(ubft-server ${HEADER_TIDER} ubft-server.cpp)
target_link_libraries(ubft-server ${CONAN_LIBS})

add_executable(ubft-client ${HEADER_TIDER} client.cpp)
target_link_libraries(ubft-client ${CONAN_LIBS})
target_compile_definitions(ubft-client PUBLIC UBFT)
