cmake_minimum_required(VERSION 3.9)
project(dorydemo CXX)

include(${CMAKE_BINARY_DIR}/setup.cmake)
dory_setup_cmake()

add_executable(ubft ${HEADER_TIDER} main.cpp)
target_link_libraries(ubft ${HEADER_TIDER} ${CONAN_LIBS})

add_executable(thread-pool-test ${HEADER_TIDER} thread-pool/test.cpp)
target_link_libraries(thread-pool-test ${CONAN_LIBS})

add_executable(tail-p2p-test ${HEADER_TIDER} tail-p2p/test.cpp)
target_link_libraries(tail-p2p-test ${CONAN_LIBS})

add_executable(ubft-client-test ${HEADER_TIDER} client-test.cpp)
target_link_libraries(ubft-client-test ${CONAN_LIBS})

add_executable(ubft-server-test ${HEADER_TIDER} server-test.cpp)
target_link_libraries(ubft-server-test ${CONAN_LIBS})

add_executable(ubft-server-test-hooks ${HEADER_TIDER} server-test.cpp latency-hooks.cpp)
target_link_libraries(ubft-server-test-hooks ${CONAN_LIBS})
target_compile_definitions(ubft-server-test-hooks PUBLIC LATENCY_HOOKS)

add_executable(rpc-client-test ${HEADER_TIDER} rpc/client-test.cpp)
target_link_libraries(rpc-client-test ${CONAN_LIBS})

add_executable(rpc-server-test ${HEADER_TIDER} rpc/server-test.cpp)
target_link_libraries(rpc-server-test ${CONAN_LIBS})

add_executable(tail-p2p-ping ${HEADER_TIDER} tail-p2p/ping.cpp)
target_link_libraries(tail-p2p-ping ${CONAN_LIBS})

add_executable(tail-cb-test ${HEADER_TIDER} tail-cb/test.cpp)
target_link_libraries(tail-cb-test ${CONAN_LIBS})

add_executable(tail-cb-ping ${HEADER_TIDER} tail-cb/ping.cpp)
target_link_libraries(tail-cb-ping ${CONAN_LIBS})

add_executable(certifier-ping ${HEADER_TIDER} certifier/ping.cpp)
target_link_libraries(certifier-ping ${CONAN_LIBS})

add_executable(swmr-test ${HEADER_TIDER} swmr/test.cpp)
target_link_libraries(swmr-test ${CONAN_LIBS})

add_executable(swmr-ping ${HEADER_TIDER} swmr/ping.cpp)
target_link_libraries(swmr-ping ${CONAN_LIBS})

add_executable(replicated-swmr-test ${HEADER_TIDER} replicated-swmr/test.cpp)
target_link_libraries(replicated-swmr-test ${CONAN_LIBS})

add_executable(replicated-swmr-ping ${HEADER_TIDER} replicated-swmr/ping.cpp)
target_link_libraries(replicated-swmr-ping ${CONAN_LIBS})

add_executable(consensus ${HEADER_TIDER} consensus/test.cpp)
target_link_libraries(consensus ${CONAN_LIBS})

# add_executable(playground ${HEADER_TIDER} playground.cpp)
# target_link_libraries(playground ${CONAN_LIBS})

add_executable(p2p-blake-bench ${HEADER_TIDER} benchmarks/p2p-send-blake.cpp)
target_link_libraries(p2p-blake-bench ${CONAN_LIBS})

add_executable(app ${HEADER_TIDER} app.cpp)
target_link_libraries(app ${CONAN_LIBS})
