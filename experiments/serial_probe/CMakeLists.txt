cmake_minimum_required(VERSION 3.20)
project(SerialProbe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(serial_probe main.cpp)

if(MSVC)
  target_compile_options(serial_probe PRIVATE /utf-8)
endif()
