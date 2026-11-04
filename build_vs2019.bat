@echo off
setlocal EnableExtensions DisableDelayedExpansion
for /f "tokens=2 delims=:" %%c in ('"%SystemRoot%\System32\chcp.com"') do set "LT_PREV_CP=%%c"
"%SystemRoot%\System32\chcp.com" 65001 >nul
rem 탐색기 실행(더블 클릭) 시 종료 전 대기
set "LT_PAUSE="
echo %cmdcmdline% | "%SystemRoot%\System32\find.exe" /i "%~0" >nul && set "LT_PAUSE=1"

rem Loopback Tester Visual Studio 2019 빌드
rem 사용: build_vs2019.bat [release 또는 debug] [x64 또는 x86] [clean] [sln] [nopause]
rem 출력: build\vs2019\플랫폼-구성\LoopbackTester.exe

set "ROOT=%~dp0"
set "SRC=%ROOT%src"
set "CONFIG=Release"
set "PLATFORM=x64"
set "CLEAN="
set "MODE=build"
set "RESULT=1"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="release" set "CONFIG=Release" & shift & goto parse
if /i "%~1"=="debug" set "CONFIG=Debug" & shift & goto parse
if /i "%~1"=="x64" set "PLATFORM=x64" & shift & goto parse
if /i "%~1"=="x86" set "PLATFORM=x86" & shift & goto parse
if /i "%~1"=="clean" set "CLEAN=1" & shift & goto parse
if /i "%~1"=="sln" set "MODE=sln" & shift & goto parse
if /i "%~1"=="nopause" set "LT_PAUSE=" & shift & goto parse
if /i "%~1"=="help" goto help
if "%~1"=="/?" goto help
echo [오류] 알 수 없는 인수: %~1
call :usage
goto finish

:help
call :usage
set "RESULT=0"
goto finish

:parsed
rem Visual Studio 2019(16.x) 설치 경로: vswhere 우선, 기본 설치 경로 차선
set "PF86=%ProgramFiles(x86)%"
if "%PF86%"=="" set "PF86=%ProgramFiles%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -version [16.0^,17.0^) -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR for %%e in (Enterprise Professional Community BuildTools) do if not defined VSDIR if exist "%PF86%\Microsoft Visual Studio\2019\%%e\VC\Auxiliary\Build\vcvarsall.bat" set "VSDIR=%PF86%\Microsoft Visual Studio\2019\%%e"
if not defined VSDIR goto novs
set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" goto novs
echo Visual Studio 2019: %VSDIR%
if /i "%MODE%"=="sln" goto sln

rem 빌드 환경: 대상 플랫폼별 MSVC v142·Windows SDK. 도구 출력은 영문 고정(UTF-8 콘솔의 한글 메시지 깨짐 방지)
set "VSLANG=1033"
call "%VCVARS%" %PLATFORM% >nul
"%SystemRoot%\System32\where.exe" cl.exe >nul 2>nul || goto novcvars

set "OUT=%ROOT%build\vs2019\%PLATFORM%-%CONFIG%"
set "OBJ=%OUT%\obj"
if defined CLEAN if exist "%OUT%" rmdir /s /q "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%OBJ%" goto fail

rem 컴파일 조건: CMakeLists.txt와 동일(C++17, /W4, /permissive-, UTF-8 원본, CRT 정적 링크, Windows 7 이상)
set "CXXFLAGS=/nologo /c /std:c++17 /EHsc /W4 /permissive- /utf-8 /Zc:__cplusplus /MP"
set "DEFINES=/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0601 /DWINVER=0x0601"
set "LINKFLAGS=/nologo /SUBSYSTEM:WINDOWS /MANIFEST:NO"
if /i "%CONFIG%"=="Release" set "CXXFLAGS=%CXXFLAGS% /O2 /MT /DNDEBUG" & set "LINKFLAGS=%LINKFLAGS% /OPT:REF /OPT:ICF"
if /i "%CONFIG%"=="Debug" set "CXXFLAGS=%CXXFLAGS% /Od /MTd /Zi /RTC1 /D_DEBUG" & set "LINKFLAGS=%LINKFLAGS% /DEBUG"
set "SOURCES="%SRC%\main.cpp" "%SRC%\app.cpp" "%SRC%\ui_draw.cpp" "%SRC%\protocol.cpp" "%SRC%\transport.cpp" "%SRC%\loopback_test.cpp" "%SRC%\scheduling.cpp""
set "OBJECTS="%OBJ%\main.obj" "%OBJ%\app.obj" "%OBJ%\ui_draw.obj" "%OBJ%\protocol.obj" "%OBJ%\transport.obj" "%OBJ%\loopback_test.obj" "%OBJ%\scheduling.obj""
set "LIBS=kernel32.lib user32.lib gdi32.lib advapi32.lib comctl32.lib ws2_32.lib winmm.lib uxtheme.lib setupapi.lib cfgmgr32.lib"

echo.
echo [1/3] 컴파일: %CONFIG% %PLATFORM%
cl %CXXFLAGS% %DEFINES% /I"%SRC%" /Fo"%OBJ%\\" /Fd"%OBJ%\\" %SOURCES%
if errorlevel 1 goto fail
echo [2/3] 리소스 컴파일
rc /nologo /i "%SRC%" /fo "%OBJ%\loopback_tester.res" "%SRC%\loopback_tester.rc"
if errorlevel 1 goto fail
echo [3/3] 링크
link %LINKFLAGS% /OUT:"%OUT%\LoopbackTester.exe" %OBJECTS% "%OBJ%\loopback_tester.res" %LIBS%
if errorlevel 1 goto fail
echo.
echo 빌드 완료: %OUT%\LoopbackTester.exe
set "RESULT=0"
goto finish

:sln
rem 솔루션 생성: Visual Studio 2019 내장 CMake 우선, PATH의 cmake 차선
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"
"%CMAKE%" --version >nul 2>nul || goto nocmake
set "CMAKE_ARCH=x64"
if /i "%PLATFORM%"=="x86" set "CMAKE_ARCH=Win32"
set "SLN=%ROOT%build\vs2019\solution-%PLATFORM%"
if defined CLEAN if exist "%SLN%" rmdir /s /q "%SLN%"
"%CMAKE%" -G "Visual Studio 16 2019" -A %CMAKE_ARCH% -S "%ROOT%." -B "%SLN%"
if errorlevel 1 goto fail
echo.
echo 솔루션 생성 완료: %SLN%\loopback_tester.sln
set "RESULT=0"
goto finish

:novs
echo [오류] Visual Studio 2019 C++ 빌드 도구 미검출.
echo        Visual Studio Installer에서 "C++를 사용한 데스크톱 개발" 워크로드(MSVC v142, Windows 10 SDK) 설치 필요.
goto finish

:novcvars
echo [오류] 빌드 환경 설정 실패: "%VCVARS%" %PLATFORM%
goto finish

:nocmake
echo [오류] CMake 미검출. Visual Studio Installer에서 "Windows용 C++ CMake 도구" 구성 요소 설치 필요.
goto finish

:fail
echo.
echo [오류] 빌드 실패.
goto finish

:usage
echo 사용: build_vs2019.bat [release^|debug] [x64^|x86] [clean] [sln] [nopause]
echo   release, debug  빌드 구성. 기본 release
echo   x64, x86        대상 플랫폼. 기본 x64
echo   clean           해당 출력 폴더 삭제 후 빌드
echo   sln             Visual Studio 2019 솔루션 생성(CMake). 빌드 생략
echo   nopause         종료 전 대기 생략
echo 출력: build\vs2019\플랫폼-구성\LoopbackTester.exe
exit /b 0

:finish
if defined LT_PAUSE pause
endlocal & "%SystemRoot%\System32\chcp.com" %LT_PREV_CP% >nul & exit /b %RESULT%
