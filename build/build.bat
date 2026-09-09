@echo off
setlocal

set ROOT=%~dp0
set OUT=%ROOT%build
set SRC=%ROOT%src

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
if defined VSCMD_VER goto compile

for %%p in ("%ProgramFiles%" "C:\Program Files" "C:\Program Files (x86)") do (
    for %%v in (2026 18 2022 17 2019 16) do (
        for %%e in (Community Professional Enterprise BuildTools Insiders Preview) do (
            if exist "%%~p\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" (
                call "%%~p\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" >nul
                goto compile
            )
        )
    )
)

goto novs

:compile
where cl >nul 2>&1
if errorlevel 1 goto novs

set MINHOOK="%ROOT%..\..\echo (2)\echo\ext\minhook\src\buffer.c" "%ROOT%..\..\echo (2)\echo\ext\minhook\src\hook.c" "%ROOT%..\..\echo (2)\echo\ext\minhook\src\trampoline.c" "%ROOT%..\..\echo (2)\echo\ext\minhook\src\hde\hde64.c"
set FLAGS=/nologo /std:c++17 /EHsc /O2 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS

pushd "%OUT%"

cl %FLAGS% /I"%ROOT%..\..\echo (2)\echo\ext\minhook\include" /LD /Fe:mini.dll /Fo:obj\ "%SRC%\main.cpp" "%SRC%\scan.cpp" %MINHOOK% /link user32.lib gdi32.lib
set CODE=%ERRORLEVEL%

popd

if not "%CODE%"=="0" goto failed

echo built %OUT%\mini.dll
endlocal
exit /b 0

:failed
echo build failed
endlocal
exit /b 1

:novs
echo visual studio with the c++ toolset was not found
endlocal
exit /b 1
