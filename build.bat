@echo off
setlocal enabledelayedexpansion

rem Destination directory for all build outputs
set "DIST_DIR=%~dp0dist"
if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"

rem Path to MinGW gcc and windres
if exist "D:\msys64\mingw64\bin\gcc.exe" (
    set "CC=D:\msys64\mingw64\bin\gcc.exe"
    set "WINDRES=D:\msys64\mingw64\bin\windres.exe"
    set "MSYS_BIN=D:\msys64\mingw64\bin"
) else if exist "E:\msys64\mingw64\bin\gcc.exe" (
    set "CC=E:\msys64\mingw64\bin\gcc.exe"
    set "WINDRES=E:\msys64\mingw64\bin\windres.exe"
    set "MSYS_BIN=E:\msys64\mingw64\bin"
) else (
    set "CC=gcc"
    set "WINDRES=windres"
    if exist "D:\msys64\mingw64\bin" (
        set "MSYS_BIN=D:\msys64\mingw64\bin"
    ) else if exist "E:\msys64\mingw64\bin" (
        set "MSYS_BIN=E:\msys64\mingw64\bin"
    )
)

rem Compile resources directly into dist
echo [1/3] Compiling Windows resources...
%WINDRES% -i "%~dp0resources.rc" -o "%DIST_DIR%\resources.o"
if %errorlevel% neq 0 (
    echo Resource compilation failed.
    exit /b 1
)

rem Compilation flags
set CFLAGS=-Wall -O2

rem Include and library directories
set INCDIR=D:\msys64\mingw64\include
set LIBDIR=D:\msys64\mingw64\lib

rem Source files
set SRCS=src\main.c src\ai_agent.c src\pdf_converter.c src\manga_translator.c src\db_migration.c src\config.c src\lang.c src\scrapers\scrapers.c src\scrapers\scraper_manganato.c src\scrapers\scraper_mgeko.c src\scrapers\scraper_asura.c src\scrapers\scraper_generic.c

if "%1"=="--static" goto build_static
if "%2"=="--static" goto build_static

rem Linker flags (resources, libcurl, sqlite3, comctl32, cJSON, Unicode, Windows subsystem, Shell/OLE, WIC, GDI)
set LDFLAGS="%DIST_DIR%\resources.o" -lcurl -lsqlite3 -lcomctl32 -lcjson -municode -mwindows -lole32 -lshell32 -lwindowscodecs -loleaut32 -lgdi32

echo [2/3] Compiling manga_downloader.exe into dist...
%CC% %CFLAGS% -I%INCDIR% -L%LIBDIR% -o "%DIST_DIR%\manga_downloader.exe" %SRCS% %LDFLAGS%

if %errorlevel% neq 0 (
    echo Build failed with error code %errorlevel%.
    exit /b %errorlevel%
)
goto post_build

:build_static
set STATIC_LDFLAGS="%DIST_DIR%\resources.o" -DCURL_STATICLIB -static -lcurl -lsqlite3 -lcjson -lssl -lcrypto -lnghttp2 -lnghttp3 -lngtcp2 -lngtcp2_crypto_ossl -lssh2 -lzstd -lbrotlidec -lbrotlicommon -lidn2 -lpsl -lunistring -lintl -liconv -lz -lcomctl32 -municode -mwindows -lole32 -lshell32 -lwindowscodecs -loleaut32 -lgdi32 -lws2_32 -lcrypt32 -lwldap32 -liphlpapi -lbcrypt -lsecur32 -s
echo [2/3] Compiling manga_downloader_standalone.exe into dist (static)...
%CC% %CFLAGS% -o "%DIST_DIR%\manga_downloader_standalone.exe" %SRCS% %STATIC_LDFLAGS%
if %errorlevel% neq 0 (
    echo Static build failed with error code %errorlevel%.
    exit /b %errorlevel%
)

:post_build
rem Cleanup temporary resource object from dist and root
if exist "%DIST_DIR%\resources.o" del /f /q "%DIST_DIR%\resources.o"
if exist "%~dp0resources.o" del /f /q "%~dp0resources.o"
if exist "%~dp0manga_downloader.exe" del /f /q "%~dp0manga_downloader.exe"
if exist "%~dp0manga_downloader_standalone.exe" del /f /q "%~dp0manga_downloader_standalone.exe"

echo [3/3] Copying runtime dependencies to dist...
rem Copy database and config if they exist
if exist "%~dp0manga_downloader.db" copy /y "%~dp0manga_downloader.db" "%DIST_DIR%\" >nul
if exist "%~dp0config.ini" copy /y "%~dp0config.ini" "%DIST_DIR%\" >nul

rem Copy language JSON directory to dist
if not exist "%DIST_DIR%\lang" mkdir "%DIST_DIR%\lang"
copy /y "%~dp0src\lang\*.json" "%DIST_DIR%\lang\" >nul

rem Copy required DLLs to dist so executable runs directly
set "DLL_LIST=libcurl-4.dll libsqlite3-0.dll libcjson-1.dll zlib1.dll libssl-3-x64.dll libcrypto-3-x64.dll libbrotlidec.dll libbrotlicommon.dll libidn2-0.dll libintl-8.dll libnghttp2-14.dll libnghttp3-9.dll libngtcp2-16.dll libngtcp2_crypto_ossl-0.dll libpsl-5.dll libssh2-1.dll libzstd.dll libiconv-2.dll libunistring-5.dll"

if defined MSYS_BIN (
    for %%f in (%DLL_LIST%) do (
        if exist "%MSYS_BIN%\%%f" (
            copy /y "%MSYS_BIN%\%%f" "%DIST_DIR%\" >nul
        )
    )
)

echo.
echo =======================================================
echo Build succeeded! All outputs are in folder: dist\
echo =======================================================
echo.

if "%~1" neq "--no-pause" if "%~2" neq "--no-pause" pause
