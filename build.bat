@echo off
setlocal
cd /d "%~dp0"

echo ==============================================
echo   MirrorCenter 一键编译脚本
echo ==============================================
echo.

set CMAKE=C:\Program Files\CMake\bin\cmake.exe
set BUILD_DIR=%CD%\MirrorCenter\build2

if not exist "%CMAKE%" (
    echo [错误] 未找到 cmake: %CMAKE%
    exit /b 1
)

echo [1/2] 编译 MirrorCenter ^(MSVC Ninja Debug^) ...
"%CMAKE%" --build "%BUILD_DIR%"
if errorlevel 1 (
    echo.
    echo [错误] MirrorCenter 编译失败
    echo        若提示 LNK1168 cannot open MirrorCenter.exe,
    echo        请先关闭正在运行的 MirrorCenter 进程再重试。
    exit /b 1
)
echo [完成] MirrorCenter.exe -^> %BUILD_DIR%\app\MirrorCenter.exe
echo.

echo [2/2] 发布 Miracast 接收服务 ^(dotnet publish^) ...
dotnet publish "%CD%\WinMiracastReceiver\MiracastReceiverService\MiracastReceiverService.csproj" -c Release -r win-x64 --self-contained true -p:PublishSingleFile=false -o "%BUILD_DIR%\app\miracast-service"
if errorlevel 1 (
    echo.
    echo [错误] Miracast 接收服务发布失败
    exit /b 1
)
echo [完成] MiracastReceiverService -^> %BUILD_DIR%\app\miracast-service
echo.

echo ==============================================
echo   全部编译完成
echo   注意: uxplay ^(AirPlay 后端^) 需在 UxPlay-src 单独编译,
echo         新版构建产物请拷贝到运行目录使用。
echo ==============================================
endlocal
