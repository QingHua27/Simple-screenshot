@echo off
setlocal
set "MSVC=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\MSVC\14.51.36231"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKV=10.0.26100.0"

set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKV%\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\winrt;%SDK%\Include\%SDKV%\cppwinrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKV%\ucrt\x64;%SDK%\Lib\%SDKV%\um\x64"

cd /d "%~dp0"
if not exist build mkdir build

rem 资源（图标 + 版本信息）。rc.exe 的相对路径以 .rc 所在目录为基准，
rem 所以 app.rc 里写的是 ../assets/icon.ico
rc /nologo /fo build\app.res src\app.rc
if errorlevel 1 (
    echo RC_FAILED=%ERRORLEVEL%
    exit /b 1
)

cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /W3 /DUNICODE /D_UNICODE ^
   /Fo:build\ /Fd:build\ ^
   src\main.cpp src\overlay.cpp src\annotate.cpp src\capture.cpp ^
   src\ocr.cpp src\translate.cpp src\pin.cpp src\output.cpp src\settings.cpp src\common.cpp ^
   /link /SUBSYSTEM:WINDOWS /OUT:build\JianJie.exe build\app.res ^
   user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib winhttp.lib dwmapi.lib shcore.lib windowsapp.lib advapi32.lib comdlg32.lib

echo EXITCODE=%ERRORLEVEL%
