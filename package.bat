@echo off
setlocal enabledelayedexpansion

echo [1/3] Compiling manga_downloader into dist...
call build.bat --no-pause
if %errorlevel% neq 0 (
    echo Compilation failed!
    exit /b 1
)

echo [2/3] Preparing portable package directory...
set "PKG_DIR=%TEMP%\manga_downloader_pkg"
if exist "%PKG_DIR%" rd /s /q "%PKG_DIR%"
mkdir "%PKG_DIR%"

rem Copy all files from dist to temporary package directory
xcopy /y /e /q "%~dp0dist\*.*" "%PKG_DIR%\" >nul
rem Ensure LICENSE is included
if exist "%~dp0LICENSE" copy /y "%~dp0LICENSE" "%PKG_DIR%\" >nul

echo [3/3] Creating portable ZIP archive...
set "ZIP_OUT=%~dp0dist\manga_downloader_portable.zip"
if exist "%ZIP_OUT%" del /f /q "%ZIP_OUT%"

powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; [System.IO.Compression.ZipFile]::CreateFromDirectory('%PKG_DIR%', '%ZIP_OUT%')"

rem Clean up temporary staging directory
if exist "%PKG_DIR%" rd /s /q "%PKG_DIR%"

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
