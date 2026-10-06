@echo off
setlocal
cd /d "%~dp0"

rem Pastikan folder downloads tersedia
if not exist "downloads" mkdir "downloads"

rem Jalankan aplikasi secara langsung
start "" "%~dp0manga_downloader.exe"
exit /b 0
