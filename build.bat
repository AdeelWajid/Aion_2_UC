@echo off
cd /d "%~dp0"
REM Build the external ESP overlay - the target every ESP change lands in.
REM The release is the only target: build_test.bat builds recon and overlay_test for verification,
REM and neither is part of a release build.
setlocal
if exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" (set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat") else (set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
call "%VCVARS%" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set "BASE=/nologo /std:c++20 /EHsc /W3 /permissive- /D_CRT_SECURE_NO_WARNINGS /I "%CD%\include" /I "%CD%\third_party\imgui" /I "%CD%\third_party\imgui\backends""
set "COMMON=%BASE% /O2"
REM obf_strings.cpp is compiled on its own, at /O2 and with Obfusk8. The optimisation is not a
REM preference: AES8 encrypts a literal at compile time only when the optimiser folds its constexpr
REM path, and at /Od it does not fold - the literal then stays in the image as plaintext while the
REM runtime path produces rubbish (measured). /bigobj for the object size; the two /wd are truncation
REM warnings inside the library itself.
set "OBF=%BASE% /O2 /bigobj /wd4305 /wd4309 /DAION_OBFUSK8 /I "%CD%\third_party\Obfusk8""
REM ImGui is vendored (third_party\imgui) and compiled as its own pass at /W0: it is not our code,
REM and /W3 over ~60k upstream lines would bury our own warnings in noise. The marker names the
REM version, so re-vendoring a different ImGui rebuilds these objects instead of silently linking
REM the old ones.
set "IMGUI_VER=1.91.9b"
set "IMGUI_MARKER=build\imgui-%IMGUI_VER%.built"
set "IMGUI=/nologo /std:c++20 /EHsc /O2 /W0 /permissive- /D_CRT_SECURE_NO_WARNINGS /I "%CD%\third_party\imgui" /I "%CD%\third_party\imgui\backends" /I "%CD%\include""
set "IMGUI_SRC=third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\backends\imgui_impl_dx11.cpp third_party\imgui\backends\imgui_impl_win32.cpp"
set "IMGUI_OBJ=build\imgui.obj build\imgui_draw.obj build\imgui_tables.obj build\imgui_widgets.obj build\imgui_impl_dx11.obj build\imgui_impl_win32.obj"
if not exist build mkdir build

REM A running overlay holds its own exe open (Windows refuses to overwrite a running image), and it
REM renames itself at startup, so killing it by image name never matches the session that is
REM actually running: the pid it wrote next to the exe is the only handle on it. for/f rather than
REM set /p, because set /p on an empty file waits for input and hangs the build.
if exist build\.aion_external.pid for /f "usebackq tokens=1" %%P in ("build\.aion_external.pid") do taskkill /F /PID %%P >nul 2>&1

echo === [1/3] obfuscated literal table (/O2: AES8 folds only when optimising) ===
cl %OBF% /c /Fd:build\obf.pdb /Fo:build\ src\obf_strings.cpp
if errorlevel 1 exit /b 1

REM The marker names the version, so re-vendoring a different ImGui rebuilds these objects instead
REM of silently linking the old ones. It is not sufficient on its own: a clean that deletes
REM build\*.obj leaves the marker behind, the pass is skipped, and the link then fails on objects
REM the clean itself removed. Check the objects the marker stands for, not just the marker.
set "IMGUI_STALE="
if not exist "%IMGUI_MARKER%" set "IMGUI_STALE=1"
for %%O in (%IMGUI_OBJ%) do if not exist "%%O" set "IMGUI_STALE=1"

echo === [2/3] ImGui %IMGUI_VER% (vendored; rebuilt when the marker or its objects are missing) ===
if defined IMGUI_STALE (
  cl %IMGUI% /c /Fd:build\imgui.pdb /Fo:build\ %IMGUI_SRC%
  if errorlevel 1 exit /b 1
  type nul > "%IMGUI_MARKER%"
)

echo === [3/3] aion_external.exe (external ESP overlay + ImGui menu) ===
cl %COMMON% /Fe:build\aion_external.exe /Fo:build\ /Fd:build\ ^
   src\external_main.cpp src\Overlay.cpp src\Menu.cpp src\ImGuiLayer.cpp src\entry_obf.cpp ^
   src\app\Window.cpp src\app\Launch.cpp src\app\Names.cpp src\app\Cli.cpp ^
   src\app\Client.cpp src\app\Modes.cpp src\app\Frame.cpp src\Foreground.cpp src\KeyboardInject.cpp src\AutoDodge.cpp src\DodgeTelegraph.cpp src\LiveAttach.cpp ^
   build\obf_strings.obj %IMGUI_OBJ%
if errorlevel 1 (
  echo    build\aion_external.exe could not be written - read the compiler output above.
  echo    LNK1104 means a session is still holding the exe: close it and build again.
  echo    LNK1181 means an input object is missing: delete build\imgui-*.built and build again.
  exit /b 1
)

REM The names above are only hidden if this says so, and nothing else in the build would notice: a
REM leaked literal still compiles, links and runs - it just describes the tool to anything reading
REM the exe. Fail the build instead of shipping that quietly.
set "LEAK="
findstr /C:"AION2.exe" build\aion_external.exe >nul 2>&1 && set "LEAK=AION2.exe"
findstr /C:"AionExternalOverlay" build\aion_external.exe >nul 2>&1 && set "LEAK=AionExternalOverlay"
findstr /C:"AION2 overlay" build\aion_external.exe >nul 2>&1 && set "LEAK=AION2 overlay"
findstr /C:"aion_external.log" build\aion_external.exe >nul 2>&1 && set "LEAK=aion_external.log"
if not "%LEAK%"=="" (
  echo    LEAK: "%LEAK%" is still a plaintext string in build\aion_external.exe.
  echo    Obfusk8 folded it at runtime instead of compile time - see src\obf_strings.cpp.
  exit /b 1
)

echo === build complete ===
