// text_input_model.h -- the focused Android editor as Windows' text services see it, with no
// Windows call and no JVM in it, so that all of its rules are tested on their own
// (test/text_input_model_test.cpp).
//
// Three parties edit one text. The editor (an EditText's InputConnection, on the app's thread)
// owns it. Windows' text services (an IME, handwriting, dictation, the touch keyboard's
// suggestions) read and write a copy of it, synchronously, inside the locks TSF takes on the
// window's thread (tsf_store.h). The copy is a Document: what TSF writes changes it at once and
// is sent to the editor as one Edit per lock, numbered; the editor applies the edits in order
// and answers each with a Snapshot of itself that names the newest edit it applied. A snapshot
// older than the newest edit sent is dropped (it would undo that edit); a current one replaces
// the copy, and what differs is what the text services are told changed (an app that filtered
// or reformatted what was typed, a key typed with no text service, the app's own setText).
//
// Positions: the editor's are UTF-16 indices into its whole text (Android's, and Java's);
// the document holds the editor's text from `offset` on (all of it for an EditText, a window
// around the caret for an editor that only answers getSurroundingText), and TSF's positions
// (ACP) are indices into that. Both count UTF-16 units, so a character beyond U+FFFF (an emoji)
// is two of them everywhere, as on Android.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace winapk::host::textinput {

// ---- the kind of field: EditorInfo.inputType and imeOptions ---------------------------------

// android.text.InputType and android.view.inputmethod.EditorInfo, as AOSP 14 defines them.
namespace android {
constexpr int32_t kClassMask = 0x0000000f, kVariationMask = 0x00000ff0;
constexpr int32_t kClassText = 1, kClassNumber = 2, kClassPhone = 3, kClassDatetime = 4;
constexpr int32_t kTextUri = 0x10, kTextEmailAddress = 0x20, kTextEmailSubject = 0x30, kTextShortMessage = 0x40,
                  kTextLongMessage = 0x50, kTextPersonName = 0x60, kTextPostalAddress = 0x70,
                  kTextPassword = 0x80, kTextVisiblePassword = 0x90, kTextWebEditText = 0xa0, kTextFilter = 0xb0,
                  kTextPhonetic = 0xc0, kTextWebEmailAddress = 0xd0, kTextWebPassword = 0xe0;
constexpr int32_t kTextFlagAutoCorrect = 0x8000, kTextFlagNoSuggestions = 0x80000;
constexpr int32_t kNumberPassword = 0x10, kNumberFlagSigned = 0x1000, kNumberFlagDecimal = 0x2000;
constexpr int32_t kDatetimeDate = 0x10, kDatetimeTime = 0x20;
constexpr int32_t kImeMaskAction = 0xff, kImeActionSearch = 3;
constexpr int32_t kImeFlagNoPersonalizedLearning = 0x1000000;
}  // namespace android

// Windows' InputScope values (inputscope.h; tsf_store.cpp checks them against the header).
namespace scope {
constexpr int32_t kDefault = 0, kUrl = 1, kEmailAddress = 5, kPersonName = 7, kPostalAddress = 13, kDate = 22,
                  kDigits = 28, kNumber = 29, kPassword = 31, kTelephone = 32, kTime = 36, kAlphanumericHalfwidth = 40,
                  kSearch = 50, kSearchIncremental = 52, kYomi = 56, kChat = 58, kPrivate = 61,
                  kNumericPassword = 63;
}  // namespace scope

struct FieldPolicy {
    bool document = true;     // false: TYPE_NULL, an editor that wants key events only
    bool compose = true;      // text services may compose in it (false: their keyboard is disabled)
    bool hide_text = false;   // the document shows them no text (a password)
    bool autocorrect = false; // the touch keyboard may correct words (TS_SS_TKBAUTOCORRECTENABLE)
    bool prediction = false;  // and suggest them (TS_SS_TKBPREDICTIONENABLE)
    std::vector<int32_t> scopes;  // InputScope values, most specific first
};

// What Windows is told about a field, from what the app told Android's input method.
FieldPolicy field_policy(int32_t input_type, int32_t ime_options);

// ---- the editor's text, and the edits made to it ---------------------------------------------

// The editor as its side last read it (ExtractedText or SurroundingText).
struct Snapshot {
    int32_t epoch = 0;  // the editor (a new editor, a new epoch)
    int32_t acked = 0;  // the newest Edit::seq the editor had applied
    int32_t offset = 0; // the editor position of text[0]
    std::u16string text;
    int32_t anchor = 0, caret = 0;  // the selection, editor positions; caret < anchor: reversed
};

// How a stretch of the composition is drawn: a text service's display attribute
// (TF_DISPLAYATTRIBUTE) made into what Android's text spans can draw.
struct Run {
    enum : uint32_t { kUnderline = 1, kThick = 2, kForeground = 4, kBackground = 8, kTarget = 16 };
    int32_t start = 0, end = 0;  // editor positions, after the edit
    uint32_t flags = 0;
    uint32_t foreground = 0, background = 0;  // ARGB, with kForeground / kBackground
    bool operator==(const Run& o) const {
        return start == o.start && end == o.end && flags == o.flags && foreground == o.foreground &&
               background == o.background;
    }
};

// One lock's worth of what the text services did, for the editor. Editor positions.
struct Edit {
    int32_t epoch = 0, seq = 0;
    int32_t start = 0, end = 0;  // the replaced range, before the edit
    std::u16string text;         // what replaces it; covers the composition when there is one
    int32_t comp_start = -1, comp_end = -1;  // the composition after the edit, -1: none
    int32_t anchor = 0, caret = 0;           // the selection after the edit
    std::vector<Run> runs;                   // inside the composition; none: the editor's default look
};

// What changed in the document from outside the text services (OnTextChange's TS_TEXTCHANGE).
struct TextChange {
    int32_t start = 0, old_end = 0, new_end = 0;  // ACP
};

struct External {
    std::optional<TextChange> text;
    bool selection = false;
};

class Document {
public:
    // A new editor (or the same one after its text was replaced wholesale).
    void reset(const Snapshot& s);

    int32_t epoch() const { return epoch_; }
    int32_t offset() const { return offset_; }
    const std::u16string& text() const { return text_; }
    int32_t length() const { return static_cast<int32_t>(text_.size()); }
    // The selection in ACP, start <= end, and which end the caret is at.
    int32_t sel_start() const { return sel_start_; }
    int32_t sel_end() const { return sel_end_; }
    bool caret_at_start() const { return caret_at_start_; }
    int32_t comp_start() const { return comp_start_; }
    int32_t comp_end() const { return comp_end_; }
    bool composing() const { return comp_start_ >= 0; }
    // Edits sent that the editor has not answered yet.
    bool in_flight() const { return acked_ < sent_; }
    int32_t sent() const { return sent_; }

    // The text services' changes, inside a write lock. False when out of range (nothing done).
    bool replace(int32_t start, int32_t end, std::u16string_view text);
    bool select(int32_t start, int32_t end, bool caret_at_start);
    // The composition, as TSF reports it (ACP); -1, -1: none.
    void compose(int32_t start, int32_t end);
    void style(std::vector<Run> runs_acp);  // the composition's display attributes, ACP

    // At the end of a lock: the edit for the editor, if the lock changed anything it sees.
    std::optional<Edit> take_edit();

    // The editor's newest copy of itself: nullopt when it is another editor's or older than an
    // edit sent (dropped); else what changed, for the text services' sink.
    std::optional<External> accept(const Snapshot& s);

private:
    void clamp_composition();
    int32_t epoch_ = 0, offset_ = 0;
    std::u16string text_;
    int32_t sel_start_ = 0, sel_end_ = 0;
    bool caret_at_start_ = false;
    int32_t comp_start_ = -1, comp_end_ = -1;
    std::vector<Run> runs_;
    int32_t sent_ = 0, acked_ = 0;
    // Since the last take_edit: the region the text services rewrote, [dirty_start_, dirty_old_end_)
    // of the text before, [dirty_start_, dirty_new_end_) now; and whether anything else moved.
    bool dirty_ = false;
    int32_t dirty_start_ = 0, dirty_old_end_ = 0, dirty_new_end_ = 0;
    int32_t base_sel_start_ = 0, base_sel_end_ = 0;
    bool base_caret_at_start_ = false;
    int32_t base_comp_start_ = -1, base_comp_end_ = -1;
    std::vector<Run> base_runs_;
};

// ---- display attributes ----------------------------------------------------------------------

// TF_DISPLAYATTRIBUTE's fields, as plain values (msctf.h: TF_CT_*, TF_LS_*, TF_ATTR_*).
struct DisplayAttribute {
    enum ColorType { kNone = 0, kSysColor = 1, kColorRef = 2 };
    enum LineStyle { kLineNone = 0, kSolid = 1, kDot = 2, kDash = 3, kSquiggle = 4 };
    enum Attr { kInput = 0, kTargetConverted = 1, kConverted = 2, kTargetNotConverted = 3, kInputError = 4,
                kFixedConverted = 5, kOther = -1 };
    int text_type = kNone;
    uint32_t text = 0;  // a COLORREF, or a GetSysColor index
    int back_type = kNone;
    uint32_t back = 0;
    int line_style = kLineNone;
    bool bold_line = false;
    int attr = kInput;
};

// A run of the composition drawn as the text service asked. sys_color: GetSysColor, as ARGB.
// The clause being converted (TF_ATTR_TARGET_*) is shown selected (the highlight color behind
// it) when the text service gives it no background of its own: Android's spans have no thick
// underline, and that clause must stand out from the others.
Run style_run(const DisplayAttribute& a, int32_t start, int32_t end, const std::function<uint32_t(int)>& sys_color);

// ---- where the editor drew its text ----------------------------------------------------------

struct Box {
    float left = 0, top = 0, right = 0, bottom = 0;
    bool empty() const { return right < left || bottom <= top; }
};

// What the editor side measured, in the app's frame pixels: the editor's bounds, its caret,
// and the boxes of the characters from editor position `first` on (each with whether it runs
// right to left, which side its caret goes on).
struct Layout {
    int32_t epoch = 0;
    int32_t acked = 0;  // the newest edit the editor had applied when it was measured
    bool has_editor = false, has_caret = false;
    Box editor, caret;
    int32_t first = 0;
    std::vector<Box> boxes;
    std::vector<bool> rtl;  // as many as boxes
};

// The bounds of editor positions [start, end): the union of their characters' boxes; for an
// empty range, a line-high sliver on the side of its character the caret goes. nullopt when
// none of them was measured. clipped: part of it lies outside the editor (scrolled away).
std::optional<Box> extent(const Layout& l, int32_t start, int32_t end, bool* clipped);

// The editor position at frame point (x, y): the character whose box holds it (round: the
// nearer of its two edges, as GXFPF_ROUND_NEAREST); nearest: else the closest character
// (GXFPF_NEAREST). -1 when there is none.
int32_t position_at(const Layout& l, float x, float y, bool round, bool nearest);

}  // namespace winapk::host::textinput
