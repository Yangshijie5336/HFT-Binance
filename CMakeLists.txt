cmake_minimum_required(VERSION 3.20)
project(marketpipe_hft_demo VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(marketpipe cpp/src/main.cpp)
target_include_directories(marketpipe PRIVATE cpp/include)

enable_testing()
add_test(NAME marketpipe_help COMMAND marketpipe --help)
add_test(NAME marketpipe_synthetic COMMAND marketpipe synthetic --events 1000)
