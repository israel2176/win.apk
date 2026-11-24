// apk_icon_test.cpp -- windows/shell/apk_icon.cpp, the parsing behind Explorer's thumbnail of an app file.
//
//   apk_icon_test                    the built-in cases: a small APK made here (binary manifest, resource
//                                    table with a density per configuration, an alias, an adaptive icon), a
//                                    bundle around it, and damaged copies
//   apk_icon_test <file>...          also each file: its package and what icon was found (OK or NONE)
//
// No Windows call: g++ -std=c++17 -I windows/shell windows/test/apk_icon_test.cpp windows/shell/apk_icon.cpp -lz
#include "apk_icon.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using winapk::shell::ApkIcon;
using winapk::shell::MemorySource;
using Bytes = std::vector<uint8_t>;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

void put16(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(Bytes& b, uint32_t v) {
    put16(b, v & 0xffff);
    put16(b, v >> 16);
}
void set32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

// A UTF-8 ResStringPool.
Bytes pool(const std::vector<std::string>& strings) {
    Bytes data;
    std::vector<uint32_t> offsets;
    for (const std::string& s : strings) {
        offsets.push_back(static_cast<uint32_t>(data.size()));
        data.push_back(static_cast<uint8_t>(s.size()));
        data.push_back(static_cast<uint8_t>(s.size()));
        data.insert(data.end(), s.begin(), s.end());
        data.push_back(0);
    }
    while (data.size() % 4) data.push_back(0);
    Bytes b;
    put16(b, 0x0001);
    put16(b, 28);
    put32(b, 0);
    put32(b, static_cast<uint32_t>(strings.size()));
    put32(b, 0);
    put32(b, 0x100);
    put32(b, static_cast<uint32_t>(28 + 4 * strings.size()));
    put32(b, 0);
    for (uint32_t o : offsets) put32(b, o);
    b.insert(b.end(), data.begin(), data.end());
    set32(b, 4, static_cast<uint32_t>(b.size()));
    return b;
}

struct Attr {
    uint32_t name;  // string index, also the resource map's index
    uint8_t type;
    uint32_t data;
    uint32_t raw = 0xffffffffu;
};

// A ResXMLTree: strings, a resource map for the first ids.size() strings, and nested start tags.
struct Tag {
    uint32_t name;
    std::vector<Attr> attrs;
    std::vector<Tag> children;
};
void write_tag(Bytes& b, const Tag& t) {
    const size_t at = b.size();
    put16(b, 0x0102);
    put16(b, 16);
    put32(b, 0);
    put32(b, 1);
    put32(b, 0xffffffffu);
    put32(b, 0xffffffffu);  // ns
    put32(b, t.name);
    put16(b, 20);
    put16(b, 20);
    put16(b, static_cast<uint16_t>(t.attrs.size()));
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    for (const Attr& a : t.attrs) {
        put32(b, 0xffffffffu);
        put32(b, a.name);
        put32(b, a.raw);
        put16(b, 8);
        b.push_back(0);
        b.push_back(a.type);
        put32(b, a.data);
    }
    set32(b, at + 4, static_cast<uint32_t>(b.size() - at));
    for (const Tag& c : t.children) write_tag(b, c);
    const size_t end = b.size();
    put16(b, 0x0103);
    put16(b, 16);
    put32(b, 24);
    put32(b, 1);
    put32(b, 0xffffffffu);
    put32(b, 0xffffffffu);
    put32(b, t.name);
    (void)end;
}
Bytes xml(const std::vector<std::string>& strings, const std::vector<uint32_t>& ids, const Tag& root) {
    Bytes b;
    put16(b, 0x0003);
    put16(b, 8);
    put32(b, 0);
    Bytes p = pool(strings);
    b.insert(b.end(), p.begin(), p.end());
    put16(b, 0x0180);
    put16(b, 8);
    put32(b, static_cast<uint32_t>(8 + 4 * ids.size()));
    for (uint32_t id : ids) put32(b, id);
    write_tag(b, root);
    set32(b, 4, static_cast<uint32_t>(b.size()));
    return b;
}

// A resource table of package 0x7f: per (type id, config density), a list of (entry, value type, data).
struct TypeChunk {
    uint8_t type;
    uint16_t density;
    std::vector<std::pair<uint8_t, uint32_t>> entries;  // index = entry id; type 0 = none
    uint16_t language = 0;
};
Bytes table(const std::vector<std::string>& values, const std::vector<TypeChunk>& types) {
    Bytes pkg;
    put16(pkg, 0x0200);
    put16(pkg, 288);
    put32(pkg, 0);
    put32(pkg, 0x7f);
    for (int i = 0; i < 128; ++i) put16(pkg, 0);
    put32(pkg, 0);
    put32(pkg, 0);
    put32(pkg, 0);
    put32(pkg, 0);
    put32(pkg, 0);
    for (const TypeChunk& t : types) {
        const size_t at = pkg.size();
        const uint32_t header = 20 + 64;
        put16(pkg, 0x0201);
        put16(pkg, static_cast<uint16_t>(header));
        put32(pkg, 0);
        pkg.push_back(t.type);
        pkg.push_back(0);
        put16(pkg, 0);
        put32(pkg, static_cast<uint32_t>(t.entries.size()));
        put32(pkg, static_cast<uint32_t>(header + 4 * t.entries.size()));
        Bytes config(64, 0);
        config[0] = 64;
        config[8] = static_cast<uint8_t>(t.language);
        config[9] = static_cast<uint8_t>(t.language >> 8);
        config[14] = static_cast<uint8_t>(t.density);
        config[15] = static_cast<uint8_t>(t.density >> 8);
        pkg.insert(pkg.end(), config.begin(), config.end());
        uint32_t off = 0;
        for (const auto& e : t.entries) {
            put32(pkg, e.first ? off : 0xffffffffu);
            if (e.first) off += 16;
        }
        for (const auto& e : t.entries) {
            if (!e.first) continue;
            put16(pkg, 8);
            put16(pkg, 0);
            put32(pkg, 0);
            put16(pkg, 8);
            pkg.push_back(0);
            pkg.push_back(e.first);
            put32(pkg, e.second);
        }
        set32(pkg, at + 4, static_cast<uint32_t>(pkg.size() - at));
    }
    set32(pkg, 4, static_cast<uint32_t>(pkg.size()));
    Bytes b;
    put16(b, 0x0002);
    put16(b, 12);
    put32(b, 0);
    put32(b, 1);
    Bytes p = pool(values);
    b.insert(b.end(), p.begin(), p.end());
    b.insert(b.end(), pkg.begin(), pkg.end());
    set32(b, 4, static_cast<uint32_t>(b.size()));
    return b;
}

// A stored zip.
Bytes zip(const std::vector<std::pair<std::string, Bytes>>& files) {
    Bytes b, dir;
    for (const auto& f : files) {
        const uint32_t local = static_cast<uint32_t>(b.size());
        put32(b, 0x04034b50);
        put16(b, 10);
        put16(b, 0);
        put16(b, 0);
        put32(b, 0);
        put32(b, 0);  // crc: not checked
        put32(b, static_cast<uint32_t>(f.second.size()));
        put32(b, static_cast<uint32_t>(f.second.size()));
        put16(b, static_cast<uint16_t>(f.first.size()));
        put16(b, 0);
        b.insert(b.end(), f.first.begin(), f.first.end());
        b.insert(b.end(), f.second.begin(), f.second.end());
        put32(dir, 0x02014b50);
        put16(dir, 20);
        put16(dir, 10);
        put16(dir, 0);
        put16(dir, 0);
        put32(dir, 0);
        put32(dir, 0);
        put32(dir, static_cast<uint32_t>(f.second.size()));
        put32(dir, static_cast<uint32_t>(f.second.size()));
        put16(dir, static_cast<uint16_t>(f.first.size()));
        put16(dir, 0);
        put16(dir, 0);
        put16(dir, 0);
        put16(dir, 0);
        put32(dir, 0);
        put32(dir, local);
        dir.insert(dir.end(), f.first.begin(), f.first.end());
    }
    const uint32_t dir_off = static_cast<uint32_t>(b.size());
    b.insert(b.end(), dir.begin(), dir.end());
    put32(b, 0x06054b50);
    put16(b, 0);
    put16(b, 0);
    put16(b, static_cast<uint16_t>(files.size()));
    put16(b, static_cast<uint16_t>(files.size()));
    put32(b, static_cast<uint32_t>(dir.size()));
    put32(b, dir_off);
    put16(b, 0);
    return b;
}

Bytes picture(const char* tag) {
    Bytes b = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    b.insert(b.end(), tag, tag + std::strlen(tag));
    return b;
}

// The manifest: <manifest package="com.example.icons"><application android:icon="@mipmap/ic_launcher"/>.
Bytes manifest(uint32_t icon) {
    // Strings: 0 "icon" (0x01010002 in the map), 1 "manifest", 2 "application", 3 "package", 4 the package.
    Tag app{2, {{0, 0x01, icon}}, {}};
    Tag root{1, {{3, 0x03, 4, 4}}, {app}};
    return xml({"icon", "manifest", "application", "package", "com.example.icons"}, {0x01010002}, root);
}

std::string text(const Bytes& b) { return std::string(b.begin() + 8, b.end()); }

void built_in() {
    // Types: 1 mipmap, 2 drawable, 3 color. mipmap/0 ic_launcher, mipmap/1 an alias of it, mipmap/2 an
    // adaptive icon (anydpi) whose foreground is drawable/0 and background color/0; mipmap/3 only a vector.
    const std::vector<std::string> values = {
        "res/mipmap-mdpi/ic_launcher.png", "res/mipmap-xxhdpi/ic_launcher.png", "res/mipmap-xxxhdpi-fr.png",
        "res/mipmap-anydpi-v26/ic_launcher.xml", "res/drawable-xxhdpi/fg.png", "res/drawable/vector.xml"};
    std::vector<TypeChunk> types = {
        {1, 0, {{0x03, 0}, {0x01, 0x7f010000}, {0, 0}, {0x03, 5}}},
        {1, 480, {{0x03, 1}, {0, 0}, {0, 0}, {0, 0}}},
        {1, 640, {{0x03, 2}, {0, 0}, {0, 0}, {0, 0}}, 'f' | ('r' << 8)},  // French only: not taken
        {1, 0xfffe, {{0, 0}, {0, 0}, {0x03, 3}, {0, 0}}},
        {2, 480, {{0x03, 4}}},
        {3, 0, {{0x1d, 0x00336699}}},
    };
    const Bytes arsc = table(values, types);
    // <adaptive-icon><background android:drawable="@color/0"/><foreground android:drawable="@drawable/0"/>
    const Bytes adaptive = xml({"drawable", "adaptive-icon", "background", "foreground"}, {0x01010199},
                               Tag{1, {}, {Tag{2, {{0, 0x01, 0x7f030000}}, {}}, Tag{3, {{0, 0x01, 0x7f020000}}, {}}}});
    const Bytes vector = xml({"vector"}, {}, Tag{0, {}, {}});
    auto apk = [&](uint32_t icon) {
        return zip({{"AndroidManifest.xml", manifest(icon)},
                    {"resources.arsc", arsc},
                    {values[0], picture("mdpi")},
                    {values[1], picture("xxhdpi")},
                    {values[2], picture("french")},
                    {values[3], adaptive},
                    {values[4], picture("foreground")},
                    {values[5], vector}});
    };

    ApkIcon icon;
    bool ok = winapk::shell::find_icon(MemorySource(apk(0x7f010000)), &icon);
    check(ok && icon.package == "com.example.icons", "the manifest's package");
    check(ok && !icon.adaptive && text(icon.icon.image) == "xxhdpi", "the densest picture, not another language's");
    ok = winapk::shell::find_icon(MemorySource(apk(0x7f010001)), &icon);
    check(ok && text(icon.icon.image) == "xxhdpi", "an alias followed");
    ok = winapk::shell::find_icon(MemorySource(apk(0x7f010002)), &icon);
    check(ok && icon.adaptive && icon.background.color && icon.background.argb == 0xff336699 &&
              text(icon.foreground.image) == "foreground",
          "an adaptive icon's color and picture layers");
    ok = winapk::shell::find_icon(MemorySource(apk(0x7f010003)), &icon);
    check(!ok && icon.package == "com.example.icons", "a vector: no icon, the package still read");
    ok = winapk::shell::find_icon(MemorySource(apk(0x7f0100ff)), &icon);
    check(!ok, "a missing resource");

    // Bundles: the store's icon.png first; else the base APK inside.
    ok = winapk::shell::find_icon(
        MemorySource(zip({{"info.json", {}}, {"icon.png", picture("store")}, {"base.apk", apk(0x7f010000)}})), &icon);
    check(ok && text(icon.icon.image) == "store", "a bundle's own icon.png");
    ok = winapk::shell::find_icon(
        MemorySource(zip({{"toc.pb", {}}, {"splits/base-master.apk", apk(0x7f010000)}})), &icon);
    check(ok && text(icon.icon.image) == "xxhdpi", "a bundle's base APK, read in place");
    {
        // The density's picture in its split only.
        Bytes base = zip({{"AndroidManifest.xml", manifest(0x7f010000)}, {"resources.arsc", arsc}});
        Bytes split = zip({{values[1], picture("split")}});
        ok = winapk::shell::find_icon(
            MemorySource(zip({{"base.apk", base}, {"split_config.xxhdpi.apk", split}})), &icon);
        check(ok && text(icon.icon.image) == "split", "a density split's picture");
    }

    // Damaged files: never a crash, never an icon from nowhere.
    const Bytes good = apk(0x7f010000);
    bool all_safe = true;
    for (size_t cut = 0; cut < good.size(); cut += 7) {
        Bytes b(good.begin(), good.begin() + cut);
        if (winapk::shell::find_icon(MemorySource(b), &icon)) all_safe = false;
    }
    check(all_safe, "every truncation refused");
    uint32_t seed = 12345;
    for (int round = 0; round < 3000; ++round) {
        Bytes b = good;
        for (int k = 0; k < 4; ++k) {
            seed = seed * 1103515245u + 12345u;
            b[(seed >> 8) % b.size()] ^= static_cast<uint8_t>(seed >> 24);
        }
        winapk::shell::find_icon(MemorySource(b), &icon);  // must return, whatever it finds
    }
    check(true, "3000 corrupted copies parsed without a crash");
}

}  // namespace

int main(int argc, char** argv) {
    built_in();
    for (int i = 1; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        Bytes b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ApkIcon icon;
        const bool ok = winapk::shell::find_icon(MemorySource(std::move(b)), &icon);
        const Bytes& shown = icon.adaptive ? icon.foreground.image : icon.icon.image;
        char magic[16] = "-";
        if (shown.size() >= 4) std::snprintf(magic, sizeof magic, "%02x%02x%02x%02x", shown[0], shown[1], shown[2], shown[3]);
        std::printf("%s %s package=%s adaptive=%d magic=%s icon=%zu bg=%s fg=%zu\n", ok ? "OK" : "NONE", argv[i],
                    icon.package.c_str(), icon.adaptive ? 1 : 0, magic, icon.icon.image.size(),
                    icon.background.color ? "color" : std::to_string(icon.background.image.size()).c_str(),
                    icon.foreground.image.size());
    }
    std::printf(failures ? "FAILED: %d\n" : "ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
