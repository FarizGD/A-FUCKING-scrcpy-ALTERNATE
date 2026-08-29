cmake_minimum_required(VERSION 3.20)
project(scrcpy_vmic_bridge LANGUAGES CXX)

add_executable(scrcpy-vmic-bridge main.cpp)
target_compile_features(scrcpy-vmic-bridge PRIVATE cxx_std_20)
target_compile_definitions(scrcpy-vmic-bridge PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(scrcpy-vmic-bridge PRIVATE ole32 uuid)
