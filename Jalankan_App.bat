@echo off
setlocal
cd /d "%~dp0"

rem Pastikan folder downloads tersedia
if not exist "downloads" mkdir "downloads"

rem Jalankan aplikasi dari folder dist jika ada, atau root
if exist "%~dp0dist\manga_downloader.exe" (
    cd /d "%~dp0dist"
    start "" "%~dp0dist\manga_downloader.exe"
) else if exist "%~dp0manga_downloader.exe" (
    start "" "%~dp0manga_downloader.exe"
) else (
    echo Aplikasi manga_downloader.exe belum dikompilasi!
    echo Silakan jalankan build.bat terlebih dahulu.
    pause
)
exit /b 0
