<div align="center">

  <img src="assets/banner.png" alt="Manga Downloader Banner" width="100%" style="border-radius: 12px; box-shadow: 0 8px 24px rgba(0,0,0,0.25);" />

  <br/><br/>

  <p align="center">
    <img src="assets/app_icon.png" alt="Manga Downloader Logo" width="80" height="80" />
  </p>

  # ⚡ MANGA DOWNLOADER ⚡
  ### *High-Performance, Ultra-Lightweight Native Win32 Manga Archiver & AI Hub*

  <p align="center">
    <b>A Pure C Desktop Application to Automatically, Neatly, Swiftly, and Memory-Efficiently Download Entire Manga Chapters.</b>
  </p>

  <p align="center">
    <a href="README.en.md"><img src="https://img.shields.io/badge/Language-English-blue?style=for-the-badge" alt="English" /></a>
    <a href="README.id.md"><img src="https://img.shields.io/badge/Language-Bahasa_Indonesia-lightgrey?style=for-the-badge" alt="Bahasa Indonesia" /></a>
  </p>

  <p align="center">
    <a href="https://github.com/gcmaniac/manga_downloader"><img src="https://img.shields.io/badge/Author-gcmaniac-orange?style=for-the-badge&logo=github" alt="Author gcmaniac" /></a>
    <a href="https://github.com/gcmaniac/manga_downloader/stargazers"><img src="https://img.shields.io/github/stars/gcmaniac/manga_downloader?style=for-the-badge&logo=github&color=ff69b4" alt="GitHub Stars" /></a>
    <a href="#-key-features"><img src="https://img.shields.io/badge/Status-Active_Production-brightgreen?style=for-the-badge&logo=visual-studio-code" alt="Status" /></a>
    <a href="#-compilation--building-from-source"><img src="https://img.shields.io/badge/Language-C99_Win32-00599C?style=for-the-badge&logo=c" alt="Language" /></a>
    <a href="#-compilation--building-from-source"><img src="https://img.shields.io/badge/Network-libcurl-blue?style=for-the-badge&logo=curl" alt="libcurl" /></a>
    <a href="#-compilation--building-from-source"><img src="https://img.shields.io/badge/Database-SQLite3-003B57?style=for-the-badge&logo=sqlite" alt="SQLite3" /></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow.svg?style=for-the-badge" alt="License" /></a>
    <a href="#-compilation--building-from-source"><img src="https://img.shields.io/badge/Platform-Windows_10_%2F_11-0078D6?style=for-the-badge&logo=windows" alt="Platform" /></a>
  </p>

  <p align="center">
    <a href="#-about-the-project">📖 About</a> •
    <a href="#-system-architecture--workflow">🔄 Workflow</a> •
    <a href="#-user-guide">🚀 User Guide</a> •
    <a href="#-key-features">✨ Key Features</a> •
    <a href="#-compilation--building-from-source">🛠️ How to Build</a> •
    <a href="#-call-for-contributions">🤝 Contributing</a> •
    <a href="#-license">📄 License</a>
  </p>

</div>

---

## 📖 About The Project

**Manga Downloader** is a native Windows desktop application crafted specifically in **C** using the **Windows Win32 API**. It was engineered from the ground up to solve common pain points found in modern manga downloaders: *excessive RAM usage (Chromium/Electron bloatware), sluggish download speeds, and disorganized folder structures*.

With an ultra-compact binary footprint (**under 250 KB**) and minimal RAM consumption (**< 20 MB RAM**), you can archive thousands of high-resolution manga chapters to local storage with absolute peace of mind, without bogging down your laptop or PC.

### 🌟 Why Choose Manga Downloader?
* **Zero Bloatware**: Pure C implementation (no Electron, no WebView, no Python GUI runtime overhead).
* **Portable Ready**: Simply extract a single ZIP directory and run it immediately—no installers or invasive Windows registry modifications.
* **Smart Heuristics Engine**: Automatically analyzes chapter page structures and image URLs across prominent manga portals.
* **Next-Gen AI Agent Hub**: Directly integrated with an AI model catalog (OpenRouter API) for benchmarking latency, pricing per million tokens, intelligence ratings, and laying the groundwork for automated workflows.

---

## 🔄 System Architecture & Workflow

Below is the workflow architecture detailing how Manga Downloader processes everything from receiving a manga URL to storing images on disk:

```mermaid
flowchart TD
    A([User Inputs Manga URL & Filters]) --> B[Auto Site Detection & Scraper Engine]
    B --> C{Engine Verification}
    C -->|MangaGeko| D1[MangaGeko Scraper]
    C -->|MangaNato / Kakalot| D2[MangaNato Scraper]
    C -->|Asura Scans| D3[Asura Scraper]
    C -->|Other Sites| D4[Universal Heuristic Engine]
    
    D1 & D2 & D3 & D4 --> E[Scrape Full Chapter List]
    E --> F[Apply User Chapter Range & Missing Chapter Filter]
    F --> G{Chapter Passes Filter?}
    G -- No --> H[Skip Chapter]
    G -- Yes --> I[Create Chapter Folder on Disk]
    
    I --> J[Fetch Chapter Page & Scan img Tags]
    J --> K[Iterative Image Download: libcurl + Referer Spoofing]
    K --> L{Image File Already Exists?}
    L -- Exists & No Overwrite --> M[Skip Image Download]
    L -- Missing / Overwrite --> N[Download & Save: 001.jpg, 002.jpg...]
    
    M & N --> O{All Chapter Images Done?}
    O -- No --> K
    O -- Yes --> P{More Chapters Remaining?}
    P -- Yes --> G
    P -- All Finished --> Q([Complete! Show Notification & Activity Log])
```

### 🔍 Internal Execution Pipeline:

1. **URL Normalization & Engine Selector**
   The application extracts the base hostname and matches it against supported site patterns without heavy regex. If no specific parser matches, the *Universal Heuristic Engine* seamlessly takes over.
2. **Chapter Discovery & Indexing**
   Fetches the manga index page, parses chapter hyperlinks (`/reader/`, `/chapter-`, etc.), sorts them in proper chronological order (Chapter 1 through the latest), and deduplicates URLs.
3. **Smart Range & Missing Chapter Filtering**
   Applies user-defined range expressions (e.g., `1-10, 25, 50-end`). If *"Only find & download missing chapters"* is checked, the system inspects existing local folders to verify if valid image files already exist before skipping the chapter.
4. **Resilient Image Fetching (Anti-Hotlinking)**
   Dispatches HTTP requests impersonating genuine desktop browsers (`User-Agent Chrome Windows 10/11`) accompanied by the appropriate `Referer Header` of the chapter reader page, circumventing *HTTP 403 Forbidden* restrictions imposed by manga CDNs.
5. **Disk I/O & Real-Time Logging**
   All page download progress and chapter statuses are streamed asynchronously to the Win32 GUI log window via a dedicated `Background Worker Thread`, ensuring the user interface remains snappy and free of freezing (*Not Responding*).

---

## ✨ Key Features

| Feature | Description |
| :--- | :--- |
| **🚀 Native Win32 UI** | Blazing-fast, ultra-lightweight, battery-efficient graphical interface with zero runtime overhead. |
| **🎯 Multi-Site Scraper Engine** | Built-in support for **MangaGeko**, **MangaNato**, **MangaKakalot**, **Asura Scans**, plus a versatile **Universal Scraper** for standard web readers. |
| **🔢 Flexible Chapter Filtering** | Supports specific chapter numbers as well as arbitrary ranges: e.g., `5`, `1-10`, `25, 28, 30`, or `116-end`. |
| **🛡️ Missing Chapter Detection** | Prevents redundant downloads for ongoing series. Check this box and the archiver will only download newly released chapters! |
| **⚡ Multi-Threaded Async Worker** | Downloads execute on background threads without locking the UI, paired with a responsive **Stop** button anytime. |
| **📁 Structured Auto-Organization** | Images are neatly saved into dedicated chapter subfolders with sequential zero-padded names (`001.jpg`, `002.jpg`, ...), fully compatible with any offline manga reader. |
| **📄 Smart PDF Converter** | Convert downloaded chapters into PDF documents (per chapter or per volume). Supports automatic chapter-to-volume folder relocation and AI Agent volume text parsing. |
| **🌐 AI Manga Translator** | Vision LLM-powered visual translation pipeline. Automatically detects speech bubbles, in-paints original text, and re-typesets translated text into target languages (ID/EN/JP), with optional direct PDF generation. |
| **🔔 Sound Notifications** | Plays an audible Windows notification chime upon completion of download, PDF conversion, and translation jobs. |
| **🧠 Built-in AI Agent Hub** | Embedded SQLite-backed management interface for AI models (OpenRouter API), supporting benchmarking across hundreds of models for token pricing, latency, and capability tiers. |
| **📦 100% Portable** | Easily runs from a USB flash drive or external drive; ready out of the box on Windows 10 and Windows 11. |

---

## 🚀 User Guide

### 1. Launching the App (1-Click Ready)
* **Quick Launch**: Double-click **`Jalankan_App.bat`** or launch **`manga_downloader.exe`** directly from the root directory.
* All library dependencies (`libcurl`, `sqlite3`, `cjson`, etc.) and the SQLite database are portably packaged within the folder, running instantly without prior installation.
* For distribution to other machines, grab the standalone ZIP archive from [Releases](https://github.com/gcmaniac/manga_downloader/releases) or rebuild a fresh package using `package.bat`.

### 2. Downloading Manga (Tab 1: Home)
1. **Main Manga Link**:
   Copy the chapter list URL of the manga from your browser and paste it into the *Main Manga Link* input field.
   > Example: `https://www.mgeko.cc/manga/7322-parnf/all-chapters/`
2. **Web Status**:
   Observe the status label below the URL field. The app will immediately detect and display the appropriate scraper engine (e.g., *MangaGeko Engine [Supported]*).
3. **Output Directory**:
   Click **Browse...** to choose a target folder on your system (or click **Default** to use the local `downloads` directory).
4. **Select Chapters (Optional)**:
   * Leave blank to download **all available chapters from first to last**.
   * Specify desired chapters using flexible expressions:
     * `1-10` : Downloads Chapter 1 through Chapter 10.
     * `5, 12, 20` : Downloads Chapters 5, 12, and 20 only.
     * `100-end` : Downloads from Chapter 100 up to the newest released chapter.
5. **Additional Options**:
   * Check **"Only find & download chapters not present in folder"** to pull only new updates without re-downloading existing chapters.
   * Check **"Overwrite files if they already exist"** only when you need to replace existing image files.
6. **Start Download**:
   * Click **Start Download**.
   * Monitor live progress and statistics directly in the **Activity Log** window.
   * You can cancel or pause the process at any moment by clicking **Stop**.

---

### 3. Converting to PDF (Tab 2: PDF Converter)
* Choose between **Per Chapter (1 PDF per chapter)** or **Per Volume (1 PDF per volume)**.
* Organize chapters by volume using custom volume lists or fixed chapter counts per volume.
* Automatically moves chapters into structured volume folders and generates standardized PDF books.

---

### 4. Translating Manga (Tab 3: Manga Translator)
* **Visual Manga Translation Pipeline**:
  1. Select the folder containing downloaded chapter subfolders.
  2. Choose your target translation language (**Bahasa Indonesia**, **English**, or **Japanese**).
  3. Filter chapters to translate (e.g. `1-5`).
  4. Options:
     * **Keep original translated images**: Saves modified pages into `[Target Folder]\Chapter XXX [ID]\...`
     * **Compile to PDF directly**: Automatically merges the translated pages into a clean chapter PDF book.
  5. The Vision LLM (`server_agent` + `model_penggunaan`) reads each page, identifies bubble locations, masks out the original text, and typesets the new translated text neatly with auto-fitted fonts.

---

### 5. Exploring the AI Hub (Tabs 4, 5, & 6)

The application includes an advanced SQLite-powered AI evaluation workspace for benchmarks and automation:

* **Tab 4 (AI Settings & Benchmark)**:
  * Enter your API Key from [OpenRouter](https://openrouter.ai/).
  * Click **Scan & Update Models**. The application retrieves the complete catalog of models, calculates estimated latency, and logs input/output token pricing.
  * Sort models by Rating, Latency, or Pricing tiers.
* **Tab 5 (Tested Model Catalog)**:
  * Filter models by price boundaries or rating thresholds.
  * Pick the top-performing model and register it into your active operational roster by clicking **Use This Model**.
* **Tab 6 (Active AI Models)**:
  * Organize priority order and manage active models configured for chapter text translation and analysis tasks.

---

## 💻 Compilation & Building from Source

For developers looking to inspect, customize, or compile the source code on Windows:

### Prerequisites
* Operating System: **Windows 10 / 11 (64-bit)**
* C Compiler: **MinGW-w64 (GCC)**
* Library Dependencies (available via MSYS2 `pacman -S`):
  * `mingw-w64-x86_64-curl`
  * `mingw-w64-x86_64-sqlite3`
  * `mingw-w64-x86_64-cjson`

### Build Instructions

1. Clone the repository:
   ```bash
   git clone https://github.com/gcmaniac/manga_downloader.git
   cd manga_downloader
   ```

2. Run the automated build script:
   ```cmd
   build.bat
   ```
   *This compiles the application icon and resource file (`resources.rc`) via `windres`, then compiles `manga_downloader.exe` with `-O2` optimizations.*

3. Package a Standalone Portable Release:
   ```cmd
   package.bat
   ```
   *This script bundles all required runtime DLLs from your MinGW environment and packages them into `dist/manga_downloader_portable.zip`.*

---

## 🤝 Call for Contributions

<div align="center">
  <h3>🌸 Let's Build Together with the Open Source Community! 🌸</h3>
  <p>
    <i>"This project is built with dedication to simplicity, performance, and the freedom to archive your favorite manga."</i>
  </p>
</div>

We warmly welcome contributions from everyone—beginners, C enthusiasts, and seasoned developers alike—to help shape **Manga Downloader**!

### 💡 Ideas & Areas for Contribution:
- [ ] **New Site Scrapers**: Add parsers for additional popular manga portals or localized scanlation sites into `src/scrapers/`.
- [ ] **CBZ / PDF Export**: Bundle downloaded chapter images into comic archive `.cbz` files or `.pdf` e-books directly from the GUI.
- [ ] **Multi-threaded Worker Pool**: Accelerate downloads by fetching multiple images or chapters in parallel.
- [ ] **Dark Mode Win32 Theme**: Implement a sleek, native dark theme for the Win32 window and controls.
- [ ] **AI-Powered OCR & Translator**: Integrate AI models from the AI Hub tab to detect and translate manga speech bubbles directly.
- [ ] **GUI Localization**: Add multi-language interface support (English, Bahasa Indonesia, 日本語) within the native GUI.

### 🛠️ Contribution Workflow (Pull Request):
1. **[Fork](https://github.com/gcmaniac/manga_downloader/fork)** this repository to your GitHub account.
2. Create a descriptive feature branch:
   ```bash
   git checkout -b feature/add-new-manga-scraper
   ```
3. Make your code changes and ensure the project builds cleanly via `build.bat`:
   ```cmd
   build.bat
   ```
4. Commit your changes with clear messages:
   ```bash
   git commit -m "feat: Add scraper engine for MangaPortalXYZ"
   ```
5. Push the branch to your fork:
   ```bash
   git push origin feature/add-new-manga-scraper
   ```
6. Open a **[Pull Request](https://github.com/gcmaniac/manga_downloader/pulls)** on GitHub explaining your proposed changes. We'll be thrilled to review and merge them!

Found a bug or have a suggestion? Feel free to open a **[New GitHub Issue](https://github.com/gcmaniac/manga_downloader/issues)**!

---

## ⚖️ Disclaimer & Usage Ethics

This project is intended strictly for educational purposes, study of low-level systems programming in C & Win32 API, and personal offline archiving. Please continue to support manga authors, illustrators, and official publishers by purchasing original volumes, merchandise, and subscribing to licensed platforms.

---

## 📄 License

This project is licensed under the **[MIT License](LICENSE)**. You are free to use, modify, and distribute it for both personal and commercial purposes, provided the original copyright notice and permission notice are preserved.

---

<div align="center">
  <sub>Crafted with ❤️ and dedication by <a href="https://github.com/gcmaniac"><b>gcmaniac</b></a> and the Open Source Manga Community.</sub><br/><br/>
  <b>⭐ If you find this project useful, don't forget to give it a <a href="https://github.com/gcmaniac/manga_downloader">Star on GitHub</a>! ⭐</b>
</div>
