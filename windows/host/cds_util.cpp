// cds_util.cpp -- see cds_util.h.

#include "cds_util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include <windows.h>
#include <bcrypt.h>

namespace fs = std::filesystem;

namespace winapk::host::cds {

namespace {

bool starts(const std::string& s, const char* prefix) { return s.compare(0, std::strlen(prefix), prefix) == 0; }

}  // namespace

std::string patch_dir_digest(const fs::path& dir) {
    std::error_code ec;
    std::vector<std::string> rel;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) rel.push_back(fs::relative(it->path(), dir, ec).generic_u8string());
    }
    if (ec || rel.empty()) return {};
    std::sort(rel.begin(), rel.end());  // byte order, as LC_ALL=C sort
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    std::string out;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        bool ok = true;
        std::vector<char> buf(1 << 16);
        for (const std::string& r : rel) {
            const fs::path full = dir / fs::u8path(r);
            std::ifstream in(full, std::ios::binary);
            const std::string head = r + "\n" + std::to_string(fs::file_size(full, ec)) + "\n";
            ok = ok && in && !ec &&
                 BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(head.data())),
                                static_cast<ULONG>(head.size()), 0) == 0;
            while (ok && (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0))
                ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(in.gcount()), 0) == 0;
        }
        unsigned char md[32];
        if (ok && BCryptFinishHash(hash, md, sizeof md, 0) == 0) {
            char two[3];
            for (unsigned char b : md) {
                std::snprintf(two, sizeof two, "%02x", b);
                out += two;
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

std::vector<std::pair<std::string, std::string>> read_baked(const fs::path& file) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream words(line);
        std::string mod, dig;
        if (words >> mod >> dig) out.emplace_back(mod, dig);
    }
    return out;
}

std::vector<std::string> drop_baked_patches(std::vector<std::string>* opts,
                                            const std::vector<std::pair<std::string, std::string>>& baked) {
    std::vector<std::string> kept, blocked;
    for (const std::string& o : *opts) {
        static const char kPm[] = "--patch-module=";
        if (!starts(o, kPm)) {
            kept.push_back(o);
            continue;
        }
        const std::string spec = o.substr(sizeof kPm - 1);  // <module>=<dir>
        const size_t eq = spec.find('=');
        const std::string mod = spec.substr(0, eq);
        bool ok = false;
        if (eq != std::string::npos) {
            for (const auto& b : baked) {
                if (b.first != mod) continue;
                const std::string dig = patch_dir_digest(fs::u8path(spec.substr(eq + 1)));
                ok = !dig.empty() && dig == b.second;
                break;
            }
        }
        if (!ok) {
            blocked.push_back(mod);
            kept.push_back(o);
        }
    }
    opts->swap(kept);
    return blocked;
}

std::string key_of(const std::vector<std::string>& parts) {
    uint64_t h = 1469598103934665603ULL;
    for (const std::string& s : parts) {
        for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;
        h = (h ^ 0xff) * 1099511628211ULL;  // a separator, so "ab"+"c" != "a"+"bc"
    }
    char hex[20];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
    return hex;
}

std::string stamp(const fs::path& p) {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec) return "-";
    const auto t = fs::last_write_time(p, ec);
    if (ec) return "-";
    return std::to_string(size) + ":" + std::to_string(static_cast<long long>(t.time_since_epoch().count()));
}

bool per_launch(const std::string& o) {
    if (starts(o, "-Djava.class.path=")) return false;
    return starts(o, "-D") || starts(o, "-XX:ErrorFile=") || starts(o, "-Xlog:");
}

std::string merge_classlists(const std::vector<std::string>& lists) {
    std::string out;
    std::unordered_set<std::string> seen;
    for (const std::string& text : lists) {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            // Another loader's class (an "unregistered" one) names its super
            // and source by the ids of its own file; the framework's are the
            // built-in loaders' and need neither.
            if (line[0] != '@' && (line.find(" super: ") != std::string::npos ||
                                   line.find(" source: ") != std::string::npos))
                continue;
            if (line[0] != '@') {
                const size_t id = line.find(" id: ");
                if (id != std::string::npos) line.erase(id);
            }
            if (seen.insert(line).second) out += line + "\n";
        }
    }
    return out;
}

}  // namespace winapk::host::cds
