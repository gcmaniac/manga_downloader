@echo off
setlocal enabledelayedexpansion

echo [1/3] Compiling manga_downloader.exe...
call build.bat --no-pause
if %errorlevel% neq 0 (
    echo Compilation failed!
    exit /b 1
)

echo [2/3] Preparing portable package directory...
set "DIST_DIR=%~dp0dist\manga_downloader"
if exist "%DIST_DIR%" rd /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%"

copy /y "%~dp0manga_downloader.exe" "%DIST_DIR%\"
copy /y "%~dp0LICENSE" "%DIST_DIR%\"
if exist "%~dp0manga_downloader.db" copy /y "%~dp0manga_downloader.db" "%DIST_DIR%\"

set "BIN=E:\msys64\mingw64\bin"

rem Copy required DLLs for standalone execution
set DLL_LIST=libcurl-4.dll libsqlite3-0.dll libcjson-1.dll zlib1.dll libssl-3-x64.dll libcrypto-3-x64.dll libbrotlidec.dll libbrotlicommon.dll libidn2-0.dll libnghttp2-14.dll libnghttp3-9.dll libngtcp2-16.dll libngtcp2_crypto_ossl-0.dll libpsl-5.dll libssh2-1.dll libzstd.dll libiconv-2.dll libunistring-5.dll

for %%f in (%DLL_LIST%) do (
    if exist "%BIN%\%%f" (
        copy /y "%BIN%\%%f" "%DIST_DIR%\" >nul
    ) else (
        echo Warning: %%f not found in %BIN%
    )
)

echo [3/3] Creating portable ZIP archive...
set "ZIP_OUT=%~dp0dist\manga_downloader_portable.zip"
if exist "%ZIP_OUT%" del /f /q "%ZIP_OUT%"

powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; [System.IO.Compression.ZipFile]::CreateFromDirectory('%DIST_DIR%', '%ZIP_OUT%')"

if exist "%ZIP_OUT%" (
    echo.
    echo =======================================================
    echo Packaging successful!
    echo Portable ZIP created at:
    echo %ZIP_OUT%
    echo =======================================================
) else (
    echo Error creating ZIP file.
)

if "%~1" neq "--no-pause" pause
