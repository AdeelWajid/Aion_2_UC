@echo off
REM Verification targets - not part of the release build, no obfuscation.
REM
REM   aion_recon.exe   read-only live validator / reconnaissance dump
REM   overlay_test.exe standalone D3D11 + ESP assertions (writes overlay_test.bmp)
REM
REM The release target is build.bat (build\aion_external.exe). These two exist to check the
REM offsets and the renderer against a running client and against synthetic data, so they are
REM kept plain: a failure in one of them has to be debuggable, and their string names must match
REM the release build's at runtime (obf_strings.cpp returns the same values either way).
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set "COMMON=/nologo /std:c++20 /EHsc /O2 /W3 /permissive- /D_CRT_SECURE_NO_WARNINGS /I "%CD%\include" /I "%CD%\third_party\imgui" /I "%CD%\third_party\imgui\backends""
REM overlay_test renders the ImGui menu, so it needs the same vendored pass the release builds.
REM Same marker as build.bat, so whichever script runs first does the work and the other reuses it.
set "IMGUI_VER=1.91.9b"
set "IMGUI_MARKER=build\imgui-%IMGUI_VER%.built"
set "IMGUI=/nologo /std:c++20 /EHsc /O2 /W0 /permissive- /D_CRT_SECURE_NO_WARNINGS /I "%CD%\third_party\imgui" /I "%CD%\third_party\imgui\backends" /I "%CD%\include""
set "IMGUI_SRC=third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\backends\imgui_impl_dx11.cpp third_party\imgui\backends\imgui_impl_win32.cpp"
set "IMGUI_OBJ=build\imgui.obj build\imgui_draw.obj build\imgui_tables.obj build\imgui_widgets.obj build\imgui_impl_dx11.obj build\imgui_impl_win32.obj"
if not exist build mkdir build

echo === [1/3] ImGui %IMGUI_VER% (vendored; rebuilt when the marker or its objects are missing) ===
set "IMGUI_STALE="
if not exist "%IMGUI_MARKER%" set "IMGUI_STALE=1"
for %%O in (%IMGUI_OBJ%) do if not exist "%%O" set "IMGUI_STALE=1"
if defined IMGUI_STALE (
  cl %IMGUI% /c /Fd:build\imgui.pdb /Fo:build\ %IMGUI_SRC%
  if errorlevel 1 exit /b 1
  type nul > "%IMGUI_MARKER%"
)

echo === [2/3] recon (external, read-only) ===
cl %COMMON% /Fe:build\aion_recon.exe /Fo:build\ /Fd:build\ ^
   src\recon.cpp src\LiveAttach.cpp src\obf_strings.cpp user32.lib || exit /b 1

echo === [3/3] overlay_test (standalone D3D11 renderer + ImGui menu) ===
cl %COMMON% /Fe:build\overlay_test.exe /Fo:build\ /Fd:build\ ^
   src\overlay_test.cpp src\Overlay.cpp src\Menu.cpp src\ImGuiLayer.cpp src\obf_strings.cpp ^
   src\app\Window.cpp ^
   %IMGUI_OBJ% || exit /b 1

echo === verification build complete ===
