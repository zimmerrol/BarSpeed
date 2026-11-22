@echo off
setlocal enabledelayedexpansion

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: Visual Studio Installer vswhere.exe not found.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALL=%%i"
)

if not defined VSINSTALL (
    echo ERROR: Visual Studio C++ Build Tools not found.
    exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: Failed to initialize x64 MSVC environment.
    exit /b 1
)

if not exist build mkdir build

echo Compiling resources...
rc /nologo /fo build\app.res src\app.rc
if errorlevel 1 exit /b 1

echo Building BarSpeed.exe...
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX ^
    /Fo:build\ /Fe:build\BarSpeed.exe ^
    src\main.cpp ^
    src\config.cpp ^
    src\net_monitor.cpp ^
    src\ping_monitor.cpp ^
    src\tray_icon.cpp ^
    src\settings_dialog.cpp ^
    src\taskbar_widget.cpp ^
    build\app.res ^
    /link /SUBSYSTEM:WINDOWS /MANIFEST:NO ^
    user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib comctl32.lib ^
    d2d1.lib dwrite.lib dwmapi.lib iphlpapi.lib ws2_32.lib

if errorlevel 1 (
    echo BUILD FAILED.
    exit /b 1
)

echo.
echo Built successfully: %CD%\build\BarSpeed.exe

if /i "%~1"=="test" (
    echo.
    echo Building unit tests...
    cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX ^
        /Isrc /Fo:build\ /Fe:build\test_core.exe ^
        tests\test_core.cpp ^
        src\config.cpp ^
        src\net_monitor.cpp ^
        src\ping_monitor.cpp ^
        src\tray_icon.cpp ^
        /link /SUBSYSTEM:CONSOLE ^
        user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib iphlpapi.lib ws2_32.lib
    if errorlevel 1 exit /b 1
    echo Running unit tests...
    build\test_core.exe
    if errorlevel 1 exit /b 1
)

endlocal
exit /b 0
