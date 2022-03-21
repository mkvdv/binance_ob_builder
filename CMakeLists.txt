cmake_minimum_required(VERSION 3.8)
project(TEST_BINANCE_OB_BUILDER)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror -Wextra -pedantic -Wcast-align -Wcast-qual -Wctor-dtor-privacy -Wenum-compare \
        -Winit-self -Wmissing-include-dirs -Wnon-virtual-dtor -Woverloaded-virtual -Wpedantic -Wredundant-decls \
        -Wshadow -Wsign-promo -Wstrict-overflow=1 -Wundef -Wconversion -Wfloat-equal -Wsign-conversion")

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED)

message(STATUS "Boost version = ${Boost_VERSION}, include = ${Boost_INCLUDE_DIRS}")
message(STATUS "OPENSSL version = ${OPENSSL_VERSION}")
include_directories(SYSTEM ${Boost_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})

set(LIB_LIST OpenSSL::SSL OpenSSL::Crypto)

include_directories(include)

add_executable(test_binance_ob_builder src/test_binance_ob_builder.cpp)
target_link_libraries(test_binance_ob_builder ${LIB_LIST})
