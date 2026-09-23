cmake_minimum_required(VERSION 3.20)
project(CapturePreview CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(capture_preview main.cpp)

if(MSVC)
  target_compile_options(capture_preview PRIVATE /utf-8)
  target_link_libraries(capture_preview PRIVATE
    mfplat.lib
    mf.lib
    mfreadwrite.lib
    mfuuid.lib
    ole32.lib
    user32.lib
    gdi32.lib
  )
endif()
