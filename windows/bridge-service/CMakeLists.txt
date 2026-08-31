cmake_minimum_required(VERSION 3.20)
project(scrcpy_bridge_service LANGUAGES CXX)

add_executable(scrcpy-bridge-service main.cpp)
target_compile_features(scrcpy-bridge-service PRIVATE cxx_std_17)
target_compile_definitions(scrcpy-bridge-service PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(scrcpy-bridge-service PRIVATE advapi32 userenv wtsapi32)
