cmake_minimum_required(VERSION 3.14)
project(DesktopCppUdpExample VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ViewAlyzer UDP sender library (defines viewalyzer_core, viewalyzer_rtos,
# and its own example executables)
add_subdirectory(c-udp)

# Example executable
add_executable(desktop_cpp_udp_example main.cpp)
target_link_libraries(desktop_cpp_udp_example PRIVATE viewalyzer_core viewalyzer_rtos)

if(UNIX)
    target_link_libraries(desktop_cpp_udp_example PRIVATE m)
endif()
