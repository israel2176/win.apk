// apk_icon.h -- an app file's own icon, read straight from the file: what
// Explorer's thumbnail of an .apk, .apkm, .xapk or .apks shows (apk_thumbnail.cpp).
//
// Nothing of the APK runs and no JVM starts: the zip's directory, the binary
// AndroidManifest.xml (the <application>'s android:icon), resources.arsc (that
// resource's files per density, or its color) and the picture's bytes are
// parsed with every offset and size checked against the data, under fixed
// limits. A bundle's own icon.png (APKMirror's .apkm, .xapk) is taken as it is;
// else its base APK is read, and a density split's file found in the split.
//
// What comes back is bytes to decode (PNG, WebP, JPEG: the caller decodes with
// WIC) or, for an adaptive icon (<adaptive-icon>, API 26), its two layers, each
// a picture or a color. A vector drawable is not drawn here: the caller falls
// back to the icon the install rendered (apps\<name>\icon.png), or to the file
// type's icon.
//
// No Windows call: windows/test/apk_icon_test.cpp runs it on Linux too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace winapk::shell {

// Random access to a file's bytes (an IStream, a file, memory, part of another).
class Source {
public:
    virtual ~Source() = default;
    virtual uint64_t size() const = 0;
    // Exactly n bytes at off, or false.
    virtual bool read(uint64_t off, void* dst, size_t n) const = 0;
};

class MemorySource final : public Source {
public:
    explicit MemorySource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
    uint64_t size() const override { return bytes_.size(); }
    bool read(uint64_t off, void* dst, size_t n) const override;

private:
    std::vector<uint8_t> bytes_;
};

// The bytes [off, off + len) of another source: a stored entry of a zip.
class SliceSource final : public Source {
public:
    SliceSource(const Source& base, uint64_t off, uint64_t len) : base_(base), off_(off), len_(len) {}
    uint64_t size() const override { return len_; }
    bool read(uint64_t off, void* dst, size_t n) const override;

private:
    const Source& base_;
    uint64_t off_, len_;
};

// A zip's central directory (zip64 too), and its entries' bytes (stored or deflated).
class Zip {
public:
    struct Entry {
        std::string name;
        uint16_t method = 0;
        uint64_t compressed = 0, size = 0, local = 0;
    };
    bool open(const Source& source);
    const Entry* find(const std::string& name) const;
    const std::vector<Entry>& entries() const { return entries_; }
    // The entry's bytes, at most max of them (false when larger, or damaged).
    bool read(const Entry& e, std::vector<uint8_t>* out, uint64_t max) const;
    // A stored entry's data, as a source of its own (no copy); null otherwise.
    std::unique_ptr<Source> stored(const Entry& e) const;

private:
    bool data_offset(const Entry& e, uint64_t* off) const;
    const Source* source_ = nullptr;
    std::vector<Entry> entries_;
};

// One layer of the icon: a picture's encoded bytes, or a color.
struct Layer {
    bool color = false;
    uint32_t argb = 0;
    std::vector<uint8_t> image;
    bool empty() const { return !color && image.empty(); }
};

struct ApkIcon {
    std::string package;  // the manifest's, when read (for the installed icon's fallback)
    bool adaptive = false;
    Layer icon;                     // !adaptive: the picture
    Layer background, foreground;   // adaptive: its layers, 108 dp square, the middle 72 dp shown
    bool found() const { return adaptive ? !foreground.empty() || !background.empty() : !icon.empty(); }
};

// The icon of an APK or a bundle. False when there is none this code can show
// (out->package may still be set).
bool find_icon(const Source& file, ApkIcon* out);

// Parts, for the test.
std::string manifest_package(const std::vector<uint8_t>& axml);
uint32_t manifest_icon(const std::vector<uint8_t>& axml);

}  // namespace winapk::shell
