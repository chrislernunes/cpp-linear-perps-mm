
cmake_minimum_required(VERSION 3.20)
project(binance_usdm_mm VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(OPENSSL_INCLUDE_DIR openssl/ssl.h)
find_path(ZLIB_INCLUDE_DIR zlib.h)
find_library(SSL_LIB NAMES ssl)
find_library(CRYPTO_LIB NAMES crypto)
find_library(Z_LIB NAMES z)

if(NOT OPENSSL_INCLUDE_DIR OR NOT ZLIB_INCLUDE_DIR)
  message(FATAL_ERROR
    "OpenSSL or zlib headers not found. Check your compiler environment.")
endif()

if(NOT SSL_LIB OR NOT CRYPTO_LIB OR NOT Z_LIB)
  message(FATAL_ERROR
    "OpenSSL or zlib libraries not found. Install the dependencies in your toolchain.")
endif()

add_library(usdm
  src/json_mini.cpp
  src/net.cpp
  src/order_book.cpp
  src/quote.cpp
  src/zip_archive.cpp
  src/history.cpp
  src/backtest.cpp
  src/report.cpp
  src/demo.cpp
  src/live.cpp
)

target_include_directories(usdm PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${OPENSSL_INCLUDE_DIR}
  ${ZLIB_INCLUDE_DIR}
)

if(MSVC)
  target_compile_options(usdm PRIVATE /W3)
else()
  target_compile_options(usdm PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

target_link_libraries(usdm PUBLIC
  ${SSL_LIB}
  ${CRYPTO_LIB}
  ${Z_LIB}
)

if(WIN32)
  target_link_libraries(usdm PUBLIC ws2_32 crypt32)
elseif(UNIX)
  target_link_libraries(usdm PUBLIC dl pthread)
endif()

function(add_usdm_exe name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE usdm)

  if(MSVC)
    target_compile_options(${name} PRIVATE /W3)
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
endfunction()

add_usdm_exe(binance_market_maker src/market_maker_main.cpp)
add_usdm_exe(binance_backtest src/backtest_main.cpp)
add_usdm_exe(unit_tests tests/unit_tests.cpp)

enable_testing()
add_test(NAME unit_tests COMMAND unit_tests)
