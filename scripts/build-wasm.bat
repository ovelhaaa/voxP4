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

if not exist "build-wasm" (
    call emcmake cmake -B build-wasm -S wasm -G Ninja
)

call cmake --build build-wasm
if errorlevel 1 (
    echo [ERROR] WASM build failed.
    exit /b 1
)

echo.
echo [SUCCESS] WASM build completed.
echo Copying artifacts to voxP4-editor...

set "EDITOR_WASM_SRC=..\voxP4-editor\src\audio\wasm"
set "EDITOR_WASM_PUB=..\voxP4-editor\public\wasm"

if not exist "!EDITOR_WASM_SRC!" mkdir "!EDITOR_WASM_SRC!"
if not exist "!EDITOR_WASM_PUB!" mkdir "!EDITOR_WASM_PUB!"

copy /y "build-wasm\voxp4-preview.mjs" "!EDITOR_WASM_SRC!\"
copy /y "build-wasm\voxp4-preview.wasm" "!EDITOR_WASM_SRC!\"
copy /y "build-wasm\voxp4-preview.wasm" "!EDITOR_WASM_PUB!\"

echo Generating dsp-compatibility.json...
node "%~dp0generate-manifest.mjs"

echo All done!
