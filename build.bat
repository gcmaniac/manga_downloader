@echo off
setlocal

rem Path to MinGW gcc and windres
if exist "E:\msys64\mingw64\bin\gcc.exe" (
    set "CC=E:\msys64\mingw64\bin\gcc.exe"
    set "WINDRES=E:\msys64\mingw64\bin\windres.exe"
) else (
    set "CC=gcc"
    set "WINDRES=windres"
)

rem Compile resources
%WINDRES% -i resources.rc -o resources.o
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
set SRCS=src\main.c src\ai_agent.c src\db_migration.c src\config.c src\scrapers\scrapers.c src\scrapers\scraper_manganato.c src\scrapers\scraper_mgeko.c src\scrapers\scraper_asura.c src\scrapers\scraper_flame.c src\scrapers\scraper_generic.c

if "%1"=="--static" goto build_static
if "%2"=="--static" goto build_static

rem Linker flags (resources, libcurl, sqlite3, comctl32, cJSON, Unicode, Windows subsystem, Shell/OLE)
set LDFLAGS=resources.o -lcurl -lsqlite3 -lcomctl32 -lcjson -municode -mwindows -lole32 -lshell32

rem Build the executable
%CC% %CFLAGS% -I%INCDIR% -L%LIBDIR% -o manga_downloader.exe %SRCS% %LDFLAGS%

if %errorlevel% neq 0 (
    echo Build failed with error code %errorlevel%.
    exit /b %errorlevel%
) else (
    echo Build succeeded. Executable created as manga_downloader.exe
)
goto finish

:build_static
set STATIC_LDFLAGS=resources.o -DCURL_STATICLIB -static -lcurl -lsqlite3 -lcjson -lssl -lcrypto -lnghttp2 -lnghttp3 -lngtcp2 -lngtcp2_crypto_ossl -lssh2 -lzstd -lbrotlidec -lbrotlicommon -lidn2 -lpsl -lunistring -lintl -liconv -lz -lcomctl32 -municode -mwindows -lole32 -lshell32 -lws2_32 -lcrypt32 -lwldap32 -liphlpapi -lbcrypt -lsecur32 -s
%CC% %CFLAGS% -o manga_downloader_standalone.exe %SRCS% %STATIC_LDFLAGS%
if %errorlevel% neq 0 (
    echo Static build failed with error code %errorlevel%.
    exit /b %errorlevel%
) else (
    echo Static build succeeded. Standalone executable created as manga_downloader_standalone.exe
)

:finish
if "%~1" neq "--no-pause" if "%~2" neq "--no-pause" pause
