@echo off
setlocal
rem ---------------------------------------------------------------------------
rem Keep this file ASCII-only. cmd.exe parses .bat in the OEM codepage (936
rem here), so a UTF-8 CJK comment can end on a byte that swallows the CRLF and
rem silently eats the next command. That is exactly how the rc step broke once.
rem ---------------------------------------------------------------------------
set "MSVC=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\MSVC\14.51.36231"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKV=10.0.26100.0"

set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKV%\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\winrt;%SDK%\Include\%SDKV%\cppwinrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKV%\ucrt\x64;%SDK%\Lib\%SDKV%\um\x64"

cd /d "%~dp0"
if not exist build mkdir build

rem Icon + version info. rc.exe resolves relative paths against the .rc file's
rem own directory, which is why app.rc points at ../assets/icon.ico
if exist build\app.res del /q build\app.res
rc /nologo /fo build\app.res src\app.rc
if errorlevel 1 (
    echo RC_FAILED=%ERRORLEVEL%
    exit /b 1
)
if not exist build\app.res (
    echo RC_NO_OUTPUT
    exit /b 1
)

rem /EHa so catch(...) also traps SEH (access violations). OCR and translation
rem run on worker threads, where an uncaught exception means std::terminate --
rem i.e. the process just disappears. /EHa downgrades that to an error panel.
cl /nologo /std:c++20 /EHa /O2 /MT /utf-8 /W3 /DUNICODE /D_UNICODE ^
   /Fo:build\ /Fd:build\ ^
   src\main.cpp src\overlay.cpp src\annotate.cpp src\capture.cpp ^
   src\ocr.cpp src\translate.cpp src\pin.cpp src\output.cpp src\settings.cpp src\common.cpp ^
   /link /SUBSYSTEM:WINDOWS /OUT:build\JianJie.exe build\app.res /MAP:build\JianJie.map ^
   user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib winhttp.lib dwmapi.lib shcore.lib windowsapp.lib advapi32.lib comdlg32.lib

echo EXITCODE=%ERRORLEVEL%
