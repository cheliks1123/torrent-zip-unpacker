// Распаковщик ZIP / RAR / 7z (+ .torrent, опционально) — окно в стиле Rufus
// Чистый Win32 API, один файл.
//
// Сборка:
//   (рядом должны лежать icon.ico и resource.rc)
//   MSVC (Developer Command Prompt):
//     rc /nologo resource.rc
//     cl /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE unpacker.cpp resource.res /link /SUBSYSTEM:WINDOWS
//   MinGW-w64:
//     windres resource.rc -O coff -o resource.o
//     g++ -O2 -finput-charset=UTF-8 -municode -mwindows unpacker.cpp resource.o -o unpacker.exe -lcomctl32 -lcomdlg32 -lshell32 -lole32
//
// Для RAR/7z/ZIP нужен установленный 7-Zip (https://www.7-zip.org/).
// Поддержка .torrent: поставьте libtorrent (vcpkg install libtorrent) и
// добавьте при сборке  /DUSE_LIBTORRENT  (и подключите библиотеку).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstdlib>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef USE_LIBTORRENT
#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#endif

using std::wstring;

enum {
    IDC_SRC = 101, IDC_SRCBTN, IDC_DST, IDC_DSTBTN, IDC_AUTO, IDC_DEL,
    IDC_SPACE, IDC_BAR, IDC_STATUS, IDC_START, IDC_STOP, IDC_CLOSE
};
enum { WM_PROGRESS = WM_APP + 1, WM_STATUS, WM_DONE, WM_SPACE };

static HWND g_hwnd, g_src, g_dst, g_auto, g_del, g_space, g_bar, g_status, g_start, g_stop;
static std::atomic<bool> g_stopFlag(false), g_running(false);
static std::atomic<HANDLE> g_proc(nullptr);
static std::thread g_worker;

// Проверка места: -1 = размер неизвестен, -2 = идёт подсчёт, >=0 = байт нужно
static std::atomic<long long> g_need(-1);
static std::atomic<int> g_gen(0);
static bool g_ready = false;
static bool g_spaceBad = false;

// ---------- утилиты ----------
static wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string Narrow(const wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    wstring s(n + 1, 0);
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}
static bool FileExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static wstring LowerExt(const wstring& p) {
    size_t d = p.find_last_of(L'.');
    if (d == wstring::npos) return L"";
    wstring e = p.substr(d);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}
static wstring FmtBytes(unsigned long long b) {
    wchar_t t[64];
    if (b >= (1ULL << 30))      swprintf(t, 64, L"%.1f ГБ", b / 1073741824.0);
    else if (b >= (1ULL << 20)) swprintf(t, 64, L"%.0f МБ", b / 1048576.0);
    else                        swprintf(t, 64, L"%.0f КБ", b / 1024.0);
    return t;
}

// Сообщения из рабочего потока в UI
static void PostProgress(int pct) { PostMessageW(g_hwnd, WM_PROGRESS, pct, 0); }
static void PostStatus(const wstring& t) {
    PostMessageW(g_hwnd, WM_STATUS, 0, (LPARAM) new wstring(t));
}
static void PostDone(bool ok, const wstring& msg) {
    PostMessageW(g_hwnd, WM_DONE, ok, (LPARAM) new wstring(msg));
}

static wstring Find7z() {
    wchar_t buf[MAX_PATH];
    for (const wchar_t* n : { L"7z.exe", L"7za.exe" })
        if (SearchPathW(nullptr, n, nullptr, MAX_PATH, buf, nullptr)) return buf;
    for (const wchar_t* env : { L"ProgramFiles", L"ProgramFiles(x86)" }) {
        wchar_t base[MAX_PATH];
        if (GetEnvironmentVariableW(env, base, MAX_PATH)) {
            wstring p = wstring(base) + L"\\7-Zip\\7z.exe";
            if (FileExists(p)) return p;
        }
    }
    return L"";
}

// ---------- свободное место ----------
// Берёт ближайшую существующую папку вверх по пути (папки назначения может ещё не быть).
static bool FreeBytes(wstring p, unsigned long long& freeb) {
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 4, full, nullptr);
    if (!n || n >= MAX_PATH * 4) return false;
    p = full;
    for (;;) {
        DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            ULARGE_INTEGER av{};
            if (GetDiskFreeSpaceExW(p.c_str(), &av, nullptr, nullptr)) { freeb = av.QuadPart; return true; }
            return false;
        }
        size_t s = p.find_last_of(L"\\/");
        if (s == wstring::npos || s == 0) return false;
        p.erase(s);
        if (p.size() == 2 && p[1] == L':') p += L"\\";
    }
}

// Запуск консольной программы и сбор её вывода
static bool RunCapture(const wstring& cmd, std::string& out) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nul;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
    BOOL ok = CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return false; }
    char buf[4096];
    DWORD got;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
}

// Сколько места займёт содержимое архива (сумма Size из `7z l -slt`)
static long long ArchiveSize(const wstring& archive) {
    wstring exe = Find7z();
    if (exe.empty()) return -1;
    std::string out;
    if (!RunCapture(L"\"" + exe + L"\" l -slt \"" + archive + L"\"", out)) return -1;
    long long total = 0;
    bool body = false, any = false;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t e = out.find('\n', pos);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line == "----------") { body = true; continue; }
        if (body && line.compare(0, 7, "Size = ") == 0) {
            total += _strtoi64(line.c_str() + 7, nullptr, 10);
            any = true;
        }
    }
    return any ? total : -1;
}

#ifdef USE_LIBTORRENT
static long long TorrentSize(const wstring& path) {
    try { return libtorrent::torrent_info(Narrow(path)).total_size(); }
    catch (...) { return -1; }
}
#endif

static long long NeedBytes(const wstring& src) {
    if (LowerExt(src) == L".torrent") {
#ifdef USE_LIBTORRENT
        return TorrentSize(src);
#else
        return -1;
#endif
    }
    return ArchiveSize(src);
}

// Обновляет строку «Место на диске» (вызывать только из UI-потока)
static void UpdateSpace() {
    if (!g_space) return;
    wstring dst = GetText(g_dst);
    unsigned long long fr = 0;
    bool okf = !dst.empty() && FreeBytes(dst, fr);
    long long need = g_need.load();
    wstring t;
    g_spaceBad = false;
    if (!okf) {
        t = L"Диск не найден — проверьте путь";
        g_spaceBad = true;
    } else {
        wstring drive = (dst.size() >= 2 && dst[1] == L':') ? dst.substr(0, 2) : L"";
        t = L"Свободно" + (drive.empty() ? wstring() : L" на " + drive) + L": " + FmtBytes(fr);
        if (need == -2) {
            t += L"   |   Размер: считаю...";
        } else if (need >= 0) {
            t += L"   |   Нужно: " + FmtBytes((unsigned long long)need);
            if ((unsigned long long)need > fr) { t += L"   —   НЕ ХВАТАЕТ МЕСТА!"; g_spaceBad = true; }
            else t += L"   —   хватает";
        } else {
            t += L"   |   Нужно: неизвестно";
        }
    }
    SetWindowTextW(g_space, t.c_str());
    InvalidateRect(g_space, nullptr, TRUE);
}

// Пересчитывает требуемый размер для выбранного файла (в фоне)
static void RefreshNeed() {
    wstring src = GetText(g_src);
    int gen = ++g_gen;
    if (!FileExists(src)) { g_need = -1; UpdateSpace(); return; }
    g_need = -2;
    UpdateSpace();
    std::thread([src, gen]() {
        long long n = NeedBytes(src);
        PostMessageW(g_hwnd, WM_SPACE, (WPARAM)gen, (LPARAM) new long long(n));
    }).detach();
}

// ---------- распаковка через 7-Zip ----------
// Возвращает 0 при успехе, иначе код ошибки; -1 — отмена.
static int Extract(const wstring& archive, const wstring& dst, int base, int span, wstring& err, bool deleteAfter) {
    wstring exe = Find7z();
    if (exe.empty()) {
        err = L"Не найден 7-Zip.\nУстановите его с https://www.7-zip.org/ и повторите.";
        return 2;
    }
    wstring name = archive.substr(archive.find_last_of(L"\\/") + 1);
    wstring cmd = L"\"" + exe + L"\" x \"" + archive + L"\" \"-o" + dst + L"\" -y -bsp1 -bso0 -bse0";

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { err = L"Не удалось создать канал."; return 2; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nul;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
    BOOL started = CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        CloseHandle(rd);
        err = L"Не удалось запустить 7-Zip.";
        return 2;
    }
    g_proc = pi.hProcess;

    PostStatus(L"Распаковка: " + name);
    char buf[512];
    DWORD got;
    int last = -1;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) {
        // ищем последнее "NN%" в куске
        for (int i = (int)got - 1; i >= 0; --i) {
            if (buf[i] == '%') {
                int j = i - 1, v = 0, mul = 1, digits = 0;
                while (j >= 0 && buf[j] >= '0' && buf[j] <= '9' && digits < 3) {
                    v += (buf[j] - '0') * mul; mul *= 10; --j; ++digits;
                }
                if (digits && v <= 100 && v != last) {
                    last = v;
                    PostProgress(base + span * v / 100);
                }
                break;
            }
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    g_proc = nullptr;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    if (g_stopFlag) return -1;
    if (code != 0 && code != 1) {
        err = L"7-Zip завершился с ошибкой (код " + std::to_wstring(code) +
              L").\nВозможные причины: архив повреждён, защищён паролем, нет остальных частей\n"
              L"или закончилось место на диске.";
        return (int)code;
    }
    if (deleteAfter) DeleteFileW(archive.c_str());
    return 0;
}

// ---------- торрент ----------
#ifdef USE_LIBTORRENT
static void DoTorrent(const wstring& path, const wstring& dst, bool autoUnpack, bool del) {
    namespace lt = libtorrent;
    try {
        lt::session ses;
        lt::add_torrent_params p;
        p.ti = std::make_shared<lt::torrent_info>(Narrow(path));
        p.save_path = Narrow(dst);
        lt::torrent_handle h = ses.add_torrent(p);
        auto ti = p.ti;
        PostStatus(L"Подключение к пирам...");
        for (;;) {
            if (g_stopFlag) { ses.remove_torrent(h); PostDone(false, L"Отменено."); return; }
            lt::torrent_status s = h.status();
            if (s.state == lt::torrent_status::seeding || s.state == lt::torrent_status::finished) break;
            int pct = (int)(s.progress * 100 * (autoUnpack ? 0.85 : 1.0));
            wchar_t t[160];
            swprintf(t, 160, L"Загрузка %.1f%%   %.2f МБ/с   пиры: %d",
                     s.progress * 100.0, s.download_rate / 1048576.0, s.num_peers);
            PostProgress(pct); PostStatus(t);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (autoUnpack) {
            std::vector<wstring> arch;
            for (int i = 0; i < ti->num_files(); ++i) {
                wstring f = dst + L"\\" + Widen(ti->files().file_path(lt::file_index_t(i)));
                wstring e = LowerExt(f);
                bool part2 = false;
                // пропускаем .part2.rar и т.п., 7-Zip сам подхватит остальные части
                size_t pp = f.rfind(L".part");
                if (pp != wstring::npos && e == L".rar") {
                    wstring num = f.substr(pp + 5, f.size() - pp - 9);
                    part2 = !num.empty() && _wtoi(num.c_str()) > 1;
                }
                if ((e == L".zip" || e == L".rar" || e == L".7z") && !part2) arch.push_back(f);
            }
            for (size_t k = 0; k < arch.size() && !g_stopFlag; ++k) {
                wstring err;
                wstring folder = arch[k].substr(0, arch[k].find_last_of(L"\\/"));
                int r = Extract(arch[k], folder, 85 + (int)(15 * k / arch.size()), 15 / (int)arch.size(), err, del);
                if (r > 0) { PostDone(false, err); return; }
            }
        }
        if (g_stopFlag) { PostDone(false, L"Отменено."); return; }
        PostDone(true, L"Готово. Файлы в:\n" + dst);
    } catch (std::exception& e) {
        PostDone(false, L"Ошибка торрента:\n" + Widen(e.what()));
    }
}
#endif

static void Worker(wstring src, wstring dst, bool autoUnpack, bool del) {
    // 1) проверка свободного места
    PostStatus(L"Проверка места на диске...");
    long long need = NeedBytes(src);
    unsigned long long fr = 0;
    if (!FreeBytes(dst, fr)) {
        PostDone(false, L"Не удаётся определить диск для пути:\n" + dst + L"\n\nПроверьте, что путь указан верно.");
        return;
    }
    if (need >= 0 && (unsigned long long)need > fr) {
        PostDone(false, L"Не хватает места на диске.\n\nНужно:  " + FmtBytes((unsigned long long)need) +
                        L"\nСвободно:  " + FmtBytes(fr) +
                        L"\n\nВыберите другую папку (кнопка ОБЗОР) или освободите место.");
        return;
    }

    // 2) создаём папку (в том числе вложенные)
    int rc = SHCreateDirectoryExW(nullptr, dst.c_str(), nullptr);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS) {
        PostDone(false, L"Не удалось создать папку:\n" + dst + L"\n\nПроверьте путь и права доступа.");
        return;
    }

    // 3) работа
    wstring ext = LowerExt(src);
    if (ext == L".torrent") {
#ifdef USE_LIBTORRENT
        DoTorrent(src, dst, autoUnpack, del);
#else
        PostDone(false, L"Эта сборка без поддержки торрентов.\n"
                        L"Пересоберите с libtorrent и флагом /DUSE_LIBTORRENT,\n"
                        L"либо скачайте торрент в qBittorrent, а здесь распакуйте архив.");
#endif
    } else {
        wstring err;
        int r = Extract(src, dst, 0, 100, err, del);
        if (r == 0) { PostProgress(100); PostDone(true, L"Распаковано в:\n" + dst); }
        else if (r == -1) PostDone(false, L"Отменено.");
        else PostDone(false, err);
    }
}

// ---------- UI ----------
static void SetBusy(bool busy) {
    g_running = busy;
    EnableWindow(g_start, !busy);
    EnableWindow(g_stop, busy);
}

static void PickSource() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW o{};
    o.lStructSize = sizeof(o);
    o.hwndOwner = g_hwnd;
    o.lpstrFilter = L"Архивы и торренты\0*.zip;*.rar;*.7z;*.torrent;*.001;*.tar;*.gz;*.iso\0Все файлы\0*.*\0";
    o.lpstrFile = file; o.nMaxFile = MAX_PATH;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&o)) SetWindowTextW(g_src, file);
}

static int CALLBACK BrowseCb(HWND h, UINT m, LPARAM, LPARAM data) {
    if (m == BFFM_INITIALIZED && data) SendMessageW(h, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

static void PickFolder() {
    wstring cur = GetText(g_dst);
    BROWSEINFOW b{};
    b.hwndOwner = g_hwnd;
    b.lpszTitle = L"Куда распаковать (можно вставить путь вручную)";
    b.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    b.lpfn = BrowseCb;
    b.lParam = (LPARAM)cur.c_str();
    if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&b)) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(g_dst, path);
        CoTaskMemFree(pidl);
    }
}

static void StartWork() {
    wstring src = GetText(g_src), dst = GetText(g_dst);
    if (!FileExists(src)) { MessageBoxW(g_hwnd, L"Выберите существующий файл.", L"Нет файла", MB_ICONWARNING); return; }
    if (dst.empty()) { MessageBoxW(g_hwnd, L"Укажите папку назначения.", L"Нет папки", MB_ICONWARNING); return; }
    // полный путь (на случай относительного)
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(dst.c_str(), MAX_PATH * 4, full, nullptr);
    if (n && n < MAX_PATH * 4) dst = full;
    while (dst.size() > 3 && (dst.back() == L'\\' || dst.back() == L'/')) dst.pop_back();

    bool a = SendMessageW(g_auto, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool d = SendMessageW(g_del, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (g_worker.joinable()) g_worker.join();
    g_stopFlag = false;
    SendMessageW(g_bar, PBM_SETPOS, 0, 0);
    SetBusy(true);
    g_worker = std::thread(Worker, src, dst, a, d);
}

static void StopWork() {
    g_stopFlag = true;
    SetWindowTextW(g_status, L"ОСТАНОВКА...");
    if (HANDLE h = g_proc.load()) TerminateProcess(h, 1);
}

static HWND Mk(const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_hwnd,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        wchar_t videos[MAX_PATH] = L"";
        if (SHGetFolderPathW(nullptr, CSIDL_MYVIDEO, nullptr, 0, videos) != S_OK) videos[0] = 0;

        Mk(L"BUTTON", L" Источник (ZIP / RAR / 7z / .torrent) ", BS_GROUPBOX, 10, 8, 480, 58, 0);
        g_src = Mk(L"EDIT", L"", ES_AUTOHSCROLL, 22, 30, 360, 24, IDC_SRC, WS_EX_CLIENTEDGE);
        Mk(L"BUTTON", L"ВЫБРАТЬ", BS_PUSHBUTTON, 392, 29, 86, 26, IDC_SRCBTN);

        Mk(L"BUTTON", L" Папка назначения (можно вписать путь вручную) ", BS_GROUPBOX, 10, 74, 480, 58, 0);
        g_dst = Mk(L"EDIT", videos, ES_AUTOHSCROLL, 22, 96, 360, 24, IDC_DST, WS_EX_CLIENTEDGE);
        Mk(L"BUTTON", L"ОБЗОР", BS_PUSHBUTTON, 392, 95, 86, 26, IDC_DSTBTN);

        Mk(L"BUTTON", L" Параметры ", BS_GROUPBOX, 10, 140, 480, 80, 0);
        g_auto = Mk(L"BUTTON", L"После загрузки торрента распаковать архивы", BS_AUTOCHECKBOX, 22, 162, 440, 20, IDC_AUTO);
        g_del  = Mk(L"BUTTON", L"Удалить архив после распаковки", BS_AUTOCHECKBOX, 22, 186, 440, 20, IDC_DEL);
        SendMessageW(g_auto, BM_SETCHECK, BST_CHECKED, 0);

        Mk(L"BUTTON", L" Место на диске ", BS_GROUPBOX, 10, 228, 480, 52, 0);
        g_space = Mk(L"STATIC", L"", SS_LEFT, 22, 250, 456, 20, IDC_SPACE);

        Mk(L"BUTTON", L" Состояние ", BS_GROUPBOX, 10, 288, 480, 84, 0);
        g_bar = Mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, 22, 310, 456, 26, IDC_BAR);
        SendMessageW(g_bar, PBM_SETRANGE32, 0, 100);
        g_status = Mk(L"STATIC", L"ГОТОВ", SS_CENTER, 22, 344, 456, 20, IDC_STATUS);

        g_start = Mk(L"BUTTON", L"СТАРТ", BS_DEFPUSHBUTTON, 204, 386, 90, 30, IDC_START);
        g_stop  = Mk(L"BUTTON", L"СТОП", BS_PUSHBUTTON, 302, 386, 90, 30, IDC_STOP);
        Mk(L"BUTTON", L"ЗАКРЫТЬ", BS_PUSHBUTTON, 400, 386, 90, 30, IDC_CLOSE);
        EnableWindow(g_stop, FALSE);

        // системный шрифт для всех контролов
        NONCLIENTMETRICSW ncm{ sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        HFONT f = CreateFontIndirectW(&ncm.lfMessageFont);
        EnumChildWindows(hwnd, [](HWND c, LPARAM l) -> BOOL {
            SendMessageW(c, WM_SETFONT, (WPARAM)l, TRUE); return TRUE; }, (LPARAM)f);
        DragAcceptFiles(hwnd, TRUE);
        g_ready = true;
        UpdateSpace();
        return 0;
    }
    case WM_DROPFILES: {
        wchar_t p[MAX_PATH];
        if (DragQueryFileW((HDROP)wp, 0, p, MAX_PATH)) SetWindowTextW(g_src, p);
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && g_ready) {
            if (LOWORD(wp) == IDC_DST) UpdateSpace();
            else if (LOWORD(wp) == IDC_SRC) RefreshNeed();
            return 0;
        }
        switch (LOWORD(wp)) {
        case IDC_SRCBTN: PickSource(); break;
        case IDC_DSTBTN: PickFolder(); break;
        case IDC_START:  StartWork(); break;
        case IDC_STOP:   StopWork(); break;
        case IDC_CLOSE:  SendMessageW(hwnd, WM_CLOSE, 0, 0); break;
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_space) {
            HDC dc = (HDC)wp;
            SetTextColor(dc, g_spaceBad ? RGB(200, 0, 0) : GetSysColor(COLOR_WINDOWTEXT));
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_SPACE: {
        long long* p = (long long*)lp;
        if ((int)wp == g_gen.load()) { g_need = *p; UpdateSpace(); }
        delete p;
        return 0;
    }
    case WM_PROGRESS:
        SendMessageW(g_bar, PBM_SETPOS, wp, 0);
        return 0;
    case WM_STATUS: {
        wstring* s = (wstring*)lp;
        SetWindowTextW(g_status, s->c_str());
        delete s;
        return 0;
    }
    case WM_DONE: {
        wstring* s = (wstring*)lp;
        SetBusy(false);
        if (wp) SendMessageW(g_bar, PBM_SETPOS, 100, 0);
        SetWindowTextW(g_status, wp ? L"ГОТОВО" : L"ОСТАНОВЛЕНО / ОШИБКА");
        UpdateSpace();
        MessageBoxW(hwnd, s->c_str(), wp ? L"Готово" : L"Сообщение", wp ? MB_ICONINFORMATION : MB_ICONWARNING);
        delete s;
        return 0;
    }
    case WM_CLOSE:
        if (g_running) {
            if (MessageBoxW(hwnd, L"Операция идёт. Прервать и выйти?", L"Выход", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            StopWork();
        }
        if (g_worker.joinable()) g_worker.join();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX ic{ sizeof(ic), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));   // иконка из resource.rc
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"UnpackerWnd";
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{ 0, 0, 500, 428 };
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"UnpackerWnd", L"Распаковщик", style, x, y, w, h,
                                nullptr, nullptr, hInst, nullptr);
    // маленькая иконка для заголовка окна, большая — для Alt+Tab и панели задач
    HICON big = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON sm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!big || !sm) {   // запасной вариант: icon.ico рядом с .exe
        wchar_t ep[MAX_PATH];
        GetModuleFileNameW(nullptr, ep, MAX_PATH);
        wstring ip = ep;
        ip = ip.substr(0, ip.find_last_of(L"\\/") + 1) + L"icon.ico";
        if (!big) big = (HICON)LoadImageW(nullptr, ip.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
        if (!sm)  sm  = (HICON)LoadImageW(nullptr, ip.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
    }
    if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (sm)  SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)sm);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    CoUninitialize();
    return 0;
}
