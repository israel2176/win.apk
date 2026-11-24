// apk_icon.cpp -- see apk_icon.h. The formats are AOSP's (frameworks/base/libs/androidfw/include/androidfw/
// ResourceTypes.h: ResChunk_header, ResStringPool, ResXMLTree, ResTable_package, ResTable_type,
// ResTable_config, ResTable_entry, Res_value) and PKWARE's zip (APPNOTE.TXT, with zip64).
#include "apk_icon.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <functional>

namespace winapk::shell {
namespace {

// Limits: a damaged or hostile file costs at most this much memory and time.
constexpr uint64_t kMaxDirectory = 64ull << 20;     // a zip's central directory
constexpr size_t kMaxEntries = 300000;
constexpr uint64_t kMaxManifest = 8ull << 20;
constexpr uint64_t kMaxTable = 96ull << 20;         // resources.arsc
constexpr uint64_t kMaxImage = 16ull << 20;         // one icon picture
constexpr uint64_t kMaxXml = 1ull << 20;            // an icon's XML drawable
constexpr uint64_t kMaxNestedInflate = 192ull << 20;  // a deflated base APK inside a bundle
constexpr int kMaxSplits = 24;                      // density splits looked into
constexpr int kMaxDepth = 8;                        // references followed

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t u64(const uint8_t* p) { return u32(p) | (static_cast<uint64_t>(u32(p + 4)) << 32); }

bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[s.size() - n + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != suffix[i]) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ResStringPool

class StringPool {
public:
    // The chunk at data[off, off + size).
    bool parse(const uint8_t* data, size_t size) {
        if (size < 28 || u16(data) != 0x0001) return false;
        const uint32_t header = u16(data + 2), chunk = u32(data + 4);
        if (header < 28 || chunk > size || header > chunk) return false;
        count_ = u32(data + 8);
        flags_ = u32(data + 16);
        const uint32_t strings = u32(data + 20);
        if (count_ > (chunk - header) / 4 || strings > chunk) return false;
        data_ = data;
        size_ = chunk;
        offsets_ = data + header;
        strings_ = strings;
        return true;
    }
    uint32_t count() const { return count_; }
    std::string get(uint32_t i) const {
        if (i >= count_) return {};
        const uint64_t at = static_cast<uint64_t>(strings_) + u32(offsets_ + 4ull * i);
        if (at >= size_) return {};
        const uint8_t* p = data_ + at;
        const uint8_t* end = data_ + size_;
        if (flags_ & 0x100) {  // UTF-8: the length in UTF-16 units, then in bytes, then the bytes
            auto len8 = [&](const uint8_t*& q, size_t* n) {
                if (q >= end) return false;
                *n = *q++;
                if (*n & 0x80) {
                    if (q >= end) return false;
                    *n = ((*n & 0x7f) << 8) | *q++;
                }
                return true;
            };
            size_t chars = 0, bytes = 0;
            if (!len8(p, &chars) || !len8(p, &bytes) || bytes > static_cast<size_t>(end - p)) return {};
            return std::string(reinterpret_cast<const char*>(p), bytes);
        }
        if (end - p < 2) return {};
        size_t n = u16(p);
        p += 2;
        if (n & 0x8000) {
            if (end - p < 2) return {};
            n = ((n & 0x7fff) << 16) | u16(p);
            p += 2;
        }
        if (n > static_cast<size_t>(end - p) / 2) return {};
        std::string out;  // UTF-16 to UTF-8 (paths and package names are ASCII; the rest is kept whole)
        for (size_t i = 0; i < n; ++i) {
            uint32_t c = u16(p + 2 * i);
            if (c >= 0xd800 && c < 0xdc00 && i + 1 < n) {
                const uint32_t lo = u16(p + 2 * (i + 1));
                if (lo >= 0xdc00 && lo < 0xe000) {
                    c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                    ++i;
                }
            }
            if (c < 0x80) {
                out += static_cast<char>(c);
            } else if (c < 0x800) {
                out += static_cast<char>(0xc0 | (c >> 6));
                out += static_cast<char>(0x80 | (c & 0x3f));
            } else if (c < 0x10000) {
                out += static_cast<char>(0xe0 | (c >> 12));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
                out += static_cast<char>(0x80 | (c & 0x3f));
            } else {
                out += static_cast<char>(0xf0 | (c >> 18));
                out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
                out += static_cast<char>(0x80 | (c & 0x3f));
            }
        }
        return out;
    }

private:
    const uint8_t* data_ = nullptr;
    const uint8_t* offsets_ = nullptr;
    size_t size_ = 0;
    uint32_t count_ = 0, flags_ = 0, strings_ = 0;
};

// Res_value's types.
constexpr uint8_t kTypeReference = 0x01, kTypeString = 0x03, kTypeFirstColor = 0x1c, kTypeLastColor = 0x1f;

struct Value {
    uint8_t type = 0;
    uint32_t data = 0;
};

uint32_t color_of(const Value& v) {
    switch (v.type) {
    case 0x1c: return v.data;                // #aarrggbb
    case 0x1d: return 0xff000000u | v.data;  // #rrggbb
    case 0x1e: {                             // #argb
        const uint32_t a = (v.data >> 12) & 0xf, r = (v.data >> 8) & 0xf, g = (v.data >> 4) & 0xf, b = v.data & 0xf;
        return (a * 0x11) << 24 | (r * 0x11) << 16 | (g * 0x11) << 8 | b * 0x11;
    }
    default: {  // #rgb
        const uint32_t r = (v.data >> 8) & 0xf, g = (v.data >> 4) & 0xf, b = v.data & 0xf;
        return 0xff000000u | (r * 0x11) << 16 | (g * 0x11) << 8 | b * 0x11;
    }
    }
}

// ---------------------------------------------------------------------------
// Binary XML (ResXMLTree): each start tag, with its attributes.

struct XmlAttr {
    std::string name;
    uint32_t id = 0;  // the attribute's resource id (android:icon is 0x01010002), from the resource map
    std::string raw;
    Value value;
};

// Calls f(depth, tag, attributes) for each start tag; f returns false to stop.
bool walk_xml(const std::vector<uint8_t>& xml,
              const std::function<bool(int, const std::string&, const std::vector<XmlAttr>&)>& f) {
    const uint8_t* d = xml.data();
    const size_t n = xml.size();
    if (n < 8 || u16(d) != 0x0003) return false;
    const size_t end = std::min<size_t>(n, u32(d + 4));
    size_t at = u16(d + 2);
    StringPool pool;
    const uint8_t* ids = nullptr;
    size_t id_count = 0;
    int depth = 0;
    while (at + 8 <= end) {
        const uint16_t type = u16(d + at), header = u16(d + at + 2);
        const uint32_t size = u32(d + at + 4);
        if (size < 8 || size > end - at || header > size) return false;
        const uint8_t* c = d + at;
        if (type == 0x0001) {
            if (!pool.parse(c, size)) return false;
        } else if (type == 0x0180) {
            ids = c + header;
            id_count = (size - header) / 4;
        } else if (type == 0x0102) {  // a start tag: ResXMLTree_node, then ResXMLTree_attrExt
            if (header < 16 || size < header + 20u) return false;
            const uint8_t* ext = c + header;
            const std::string tag = pool.get(u32(ext + 4));
            const uint16_t attr_start = u16(ext + 8), attr_size = u16(ext + 10), attr_count = u16(ext + 12);
            if (attr_size < 20 || header + attr_start + static_cast<uint64_t>(attr_size) * attr_count > size)
                return false;
            std::vector<XmlAttr> attrs(attr_count);
            for (uint16_t i = 0; i < attr_count; ++i) {
                const uint8_t* a = ext + attr_start + static_cast<size_t>(attr_size) * i;
                const uint32_t name = u32(a + 4), raw = u32(a + 8);
                attrs[i].name = pool.get(name);
                if (ids != nullptr && name < id_count) attrs[i].id = u32(ids + 4ull * name);
                if (raw != 0xffffffffu) attrs[i].raw = pool.get(raw);
                attrs[i].value.type = a[15];
                attrs[i].value.data = u32(a + 16);
                if (attrs[i].value.type == kTypeString && attrs[i].raw.empty())
                    attrs[i].raw = pool.get(attrs[i].value.data);
            }
            if (!f(depth, tag, attrs)) return true;
            ++depth;
        } else if (type == 0x0103) {
            --depth;
        }
        at += size;
    }
    return true;
}

const XmlAttr* attr(const std::vector<XmlAttr>& attrs, uint32_t id, const char* name) {
    for (const XmlAttr& a : attrs)
        if (a.id == id) return &a;
    for (const XmlAttr& a : attrs)  // a tree without a resource map names them only
        if (a.id == 0 && a.name == name) return &a;
    return nullptr;
}

constexpr uint32_t kAttrIcon = 0x01010002, kAttrDrawable = 0x01010199, kAttrSrc = 0x01010119;

// ---------------------------------------------------------------------------
// resources.arsc: a resource's values, per configuration.

struct Candidate {
    Value value;
    uint16_t density = 0;  // ResTable_config's: 0 default (mdpi), 0xfffe anydpi, 0xffff nodpi
};

class ResTable {
public:
    bool parse(std::vector<uint8_t> bytes) {
        bytes_ = std::move(bytes);
        const uint8_t* d = bytes_.data();
        const size_t n = bytes_.size();
        if (n < 12 || u16(d) != 0x0002) return false;
        const size_t end = std::min<size_t>(n, u32(d + 4));
        size_t at = u16(d + 2);
        while (at + 8 <= end) {
            const uint16_t type = u16(d + at), header = u16(d + at + 2);
            const uint32_t size = u32(d + at + 4);
            if (size < 8 || size > end - at || header > size) return false;
            if (type == 0x0001 && !strings_ok_) {
                strings_ok_ = strings_.parse(d + at, size);
            } else if (type == 0x0200 && header >= 288) {
                packages_.push_back({static_cast<uint8_t>(u32(d + at + 8)), at, size, header});
            }
            at += size;
        }
        return strings_ok_ && !packages_.empty();
    }

    std::string string(uint32_t i) const { return strings_.get(i); }

    // Every value of resource id, skipping configurations a phone in English, by day, would not take
    // (another country or language, night mode, a television, car or watch).
    std::vector<Candidate> lookup(uint32_t id) const {
        std::vector<Candidate> out;
        const uint8_t pkg_id = static_cast<uint8_t>(id >> 24), type_id = static_cast<uint8_t>(id >> 16);
        const uint32_t entry = id & 0xffff;
        const uint8_t* d = bytes_.data();
        for (const Package& p : packages_) {
            if (p.id != pkg_id && !(packages_.size() == 1 && pkg_id == 0x7f)) continue;
            size_t at = p.offset + p.header;
            const size_t end = p.offset + p.size;
            while (at + 8 <= end) {
                const uint16_t type = u16(d + at), header = u16(d + at + 2);
                const uint32_t size = u32(d + at + 4);
                if (size < 8 || size > end - at || header > size) break;
                if (type == 0x0201 && header >= 20 + 16 && d[at + 8] == type_id) read_type(d + at, size, entry, &out);
                at += size;
            }
        }
        return out;
    }

private:
    struct Package {
        uint8_t id;
        size_t offset, size, header;
    };

    static void read_type(const uint8_t* c, uint32_t size, uint32_t entry, std::vector<Candidate>* out) {
        const uint16_t header = u16(c + 2);
        const uint8_t flags = c[9];
        const uint32_t count = u32(c + 12), entries = u32(c + 16);
        const uint8_t* config = c + 20;
        const uint32_t config_size = u32(config);
        if (config_size < 16 || 20ull + config_size > header) return;
        auto field = [&](uint32_t off, uint32_t width) -> uint32_t {
            if (off + width > config_size) return 0;
            return width == 1 ? config[off] : width == 2 ? u16(config + off) : u32(config + off);
        };
        if (field(4, 4) != 0 || field(8, 4) != 0) return;  // mcc/mnc, language/country
        const uint32_t ui_mode = field(29, 1);
        if ((ui_mode & 0x30) == 0x20) return;                          // night
        if ((ui_mode & 0x0f) > 1) return;                              // not a phone
        const uint16_t density = static_cast<uint16_t>(field(14, 2));
        if (entries > size) return;
        uint64_t offset = 0;
        bool found = false;
        if (flags & 0x01) {  // FLAG_SPARSE: (index, offset / 4) pairs, sorted by index
            if (header + 4ull * count > size) return;
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t* e = c + header + 4ull * i;
                if (u16(e) == entry) {
                    offset = 4ull * u16(e + 2);
                    found = true;
                    break;
                }
            }
        } else if (flags & 0x02) {  // FLAG_OFFSET16: offset / 4, 0xffff for none
            if (entry >= count || header + 2ull * count > size) return;
            const uint16_t o = u16(c + header + 2ull * entry);
            if (o != 0xffff) {
                offset = 4ull * o;
                found = true;
            }
        } else {
            if (entry >= count || header + 4ull * count > size) return;
            const uint32_t o = u32(c + header + 4ull * entry);
            if (o != 0xffffffffu) {
                offset = o;
                found = true;
            }
        }
        if (!found) return;
        const uint64_t at = entries + offset;
        if (at + 8 > size) return;
        const uint8_t* e = c + at;
        const uint16_t entry_flags = u16(e + 2);
        Candidate cand;
        cand.density = density;
        if (entry_flags & 0x0008) {  // FLAG_COMPACT: the value's type in the flags' high byte, its data after
            cand.value.type = static_cast<uint8_t>(entry_flags >> 8);
            cand.value.data = u32(e + 4);
        } else {
            if (entry_flags & 0x0001) return;  // a bag (style, array): not an icon
            const uint16_t entry_size = u16(e);
            if (at + entry_size + 8 > size) return;
            const uint8_t* v = e + entry_size;
            cand.value.type = v[3];
            cand.value.data = u32(v + 4);
        }
        out->push_back(cand);
    }

    std::vector<uint8_t> bytes_;
    StringPool strings_;
    bool strings_ok_ = false;
    std::vector<Package> packages_;
};

// ---------------------------------------------------------------------------
// The icon

// A density's rank: the larger, the better for a thumbnail (up to 256 px).
int density_rank(uint16_t d) {
    if (d == 0xffff) return 500;   // nodpi: one picture for all
    if (d == 0xfffe) return 1;     // anydpi: XML only
    if (d == 0) return 160;
    return d;
}

bool is_picture(const std::string& path) {
    return ends_with(path, ".png") || ends_with(path, ".webp") || ends_with(path, ".jpg") ||
           ends_with(path, ".jpeg");
}

// The framework's colors an icon's background names most (android:color/...): their values are fixed.
bool framework_color(uint32_t id, uint32_t* argb) {
    switch (id) {
    case 0x01060000: *argb = 0x00000000; return true;  // transparent
    case 0x0106000b: *argb = 0xffffffff; return true;  // white
    case 0x0106000c: *argb = 0xff000000; return true;  // black
    default: return false;
    }
}

class Apk {
public:
    // A file the base APK lacks: in a bundle, a density split's (no function elsewhere).
    using Elsewhere = std::function<bool(const std::string&, std::vector<uint8_t>*, uint64_t)>;
    Apk(const Zip& base, Elsewhere elsewhere) : base_(base), elsewhere_(std::move(elsewhere)) {}

    bool load(ApkIcon* out) {
        const Zip::Entry* m = base_.find("AndroidManifest.xml");
        std::vector<uint8_t> manifest;
        if (m == nullptr || !base_.read(*m, &manifest, kMaxManifest)) return false;
        out->package = manifest_package(manifest);
        const uint32_t icon = manifest_icon(manifest);
        if (icon == 0) return false;
        const Zip::Entry* t = base_.find("resources.arsc");
        std::vector<uint8_t> table;
        if (t == nullptr || !base_.read(*t, &table, kMaxTable) || !table_.parse(std::move(table))) return false;
        return resolve_icon(icon, out);
    }

private:
    bool file(const std::string& path, std::vector<uint8_t>* out, uint64_t max) const {
        if (const Zip::Entry* e = base_.find(path)) return base_.read(*e, out, max);
        return elsewhere_ && elsewhere_(path, out, max);
    }

    // The icon resource: its best picture, else its adaptive icon.
    bool resolve_icon(uint32_t id, ApkIcon* out) {
        for (int depth = 0; depth < kMaxDepth; ++depth) {
            std::vector<Candidate> c = table_.lookup(id);
            if (c.empty()) return false;
            std::stable_sort(c.begin(), c.end(), [](const Candidate& a, const Candidate& b) {
                return density_rank(a.density) > density_rank(b.density);
            });
            // An alias (@mipmap/ic_launcher pointing at @drawable/...): follow it.
            if (c.front().value.type == kTypeReference) {
                id = c.front().value.data;
                continue;
            }
            for (const Candidate& k : c) {
                if (k.value.type != kTypeString) continue;
                const std::string path = table_.string(k.value.data);
                if (is_picture(path) && file(path, &out->icon.image, kMaxImage)) return true;
            }
            for (const Candidate& k : c) {
                if (k.value.type != kTypeString) continue;
                const std::string path = table_.string(k.value.data);
                if (ends_with(path, ".xml") && adaptive(path, out)) return true;
            }
            for (const Candidate& k : c) {
                if (k.value.type != kTypeString) continue;
                const std::string path = table_.string(k.value.data);
                if (ends_with(path, ".xml") && layer_from_xml(path, &out->icon, 0)) return true;
            }
            return false;
        }
        return false;
    }

    bool adaptive(const std::string& path, ApkIcon* out) {
        std::vector<uint8_t> xml;
        if (!file(path, &xml, kMaxXml)) return false;
        bool is_adaptive = false;
        Value bg, fg;
        walk_xml(xml, [&](int depth, const std::string& tag, const std::vector<XmlAttr>& attrs) {
            if (depth == 0) {
                is_adaptive = tag == "adaptive-icon";
                return is_adaptive;
            }
            if (depth == 1 && (tag == "background" || tag == "foreground")) {
                if (const XmlAttr* a = attr(attrs, kAttrDrawable, "drawable")) (tag == "background" ? bg : fg) = a->value;
            }
            return true;
        });
        if (!is_adaptive) return false;
        // Both layers or nothing: a foreground on no background, or the reverse, is not the app's icon.
        Layer b, f;
        if (!layer(bg, &b, 0) || !layer(fg, &f, 0)) return false;
        out->adaptive = true;
        out->background = std::move(b);
        out->foreground = std::move(f);
        return true;
    }

    // A layer's value: a color, or a reference to a color or a picture.
    bool layer(const Value& v, Layer* out, int depth) {
        if (depth > kMaxDepth) return false;
        if (v.type >= kTypeFirstColor && v.type <= kTypeLastColor) {
            out->color = true;
            out->argb = color_of(v);
            return true;
        }
        if (v.type != kTypeReference) return false;
        if (framework_color(v.data, &out->argb)) {
            out->color = true;
            return true;
        }
        std::vector<Candidate> c = table_.lookup(v.data);
        std::stable_sort(c.begin(), c.end(), [](const Candidate& a, const Candidate& b) {
            return density_rank(a.density) > density_rank(b.density);
        });
        for (const Candidate& k : c) {
            if (k.value.type == kTypeString) {
                const std::string path = table_.string(k.value.data);
                if (is_picture(path) && file(path, &out->image, kMaxImage)) return true;
                if (ends_with(path, ".xml") && layer_from_xml(path, out, depth + 1)) return true;
            } else if (layer(k.value, out, depth + 1)) {
                return true;
            }
        }
        return false;
    }

    // An XML drawable this code can show: <bitmap android:src>, <inset> with no inset of its own, and a
    // color resource's <selector> is not one. A <vector> is not drawn here.
    bool layer_from_xml(const std::string& path, Layer* out, int depth) {
        if (depth > kMaxDepth) return false;
        std::vector<uint8_t> xml;
        if (!file(path, &xml, kMaxXml)) return false;
        Value src;
        bool ok = false;
        walk_xml(xml, [&](int d, const std::string& tag, const std::vector<XmlAttr>& attrs) {
            if (d != 0) return false;
            if (tag == "bitmap") {
                if (const XmlAttr* a = attr(attrs, kAttrSrc, "src")) {
                    src = a->value;
                    ok = true;
                }
            }
            return false;
        });
        return ok && layer(src, out, depth + 1);
    }

    const Zip& base_;
    Elsewhere elsewhere_;
    ResTable table_;
};

std::vector<uint8_t> inflate_raw(const Source& s, uint64_t off, uint64_t compressed, uint64_t size, bool* ok) {
    *ok = false;
    std::vector<uint8_t> out;
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return out;
    out.resize(static_cast<size_t>(size));
    std::vector<uint8_t> in(64 * 1024);
    uint64_t left = compressed;
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        if (z.avail_in == 0) {
            if (left == 0) break;
            const size_t n = static_cast<size_t>(std::min<uint64_t>(left, in.size()));
            if (!s.read(off, in.data(), n)) break;
            off += n;
            left -= n;
            z.next_in = in.data();
            z.avail_in = static_cast<uInt>(n);
        }
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) break;
        if (z.avail_out == 0 && rc != Z_STREAM_END) {
            // The output is full: the stream must end now, with no byte more than the directory said.
            uint8_t probe = 0;
            z.next_out = &probe;
            z.avail_out = 1;
            rc = inflate(&z, Z_NO_FLUSH);
            if (rc != Z_STREAM_END || z.avail_out == 0) break;
        }
    }
    *ok = rc == Z_STREAM_END && z.total_out == size;
    inflateEnd(&z);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------

bool MemorySource::read(uint64_t off, void* dst, size_t n) const {
    if (off > bytes_.size() || n > bytes_.size() - off) return false;
    if (n != 0) std::memcpy(dst, bytes_.data() + off, n);
    return true;
}

bool SliceSource::read(uint64_t off, void* dst, size_t n) const {
    if (off > len_ || n > len_ - off) return false;
    return base_.read(off_ + off, dst, n);
}

bool Zip::open(const Source& source) {
    source_ = &source;
    entries_.clear();
    const uint64_t size = source.size();
    if (size < 22) return false;
    // The end of central directory record: in the last 64 KiB + 22 bytes (its comment comes after it).
    const uint64_t tail = std::min<uint64_t>(size, 65557);
    std::vector<uint8_t> t(static_cast<size_t>(tail));
    if (!source.read(size - tail, t.data(), t.size())) return false;
    int64_t eocd = -1;
    for (int64_t i = static_cast<int64_t>(tail) - 22; i >= 0; --i) {
        if (u32(&t[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) return false;
    uint64_t count = u16(&t[eocd + 10]), dir_size = u32(&t[eocd + 12]), dir_off = u32(&t[eocd + 16]);
    if (count == 0xffff || dir_size == 0xffffffffu || dir_off == 0xffffffffu) {
        // Zip64: the locator just before, naming the zip64 end record.
        const uint64_t at = size - tail + eocd;
        if (at < 20) return false;
        uint8_t loc[20];
        if (!source.read(at - 20, loc, 20) || u32(loc) != 0x07064b50) return false;
        const uint64_t rec_off = u64(loc + 8);
        uint8_t rec[56];
        if (rec_off > size || !source.read(rec_off, rec, 56) || u32(rec) != 0x06064b50) return false;
        count = u64(rec + 32);
        dir_size = u64(rec + 40);
        dir_off = u64(rec + 48);
    }
    if (dir_size > kMaxDirectory || count > kMaxEntries || dir_off > size || dir_size > size - dir_off) return false;
    std::vector<uint8_t> dir(static_cast<size_t>(dir_size));
    if (!source.read(dir_off, dir.data(), dir.size())) return false;
    size_t p = 0;
    entries_.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        if (p + 46 > dir.size() || u32(&dir[p]) != 0x02014b50) return false;
        const uint16_t name_len = u16(&dir[p + 28]), extra_len = u16(&dir[p + 30]), comment_len = u16(&dir[p + 32]);
        if (p + 46ull + name_len + extra_len + comment_len > dir.size()) return false;
        Entry e;
        e.method = u16(&dir[p + 10]);
        e.compressed = u32(&dir[p + 20]);
        e.size = u32(&dir[p + 24]);
        e.local = u32(&dir[p + 42]);
        e.name.assign(reinterpret_cast<const char*>(&dir[p + 46]), name_len);
        // Zip64's extra field (0x0001): the 64-bit values of those that read 0xffffffff, in this order.
        size_t x = p + 46 + name_len;
        const size_t x_end = x + extra_len;
        while (x + 4 <= x_end) {
            const uint16_t id = u16(&dir[x]), len = u16(&dir[x + 2]);
            if (x + 4 + len > x_end) break;
            if (id == 0x0001) {
                size_t q = x + 4;
                const size_t q_end = q + len;
                auto take = [&](uint64_t* v) {
                    if (*v != 0xffffffffu) return;
                    if (q + 8 <= q_end) *v = u64(&dir[q]);
                    q += 8;
                };
                take(&e.size);
                take(&e.compressed);
                take(&e.local);
            }
            x += 4 + len;
        }
        entries_.push_back(std::move(e));
        p += 46ull + name_len + extra_len + comment_len;
    }
    return true;
}

const Zip::Entry* Zip::find(const std::string& name) const {
    for (const Entry& e : entries_)
        if (e.name == name) return &e;
    return nullptr;
}

bool Zip::data_offset(const Entry& e, uint64_t* off) const {
    uint8_t h[30];
    if (e.local > source_->size() || !source_->read(e.local, h, 30) || u32(h) != 0x04034b50) return false;
    *off = e.local + 30 + u16(h + 26) + u16(h + 28);
    return *off <= source_->size() && e.compressed <= source_->size() - *off;
}

bool Zip::read(const Entry& e, std::vector<uint8_t>* out, uint64_t max) const {
    out->clear();
    if (e.size > max) return false;
    uint64_t off = 0;
    if (!data_offset(e, &off)) return false;
    if (e.method == 0) {
        if (e.compressed != e.size) return false;
        out->resize(static_cast<size_t>(e.size));
        return source_->read(off, out->data(), out->size());
    }
    if (e.method != 8) return false;
    bool ok = false;
    *out = inflate_raw(*source_, off, e.compressed, e.size, &ok);
    if (!ok) out->clear();
    return ok;
}

std::unique_ptr<Source> Zip::stored(const Entry& e) const {
    uint64_t off = 0;
    if (e.method != 0 || e.compressed != e.size || !data_offset(e, &off)) return nullptr;
    return std::make_unique<SliceSource>(*source_, off, e.size);
}

std::string manifest_package(const std::vector<uint8_t>& axml) {
    std::string package;
    walk_xml(axml, [&](int depth, const std::string& tag, const std::vector<XmlAttr>& attrs) {
        if (depth == 0 && tag == "manifest") {
            for (const XmlAttr& a : attrs)
                if (a.name == "package" && a.id == 0) package = a.raw;
        }
        return false;
    });
    return package;
}

uint32_t manifest_icon(const std::vector<uint8_t>& axml) {
    uint32_t icon = 0;
    walk_xml(axml, [&](int depth, const std::string& tag, const std::vector<XmlAttr>& attrs) {
        if (depth == 1 && tag == "application") {
            if (const XmlAttr* a = attr(attrs, kAttrIcon, "icon"))
                if (a->value.type == kTypeReference) icon = a->value.data;
            return false;
        }
        return true;
    });
    return icon;
}

bool find_icon(const Source& file, ApkIcon* out) {
    *out = ApkIcon();
    Zip zip;
    if (!zip.open(file)) return false;
    if (zip.find("AndroidManifest.xml") != nullptr) return Apk(zip, nullptr).load(out);

    // A bundle. Its own icon first (APKMirror's .apkm and .xapk carry the store's icon.png).
    for (const char* name : {"icon.png", "icon.webp"}) {
        if (const Zip::Entry* e = zip.find(name)) {
            if (zip.read(*e, &out->icon.image, kMaxImage)) return true;
        }
    }
    // The base APK: base.apk (.apkm, .xapk, SAI's .apks), splits/base-master.apk (bundletool's .apks),
    // else the package's own <package>.apk (.xapk), else the largest APK.
    const Zip::Entry* base = nullptr;
    std::vector<const Zip::Entry*> apks;
    for (const Zip::Entry& e : zip.entries()) {
        if (!ends_with(e.name, ".apk")) continue;
        apks.push_back(&e);
        const size_t slash = e.name.rfind('/');
        const std::string leaf = slash == std::string::npos ? e.name : e.name.substr(slash + 1);
        if (leaf == "base.apk" || leaf == "base-master.apk") base = &e;
    }
    if (base == nullptr) {
        for (const Zip::Entry* e : apks)
            if (e->name.find("split") == std::string::npos && e->name.find("config.") == std::string::npos &&
                (base == nullptr || e->size > base->size))
                base = e;
    }
    if (base == nullptr) return false;

    // A nested APK: read in place when stored (as bundles mostly keep them), else inflated into memory.
    struct Nested {
        std::unique_ptr<Source> source;
        Zip zip;
    };
    auto open_nested = [&](const Zip::Entry& e, Nested* n) {
        n->source = zip.stored(e);
        if (n->source == nullptr) {
            std::vector<uint8_t> bytes;
            if (!zip.read(e, &bytes, kMaxNestedInflate)) return false;
            n->source = std::make_unique<MemorySource>(std::move(bytes));
        }
        return n->zip.open(*n->source);
    };
    Nested b;
    if (!open_nested(*base, &b)) return false;
    // The density splits (split_config.xxhdpi.apk, base-xxhdpi.apk), largest density first: a density's
    // pictures live there and not in the base. Opened only when the base lacks a file, each once.
    std::vector<const Zip::Entry*> dpi;
    for (const Zip::Entry* e : apks)
        if (e != base && e->name.find("dpi") != std::string::npos) dpi.push_back(e);
    auto rank = [](const std::string& n) {
        static const char* const order[] = {"xxxhdpi", "xxhdpi", "xhdpi", "hdpi", "mdpi", "ldpi"};
        for (int i = 0; i < 6; ++i)
            if (n.find(order[i]) != std::string::npos) return i;
        return 6;
    };
    std::stable_sort(dpi.begin(), dpi.end(), [&](const Zip::Entry* a, const Zip::Entry* c) {
        return rank(a->name) < rank(c->name);
    });
    if (dpi.size() > static_cast<size_t>(kMaxSplits)) dpi.resize(kMaxSplits);
    std::vector<std::unique_ptr<Nested>> splits(dpi.size());
    std::vector<bool> tried(dpi.size(), false);
    auto elsewhere = [&](const std::string& path, std::vector<uint8_t>* bytes, uint64_t max) {
        for (size_t i = 0; i < dpi.size(); ++i) {
            if (!tried[i]) {
                tried[i] = true;
                auto n = std::make_unique<Nested>();
                if (open_nested(*dpi[i], n.get())) splits[i] = std::move(n);
            }
            if (splits[i] == nullptr) continue;
            if (const Zip::Entry* e = splits[i]->zip.find(path)) return splits[i]->zip.read(*e, bytes, max);
        }
        return false;
    };
    return Apk(b.zip, elsewhere).load(out);
}

}  // namespace winapk::shell
