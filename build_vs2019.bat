@echo off
setlocal EnableExtensions DisableDelayedExpansion
for /f "tokens=2 delims=:" %%c in ('"%SystemRoot%\System32\chcp.com"') do set "LT_PREV_CP=%%c"
"%SystemRoot%\System32\chcp.com" 65001 >nul
rem 탐색기 실행(더블 클릭)만 종료 전 대기. 탐색기 호출 형태: ComSpec /c ""스크립트 경로" "
set "LT_PAUSE="
set "LT_CMD=%cmdcmdline:"=%"
set "LT_EXPLORER=%ComSpec% /c %~f0 "
rem 비교는 지연 확장 사용. 경로 내 특수 문자(&, 괄호, 공백)의 구문 해석 배제
setlocal EnableDelayedExpansion
set "LT_MATCH="
if /i "!LT_CMD!"=="!LT_EXPLORER!" set "LT_MATCH=1"
endlocal & set "LT_PAUSE=%LT_MATCH%"

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
echo [오류] 알 수 없는 인수: "%~1"
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
if exist "%VSWHERE%" call :findvs
if not defined VSDIR for %%e in (Enterprise Professional Community BuildTools) do if not defined VSDIR if exist "%PF86%\Microsoft Visual Studio\2019\%%e\VC\Auxiliary\Build\vcvarsall.bat" set "VSDIR=%PF86%\Microsoft Visual Studio\2019\%%e"
if not defined VSDIR goto novs
set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" goto novs
echo Visual Studio 2019: "%VSDIR%"
if /i "%MODE%"=="sln" goto sln

rem 빌드 환경: 대상 플랫폼별 MSVC v142·Windows SDK. 도구 출력은 영문 고정(UTF-8 콘솔의 한글 메시지 깨짐 방지)
set "VSLANG=1033"
call "%VCVARS%" %PLATFORM% >nul
"%SystemRoot%\System32\where.exe" cl.exe >nul 2>nul
if errorlevel 1 goto novcvars

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
rem 원본·목적 파일은 해당 폴더 기준 상대 이름 사용. 경로 내 특수 문자(&, ^) 해석 배제
set "SOURCES=main.cpp app.cpp ui_draw.cpp frame_diff.cpp protocol.cpp transport.cpp loopback_test.cpp scheduling.cpp"
set "OBJECTS=main.obj app.obj ui_draw.obj frame_diff.obj protocol.obj transport.obj loopback_test.obj scheduling.obj loopback_tester.res"
set "LIBS=kernel32.lib user32.lib gdi32.lib advapi32.lib comctl32.lib ws2_32.lib winmm.lib uxtheme.lib setupapi.lib cfgmgr32.lib"

echo.
echo [1/3] 컴파일: %CONFIG% %PLATFORM%
pushd "%SRC%"
cl %CXXFLAGS% %DEFINES% /I. /Fo"%OBJ%\\" /Fd"%OBJ%\\" %SOURCES%
set "STEP=%ERRORLEVEL%"
popd
if not "%STEP%"=="0" goto fail
echo [2/3] 리소스 컴파일
rc /nologo /i "%SRC%" /fo "%OBJ%\loopback_tester.res" "%SRC%\loopback_tester.rc"
if errorlevel 1 goto fail
echo [3/3] 링크
pushd "%OBJ%"
link %LINKFLAGS% /OUT:"%OUT%\LoopbackTester.exe" %OBJECTS% %LIBS%
set "STEP=%ERRORLEVEL%"
popd
if not "%STEP%"=="0" goto fail
echo.
echo 빌드 완료: "%OUT%\LoopbackTester.exe"
set "RESULT=0"
goto finish

:sln
rem 솔루션 생성: CMake 3.15 이상. Visual Studio 2019 내장 CMake 우선, PATH의 cmake 차선
set "CMAKE="
call :pickcmake "%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined CMAKE call :pickcmake cmake
if not defined CMAKE goto nocmake
set "CMAKE_ARCH=x64"
if /i "%PLATFORM%"=="x86" set "CMAKE_ARCH=Win32"
set "SLN=%ROOT%build\vs2019\solution-%PLATFORM%"
if defined CLEAN if exist "%SLN%" rmdir /s /q "%SLN%"
"%CMAKE%" -G "Visual Studio 16 2019" -A %CMAKE_ARCH% -S "%ROOT%." -B "%SLN%"
if errorlevel 1 goto slnfail
echo.
echo 솔루션 생성 완료: "%SLN%\loopback_tester.sln"
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
echo [오류] CMake 3.15 이상 미검출. Visual Studio 2019 16.4 이상 업데이트("Windows용 C++ CMake 도구" 포함) 또는 CMake 설치 필요.
goto finish

:slnfail
echo.
echo [오류] 솔루션 생성 실패.
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
echo   sln             Visual Studio 2019 솔루션 생성(CMake 3.15 이상). 빌드 생략
echo   nopause         종료 전 대기 생략(탐색기 실행 시에만 대기)
echo 출력: build\vs2019\플랫폼-구성\LoopbackTester.exe
exit /b 0

rem vswhere 실행: 설치 폴더 이동 후 상대 실행. 경로 괄호(Program Files (x86))의 명령 해석기 따옴표 처리 회피
:findvs
pushd "%PF86%\Microsoft Visual Studio\Installer"
for /f "usebackq tokens=*" %%i in (`vswhere.exe -version [16.0^,17.0^) -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
popd
exit /b 0

rem 지정 CMake(전체 경로 또는 cmake)의 버전 3.15 이상 여부. 충족 시 CMAKE 지정
:pickcmake
set "LT_VER="
set "LT_MAJ=0"
set "LT_MIN=0"
if /i "%~1"=="cmake" goto pickpath
if not exist "%~1" exit /b 1
pushd "%~dp1"
for /f "tokens=3" %%v in ('cmake.exe --version 2^>nul') do if not defined LT_VER set "LT_VER=%%v"
popd
goto pickcheck
:pickpath
for /f "tokens=3" %%v in ('cmake --version 2^>nul') do if not defined LT_VER set "LT_VER=%%v"
:pickcheck
if not defined LT_VER exit /b 1
for /f "tokens=1,2 delims=.-" %%a in ("%LT_VER%") do set /a "LT_MAJ=%%a, LT_MIN=%%b" >nul 2>nul
if %LT_MAJ% GTR 3 set "CMAKE=%~1" & exit /b 0
if %LT_MAJ% EQU 3 if %LT_MIN% GEQ 15 set "CMAKE=%~1" & exit /b 0
exit /b 1

:finish
if defined LT_PAUSE pause
endlocal & "%SystemRoot%\System32\chcp.com" %LT_PREV_CP% >nul & exit /b %RESULT%
