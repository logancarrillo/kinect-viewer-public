@echo off
rem Configure and build the Windows host with MSVC + Ninja (4 parallel jobs), then optionally run tests.
rem Usage: windows\scripts\build.cmd [test|configure]   (configure: make the build folder and stop)
rem libfreenect2, vcpkg and vcpkg's packages are looked for beside this checkout, in the folder that holds it
rem (docs\windows-setup.md): libfreenect2\install, vcpkg and vcpkg-installed\kinect-viewer. To use other folders,
rem set FREENECT2_PREFIX, KV_VCPKG_ROOT and KV_VCPKG_INSTALLED; KV_BUILD_DIR moves the build (default build\windows).
setlocal
set "SRC=%~dp0..\.."
for %%I in ("%SRC%\..") do set "KV_PARENT=%%~fI"
set "KV_PARENT=%KV_PARENT:\=/%"
if "%KV_PARENT:~-1%"=="/" set "KV_PARENT=%KV_PARENT:~0,-1%"
if not defined KV_BUILD_DIR set "KV_BUILD_DIR=%SRC%\build\windows"
if not defined FREENECT2_PREFIX set "FREENECT2_PREFIX=%KV_PARENT%/libfreenect2/install"
if not defined KV_VCPKG_ROOT set "KV_VCPKG_ROOT=%KV_PARENT%/vcpkg"
if not defined KV_VCPKG_INSTALLED set "KV_VCPKG_INSTALLED=%KV_PARENT%/vcpkg-installed/kinect-viewer"

if exist "%FREENECT2_PREFIX%\include\libfreenect2\libfreenect2.hpp" goto :have_freenect2
echo libfreenect2 isn't installed at FREENECT2_PREFIX=%FREENECT2_PREFIX%
echo Install it there, or set FREENECT2_PREFIX to the folder it is installed in. See docs\windows-setup.md.
exit /b 1
:have_freenect2
if exist "%KV_VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" goto :have_vcpkg
echo vcpkg isn't at KV_VCPKG_ROOT=%KV_VCPKG_ROOT%
echo Clone and bootstrap it there, or set KV_VCPKG_ROOT to where it is. See docs\windows-setup.md.
exit /b 1
:have_vcpkg

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
  echo Visual Studio C++ build tools not found.
  exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

rem Each -DKEY=VALUE quoted whole, so a checkout (or a folder beside it) whose path has spaces works.
set "TOOLCHAIN_ARGS="-DCMAKE_TOOLCHAIN_FILE=%KV_VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" "-DVCPKG_MANIFEST_DIR=%SRC%/windows" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md "-DVCPKG_INSTALLED_DIR=%KV_VCPKG_INSTALLED%""
set VCPKG_MAX_CONCURRENCY=4
set VCPKG_DISABLE_METRICS=1

cmake -S "%SRC%" -B "%KV_BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DFREENECT2_PREFIX=%FREENECT2_PREFIX%" %TOOLCHAIN_ARGS% || exit /b 1
if /i "%~1"=="configure" exit /b 0
cmake --build "%KV_BUILD_DIR%" -j 4 || exit /b 1

if /i "%~1"=="test" (
  ctest --test-dir "%KV_BUILD_DIR%" --output-on-failure -j 1 || exit /b 1
)
exit /b 0
