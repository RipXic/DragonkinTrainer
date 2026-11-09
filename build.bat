@echo off
rem Dragonkin Trainer - build from source (Visual Studio 2022 or newer, "Desktop development with C++", x64).
rem Output: bin\DragonkinTrainer.exe + bin\native.dll + bin\panel.json
setlocal
set ROOT=%~dp0
set TP=%ROOT%third_party
set IMGUI=%TP%\imgui-1.91.9b
set MH=%TP%\minhook-master
set OUT=%ROOT%bin
set OBJ=%ROOT%obj
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"
where cl >nul 2>nul && goto havecl
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do call "%%I\VC\Auxiliary\Build\vcvars64.bat" >nul
:havecl
where cl >nul 2>nul || (echo Visual Studio C++ tools not found - run this from an "x64 Native Tools Command Prompt" & exit /b 1)

set INC=/I"%ROOT%src" /I"%TP%" /I"%IMGUI%" /I"%IMGUI%\backends" /I"%MH%\include"
set FLAGS=/nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /DNDEBUG /DNOMINMAX /DWB_PUBLIC=1 /DIMGUI_DISABLE_DEBUG_TOOLS /wd4996 /wd4244 /wd4267

echo === third-party (Dear ImGui, MinHook)
cl %FLAGS% %INC% /c /Fo"%OBJ%\\" "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" "%IMGUI%\backends\imgui_impl_win32.cpp" "%IMGUI%\backends\imgui_impl_dx12.cpp" "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" || exit /b 1

echo === trainer
cl %FLAGS% %INC% /c /Fo"%OBJ%\\" "%ROOT%src\core.cpp" "%ROOT%src\overlay.cpp" "%ROOT%src\panel.cpp" "%ROOT%src\settings.cpp" "%ROOT%src\dx12_hook.cpp" "%ROOT%src\icons_none.cpp" "%ROOT%src\ue.cpp" "%ROOT%src\dk.cpp" "%ROOT%src\dk_editor.cpp" "%ROOT%src\dk_gridhook.cpp" "%ROOT%src\trinity\hooks\xinput_hook.cpp" "%ROOT%src\trinity\hooks\dualsense.cpp" || exit /b 1

echo === version resources
rc /nologo /i "%ROOT%res" /fo "%OBJ%\dll_version.res" "%ROOT%res\dll.rc" || exit /b 1
rc /nologo /i "%ROOT%res" /fo "%OBJ%\loader_version.res" "%ROOT%res\loader.rc" || exit /b 1

echo === link native.dll
link /nologo /DLL /OUT:"%OUT%\native.dll" "%OBJ%\*.obj" "%OBJ%\dll_version.res" user32.lib gdi32.lib psapi.lib ws2_32.lib d3d12.lib dxgi.lib dwmapi.lib imm32.lib version.lib d3dcompiler.lib xinput9_1_0.lib hid.lib setupapi.lib ole32.lib || exit /b 1

echo === DragonkinTrainer.exe
cl /nologo /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE /DWB_PUBLIC=1 "%ROOT%loader\loader.cpp" /Fo"%OBJ%\..\loader.obj" /Fe"%OUT%\DragonkinTrainer.exe" /link "%OBJ%\loader_version.res" user32.lib shell32.lib || exit /b 1
copy /y "%ROOT%panel.json" "%OUT%\panel.json" >nul
echo BUILD OK - bin\DragonkinTrainer.exe, bin\native.dll, bin\panel.json
endlocal
exit /b 0
