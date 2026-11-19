@echo off
rem SPDX-License-Identifier: MIT
rem Copyright (c) 2026 Marco Pistella
rem
rem build.bat - builds ADFDMSView.dll (Far Manager 3 plug-in) with the MSYS2 GCC toolchains.
rem Run it from the Windows Command Prompt, in this folder.
rem
rem   build.bat         builds both versions
rem   build.bat x64     builds only the 64-bit version, into .\x64
rem   build.bat x86     builds only the 32-bit version, into .\x86
rem   build.bat noupx   (with or without x64/x86) skips UPX compression
rem   build.bat max     (with or without x64/x86) UPX also tries --ultra-brute: much slower
rem   build.bat zip     (with or without the others) creates ADFDMSView-<version>.zip; needs tar.exe
rem   Options can be combined (build.bat x64 max zip), except "max" with "noupx".
rem
rem MSYS2: if not found, the script offers to download and install it unattended (64-bit Windows 10
rem   1809 or later, curl.exe available, destination folder missing or empty; default C:\msys64).
rem   Nothing is installed unless you choose 1; choose 0 to see the official download page.
rem Compilers (installed with pacman if missing): x64 uses MINGW64 (smaller DLLs), UCRT64 only as an
rem   explicit alternative; x86 uses MINGW32. MINGW64 and MINGW32 are legacy MSYS2 environments.
rem   If none is found, the script lists the GCC packages it knows and asks which one to install.
rem Far SDK: plugin.hpp is searched in this folder, FARSDK, .\PluginSDK and the PluginSDK\Headers.c
rem   folder of an installed Far; if not found, it can be downloaded from the official repository.
rem Version: Info->Version in adfdmsview.c must agree with VER_MAJOR, VER_MINOR, VER_PATCH and
rem   VER_BUILD in adfdmsview.rc, and VER_STRING must be consistent with them.
rem UPX: the DLLs are packed with --best --lzma (--ultra-brute too with "max"). If UPX is missing,
rem   it is installed with pacman if possible; otherwise the DLL stays unpacked.
rem
rem Optional environment variables:
rem   MSYS2_ROOT   MSYS2 folder (default C:\msys64; prefer a short ASCII path without spaces)
rem   MSYS2URL     installer URL used by the unattended MSYS2 installation
rem   ENV64        x64 GCC environments to try, in order (default: mingw64 only)
rem   ENV32        x86 GCC environments to try, in order (default: mingw32)
rem   FARSDK       folder containing plugin.hpp, or its Headers.c subfolder
rem   UPX_EXE      full path to upx.exe (otherwise PATH and MSYS2 folders are searched)
setlocal EnableExtensions DisableDelayedExpansion
pushd "%~dp0"
if errorlevel 1 (
  echo ERROR: cannot change to the folder of build.bat.
  exit /b 1
)

rem Use the folder of this script, not the caller's current directory. pushd also handles UNC paths.
set "HERE=%CD%\"
set "SRC=%HERE%adfdmsview.c"
set "RCFILE=%HERE%adfdmsview.rc"
set "OUTNAME=ADFDMSView.dll"
set "OBJDIR=_ADFDMSView_build"
set "SDK="
set "MSYS="
set "UPX="

rem ---- command line ----
set "DO64="
set "DO32="
set "USEUPX=1"
set "UPXMAX="
set "DOZIP="
:args
if "%~1"=="" goto args_done
if /i "%~1"=="x64" (
  set "DO64=1"
) else if /i "%~1"=="x86" (
  set "DO32=1"
) else if /i "%~1"=="noupx" (
  set "USEUPX=0"
) else if /i "%~1"=="max" (
  set "UPXMAX=1"
) else if /i "%~1"=="zip" (
  set "DOZIP=1"
) else (
  echo ERROR: unknown option "%~1".
  goto usage
)
shift
goto args
:args_done
if "%USEUPX%"=="0" if defined UPXMAX (
  echo ERROR: "max" and "noupx" cannot be used together.
  goto usage
)
if not defined DO64 if not defined DO32 (
  set "DO64=1"
  set "DO32=1"
)
set "ENV64_DEFAULT="
if not defined ENV64 (
  set "ENV64=mingw64"
  set "ENV64_DEFAULT=1"
)
if not defined ENV32 set "ENV32=mingw32"
if not defined MSYS2URL set "MSYS2URL=https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-x86_64-latest.exe"

rem ---- source files and distribution files ----
if not exist "%SRC%" (
  echo ERROR: source file "%SRC%" not found.
  goto fail
)
if not exist "%RCFILE%" (
  echo ERROR: resource file "%RCFILE%" not found.
  goto fail
)
if not exist "ADFDMSViewEng.lng" (
  echo ERROR: language file ADFDMSViewEng.lng not found.
  goto fail
)
if defined DOZIP for %%F in (ADFDMSView.txt CHANGELOG.TXT) do if not exist "%%F" echo ERROR: %%F is needed by the zip option.& goto fail
if defined DOZIP (
  where.exe tar.exe >nul 2>&1
  if errorlevel 1 (
    echo ERROR: tar.exe was not found; the zip option needs Windows 10 or later with tar.exe in PATH.
    goto fail
  )
)
set "LICFILE="
if exist "LICENSE" set "LICFILE=LICENSE"
if not defined LICFILE if exist "LICENSE.TXT" set "LICFILE=LICENSE.TXT"
if defined DOZIP if not defined LICFILE (
  echo ERROR: file LICENSE or LICENSE.TXT not found; it is needed by the zip option.
  goto fail
)

echo Source: %SRC%

rem ---- version: source and resource must agree ----
call :checkver
if errorlevel 1 goto fail

rem ---- Far Manager SDK header ----
call :findsdk
if errorlevel 1 goto fail

echo Far SDK: %SDK%

rem ---- MSYS2 folder ----
set "MSYSWANT=%MSYS2_ROOT%"
set "MSYSNEW="
if defined MSYS2_ROOT if exist "%MSYS2_ROOT%\usr\bin\bash.exe" set "MSYS=%MSYS2_ROOT%"
if not defined MSYS if exist "C:\msys64\usr\bin\bash.exe" set "MSYS=C:\msys64"
if not defined MSYS if exist "C:\msys2\usr\bin\bash.exe" set "MSYS=C:\msys2"
for %%D in (C D E F G) do if not defined MSYS if exist "%%D:\msys64\usr\bin\bash.exe" set "MSYS=%%D:\msys64"
if not defined MSYS (
  for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\MSYS2 64bit" /v InstallLocation 2^>nul ^| %SystemRoot%\System32\find.exe "InstallLocation"') do (
    if exist "%%B\usr\bin\bash.exe" set "MSYS=%%B"
  )
)
if not defined MSYS call :msysget
if not defined MSYS if errorlevel 1 goto fail
if not defined MSYS set "MSYS=%MSYS2_ROOT%"
if not exist "%MSYS%\usr\bin\bash.exe" (
  echo ERROR: MSYS2 shell not found in "%MSYS%".
  goto fail
)
set "MSYS2_ROOT=%MSYS%"
echo MSYS2: %MSYS%

rem ---- UPX ----
if "%USEUPX%"=="1" call :findupx
if "%USEUPX%"=="1" if defined UPXEXE echo UPX: %UPXEXE%
if "%USEUPX%"=="1" if not defined UPXEXE echo UPX not available: the DLLs will be left unpacked.

rem ---- compiler and linker flags (kept separate because they contain commas) ----
rem No -fdata-sections: it enlarges the zero-filled tables.
set "CFLAGS=-Os -fno-ident -fno-unwind-tables -fno-asynchronous-unwind-tables -fomit-frame-pointer -ffunction-sections -D__USE_MINGW_ANSI_STDIO=0 -DNDEBUG -Wall -Wno-unused-function"
set "LFLAGS=-shared -s -static-libgcc -nostartfiles -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,--build-id=none -Wl,--no-insert-timestamp -Wl,--file-alignment,512"

rem ---- builds ----
rem Call each build routine as a single IF command, never from a parenthesized IF block:
rem the routine uses labels and must return to the exact CALL site before the next
rem architecture starts.
set "FAILED="
if defined DO64 call :build x64 "%ENV64%" x64
if defined DO64 if errorlevel 1 set "FAILED=1"
if defined DO32 call :build x86 "%ENV32%" x86
if defined DO32 if errorlevel 1 set "FAILED=1"
rem Remove only our own temporary files; never recursively delete a possibly pre-existing folder.
for %%F in (res_x64.o res_x86.o ADFDMSView_x64.unpacked ADFDMSView_x86.unpacked) do if exist "%OBJDIR%\%%F" del /q "%OBJDIR%\%%F"
if exist "%OBJDIR%" rd "%OBJDIR%" 2>nul
if defined FAILED goto fail
if defined DOZIP call :mkzip
if defined DOZIP if errorlevel 1 goto fail

echo.
echo Build finished.
popd
exit /b 0

:usage
echo.
echo Usage: build.bat [x64] [x86] [noupx] [max] [zip]
echo   no option  builds both versions
echo   x64        builds only the 64-bit DLL into .\x64
echo   x86        builds only the 32-bit DLL into .\x86
echo   noupx      builds without UPX compression
echo   max        also asks UPX to try --ultra-brute ^(not with noupx^)
echo   zip        creates ADFDMSView-^<version^>.zip ^(needs tar.exe, Windows 10 or later^)
popd
exit /b 1

:fail
echo.
echo Build failed.
popd
exit /b 1

rem :findsdk - finds plugin.hpp in the project, FARSDK, or an installed Far; downloads it if needed
:findsdk
if exist "%HERE%plugin.hpp" (
  set "SDK=."
  exit /b 0
)

rem When FARSDK is explicitly set, use only that location (and its Headers.c subfolder).
if defined FARSDK set "SDK=%FARSDK%"
if defined FARSDK if not exist "%SDK%\plugin.hpp" if exist "%SDK%\Headers.c\plugin.hpp" set "SDK=%SDK%\Headers.c"
if defined FARSDK if exist "%SDK%\plugin.hpp" exit /b 0
if defined FARSDK echo ERROR: plugin.hpp not found in FARSDK="%FARSDK%" or its Headers.c subfolder.& goto sdk_failed

rem Default local SDK folder, then the usual installation folders of Far Manager.
set "SDK=PluginSDK"
if not exist "%SDK%\plugin.hpp" if exist "%SDK%\Headers.c\plugin.hpp" set "SDK=%SDK%\Headers.c"
if exist "%SDK%\plugin.hpp" exit /b 0
set "SDKFOUND="
for %%D in ("%ProgramFiles%" "%ProgramW6432%" "%ProgramFiles(x86)%") do (
  if not defined SDKFOUND if exist "%%~D\Far Manager\PluginSDK\Headers.c\plugin.hpp" (
    set "SDK=%%~D\Far Manager\PluginSDK\Headers.c"
    set "SDKFOUND=1"
  )
)
if defined SDKFOUND exit /b 0

rem The official header can be cached in .\PluginSDK for later builds.
set "SDK=%HERE%PluginSDK"
if not exist "%SDK%" mkdir "%SDK%"
if not exist "%SDK%" (
  echo ERROR: cannot create "%SDK%".
  goto sdk_failed
)
where curl.exe >nul 2>&1
if errorlevel 1 (
  echo plugin.hpp was not found and curl.exe is not available for downloading it.
  goto sdk_failed
)
echo plugin.hpp not found: downloading it from the official Far Manager repository ...
curl.exe -fsSL -o "%SDK%\plugin.hpp" https://raw.githubusercontent.com/FarGroup/FarManager/master/plugins/common/unicode/plugin.hpp
if errorlevel 1 del /q "%SDK%\plugin.hpp" 2>nul& goto sdk_download_failed
%SystemRoot%\System32\findstr.exe /c:"FARMANAGERVERSION_MAJOR" "%SDK%\plugin.hpp" >nul
if errorlevel 1 del /q "%SDK%\plugin.hpp" 2>nul& goto sdk_download_failed
echo Far SDK header downloaded into %SDK%.
exit /b 0

:sdk_download_failed
echo ERROR: plugin.hpp could not be downloaded or did not pass the header check.
:sdk_failed
echo        Copy Far's plugin.hpp into .\PluginSDK, set FARSDK to its folder,
echo        or install Far Manager with its PluginSDK\Headers.c folder.
exit /b 1

rem :msysget - MSYS2 was not found: offers unattended installation, or prints the official URL
:msysget
set "MSYSARCH=%PROCESSOR_ARCHITECTURE%"
if defined PROCESSOR_ARCHITEW6432 set "MSYSARCH=%PROCESSOR_ARCHITEW6432%"
if /i not "%MSYSARCH%"=="AMD64" goto msysget_manual
ver | %SystemRoot%\System32\findstr.exe /r /c:"\[Version 10\." >nul
if errorlevel 1 goto msysget_manual
set "MSYS_WINBUILD="
for /f "tokens=6 delims=. ]" %%B in ('ver') do set "MSYS_WINBUILD=%%B"
if not defined MSYS_WINBUILD goto msysget_manual
if %MSYS_WINBUILD% lss 17763 goto msysget_manual
where curl.exe >nul 2>&1
if errorlevel 1 goto msysget_manual
set "MSYSDIR=%MSYSWANT%"
if not defined MSYSDIR set "MSYSDIR=C:\msys64"
set "MSYSFWD=%MSYSDIR:\=/%"
set "MSYSSETUP=%TEMP%\build-msys2-setup.exe"

rem The installer must not write into a folder that already contains unrelated files.
if not exist "%MSYSDIR%\" goto msysget_free
dir /b "%MSYSDIR%" 2>nul | %SystemRoot%\System32\findstr.exe "^" >nul
if not errorlevel 1 goto msysget_notempty

:msysget_free
echo.
echo MSYS2 was not found. It provides the compilers used to build the DLLs.
echo   installer: %MSYS2URL%
echo   destination: %MSYSDIR%
echo The installer will run silently; MSYS2 will then be updated with pacman.
echo Nothing is downloaded or installed unless you choose 1.
echo.
echo         1  download and install MSYS2 now
echo         0  none     do not install; show the official download page
call :ask 1
if errorlevel 1 goto msysget_go
goto msysget_manual

:msysget_go
if exist "%MSYSSETUP%" del /q "%MSYSSETUP%"
echo.
echo Downloading the MSYS2 installer ...
curl.exe -f -L -# --retry 2 -o "%MSYSSETUP%" "%MSYS2URL%"
if errorlevel 1 goto msysget_dlfail
if not exist "%MSYSSETUP%" goto msysget_dlfail
for %%F in ("%MSYSSETUP%") do set "MSYSSIZE=%%~zF"
if %MSYSSIZE% lss 10000000 goto msysget_dlfail
echo.
echo Installing MSYS2 into %MSYSDIR% (this may take a few minutes) ...
start "" /wait "%MSYSSETUP%" in --confirm-command --accept-messages --root "%MSYSFWD%"
set "MSYSRC=%ERRORLEVEL%"
del /q "%MSYSSETUP%" 2>nul
if not "%MSYSRC%"=="0" echo The MSYS2 installer returned exit code %MSYSRC%; checking whether it completed.
if not exist "%MSYSDIR%\usr\bin\bash.exe" goto msysget_instfail
set "MSYS2_ROOT=%MSYSDIR%"
set "MSYSNEW=1"
echo.
echo MSYS2 is installed in %MSYS2_ROOT%. Initializing and updating it with pacman ...
"%MSYS2_ROOT%\usr\bin\bash.exe" -lc " "
if errorlevel 1 echo Warning: the first MSYS2 shell initialization returned an error.
"%MSYS2_ROOT%\usr\bin\bash.exe" -lc "pacman --noconfirm -Syuu"
if errorlevel 1 echo Warning: pacman update pass 1 failed; the compiler installation may need a manual update.
"%MSYS2_ROOT%\usr\bin\bash.exe" -lc "pacman --noconfirm -Syuu"
if errorlevel 1 echo Warning: pacman update pass 2 failed; the compiler installation may need a manual update.
exit /b 0

:msysget_notempty
echo.
echo MSYS2 was not found, but "%MSYSDIR%" exists and is not empty.
echo Choose an empty folder or set MSYS2_ROOT to another location, then run build.bat again.
goto msysget_manual

:msysget_dlfail
if exist "%MSYSSETUP%" del /q "%MSYSSETUP%"
echo.
echo ERROR: the MSYS2 installer could not be downloaded from %MSYS2URL%.
goto msysget_manual

:msysget_instfail
echo.
echo ERROR: MSYS2 installation did not complete: "%MSYSDIR%\usr\bin\bash.exe" was not created.
:msysget_manual
echo.
echo ERROR: MSYS2 was not found or could not be installed automatically.
echo        Download it from the official website: https://www.msys2.org
exit /b 1

rem :build <arch> <environments to try> <output folder>
rem Keep this routine linear: it is called outside parenthesized IF blocks and returns with EXIT /B.
:build
setlocal
set "B_ARCH=%~1"
set "B_ENVS=%~2"
set "B_OUT=%~3"
set "B_BIN="
set "B_ENV="
set "SELENV="
set "B_INSTALL_ENVS=%B_ENVS%"
if /i "%B_ARCH%"=="x64" if defined ENV64_DEFAULT set "B_INSTALL_ENVS=mingw64 ucrt64"
for %%E in (%B_ENVS%) do call :probe %%E
if not defined B_BIN call :install "%B_INSTALL_ENVS%"
if not defined B_BIN if errorlevel 1 goto build_failed
if not defined B_BIN if defined SELENV call :probe %SELENV%
if not defined B_BIN echo [%B_ARCH%] ERROR: gcc.exe and windres.exe are not available; this build cannot continue.& goto build_failed

set "PATH=%B_BIN%;%MSYS%\usr\bin;%PATH%"
echo.
echo ==== Building %B_ARCH% with %B_ENV% ====
echo Compiler folder: %B_BIN%
if /i "%B_ENV%"=="mingw64" echo Runtime: MSVCRT ^(MINGW64; compact DLL target^)
if /i "%B_ENV%"=="ucrt64" echo Runtime: UCRT ^(UCRT64; explicitly selected^)
if /i "%B_ENV%"=="mingw32" echo Runtime: MSVCRT ^(MINGW32^)

if not exist "%OBJDIR%" mkdir "%OBJDIR%"
if not exist "%OBJDIR%" echo [%B_ARCH%] ERROR: cannot create the temporary build folder "%OBJDIR%".& goto build_failed
if not exist "%B_OUT%" mkdir "%B_OUT%"
if not exist "%B_OUT%" echo [%B_ARCH%] ERROR: cannot create output folder "%B_OUT%".& goto build_failed
if exist "%B_OUT%\%OUTNAME%" del /q "%B_OUT%\%OUTNAME%"
if exist "%B_OUT%\%OUTNAME%" echo [%B_ARCH%] ERROR: cannot remove the previous DLL "%B_OUT%\%OUTNAME%".& goto build_failed

rem Compile the Windows version resource and link it into the DLL.
echo [%B_ARCH%] Compiling version resource ...
"%B_BIN%\windres.exe" -I"%SDK%" -O coff -i "%RCFILE%" -o "%OBJDIR%\res_%B_ARCH%.o"
if errorlevel 1 echo [%B_ARCH%] ERROR: resource compilation failed.& goto build_failed

set "B_EXTRA=-Wl,-e,DllMain"
if /i "%B_ARCH%"=="x86" set "B_EXTRA=-Wl,--kill-at -Wl,-e,_DllMain@12"
echo [%B_ARCH%] Compiling and linking with %B_ENV% GCC ...
"%B_BIN%\gcc.exe" %CFLAGS% -I"%SDK%" %LFLAGS% %B_EXTRA% -o "%B_OUT%\%OUTNAME%" "%SRC%" "%OBJDIR%\res_%B_ARCH%.o"
set "B_RC=%ERRORLEVEL%"
if exist "%OBJDIR%\res_%B_ARCH%.o" del /q "%OBJDIR%\res_%B_ARCH%.o"
if not "%B_RC%"=="0" echo [%B_ARCH%] ERROR: compilation or linking failed ^(exit code %B_RC%^).& del /q "%B_OUT%\%OUTNAME%" 2>nul& goto build_failed

echo [%B_ARCH%] Checking Far exports ...
call :chkexp
if errorlevel 1 del /q "%B_OUT%\%OUTNAME%" 2>nul& goto build_failed

rem Pack the DLL, if UPX is available. Preserve the plain DLL if UPX fails.
if "%USEUPX%"=="0" goto copy_languages
if not defined UPXEXE echo [%B_ARCH%] Note: UPX is not available; the DLL is left unpacked.& goto copy_languages
set "UPXOPT=--best --lzma"
if defined UPXMAX set "UPXOPT=--best --ultra-brute --lzma"
for %%F in ("%B_OUT%\%OUTNAME%") do set "SIZE0=%%~zF"
set "B_BACKUP=%OBJDIR%\ADFDMSView_%B_ARCH%.unpacked"
copy /b /y "%B_OUT%\%OUTNAME%" "%B_BACKUP%" >nul
if errorlevel 1 echo [%B_ARCH%] Warning: cannot make a backup; UPX is skipped and the DLL stays unpacked.& goto copy_languages
if defined UPXMAX echo [%B_ARCH%] UPX --ultra-brute can take a long time ...
echo [%B_ARCH%] Packing with UPX (%UPXOPT%) ...
"%UPXEXE%" %UPXOPT% -q "%B_OUT%\%OUTNAME%"
if errorlevel 1 goto upx_failed
for %%F in ("%B_OUT%\%OUTNAME%") do echo [%B_ARCH%] UPX: %SIZE0% bytes before, %%~zF bytes after.
del /q "%B_BACKUP%" 2>nul
goto copy_languages

:upx_failed
copy /b /y "%B_BACKUP%" "%B_OUT%\%OUTNAME%" >nul
if errorlevel 1 del /q "%B_OUT%\%OUTNAME%" "%B_BACKUP%" 2>nul& echo [%B_ARCH%] ERROR: UPX failed and the unpacked DLL could not be restored.& goto build_failed
del /q "%B_BACKUP%" 2>nul
echo [%B_ARCH%] Warning: UPX failed; the unpacked DLL has been restored.

:copy_languages
copy /y "ADFDMSView*.lng" "%B_OUT%\" >nul
if errorlevel 1 echo [%B_ARCH%] ERROR: could not copy ADFDMSView language files into "%B_OUT%".& goto build_failed
for %%F in ("%B_OUT%\%OUTNAME%") do echo [%B_ARCH%] Built: %%~fF - %%~zF bytes
endlocal
exit /b 0

:build_failed
if exist "%OBJDIR%\res_%B_ARCH%.o" del /q "%OBJDIR%\res_%B_ARCH%.o"
endlocal
exit /b 1

rem :probe <environment> - selects the first environment with gcc.exe and windres.exe
:probe
if defined B_BIN exit /b 0
if /i "%B_ARCH%"=="x64" if /i not "%~1"=="ucrt64" if /i not "%~1"=="mingw64" exit /b 0
if /i "%B_ARCH%"=="x86" if /i not "%~1"=="mingw32" exit /b 0
if exist "%MSYS%\%~1\bin\gcc.exe" if exist "%MSYS%\%~1\bin\windres.exe" (
  set "B_BIN=%MSYS%\%~1\bin"
  set "B_ENV=%~1"
)
exit /b 0

rem :install <environment list> - asks which compatible GCC package to install with pacman
:install
echo.
if defined MSYSNEW echo [%B_ARCH%] MSYS2 was just installed and has no compiler yet.
if not defined MSYSNEW echo [%B_ARCH%] No complete GCC toolchain was found in: %~1
set "NOPT=0"
for %%E in (%~1) do call :addopt %%E
if "%NOPT%"=="0" goto install_unknown
echo         0  none     install nothing; this DLL will not be built
echo.
call :ask %NOPT%
set "PICK=%ERRORLEVEL%"
if "%PICK%"=="0" goto install_none
set "N=0"
set "SELPKG="
for %%E in (%~1) do call :selopt %%E
if not defined SELPKG goto install_none
echo.
echo [%B_ARCH%] Installing %SELPKG% with pacman ...
"%MSYS%\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm %SELPKG%"
if errorlevel 1 goto install_failed
exit /b 0

:install_unknown
echo [%B_ARCH%] ERROR: no installable GCC package is known for these environments.
echo [%B_ARCH%] Open an MSYS2 shell, update it with pacman -Syu, then run build.bat again.
exit /b 1

:install_none
echo [%B_ARCH%] Compiler installation skipped; this DLL will not be built.
exit /b 1

:install_failed
echo.
echo [%B_ARCH%] ERROR: pacman could not install %SELPKG%; see its messages above.
echo [%B_ARCH%] If MSYS2 is old, update it in an MSYS2 shell with pacman -Syu, then run build.bat again.
exit /b 1

rem :addopt <environment> - adds an install menu choice when a package is known
:addopt
call :pkgfor %~1
if not defined PKG exit /b 0
if "%NOPT%"=="0" echo [%B_ARCH%] Choose the GCC toolchain to install with pacman:
set /a NOPT+=1
echo         %NOPT%  %PKGTXT%
exit /b 0

rem :selopt <environment> - selects package number PICK
:selopt
call :pkgfor %~1
if not defined PKG exit /b 0
set /a N+=1
if "%N%"=="%PICK%" (
  set "SELPKG=%PKG%"
  set "SELENV=%~1"
)
exit /b 0

rem :pkgfor <environment> - package name and short description for a supported GCC environment
:pkgfor
set "PKG="
set "PKGTXT="
if /i "%B_ARCH%"=="x64" if /i "%~1"=="mingw64" (
  set "PKG=mingw-w64-x86_64-gcc"
  set "PKGTXT=MINGW64 mingw-w64-x86_64-gcc       default x64 GCC (MSVCRT runtime; smaller DLLs)"
)
if /i "%B_ARCH%"=="x64" if /i "%~1"=="ucrt64" (
  set "PKG=mingw-w64-ucrt-x86_64-gcc"
  set "PKGTXT=UCRT64  mingw-w64-ucrt-x86_64-gcc  optional x64 GCC (UCRT runtime)"
)
if /i "%B_ARCH%"=="x86" if /i "%~1"=="mingw32" (
  set "PKG=mingw-w64-i686-gcc"
  set "PKGTXT=MINGW32 mingw-w64-i686-gcc         x86 GCC (32-bit; legacy environment)"
)
exit /b 0

rem :findupx - searches PATH/MSYS2 and installs UPX with pacman if it is not found
:findupx
set "UPXEXE="
if defined UPX_EXE if exist "%UPX_EXE%" set "UPXEXE=%UPX_EXE%"
if not defined UPXEXE for /f "delims=" %%U in ('where.exe upx.exe 2^>nul') do if not defined UPXEXE set "UPXEXE=%%U"
for %%P in ("%MSYS%\mingw64\bin" "%MSYS%\mingw32\bin" "%MSYS%\ucrt64\bin" "%MSYS%\usr\bin") do (
  if not defined UPXEXE if exist "%%~P\upx.exe" set "UPXEXE=%%~P\upx.exe"
)
if defined UPXEXE exit /b 0

rem Prefer a native MinGW UPX executable: it runs directly from cmd.exe and accepts Windows paths.
set "UPXPKG=mingw-w64-x86_64-upx"
echo UPX was not found; trying to install the MINGW64 package %UPXPKG% with pacman ...
"%MSYS%\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm %UPXPKG%"
if exist "%MSYS%\mingw64\bin\upx.exe" set "UPXEXE=%MSYS%\mingw64\bin\upx.exe"
if not defined UPXEXE if exist "%MSYS%\ucrt64\bin\upx.exe" set "UPXEXE=%MSYS%\ucrt64\bin\upx.exe"
if not defined UPXEXE echo Warning: UPX could not be installed; the DLLs will be built unpacked.
exit /b 0

rem :checkver - source Info->Version and resource version must agree
:checkver
rem In FOR /F command strings, leave find.exe unquoted: cmd /c otherwise misparses the command line.
set "CV="
for /f "tokens=1,4-7 delims=(), " %%a in ('%SystemRoot%\System32\find.exe "Info->Version" "%SRC%"') do (
  if "%%a"=="Info->Version" set "CV=%%b.%%c.%%d.%%e"
)
if not defined CV (
  echo ERROR: Info-^>Version was not found in %SRC%.
  exit /b 1
)
for %%V in (MAJOR MINOR PATCH BUILD) do (
  for /f "tokens=2,3" %%A in ('%SystemRoot%\System32\find.exe "#define VER_%%V " "%RCFILE%"') do (
    if "%%A"=="VER_%%V" set "RV_%%V=%%B"
  )
)
set "RCV=%RV_MAJOR%.%RV_MINOR%.%RV_PATCH%.%RV_BUILD%"
if not "%CV%"=="%RCV%" (
  echo ERROR: version mismatch: %SRC% has %CV%, %RCFILE% has %RCV%.
  exit /b 1
)
set "VERSTR=%RV_MAJOR%.%RV_MINOR%.%RV_PATCH%"
if not "%RV_BUILD%"=="0" set "VERSTR=%VERSTR%.%RV_BUILD%"
set "RS="
for /f "tokens=3" %%N in ('%SystemRoot%\System32\find.exe "#define VER_STRING " "%RCFILE%"') do set "RS=%%~N"
if not "%RS%"=="%VERSTR%" (
  echo ERROR: VER_STRING in %RCFILE% is "%RS%", expected "%VERSTR%".
  exit /b 1
)
echo Version: %VERSTR%
exit /b 0

rem :chkexp - verifies all Far plug-in entry points are exported with their expected names
:chkexp
if not exist "%B_BIN%\objdump.exe" (
  echo [%B_ARCH%] Warning: objdump.exe not found; export check skipped.
  exit /b 0
)
rem Keep the FOR body as a CALL to a separate routine. An unescaped parenthesis in an
rem diagnostic inside nested FOR/IF blocks can terminate a CMD block while it is parsed.
set "CHKEXP_FAILED="
for %%X in (GetGlobalInfoW SetStartupInfoW GetPluginInfoW AnalyseW OpenW GetOpenPanelInfoW GetFindDataW FreeFindDataW ClosePanelW SetDirectoryW GetFilesW PutFilesW DeleteFilesW MakeDirectoryW) do call :chkexp_one %%X
if defined CHKEXP_FAILED exit /b 1
exit /b 0

:chkexp_one
"%B_BIN%\objdump.exe" -p "%B_OUT%\%OUTNAME%" | %SystemRoot%\System32\findstr.exe /r /c:"[^A-Za-z0-9_@]%~1$" >nul
if errorlevel 1 (
  echo [%B_ARCH%] ERROR: export %~1 not found in %B_OUT%\%OUTNAME% - check the linker export options.
  set "CHKEXP_FAILED=1"
)
exit /b 0

rem :mkzip - creates ADFDMSView-<version>.zip with the built DLL folders and documentation
:mkzip
setlocal
set "ZIPLIST=ADFDMSView.txt CHANGELOG.TXT %LICFILE%"
if defined DO64 set "ZIPLIST=%ZIPLIST% x64"
if defined DO32 set "ZIPLIST=%ZIPLIST% x86"
set "ZIPFILE=ADFDMSView-%VERSTR%.zip"
if exist "%ZIPFILE%" del /q "%ZIPFILE%"
tar.exe -a -c -f "%ZIPFILE%" %ZIPLIST%
if errorlevel 1 (
  echo ERROR: tar.exe could not create %ZIPFILE%.
  endlocal
  exit /b 1
)
for %%F in ("%ZIPFILE%") do echo Package: %%~fF - %%~zF bytes
endlocal
exit /b 0

rem :ask <highest choice> - asks for a number from 0 to N; returns the number in ERRORLEVEL
:ask
setlocal EnableDelayedExpansion
set "ASKMAX=%~1"
set "TRIES=0"
:ask_again
set "CH="
set /p "CH=Your choice (0-!ASKMAX!): "
set "PICKED=-1"
for /l %%I in (0,1,!ASKMAX!) do if "!CH!"=="%%I" set "PICKED=%%I"
if !PICKED! geq 0 goto ask_return
set /a TRIES+=1
if !TRIES! lss 3 (
  echo Please type a number from 0 to !ASKMAX!.
  goto ask_again
)
echo No valid choice; treating it as 0 (none).
endlocal
exit /b 0
:ask_return
endlocal & exit /b %PICKED%
