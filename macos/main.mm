// ============================================================================
// Adobe Tools (by RITA) — Native macOS Objective-C++17 + WKWebView Desktop App
// Shares src/ui.html with the Windows C++17 WebView2 build.
// ============================================================================

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static const char* kAppVersion             = "2.1.0";
static const char* kDefaultUpdateApiUrl    = "https://api.github.com/repos/Rita6333/Adobe-Tools/releases/latest";
static const char* kDefaultReleasesPageUrl = "https://github.com/Rita6333/Adobe-Tools/releases";

static const char* kHostsBeginPrimary = "# >>> Zaka Adobe Block BEGIN >>>";
static const char* kHostsEndPrimary   = "# <<< Zaka Adobe Block END <<<";
static const char* kHostsBeginLegacy  = "# >>> ZakaAdobeBlock BEGIN >>>";
static const char* kHostsEndLegacy    = "# <<< ZakaAdobeBlock END <<<";

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

static const std::vector<std::string> kAdobeProcesses = {
    "Creative Cloud",
    "Adobe Creative Cloud",
    "Adobe Desktop Service",
    "AdobeIPCBroker",
    "CCXProcess",
    "CoreSync",
    "AdobeResourceSynchronizer",
    "Adobe CEF Helper",
    "AdobeUpdateService",
    "AGSService",
    "AGMService",
    "AdobeCRDaemon",
    "AdobeNotificationClient"
};

// ============================================================================
// String & JSON Helpers
// ============================================================================

static std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
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

static std::string ShellSingleQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static std::string ReadFileUtf8(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return "";
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

static bool WriteFileUtf8(const fs::path& p, const std::string& content) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), (std::streamsize)content.size());
    return ofs.good();
}

static std::string RunCapture(const std::string& cmd, int* exitCodeOut = nullptr) {
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        if (exitCodeOut) *exitCodeOut = -1;
        return "";
    }
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        output += buf;
    }
    int status = pclose(pipe);
    if (exitCodeOut) *exitCodeOut = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return output;
}

// ============================================================================
// macOS Paths & Hosts Logic (13-Point Self-Test Compatible)
// ============================================================================

static fs::path GetHomeDir() {
    const char* h = getenv("HOME");
    return h ? fs::path(h) : fs::path("/tmp");
}

static fs::path GetUserSupportDir() {
    return GetHomeDir() / "Library/Application Support/AdobeTools";
}

static fs::path GetCustomDomainsPath() {
    return GetUserSupportDir() / "adobe-domains.txt";
}

static fs::path GetStateDir() {
    return fs::path("/usr/local/var/zaka-adobe-block");
}

static fs::path GetSystemHostsPath() {
    return fs::path("/etc/hosts");
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
                normalized.insert(pos, "\n");
                gluedModified = true;
                pos += 1 + bStr.size();
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
    if (start < normalized.size()) rawLines.push_back(normalized.substr(start));

    std::vector<std::string> kept;
    bool inside = false;
    bool modified = gluedModified;
    for (const auto& raw : rawLines) {
        std::string trimmed = Trim(raw);
        if (!inside) {
            if (IsHostsBeginMarker(trimmed)) { inside = true; modified = true; continue; }
            if (IsHostsEndMarker(trimmed))   { modified = true; continue; }
            kept.push_back(raw);
        } else {
            if (IsHostsEndMarker(trimmed)) inside = false;
        }
    }

    if (outModified) *outModified = modified;
    if (!modified) return content;

    std::string joined;
    for (const auto& l : kept) joined += l;
    while (joined.size() >= 2 && joined.compare(joined.size() - 2, 2, "\n\n") == 0) {
        joined.pop_back();
    }
    if (gluedModified && (content.empty() || content.back() != '\n')) {
        while (!joined.empty() && (joined.back() == '\r' || joined.back() == '\n')) joined.pop_back();
    }
    return joined;
}

static std::string BuildHostsBlock(const std::vector<std::string>& domains) {
    std::ostringstream oss;
    oss << kHostsBeginPrimary << "\n";
    oss << "# Added by Adobe Tools (by RITA). Delete this block to undo.\n";
    for (const auto& d : domains) {
        if (d.empty()) continue;
        oss << "0.0.0.0 " << d << "\n";
        oss << "::1 " << d << "\n";
    }
    oss << kHostsEndPrimary << "\n";
    return oss.str();
}

static std::string ApplyHostsToString(const std::string& existingContent, const std::vector<std::string>& domains) {
    bool dummy = false;
    std::string clean = StripHostsBlock(existingContent, &dummy);
    while (!clean.empty() && (clean.back() == '\r' || clean.back() == '\n')) clean.pop_back();
    std::string block = BuildHostsBlock(domains);
    return clean.empty() ? block : (clean + "\n\n" + block);
}

static std::vector<std::string> GetAdobeDomains() {
    std::set<std::string> setDom;
    for (const auto& d : kBuiltInDomains) setDom.insert(ToLowerAscii(Trim(d)));
    fs::path cp = GetCustomDomainsPath();
    if (fs::exists(cp)) {
        std::istringstream iss(ReadFileUtf8(cp));
        std::string line;
        while (std::getline(iss, line)) {
            std::string t = Trim(line);
            if (t.empty() || t[0] == '#') continue;
            std::istringstream ls(t);
            std::string tok, lastTok;
            while (ls >> tok) lastTok = tok;
            lastTok = ToLowerAscii(Trim(lastTok));
            if (!lastTok.empty() && lastTok[0] != '#') setDom.insert(lastTok);
        }
    }
    return std::vector<std::string>(setDom.begin(), setDom.end());
}

static std::string CurrentTimeIso() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf;
    localtime_r(&t, &tmBuf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return buf;
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

static bool EndsWithNoCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return ToLowerAscii(s.substr(s.size() - suffix.size())) == ToLowerAscii(suffix);
}

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
    std::string line, fallback;
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
        if (ToLowerAscii(fname) == targetLower) return ToLowerAscii(hash);
        if (fallback.empty() && !targetNorm.empty() && NormalizeAssetKey(fname) == targetNorm) {
            fallback = ToLowerAscii(hash);
        }
    }
    return fallback;
}

static bool ComputeBufferSha256Hex(const uint8_t* data, size_t len, std::string& outHex) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, (CC_LONG)len, digest);
    static const char* kHex = "0123456789abcdef";
    outHex.clear();
    outHex.reserve(CC_SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        outHex += kHex[(digest[i] >> 4) & 0xF];
        outHex += kHex[digest[i] & 0xF];
    }
    return true;
}

static bool ComputeFileSha256Hex(const fs::path& filePath, std::string& outHex) {
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs) return false;
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    std::vector<char> chunk(65536);
    while (ifs) {
        ifs.read(chunk.data(), (std::streamsize)chunk.size());
        std::streamsize n = ifs.gcount();
        if (n > 0) CC_SHA256_Update(&ctx, chunk.data(), (CC_LONG)n);
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    static const char* kHex = "0123456789abcdef";
    outHex.clear();
    outHex.reserve(CC_SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        outHex += kHex[(digest[i] >> 4) & 0xF];
        outHex += kHex[digest[i] & 0xF];
    }
    return true;
}

struct UpdateConfig {
    bool autoCheck = true;
    std::string ignoredVersion;
    std::string lastChecked;
};

static fs::path GetUpdateConfigPath() {
    return GetUserSupportDir() / "update-config.json";
}

static UpdateConfig LoadUpdateConfig() {
    UpdateConfig cfg;
    fs::path p = GetUpdateConfigPath();
    if (!fs::exists(p)) return cfg;
    std::string raw = ReadFileUtf8(p);
    cfg.autoCheck = JsonGetBool(raw, "autoCheck", true);
    cfg.ignoredVersion = JsonGetString(raw, "ignoredVersion");
    cfg.lastChecked = JsonGetString(raw, "lastChecked");
    return cfg;
}

static void SaveUpdateConfig(const UpdateConfig& cfg) {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"autoCheck\": " << (cfg.autoCheck ? "true" : "false") << ",\n"
        << "  \"ignoredVersion\": \"" << JsonEscape(cfg.ignoredVersion) << "\",\n"
        << "  \"lastChecked\": \"" << JsonEscape(cfg.lastChecked) << "\"\n"
        << "}\n";
    WriteFileUtf8(GetUpdateConfigPath(), oss.str());
}

struct MacReleaseInfo {
    std::string tagName;
    std::string version;
    std::string releaseName;
    std::string releaseUrl = kDefaultReleasesPageUrl;
    std::string publishedAt;
    std::string notes;
    std::string assetName;
    int64_t assetSize = 0;
    std::string assetUrl;
    std::string checksumAssetUrl;
};

static int RunSelfTestEngine(std::function<void(const std::string&, const std::string&, bool)> logFn) {
    int fails = 0;
    auto check = [&](bool ok, const std::string& title) {
        if (ok) { if (logFn) logFn("PASS", title, false); }
        else    { fails++; if (logFn) logFn("FAIL", title, false); }
    };

    std::string orig = "# macOS hosts\n127.0.0.1 localhost\n::1 localhost\n";
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

    std::string glued = std::string("127.0.0.1 localhost") + kHostsBeginLegacy + "\n0.0.0.0 adobe.com\n" + kHostsEndLegacy;
    bool mod4 = false;
    std::string unglued = StripHostsBlock(glued, &mod4);
    check(mod4 && Trim(unglued) == "127.0.0.1 localhost", "行尾无换行 + 块粘连也能剥离");

    std::string withCustom = orig + "0.0.0.0 mycustom.local\n\n" + BuildHostsBlock({"activate.adobe.com"});
    bool mod5 = false;
    std::string afterCustom = StripHostsBlock(withCustom, &mod5);
    check(afterCustom.find("0.0.0.0 mycustom.local") != std::string::npos, "非块内的自定义行保留");

    std::string broken = orig + kHostsBeginPrimary + "\n0.0.0.0 activate.adobe.com\n";
    bool mod6 = false;
    std::string fixedBroken = StripHostsBlock(broken, &mod6);
    check(mod6 && !HostsHasBlock(fixedBroken), "残缺块也能清掉");

    bool semverOk = (CompareSemVer("2.1.0", "2.2.0") < 0) &&
                    (CompareSemVer("v2.2.0", "v2.10.0") < 0) &&
                    (CompareSemVer("v2.1.0", "2.1.0.0") == 0) &&
                    (CompareSemVer("2.2.0-rc.1", "2.2.0") < 0) &&
                    (CompareSemVer("3.0.0", "2.9.9") > 0);
    check(semverOk, "SemVer 语义化版本号比较逻辑准确");

    check(NormalizeVersionTag("v2.2.0") == "2.2.0", "版本标签前缀规范化正常");
    check(EndsWithNoCase("Adobe-Tools-macOS-Universal.dmg", ".dmg"), "macOS 更新资产后缀匹配正常");

    std::string sampleSums =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  Adobe-Tools-macOS-Universal.dmg\n";
    check(ParseExpectedSha256(sampleSums, "Adobe-Tools-macOS-Universal.dmg") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA256SUMS 校验值解析正常");

    std::string abcHash;
    check(ComputeBufferSha256Hex(reinterpret_cast<const uint8_t*>("abc"), 3, abcHash) &&
              abcHash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "CommonCrypto SHA-256 哈希计算准确");

    if (logFn) {
        logFn(fails == 0 ? "DONE" : "FAIL",
              fails == 0 ? "SELFTEST_OK — 18 项内置逻辑自检全部通过！" : "SELFTEST_FAIL",
              fails == 0);
    }
    return fails;
}

// ============================================================================
// macOS Scanner (Adobe .app Bundles, LaunchAgents/Daemons, Firewall)
// ============================================================================

struct LaunchItemInfo {
    std::string kind;
    std::string name;
    std::string display;
    std::string state;
    std::string startMode;
};

static std::vector<std::string> FindAdobeApps(const std::vector<std::string>& extraPaths) {
    std::set<std::string> roots = {
        "/Applications",
        "/Library/Application Support/Adobe",
        (GetHomeDir() / "Applications").string()
    };
    for (const auto& ep : extraPaths) {
        if (!ep.empty()) roots.insert(ep);
    }

    std::set<std::string> found;
    std::error_code ec;

    auto scanDir = [&](const fs::path& dir, int depth) {
        if (!fs::exists(dir, ec)) return;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (it.depth() > depth) {
                it.disable_recursion_pending();
                continue;
            }
            fs::path p = it->path();
            std::string fname = ToLowerAscii(p.filename().string());
            if (p.extension() == ".app") {
                std::string fullLower = ToLowerAscii(p.string());
                if (fullLower.find("adobe") != std::string::npos ||
                    fname.find("acrobat") != std::string::npos ||
                    fname.find("creative cloud") != std::string::npos) {
                    found.insert(p.string());
                }
                it.disable_recursion_pending();
            }
        }
    };

    for (const auto& r : roots) {
        scanDir(fs::path(r), 2);
    }
    return std::vector<std::string>(found.begin(), found.end());
}

static std::vector<LaunchItemInfo> FindAdobeLaunchItems() {
    std::vector<LaunchItemInfo> items;
    std::vector<std::pair<std::string, fs::path>> dirs = {
        {"LaunchAgent",  "/Library/LaunchAgents"},
        {"LaunchDaemon", "/Library/LaunchDaemons"},
        {"用户启动项",   GetHomeDir() / "Library/LaunchAgents"}
    };

    std::error_code ec;
    for (const auto& kv : dirs) {
        if (!fs::exists(kv.second, ec)) continue;
        for (const auto& entry : fs::directory_iterator(kv.second, ec)) {
            std::string fn = entry.path().filename().string();
            std::string fl = ToLowerAscii(fn);
            if (fl.find("com.adobe.") != 0) continue;
            bool disabled = (fl.size() > 14 && fl.substr(fl.size() - 14) == ".zaka-disabled");
            items.push_back({
                kv.first,
                fn,
                kv.second.string(),
                disabled ? "Disabled" : "Ready",
                disabled ? "Disabled" : "Enabled"
            });
        }
    }
    return items;
}

static int CountBlockedFirewallApps() {
    int rc = 0;
    std::string out = RunCapture("/usr/libexec/ApplicationFirewall/socketfilterfw --listapps 2>/dev/null", &rc);
    if (out.empty()) return 0;
    std::istringstream iss(out);
    std::string line, prevLine;
    int count = 0;
    while (std::getline(iss, line)) {
        std::string low = ToLowerAscii(line);
        std::string prevLow = ToLowerAscii(prevLine);
        if (low.find("block incoming") != std::string::npos && prevLow.find("adobe") != std::string::npos) {
            count++;
        }
        prevLine = line;
    }
    return count;
}

static bool RunPrivilegedShellScript(const std::string& shellScript, std::string* errOut = nullptr) {
    fs::path tmpScript = GetUserSupportDir() / "run_privileged.sh";
    if (!WriteFileUtf8(tmpScript, "#!/bin/bash\nset -e\n" + shellScript + "\n")) {
        if (errOut) *errOut = "无法创建临时提权脚本";
        return false;
    }
    chmod(tmpScript.c_str(), 0700);

    std::string asEscapedPath;
    asEscapedPath.reserve(tmpScript.string().size() + 8);
    for (char c : tmpScript.string()) {
        if (c == '\\' || c == '\"') asEscapedPath += '\\';
        asEscapedPath += c;
    }
    std::string asCode =
        "do shell script \"/bin/bash \" & quoted form of \"" + asEscapedPath + "\" with administrator privileges";
    std::string appleScript = "osascript -e " + ShellSingleQuote(asCode) + " 2>&1";

    int rc = 0;
    std::string output = RunCapture(appleScript, &rc);
    std::error_code ec;
    fs::remove(tmpScript, ec);
    if (rc != 0) {
        if (errOut) *errOut = Trim(output).empty() ? "管理员授权被取消或执行失败" : Trim(output);
        return false;
    }
    return true;
}

// ============================================================================
// Cocoa + WKWebView Window & Bridge Controller
// ============================================================================

@interface AdobeToolsController : NSObject <NSApplicationDelegate, WKScriptMessageHandler, WKNavigationDelegate, NSURLSessionDownloadDelegate>
@property (strong, nonatomic) NSWindow* window;
@property (strong, nonatomic) WKWebView* webView;
@property (strong, nonatomic) NSURLSessionDownloadTask* activeDownloadTask;
@property (assign, nonatomic) double downloadStartTime;
- (void)startMacUpdateDownload;
@end

static std::mutex g_stateMutex;
static std::vector<std::string> g_extraPaths;
static std::atomic<bool> g_workerBusy{false};
static AdobeToolsController* g_appCtrl = nil;

static void PostJsonToUI(const std::string& jsonUtf8);
static void SendLogToUI(const std::string& tag, const std::string& text, bool highlight = false);

static std::mutex g_updateMutex;
static MacReleaseInfo g_cachedRelease;
static std::atomic<bool> g_updateBusy{false};

static void PostUpdateStateMac(
    const std::string& state, bool silent, const MacReleaseInfo& rel,
    int progress, int64_t dlBytes, int64_t totBytes, int64_t speedBps,
    const std::string& msg, const UpdateConfig& cfg)
{
    std::ostringstream oss;
    oss << "{\"type\":\"updateState\",\"data\":{"
        << "\"state\":\"" << JsonEscape(state) << "\","
        << "\"silent\":" << (silent ? "true" : "false") << ","
        << "\"currentVersion\":\"" << JsonEscape(kAppVersion) << "\","
        << "\"latestVersion\":\"" << JsonEscape(rel.version) << "\","
        << "\"releaseName\":\"" << JsonEscape(rel.releaseName) << "\","
        << "\"releaseUrl\":\"" << JsonEscape(rel.releaseUrl.empty() ? kDefaultReleasesPageUrl : rel.releaseUrl) << "\","
        << "\"publishedAt\":\"" << JsonEscape(rel.publishedAt) << "\","
        << "\"notes\":\"" << JsonEscape(rel.notes) << "\","
        << "\"assetName\":\"" << JsonEscape(rel.assetName) << "\","
        << "\"assetSize\":" << rel.assetSize << ","
        << "\"hasDirectAsset\":" << (!rel.assetUrl.empty() ? "true" : "false") << ","
        << "\"hasChecksum\":" << (!rel.checksumAssetUrl.empty() ? "true" : "false") << ","
        << "\"progress\":" << progress << ","
        << "\"downloadedBytes\":" << dlBytes << ","
        << "\"totalBytes\":" << totBytes << ","
        << "\"speedBps\":" << speedBps << ","
        << "\"message\":\"" << JsonEscape(msg) << "\","
        << "\"autoCheck\":" << (cfg.autoCheck ? "true" : "false") << ","
        << "\"ignoredVersion\":\"" << JsonEscape(cfg.ignoredVersion) << "\","
        << "\"lastChecked\":\"" << JsonEscape(cfg.lastChecked) << "\""
        << "}}";
    PostJsonToUI(oss.str());
}

static void CheckForUpdatesMacAsync(bool silent) {
    if (g_updateBusy.exchange(true)) return;
    UpdateConfig cfg = LoadUpdateConfig();
    MacReleaseInfo emptyRel;
    PostUpdateStateMac("checking", silent, emptyRel, 0, 0, 0, 0, "正在连接 GitHub Releases 检查更新...", cfg);

    NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:kDefaultUpdateApiUrl]];
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url
                                                       cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                   timeoutInterval:15.0];
    [req setValue:[NSString stringWithFormat:@"Adobe-Tools/%s", kAppVersion] forHTTPHeaderField:@"User-Agent"];
    [req setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];

    NSURLSessionDataTask* task = [[NSURLSession sharedSession] dataTaskWithRequest:req
        completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
            UpdateConfig latestCfg = LoadUpdateConfig();
            MacReleaseInfo rel;
            if (error || !response) {
                std::string errMsg = error ? std::string([error.localizedDescription UTF8String]) : "网络连接失败";
                PostUpdateStateMac("error", silent, rel, 0, 0, 0, 0, "检查更新失败：" + errMsg, latestCfg);
                g_updateBusy = false;
                return;
            }
            NSInteger status = ((NSHTTPURLResponse*)response).statusCode;
            latestCfg.lastChecked = CurrentTimeIso();
            SaveUpdateConfig(latestCfg);

            if (status == 404) {
                rel.version = kAppVersion;
                PostUpdateStateMac("no_release", silent, rel, 0, 0, 0, 0,
                                   std::string("当前版本 v") + kAppVersion + "（仓库尚未发布正式 Release）", latestCfg);
                g_updateBusy = false;
                return;
            }
            if (status == 403 || status == 429) {
                PostUpdateStateMac("error", silent, rel, 0, 0, 0, 0,
                                   "GitHub API 请求频率受限，请稍后再试或直接前往 Releases 页面", latestCfg);
                g_updateBusy = false;
                return;
            }
            if (status < 200 || status >= 300 || !data) {
                PostUpdateStateMac("error", silent, rel, 0, 0, 0, 0,
                                   "检查更新失败（HTTP " + std::to_string((int)status) + "）", latestCfg);
                g_updateBusy = false;
                return;
            }

            NSError* jsonErr = nil;
            id obj = [NSJSONSerialization JSONObjectWithData:data options:0 error:&jsonErr];
            if (jsonErr || ![obj isKindOfClass:[NSDictionary class]]) {
                PostUpdateStateMac("error", silent, rel, 0, 0, 0, 0, "无法解析更新响应数据", latestCfg);
                g_updateBusy = false;
                return;
            }

            NSDictionary* dict = (NSDictionary*)obj;
            auto getStr = [&](NSString* k) -> std::string {
                id v = dict[k];
                return [v isKindOfClass:[NSString class]] ? std::string([(NSString*)v UTF8String]) : "";
            };
            rel.tagName = getStr(@"tag_name");
            rel.version = NormalizeVersionTag(rel.tagName);
            rel.releaseName = getStr(@"name");
            if (rel.releaseName.empty()) rel.releaseName = "Adobe Tools " + rel.tagName;
            rel.releaseUrl = getStr(@"html_url");
            if (rel.releaseUrl.empty()) rel.releaseUrl = kDefaultReleasesPageUrl;
            std::string pub = getStr(@"published_at");
            rel.publishedAt = pub.size() >= 10 ? pub.substr(0, 10) : pub;
            rel.notes = getStr(@"body");

            NSArray* assets = [dict[@"assets"] isKindOfClass:[NSArray class]] ? dict[@"assets"] : @[];
            int bestScore = -1;
            for (id item in assets) {
                if (![item isKindOfClass:[NSDictionary class]]) continue;
                NSDictionary* a = (NSDictionary*)item;
                NSString* n = [a[@"name"] isKindOfClass:[NSString class]] ? a[@"name"] : @"";
                NSString* u = [a[@"browser_download_url"] isKindOfClass:[NSString class]] ? a[@"browser_download_url"] : @"";
                int64_t sz = [a[@"size"] isKindOfClass:[NSNumber class]] ? [a[@"size"] longLongValue] : 0;
                std::string name = [n UTF8String];
                std::string dlUrl = [u UTF8String];
                if (name.empty() || dlUrl.empty()) continue;

                std::string low = ToLowerAscii(name);
                if (low == "sha256sums.txt" || low == "checksums.txt") {
                    rel.checksumAssetUrl = dlUrl;
                    continue;
                }
                if (EndsWithNoCase(name, ".dmg") || EndsWithNoCase(name, ".pkg")) {
                    int score = EndsWithNoCase(name, ".dmg") ? 20 : 10;
                    if (low.find("universal") != std::string::npos) score += 10;
                    if (score > bestScore) {
                        bestScore = score;
                        rel.assetName = name;
                        rel.assetSize = sz;
                        rel.assetUrl = dlUrl;
                    }
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_updateMutex);
                g_cachedRelease = rel;
            }

            if (CompareSemVer(kAppVersion, rel.version) < 0) {
                std::string msg = "发现新版本 v" + rel.version + "（当前 v" + kAppVersion + "）";
                PostUpdateStateMac("available", silent, rel, 0, 0, 0, 0, msg, latestCfg);
                SendLogToUI("OK", "自动更新检查：" + msg, false);
            } else {
                std::string msg = std::string("当前已是最新版本 v") + kAppVersion;
                PostUpdateStateMac("up_to_date", silent, rel, 0, 0, 0, 0, msg, latestCfg);
                if (!silent) SendLogToUI("INFO", "自动更新检查：" + msg, false);
            }
            g_updateBusy = false;
        }];
    [task resume];
}

static void PostJsonToUI(const std::string& jsonUtf8) {
    if (!g_appCtrl || !g_appCtrl.webView) return;
    std::string escaped = JsonEscape(jsonUtf8);
    NSString* escapedNs = [NSString stringWithUTF8String:escaped.c_str()];
    if (!escapedNs) return;
    NSString* js = [NSString stringWithFormat:@"window.__receiveHostMessage && window.__receiveHostMessage(\"%@\");", escapedNs];
    dispatch_async(dispatch_get_main_queue(), ^{
        [g_appCtrl.webView evaluateJavaScript:js completionHandler:nil];
    });
}

static void SendLogToUI(const std::string& tag, const std::string& text, bool highlight) {
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

static std::string BuildStatusJsonPayload() {
    std::vector<std::string> extraCopy;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        extraCopy = g_extraPaths;
    }

    auto apps = FindAdobeApps(extraCopy);
    auto domains = GetAdobeDomains();
    auto items = FindAdobeLaunchItems();
    bool hostsOn = HostsHasBlock(ReadFileUtf8(GetSystemHostsPath()));
    bool hostsBackup = fs::exists(GetStateDir() / "hosts.original");
    int fwBlocked = CountBlockedFirewallApps();

    int totalLaunch = (int)items.size();
    int disabledLaunch = 0;
    for (const auto& it : items) {
        if (it.state == "Disabled") disabledLaunch++;
    }

    std::string customDomains = ReadFileUtf8(GetCustomDomainsPath());
    if (customDomains.empty()) {
        customDomains = "# 额外要屏蔽的域名（一行一个，# 开头为注释）\n";
    }

    std::string lastApplied = Trim(ReadFileUtf8(GetStateDir() / "last_applied.txt"));
    if (lastApplied.empty()) lastApplied = "尚无记录";
    UpdateConfig updCfg = LoadUpdateConfig();

    std::ostringstream oss;
    oss << "{\"type\":\"status\",\"data\":{";
    oss << "\"appVersion\":\"" << JsonEscape(kAppVersion) << "\",";
    oss << "\"platform\":\"macos\",";
    oss << "\"justUpdatedFrom\":\"\",";
    oss << "\"updateConfig\":{"
        << "\"autoCheck\":" << (updCfg.autoCheck ? "true" : "false") << ","
        << "\"ignoredVersion\":\"" << JsonEscape(updCfg.ignoredVersion) << "\","
        << "\"lastChecked\":\"" << JsonEscape(updCfg.lastChecked) << "\"},";
    oss << "\"isAdmin\":true,";
    oss << "\"fwServiceRunning\":true,";
    oss << "\"fwRuleCount\":" << fwBlocked << ",";
    oss << "\"hostsEnabled\":" << (hostsOn ? "true" : "false") << ",";
    oss << "\"hostsBackup\":" << (hostsBackup ? "true" : "false") << ",";
    oss << "\"domainCount\":" << domains.size() << ",";
    oss << "\"svcTotal\":" << totalLaunch << ",";
    oss << "\"svcDisabled\":" << disabledLaunch << ",";
    oss << "\"taskTotal\":0,\"taskDisabled\":0,";
    oss << "\"lastApplied\":\"" << JsonEscape(lastApplied) << "\",";
    oss << "\"customDomainsText\":\"" << JsonEscape(customDomains) << "\",";

    oss << "\"extraPaths\":[";
    for (size_t i = 0; i < extraCopy.size(); ++i) {
        oss << "\"" << JsonEscape(extraCopy[i]) << "\"" << (i + 1 < extraCopy.size() ? "," : "");
    }
    oss << "],\"exes\":[";
    for (size_t i = 0; i < apps.size(); ++i) {
        std::string fname = fs::path(apps[i]).filename().string();
        oss << "{\"name\":\"" << JsonEscape(fname) << "\",\"path\":\"" << JsonEscape(apps[i]) << "\"}"
            << (i + 1 < apps.size() ? "," : "");
    }
    oss << "],\"servicesAndTasks\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        oss << "{\"kind\":\"" << JsonEscape(it.kind)
            << "\",\"name\":\"" << JsonEscape(it.name)
            << "\",\"display\":\"" << JsonEscape(it.display)
            << "\",\"state\":\"" << JsonEscape(it.state)
            << "\",\"startMode\":\"" << JsonEscape(it.startMode) << "\"}"
            << (i + 1 < items.size() ? "," : "");
    }
    oss << "]}}";
    return oss.str();
}

static void SpawnRefreshAsync() {
    std::thread([]() {
        PostJsonToUI(BuildStatusJsonPayload());
    }).detach();
}

static void ExecuteMacApply(bool skipHosts, bool skipServices) {
    if (g_workerBusy.exchange(true)) return;
    std::vector<std::string> extraCopy;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        extraCopy = g_extraPaths;
    }

    std::thread([extraCopy, skipHosts, skipServices]() {
        SendProgressToUI(15, "正在准备 macOS 断网拦截操作（请在弹窗中输入管理员密码）...");
        SendLogToUI("STEP", "开始执行 macOS 三层断网拦截", false);

        auto apps = FindAdobeApps(extraCopy);
        auto domains = GetAdobeDomains();
        fs::path stagedHosts = GetUserSupportDir() / "staged_hosts.tmp";
        if (!skipHosts) {
            std::string updatedHosts = ApplyHostsToString(ReadFileUtf8(GetSystemHostsPath()), domains);
            WriteFileUtf8(stagedHosts, updatedHosts);
        }

        std::ostringstream sh;
        sh << "mkdir -p /usr/local/var/zaka-adobe-block\n";

        if (!skipHosts) {
            sh << "if [ ! -f /usr/local/var/zaka-adobe-block/hosts.original ]; then cp /etc/hosts /usr/local/var/zaka-adobe-block/hosts.original; fi\n";
            sh << "cp " << ShellSingleQuote(stagedHosts.string()) << " /etc/hosts\n";
            sh << "chmod 644 /etc/hosts\n";
            sh << "dscacheutil -flushcache 2>/dev/null || true\n";
            sh << "killall -HUP mDNSResponder 2>/dev/null || true\n";
        }

        for (const auto& app : apps) {
            sh << "/usr/libexec/ApplicationFirewall/socketfilterfw --add " << ShellSingleQuote(app) << " >/dev/null 2>&1 || true\n";
            sh << "/usr/libexec/ApplicationFirewall/socketfilterfw --blockapp " << ShellSingleQuote(app) << " >/dev/null 2>&1 || true\n";
        }

        if (!skipServices) {
            std::vector<std::string> dirs = {
                "/Library/LaunchAgents",
                "/Library/LaunchDaemons",
                (GetHomeDir() / "Library/LaunchAgents").string()
            };
            for (const auto& d : dirs) {
                sh << "for f in " << ShellSingleQuote(d) << "/com.adobe.*.plist; do "
                   << "[ -e \"$f\" ] || continue; "
                   << "launchctl unload -w \"$f\" >/dev/null 2>&1 || true; "
                   << "mv \"$f\" \"$f.zaka-disabled\" 2>/dev/null || true; done\n";
            }
            for (const auto& proc : kAdobeProcesses) {
                sh << "pkill -x " << ShellSingleQuote(proc) << " >/dev/null 2>&1 || true\n";
            }
        }
        sh << "date '+%Y-%m-%d %H:%M:%S' > /usr/local/var/zaka-adobe-block/last_applied.txt\n";

        SendProgressToUI(55, "等待 macOS 管理员授权执行...");
        std::string err;
        bool ok = RunPrivilegedShellScript(sh.str(), &err);
        std::error_code ec;
        fs::remove(stagedHosts, ec);

        if (ok) {
            SendProgressToUI(100, "macOS 断网拦截已生效！");
            SendLogToUI("DONE", "搞定！已完成 hosts 屏蔽、应用防火墙拦截与开机项禁用。", true);
        } else {
            SendProgressToUI(0, "操作未完成");
            SendLogToUI("WARN", "执行取消或失败：" + err, false);
        }
        PostJsonToUI(BuildStatusJsonPayload());
        g_workerBusy = false;
    }).detach();
}

static void ExecuteMacRevert() {
    if (g_workerBusy.exchange(true)) return;
    std::vector<std::string> extraCopy;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        extraCopy = g_extraPaths;
    }

    std::thread([extraCopy]() {
        SendProgressToUI(20, "正在还原 macOS 防火墙、hosts 与开机启动项...");
        SendLogToUI("STEP", "开始一键还原 macOS 全部改动", false);

        auto apps = FindAdobeApps(extraCopy);
        bool mod = false;
        std::string stripped = StripHostsBlock(ReadFileUtf8(GetSystemHostsPath()), &mod);
        fs::path stagedHosts = GetUserSupportDir() / "staged_hosts_revert.tmp";
        if (mod) WriteFileUtf8(stagedHosts, stripped);

        std::ostringstream sh;
        if (mod) {
            sh << "cp " << ShellSingleQuote(stagedHosts.string()) << " /etc/hosts\n";
            sh << "chmod 644 /etc/hosts\n";
            sh << "dscacheutil -flushcache 2>/dev/null || true\n";
            sh << "killall -HUP mDNSResponder 2>/dev/null || true\n";
        }
        for (const auto& app : apps) {
            sh << "/usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp " << ShellSingleQuote(app) << " >/dev/null 2>&1 || true\n";
            sh << "/usr/libexec/ApplicationFirewall/socketfilterfw --remove " << ShellSingleQuote(app) << " >/dev/null 2>&1 || true\n";
        }
        std::vector<std::string> dirs = {
            "/Library/LaunchAgents",
            "/Library/LaunchDaemons",
            (GetHomeDir() / "Library/LaunchAgents").string()
        };
        for (const auto& d : dirs) {
            sh << "for f in " << ShellSingleQuote(d) << "/com.adobe.*.plist.zaka-disabled; do "
               << "[ -e \"$f\" ] || continue; "
               << "mv \"$f\" \"${f%.zaka-disabled}\" 2>/dev/null || true; done\n";
        }
        sh << "rm -f /usr/local/var/zaka-adobe-block/last_applied.txt\n";

        std::string err;
        bool ok = RunPrivilegedShellScript(sh.str(), &err);
        std::error_code ec;
        fs::remove(stagedHosts, ec);

        if (ok) {
            SendProgressToUI(100, "已全部还原！");
            SendLogToUI("DONE", "已还原 hosts、应用防火墙与 Adobe 启动项。", true);
        } else {
            SendProgressToUI(0, "还原未完成");
            SendLogToUI("WARN", "还原取消或失败：" + err, false);
        }
        PostJsonToUI(BuildStatusJsonPayload());
        g_workerBusy = false;
    }).detach();
}

@implementation AdobeToolsController

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    NSRect frame = NSMakeRect(0, 0, 920, 620);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                              NSWindowStyleMaskFullSizeContentView;
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"Adobe Tools";
    self.window.titleVisibility = NSWindowTitleHidden;
    self.window.titlebarAppearsTransparent = YES;
    self.window.minSize = NSMakeSize(840, 580);
    [self.window standardWindowButton:NSWindowCloseButton].hidden = YES;
    [self.window standardWindowButton:NSWindowMiniaturizeButton].hidden = YES;
    [self.window standardWindowButton:NSWindowZoomButton].hidden = YES;
    [self.window center];

    WKWebViewConfiguration* cfg = [[WKWebViewConfiguration alloc] init];
    [cfg.userContentController addScriptMessageHandler:self name:@"adobeTools"];

    self.webView = [[WKWebView alloc] initWithFrame:self.window.contentView.bounds configuration:cfg];
    self.webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.webView.navigationDelegate = self;
    [self.window.contentView addSubview:self.webView];

    NSString* htmlPath = [[NSBundle mainBundle] pathForResource:@"ui" ofType:@"html"];
    if (htmlPath) {
        NSString* htmlStr = [NSString stringWithContentsOfFile:htmlPath encoding:NSUTF8StringEncoding error:nil];
        [self.webView loadHTMLString:htmlStr baseURL:[NSURL fileURLWithPath:[htmlPath stringByDeletingLastPathComponent]]];
    }

    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}

- (void)userContentController:(WKUserContentController*)userContentController didReceiveScriptMessage:(WKScriptMessage*)message {
    if (![message.body isKindOfClass:[NSString class]]) return;
    std::string jsonMsg = [(NSString*)message.body UTF8String];
    std::string cmd = JsonGetString(jsonMsg, "cmd");

    if (cmd == "init" || cmd == "refresh") {
        SpawnRefreshAsync();
    } else if (cmd == "windowDrag") {
        NSEvent* evt = [NSApp currentEvent];
        if (evt) [self.window performWindowDragWithEvent:evt];
    } else if (cmd == "windowMinimize") {
        [self.window miniaturize:nil];
    } else if (cmd == "windowMaximize") {
        [self.window zoom:nil];
    } else if (cmd == "windowClose") {
        [self.window close];
    } else if (cmd == "pickFolder") {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = NO;
        [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
            if (result == NSModalResponseOK && panel.URL) {
                std::string chosen = [panel.URL.path UTF8String];
                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    if (std::find(g_extraPaths.begin(), g_extraPaths.end(), chosen) == g_extraPaths.end()) {
                        g_extraPaths.push_back(chosen);
                    }
                }
                SendLogToUI("INFO", "已添加自定义扫描目录: " + chosen, false);
                SpawnRefreshAsync();
            }
        }];
    } else if (cmd == "clearPaths") {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_extraPaths.clear();
        }
        SpawnRefreshAsync();
    } else if (cmd == "saveDomains") {
        WriteFileUtf8(GetCustomDomainsPath(), JsonGetString(jsonMsg, "content"));
        SendLogToUI("OK", "已保存自定义域名清单", false);
        SpawnRefreshAsync();
    } else if (cmd == "locateFile") {
        std::string p = JsonGetString(jsonMsg, "path");
        if (!p.empty()) {
            NSURL* u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:p.c_str()]];
            [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[u]];
        }
    } else if (cmd == "openHostsDir") {
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:@"/etc"]];
    } else if (cmd == "openStateDir") {
        fs::path sd = GetStateDir();
        if (!fs::exists(sd)) sd = GetUserSupportDir();
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:sd.c_str()]]];
    } else if (cmd == "openUrl") {
        std::string url = JsonGetString(jsonMsg, "url");
        if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) {
            NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
            if (u) [[NSWorkspace sharedWorkspace] openURL:u];
        }
    } else if (cmd == "selftest") {
        std::thread([]() {
            SendLogToUI("STEP", "开始运行内置 18 项 Hosts 与自动更新引擎自检：", false);
            RunSelfTestEngine(SendLogToUI);
        }).detach();
    } else if (cmd == "checkUpdate") {
        CheckForUpdatesMacAsync(JsonGetBool(jsonMsg, "silent", false));
    } else if (cmd == "downloadUpdate") {
        [self startMacUpdateDownload];
    } else if (cmd == "cancelUpdate") {
        if (self.activeDownloadTask) {
            [self.activeDownloadTask cancel];
            self.activeDownloadTask = nil;
        }
    } else if (cmd == "setUpdateConfig") {
        UpdateConfig cfg = LoadUpdateConfig();
        if (jsonMsg.find("\"autoCheck\"") != std::string::npos) {
            cfg.autoCheck = JsonGetBool(jsonMsg, "autoCheck", cfg.autoCheck);
        }
        if (jsonMsg.find("\"ignoredVersion\"") != std::string::npos) {
            cfg.ignoredVersion = JsonGetString(jsonMsg, "ignoredVersion");
        }
        SaveUpdateConfig(cfg);
        MacReleaseInfo rel;
        {
            std::lock_guard<std::mutex> lock(g_updateMutex);
            rel = g_cachedRelease;
        }
        std::string st = (!rel.version.empty() && CompareSemVer(kAppVersion, rel.version) < 0) ? "available" : "idle";
        PostUpdateStateMac(st, true, rel, 0, 0, 0, 0, "", cfg);
    } else if (cmd == "apply") {
        ExecuteMacApply(JsonGetBool(jsonMsg, "skipHosts", false), JsonGetBool(jsonMsg, "skipServices", false));
    } else if (cmd == "revert") {
        ExecuteMacRevert();
    }
}

- (void)startMacUpdateDownload {
    if (g_updateBusy.exchange(true)) return;
    MacReleaseInfo rel;
    {
        std::lock_guard<std::mutex> lock(g_updateMutex);
        rel = g_cachedRelease;
    }
    UpdateConfig cfg = LoadUpdateConfig();
    if (rel.assetUrl.empty()) {
        PostUpdateStateMac("error", false, rel, 0, 0, 0, 0, "未找到 macOS 更新安装包，请前往 Releases 页面下载", cfg);
        g_updateBusy = false;
        return;
    }
    PostUpdateStateMac("downloading", false, rel, 0, 0, rel.assetSize, 0, "正在下载 " + rel.assetName + "...", cfg);
    SendLogToUI("STEP", "开始下载 macOS 更新包：" + rel.assetName, false);

    self.downloadStartTime = CFAbsoluteTimeGetCurrent();
    NSURLSessionConfiguration* scfg = [NSURLSessionConfiguration defaultSessionConfiguration];
    NSURLSession* session = [NSURLSession sessionWithConfiguration:scfg delegate:self delegateQueue:nil];
    NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:rel.assetUrl.c_str()]];
    self.activeDownloadTask = [session downloadTaskWithURL:u];
    [self.activeDownloadTask resume];
}

- (void)URLSession:(NSURLSession*)session
      downloadTask:(NSURLSessionDownloadTask*)downloadTask
      didWriteData:(int64_t)bytesWritten
 totalBytesWritten:(int64_t)totalBytesWritten
totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite {
    MacReleaseInfo rel;
    {
        std::lock_guard<std::mutex> lock(g_updateMutex);
        rel = g_cachedRelease;
    }
    int64_t tot = totalBytesExpectedToWrite > 0 ? totalBytesExpectedToWrite : rel.assetSize;
    double elapsed = std::max(0.05, CFAbsoluteTimeGetCurrent() - self.downloadStartTime);
    int64_t speed = (int64_t)((double)totalBytesWritten / elapsed);
    int pct = tot > 0 ? (int)std::min<int64_t>(99, (totalBytesWritten * 100) / tot) : 50;
    PostUpdateStateMac("downloading", false, rel, pct, totalBytesWritten, tot, speed,
                       "正在下载 " + rel.assetName + "...", LoadUpdateConfig());
}

- (void)URLSession:(NSURLSession*)session
      downloadTask:(NSURLSessionDownloadTask*)downloadTask
didFinishDownloadingToURL:(NSURL*)location {
    MacReleaseInfo rel;
    {
        std::lock_guard<std::mutex> lock(g_updateMutex);
        rel = g_cachedRelease;
    }
    UpdateConfig cfg = LoadUpdateConfig();
    PostUpdateStateMac("verifying", false, rel, 100, rel.assetSize, rel.assetSize, 0, "正在校验下载的安装包...", cfg);

    fs::path destDir = GetHomeDir() / "Downloads";
    std::error_code ec;
    fs::create_directories(destDir, ec);
    fs::path destPath = destDir / rel.assetName;
    fs::remove(destPath, ec);

    NSError* moveErr = nil;
    NSURL* destUrl = [NSURL fileURLWithPath:[NSString stringWithUTF8String:destPath.c_str()]];
    if (![[NSFileManager defaultManager] copyItemAtURL:location toURL:destUrl error:&moveErr]) {
        std::string errMsg = moveErr ? std::string([moveErr.localizedDescription UTF8String]) : "保存安装包失败";
        PostUpdateStateMac("error", false, rel, 0, 0, 0, 0, errMsg, cfg);
        self.activeDownloadTask = nil;
        g_updateBusy = false;
        return;
    }

    if (!rel.checksumAssetUrl.empty()) {
        NSURL* sumUrl = [NSURL URLWithString:[NSString stringWithUTF8String:rel.checksumAssetUrl.c_str()]];
        NSData* sumData = [NSData dataWithContentsOfURL:sumUrl];
        if (sumData) {
            std::string sumStr((const char*)sumData.bytes, sumData.length);
            std::string expected = ParseExpectedSha256(sumStr, rel.assetName);
            if (!expected.empty()) {
                std::string actual;
                if (!ComputeFileSha256Hex(destPath, actual) || actual != expected) {
                    fs::remove(destPath, ec);
                    PostUpdateStateMac("error", false, rel, 0, 0, 0, 0, "SHA-256 完整性校验失败，已删除安装包", cfg);
                    self.activeDownloadTask = nil;
                    g_updateBusy = false;
                    return;
                }
            }
        }
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSWorkspace sharedWorkspace] openURL:destUrl];
    });
    PostUpdateStateMac("ready_to_install", false, rel, 100, rel.assetSize, rel.assetSize, 0,
                       "已下载至 ~/Downloads/" + rel.assetName + " 并为您自动打开安装程序", cfg);
    SendLogToUI("DONE", "macOS 更新包已下载并打开：" + destPath.string(), true);
    self.activeDownloadTask = nil;
    g_updateBusy = false;
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    if (!error) return;
    MacReleaseInfo rel;
    {
        std::lock_guard<std::mutex> lock(g_updateMutex);
        rel = g_cachedRelease;
    }
    UpdateConfig cfg = LoadUpdateConfig();
    if (error.code == NSURLErrorCancelled) {
        PostUpdateStateMac("available", false, rel, 0, 0, 0, 0, "已取消下载更新", cfg);
    } else {
        std::string errMsg = [error.localizedDescription UTF8String];
        PostUpdateStateMac("error", false, rel, 0, 0, 0, 0, "下载失败：" + errMsg, cfg);
    }
    self.activeDownloadTask = nil;
    g_updateBusy = false;
}

@end

int main(int argc, const char* argv[]) {
    if (argc >= 2 && std::string(argv[1]) == "selftest") {
        int fails = RunSelfTestEngine([](const std::string& tag, const std::string& text, bool) {
            std::cout << "  " << tag << "  " << text << "\n";
        });
        return fails == 0 ? 0 : 1;
    }

    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        g_appCtrl = [[AdobeToolsController alloc] init];
        app.delegate = g_appCtrl;
        [app run];
    }
    return 0;
}
