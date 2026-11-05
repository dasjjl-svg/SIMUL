cmake_minimum_required(VERSION 3.15)
project(loopback_tester LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Windows 전용.")
endif()

add_executable(loopback_tester WIN32
    src/main.cpp
    src/app.cpp
    src/app.h
    src/ui_draw.cpp
    src/ui_draw.h
    src/format.h
    src/frame_diff.cpp
    src/frame_diff.h
    src/protocol.cpp
    src/protocol.h
    src/transport.cpp
    src/transport.h
    src/loopback_test.cpp
    src/loopback_test.h
    src/scheduling.cpp
    src/scheduling.h
    src/resource.h
    src/loopback_tester.rc
)

# 리소스 내장 매니페스트·아이콘 변경 시 리소스 재컴파일
set_source_files_properties(src/loopback_tester.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/loopback_tester.manifest;${CMAKE_CURRENT_SOURCE_DIR}/src/loopback_tester.ico")

target_include_directories(loopback_tester PRIVATE src)
target_compile_definitions(loopback_tester PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601 WINVER=0x0601)

if(MSVC)
    target_compile_options(loopback_tester PRIVATE /W4 /permissive- /utf-8)
    # 매니페스트는 리소스로 내장. 링커 자동 생성분과 중복 방지
    target_link_options(loopback_tester PRIVATE /MANIFEST:NO)
elseif(MINGW)
    target_compile_options(loopback_tester PRIVATE -Wall -Wextra)
    # wWinMain 진입점 선택, 런타임 정적 링크로 MinGW DLL 배포 불요
    target_link_options(loopback_tester PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

target_link_libraries(loopback_tester PRIVATE
    comctl32 ws2_32 advapi32 user32 gdi32 winmm uxtheme setupapi cfgmgr32)

# VC++ 재배포 패키지 미설치·구버전 PC 대비 CRT 정적 링크
set_property(TARGET loopback_tester PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set_target_properties(loopback_tester PROPERTIES OUTPUT_NAME "LoopbackTester")
