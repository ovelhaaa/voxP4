@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo   Compiling VoxP4 WebAssembly Preview Target
echo ===================================================

if exist "C:\emsdk\emsdk_env.bat" (
    call "C:\emsdk\emsdk_env.bat"
) else (
    echo [WARN] C:\emsdk\emsdk_env.bat not found, assuming emcc is in PATH
)

cd /d "%~dp0\.."

rem Always reconfigure so VOXP4_GIT_COMMIT reflects the current HEAD.
call emcmake cmake -B build-wasm -S wasm -G Ninja
if errorlevel 1 (
    echo [ERROR] WASM configure failed.
    exit /b 1
)

call cmake --build build-wasm
if errorlevel 1 (
    echo [ERROR] WASM build failed.
    exit /b 1
)

echo.
echo [SUCCESS] WASM build completed.
echo Generating dsp-compatibility.json into build-wasm...
node "%~dp0generate-manifest.mjs"
if errorlevel 1 (
    echo [ERROR] Manifest generation failed.
    exit /b 1
)

echo.
echo Atomically syncing WASM package into voxP4-editor...
node "%~dp0..\..\voxP4-editor\scripts\sync-wasm.mjs" "build-wasm"
if errorlevel 1 (
    echo [ERROR] Editor WASM sync failed. Editor artifacts left untouched.
    exit /b 1
)

echo All done!
