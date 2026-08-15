@echo off
setlocal
cd /d "%~dp0"

echo ==============================================
echo   MirrorCenter Build Script
echo   usage: build.bat [debug^|release]  (default release)
echo ==============================================
echo.

set CMAKE=C:\Program Files\CMake\bin\cmake.exe
set QT_BIN=C:\Qt\6.5.3\msvc2019_64\bin
set QT_PREFIX=C:/Qt/6.5.3/msvc2019_64
set NINJA=C:/Espressif/tools/ninja/1.12.1/ninja.exe

set MODE=release
if /i "%~1"=="debug" set MODE=debug
set BUILD_DIR=%CD%\MirrorCenter\build2
if "%MODE%"=="release" set BUILD_DIR=%CD%\MirrorCenter\build-release

if not exist "%CMAKE%" (
    echo [ERROR] cmake not found: %CMAKE%
    exit /b 1
)

echo [1/2] Build MirrorCenter ^(MSVC Ninja %MODE%^) ...

rem load VS env if cl not on PATH (needed for configure)
where cl >nul 2>nul
if errorlevel 1 (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
)

rem configure once
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [INFO] Configuring %BUILD_DIR% ...
    "%CMAKE%" -S "%CD%\MirrorCenter" -B "%BUILD_DIR%" -G Ninja ^
        -DCMAKE_BUILD_TYPE=%MODE% ^
        -DCMAKE_PREFIX_PATH=%QT_PREFIX% ^
        -DCMAKE_MAKE_PROGRAM=%NINJA%
    if errorlevel 1 (
        echo [ERROR] CMake configure failed. Ensure VS2022 x64 toolset installed.
        exit /b 1
    )
)

"%CMAKE%" --build "%BUILD_DIR%"
if errorlevel 1 (
    echo.
    echo [ERROR] Build failed. If LNK1168 cannot open MirrorCenter.exe,
    echo         close the running MirrorCenter and retry.
    exit /b 1
)

rem release deploy: reuse GStreamer/uxplay from build2, add Qt release DLLs
if "%MODE%"=="release" (
    set SRC=%CD%\MirrorCenter\build2\app
    set DST=%BUILD_DIR%\app
    robocopy "%SRC%" "%DST%" /E /XF MirrorCenter.exe mirrorsdk.dll "Qt6*d.dll" *.obj *.pdb *.lib *.ilk /XD CMakeFiles platforms /NFL /NDL /NJH /NJS /NP >nul
    "%QT_BIN%\windeployqt.exe" --release --no-translations "%DST%\MirrorCenter.exe" >nul
)

echo [OK] MirrorCenter.exe -^> %BUILD_DIR%\app\MirrorCenter.exe
echo.

echo [2/2] Publish Miracast Receiver Service ^(dotnet publish^) ...
dotnet publish "%CD%\WinMiracastReceiver\MiracastReceiverService\MiracastReceiverService.csproj" -c Release -r win-x64 --self-contained true -p:PublishSingleFile=false -o "%BUILD_DIR%\app\miracast-service"
if errorlevel 1 (
    echo.
    echo [ERROR] Miracast service publish failed.
    exit /b 1
)
echo [OK] MiracastReceiverService -^> %BUILD_DIR%\app\miracast-service
echo.

echo ==============================================
echo   Build done.  Mode: %MODE%
echo   Note: uxplay (AirPlay backend) is built in UxPlay-src
echo         and copied from build2\app on release deploy.
echo ==============================================
endlocal
