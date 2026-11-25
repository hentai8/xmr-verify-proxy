cmake_minimum_required(VERSION 3.20)
project(xmr-verify-proxy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static CRT: the produced exe runs on a clean Windows box without the VC++ redist.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(third_party/randomx EXCLUDE_FROM_ALL)

option(XVP_AVX2 "Compile RandomX with /arch:AVX2 (needs an AVX2-capable CPU)" OFF)
if(XVP_AVX2 AND MSVC)
  target_compile_options(randomx PRIVATE /arch:AVX2)
endif()

add_executable(xmr-verify-proxy
  src/main.cpp
  src/util.cpp
  src/log.cpp
  src/randomx_engine.cpp
  src/config.cpp
  src/net.cpp
  src/proxy.cpp
  src/stats.cpp
  src/testminer.cpp
  src/mockpool.cpp
)

target_include_directories(xmr-verify-proxy PRIVATE
  src
  third_party/nlohmann
  third_party/randomx/src
)

target_link_libraries(xmr-verify-proxy PRIVATE randomx ws2_32)
target_compile_definitions(xmr-verify-proxy PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
