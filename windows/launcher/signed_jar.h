// signed_jar.h -- a signed JAR as F-Droid publishes its index: index-v1.jar holds index-v1.json, signed with
// jarsigner by the repository's key. The store reads the index only out of a jar whose signature verifies and
// whose signer's certificate is the one the store pins, as F-Droid's own client does (IndexV1Updater: the jar's
// signer must be the repository's fingerprint); the APKs it then lists are checked by their SHA-256 as before.
//
// The JAR rules kept here (java.util.jar's, strictly):
//   - the jar holds META-INF/MANIFEST.MF, one META-INF/<X>.SF, its META-INF/<X>.RSA (or .DSA, .EC) and the one
//     file wanted, and nothing else: no second signer, no file the signature does not cover;
//   - <X>.RSA is a PKCS#7 signature over <X>.SF's bytes, by the pinned certificate;
//   - <X>.SF's main section has <alg>-Digest-Manifest, the digest of all of MANIFEST.MF;
//   - MANIFEST.MF's section for the file has <alg>-Digest, the digest of the file's bytes.
// <alg> is SHA-256 or SHA1, SHA-256 preferred where both are given (F-Droid's index-v1.jar has SHA1 only).
//
// The parts with no Windows call (the zip's directory, the manifests, the layout) are tested by
// test/launcher_state_test.cpp; verify_signed_jar(), under _WIN32, does the crypto with BCrypt and crypt32 and
// the inflating with zlib.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace winapk::launcher {

struct ZipEntry {
    std::string name;
    uint16_t flags = 0, method = 0;  // method: 0 stored, 8 deflated
    uint32_t crc = 0;
    uint64_t csize = 0, usize = 0, local = 0;  // compressed and real sizes, the local header's offset
};

namespace zip_detail {
inline uint16_t u16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline uint32_t u32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
}  // namespace zip_detail

// The zip's central directory. False when it is not a zip this reads: no end record, an entry past the end,
// zip64, a multi-disk archive.
inline bool zip_entries(const std::string& zip, std::vector<ZipEntry>* out) {
    using namespace zip_detail;
    out->clear();
    const auto* d = reinterpret_cast<const unsigned char*>(zip.data());
    const size_t n = zip.size();
    if (n < 22) return false;
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 22 + 65535;) {
        if (u32(d + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) return false;
    if (u16(d + eocd + 4) != 0 || u16(d + eocd + 6) != 0) return false;  // one disk
    const uint16_t count = u16(d + eocd + 10);
    const uint32_t dir_size = u32(d + eocd + 12), dir_at = u32(d + eocd + 16);
    if (count == 0xFFFF || dir_size == 0xFFFFFFFF || dir_at == 0xFFFFFFFF) return false;  // zip64
    if (static_cast<uint64_t>(dir_at) + dir_size > eocd) return false;
    size_t p = dir_at;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > eocd || u32(d + p) != 0x02014b50) return false;
        ZipEntry e;
        e.flags = u16(d + p + 8);
        e.method = u16(d + p + 10);
        e.crc = u32(d + p + 16);
        e.csize = u32(d + p + 20);
        e.usize = u32(d + p + 24);
        const uint16_t name_len = u16(d + p + 28), extra_len = u16(d + p + 30), comment_len = u16(d + p + 32);
        e.local = u32(d + p + 42);
        if (e.csize == 0xFFFFFFFF || e.usize == 0xFFFFFFFF || e.local == 0xFFFFFFFF) return false;
        if (p + 46 + name_len + extra_len + comment_len > eocd) return false;
        e.name.assign(zip.data() + p + 46, name_len);
        out->push_back(std::move(e));
        p += 46 + name_len + extra_len + comment_len;
    }
    return true;
}

// Where an entry's data starts, from its local header; 0 when the header is not one or the data runs past the end.
inline size_t zip_data_at(const std::string& zip, const ZipEntry& e) {
    using namespace zip_detail;
    const auto* d = reinterpret_cast<const unsigned char*>(zip.data());
    if (e.local + 30 > zip.size() || u32(d + e.local) != 0x04034b50) return 0;
    const uint16_t name_len = u16(d + e.local + 26), extra_len = u16(d + e.local + 28);
    if (std::string(zip.data() + e.local + 30, (std::min<size_t>)(name_len, zip.size() - e.local - 30)) != e.name) {
        return 0;  // the local header names another file than the directory does
    }
    const uint64_t at = e.local + 30 + name_len + extra_len;
    return at + e.csize <= zip.size() ? static_cast<size_t>(at) : 0;
}

// A manifest's sections (MANIFEST.MF, <X>.SF): the main one under "", the others under their Name. Lines end in
// CRLF, LF or CR; a line that starts with a space continues the one before. Empty when two sections share a name.
inline std::map<std::string, std::map<std::string, std::string>> manifest_sections(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : '\n';
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            if (!cur.empty() && cur[0] == ' ' && !lines.empty() && !lines.back().empty()) lines.back() += cur.substr(1);
            else lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::map<std::string, std::map<std::string, std::string>> out;
    std::map<std::string, std::string> attrs;
    bool in_section = false;
    bool dup = false;
    auto close = [&] {
        if (!in_section) return;
        const auto named = attrs.find("Name");
        if (!out.emplace(named == attrs.end() ? std::string() : named->second, attrs).second) dup = true;
        attrs.clear();
        in_section = false;
    };
    for (const std::string& line : lines) {
        if (line.empty()) {
            close();
            continue;
        }
        const size_t colon = line.find(": ");
        if (colon == std::string::npos || colon == 0) {
            dup = true;  // not "Name: value"
            continue;
        }
        in_section = true;
        attrs[line.substr(0, colon)] = line.substr(colon + 2);
    }
    close();
    if (dup) out.clear();
    return out;
}

// A digest a manifest gives: its algorithm ("SHA-256" or "SHA1") and its base64 value.
struct ManifestDigest {
    std::string alg, base64;
};

// `attrs`' "<alg><suffix>" for suffix "-Digest" or "-Digest-Manifest": SHA-256 when given, else SHA1, else none.
inline bool pick_digest(const std::map<std::string, std::string>& attrs, const char* suffix, ManifestDigest* out) {
    for (const char* alg : {"SHA-256", "SHA1"}) {
        const auto it = attrs.find(std::string(alg) + suffix);
        if (it != attrs.end() && !it->second.empty()) {
            *out = {alg, it->second};
            return true;
        }
    }
    return false;
}

// The jar's entries by role, as indexes into the directory.
struct JarLayout {
    int manifest = -1, sf = -1, signature = -1, payload = -1;
};

// Whether the jar holds exactly MANIFEST.MF, one signer's .SF and its signature block, and `payload`, each once.
inline bool jar_layout(const std::vector<ZipEntry>& entries, const std::string& payload, JarLayout* out) {
    *out = {};
    std::string sf_stem, sig_stem;
    for (size_t i = 0; i < entries.size(); ++i) {
        const std::string& n = entries[i].name;
        const int idx = static_cast<int>(i);
        auto once = [&](int* slot) {
            if (*slot >= 0) return false;
            *slot = idx;
            return true;
        };
        auto ends = [&n](const char* ext) {
            const size_t l = strlen(ext);
            return n.size() > l && n.compare(n.size() - l, l, ext) == 0;
        };
        const bool meta = n.compare(0, 9, "META-INF/") == 0 && n.find('/', 9) == std::string::npos && n.size() > 9;
        if (n == payload) {
            if (!once(&out->payload)) return false;
        } else if (n == "META-INF/MANIFEST.MF") {
            if (!once(&out->manifest)) return false;
        } else if (meta && ends(".SF")) {
            if (!once(&out->sf)) return false;
            sf_stem = n.substr(9, n.size() - 9 - 3);
        } else if (meta && (ends(".RSA") || ends(".DSA") || ends(".EC"))) {
            if (!once(&out->signature)) return false;
            sig_stem = n.substr(9, n.rfind('.') - 9);
        } else {
            return false;  // anything else: a file the signature may not cover, or a second signer
        }
    }
    return out->manifest >= 0 && out->sf >= 0 && out->signature >= 0 && out->payload >= 0 && !sf_stem.empty() &&
           sf_stem == sig_stem;
}

}  // namespace winapk::launcher

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <zlib.h>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace winapk::launcher {

// A running digest with BCrypt: "SHA-256" or "SHA1", as a manifest names them.
class JarHasher {
public:
    explicit JarHasher(const std::string& alg) {
        const wchar_t* id = alg == "SHA-256" ? BCRYPT_SHA256_ALGORITHM : alg == "SHA1" ? BCRYPT_SHA1_ALGORITHM : nullptr;
        if (id && BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, id, nullptr, 0)) &&
            BCRYPT_SUCCESS(BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0))) {
            ok_ = true;
        }
    }
    ~JarHasher() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    JarHasher(const JarHasher&) = delete;
    JarHasher& operator=(const JarHasher&) = delete;
    void add(const void* p, size_t n) {
        if (ok_ && n) ok_ = BCRYPT_SUCCESS(BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(p)),
                                                          static_cast<ULONG>(n), 0));
    }
    // The digest as base64, as manifests write it; empty on failure.
    std::string base64() {
        DWORD len = 0, got = 0;
        if (!ok_ || !BCRYPT_SUCCESS(BCryptGetProperty(alg_, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&len),
                                                      sizeof(len), &got, 0))) {
            return {};
        }
        std::vector<UCHAR> d(len);
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash_, d.data(), len, 0))) return {};
        ok_ = false;
        DWORD chars = 0;
        if (!CryptBinaryToStringA(d.data(), len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &chars)) return {};
        std::string out(chars, '\0');
        if (!CryptBinaryToStringA(d.data(), len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &chars)) {
            return {};
        }
        out.resize(chars);
        return out;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    bool ok_ = false;
};

namespace jar_detail {
// An entry's bytes, through `sink` in pieces, inflated when deflated; false when they do not inflate whole or
// their CRC-32 or size is not the directory's.
template <class Sink>
bool read_entry(const std::string& zip, const ZipEntry& e, Sink sink) {
    const size_t at = zip_data_at(zip, e);
    if (!at || (e.flags & 1)) return false;  // missing, or encrypted
    const auto* src = reinterpret_cast<const Bytef*>(zip.data() + at);
    uLong crc = crc32(0, nullptr, 0);
    uint64_t total = 0;
    if (e.method == 0) {
        if (e.csize != e.usize) return false;
        crc = crc32_z(crc, src, static_cast<z_size_t>(e.csize));
        if (!sink(zip.data() + at, static_cast<size_t>(e.csize))) return false;
        total = e.csize;
    } else if (e.method == 8) {
        z_stream z{};
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
        std::vector<unsigned char> buf(1 << 16);
        uint64_t left = e.csize;
        int r = Z_OK;
        const Bytef* in = src;
        while (r != Z_STREAM_END) {
            if (z.avail_in == 0 && left) {
                const uInt take = static_cast<uInt>((std::min<uint64_t>)(left, 1u << 30));
                z.next_in = const_cast<Bytef*>(in);
                z.avail_in = take;
                in += take;
                left -= take;
            }
            z.next_out = buf.data();
            z.avail_out = static_cast<uInt>(buf.size());
            r = inflate(&z, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END) break;
            const size_t got = buf.size() - z.avail_out;
            if (got == 0 && r == Z_OK && z.avail_in == 0 && left == 0) break;  // cut off
            total += got;
            if (total > e.usize) break;
            crc = crc32_z(crc, buf.data(), got);
            if (got && !sink(reinterpret_cast<const char*>(buf.data()), got)) break;
        }
        inflateEnd(&z);
        if (r != Z_STREAM_END) return false;
    } else {
        return false;
    }
    return total == e.usize && crc == e.crc;
}

inline bool entry_text(const std::string& zip, const ZipEntry& e, std::string* out, size_t max = 1 << 20) {
    out->clear();
    if (e.usize > max) return false;
    return read_entry(zip, e, [out](const char* p, size_t n) {
        out->append(p, n);
        return true;
    });
}
}  // namespace jar_detail

// Verifies `jar` as above, signed by the certificate whose SHA-256 is `signer_sha256` (lower-case hex), and
// writes its `payload` to `out` (through <out>.part, so that a half-written file never looks whole). False, with
// nothing written, when anything does not hold; `why` then says what, for the log.
inline bool verify_signed_jar(const std::filesystem::path& jar, const std::string& payload,
                              const std::string& signer_sha256, const std::filesystem::path& out, std::string* why) {
    using namespace jar_detail;
    auto fail = [why](const char* w) {
        if (why) *why = w;
        return false;
    };
    std::string zip;
    {
        std::ifstream f(jar, std::ios::binary);
        zip.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::vector<ZipEntry> entries;
    JarLayout l;
    if (!zip_entries(zip, &entries)) return fail("not a zip");
    if (!jar_layout(entries, payload, &l)) return fail("not one signer, its manifest and the file");
    std::string manifest, sf, sig;
    if (!entry_text(zip, entries[l.manifest], &manifest) || !entry_text(zip, entries[l.sf], &sf) ||
        !entry_text(zip, entries[l.signature], &sig)) {
        return fail("the signature's files do not read");
    }

    // The signature block over the .SF, and who made it.
    CRYPT_VERIFY_MESSAGE_PARA para{};
    para.cbSize = sizeof(para);
    para.dwMsgAndCertEncodingType = PKCS_7_ASN_ENCODING | X509_ASN_ENCODING;
    const BYTE* parts[] = {reinterpret_cast<const BYTE*>(sf.data())};
    DWORD part_sizes[] = {static_cast<DWORD>(sf.size())};
    PCCERT_CONTEXT cert = nullptr;
    if (!CryptVerifyDetachedMessageSignature(&para, 0, reinterpret_cast<const BYTE*>(sig.data()),
                                             static_cast<DWORD>(sig.size()), 1, parts, part_sizes, &cert) ||
        !cert) {
        if (cert) CertFreeCertificateContext(cert);
        return fail("the signature does not verify");
    }
    std::string fingerprint;
    {
        BYTE h[32];
        DWORD hn = sizeof(h);
        if (CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr, cert->pbCertEncoded, cert->cbCertEncoded, h,
                                  &hn) &&
            hn == 32) {
            static const char kHex[] = "0123456789abcdef";
            for (BYTE b : h) fingerprint += {kHex[b >> 4], kHex[b & 15]};
        }
    }
    CertFreeCertificateContext(cert);
    if (fingerprint.empty() || fingerprint != signer_sha256) return fail("signed by another key than the pinned one");

    // The .SF over the whole manifest, and the manifest over the file.
    const auto sf_sections = manifest_sections(sf), mf_sections = manifest_sections(manifest);
    ManifestDigest of_manifest, of_payload;
    const auto sf_main = sf_sections.find("");
    const auto mf_entry = mf_sections.find(payload);
    if (sf_main == sf_sections.end() || !pick_digest(sf_main->second, "-Digest-Manifest", &of_manifest)) {
        return fail("the .SF has no digest of the manifest");
    }
    if (mf_entry == mf_sections.end() || !pick_digest(mf_entry->second, "-Digest", &of_payload)) {
        return fail("the manifest has no digest of the file");
    }
    {
        JarHasher h(of_manifest.alg);
        h.add(manifest.data(), manifest.size());
        if (h.base64() != of_manifest.base64) return fail("the manifest is not the one signed");
    }

    namespace fs = std::filesystem;
    const fs::path part = out.wstring() + L".part";
    std::error_code ec;
    bool ok;
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        JarHasher h(of_payload.alg);
        ok = f && read_entry(zip, entries[l.payload], [&](const char* p, size_t n) {
            h.add(p, n);
            f.write(p, static_cast<std::streamsize>(n));
            return static_cast<bool>(f);
        });
        f.close();
        if (!ok) {
            fs::remove(part, ec);
            return fail("the file does not inflate whole");
        }
        if (h.base64() != of_payload.base64) {
            fs::remove(part, ec);
            return fail("the file is not the one signed");
        }
    }
    if (!MoveFileExW(part.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        fs::remove(part, ec);
        return fail("the file could not be kept");
    }
    return true;
}

}  // namespace winapk::launcher
#endif  // _WIN32
