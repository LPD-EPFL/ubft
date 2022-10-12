add_library(dorythirdpartyliquibook market/Market.cpp market/Order.cpp simple/simple_order.cpp)

target_link_libraries(dorythirdpartyliquibook ${CONAN_LIBS})
