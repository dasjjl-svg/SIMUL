cmake_minimum_required(VERSION 3.15)
project(simul LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "SIMUL targets Windows only.")
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    add_compile_definitions(_UNICODE UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

set(SIMUL_SOURCES
    src/main.cpp
    src/app.cpp
    src/app.h
    src/protocol.cpp
    src/protocol.h
    src/transport.cpp
    src/transport.h
    src/loopback_test.cpp
    src/loopback_test.h
    src/vspe_launcher.cpp
    src/vspe_launcher.h
    src/resource.h
    src/simul.rc
)

add_executable(simul WIN32 ${SIMUL_SOURCES})
target_include_directories(simul PRIVATE src)
target_link_libraries(simul PRIVATE
    comctl32
    ws2_32
    shell32
    advapi32
    user32
    gdi32
)

set_target_properties(simul PROPERTIES
    OUTPUT_NAME "SIMUL"
    VS_DPI_AWARE "PerMonitor"
)
