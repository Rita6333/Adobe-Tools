#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <netfw.h>
#include <taskschd.h>
#include <comdef.h>
#include <winhttp.h>
#include <bcrypt.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "webview2/WebView2.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

// ============================================================================
// Minimal COM Callback Helper for WebView2 (replaces WRL dependency)
// ============================================================================

template <typename Interface, typename A1, typename A2>
class ComCallback2 : public Interface {
    std::atomic<ULONG> m_ref{1};
    std::function<HRESULT(A1, A2)> m_fn;
public:
    explicit ComCallback2(std::function<HRESULT(A1, A2)> fn) : m_fn(std::move(fn)) {}
    virtual ~ComCallback2() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(Interface)) {
            *ppv = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(A1 a, A2 b) override { return m_fn(a, b); }
};

template <typename Interface, typename A1>
class ComCallback1 : public Interface {
    std::atomic<ULONG> m_ref{1};
    std::function<HRESULT(A1)> m_fn;
public:
    explicit ComCallback1(std::function<HRESULT(A1)> fn) : m_fn(std::move(fn)) {}
    virtual ~ComCallback1() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(Interface)) {
            *ppv = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(A1 a) override { return m_fn(a); }
};

// ============================================================================
// String & JSON Utilities
// ============================================================================

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring res(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &res[0], len);
    return res;
}

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string res(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &res[0], len, nullptr, nullptr);
    return res;
}

static std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::wstring ToLowerWide(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

static std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return "";
    pos = json.find('\"', pos + 1);
    if (pos == std::string::npos) return "";
    std::string out;
    for (size_t i = pos + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            char next = json[++i];
            if (next == 'n') out += '\n';
            else if (next == 'r') out += '\r';
            else if (next == 't') out += '\t';
            else out += next;
        } else if (c == '\"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

static bool JsonGetBool(const std::string& json, const std::string& key, bool defaultVal = false) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return defaultVal;
    size_t v = json.find_first_not_of(" \t\r\n", pos + 1);
    if (v == std::string::npos) return defaultVal;
    if (json.compare(v, 4, "true") == 0) return true;
    if (json.compare(v, 5, "false") == 0) return false;
    return defaultVal;
}

// ============================================================================
// Constants & Built-in Domains
// ============================================================================

static const char* kAppVersion             = "2.2.0";
static const char* kGitHubRepo             = "Rita6333/Adobe-Tools";
static const char* kDefaultUpdateApiUrl    = "https://api.github.com/repos/Rita6333/Adobe-Tools/releases/latest";
static const char* kDefaultReleasesPageUrl = "https://github.com/Rita6333/Adobe-Tools/releases";

static const char* kRuleGroupPrimary   = "Zaka Adobe Block";
static const char* kRuleGroupLegacy    = "ZakaAdobeBlock";
static const char* kRulePrefix         = "ZakaAdobeBlock";

static const char* kHostsBeginPrimary  = "# >>> Zaka Adobe Block BEGIN >>>";
static const char* kHostsEndPrimary    = "# <<< Zaka Adobe Block END <<<";
static const char* kHostsBeginLegacy   = "# >>> ZakaAdobeBlock BEGIN >>>";
static const char* kHostsEndLegacy     = "# <<< ZakaAdobeBlock END <<<";

static const std::vector<std::string> kBuiltInDomains = {
    "activate.adobe.com",
    "practivate.adobe.com",
    "activate-sea.adobe.com",
    "activate-sjc0.adobe.com",
    "activate.wip3.adobe.com",
    "activate.wip4.adobe.com",
    "ereg.adobe.com",
    "ereg.wip3.adobe.com",
    "wip3.adobe.com",
    "wip4.adobe.com",
    "3dns.adobe.com",
    "3dns-2.adobe.com",
    "3dns-3.adobe.com",
    "3dns-4.adobe.com",
    "adobe-dns.adobe.com",
    "adobe-dns-2.adobe.com",
    "adobe-dns-3.adobe.com",
    "adobe-dns-4.adobe.com",
    "wwis-dubc1-vip60.adobe.com",
    "wwis-dubc1-vip61.adobe.com",
    "uds.licenses.adobe.com",
    "lm.licenses.adobe.com",
    "lmlicenses.wip4.adobe.com",
    "licenses.adobe.com",
    "na1r.services.adobe.com",
    "na1m-pr.licenses.adobe.com",
    "na2m-pr.licenses.adobe.com",
    "genuine.adobe.com",
    "prod.adobegenuine.com",
    "adobegenuine.com",
    "ims-na1.adobelogin.com",
    "adobelogin.com",
    "swupmf.adobe.com",
    "swupdl.adobe.com",
    "armmf.adobe.com",
    "armdl.adobe.com",
    "prod-wd.adobe.com",
    "cc-cdn.adobe.com",
    "cc-api-data.adobe.io",
    "ic.adobe.io",
    "ccd.storage.adobe.io"
};

// ============================================================================
// System Paths & Privileges
// ============================================================================

static fs::path GetAppDir() {
    wchar_t buf[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return fs::path(buf).parent_path();
}

static fs::path GetStateDir() {
    wchar_t buf[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH) > 0) {
        return fs::path(buf) / L"ZakaAdobeNetworkBlock";
    }
    return fs::path(L"C:\\ProgramData\\ZakaAdobeNetworkBlock");
}

static fs::path GetStateFilePath() { return GetStateDir() / L"state.json"; }
static fs::path GetHostsBackupPath() { return GetStateDir() / L"hosts.original"; }
static fs::path GetCustomDomainsPath() { return GetAppDir() / L"adobe-domains.txt"; }

static fs::path GetSystemHostsPath() {
    wchar_t buf[MAX_PATH] = {0};
    if (GetSystemDirectoryW(buf, MAX_PATH) > 0) {
        return fs::path(buf) / L"drivers\\etc\\hosts";
    }
    return fs::path(L"C:\\Windows\\System32\\drivers\\etc\\hosts");
}

static bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

static bool IsFirewallServiceRunning() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"mpssvc", SERVICE_QUERY_STATUS);
    bool running = false;
    if (svc) {
        SERVICE_STATUS_PROCESS ssp = {0};
        DWORD bytesNeeded = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
            running = (ssp.dwCurrentState == SERVICE_RUNNING);
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return running;
}

static int RunHiddenCommand(const std::wstring& cmdLine) {
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    std::wstring mutableCmd = cmdLine;
    if (!CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)exitCode;
}

static std::string ReadFileUtf8(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return "";
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string s = oss.str();
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

static bool WriteFileUtf8(const fs::path& p, const std::string& content) {
    try {
        if (p.has_parent_path() && !fs::exists(p.parent_path())) {
            fs::create_directories(p.parent_path());
        }
        if (fs::exists(p)) {
            DWORD attr = GetFileAttributesW(p.wstring().c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(p.wstring().c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
            }
        }
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(content.data(), (std::streamsize)content.size());
        return ofs.good();
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Domains & Hosts Engine
// ============================================================================

static std::vector<std::string> GetAdobeDomains() {
    std::set<std::string> uniqueDomains;
    for (const auto& d : kBuiltInDomains) {
        uniqueDomains.insert(ToLowerAscii(Trim(d)));
    }
    fs::path customPath = GetCustomDomainsPath();
    if (fs::exists(customPath)) {
        std::istringstream iss(ReadFileUtf8(customPath));
        std::string line;
        while (std::getline(iss, line)) {
            std::string t = Trim(line);
            if (t.empty() || t[0] == '#') continue;
            std::istringstream ls(t);
            std::string tok, lastTok;
            while (ls >> tok) lastTok = tok;
            lastTok = ToLowerAscii(Trim(lastTok));
            if (!lastTok.empty() && lastTok[0] != '#') {
                uniqueDomains.insert(lastTok);
            }
        }
    }
    return std::vector<std::string>(uniqueDomains.begin(), uniqueDomains.end());
}

static bool IsHostsBeginMarker(const std::string& trimmed) {
    return trimmed == kHostsBeginPrimary || trimmed == kHostsBeginLegacy;
}

static bool IsHostsEndMarker(const std::string& trimmed) {
    return trimmed == kHostsEndPrimary || trimmed == kHostsEndLegacy;
}

static bool HostsHasBlock(const std::string& content) {
    return content.find(kHostsBeginPrimary) != std::string::npos ||
           content.find(kHostsBeginLegacy) != std::string::npos;
}

static std::string StripHostsBlock(const std::string& content, bool* outModified = nullptr) {
    if (outModified) *outModified = false;
    if (content.empty()) return "";

    std::string normalized = content;
    bool gluedModified = false;
    const char* begins[] = {kHostsBeginPrimary, kHostsBeginLegacy};
    for (const char* b : begins) {
        std::string bStr(b);
        size_t pos = 0;
        while ((pos = normalized.find(bStr, pos)) != std::string::npos) {
            if (pos > 0 && normalized[pos - 1] != '\n' && normalized[pos - 1] != '\r') {
                normalized.insert(pos, "\r\n");
                gluedModified = true;
                pos += 2 + bStr.size();
            } else {
                pos += bStr.size();
            }
        }
    }

    std::vector<std::string> rawLines;
    size_t start = 0;
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (normalized[i] == '\n') {
            rawLines.push_back(normalized.substr(start, i - start + 1));
            start = i + 1;
        }
    }
    if (start < normalized.size()) {
        rawLines.push_back(normalized.substr(start));
    }

    std::vector<std::string> kept;
    bool inside = false;
    bool modified = gluedModified;

    for (const auto& raw : rawLines) {
        std::string trimmed = Trim(raw);
        if (!inside) {
            if (IsHostsBeginMarker(trimmed)) {
                inside = true;
                modified = true;
                continue;
            }
            if (IsHostsEndMarker(trimmed)) {
                modified = true;
                continue;
            }
            kept.push_back(raw);
        } else {
            if (IsHostsEndMarker(trimmed)) {
                inside = false;
            }
        }
    }

    if (outModified) *outModified = modified;
    if (!modified) return content;

    std::string joined;
    for (const auto& l : kept) joined += l;

    while (joined.size() >= 4 &&
           joined.compare(joined.size() - 4, 4, "\r\n\r\n") == 0) {
        joined.erase(joined.size() - 2);
    }
    while (joined.size() >= 2 &&
           joined.compare(joined.size() - 2, 2, "\n\n") == 0) {
        joined.erase(joined.size() - 1);
    }

    if (gluedModified && (content.empty() || (content.back() != '\n' && content.back() != '\r'))) {
        while (!joined.empty() && (joined.back() == '\r' || joined.back() == '\n')) {
            joined.pop_back();
        }
    }
    return joined;
}

static std::string BuildHostsBlock(const std::vector<std::string>& domains) {
    std::ostringstream oss;
    oss << kHostsBeginPrimary << "\r\n";
    oss << "# Added by Zaka Adobe Network Block. Delete this block to undo.\r\n";
    for (const auto& d : domains) {
        if (d.empty()) continue;
        oss << "0.0.0.0 " << d << "\r\n";
        oss << "::1 " << d << "\r\n";
    }
    oss << kHostsEndPrimary << "\r\n";
    return oss.str();
}

static std::string ApplyHostsToString(const std::string& existingContent, const std::vector<std::string>& domains) {
    bool dummy = false;
    std::string clean = StripHostsBlock(existingContent, &dummy);
    while (!clean.empty() && (clean.back() == '\r' || clean.back() == '\n')) {
        clean.pop_back();
    }
    std::string block = BuildHostsBlock(domains);
    if (clean.empty()) return block;
    return clean + "\r\n\r\n" + block;
}

static bool ApplySystemHosts(const std::vector<std::string>& domains, std::string* errOut = nullptr) {
    fs::path hostsPath = GetSystemHostsPath();
    fs::path backupPath = GetHostsBackupPath();
    try {
        if (!fs::exists(GetStateDir())) fs::create_directories(GetStateDir());
        if (fs::exists(hostsPath) && !fs::exists(backupPath)) {
            fs::copy_file(hostsPath, backupPath, fs::copy_options::none);
        }
    } catch (...) {}

    std::string current = ReadFileUtf8(hostsPath);
    std::string updated = ApplyHostsToString(current, domains);
    if (!WriteFileUtf8(hostsPath, updated)) {
        if (errOut) *errOut = "无法写入 hosts 文件（可能缺少管理员权限或被安全软件锁定）";
        return false;
    }
    RunHiddenCommand(L"ipconfig.exe /flushdns");
    return true;
}

static bool RevertSystemHosts() {
    fs::path hostsPath = GetSystemHostsPath();
    if (!fs::exists(hostsPath)) return false;
    std::string current = ReadFileUtf8(hostsPath);
    bool modified = false;
    std::string stripped = StripHostsBlock(current, &modified);
    if (!modified) return false;
    while (!stripped.empty() && (stripped.back() == '\r' || stripped.back() == '\n')) {
        stripped.pop_back();
    }
    if (!stripped.empty()) stripped += "\r\n";
    WriteFileUtf8(hostsPath, stripped);
    RunHiddenCommand(L"ipconfig.exe /flushdns");
    return true;
}

// ============================================================================
// Scan Adobe Executables (Standard Folders + Registry Uninstall + Extra Paths)
// ============================================================================

static std::wstring GetEnvVarW(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n > 0 ? std::wstring(buf, n) : L"";
}

static void ScanRegistryUninstallDirs(HKEY rootKey, const wchar_t* subKeyPath, std::set<std::wstring>& roots) {
    HKEY hUninst = nullptr;
    if (RegOpenKeyExW(rootKey, subKeyPath, 0, KEY_READ, &hUninst) != ERROR_SUCCESS) return;

    wchar_t subName[256];
    DWORD idx = 0;
    DWORD subNameLen = 256;
    while (RegEnumKeyExW(hUninst, idx++, subName, &subNameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        subNameLen = 256;
        HKEY hItem = nullptr;
        if (RegOpenKeyExW(hUninst, subName, 0, KEY_READ, &hItem) == ERROR_SUCCESS) {
            auto readRegStr = [&](const wchar_t* valName) -> std::wstring {
                wchar_t val[1024] = {0};
                DWORD sz = sizeof(val);
                DWORD type = 0;
                if (RegQueryValueExW(hItem, valName, nullptr, &type, (LPBYTE)val, &sz) == ERROR_SUCCESS &&
                    (type == REG_SZ || type == REG_EXPAND_SZ)) {
                    return std::wstring(val);
                }
                return L"";
            };

            std::wstring dn = ToLowerWide(readRegStr(L"DisplayName"));
            std::wstring pub = ToLowerWide(readRegStr(L"Publisher"));
            if (dn.find(L"adobe") == 0 || pub.find(L"adobe") == 0) {
                std::wstring loc = readRegStr(L"InstallLocation");
                if (!loc.empty()) {
                    if (loc.front() == L'\"') loc.erase(0, 1);
                    if (!loc.empty() && loc.back() == L'\"') loc.pop_back();
                    if (fs::exists(loc)) roots.insert(loc);
                }
                std::wstring icon = readRegStr(L"DisplayIcon");
                if (!icon.empty()) {
                    size_t comma = icon.find(L',');
                    if (comma != std::wstring::npos) icon = icon.substr(0, comma);
                    if (!icon.empty() && icon.front() == L'\"') icon.erase(0, 1);
                    if (!icon.empty() && icon.back() == L'\"') icon.pop_back();
                    fs::path ip(icon);
                    if (fs::exists(ip) && ip.has_parent_path()) {
                        roots.insert(ip.parent_path().wstring());
                    }
                }
            }
            RegCloseKey(hItem);
        }
    }
    RegCloseKey(hUninst);
}

static std::vector<std::string> FindAdobeExecutables(const std::vector<std::string>& extraPaths) {
    std::set<std::wstring> roots;
    auto addEnvSub = [&](const wchar_t* envName, const wchar_t* sub) {
        std::wstring base = GetEnvVarW(envName);
        if (!base.empty()) {
            fs::path p = fs::path(base) / sub;
            roots.insert(p.wstring());
        }
    };

    addEnvSub(L"ProgramFiles", L"Adobe");
    addEnvSub(L"ProgramFiles", L"Common Files\\Adobe");
    addEnvSub(L"ProgramFiles(x86)", L"Adobe");
    addEnvSub(L"ProgramFiles(x86)", L"Common Files\\Adobe");
    addEnvSub(L"ProgramData", L"Adobe");
    addEnvSub(L"LOCALAPPDATA", L"Adobe");

    std::wstring localApp = GetEnvVarW(L"LOCALAPPDATA");
    if (!localApp.empty()) {
        fs::path progs = fs::path(localApp) / L"Programs";
        if (fs::exists(progs)) {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(progs, ec)) {
                if (entry.is_directory(ec)) {
                    std::wstring fname = ToLowerWide(entry.path().filename().wstring());
                    if (fname.find(L"adobe") != std::wstring::npos) {
                        roots.insert(entry.path().wstring());
                    }
                }
            }
        }
    }

    ScanRegistryUninstallDirs(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", roots);
    ScanRegistryUninstallDirs(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall", roots);
    ScanRegistryUninstallDirs(HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", roots);

    for (const auto& ep : extraPaths) {
        if (!ep.empty()) roots.insert(Utf8ToWide(ep));
    }

    std::map<std::wstring, std::string> uniqueExes;
    std::error_code ec;

    for (const auto& r : roots) {
        fs::path rp(r);
        if (!fs::exists(rp, ec)) continue;
        if (fs::is_regular_file(rp, ec)) {
            if (ToLowerWide(rp.extension().wstring()) == L".exe") {
                uniqueExes[ToLowerWide(rp.wstring())] = WideToUtf8(rp.wstring());
            }
            continue;
        }
        fs::recursive_directory_iterator it(rp, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        while (it != end) {
            if (it->is_regular_file(ec)) {
                if (ToLowerWide(it->path().extension().wstring()) == L".exe") {
                    std::wstring fullW = it->path().wstring();
                    uniqueExes[ToLowerWide(fullW)] = WideToUtf8(fullW);
                }
            }
            it.increment(ec);
        }
    }

    std::vector<std::string> res;
    res.reserve(uniqueExes.size());
    for (const auto& kv : uniqueExes) {
        res.push_back(kv.second);
    }
    return res;
}

// ============================================================================
// Windows Firewall COM Engine (INetFwPolicy2)
// ============================================================================

static std::wstring BuildFirewallRuleName(const std::string& exePath, bool inbound) {
    std::string dir = inbound ? "In" : "Out";
    std::string lower = ToLowerAscii(exePath);
    std::string safe;
    safe.reserve(lower.size());
    for (char c : lower) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')
            safe += c;
        else
            safe += '_';
    }
    if (safe.size() > 110) {
        safe = safe.substr(safe.size() - 110);
    }
    return Utf8ToWide(std::string(kRulePrefix) + "-" + dir + "-" + safe);
}

static bool IsOurFirewallRule(INetFwRule* rule, BSTR* outName = nullptr) {
    if (!rule) return false;
    BSTR bGroup = nullptr;
    bool match = false;
    if (SUCCEEDED(rule->get_Grouping(&bGroup)) && bGroup) {
        std::wstring grp(bGroup);
        if (_wcsicmp(grp.c_str(), L"Zaka Adobe Block") == 0 ||
            _wcsicmp(grp.c_str(), L"ZakaAdobeBlock") == 0) {
            match = true;
        }
        SysFreeString(bGroup);
    }
    BSTR bName = nullptr;
    if (SUCCEEDED(rule->get_Name(&bName)) && bName) {
        if (!match && _wcsnicmp(bName, L"ZakaAdobeBlock-", 15) == 0) {
            match = true;
        }
        if (match && outName) {
            *outName = bName;
            return true;
        }
        SysFreeString(bName);
    }
    return match;
}

static int CountFirewallRules() {
    INetFwPolicy2* pPolicy = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2), (void**)&pPolicy);
    if (FAILED(hr) || !pPolicy) return 0;

    INetFwRules* pRules = nullptr;
    int count = 0;
    if (SUCCEEDED(pPolicy->get_Rules(&pRules)) && pRules) {
        IUnknown* pUnk = nullptr;
        if (SUCCEEDED(pRules->get__NewEnum(&pUnk)) && pUnk) {
            IEnumVARIANT* pEnum = nullptr;
            if (SUCCEEDED(pUnk->QueryInterface(__uuidof(IEnumVARIANT), (void**)&pEnum)) && pEnum) {
                VARIANT var;
                VariantInit(&var);
                ULONG fetched = 0;
                while (pEnum->Next(1, &var, &fetched) == S_OK && fetched > 0) {
                    if (var.vt == VT_DISPATCH && var.pdispVal) {
                        INetFwRule* pRule = nullptr;
                        if (SUCCEEDED(var.pdispVal->QueryInterface(__uuidof(INetFwRule), (void**)&pRule)) && pRule) {
                            if (IsOurFirewallRule(pRule)) count++;
                            pRule->Release();
                        }
                    }
                    VariantClear(&var);
                }
                pEnum->Release();
            }
            pUnk->Release();
        }
        pRules->Release();
    }
    pPolicy->Release();
    return count;
}

static int ClearFirewallRules() {
    INetFwPolicy2* pPolicy = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2), (void**)&pPolicy);
    int removed = 0;
    if (SUCCEEDED(hr) && pPolicy) {
        INetFwRules* pRules = nullptr;
        if (SUCCEEDED(pPolicy->get_Rules(&pRules)) && pRules) {
            std::vector<BSTR> namesToRemove;
            IUnknown* pUnk = nullptr;
            if (SUCCEEDED(pRules->get__NewEnum(&pUnk)) && pUnk) {
                IEnumVARIANT* pEnum = nullptr;
                if (SUCCEEDED(pUnk->QueryInterface(__uuidof(IEnumVARIANT), (void**)&pEnum)) && pEnum) {
                    VARIANT var;
                    VariantInit(&var);
                    ULONG fetched = 0;
                    while (pEnum->Next(1, &var, &fetched) == S_OK && fetched > 0) {
                        if (var.vt == VT_DISPATCH && var.pdispVal) {
                            INetFwRule* pRule = nullptr;
                            if (SUCCEEDED(var.pdispVal->QueryInterface(__uuidof(INetFwRule), (void**)&pRule)) && pRule) {
                                BSTR bName = nullptr;
                                if (IsOurFirewallRule(pRule, &bName) && bName) {
                                    namesToRemove.push_back(bName);
                                }
                                pRule->Release();
                            }
                        }
                        VariantClear(&var);
                    }
                    pEnum->Release();
                }
                pUnk->Release();
            }
            for (BSTR bName : namesToRemove) {
                if (SUCCEEDED(pRules->Remove(bName))) removed++;
                SysFreeString(bName);
            }
            pRules->Release();
        }
        pPolicy->Release();
    }
    RunHiddenCommand(L"netsh.exe advfirewall firewall delete rule name=all group=\"Zaka Adobe Block\"");
    RunHiddenCommand(L"netsh.exe advfirewall firewall delete rule name=all group=\"ZakaAdobeBlock\"");
    return removed;
}

static bool AddFirewallBlockRule(const std::string& exePath, bool inbound) {
    std::wstring ruleName = BuildFirewallRuleName(exePath, inbound);
    std::wstring exeW = Utf8ToWide(exePath);

    INetFwPolicy2* pPolicy = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2), (void**)&pPolicy);
    if (SUCCEEDED(hr) && pPolicy) {
        INetFwRules* pRules = nullptr;
        if (SUCCEEDED(pPolicy->get_Rules(&pRules)) && pRules) {
            BSTR bRuleName = SysAllocString(ruleName.c_str());
            pRules->Remove(bRuleName);

            INetFwRule* pRule = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwRule), (void**)&pRule)) && pRule) {
                BSTR bDesc = SysAllocString(L"Blocks Adobe product network access (Zaka Adobe Network Block)");
                BSTR bApp  = SysAllocString(exeW.c_str());
                BSTR bGrp  = SysAllocString(L"Zaka Adobe Block");

                pRule->put_Name(bRuleName);
                pRule->put_Description(bDesc);
                pRule->put_ApplicationName(bApp);
                pRule->put_Direction(inbound ? NET_FW_RULE_DIR_IN : NET_FW_RULE_DIR_OUT);
                pRule->put_Action(NET_FW_ACTION_BLOCK);
                pRule->put_Enabled(VARIANT_TRUE);
                pRule->put_Profiles(NET_FW_PROFILE2_ALL);
                pRule->put_Grouping(bGrp);

                HRESULT addHr = pRules->Add(pRule);

                SysFreeString(bDesc);
                SysFreeString(bApp);
                SysFreeString(bGrp);
                SysFreeString(bRuleName);
                pRule->Release();
                pRules->Release();
                pPolicy->Release();
                if (SUCCEEDED(addHr)) return true;
            } else {
                SysFreeString(bRuleName);
                pRules->Release();
                pPolicy->Release();
            }
        } else {
            pPolicy->Release();
        }
    }

    // Fallback to netsh
    std::wstring dirFlag = inbound ? L"in" : L"out";
    RunHiddenCommand(L"netsh.exe advfirewall firewall delete rule name=\"" + ruleName + L"\"");
    int rc = RunHiddenCommand(L"netsh.exe advfirewall firewall add rule name=\"" + ruleName +
                              L"\" dir=" + dirFlag + L" program=\"" + exeW +
                              L"\" action=block enable=yes profile=any group=\"Zaka Adobe Block\"");
    return rc == 0;
}

// ============================================================================
// Windows Services & Scheduled Tasks (Win32 SCM & Task Scheduler COM)
// ============================================================================

struct ServiceOrTaskInfo {
    std::string kind;
    std::string name;
    std::string display;
    std::string state;
    std::string startMode;
};

static bool IsAdobeService(const std::wstring& name, const std::wstring& display, const std::wstring& binPath) {
    std::wstring ln = ToLowerWide(name);
    std::wstring ld = ToLowerWide(display);
    std::wstring lp = ToLowerWide(binPath);
    if (ln == L"agsservice" || ln == L"agmservice" || ln == L"adobearmservice" || ln == L"adobeupdateservice")
        return true;
    if (ln.find(L"adobe") != std::wstring::npos ||
        ld.find(L"adobe") != std::wstring::npos ||
        lp.find(L"adobe") != std::wstring::npos)
        return true;
    return false;
}

static std::vector<ServiceOrTaskInfo> FindAdobeServices() {
    std::vector<ServiceOrTaskInfo> list;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return list;

    DWORD bytesNeeded = 0, servicesReturned = 0, resumeHandle = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                          nullptr, 0, &bytesNeeded, &servicesReturned, &resumeHandle, nullptr);
    if (bytesNeeded == 0) {
        CloseServiceHandle(scm);
        return list;
    }

    std::vector<BYTE> buf(bytesNeeded);
    resumeHandle = 0;
    if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                              buf.data(), (DWORD)buf.size(), &bytesNeeded, &servicesReturned, &resumeHandle, nullptr)) {
        auto* arr = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
        for (DWORD i = 0; i < servicesReturned; ++i) {
            std::wstring sName = arr[i].lpServiceName ? arr[i].lpServiceName : L"";
            std::wstring sDisp = arr[i].lpDisplayName ? arr[i].lpDisplayName : L"";
            std::wstring sPath;
            std::string startMode = "Manual";

            SC_HANDLE hSvc = OpenServiceW(scm, sName.c_str(), SERVICE_QUERY_CONFIG);
            if (hSvc) {
                DWORD cfgBytes = 0;
                QueryServiceConfigW(hSvc, nullptr, 0, &cfgBytes);
                if (cfgBytes > 0) {
                    std::vector<BYTE> cfgBuf(cfgBytes);
                    auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cfgBuf.data());
                    if (QueryServiceConfigW(hSvc, cfg, cfgBytes, &cfgBytes)) {
                        if (cfg->lpBinaryPathName) sPath = cfg->lpBinaryPathName;
                        if (cfg->dwStartType == SERVICE_AUTO_START) startMode = "Auto";
                        else if (cfg->dwStartType == SERVICE_DISABLED) startMode = "Disabled";
                        else startMode = "Manual";
                    }
                }
                CloseServiceHandle(hSvc);
            }

            if (IsAdobeService(sName, sDisp, sPath)) {
                std::string stateStr = (arr[i].ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING) ? "Running" : "Stopped";
                list.push_back({
                    "后台服务",
                    WideToUtf8(sName),
                    WideToUtf8(sDisp.empty() ? sPath : sDisp),
                    stateStr,
                    startMode
                });
            }
        }
    }
    CloseServiceHandle(scm);
    return list;
}

static void TraverseTaskFolder(ITaskFolder* folder, std::vector<ServiceOrTaskInfo>& outList) {
    if (!folder) return;

    IRegisteredTaskCollection* pTasks = nullptr;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &pTasks)) && pTasks) {
        LONG count = 0;
        pTasks->get_Count(&count);
        for (LONG i = 1; i <= count; ++i) {
            IRegisteredTask* pTask = nullptr;
            VARIANT idx;
            VariantInit(&idx);
            idx.vt = VT_I4;
            idx.lVal = i;
            if (SUCCEEDED(pTasks->get_Item(idx, &pTask)) && pTask) {
                BSTR bName = nullptr, bPath = nullptr, bXml = nullptr;
                VARIANT_BOOL bEnabled = VARIANT_TRUE;
                TASK_STATE tState = TASK_STATE_UNKNOWN;

                pTask->get_Name(&bName);
                pTask->get_Path(&bPath);
                pTask->get_Xml(&bXml);
                pTask->get_Enabled(&bEnabled);
                pTask->get_State(&tState);

                std::wstring wName = bName ? bName : L"";
                std::wstring wPath = bPath ? bPath : L"";
                std::wstring wXml  = bXml  ? bXml  : L"";

                bool isAdobe = (ToLowerWide(wName).find(L"adobe") != std::wstring::npos) ||
                               (ToLowerWide(wPath).find(L"adobe") != std::wstring::npos) ||
                               (ToLowerWide(wXml).find(L"adobe") != std::wstring::npos);

                if (isAdobe) {
                    std::wstring folderPath = L"\\";
                    size_t lastSlash = wPath.find_last_of(L'\\');
                    if (lastSlash != std::wstring::npos) {
                        folderPath = wPath.substr(0, lastSlash + 1);
                    }
                    bool enabled = (bEnabled != VARIANT_FALSE) && (tState != TASK_STATE_DISABLED);
                    std::string stateStr = !enabled ? "Disabled" : (tState == TASK_STATE_RUNNING ? "Running" : "Ready");
                    outList.push_back({
                        "计划任务",
                        WideToUtf8(wName),
                        WideToUtf8(folderPath),
                        stateStr,
                        enabled ? "Enabled" : "Disabled"
                    });
                }

                if (bName) SysFreeString(bName);
                if (bPath) SysFreeString(bPath);
                if (bXml)  SysFreeString(bXml);
                pTask->Release();
            }
        }
        pTasks->Release();
    }

    ITaskFolderCollection* pFolders = nullptr;
    if (SUCCEEDED(folder->GetFolders(0, &pFolders)) && pFolders) {
        LONG count = 0;
        pFolders->get_Count(&count);
        for (LONG i = 1; i <= count; ++i) {
            ITaskFolder* pSub = nullptr;
            VARIANT idx;
            VariantInit(&idx);
            idx.vt = VT_I4;
            idx.lVal = i;
            if (SUCCEEDED(pFolders->get_Item(idx, &pSub)) && pSub) {
                BSTR bSubPath = nullptr;
                pSub->get_Path(&bSubPath);
                std::wstring sp = bSubPath ? bSubPath : L"";
                if (bSubPath) SysFreeString(bSubPath);
                if (_wcsnicmp(sp.c_str(), L"\\Microsoft", 10) != 0) {
                    TraverseTaskFolder(pSub, outList);
                }
                pSub->Release();
            }
        }
        pFolders->Release();
    }
}

static std::vector<ServiceOrTaskInfo> FindAdobeTasks() {
    std::vector<ServiceOrTaskInfo> list;
    ITaskService* pService = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITaskService), (void**)&pService)) && pService) {
        VARIANT empty;
        VariantInit(&empty);
        if (SUCCEEDED(pService->Connect(empty, empty, empty, empty))) {
            ITaskFolder* pRoot = nullptr;
            BSTR bRoot = SysAllocString(L"\\");
            if (SUCCEEDED(pService->GetFolder(bRoot, &pRoot)) && pRoot) {
                TraverseTaskFolder(pRoot, list);
                pRoot->Release();
            }
            SysFreeString(bRoot);
        }
        pService->Release();
    }
    return list;
}

// ============================================================================
// State Persistence (state.json)
// ============================================================================

struct SavedState {
    std::string created;
    int exeCount = 0;
    std::vector<std::pair<std::string, std::string>> services; // Name -> StartMode
    std::vector<std::tuple<std::string, std::string, std::string>> tasks; // TaskPath, TaskName, State
};

static SavedState LoadSavedState() {
    SavedState st;
    fs::path p = GetStateFilePath();
    if (!fs::exists(p)) return st;
    std::string json = ReadFileUtf8(p);
    st.created = JsonGetString(json, "created");

    // Extract service records
    std::regex svcRe("\\{[^{}]*\"Name\"\\s*:\\s*\"([^\"]+)\"[^{}]*\"StartMode\"\\s*:\\s*\"([^\"]+)\"[^{}]*\\}", std::regex::icase);
    auto svcBegin = std::sregex_iterator(json.begin(), json.end(), svcRe);
    auto svcEnd   = std::sregex_iterator();
    for (auto it = svcBegin; it != svcEnd; ++it) {
        st.services.push_back({(*it)[1].str(), (*it)[2].str()});
    }

    // Extract task records
    std::regex taskRe("\\{[^{}]*\"TaskName\"\\s*:\\s*\"([^\"]+)\"[^{}]*\"TaskPath\"\\s*:\\s*\"([^\"]+)\"[^{}]*\"State\"\\s*:\\s*\"([^\"]+)\"[^{}]*\\}", std::regex::icase);
    auto tBegin = std::sregex_iterator(json.begin(), json.end(), taskRe);
    for (auto it = tBegin; it != svcEnd; ++it) {
        st.tasks.push_back({(*it)[2].str(), (*it)[1].str(), (*it)[3].str()});
    }
    return st;
}

static void SaveAppState(const SavedState& newState) {
    SavedState existing = LoadSavedState();
    std::map<std::string, std::string> origSvcModes;
    for (const auto& kv : existing.services) {
        if (ToLowerAscii(kv.second) != "disabled") origSvcModes[kv.first] = kv.second;
    }

    std::ostringstream oss;
    oss << "{\r\n";
    oss << "  \"created\": \"" << JsonEscape(newState.created) << "\",\r\n";
    oss << "  \"ruleGroup\": \"" << kRuleGroupPrimary << "\",\r\n";
    oss << "  \"exeCount\": " << newState.exeCount << ",\r\n";
    oss << "  \"hostsBackup\": \"" << JsonEscape(WideToUtf8(GetHostsBackupPath().wstring())) << "\",\r\n";
    oss << "  \"services\": [\r\n";
    for (size_t i = 0; i < newState.services.size(); ++i) {
        std::string name = newState.services[i].first;
        std::string mode = newState.services[i].second;
        if (origSvcModes.count(name)) mode = origSvcModes[name];
        oss << "    { \"Name\": \"" << JsonEscape(name) << "\", \"StartMode\": \"" << JsonEscape(mode) << "\" }"
            << (i + 1 < newState.services.size() ? "," : "") << "\r\n";
    }
    oss << "  ],\r\n";
    oss << "  \"tasks\": [\r\n";
    for (size_t i = 0; i < newState.tasks.size(); ++i) {
        const auto& t = newState.tasks[i];
        oss << "    { \"TaskName\": \"" << JsonEscape(std::get<1>(t))
            << "\", \"TaskPath\": \"" << JsonEscape(std::get<0>(t))
            << "\", \"State\": \"" << JsonEscape(std::get<2>(t)) << "\" }"
            << (i + 1 < newState.tasks.size() ? "," : "") << "\r\n";
    }
    oss << "  ]\r\n}\r\n";
    WriteFileUtf8(GetStateFilePath(), oss.str());
}

// ============================================================================
// Auto-Update Engine (WinHTTP + JSON Parser + SemVer + BCrypt SHA-256 + Swap)
// ============================================================================

static std::string g_updateFeedOverride;
static bool g_devUiMode = false;
static std::string g_justUpdatedFrom;

static std::string CurrentTimeIso() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf;
    localtime_s(&tmBuf, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return buf;
}

static fs::path GetCurrentExePath() {
    wchar_t buf[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return fs::path(buf);
}

static fs::path GetLocalFallbackDir() {
    wchar_t buf[MAX_PATH * 2] = {0};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2) > 0) {
        return fs::path(buf) / L"AdobeTools";
    }
    if (GetTempPathW(MAX_PATH * 2, buf) > 0) {
        return fs::path(buf) / L"AdobeTools";
    }
    return GetAppDir();
}

static fs::path GetUpdatesDir() {
    std::error_code ec;
    fs::path primary = GetStateDir() / L"Updates";
    fs::create_directories(primary, ec);
    if (!ec) {
        fs::path probe = primary / L".write_test";
        if (WriteFileUtf8(probe, "ok")) {
            fs::remove(probe, ec);
            return primary;
        }
    }
    fs::path fb = GetLocalFallbackDir() / L"Updates";
    fs::create_directories(fb, ec);
    return fb;
}

static fs::path GetUpdateConfigPathForRead() {
    fs::path primary = GetStateDir() / L"update-config.json";
    if (fs::exists(primary)) return primary;
    fs::path fb = GetLocalFallbackDir() / L"update-config.json";
    if (fs::exists(fb)) return fb;
    return primary;
}

static void AppendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolVal = false;
    int64_t intVal = 0;
    double numVal = 0.0;
    std::string strVal;
    std::vector<JsonValue> arrVal;
    std::map<std::string, JsonValue> objVal;

    bool IsNull() const { return type == Type::Null; }
    bool IsBool() const { return type == Type::Bool; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }

    const JsonValue& Get(const std::string& key) const {
        static const JsonValue kNull;
        if (type != Type::Object) return kNull;
        auto it = objVal.find(key);
        return it != objVal.end() ? it->second : kNull;
    }

    std::string AsString(const std::string& def = "") const {
        return type == Type::String ? strVal : def;
    }

    int64_t AsInt64(int64_t def = 0) const {
        return type == Type::Number ? intVal : def;
    }

    bool AsBool(bool def = false) const {
        return type == Type::Bool ? boolVal : def;
    }
};

class JsonParser {
    const std::string& s;
    size_t pos = 0;
    int depth = 0;

    void SkipWs() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) pos++;
    }

    bool ParseHex4(uint32_t& out) {
        if (pos + 4 > s.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s[pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') out |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool ParseString(std::string& out) {
        if (pos >= s.size() || s[pos] != '\"') return false;
        pos++;
        out.clear();
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '\"') return true;
            if (c == '\\') {
                if (pos >= s.size()) return false;
                char esc = s[pos++];
                switch (esc) {
                    case '\"': out += '\"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!ParseHex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
                            size_t saved = pos;
                            pos += 2;
                            uint32_t low = 0;
                            if (ParseHex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                            } else {
                                pos = saved;
                            }
                        }
                        AppendUtf8Codepoint(out, cp);
                        break;
                    }
                    default: out += esc; break;
                }
            } else {
                out += c;
            }
        }
        return false;
    }

    bool ParseNumber(JsonValue& out) {
        size_t start = pos;
        if (pos < s.size() && s[pos] == '-') pos++;
        if (pos >= s.size() || !std::isdigit((unsigned char)s[pos])) return false;
        while (pos < s.size() && std::isdigit((unsigned char)s[pos])) pos++;
        bool isFloat = false;
        if (pos < s.size() && s[pos] == '.') {
            isFloat = true;
            pos++;
            while (pos < s.size() && std::isdigit((unsigned char)s[pos])) pos++;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            isFloat = true;
            pos++;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
            while (pos < s.size() && std::isdigit((unsigned char)s[pos])) pos++;
        }
        std::string token = s.substr(start, pos - start);
        out.type = JsonValue::Type::Number;
        try {
            out.numVal = std::stod(token);
            out.intVal = isFloat ? static_cast<int64_t>(out.numVal) : std::stoll(token);
        } catch (...) {
            return false;
        }
        return true;
    }

    bool ParseArray(JsonValue& out) {
        if (pos >= s.size() || s[pos] != '[') return false;
        pos++;
        out.type = JsonValue::Type::Array;
        SkipWs();
        if (pos < s.size() && s[pos] == ']') { pos++; return true; }
        while (pos < s.size()) {
            JsonValue elem;
            if (!ParseValue(elem)) return false;
            out.arrVal.push_back(std::move(elem));
            SkipWs();
            if (pos < s.size() && s[pos] == ',') { pos++; continue; }
            if (pos < s.size() && s[pos] == ']') { pos++; return true; }
            return false;
        }
        return false;
    }

    bool ParseObject(JsonValue& out) {
        if (pos >= s.size() || s[pos] != '{') return false;
        pos++;
        out.type = JsonValue::Type::Object;
        SkipWs();
        if (pos < s.size() && s[pos] == '}') { pos++; return true; }
        while (pos < s.size()) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (pos >= s.size() || s[pos] != ':') return false;
            pos++;
            JsonValue val;
            if (!ParseValue(val)) return false;
            out.objVal[std::move(key)] = std::move(val);
            SkipWs();
            if (pos < s.size() && s[pos] == ',') { pos++; continue; }
            if (pos < s.size() && s[pos] == '}') { pos++; return true; }
            return false;
        }
        return false;
    }

public:
    explicit JsonParser(const std::string& input) : s(input) {}

    bool ParseValue(JsonValue& out) {
        if (++depth > 64) return false;
        SkipWs();
        bool ok = false;
        if (pos < s.size()) {
            char c = s[pos];
            if (c == '{') ok = ParseObject(out);
            else if (c == '[') ok = ParseArray(out);
            else if (c == '\"') {
                out.type = JsonValue::Type::String;
                ok = ParseString(out.strVal);
            } else if (c == '-' || std::isdigit((unsigned char)c)) {
                ok = ParseNumber(out);
            } else if (s.compare(pos, 4, "true") == 0) {
                pos += 4; out.type = JsonValue::Type::Bool; out.boolVal = true; ok = true;
            } else if (s.compare(pos, 5, "false") == 0) {
                pos += 5; out.type = JsonValue::Type::Bool; out.boolVal = false; ok = true;
            } else if (s.compare(pos, 4, "null") == 0) {
                pos += 4; out.type = JsonValue::Type::Null; ok = true;
            }
        }
        depth--;
        return ok;
    }
};

static bool ParseJson(const std::string& input, JsonValue& out) {
    JsonParser parser(input);
    return parser.ParseValue(out);
}

static std::string NormalizeVersionTag(const std::string& raw) {
    std::string s = Trim(raw);
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    return Trim(s);
}

struct ParsedSemVer {
    std::vector<long long> nums;
    std::string pre;
};

static ParsedSemVer ParseSemVer(const std::string& raw) {
    ParsedSemVer v;
    std::string s = NormalizeVersionTag(raw);
    size_t plus = s.find('+');
    if (plus != std::string::npos) s = s.substr(0, plus);
    size_t dash = s.find('-');
    if (dash != std::string::npos) {
        v.pre = ToLowerAscii(Trim(s.substr(dash + 1)));
        s = s.substr(0, dash);
    }
    std::istringstream iss(s);
    std::string part;
    while (std::getline(iss, part, '.')) {
        std::string digits;
        for (char c : part) {
            if (std::isdigit((unsigned char)c)) digits += c;
            else break;
        }
        long long n = 0;
        if (!digits.empty()) {
            try { n = std::stoll(digits); } catch (...) { n = 0; }
        }
        v.nums.push_back(n);
    }
    while (v.nums.size() < 4) v.nums.push_back(0);
    return v;
}

// Returns -1 if a < b, 0 if a == b, +1 if a > b
static int CompareSemVer(const std::string& aStr, const std::string& bStr) {
    ParsedSemVer a = ParseSemVer(aStr);
    ParsedSemVer b = ParseSemVer(bStr);
    size_t n = std::max(a.nums.size(), b.nums.size());
    for (size_t i = 0; i < n; ++i) {
        long long av = i < a.nums.size() ? a.nums[i] : 0;
        long long bv = i < b.nums.size() ? b.nums[i] : 0;
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    if (a.pre.empty() && !b.pre.empty()) return 1;
    if (!a.pre.empty() && b.pre.empty()) return -1;
    if (a.pre < b.pre) return -1;
    if (a.pre > b.pre) return 1;
    return 0;
}

struct UpdateConfig {
    bool autoCheck = true;
    std::string ignoredVersion;
    std::string lastChecked;
};

static UpdateConfig LoadUpdateConfig() {
    UpdateConfig cfg;
    fs::path p = GetUpdateConfigPathForRead();
    if (!fs::exists(p)) return cfg;
    JsonValue root;
    if (!ParseJson(ReadFileUtf8(p), root) || !root.IsObject()) return cfg;
    cfg.autoCheck = root.Get("autoCheck").AsBool(true);
    cfg.ignoredVersion = root.Get("ignoredVersion").AsString("");
    cfg.lastChecked = root.Get("lastChecked").AsString("");
    return cfg;
}

static void SaveUpdateConfig(const UpdateConfig& cfg) {
    std::ostringstream oss;
    oss << "{\r\n"
        << "  \"autoCheck\": " << (cfg.autoCheck ? "true" : "false") << ",\r\n"
        << "  \"ignoredVersion\": \"" << JsonEscape(cfg.ignoredVersion) << "\",\r\n"
        << "  \"lastChecked\": \"" << JsonEscape(cfg.lastChecked) << "\"\r\n"
        << "}\r\n";
    std::string content = oss.str();
    if (!WriteFileUtf8(GetStateDir() / L"update-config.json", content)) {
        WriteFileUtf8(GetLocalFallbackDir() / L"update-config.json", content);
    }
}

struct ReleaseAsset {
    std::string name;
    int64_t size = 0;
    std::string downloadUrl;
};

struct UpdateReleaseInfo {
    std::string tagName;
    std::string version;
    std::string releaseName;
    std::string releaseUrl;
    std::string publishedAt;
    std::string notes;
    std::string assetName;
    int64_t assetSize = 0;
    std::string assetUrl;
    std::string checksumAssetUrl;
};

struct UpdateStateSnapshot {
    std::string state = "idle"; // idle|checking|available|up_to_date|no_release|downloading|verifying|installing|error
    bool silent = false;
    std::string currentVersion = kAppVersion;
    std::string latestVersion;
    std::string releaseName;
    std::string releaseUrl = kDefaultReleasesPageUrl;
    std::string publishedAt;
    std::string notes;
    std::string assetName;
    int64_t assetSize = 0;
    bool hasDirectAsset = false;
    bool hasChecksum = false;
    int progress = 0;
    int64_t downloadedBytes = 0;
    int64_t totalBytes = 0;
    int64_t speedBps = 0;
    std::string message;
    UpdateConfig config;
};

static std::string NormalizeAssetKey(const std::string& name) {
    std::string base = name;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    base = ToLowerAscii(Trim(base));
    std::string out;
    for (char c : base) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
    }
    return out;
}

static bool EndsWithNoCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return ToLowerAscii(s.substr(s.size() - suffix.size())) == ToLowerAscii(suffix);
}

static int SelectWindowsAssetIndex(const std::vector<ReleaseAsset>& assets) {
    int bestIdx = -1;
    int bestScore = -1;
    for (size_t i = 0; i < assets.size(); ++i) {
        const auto& a = assets[i];
        if (!EndsWithNoCase(a.name, ".exe")) continue;
        std::string low = ToLowerAscii(a.name);
        if (low.find("macos") != std::string::npos || low.find("darwin") != std::string::npos) continue;
        int score = 10;
        if (low.find("adobe") != std::string::npos && low.find("tools") != std::string::npos) score += 50;
        if (low == "adobe tools.exe" || low == "adobe.tools.exe") score += 40;
        if (low.find("x64") != std::string::npos || low.find("win") != std::string::npos) score += 15;
        if (score > bestScore) {
            bestScore = score;
            bestIdx = (int)i;
        }
    }
    return bestIdx;
}

static int FindChecksumAssetIndex(const std::vector<ReleaseAsset>& assets) {
    for (size_t i = 0; i < assets.size(); ++i) {
        std::string low = ToLowerAscii(assets[i].name);
        if (low == "sha256sums.txt" || low == "sha256sums" || low == "checksums.txt" ||
            (low.find("sha256") != std::string::npos && EndsWithNoCase(low, ".txt"))) {
            return (int)i;
        }
    }
    return -1;
}

static bool IsHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static std::string ParseExpectedSha256(const std::string& checksumText, const std::string& assetName) {
    std::string targetLower = ToLowerAscii(Trim(assetName));
    std::string targetNorm = NormalizeAssetKey(assetName);
    std::istringstream iss(checksumText);
    std::string line;
    std::string fallbackNormMatch;
    while (std::getline(iss, line)) {
        std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t sp = t.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        std::string hash = t.substr(0, sp);
        if (!IsHex64(hash)) continue;
        std::string fname = Trim(t.substr(sp + 1));
        if (!fname.empty() && fname[0] == '*') fname.erase(0, 1);
        fname = Trim(fname);
        if (ToLowerAscii(fname) == targetLower) {
            return ToLowerAscii(hash);
        }
        if (fallbackNormMatch.empty() && !targetNorm.empty() && NormalizeAssetKey(fname) == targetNorm) {
            fallbackNormMatch = ToLowerAscii(hash);
        }
    }
    return fallbackNormMatch;
}

static bool ParseGitHubReleaseJson(const std::string& jsonStr, UpdateReleaseInfo& outInfo, std::string& errOut) {
    JsonValue root;
    if (!ParseJson(jsonStr, root) || !root.IsObject()) {
        errOut = "无法解析更新响应 JSON 数据";
        return false;
    }
    std::string tag = root.Get("tag_name").AsString();
    if (tag.empty()) {
        std::string msg = root.Get("message").AsString();
        errOut = msg.empty() ? "更新响应中缺少版本号字段" : ("GitHub 返回: " + msg);
        return false;
    }

    outInfo = UpdateReleaseInfo{};
    outInfo.tagName = tag;
    outInfo.version = NormalizeVersionTag(tag);
    outInfo.releaseName = root.Get("name").AsString();
    if (outInfo.releaseName.empty()) outInfo.releaseName = "Adobe Tools " + tag;
    outInfo.releaseUrl = root.Get("html_url").AsString(kDefaultReleasesPageUrl);
    std::string pub = root.Get("published_at").AsString();
    outInfo.publishedAt = pub.size() >= 10 ? pub.substr(0, 10) : pub;
    outInfo.notes = root.Get("body").AsString();

    std::vector<ReleaseAsset> assets;
    const JsonValue& arr = root.Get("assets");
    if (arr.IsArray()) {
        for (const auto& item : arr.arrVal) {
            if (!item.IsObject()) continue;
            ReleaseAsset a;
            a.name = item.Get("name").AsString();
            a.size = item.Get("size").AsInt64(0);
            a.downloadUrl = item.Get("browser_download_url").AsString();
            if (!a.name.empty() && !a.downloadUrl.empty()) {
                assets.push_back(std::move(a));
            }
        }
    }

    int winIdx = SelectWindowsAssetIndex(assets);
    if (winIdx >= 0) {
        outInfo.assetName = assets[winIdx].name;
        outInfo.assetSize = assets[winIdx].size;
        outInfo.assetUrl = assets[winIdx].downloadUrl;
    }
    int sumIdx = FindChecksumAssetIndex(assets);
    if (sumIdx >= 0) {
        outInfo.checksumAssetUrl = assets[sumIdx].downloadUrl;
    }
    return true;
}

static bool ComputeBufferSha256Hex(const uint8_t* data, size_t len, std::string& outHex) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    DWORD cbHashObject = 0, cbData = 0, cbHash = 0;

    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return false;
    bool ok = false;
    if (NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PBYTE)&cbHashObject, sizeof(DWORD), &cbData, 0)) &&
        NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PBYTE)&cbHash, sizeof(DWORD), &cbData, 0))) {
        std::vector<BYTE> hashObj(cbHashObject);
        std::vector<BYTE> hashBuf(cbHash);
        if (NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, hashObj.data(), cbHashObject, nullptr, 0, 0))) {
            if (NT_SUCCESS(BCryptHashData(hHash, (PBYTE)data, (ULONG)len, 0)) &&
                NT_SUCCESS(BCryptFinishHash(hHash, hashBuf.data(), cbHash, 0))) {
                static const char* kHex = "0123456789abcdef";
                outHex.clear();
                outHex.reserve(cbHash * 2);
                for (DWORD i = 0; i < cbHash; ++i) {
                    outHex += kHex[(hashBuf[i] >> 4) & 0xF];
                    outHex += kHex[hashBuf[i] & 0xF];
                }
                ok = true;
            }
            BCryptDestroyHash(hHash);
        }
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

static bool ComputeFileSha256Hex(const fs::path& filePath, std::string& outHex) {
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs) return false;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    DWORD cbHashObject = 0, cbData = 0, cbHash = 0;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return false;

    bool ok = false;
    if (NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PBYTE)&cbHashObject, sizeof(DWORD), &cbData, 0)) &&
        NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PBYTE)&cbHash, sizeof(DWORD), &cbData, 0))) {
        std::vector<BYTE> hashObj(cbHashObject);
        std::vector<BYTE> hashBuf(cbHash);
        if (NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, hashObj.data(), cbHashObject, nullptr, 0, 0))) {
            std::vector<char> chunk(65536);
            bool streamOk = true;
            while (ifs) {
                ifs.read(chunk.data(), (std::streamsize)chunk.size());
                std::streamsize n = ifs.gcount();
                if (n > 0) {
                    if (!NT_SUCCESS(BCryptHashData(hHash, (PBYTE)chunk.data(), (ULONG)n, 0))) {
                        streamOk = false;
                        break;
                    }
                }
            }
            if (streamOk && NT_SUCCESS(BCryptFinishHash(hHash, hashBuf.data(), cbHash, 0))) {
                static const char* kHex = "0123456789abcdef";
                outHex.clear();
                outHex.reserve(cbHash * 2);
                for (DWORD i = 0; i < cbHash; ++i) {
                    outHex += kHex[(hashBuf[i] >> 4) & 0xF];
                    outHex += kHex[hashBuf[i] & 0xF];
                }
                ok = true;
            }
            BCryptDestroyHash(hHash);
        }
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

static bool VerifyWindowsPeFile(const fs::path& filePath, std::string& errOut) {
    std::error_code ec;
    auto sz = fs::file_size(filePath, ec);
    if (ec || sz < 1024) {
        errOut = "下载的安装文件体积异常，可能未下载完整";
        return false;
    }
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs) {
        errOut = "无法读取已下载的更新文件";
        return false;
    }
    IMAGE_DOS_HEADER dos = {};
    ifs.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!ifs || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        errOut = "下载的文件不是有效的 Windows 可执行程序（缺少 MZ 头）";
        return false;
    }
    if (dos.e_lfanew < (LONG)sizeof(IMAGE_DOS_HEADER) || (uint64_t)dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > sz) {
        errOut = "下载的文件 PE 头偏移异常";
        return false;
    }
    ifs.seekg(dos.e_lfanew, std::ios::beg);
    DWORD peSig = 0;
    IMAGE_FILE_HEADER fileHdr = {};
    ifs.read(reinterpret_cast<char*>(&peSig), sizeof(peSig));
    ifs.read(reinterpret_cast<char*>(&fileHdr), sizeof(fileHdr));
    if (!ifs || peSig != IMAGE_NT_SIGNATURE) {
        errOut = "下载的文件 PE 签名无效";
        return false;
    }
    if (fileHdr.Machine != IMAGE_FILE_MACHINE_AMD64 &&
        fileHdr.Machine != IMAGE_FILE_MACHINE_I386 &&
        fileHdr.Machine != IMAGE_FILE_MACHINE_ARM64) {
        errOut = "下载的可执行文件架构不受支持";
        return false;
    }
    return true;
}

struct ParsedUrl {
    bool isHttps = true;
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path;
};

static bool CrackUrlUtf8(const std::string& urlUtf8, ParsedUrl& out) {
    std::wstring urlW = Utf8ToWide(urlUtf8);
    if (urlW.empty()) return false;
    URL_COMPONENTSW comp = {};
    comp.dwStructSize = sizeof(comp);
    comp.dwHostNameLength = (DWORD)-1;
    comp.dwUrlPathLength = (DWORD)-1;
    comp.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(urlW.c_str(), (DWORD)urlW.size(), 0, &comp)) return false;
    if (comp.nScheme != INTERNET_SCHEME_HTTPS && comp.nScheme != INTERNET_SCHEME_HTTP) return false;
    out.isHttps = (comp.nScheme == INTERNET_SCHEME_HTTPS);
    out.host.assign(comp.lpszHostName, comp.dwHostNameLength);
    out.port = comp.nPort;
    out.path.assign(comp.lpszUrlPath, comp.dwUrlPathLength);
    if (comp.dwExtraInfoLength > 0 && comp.lpszExtraInfo) {
        out.path.append(comp.lpszExtraInfo, comp.dwExtraInfoLength);
    }
    if (out.path.empty()) out.path = L"/";
    return !out.host.empty();
}

static bool IsAllowedUpdateUrl(const std::string& url) {
    ParsedUrl u;
    if (!CrackUrlUtf8(url, u)) return false;
    std::string host = ToLowerAscii(WideToUtf8(u.host));
    bool isLoopback = (host == "127.0.0.1" || host == "localhost");
    if (!g_updateFeedOverride.empty()) {
        if (isLoopback) return true;
        ParsedUrl ov;
        if (CrackUrlUtf8(g_updateFeedOverride, ov) && u.isHttps && ToLowerAscii(WideToUtf8(ov.host)) == host) {
            return true;
        }
    }
    if (!u.isHttps) return false;
    if (host == "api.github.com" || host == "github.com" ||
        EndsWithNoCase(host, ".github.com") || EndsWithNoCase(host, ".githubusercontent.com")) {
        return true;
    }
    return false;
}

class WinHttpHandle {
    HINTERNET h = nullptr;
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    operator HINTERNET() const { return h; }
    explicit operator bool() const { return h != nullptr; }
};

static HINTERNET OpenUpdateSession() {
    std::wstring ua = L"Adobe-Tools/" + Utf8ToWide(kAppVersion);
    HINTERNET hSess = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) {
        hSess = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (hSess) {
        WinHttpSetTimeouts(hSess, 8000, 10000, 15000, 30000);
        DWORD tlsFlags = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        DWORD tls13 = tlsFlags | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(hSess, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls13, sizeof(tls13))) {
            WinHttpSetOption(hSess, WINHTTP_OPTION_SECURE_PROTOCOLS, &tlsFlags, sizeof(tlsFlags));
        }
#else
        WinHttpSetOption(hSess, WINHTTP_OPTION_SECURE_PROTOCOLS, &tlsFlags, sizeof(tlsFlags));
#endif
    }
    return hSess;
}

static bool WinHttpGetString(const std::string& url, const std::wstring& extraHeaders,
                             DWORD& outStatusCode, std::string& outBody, std::string& errOut) {
    outStatusCode = 0;
    outBody.clear();
    if (!IsAllowedUpdateUrl(url)) {
        errOut = "更新地址不受信任，已拒绝请求";
        return false;
    }
    ParsedUrl u;
    if (!CrackUrlUtf8(url, u)) {
        errOut = "更新地址格式无效";
        return false;
    }
    WinHttpHandle hSess(OpenUpdateSession());
    if (!hSess) {
        errOut = "无法初始化 WinHTTP 网络会话";
        return false;
    }
    WinHttpHandle hConn(WinHttpConnect(hSess, u.host.c_str(), u.port, 0));
    if (!hConn) {
        errOut = "无法连接到更新服务器";
        return false;
    }
    DWORD reqFlags = u.isHttps ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle hReq(WinHttpOpenRequest(hConn, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags));
    if (!hReq) {
        errOut = "创建网络请求失败";
        return false;
    }
    if (!extraHeaders.empty()) {
        WinHttpAddRequestHeaders(hReq, extraHeaders.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr)) {
        DWORD le = GetLastError();
        errOut = "连接更新服务器失败（错误码 " + std::to_string(le) + "，请检查网络或代理设置）";
        return false;
    }

    DWORD statusSize = sizeof(outStatusCode);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &outStatusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    char buf[16384];
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        DWORD toRead = std::min<DWORD>(avail, sizeof(buf));
        DWORD readBytes = 0;
        if (!WinHttpReadData(hReq, buf, toRead, &readBytes) || readBytes == 0) break;
        outBody.append(buf, readBytes);
        if (outBody.size() > 4 * 1024 * 1024) break;
    }
    return true;
}

static bool WinHttpDownloadToFile(
    const std::string& url,
    const fs::path& destPath,
    int64_t expectedSizeHint,
    std::atomic<bool>& cancelFlag,
    std::function<void(int64_t downloaded, int64_t total, int64_t speedBps)> onProgress,
    std::string& errOut)
{
    if (!IsAllowedUpdateUrl(url)) {
        errOut = "下载地址不受信任，已拒绝下载";
        return false;
    }
    ParsedUrl u;
    if (!CrackUrlUtf8(url, u)) {
        errOut = "下载地址格式无效";
        return false;
    }
    WinHttpHandle hSess(OpenUpdateSession());
    if (!hSess) {
        errOut = "无法初始化下载会话";
        return false;
    }
    WinHttpHandle hConn(WinHttpConnect(hSess, u.host.c_str(), u.port, 0));
    if (!hConn) {
        errOut = "无法连接下载服务器";
        return false;
    }
    DWORD reqFlags = u.isHttps ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle hReq(WinHttpOpenRequest(hConn, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags));
    if (!hReq) {
        errOut = "创建下载请求失败";
        return false;
    }
    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr)) {
        DWORD le = GetLastError();
        errOut = "下载请求失败（错误码 " + std::to_string(le) + "）";
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (statusCode < 200 || statusCode >= 300) {
        errOut = "下载服务器返回 HTTP " + std::to_string(statusCode);
        return false;
    }

    int64_t totalBytes = expectedSizeHint;
    wchar_t clBuf[64] = {0};
    DWORD clSize = sizeof(clBuf);
    if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                            clBuf, &clSize, WINHTTP_NO_HEADER_INDEX)) {
        try {
            int64_t parsed = std::stoll(WideToUtf8(clBuf));
            if (parsed > 0) totalBytes = parsed;
        } catch (...) {}
    }

    std::error_code ec;
    if (destPath.has_parent_path()) fs::create_directories(destPath.parent_path(), ec);
    std::ofstream ofs(destPath, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        errOut = "无法创建临时下载文件";
        return false;
    }

    std::vector<char> buf(32768);
    int64_t downloaded = 0;
    auto tStart = std::chrono::steady_clock::now();
    auto tLastReport = tStart;

    while (true) {
        if (cancelFlag.load()) {
            ofs.close();
            fs::remove(destPath, ec);
            errOut = "已取消下载";
            return false;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) {
            ofs.close();
            fs::remove(destPath, ec);
            errOut = "下载连接中断";
            return false;
        }
        if (avail == 0) break;
        DWORD toRead = std::min<DWORD>(avail, (DWORD)buf.size());
        DWORD readBytes = 0;
        if (!WinHttpReadData(hReq, buf.data(), toRead, &readBytes)) {
            ofs.close();
            fs::remove(destPath, ec);
            errOut = "读取下载数据失败";
            return false;
        }
        if (readBytes == 0) break;
        ofs.write(buf.data(), (std::streamsize)readBytes);
        if (!ofs.good()) {
            ofs.close();
            fs::remove(destPath, ec);
            errOut = "写入临时更新文件失败（请检查磁盘空间）";
            return false;
        }
        downloaded += readBytes;

        auto now = std::chrono::steady_clock::now();
        auto msSince = std::chrono::duration_cast<std::chrono::milliseconds>(now - tLastReport).count();
        if (onProgress && msSince >= 90) {
            double elapsedSec = std::max(0.05, std::chrono::duration<double>(now - tStart).count());
            int64_t speed = static_cast<int64_t>(downloaded / elapsedSec);
            onProgress(downloaded, totalBytes, speed);
            tLastReport = now;
        }
    }

    ofs.flush();
    ofs.close();

    if (totalBytes > 0 && downloaded != totalBytes) {
        fs::remove(destPath, ec);
        errOut = "下载文件大小不完整（已下载 " + std::to_string(downloaded) + " / 预期 " + std::to_string(totalBytes) + " 字节）";
        return false;
    }
    if (onProgress) {
        auto now = std::chrono::steady_clock::now();
        double elapsedSec = std::max(0.05, std::chrono::duration<double>(now - tStart).count());
        onProgress(downloaded, totalBytes > 0 ? totalBytes : downloaded, static_cast<int64_t>(downloaded / elapsedSec));
    }
    return true;
}

static bool CheckForUpdatesSync(bool silent, UpdateStateSnapshot& snap, UpdateReleaseInfo& outRel) {
    UpdateConfig cfg = LoadUpdateConfig();
    snap = UpdateStateSnapshot{};
    snap.silent = silent;
    snap.currentVersion = kAppVersion;
    snap.config = cfg;
    snap.releaseUrl = kDefaultReleasesPageUrl;

    std::string apiUrl = g_updateFeedOverride.empty() ? std::string(kDefaultUpdateApiUrl) : g_updateFeedOverride;
    DWORD statusCode = 0;
    std::string body, err;
    std::wstring hdrs = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";

    if (!WinHttpGetString(apiUrl, hdrs, statusCode, body, err)) {
        snap.state = "error";
        snap.message = err;
        return false;
    }

    cfg.lastChecked = CurrentTimeIso();
    SaveUpdateConfig(cfg);
    snap.config = cfg;

    if (statusCode == 404) {
        snap.state = "no_release";
        snap.latestVersion = kAppVersion;
        snap.message = std::string("当前版本 v") + kAppVersion;
        return true;
    }
    if (statusCode == 403 || statusCode == 429) {
        snap.state = "error";
        snap.message = "GitHub API 请求频率受限，请稍后再试或直接前往 Releases 页面查看";
        return false;
    }
    if (statusCode < 200 || statusCode >= 300) {
        snap.state = "error";
        snap.message = "检查更新失败（HTTP " + std::to_string(statusCode) + "）";
        return false;
    }

    UpdateReleaseInfo rel;
    if (!ParseGitHubReleaseJson(body, rel, err)) {
        snap.state = "error";
        snap.message = err;
        return false;
    }

    outRel = rel;
    snap.latestVersion = rel.version;
    snap.releaseName = rel.releaseName;
    snap.releaseUrl = rel.releaseUrl.empty() ? kDefaultReleasesPageUrl : rel.releaseUrl;
    snap.publishedAt = rel.publishedAt;
    snap.notes = rel.notes;
    snap.assetName = rel.assetName;
    snap.assetSize = rel.assetSize;
    snap.hasDirectAsset = !rel.assetUrl.empty();
    snap.hasChecksum = !rel.checksumAssetUrl.empty();

    if (CompareSemVer(kAppVersion, rel.version) < 0) {
        snap.state = "available";
        snap.message = "发现新版本 v" + rel.version + "（当前 v" + kAppVersion + "）";
    } else {
        snap.state = "up_to_date";
        snap.message = std::string("当前已是最新版本 v") + kAppVersion;
    }
    return true;
}

static bool LaunchProcessCommand(const std::wstring& exePath, const std::wstring& args) {
    std::wstring cmdLine = L"\"" + exePath + L"\" " + args;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exePath.c_str(), &cmdLine[0], nullptr, nullptr, FALSE, 0, nullptr,
                       fs::path(exePath).parent_path().wstring().c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
    return (INT_PTR)ShellExecuteW(nullptr, L"open", exePath.c_str(), args.c_str(),
                                  fs::path(exePath).parent_path().wstring().c_str(), SW_SHOWNORMAL) > 32;
}

static bool ApplyDownloadedUpdateWindows(const fs::path& stagingExe, std::string& errOut) {
    if (!VerifyWindowsPeFile(stagingExe, errOut)) return false;

    fs::path currentExe = GetCurrentExePath();
    fs::path backupExe = fs::path(currentExe.wstring() + L".old");
    DWORD myPid = GetCurrentProcessId();

    DeleteFileW(backupExe.wstring().c_str());

    if (MoveFileExW(currentExe.wstring().c_str(), backupExe.wstring().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (!CopyFileW(stagingExe.wstring().c_str(), currentExe.wstring().c_str(), FALSE)) {
            DWORD le = GetLastError();
            MoveFileExW(backupExe.wstring().c_str(), currentExe.wstring().c_str(), MOVEFILE_REPLACE_EXISTING);
            errOut = "写入新版本主程序失败（错误码 " + std::to_string(le) + "），已自动回滚原程序";
            return false;
        }
        std::error_code ec;
        fs::remove(stagingExe, ec);

        std::wstring args = L"--post-update " + std::to_wstring(myPid) +
                            L" \"" + backupExe.wstring() + L"\" --updated-from " + Utf8ToWide(kAppVersion);
        if (!LaunchProcessCommand(currentExe.wstring(), args)) {
            errOut = "新版本已替换完成，但自动拉起失败，请手动重新打开 Adobe Tools.exe";
            return false;
        }
        return true;
    }

    // Fallback: PowerShell UTF-8 BOM helper script if running binary could not be renamed in place
    fs::path psScript = GetUpdatesDir() / L"apply_update.ps1";
    auto escapePsSingle = [](const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '\'') r += "''";
            else r += c;
        }
        return r;
    };
    std::ostringstream ps;
    ps << "\xEF\xBB\xBF"; // UTF-8 BOM for Windows PowerShell 5.1 Unicode path support
    ps << "$ErrorActionPreference = 'SilentlyContinue'\r\n";
    ps << "$pidToWait = " << myPid << "\r\n";
    ps << "$src = '" << escapePsSingle(WideToUtf8(stagingExe.wstring())) << "'\r\n";
    ps << "$dst = '" << escapePsSingle(WideToUtf8(currentExe.wstring())) << "'\r\n";
    ps << "for ($i = 0; $i -lt 40; $i++) { if (-not (Get-Process -Id $pidToWait -ErrorAction SilentlyContinue)) { break }; Start-Sleep -Milliseconds 150 }\r\n";
    ps << "$copied = $false\r\n";
    ps << "for ($i = 0; $i -lt 25; $i++) {\r\n";
    ps << "  try { Copy-Item -LiteralPath $src -Destination $dst -Force -ErrorAction Stop; $copied = $true; break } catch { Start-Sleep -Milliseconds 200 }\r\n";
    ps << "}\r\n";
    ps << "if ($copied) {\r\n";
    ps << "  Remove-Item -LiteralPath $src -Force -ErrorAction SilentlyContinue\r\n";
    ps << "  Start-Process -FilePath $dst -ArgumentList @('--updated-from', '" << kAppVersion << "')\r\n";
    ps << "}\r\n";
    ps << "Remove-Item -LiteralPath $MyInvocation.MyCommand.Path -Force -ErrorAction SilentlyContinue\r\n";

    if (!WriteFileUtf8(psScript, ps.str())) {
        errOut = "无法替换主程序，且创建辅助更新脚本失败";
        return false;
    }
    std::wstring cmd = L"powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \"" + psScript.wstring() + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        errOut = "无法启动后台替换进程，请前往 GitHub Releases 手动下载更新";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void HandlePostUpdateCleanup(DWORD oldPid, const std::wstring& oldBackupPath) {
    if (oldPid != 0) {
        HANDLE hOld = OpenProcess(SYNCHRONIZE, FALSE, oldPid);
        if (hOld) {
            WaitForSingleObject(hOld, 6000);
            CloseHandle(hOld);
        }
    }
    if (!oldBackupPath.empty()) {
        for (int i = 0; i < 20; ++i) {
            if (DeleteFileW(oldBackupPath.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) break;
            Sleep(150);
        }
    }
}

static void CleanupStaleOldBinary() {
    fs::path stale = fs::path(GetCurrentExePath().wstring() + L".old");
    if (fs::exists(stale)) {
        DeleteFileW(stale.wstring().c_str());
    }
}

// ============================================================================
// Mihomo (Clash.Meta) Anti-Leak Rule-Set Generator & Exporter
// ============================================================================

static const std::vector<std::string> kMihomoDomainSuffixes = {
    "adobegenuine.com",
    "adobelogin.com",
    "licenses.adobe.com",
    "wip3.adobe.com",
    "wip4.adobe.com"
};

static const std::vector<std::string> kMihomoDomainKeywords = {
    "adobegenuine",
    "adobe-dns",
    "3dns",
    "lmlicenses"
};

static const std::vector<std::string> kMihomoExtraDomains = {
    "lcs-cops.adobe.io",
    "lcs-ulecs.adobe.io",
    "p13n.adobe.io",
    "notify.adobe.io",
    "geo2.adobe.com",
    "hl2rcv.adobe.com",
    "oobe.adobe.com",
    "ims-prod06.adobelogin.com",
    "ims-prod07.adobelogin.com"
};

static const std::vector<std::string> kMihomoDefaultProcessesWin = {
    "AGSService.exe",
    "AGMService.exe",
    "AdobeGCClient.exe",
    "Adobe Genuine Helper.exe",
    "Adobe Desktop Service.exe",
    "Creative Cloud.exe",
    "Creative Cloud Helper.exe",
    "AdobeIPCBroker.exe",
    "CCXProcess.exe",
    "CoreSync.exe",
    "CCLibrary.exe",
    "AdobeUpdateService.exe",
    "AdobeARM.exe",
    "armsvc.exe",
    "AcroCEF.exe",
    "RdrCEF.exe",
    "Photoshop.exe",
    "Illustrator.exe",
    "AfterFX.exe",
    "Adobe Premiere Pro.exe",
    "Acrobat.exe",
    "InDesign.exe",
    "Lightroom.exe",
    "LightroomClassic.exe",
    "Audition.exe",
    "Adobe Media Encoder.exe",
    "Animate.exe",
    "Bridge.exe"
};

static const std::vector<std::string> kMihomoDefaultProcessesMac = {
    "AdobeGCClient",
    "AGSService",
    "AGMService",
    "Adobe Desktop Service",
    "Creative Cloud",
    "AdobeIPCBroker",
    "CCXProcess",
    "CoreSync",
    "AdobeCRDaemon"
};

static std::vector<std::string> MergeMihomoDomains(const std::vector<std::string>& domains, bool includeExtra) {
    std::set<std::string> uniq;
    for (const auto& d : domains) {
        std::string t = ToLowerAscii(Trim(d));
        if (!t.empty()) uniq.insert(t);
    }
    if (includeExtra) {
        for (const auto& d : kMihomoExtraDomains) {
            uniq.insert(ToLowerAscii(Trim(d)));
        }
    }
    for (const auto& s : kMihomoDomainSuffixes) {
        uniq.insert(ToLowerAscii(Trim(s)));
    }
    return std::vector<std::string>(uniq.begin(), uniq.end());
}

static bool IsSafeMihomoProcessName(const std::string& fname) {
    std::string low = ToLowerAscii(Trim(fname));
    if (low.empty()) return false;
    static const char* kBlockedGeneric[] = {
        "node.exe", "msedgewebview2.exe", "setup.exe", "set-up.exe",
        "uninstall.exe", "uninstaller.exe", "convert.exe", "pngquant.exe",
        "notification_helper.exe", "sniffer.exe", "droplet template.exe",
        "python.exe", "ffmpeg.exe", "7z.exe", "curl.exe"
    };
    for (const char* b : kBlockedGeneric) {
        if (low == b) return false;
    }
    if (low.rfind("vcredist", 0) == 0) return false;

    static const char* kAdobeTokens[] = {
        "adobe", "acro", "rdrcef", "armsvc", "agsservice", "agmservice",
        "photoshop", "illustrator", "afterfx", "premiere", "indesign",
        "lightroom", "audition", "animate", "bridge", "dreamweaver",
        "creative", "coresync", "ccx", "cclibrary", "cephtml",
        "dynamiclink", "flitetranscoder", "importerred", "tokenresolver",
        "hdcore", "hdhelper", "adscustom", "crlog", "crwindows",
        "amecommand", "logtransport", "logcollector"
    };
    for (const char* tok : kAdobeTokens) {
        if (low.find(tok) != std::string::npos) return true;
    }
    return false;
}

static std::vector<std::string> MergeMihomoProcessNames(const std::vector<std::string>& exePaths, bool includeMac) {
    std::vector<std::string> out;
    std::set<std::string> seenLower;
    auto pushUnique = [&](const std::string& name) {
        std::string t = Trim(name);
        if (t.empty()) return;
        std::string low = ToLowerAscii(t);
        if (seenLower.insert(low).second) {
            out.push_back(t);
        }
    };
    for (const auto& p : kMihomoDefaultProcessesWin) pushUnique(p);
    for (const auto& ep : exePaths) {
        std::string fname = WideToUtf8(fs::path(Utf8ToWide(ep)).filename().wstring());
        if (IsSafeMihomoProcessName(fname)) pushUnique(fname);
    }
    if (includeMac) {
        for (const auto& p : kMihomoDefaultProcessesMac) pushUnique(p);
    }
    return out;
}

static std::string BuildMihomoClassicalYaml(const std::vector<std::string>& domains, const std::vector<std::string>& exePaths) {
    auto mergedDomains = MergeMihomoDomains(domains, true);
    auto winProcs = MergeMihomoProcessNames(exePaths, false);

    std::ostringstream oss;
    oss << "# Adobe Tools - Mihomo / Clash.Meta 防侧漏规则集 (behavior: classical)\r\n"
        << "# 仓库地址: https://github.com/Rita6333/Adobe-Tools\r\n"
        << "# 订阅链接: https://raw.githubusercontent.com/Rita6333/Adobe-Tools/main/rules/mihomo/adobe-block.yaml\r\n"
        << "# 建议动作: REJECT 或 REJECT-DROP（配合 dns.use-system-hosts: true 防止 TUN / Fake-IP / 系统代理侧漏）\r\n\r\n"
        << "payload:\r\n"
        << "  # ==================== 通配符域名后缀 ====================\r\n";
    for (const auto& s : kMihomoDomainSuffixes) {
        oss << "  - 'DOMAIN-SUFFIX," << s << "'\r\n";
    }
    oss << "\r\n  # ==================== 高危关键词匹配 ====================\r\n";
    for (const auto& kw : kMihomoDomainKeywords) {
        oss << "  - 'DOMAIN-KEYWORD," << kw << "'\r\n";
    }
    oss << "\r\n  # ==================== 激活、正版校验与遥测域名 ====================\r\n";
    for (const auto& d : mergedDomains) {
        oss << "  - 'DOMAIN," << d << "'\r\n";
    }
    oss << "\r\n  # ==================== 进程级防侧漏 (Windows) ====================\r\n";
    for (const auto& proc : winProcs) {
        oss << "  - 'PROCESS-NAME," << proc << "'\r\n";
    }
    oss << "\r\n  # ==================== 进程级防侧漏 (macOS) ====================\r\n";
    for (const auto& proc : kMihomoDefaultProcessesMac) {
        oss << "  - 'PROCESS-NAME," << proc << "'\r\n";
    }
    return oss.str();
}

static std::string BuildMihomoDomainYaml(const std::vector<std::string>& domains) {
    auto mergedDomains = MergeMihomoDomains(domains, true);
    std::ostringstream oss;
    oss << "# Adobe Tools - Mihomo / Clash.Meta 域名规则集 (behavior: domain)\r\n"
        << "# 可用于 rule-providers (behavior: domain) 或 dns.fake-ip-filter\r\n"
        << "# 订阅链接: https://raw.githubusercontent.com/Rita6333/Adobe-Tools/main/rules/mihomo/adobe-domain.yaml\r\n\r\n"
        << "payload:\r\n";
    for (const auto& d : mergedDomains) {
        oss << "  - '+." << d << "'\r\n";
    }
    return oss.str();
}

static std::string BuildMihomoClassicalList(const std::vector<std::string>& domains, const std::vector<std::string>& exePaths) {
    auto mergedDomains = MergeMihomoDomains(domains, true);
    auto winProcs = MergeMihomoProcessNames(exePaths, false);

    std::ostringstream oss;
    oss << "# Adobe Tools - 通用防侧漏规则列表 (behavior: classical, format: text)\r\n"
        << "# 适用: Mihomo (format: text) / Clash / Surge / Quantumult X\r\n"
        << "# 订阅链接: https://raw.githubusercontent.com/Rita6333/Adobe-Tools/main/rules/mihomo/adobe-block.list\r\n\r\n"
        << "# 通配符域名后缀\r\n";
    for (const auto& s : kMihomoDomainSuffixes) {
        oss << "DOMAIN-SUFFIX," << s << "\r\n";
    }
    oss << "\r\n# 高危关键词匹配\r\n";
    for (const auto& kw : kMihomoDomainKeywords) {
        oss << "DOMAIN-KEYWORD," << kw << "\r\n";
    }
    oss << "\r\n# 激活、正版校验与遥测域名\r\n";
    for (const auto& d : mergedDomains) {
        oss << "DOMAIN," << d << "\r\n";
    }
    oss << "\r\n# 进程级防侧漏 (Windows)\r\n";
    for (const auto& proc : winProcs) {
        oss << "PROCESS-NAME," << proc << "\r\n";
    }
    oss << "\r\n# 进程级防侧漏 (macOS)\r\n";
    for (const auto& proc : kMihomoDefaultProcessesMac) {
        oss << "PROCESS-NAME," << proc << "\r\n";
    }
    return oss.str();
}

static bool ExportMihomoRulesToDisk(const std::vector<std::string>& extraPaths, fs::path& outDir, std::string& errOut) {
    auto domains = GetAdobeDomains();
    auto exes = FindAdobeExecutables(extraPaths);
    std::string yamlClassical = BuildMihomoClassicalYaml(domains, exes);
    std::string yamlDomain = BuildMihomoDomainYaml(domains);
    std::string listClassical = BuildMihomoClassicalList(domains, exes);

    std::vector<fs::path> candidates = {
        GetAppDir() / L"rules\\mihomo",
        GetStateDir() / L"mihomo",
        GetLocalFallbackDir() / L"mihomo"
    };

    for (const auto& dir : candidates) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) continue;
        if (WriteFileUtf8(dir / L"adobe-block.yaml", yamlClassical) &&
            WriteFileUtf8(dir / L"adobe-domain.yaml", yamlDomain) &&
            WriteFileUtf8(dir / L"adobe-block.list", listClassical)) {
            outDir = dir;
            return true;
        }
    }

    errOut = "无法创建或写入 Mihomo 规则导出目录";
    return false;
}

// ============================================================================
// 19-Point Built-in Self-Test (13 Hosts + 5 Auto-Update + 1 Mihomo Rule-Set)
// ============================================================================

static int RunSelfTestEngine(std::function<void(const std::string&, const std::string&, bool)> logFn) {
    int fails = 0;
    auto check = [&](bool ok, const std::string& title) {
        if (ok) {
            if (logFn) logFn("PASS", title, false);
        } else {
            fails++;
            if (logFn) logFn("FAIL", title, false);
        }
    };

    std::string orig = "# Managed by Windows\r\n127.0.0.1 localhost\r\n::1 localhost\r\n";
    bool mod1 = false;
    std::string s1 = StripHostsBlock(orig, &mod1);
    check(!mod1, "原始 hosts 无块，剥离为空操作");
    check(s1 == orig, "原文完整保留");

    std::string applied1 = ApplyHostsToString(orig, kBuiltInDomains);
    check(HostsHasBlock(applied1), "块已写入");
    check(kBuiltInDomains.size() == 41, "域名条数 = 41");

    bool mod2 = false;
    StripHostsBlock(applied1, &mod2);
    check(mod2, "第二次剥离能识别旧块");

    std::string applied2 = ApplyHostsToString(applied1, kBuiltInDomains);
    size_t firstPos = applied2.find(kHostsBeginPrimary);
    size_t secondPos = (firstPos != std::string::npos) ? applied2.find(kHostsBeginPrimary, firstPos + 1) : std::string::npos;
    check(firstPos != std::string::npos && secondPos == std::string::npos, "第二次写入后块数量仍为 1");
    check(applied1 == applied2, "幂等：两次结果完全一致");
    check(applied2.find("127.0.0.1 localhost") != std::string::npos, "原文没有丢失");

    bool mod3 = false;
    std::string restored = StripHostsBlock(applied2, &mod3);
    check(mod3 && !HostsHasBlock(restored), "剥离成功");
    check(Trim(restored) == Trim(orig), "还原后与原文一致");

    std::string glued = std::string("127.0.0.1 localhost") + kHostsBeginLegacy + "\r\n0.0.0.0 adobe.com\r\n" + kHostsEndLegacy;
    bool mod4 = false;
    std::string unglued = StripHostsBlock(glued, &mod4);
    check(mod4 && Trim(unglued) == "127.0.0.1 localhost", "行尾无换行 + 块粘连也能剥离");

    std::string withCustom = orig + "0.0.0.0 mycustom.local\r\n\r\n" + BuildHostsBlock({"activate.adobe.com"});
    bool mod5 = false;
    std::string afterCustom = StripHostsBlock(withCustom, &mod5);
    check(afterCustom.find("0.0.0.0 mycustom.local") != std::string::npos, "非块内的自定义行保留");

    std::string broken = orig + kHostsBeginPrimary + "\r\n0.0.0.0 activate.adobe.com\r\n";
    bool mod6 = false;
    std::string fixedBroken = StripHostsBlock(broken, &mod6);
    check(mod6 && !HostsHasBlock(fixedBroken), "残缺块也能清掉");

    // Auto-Update engine self-tests (5 checks)
    bool semverOk = (CompareSemVer("2.1.0", "2.2.0") < 0) &&
                    (CompareSemVer("v2.2.0", "v2.10.0") < 0) &&
                    (CompareSemVer("v2.1.0", "2.1.0.0") == 0) &&
                    (CompareSemVer("2.2.0-rc.1", "2.2.0") < 0) &&
                    (CompareSemVer("3.0.0", "2.9.9") > 0);
    check(semverOk, "SemVer 语义化版本号比较逻辑准确");

    std::string mockReleaseJson =
        "{\"tag_name\":\"v2.2.0\",\"name\":\"Adobe Tools v2.2.0\",\"published_at\":\"2026-11-28T10:00:00Z\","
        "\"html_url\":\"https://github.com/Rita6333/Adobe-Tools/releases/tag/v2.2.0\","
        "\"body\":\"\\u65b0\\u589e\\u81ea\\u52a8\\u66f4\\u65b0\",\"assets\":["
        "{\"name\":\"Adobe-Tools-macOS-Universal.dmg\",\"size\":1200000,\"browser_download_url\":\"https://github.com/Rita6333/Adobe-Tools/releases/download/v2.2.0/Adobe-Tools-macOS-Universal.dmg\"},"
        "{\"name\":\"Adobe.Tools.exe\",\"size\":680000,\"browser_download_url\":\"https://github.com/Rita6333/Adobe-Tools/releases/download/v2.2.0/Adobe.Tools.exe\"},"
        "{\"name\":\"SHA256SUMS.txt\",\"size\":128,\"browser_download_url\":\"https://github.com/Rita6333/Adobe-Tools/releases/download/v2.2.0/SHA256SUMS.txt\"}]}";
    UpdateReleaseInfo parsedRel;
    std::string parseErr;
    bool jsonOk = ParseGitHubReleaseJson(mockReleaseJson, parsedRel, parseErr) &&
                  parsedRel.version == "2.2.0" &&
                  parsedRel.notes == "新增自动更新" &&
                  parsedRel.publishedAt == "2026-11-28";
    check(jsonOk, "GitHub Release JSON 解析与 Unicode 解码正常");

    bool assetOk = (parsedRel.assetName == "Adobe.Tools.exe") &&
                   (parsedRel.assetSize == 680000) &&
                   (!parsedRel.checksumAssetUrl.empty());
    check(assetOk, "Windows 可执行资产与校验清单智能匹配正常");

    std::string sampleSums =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  Adobe-Tools-macOS-Universal.dmg\r\n"
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  Adobe Tools.exe\r\n";
    std::string extractedHash = ParseExpectedSha256(sampleSums, "Adobe.Tools.exe");
    check(extractedHash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA256SUMS 校验值解析（兼容空格/点号映射）");

    std::string abcHash;
    bool hashOk = ComputeBufferSha256Hex(reinterpret_cast<const uint8_t*>("abc"), 3, abcHash) &&
                  abcHash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    check(hashOk, "Windows CNG BCrypt SHA-256 哈希计算准确");

    std::string mihomoYaml = BuildMihomoClassicalYaml({"activate.adobe.com"}, {
        "C:\\Program Files\\Adobe\\Photoshop.exe",
        "C:\\Program Files\\Adobe\\node.exe",
        "C:\\Program Files\\Adobe\\msedgewebview2.exe"
    });
    bool mihomoOk = (mihomoYaml.find("DOMAIN-SUFFIX,adobegenuine.com") != std::string::npos) &&
                    (mihomoYaml.find("DOMAIN,activate.adobe.com") != std::string::npos) &&
                    (mihomoYaml.find("PROCESS-NAME,Photoshop.exe") != std::string::npos) &&
                    (mihomoYaml.find("PROCESS-NAME,node.exe") == std::string::npos) &&
                    (mihomoYaml.find("PROCESS-NAME,msedgewebview2.exe") == std::string::npos);
    check(mihomoOk, "Mihomo 防侧漏规则集生成与通用进程名过滤正常");

    if (logFn) {
        logFn(fails == 0 ? "DONE" : "FAIL",
              fails == 0 ? "SELFTEST_OK — 19 项内置逻辑自检全部通过！" : "SELFTEST_FAIL",
              fails == 0);
    }
    return fails;
}

// ============================================================================
// Apply & Revert Workflows
// ============================================================================

static void ExecuteApplyWorkflow(
    const std::vector<std::string>& extraPaths,
    bool skipHosts,
    bool skipServices,
    bool includeInbound,
    std::function<void(const std::string&, const std::string&, bool)> logFn,
    std::function<void(int, const std::string&)> progFn)
{
    if (progFn) progFn(5, "1/4 正在扫描本地 Adobe 可执行文件...");
    if (logFn) logFn("STEP", "1/4 扫描本地 Adobe 可执行文件", false);

    auto exes = FindAdobeExecutables(extraPaths);
    if (exes.empty()) {
        if (logFn) logFn("WARN", "未扫描到任何 Adobe 可执行文件（可点击「+ 添加目录」指定安装位置）", false);
    } else {
        if (logFn) logFn("OK", "扫描完成，共找到 " + std::to_string(exes.size()) + " 个 Adobe 可执行程序", false);
    }

    if (progFn) progFn(15, "2/4 正在建立 Windows 防火墙拦截规则...");
    if (logFn) logFn("STEP", "2/4 建立 Windows 防火墙出站拦截规则", false);

    int oldCleared = ClearFirewallRules();
    if (oldCleared > 0 && logFn) {
        logFn("INFO", "已清理旧拦截规则 " + std::to_string(oldCleared) + " 条", false);
    }

    int ruleCreated = 0;
    for (size_t i = 0; i < exes.size(); ++i) {
        int pct = 15 + (int)(((i + 1) * 45) / std::max<size_t>(1, exes.size()));
        if (progFn && (i % 5 == 0 || i + 1 == exes.size())) {
            std::string fname = WideToUtf8(fs::path(Utf8ToWide(exes[i])).filename().wstring());
            progFn(pct, "2/4 正在写入防火墙规则 (" + std::to_string(i + 1) + "/" + std::to_string(exes.size()) + "): " + fname);
        }
        if (AddFirewallBlockRule(exes[i], false)) ruleCreated++;
        if (includeInbound && AddFirewallBlockRule(exes[i], true)) ruleCreated++;
    }
    if (logFn) {
        logFn("OK", "防火墙拦截规则已建立：" + std::to_string(ruleCreated) + " 条" + (includeInbound ? "（出站 + 入站）" : "（出站拦截）"), false);
    }

    if (progFn) progFn(70, "3/4 正在写入系统 hosts 屏蔽域名...");
    if (logFn) logFn("STEP", "3/4 修改系统 hosts 屏蔽校验与激活域名", false);

    if (skipHosts) {
        if (logFn) logFn("INFO", "已按策略配置跳过 hosts 修改", false);
    } else {
        auto domains = GetAdobeDomains();
        std::string err;
        if (ApplySystemHosts(domains, &err)) {
            if (logFn) logFn("OK", "hosts 已写入 " + std::to_string(domains.size()) + " 个 Adobe 屏蔽域名并刷新 DNS 缓存", false);
        } else {
            if (logFn) logFn("WARN", "hosts 写入失败：" + err, false);
        }
    }

    if (progFn) progFn(85, "4/4 正在停用后台正版校验服务与计划任务...");
    if (logFn) logFn("STEP", "4/4 停用后台正版校验服务与计划任务", false);

    SavedState st;
    st.created = CurrentTimeIso();
    st.exeCount = (int)exes.size();

    if (skipServices) {
        if (logFn) logFn("INFO", "已按策略配置跳过服务与计划任务处理", false);
    } else {
        auto svcs = FindAdobeServices();
        if (svcs.empty()) {
            if (logFn) logFn("INFO", "当前系统未发现 Adobe 后台服务", false);
        } else {
            for (const auto& s : svcs) {
                st.services.push_back({s.name, s.startMode});
                std::wstring wName = Utf8ToWide(s.name);
                RunHiddenCommand(L"sc.exe stop \"" + wName + L"\"");
                int rc = RunHiddenCommand(L"sc.exe config \"" + wName + L"\" start= disabled");
                if (rc == 0 && logFn) {
                    logFn("OK", "已停用服务: " + s.name + " (" + s.display + ")", false);
                }
            }
        }

        auto tasks = FindAdobeTasks();
        if (tasks.empty()) {
            if (logFn) logFn("INFO", "当前系统未发现 Adobe 计划任务", false);
        } else {
            for (const auto& t : tasks) {
                st.tasks.push_back({t.display, t.name, t.state});
                std::string full = t.display;
                if (full.empty() || full.back() != '\\') full += "\\";
                full += t.name;
                int rc = RunHiddenCommand(L"schtasks.exe /Change /TN \"" + Utf8ToWide(full) + L"\" /DISABLE");
                if (rc == 0 && logFn) {
                    logFn("OK", "已禁用计划任务: " + full, false);
                }
            }
        }
    }

    SaveAppState(st);
    if (progFn) progFn(100, "三层断网拦截已全面生效！");
    if (logFn) {
        logFn("DONE", "搞定！拦截程序 " + std::to_string(exes.size()) + " 个，防火墙规则 " +
              std::to_string(CountFirewallRules()) + " 条。已运行的 Adobe 软件请重启以生效。", true);
    }
}

static void ExecuteRevertWorkflow(
    std::function<void(const std::string&, const std::string&, bool)> logFn,
    std::function<void(int, const std::string&)> progFn)
{
    if (progFn) progFn(20, "1/4 正在删除防火墙拦截规则...");
    if (logFn) logFn("STEP", "1/4 删除 Windows 防火墙拦截规则", false);

    int removed = ClearFirewallRules();
    int left = CountFirewallRules();
    if (logFn) logFn("OK", "已删除规则 " + std::to_string(removed) + " 条，剩余 " + std::to_string(left) + " 条", false);

    if (progFn) progFn(50, "2/4 正在还原系统 hosts 文件...");
    if (logFn) logFn("STEP", "2/4 还原系统 hosts 文件", false);

    if (RevertSystemHosts()) {
        if (logFn) logFn("OK", "已从 hosts 中完整剥离 Adobe 屏蔽段并刷新 DNS", false);
    } else {
        if (logFn) logFn("INFO", "hosts 中未包含屏蔽块，无需改动", false);
    }

    if (progFn) progFn(75, "3/4 正在按记录恢复后台服务与计划任务...");
    if (logFn) logFn("STEP", "3/4 恢复后台服务与计划任务", false);

    SavedState st = LoadSavedState();
    for (const auto& kv : st.services) {
        std::string mode = ToLowerAscii(kv.second);
        std::wstring scMode = L"demand";
        if (mode == "auto" || mode == "automatic") scMode = L"auto";
        else if (mode == "disabled") scMode = L"disabled";
        RunHiddenCommand(L"sc.exe config \"" + Utf8ToWide(kv.first) + L"\" start= " + scMode);
        if (logFn) logFn("OK", "已恢复服务启动类型: " + kv.first + " -> " + kv.second, false);
    }

    for (const auto& t : st.tasks) {
        std::string path = std::get<0>(t);
        std::string name = std::get<1>(t);
        std::string state = std::get<2>(t);
        if (ToLowerAscii(state) == "disabled") continue;
        std::string full = path;
        if (full.empty() || full.back() != '\\') full += "\\";
        full += name;
        RunHiddenCommand(L"schtasks.exe /Change /TN \"" + Utf8ToWide(full) + L"\" /ENABLE");
        if (logFn) logFn("OK", "已恢复计划任务: " + full, false);
    }

    if (progFn) progFn(95, "4/4 正在清理状态文件...");
    std::error_code ec;
    if (fs::exists(GetStateFilePath(), ec)) {
        fs::remove(GetStateFilePath(), ec);
    }

    if (progFn) progFn(100, "已全部还原至拦截前状态！");
    if (logFn) logFn("DONE", "已全部还原完毕。", true);
}

// ============================================================================
// WebView2 Window & Bidirectional JSON Bridge
// ============================================================================

static HWND g_hWnd = nullptr;
static ICoreWebView2Controller* g_controller = nullptr;
static ICoreWebView2* g_webview = nullptr;

static std::mutex g_queueMutex;
static std::vector<std::wstring> g_msgQueue;

static std::mutex g_stateMutex;
static std::vector<std::string> g_extraPaths;
static std::atomic<bool> g_workerBusy{false};

static std::mutex g_updateMutex;
static UpdateReleaseInfo g_cachedRelease;
static UpdateStateSnapshot g_lastUpdateSnap;
static std::atomic<bool> g_updateBusy{false};
static std::atomic<bool> g_updateCancel{false};

static std::wstring g_previewBasePng;
static int g_previewStep = 0;

static constexpr UINT WM_POST_WEB_MSG  = WM_APP + 10;
static constexpr UINT WM_PREVIEW_READY = WM_APP + 11;
static constexpr UINT_PTR TIMER_PREVIEW = 901;

static void PostJsonToUI(const std::string& jsonUtf8) {
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_msgQueue.push_back(Utf8ToWide(jsonUtf8));
    }
    if (g_hWnd) {
        PostMessageW(g_hWnd, WM_POST_WEB_MSG, 0, 0);
    }
}

static void SendLogToUI(const std::string& tag, const std::string& text, bool highlight = false) {
    std::ostringstream oss;
    oss << "{\"type\":\"log\",\"tag\":\"" << JsonEscape(tag)
        << "\",\"text\":\"" << JsonEscape(text)
        << "\",\"highlight\":" << (highlight ? "true" : "false") << "}";
    PostJsonToUI(oss.str());
}

static void SendProgressToUI(int percent, const std::string& text) {
    std::ostringstream oss;
    oss << "{\"type\":\"progress\",\"percent\":" << percent
        << ",\"text\":\"" << JsonEscape(text) << "\"}";
    PostJsonToUI(oss.str());
}

static std::string SerializeUpdateStateJson(const UpdateStateSnapshot& s) {
    std::ostringstream oss;
    oss << "{\"type\":\"updateState\",\"data\":{"
        << "\"state\":\"" << JsonEscape(s.state) << "\","
        << "\"silent\":" << (s.silent ? "true" : "false") << ","
        << "\"currentVersion\":\"" << JsonEscape(s.currentVersion) << "\","
        << "\"latestVersion\":\"" << JsonEscape(s.latestVersion) << "\","
        << "\"releaseName\":\"" << JsonEscape(s.releaseName) << "\","
        << "\"releaseUrl\":\"" << JsonEscape(s.releaseUrl) << "\","
        << "\"publishedAt\":\"" << JsonEscape(s.publishedAt) << "\","
        << "\"notes\":\"" << JsonEscape(s.notes) << "\","
        << "\"assetName\":\"" << JsonEscape(s.assetName) << "\","
        << "\"assetSize\":" << s.assetSize << ","
        << "\"hasDirectAsset\":" << (s.hasDirectAsset ? "true" : "false") << ","
        << "\"hasChecksum\":" << (s.hasChecksum ? "true" : "false") << ","
        << "\"progress\":" << s.progress << ","
        << "\"downloadedBytes\":" << s.downloadedBytes << ","
        << "\"totalBytes\":" << s.totalBytes << ","
        << "\"speedBps\":" << s.speedBps << ","
        << "\"message\":\"" << JsonEscape(s.message) << "\","
        << "\"autoCheck\":" << (s.config.autoCheck ? "true" : "false") << ","
        << "\"ignoredVersion\":\"" << JsonEscape(s.config.ignoredVersion) << "\","
        << "\"lastChecked\":\"" << JsonEscape(s.config.lastChecked) << "\""
        << "}}";
    return oss.str();
}

static void PostUpdateStateToUI(const UpdateStateSnapshot& s) {
    {
        std::lock_guard<std::mutex> lock(g_updateMutex);
        g_lastUpdateSnap = s;
    }
    PostJsonToUI(SerializeUpdateStateJson(s));
}

static void SpawnCheckUpdateAsync(bool silent) {
    if (g_updateBusy.exchange(true)) return;
    std::thread([silent]() {
        UpdateStateSnapshot checkingSnap;
        checkingSnap.state = "checking";
        checkingSnap.silent = silent;
        checkingSnap.currentVersion = kAppVersion;
        checkingSnap.config = LoadUpdateConfig();
        checkingSnap.message = "正在检查更新...";
        PostUpdateStateToUI(checkingSnap);

        UpdateStateSnapshot result;
        UpdateReleaseInfo rel;
        CheckForUpdatesSync(silent, result, rel);
        {
            std::lock_guard<std::mutex> lock(g_updateMutex);
            if (!rel.tagName.empty()) g_cachedRelease = rel;
        }
        PostUpdateStateToUI(result);

        if (!silent || result.state == "available") {
            std::string tag = (result.state == "error") ? "WARN" : (result.state == "available" ? "OK" : "INFO");
            SendLogToUI(tag, "自动更新检查：" + result.message, false);
        }
        g_updateBusy = false;
    }).detach();
}

static void SpawnDownloadUpdateAsync() {
    if (g_updateBusy.exchange(true)) return;
    g_updateCancel = false;

    UpdateReleaseInfo rel;
    UpdateStateSnapshot baseSnap;
    {
        std::lock_guard<std::mutex> lock(g_updateMutex);
        rel = g_cachedRelease;
        baseSnap = g_lastUpdateSnap;
    }

    if (rel.assetUrl.empty()) {
        baseSnap.state = "error";
        baseSnap.silent = false;
        baseSnap.message = "当前 Release 未提供 Windows 可执行文件，请前往 Releases 页面手动下载";
        PostUpdateStateToUI(baseSnap);
        g_updateBusy = false;
        return;
    }

    std::thread([rel, baseSnap]() mutable {
        baseSnap.silent = false;
        baseSnap.state = "downloading";
        baseSnap.progress = 0;
        baseSnap.downloadedBytes = 0;
        baseSnap.totalBytes = rel.assetSize;
        baseSnap.speedBps = 0;
        baseSnap.message = "正在下载 " + rel.assetName + "...";
        PostUpdateStateToUI(baseSnap);
        SendLogToUI("STEP", "开始下载新版本 v" + rel.version + " (" + rel.assetName + ")", false);

        fs::path stagingPath = GetUpdatesDir() / L"AdobeTools_update.exe";
        std::string err;
        bool dlOk = WinHttpDownloadToFile(
            rel.assetUrl,
            stagingPath,
            rel.assetSize,
            g_updateCancel,
            [&](int64_t downloaded, int64_t total, int64_t speedBps) {
                baseSnap.state = "downloading";
                baseSnap.downloadedBytes = downloaded;
                baseSnap.totalBytes = total;
                baseSnap.speedBps = speedBps;
                baseSnap.progress = (total > 0) ? (int)std::min<int64_t>(99, (downloaded * 100) / total) : 50;
                baseSnap.message = "正在下载 " + rel.assetName + "...";
                PostUpdateStateToUI(baseSnap);
            },
            err);

        if (!dlOk) {
            if (g_updateCancel.load()) {
                baseSnap.state = "available";
                baseSnap.progress = 0;
                baseSnap.downloadedBytes = 0;
                baseSnap.speedBps = 0;
                baseSnap.message = "已取消下载更新";
                SendLogToUI("INFO", "已取消更新下载", false);
            } else {
                baseSnap.state = "error";
                baseSnap.message = err;
                SendLogToUI("WARN", "下载更新失败：" + err, false);
            }
            PostUpdateStateToUI(baseSnap);
            g_updateBusy = false;
            return;
        }

        // Verify PE & optional SHA-256
        baseSnap.state = "verifying";
        baseSnap.progress = 100;
        baseSnap.message = "正在校验文件完整性与数字结构...";
        PostUpdateStateToUI(baseSnap);

        if (!VerifyWindowsPeFile(stagingPath, err)) {
            std::error_code ec;
            fs::remove(stagingPath, ec);
            baseSnap.state = "error";
            baseSnap.message = err;
            PostUpdateStateToUI(baseSnap);
            SendLogToUI("WARN", "更新校验失败：" + err, false);
            g_updateBusy = false;
            return;
        }

        if (!rel.checksumAssetUrl.empty()) {
            DWORD sumStatus = 0;
            std::string sumBody, sumErr;
            if (WinHttpGetString(rel.checksumAssetUrl, L"", sumStatus, sumBody, sumErr) && sumStatus >= 200 && sumStatus < 300) {
                std::string expected = ParseExpectedSha256(sumBody, rel.assetName);
                if (!expected.empty()) {
                    std::string actual;
                    if (!ComputeFileSha256Hex(stagingPath, actual) || actual != expected) {
                        std::error_code ec;
                        fs::remove(stagingPath, ec);
                        baseSnap.state = "error";
                        baseSnap.message = "SHA-256 完整性校验失败，已中止更新以保护系统安全";
                        PostUpdateStateToUI(baseSnap);
                        SendLogToUI("WARN", baseSnap.message, false);
                        g_updateBusy = false;
                        return;
                    }
                    SendLogToUI("OK", "SHA-256 校验通过：" + actual.substr(0, 16) + "...", false);
                }
            }
        }

        baseSnap.state = "installing";
        baseSnap.message = "正在替换程序并重启至 v" + rel.version + "...";
        PostUpdateStateToUI(baseSnap);
        SendLogToUI("STEP", "校验通过，正在替换可执行文件并重启...", false);

        if (!ApplyDownloadedUpdateWindows(stagingPath, err)) {
            baseSnap.state = "error";
            baseSnap.message = err;
            PostUpdateStateToUI(baseSnap);
            SendLogToUI("WARN", "应用更新失败：" + err, false);
            g_updateBusy = false;
            return;
        }

        SendLogToUI("DONE", "更新就绪，正在重启至 v" + rel.version, true);
        Sleep(320);
        if (g_hWnd) {
            PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
        }
        g_updateBusy = false;
    }).detach();
}

static std::string BuildStatusJsonPayload() {
    std::vector<std::string> extraCopy;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        extraCopy = g_extraPaths;
    }

    bool isAdmin = IsRunningAsAdmin();
    bool fwSvc = IsFirewallServiceRunning();
    int fwRules = CountFirewallRules();
    bool hostsOn = HostsHasBlock(ReadFileUtf8(GetSystemHostsPath()));
    bool hostsBackup = fs::exists(GetHostsBackupPath());
    auto domains = GetAdobeDomains();
    auto exes = FindAdobeExecutables(extraCopy);
    auto svcs = FindAdobeServices();
    auto tasks = FindAdobeTasks();

    int svcTotal = (int)svcs.size();
    int svcDisabled = 0;
    for (const auto& s : svcs) {
        if (ToLowerAscii(s.startMode) == "disabled") svcDisabled++;
    }

    int taskTotal = (int)tasks.size();
    int taskDisabled = 0;
    for (const auto& t : tasks) {
        if (ToLowerAscii(t.startMode) == "disabled" || ToLowerAscii(t.state) == "disabled") taskDisabled++;
    }

    SavedState saved = LoadSavedState();
    UpdateConfig updCfg = LoadUpdateConfig();
    std::string customDomainsText = ReadFileUtf8(GetCustomDomainsPath());
    if (customDomainsText.empty()) {
        customDomainsText = "# 额外要屏蔽的域名（可选，一行一个，# 开头是注释）\r\n";
    }

    std::ostringstream oss;
    oss << "{\"type\":\"status\",\"data\":{";
    oss << "\"appVersion\":\"" << JsonEscape(kAppVersion) << "\",";
    oss << "\"platform\":\"windows\",";
    oss << "\"justUpdatedFrom\":\"" << JsonEscape(g_justUpdatedFrom) << "\",";
    oss << "\"updateConfig\":{"
        << "\"autoCheck\":" << (updCfg.autoCheck ? "true" : "false") << ","
        << "\"ignoredVersion\":\"" << JsonEscape(updCfg.ignoredVersion) << "\","
        << "\"lastChecked\":\"" << JsonEscape(updCfg.lastChecked) << "\"},";
    oss << "\"isAdmin\":" << (isAdmin ? "true" : "false") << ",";
    oss << "\"fwServiceRunning\":" << (fwSvc ? "true" : "false") << ",";
    oss << "\"fwRuleCount\":" << fwRules << ",";
    oss << "\"hostsEnabled\":" << (hostsOn ? "true" : "false") << ",";
    oss << "\"hostsBackup\":" << (hostsBackup ? "true" : "false") << ",";
    oss << "\"domainCount\":" << domains.size() << ",";
    oss << "\"svcTotal\":" << svcTotal << ",";
    oss << "\"svcDisabled\":" << svcDisabled << ",";
    oss << "\"taskTotal\":" << taskTotal << ",";
    oss << "\"taskDisabled\":" << taskDisabled << ",";
    oss << "\"lastApplied\":\"" << JsonEscape(saved.created.empty() ? "尚无记录" : saved.created) << "\",";
    oss << "\"customDomainsText\":\"" << JsonEscape(customDomainsText) << "\",";

    oss << "\"extraPaths\":[";
    for (size_t i = 0; i < extraCopy.size(); ++i) {
        oss << "\"" << JsonEscape(extraCopy[i]) << "\"" << (i + 1 < extraCopy.size() ? "," : "");
    }
    oss << "],";

    oss << "\"exes\":[";
    for (size_t i = 0; i < exes.size(); ++i) {
        std::string fname = WideToUtf8(fs::path(Utf8ToWide(exes[i])).filename().wstring());
        oss << "{\"name\":\"" << JsonEscape(fname) << "\",\"path\":\"" << JsonEscape(exes[i]) << "\"}"
            << (i + 1 < exes.size() ? "," : "");
    }
    oss << "],";

    std::vector<ServiceOrTaskInfo> combined = svcs;
    combined.insert(combined.end(), tasks.begin(), tasks.end());
    oss << "\"servicesAndTasks\":[";
    for (size_t i = 0; i < combined.size(); ++i) {
        const auto& item = combined[i];
        oss << "{\"kind\":\"" << JsonEscape(item.kind)
            << "\",\"name\":\"" << JsonEscape(item.name)
            << "\",\"display\":\"" << JsonEscape(item.display)
            << "\",\"state\":\"" << JsonEscape(item.state)
            << "\",\"startMode\":\"" << JsonEscape(item.startMode) << "\"}"
            << (i + 1 < combined.size() ? "," : "");
    }
    oss << "]}}";
    return oss.str();
}

static void SpawnRefreshAsync(bool logSummary, bool isPreviewInit = false) {
    std::thread([logSummary, isPreviewInit]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SendProgressToUI(0, "扫描完成");
        std::string payload = BuildStatusJsonPayload();
        PostJsonToUI(payload);

        if (isPreviewInit) {
            if (g_hWnd) PostMessageW(g_hWnd, WM_PREVIEW_READY, 0, 0);
        }
        CoUninitialize();
    }).detach();
}

static std::string LoadHtmlContent() {
    // 1. Prefer embedded RCDATA resource 101 so auto-updated single-file binaries always load matching UI
    if (g_devUiMode) {
        fs::path diskHtml = GetAppDir() / L"src\\ui.html";
        if (fs::exists(diskHtml)) {
            std::string s = ReadFileUtf8(diskHtml);
            if (!s.empty()) return s;
        }
    }
    HMODULE hMod = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(101), RT_RCDATA);
    if (hRes) {
        DWORD sz = SizeofResource(hMod, hRes);
        HGLOBAL hData = LoadResource(hMod, hRes);
        if (hData && sz > 0) {
            const char* ptr = static_cast<const char*>(LockResource(hData));
            if (ptr) return std::string(ptr, sz);
        }
    }
    fs::path diskHtml = GetAppDir() / L"src\\ui.html";
    if (fs::exists(diskHtml)) {
        std::string s = ReadFileUtf8(diskHtml);
        if (!s.empty()) return s;
    }
    return "<html><body style='background:#090b10;color:white;'>Failed to load UI resource</body></html>";
}

static void HandleFolderPicker() {
    IFileOpenDialog* pDlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg))) && pDlg) {
        DWORD opts = 0;
        pDlg->GetOptions(&opts);
        pDlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        pDlg->SetTitle(L"选择 Adobe 软件自定义安装目录（将自动递归扫描其中的 .exe 程序）");
        if (SUCCEEDED(pDlg->Show(g_hWnd))) {
            IShellItem* pItem = nullptr;
            if (SUCCEEDED(pDlg->GetResult(&pItem)) && pItem) {
                PWSTR pszPath = nullptr;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath) {
                    std::string chosen = WideToUtf8(pszPath);
                    CoTaskMemFree(pszPath);
                    {
                        std::lock_guard<std::mutex> lock(g_stateMutex);
                        if (std::find(g_extraPaths.begin(), g_extraPaths.end(), chosen) == g_extraPaths.end()) {
                            g_extraPaths.push_back(chosen);
                        }
                    }
                    SendLogToUI("INFO", "已添加自定义扫描目录: " + chosen, false);
                    SpawnRefreshAsync(false);
                }
                pItem->Release();
            }
        }
        pDlg->Release();
    }
}

static void HandleWebMessage(const std::string& jsonMsg) {
    std::string cmd = JsonGetString(jsonMsg, "cmd");

    if (cmd == "init") {
        SpawnRefreshAsync(true, !g_previewBasePng.empty());
    } else if (cmd == "refresh") {
        SpawnRefreshAsync(true, false);
    } else if (cmd == "pickFolder") {
        HandleFolderPicker();
    } else if (cmd == "clearPaths") {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_extraPaths.clear();
        }
        SendLogToUI("INFO", "已清空自定义扫描目录", false);
        SpawnRefreshAsync(false);
    } else if (cmd == "saveDomains") {
        std::string content = JsonGetString(jsonMsg, "content");
        WriteFileUtf8(GetCustomDomainsPath(), content);
        SendLogToUI("OK", "已保存自定义域名配置: adobe-domains.txt", false);
        SpawnRefreshAsync(false);
    } else if (cmd == "locateFile") {
        std::string path = JsonGetString(jsonMsg, "path");
        if (!path.empty()) {
            std::wstring arg = L"/select,\"" + Utf8ToWide(path) + L"\"";
            ShellExecuteW(g_hWnd, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        }
    } else if (cmd == "openHostsDir") {
        std::wstring dir = GetSystemHostsPath().parent_path().wstring();
        ShellExecuteW(g_hWnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == "openStateDir") {
        fs::path sd = GetStateDir();
        if (!fs::exists(sd)) {
            std::error_code ec;
            fs::create_directories(sd, ec);
        }
        ShellExecuteW(g_hWnd, L"open", sd.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == "openUrl") {
        std::string url = JsonGetString(jsonMsg, "url");
        if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) {
            ShellExecuteW(g_hWnd, L"open", Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else if (cmd == "relaunchAdmin") {
        wchar_t exeBuf[MAX_PATH * 2] = {0};
        GetModuleFileNameW(nullptr, exeBuf, MAX_PATH * 2);
        if ((INT_PTR)ShellExecuteW(g_hWnd, L"runas", exeBuf, nullptr, nullptr, SW_SHOWNORMAL) > 32) {
            PostQuitMessage(0);
        }
    } else if (cmd == "windowDrag") {
        ReleaseCapture();
        SendMessageW(g_hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    } else if (cmd == "windowMinimize") {
        ShowWindow(g_hWnd, SW_MINIMIZE);
    } else if (cmd == "windowMaximize") {
        if (IsZoomed(g_hWnd)) ShowWindow(g_hWnd, SW_RESTORE);
        else ShowWindow(g_hWnd, SW_MAXIMIZE);
    } else if (cmd == "windowClose") {
        PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
    } else if (cmd == "selftest") {
        if (g_workerBusy.exchange(true)) return;
        std::thread([]() {
            SendLogToUI("STEP", "开始运行内置 19 项 Hosts、自动更新与 Mihomo 规则自检：", false);
            int fails = RunSelfTestEngine(SendLogToUI);
            SendProgressToUI(100, fails == 0 ? "内置自检全部通过 (19/19 PASS)" : "自检发现异常");
            PostJsonToUI(BuildStatusJsonPayload());
            g_workerBusy = false;
        }).detach();
    } else if (cmd == "exportMihomoRules") {
        std::vector<std::string> extraCopy;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            extraCopy = g_extraPaths;
        }
        std::thread([extraCopy]() {
            fs::path outDir;
            std::string err;
            if (ExportMihomoRulesToDisk(extraCopy, outDir, err)) {
                std::string dirUtf8 = WideToUtf8(outDir.wstring());
                SendLogToUI("OK", "已导出 Mihomo 防侧漏规则集至: " + dirUtf8, false);
                PostJsonToUI("{\"type\":\"mihomoExported\",\"path\":\"" + JsonEscape(dirUtf8) + "\"}");
                std::wstring arg = L"/select,\"" + (outDir / L"adobe-block.yaml").wstring() + L"\"";
                ShellExecuteW(g_hWnd, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            } else {
                SendLogToUI("WARN", "导出 Mihomo 规则集失败：" + err, false);
            }
        }).detach();
    } else if (cmd == "checkUpdate") {
        bool silent = JsonGetBool(jsonMsg, "silent", false);
        SpawnCheckUpdateAsync(silent);
    } else if (cmd == "downloadUpdate") {
        SpawnDownloadUpdateAsync();
    } else if (cmd == "cancelUpdate") {
        g_updateCancel = true;
    } else if (cmd == "setUpdateConfig") {
        UpdateConfig cfg = LoadUpdateConfig();
        if (jsonMsg.find("\"autoCheck\"") != std::string::npos) {
            cfg.autoCheck = JsonGetBool(jsonMsg, "autoCheck", cfg.autoCheck);
        }
        if (jsonMsg.find("\"ignoredVersion\"") != std::string::npos) {
            cfg.ignoredVersion = JsonGetString(jsonMsg, "ignoredVersion");
        }
        SaveUpdateConfig(cfg);
        UpdateStateSnapshot snap;
        {
            std::lock_guard<std::mutex> lock(g_updateMutex);
            snap = g_lastUpdateSnap;
            snap.currentVersion = kAppVersion;
            snap.config = cfg;
        }
        PostUpdateStateToUI(snap);
    } else if (cmd == "apply") {
        if (g_workerBusy.exchange(true)) return;
        bool skipHosts = JsonGetBool(jsonMsg, "skipHosts", false);
        bool skipServices = JsonGetBool(jsonMsg, "skipServices", false);
        bool includeInbound = JsonGetBool(jsonMsg, "includeInbound", false);
        std::vector<std::string> extraCopy;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            extraCopy = g_extraPaths;
        }
        std::thread([extraCopy, skipHosts, skipServices, includeInbound]() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ExecuteApplyWorkflow(extraCopy, skipHosts, skipServices, includeInbound, SendLogToUI, SendProgressToUI);
            PostJsonToUI(BuildStatusJsonPayload());
            CoUninitialize();
            g_workerBusy = false;
        }).detach();
    } else if (cmd == "revert") {
        if (g_workerBusy.exchange(true)) return;
        std::thread([]() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ExecuteRevertWorkflow(SendLogToUI, SendProgressToUI);
            PostJsonToUI(BuildStatusJsonPayload());
            CoUninitialize();
            g_workerBusy = false;
        }).detach();
    }
}

static void CaptureWebViewPng(const std::wstring& outPath, std::function<void()> onDone) {
    if (!g_webview) {
        if (onDone) onDone();
        return;
    }
    IStream* pStream = nullptr;
    if (FAILED(SHCreateStreamOnFileEx(outPath.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
                                      FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &pStream)) || !pStream) {
        if (onDone) onDone();
        return;
    }
    g_webview->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
        pStream,
        new ComCallback1<ICoreWebView2CapturePreviewCompletedHandler, HRESULT>([pStream, onDone](HRESULT) -> HRESULT {
            pStream->Commit(STGC_DEFAULT);
            pStream->Release();
            if (onDone) onDone();
            return S_OK;
        }));
}

static void AdvancePreviewCapture() {
    if (g_previewBasePng.empty()) return;

    auto replaceSuffix = [](const std::wstring& base, const std::wstring& suffix) -> std::wstring {
        size_t dot = base.find_last_of(L'.');
        if (dot == std::wstring::npos) return base + suffix;
        return base.substr(0, dot) + suffix + base.substr(dot);
    };

    if (g_previewStep == 0) {
        g_previewStep = 1;
        CaptureWebViewPng(g_previewBasePng, []() {
            PostJsonToUI("{\"type\":\"theme\",\"value\":\"dark\"}");
            SetTimer(g_hWnd, TIMER_PREVIEW, 260, nullptr);
        });
    } else if (g_previewStep == 1) {
        g_previewStep = 2;
        CaptureWebViewPng(replaceSuffix(g_previewBasePng, L"_dark"), []() {
            PostJsonToUI("{\"type\":\"theme\",\"value\":\"light\"}");
            PostJsonToUI("{\"type\":\"tab\",\"index\":1}");
            SetTimer(g_hWnd, TIMER_PREVIEW, 260, nullptr);
        });
    } else if (g_previewStep == 2) {
        g_previewStep = 3;
        CaptureWebViewPng(replaceSuffix(g_previewBasePng, L"_exes"), []() {
            PostJsonToUI("{\"type\":\"tab\",\"index\":3}");
            SetTimer(g_hWnd, TIMER_PREVIEW, 260, nullptr);
        });
    } else if (g_previewStep == 3) {
        g_previewStep = 4;
        CaptureWebViewPng(replaceSuffix(g_previewBasePng, L"_domains"), []() {
            PostQuitMessage(0);
        });
    }
}

static void InitWebView2(HWND hWnd) {
    fs::path cacheDir = GetStateDir() / L"WebView2Cache";
    std::error_code ec;
    fs::create_directories(cacheDir, ec);

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        cacheDir.wstring().c_str(),
        nullptr,
        new ComCallback2<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(
            [hWnd](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) return hr;
                env->CreateCoreWebView2Controller(
                    hWnd,
                    new ComCallback2<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(
                        [hWnd](HRESULT hr2, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (FAILED(hr2) || !ctrl) return hr2;
                            g_controller = ctrl;
                            g_controller->AddRef();

                            // Set subtle light background color (#F5F6F8) to avoid flicker
                            ICoreWebView2Controller2* ctrl2 = nullptr;
                            if (SUCCEEDED(g_controller->QueryInterface(__uuidof(ICoreWebView2Controller2), (void**)&ctrl2)) && ctrl2) {
                                COREWEBVIEW2_COLOR bgCol = {255, 245, 246, 248};
                                ctrl2->put_DefaultBackgroundColor(bgCol);
                                ctrl2->Release();
                            }

                            g_controller->get_CoreWebView2(&g_webview);
                            if (g_webview) {
                                ICoreWebView2Settings* settings = nullptr;
                                if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
                                    settings->put_AreDefaultContextMenusEnabled(FALSE);
                                    settings->put_IsStatusBarEnabled(FALSE);
                                    settings->Release();
                                }

                                EventRegistrationToken token;
                                g_webview->add_WebMessageReceived(
                                    new ComCallback2<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(
                                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                            LPWSTR pszMsg = nullptr;
                                            if (SUCCEEDED(args->TryGetWebMessageAsString(&pszMsg)) && pszMsg) {
                                                std::string msgUtf8 = WideToUtf8(pszMsg);
                                                CoTaskMemFree(pszMsg);
                                                HandleWebMessage(msgUtf8);
                                            }
                                            return S_OK;
                                        }),
                                    &token);

                                RECT rc;
                                GetClientRect(hWnd, &rc);
                                g_controller->put_Bounds(rc);
                                g_controller->put_IsVisible(TRUE);

                                std::wstring htmlW = Utf8ToWide(LoadHtmlContent());
                                g_webview->NavigateToString(htmlW.c_str());
                            }
                            return S_OK;
                        }));
                return S_OK;
            }));
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wParam == TRUE) {
                return 0;
            }
            return DefWindowProcW(hWnd, msg, wParam, lParam);

        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProcW(hWnd, msg, wParam, lParam);
            if (hit == HTCLIENT && !IsZoomed(hWnd)) {
                POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
                ScreenToClient(hWnd, &pt);
                RECT rc;
                GetClientRect(hWnd, &rc);
                const int b = 6;
                bool left = pt.x < b;
                bool right = pt.x >= rc.right - b;
                bool top = pt.y < b;
                bool bottom = pt.y >= rc.bottom - b;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
            }
            return hit;
        }

        case WM_SIZE:
            if (g_controller) {
                RECT rc;
                GetClientRect(hWnd, &rc);
                g_controller->put_Bounds(rc);
            }
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 840;
            mmi->ptMinTrackSize.y = 580;
            return 0;
        }

        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hWnd, &rc);
            HBRUSH hBr = CreateSolidBrush(RGB(245, 246, 248));
            FillRect(hdc, &rc, hBr);
            DeleteObject(hBr);
            return 1;
        }

        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wParam;
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            bool added = false;
            for (UINT i = 0; i < count; ++i) {
                wchar_t pathBuf[MAX_PATH * 2] = {0};
                if (DragQueryFileW(hDrop, i, pathBuf, MAX_PATH * 2) > 0) {
                    fs::path p(pathBuf);
                    if (fs::is_regular_file(p)) p = p.parent_path();
                    if (fs::is_directory(p)) {
                        std::string utf8Dir = WideToUtf8(p.wstring());
                        std::lock_guard<std::mutex> lock(g_stateMutex);
                        if (std::find(g_extraPaths.begin(), g_extraPaths.end(), utf8Dir) == g_extraPaths.end()) {
                            g_extraPaths.push_back(utf8Dir);
                            added = true;
                        }
                    }
                }
            }
            DragFinish(hDrop);
            if (added) {
                SendLogToUI("INFO", "已通过拖拽添加自定义扫描目录", false);
                SpawnRefreshAsync(false);
            }
            return 0;
        }

        case WM_POST_WEB_MSG: {
            std::vector<std::wstring> batch;
            {
                std::lock_guard<std::mutex> lock(g_queueMutex);
                batch.swap(g_msgQueue);
            }
            if (g_webview) {
                for (const auto& m : batch) {
                    g_webview->PostWebMessageAsJson(m.c_str());
                }
            }
            return 0;
        }

        case WM_PREVIEW_READY:
            SetTimer(hWnd, TIMER_PREVIEW, 280, nullptr);
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_PREVIEW) {
                KillTimer(hWnd, TIMER_PREVIEW);
                AdvancePreviewCapture();
            }
            return 0;

        case WM_DESTROY:
            if (g_webview) { g_webview->Release(); g_webview = nullptr; }
            if (g_controller) { g_controller->Close(); g_controller->Release(); g_controller = nullptr; }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ============================================================================
// CLI Mode & WinMain Entry Point
// ============================================================================

static int RunCliAction(const std::string& action, const std::vector<std::string>& extraPaths) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    SetConsoleOutputCP(CP_UTF8);

    auto cliLog = [](const std::string& tag, const std::string& text, bool) {
        std::string line = "  [" + tag + "] " + text + "\r\n";
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), (DWORD)line.size(), &written, nullptr);
    };

    if (action == "selftest") {
        int fails = RunSelfTestEngine([](const std::string& tag, const std::string& text, bool) {
            std::string line = (tag == "DONE" || tag == "FAIL") ? ("\r\n  " + text + "\r\n") : ("  " + tag + "  " + text + "\r\n");
            DWORD written = 0;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), (DWORD)line.size(), &written, nullptr);
        });
        return fails == 0 ? 0 : 1;
    }

    if (action == "status") {
        auto exes = FindAdobeExecutables(extraPaths);
        auto domains = GetAdobeDomains();
        auto svcs = FindAdobeServices();
        auto tasks = FindAdobeTasks();
        int svcOff = 0, taskOff = 0;
        for (const auto& s : svcs) if (ToLowerAscii(s.startMode) == "disabled") svcOff++;
        for (const auto& t : tasks) if (ToLowerAscii(t.startMode) == "disabled" || ToLowerAscii(t.state) == "disabled") taskOff++;
        bool hostsOn = HostsHasBlock(ReadFileUtf8(GetSystemHostsPath()));

        std::ostringstream oss;
        oss << "\r\n  Adobe 联网拦截状态 (Adobe Tools by RITA)\r\n"
            << "  ------------------------------------------------------------\r\n"
            << "  防火墙规则：" << CountFirewallRules() << " 条 (扫描到 " << exes.size() << " 个 Adobe 程序)\r\n"
            << "  hosts 屏蔽 ：" << (hostsOn ? "已启用" : "未启用") << " (" << domains.size() << " 个域名)\r\n"
            << "  Adobe 服务 ：共 " << svcs.size() << " 个，已停用 " << svcOff << " 个\r\n"
            << "  计划任务   ：共 " << tasks.size() << " 个，已禁用 " << taskOff << " 个\r\n\r\n";
        std::string out = oss.str();
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out.data(), (DWORD)out.size(), &written, nullptr);
        return 0;
    }

    if (action == "apply") {
        ExecuteApplyWorkflow(extraPaths, false, false, false, cliLog, nullptr);
        return 0;
    }

    if (action == "revert") {
        ExecuteRevertWorkflow(cliLog, nullptr);
        return 0;
    }

    if (action == "export-mihomo") {
        fs::path outDir;
        std::string err;
        if (ExportMihomoRulesToDisk(extraPaths, outDir, err)) {
            cliLog("OK", "已导出 Mihomo 防侧漏规则集至: " + WideToUtf8(outDir.wstring()), true);
            return 0;
        }
        cliLog("FAIL", "导出 Mihomo 规则集失败: " + err, false);
        return 1;
    }

    if (action == "check-update" || action == "update") {
        UpdateStateSnapshot snap;
        UpdateReleaseInfo rel;
        CheckForUpdatesSync(false, snap, rel);
        cliLog("INFO", std::string("当前版本：v") + kAppVersion, false);
        if (!snap.latestVersion.empty()) {
            cliLog("INFO", "最新版本：v" + snap.latestVersion + (snap.publishedAt.empty() ? "" : (" (" + snap.publishedAt + ")")), false);
        }
        cliLog(snap.state == "error" ? "WARN" : "OK", snap.message, false);
        if (snap.state == "available" && action == "update") {
            if (rel.assetUrl.empty()) {
                cliLog("WARN", "未找到 Windows 可执行更新包，请访问: " + snap.releaseUrl, false);
                return 1;
            }
            fs::path staging = GetUpdatesDir() / L"AdobeTools_update.exe";
            std::atomic<bool> cancelFlag{false};
            std::string err;
            cliLog("STEP", "正在下载 " + rel.assetName + "...", false);
            if (!WinHttpDownloadToFile(rel.assetUrl, staging, rel.assetSize, cancelFlag, nullptr, err)) {
                cliLog("FAIL", "下载失败：" + err, false);
                return 1;
            }
            if (!VerifyWindowsPeFile(staging, err)) {
                std::error_code ec;
                fs::remove(staging, ec);
                cliLog("FAIL", "校验失败：" + err, false);
                return 1;
            }
            if (!rel.checksumAssetUrl.empty()) {
                DWORD sc = 0;
                std::string sumBody, sumErr;
                if (WinHttpGetString(rel.checksumAssetUrl, L"", sc, sumBody, sumErr) && sc >= 200 && sc < 300) {
                    std::string expected = ParseExpectedSha256(sumBody, rel.assetName);
                    if (!expected.empty()) {
                        std::string actual;
                        if (!ComputeFileSha256Hex(staging, actual) || actual != expected) {
                            std::error_code ec;
                            fs::remove(staging, ec);
                            cliLog("FAIL", "SHA-256 校验失败，已中止更新", false);
                            return 1;
                        }
                        cliLog("OK", "SHA-256 校验通过：" + actual, false);
                    }
                }
            }
            if (!ApplyDownloadedUpdateWindows(staging, err)) {
                cliLog("FAIL", "替换失败：" + err, false);
                return 1;
            }
            cliLog("DONE", "更新已完成并启动新版本 v" + rel.version, true);
            return 0;
        }
        return snap.state == "error" ? 1 : 0;
    }

    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string cliAction;
    DWORD postUpdatePid = 0;
    std::wstring postUpdateOldPath;

    if (argv) {
        for (int i = 1; i < argc; ++i) {
            std::wstring argW = argv[i];
            std::string arg = WideToUtf8(argW);
            std::string lower = ToLowerAscii(arg);
            while (!lower.empty() && (lower[0] == '-' || lower[0] == '/')) lower.erase(0, 1);

            if (lower == "apply" || lower == "revert" || lower == "status" || lower == "selftest" ||
                lower == "check-update" || lower == "update" || lower == "export-mihomo") {
                cliAction = lower;
            } else if (lower == "gui-preview" && i + 1 < argc) {
                g_previewBasePng = argv[++i];
            } else if (lower == "update-feed" && i + 1 < argc) {
                g_updateFeedOverride = WideToUtf8(argv[++i]);
            } else if (lower == "dev-ui") {
                g_devUiMode = true;
            } else if (lower == "post-update" && i + 2 < argc) {
                try { postUpdatePid = (DWORD)std::stoul(WideToUtf8(argv[++i])); } catch (...) { postUpdatePid = 0; }
                postUpdateOldPath = argv[++i];
            } else if (lower == "updated-from" && i + 1 < argc) {
                g_justUpdatedFrom = WideToUtf8(argv[++i]);
            } else if (fs::is_directory(argW)) {
                g_extraPaths.push_back(arg);
            }
        }
        LocalFree(argv);
    }

    if (postUpdatePid != 0 || !postUpdateOldPath.empty()) {
        HandlePostUpdateCleanup(postUpdatePid, postUpdateOldPath);
    }
    CleanupStaleOldBinary();

    if (!cliAction.empty()) {
        int rc = RunCliAction(cliAction, g_extraPaths);
        CoUninitialize();
        return rc;
    }

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(15, 15, 16));
    wc.lpszClassName = L"ZakaAdobeNetBlockCppHtmlWnd";
    RegisterClassExW(&wc);

    // Center window calculated with screen DPI
    HDC screenDC = GetDC(nullptr);
    int dpiX = GetDeviceCaps(screenDC, LOGPIXELSX);
    int dpiY = GetDeviceCaps(screenDC, LOGPIXELSY);
    ReleaseDC(nullptr, screenDC);

    int winW = MulDiv(920, dpiX, 96);
    int winH = MulDiv(620, dpiY, 96);
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = std::max(0, (screenW - winW) / 2);
    int posY = std::max(0, (screenH - winH) / 2);

    g_hWnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Adobe Tools",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU,
        posX, posY, winW, winH,
        nullptr, nullptr, hInstance, nullptr);

    // Enable Windows 11 Native Rounded Corners & Drop Shadow
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(g_hWnd, 33, &cornerPref, sizeof(cornerPref));
    MARGINS margins = {1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(g_hWnd, &margins);
    SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    DragAcceptFiles(g_hWnd, TRUE);
    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    InitWebView2(g_hWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
