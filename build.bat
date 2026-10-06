@echo off
setlocal

rem Path to MinGW gcc and windres
if exist "E:\msys64\mingw64\bin\gcc.exe" (
    set "CC=E:\msys64\mingw64\bin\gcc.exe"
    set "WINDRES=E:\msys64\mingw64\bin\windres.exe"
) else (
    set CC=gcc.exe
    set WINDRES=windres.exe
)

rem Compile resources
%WINDRES% -i resources.rc -o resources.o
if %errorlevel% neq 0 (
    echo Resource compilation failed.
    exit /b 1
)

rem Compilation flags
set CFLAGS=-Wall -O2
rem Linker flags (resources, libcurl, sqlite3, comctl32, cJSON, Unicode, Windows subsystem, Shell/OLE)
set LDFLAGS=resources.o -lcurl -lsqlite3 -lcomctl32 -lcjson -municode -mwindows -lole32 -lshell32

rem Build the executable
%CC% %CFLAGS% -o manga_downloader.exe src\main.c src\ai_agent.c src\db_migration.c %LDFLAGS%

if %errorlevel% neq 0 (
    echo Build failed with error code %errorlevel%.
) else (
    echo Build succeeded. Executable created as manga_downloader.exe
)

if "%~1" neq "--no-pause" pause
