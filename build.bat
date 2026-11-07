@echo off
rem ===========================================================================
rem  ADFDMSView - Far Manager 3 plug-in: build script (MSYS2/MinGW-w64)
rem  Copyright (c) 2026 Marco Pistella - MIT License (see the file LICENSE)
rem
rem  Usage:  build.bat [x64] [x86] [noupx] [zip]
rem            x64    build the 64 bit DLL  -> .\x64\ADFDMSView.dll
rem            x86    build the 32 bit DLL  -> .\x86\ADFDMSView.dll
rem            (none) build both
rem            noupx  do not compress the DLLs with UPX (by default they are compressed)
rem            zip    also create ADFDMSView-<version>.zip (for example ADFDMSView-2.3.0.zip), with the folders that were built,
rem                   ADFDMSView.txt and LICENSE; needs tar.exe (Windows 10 or later)
rem  UPX is taken from the UPX_EXE variable, the PATH or the MSYS2 folders; if it is
rem  not found, the MSYS2 package mingw-w64-x86_64-upx is installed with pacman.
rem
rem  Needs: MSYS2 (default C:\msys64, or set MSYS2_ROOT) with the packages
rem         mingw-w64-x86_64-gcc (64 bit) and mingw-w64-i686-gcc (32 bit).
rem         A missing package is installed with pacman automatically.
rem         plugin.hpp of Far 3 in .\PluginSDK, or in the folder named by the FARSDK variable; if it is
rem         not directly there, its Headers.c subfolder and the PluginSDK\Headers.c folder of an
rem         installed Far are tried.
rem
rem  Version: the build stops if the version in adfdmsview.rc (VER_MAJOR ... VER_STRING) differs from
rem  the one of Info->Version in GetGlobalInfoW (adfdmsview.c).
rem ===========================================================================
setlocal EnableExtensions DisableDelayedExpansion

rem Everything is taken from the folder of this script, and from nowhere else: the source files are named with
rem their full path, and the script stops if it cannot make that folder the current one (for example a UNC path).
set "HERE=%~dp0"
cd /d "%HERE%"
if /i not "%CD%\"=="%HERE%" if /i not "%CD%"=="%HERE%" (
    echo ERROR: cannot change to the folder of build.bat: "%HERE%"
    echo        Run it from a local or a mapped drive, not from a UNC path.
    exit /b 1
)

rem UPX itself reads the environment variable UPX as a list of options: it must not be set
set "UPX="

set "SRC=%HERE%adfdmsview.c"
set "RCFILE=%HERE%adfdmsview.rc"
set "OUTNAME=ADFDMSView.dll"
set "SDK=PluginSDK"

set "WANT64=0"
set "WANT32=0"
set "USEUPX=1"
set "DOZIP=0"
:args
if "%~1"=="" goto argsdone
if /i "%~1"=="x64" (
    set "WANT64=1"
) else if /i "%~1"=="x86" (
    set "WANT32=1"
) else if /i "%~1"=="noupx" (
    set "USEUPX=0"
) else if /i "%~1"=="zip" (
    set "DOZIP=1"
) else (
    echo Unknown option "%~1".
    echo Usage: build.bat [x64] [x86] [noupx] [zip]
    exit /b 1
)
shift
goto args
:argsdone
if "%WANT64%%WANT32%"=="00" (
    set "WANT64=1"
    set "WANT32=1"
)

rem ---- checks ----------------------------------------------------------------
if not exist "%SRC%" (
    echo ERROR: source file "%SRC%" not found in %CD%
    exit /b 1
)
if not exist "%RCFILE%" (
    echo ERROR: resource file "%RCFILE%" not found in %CD%
    exit /b 1
)
rem the license file is LICENSE (or LICENSE.TXT)
set "LICFILE="
if exist "LICENSE" set "LICFILE=LICENSE"
if not defined LICFILE if exist "LICENSE.TXT" set "LICFILE=LICENSE.TXT"
if "%DOZIP%"=="1" if not defined LICFILE (
    echo ERROR: file LICENSE not found, it is needed by the zip option.
    exit /b 1
)
rem plugin.hpp: .\PluginSDK (or FARSDK), then its Headers.c subfolder, then the SDK of an installed Far
if defined FARSDK set "SDK=%FARSDK%"
if not exist "%SDK%\plugin.hpp" if exist "%SDK%\Headers.c\plugin.hpp" set "SDK=%SDK%\Headers.c"
set "SDKFOUND="
if not exist "%SDK%\plugin.hpp" for %%D in ("%ProgramFiles%" "%ProgramW6432%" "%ProgramFiles(x86)%") do if not defined SDKFOUND if exist "%%~D\Far Manager\PluginSDK\Headers.c\plugin.hpp" set "SDK=%%~D\Far Manager\PluginSDK\Headers.c" & set "SDKFOUND=1"
if not exist "%SDK%\plugin.hpp" (
    echo ERROR: plugin.hpp of Far 3 not found in "%SDK%".
    echo        Put it in .\PluginSDK, or set FARSDK to the folder that contains it.
    exit /b 1
)

echo Source: %SRC%

rem ---- version: adfdmsview.rc must agree with Info->Version of adfdmsview.c ----------------------
call :checkver
if errorlevel 1 exit /b 1

rem ---- MSYS2 location ----------------------------------------------------------
set "MSYS="
if defined MSYS2_ROOT if exist "%MSYS2_ROOT%\usr\bin\bash.exe" set "MSYS=%MSYS2_ROOT%"
if not defined MSYS if exist "C:\msys64\usr\bin\bash.exe" set "MSYS=C:\msys64"
if not defined MSYS if exist "C:\msys2\usr\bin\bash.exe" set "MSYS=C:\msys2"
if not defined MSYS if exist "%SystemDrive%\msys64\usr\bin\bash.exe" set "MSYS=%SystemDrive%\msys64"
if not defined MSYS (
    echo ERROR: MSYS2 not found. Install it in C:\msys64 or set MSYS2_ROOT.
    exit /b 1
)
echo MSYS2 found in %MSYS%

rem ---- UPX ---------------------------------------------------------------------
set "UPXEXE="
if "%USEUPX%"=="1" call :findupx
if "%USEUPX%"=="1" if not defined UPXEXE (
    echo ERROR: UPX not found and it could not be installed. Use the noupx option to build without it.
    exit /b 1
)
if defined UPXEXE echo UPX found: %UPXEXE%

rem ---- compiler flags (kept in variables: they contain commas) -----------------
rem Size: -Os, no unwind tables, no identification strings, section GC, stripped, no timestamp, no
rem       build-id, 512 bytes file alignment, msvcrt printf instead of the (big) MinGW one, no C runtime
rem       start-up code (-nostartfiles: the plug-in has no constructors; DllMain is the entry point).
rem NOTE: no -fdata-sections: with it MinGW puts the zero-filled static tables in .data (+2.4 MB on file).
set "CFLAGS=-Os -fno-ident -fno-unwind-tables -fno-asynchronous-unwind-tables -fomit-frame-pointer -ffunction-sections -D__USE_MINGW_ANSI_STDIO=0 -DNDEBUG -Wall -Wno-unused-function"
set "LFLAGS=-shared -s -static-libgcc -nostartfiles -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,--build-id=none -Wl,--no-insert-timestamp -Wl,--file-alignment,512"

set "RC=0"

if "%WANT64%"=="1" (
    call :build x64 mingw64 "-Wl,-e,DllMain" "mingw-w64-x86_64-gcc"
    if errorlevel 1 set "RC=1"
)
if "%WANT32%"=="1" (
    rem a 32 bit Far plug-in exports stdcall functions with undecorated names: --kill-at
    call :build x86 mingw32 "-Wl,--kill-at -Wl,-e,_DllMain@12" "mingw-w64-i686-gcc"
    if errorlevel 1 set "RC=1"
)

if "%RC%"=="0" if "%DOZIP%"=="1" call :mkzip
if "%RC%"=="0" if "%DOZIP%"=="1" if errorlevel 1 set "RC=1"

echo.
if "%RC%"=="0" (echo Build finished.) else (echo Build FAILED.)
exit /b %RC%


rem ===========================================================================
rem  :build <outdir> <msys subdir> <extra link flags> <pacman package>
rem ===========================================================================
:build
setlocal
set "B_OUT=%~1"
set "B_BIN=%MSYS%\%~2\bin"
set "B_GCC=%B_BIN%\gcc.exe"
set "B_EXTRA=%~3"
set "B_PKG=%~4"

echo.
echo ==== Building %B_OUT% ====

if not exist "%B_GCC%" (
    echo Compiler for %B_OUT% not found, installing %B_PKG% with pacman...
    "%MSYS%\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm %B_PKG%"
    if not exist "%B_GCC%" (
        echo ERROR: compiler "%B_GCC%" is not available.
        exit /b 1
    )
)

if not exist "%B_OUT%" mkdir "%B_OUT%"
if exist "%B_OUT%\%OUTNAME%" del /q "%B_OUT%\%OUTNAME%"

rem the compiler needs its own DLLs on the PATH
set "PATH=%B_BIN%;%MSYS%\usr\bin;%PATH%"

rem version resource (adfdmsview.rc): compiled to an object file that is linked into the DLL
"%B_BIN%\windres.exe" -I"%SDK%" -O coff -i "%RCFILE%" -o "%B_OUT%\version.o"
if errorlevel 1 (
    echo ERROR: resource compilation of %B_OUT% failed.
    exit /b 1
)

"%B_GCC%" %CFLAGS% -I"%SDK%" %LFLAGS% %B_EXTRA% -o "%B_OUT%\%OUTNAME%" "%SRC%" "%B_OUT%\version.o"
set "B_RC=%ERRORLEVEL%"
del /q "%B_OUT%\version.o" 2>nul
if not "%B_RC%"=="0" (
    echo ERROR: compilation of %B_OUT% failed.
    exit /b 1
)

rem the entry points must be exported with the names that Far looks for
call :chkexp
if errorlevel 1 exit /b 1

rem language files (ADFDMSView<Language>.lng) go next to the DLL
copy /y "ADFDMSView*.lng" "%B_OUT%\" >nul
if errorlevel 1 (
    echo ERROR: no language file ADFDMSView*.lng found in %CD%
    exit /b 1
)

for %%F in ("%B_OUT%\%OUTNAME%") do echo Compiled: %B_OUT%\%OUTNAME% - %%~zF bytes
if defined UPXEXE (
    "%UPXEXE%" --best --lzma -q "%B_OUT%\%OUTNAME%" >nul
    if errorlevel 1 (
        echo ERROR: UPX failed on %B_OUT%\%OUTNAME%
        exit /b 1
    )
    for %%F in ("%B_OUT%\%OUTNAME%") do echo Compressed with UPX: %B_OUT%\%OUTNAME% - %%~zF bytes
)
exit /b 0


rem ===========================================================================
rem  :findupx - sets UPXEXE to the full path of upx.exe (UPX_EXE, PATH, MSYS2; else installs it)
rem ===========================================================================
:findupx
if defined UPX_EXE if exist "%UPX_EXE%" set "UPXEXE=%UPX_EXE%"
if not defined UPXEXE for /f "delims=" %%U in ('where upx.exe 2^>nul') do if not defined UPXEXE set "UPXEXE=%%U"
if not defined UPXEXE if exist "%MSYS%\mingw64\bin\upx.exe" set "UPXEXE=%MSYS%\mingw64\bin\upx.exe"
if not defined UPXEXE if exist "%MSYS%\usr\bin\upx.exe" set "UPXEXE=%MSYS%\usr\bin\upx.exe"
if not defined UPXEXE (
    echo UPX not found, installing mingw-w64-x86_64-upx with pacman...
    "%MSYS%\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm mingw-w64-x86_64-upx"
    if exist "%MSYS%\mingw64\bin\upx.exe" set "UPXEXE=%MSYS%\mingw64\bin\upx.exe"
)
exit /b 0


rem ===========================================================================
rem  :checkver - reads the version of Info->Version (adfdmsview.c) and the one of adfdmsview.rc: they must
rem  be equal. Sets VERSTR (major.minor.patch). Uses the find.exe of Windows (not the one of MSYS2).
rem ===========================================================================
:checkver
set "CV="
for /f "tokens=1,4-7 delims=(), " %%a in ('%SystemRoot%\System32\find.exe "Info->Version" "%SRC%"') do if "%%a"=="Info->Version" set "CV=%%b.%%c.%%d.%%e"
if not defined CV echo ERROR: the version (Info-^>Version) was not found in %SRC%. & exit /b 1
for %%V in (MAJOR MINOR PATCH BUILD) do for /f "tokens=2,3" %%A in ('%SystemRoot%\System32\find.exe "#define VER_%%V " "%RCFILE%"') do if "%%A"=="VER_%%V" set "RV_%%V=%%B"
set "RCV=%RV_MAJOR%.%RV_MINOR%.%RV_PATCH%.%RV_BUILD%"
set "RS="
for /f "tokens=3" %%N in ('%SystemRoot%\System32\find.exe "#define VER_STRING " "%RCFILE%"') do set "RS=%%~N"
if not "%CV%"=="%RCV%" echo ERROR: version mismatch: %SRC% has %CV%, %RCFILE% has %RCV%. & exit /b 1
rem the version shown (VER_STRING, the name of the zip): major.minor.patch (the build number is shown only if it is not 0)
set "VERSTR=%RV_MAJOR%.%RV_MINOR%.%RV_PATCH%"
if not "%RV_BUILD%"=="0" set "VERSTR=%VERSTR%.%RV_BUILD%"
if not "%RS%"=="%VERSTR%" echo ERROR: VER_STRING in %RCFILE% is "%RS%", expected "%VERSTR%". & exit /b 1
exit /b 0


rem ===========================================================================
rem  :chkexp - checks that the DLL exports the entry points of the plug-in with undecorated names
rem  (the source has no __declspec(dllexport): the linker exports the functions that are not static)
rem ===========================================================================
:chkexp
if not exist "%B_BIN%\objdump.exe" exit /b 0
for %%X in (GetGlobalInfoW SetStartupInfoW GetPluginInfoW AnalyseW OpenW GetOpenPanelInfoW GetFindDataW FreeFindDataW ClosePanelW SetDirectoryW GetFilesW PutFilesW DeleteFilesW MakeDirectoryW) do "%B_BIN%\objdump.exe" -p "%B_OUT%\%OUTNAME%" | findstr /r /c:"[^A-Za-z0-9_@]%%X$" >nul || (echo ERROR: export %%X not found in %B_OUT%\%OUTNAME% ^(check --kill-at^) & exit /b 1)
exit /b 0


rem ===========================================================================
rem  :mkzip - creates ADFDMSView-<version>.zip with the folders that were built, ADFDMSView.txt and LICENSE
rem ===========================================================================
:mkzip
setlocal
set "ZIPLIST=ADFDMSView.txt %LICFILE%"
if "%WANT64%"=="1" set "ZIPLIST=%ZIPLIST% x64"
if "%WANT32%"=="1" set "ZIPLIST=%ZIPLIST% x86"
set "ZIPFILE=ADFDMSView-%VERSTR%.zip"
if exist "%ZIPFILE%" del /q "%ZIPFILE%"
tar.exe -a -c -f "%ZIPFILE%" %ZIPLIST%
if errorlevel 1 echo ERROR: tar.exe could not create %ZIPFILE%. & exit /b 1
for %%F in ("%ZIPFILE%") do echo %%~fF - %%~zF bytes
exit /b 0
