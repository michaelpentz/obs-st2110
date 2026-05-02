@echo off
:: Build obs-st2110 OBS plugin on Windows.
::
:: Prerequisites:
::   1. Visual Studio 2022 Build Tools at C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
::   2. CMake at C:\Program Files\CMake\bin\cmake.exe
::   3. obs-studio source tree at <obs-source-dir> (matching the installed OBS version)
::   4. OBS Studio installed at C:\Program Files (x86)\obs-studio\ (we extract obs.lib from its obs.dll)
::
:: Run from the obs-st2110 repo root: obs-plugin\build-windows.bat
setlocal enabledelayedexpansion

set "VS_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "OBS_SOURCE=<obs-source-dir>"
set "OBS_INSTALL=C:\Program Files (x86)\obs-studio"
set "OBS_DLL=%OBS_INSTALL%\bin\64bit\obs.dll"
set "BUILD_DIR=build"
set "DEPS_DIR=%BUILD_DIR%\obs-deps"

if not exist "%VS_VCVARS%" (
  echo ERROR: vcvars64.bat not found at %VS_VCVARS%
  exit /b 1
)
if not exist "%OBS_SOURCE%\libobs\obs-module.h" (
  echo ERROR: obs-studio source not found at %OBS_SOURCE%
  echo Clone with: git clone --depth 1 --branch 32.1.2 https://github.com/obsproject/obs-studio.git %OBS_SOURCE%
  exit /b 1
)
if not exist "%OBS_DLL%" (
  echo ERROR: %OBS_DLL% not found. Is OBS Studio installed?
  exit /b 1
)

call "%VS_VCVARS%" >nul
if errorlevel 1 (
  echo ERROR: vcvars64.bat failed
  exit /b 1
)

if not exist "%DEPS_DIR%" mkdir "%DEPS_DIR%"

:: Step 1: extract export table from obs.dll, generate obs.lib import library.
::         Plugin DLLs link against this so the loader can resolve symbols at runtime.
echo === Generating obs.lib from obs.dll ===
dumpbin /exports "%OBS_DLL%" > "%DEPS_DIR%\obs-exports.txt"
if errorlevel 1 exit /b 1

:: Convert dumpbin output to .def (one symbol per line under EXPORTS).
:: Skip the header lines and the trailing summary; keep only ordinal/name pairs.
echo LIBRARY obs > "%DEPS_DIR%\obs.def"
echo EXPORTS >> "%DEPS_DIR%\obs.def"
for /f "tokens=1,2,3,4" %%A in (%DEPS_DIR%\obs-exports.txt) do (
  if not "%%D"=="" (
    echo %%A | findstr /r "^[0-9][0-9]*$" >nul && (
      echo     %%D >> "%DEPS_DIR%\obs.def"
    )
  )
)

lib /def:"%DEPS_DIR%\obs.def" /out:"%DEPS_DIR%\obs.lib" /machine:x64 /nologo
if errorlevel 1 exit /b 1
echo Generated %DEPS_DIR%\obs.lib

:: Step 2: configure + build the plugin.
echo === Configuring CMake ===
cmake -B "%BUILD_DIR%" -G Ninja ^
  -DBUILD_OBS_PLUGIN=ON ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DOBS_SOURCE_DIR="%OBS_SOURCE%" ^
  -DOBS_LIB="%CD%\%DEPS_DIR%\obs.lib" ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
if errorlevel 1 exit /b 1

echo === Building ===
cmake --build "%BUILD_DIR%" --target obs-st2110
if errorlevel 1 exit /b 1

echo.
echo === Build succeeded ===
echo Plugin DLL: %CD%\%BUILD_DIR%\obs-plugin\obs-st2110.dll
echo.
echo To install (run from an Administrator prompt):
echo   copy %BUILD_DIR%\obs-plugin\obs-st2110.dll "%OBS_INSTALL%\obs-plugins\64bit\"
echo   xcopy obs-plugin\data\* "%OBS_INSTALL%\data\obs-plugins\obs-st2110\" /s /e /i /y
endlocal
