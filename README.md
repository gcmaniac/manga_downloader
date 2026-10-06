# <div align="center">

![Banner](assets/banner.png)

</div>

---

# 📖 Manga Downloader

**High‑Performance, Ultra‑Lightweight native Win32 manga archiver**

---

[![English](https://img.shields.io/badge/Language-English-blue)](README.md)  
[![Indonesian](https://img.shields.io/badge/Language-Indonesia-red)](README.id-ID.md)

---

## 📚 About

Manga Downloader is a pure C, Win32‑API desktop application that downloads entire manga chapters automatically, stores them efficiently, and runs with a tiny memory footprint (< 20 MiB) and a binary size under 250 KB. It is ideal for users who want a fast, portable solution without the bloat of Electron or Python runtimes.

---

## ✨ Features

- **Zero bloat** – pure native Win32 binary.
- **Multi‑site scraper** – supports MangaGeko, MangaNato, Asura Scans, Flame Comics and a generic heuristic engine.
- **Smart chapter filtering** – download specific ranges, skip already‑downloaded chapters, and avoid duplicates.
- **AI Hub** – built‑in SQLite database for OpenRouter AI model benchmarking.
- **Portable** – run from a USB stick; no installer required.

---

## 🛠️ Build & Compilation

### Prerequisites
* Sistem Operasi: **Windows 10 / 11 (64‑bit)**
* Compiler: **MinGW‑w64 (GCC)**
* Dependencies: install via MSYS2 (`pacman -S mingw-w64-x86_64-curl mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-cjson`). See the setup guide.

### Build steps
1. Open a **MSYS2 MinGW‑64** shell.
2. Navigate to the project root:
   ```bash
   cd D:/c_projects/manga_downloader
   ```
3. Run the build script:
   ```cmd
   build.bat --no-pause
   ```
   The script compiles the resources and produces `manga_downloader.exe`.

---

## 🚀 Usage

1. Launch `manga_downloader.exe`.
2. Paste the manga URL into the **Main URL** field.
3. Choose a destination folder.
4. (Optional) Enter a chapter range such as `1-10, 25, 100‑end`.
5. Click **Start Download**. Progress and logs appear in the UI.

---

## 🤝 Contributing

Contributions are welcome! Fork the repository, make your changes, and open a pull request. Please keep the code style consistent and update the README when adding new features or languages.

---

## 📄 License

This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for details.
