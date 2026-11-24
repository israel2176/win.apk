// text_input_model.cpp -- see text_input_model.h.

#include "text_input_model.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace winapk::host::textinput {

// ---- the kind of field ---------------------------------------------------------------------

FieldPolicy field_policy(int32_t input_type, int32_t ime_options) {
    using namespace android;
    FieldPolicy p;
    const int32_t cls = input_type & kClassMask, variation = input_type & kVariationMask;
    auto secret = [&p](int32_t s) {
        // A password: no composition (an IME would learn it, and show it in its candidate
        // window), and no text in the document for a text service to read.
        p.scopes.push_back(s);
        p.compose = false;
        p.hide_text = true;
    };
    switch (cls) {
        case 0:  // TYPE_NULL: the editor takes key events only, as from a hardware keyboard
            p.document = false;
            p.compose = false;
            return p;
        case kClassText: {
            // LatinIME's rules: no suggestions in a password or with FLAG_NO_SUGGESTIONS; no
            // correction in an address, a URL or a filter, or without FLAG_AUTO_CORRECT.
            bool suggest = (input_type & kTextFlagNoSuggestions) == 0;
            bool correct = (input_type & kTextFlagAutoCorrect) != 0;
            switch (variation) {
                case kTextPassword:
                case kTextWebPassword:
                    secret(scope::kPassword);
                    suggest = false;
                    break;
                case kTextVisiblePassword:
                    // Shown as typed, but still a secret: plain characters, nothing learned.
                    secret(scope::kAlphanumericHalfwidth);
                    suggest = false;
                    break;
                case kTextUri:
                    p.scopes.push_back(scope::kUrl);
                    correct = false;
                    break;
                case kTextEmailAddress:
                case kTextWebEmailAddress:
                    p.scopes.push_back(scope::kEmailAddress);
                    correct = false;
                    break;
                case kTextPersonName: p.scopes.push_back(scope::kPersonName); break;
                case kTextPostalAddress: p.scopes.push_back(scope::kPostalAddress); break;
                case kTextShortMessage: p.scopes.push_back(scope::kChat); break;
                case kTextFilter:
                    p.scopes.push_back(scope::kSearchIncremental);
                    correct = false;
                    break;
                case kTextPhonetic: p.scopes.push_back(scope::kYomi); break;  // a reading, as Japanese names have
                default:
                    p.scopes.push_back((ime_options & kImeMaskAction) == kImeActionSearch ? scope::kSearch
                                                                                         : scope::kDefault);
                    break;
            }
            p.prediction = suggest;
            p.autocorrect = suggest && correct;
            break;
        }
        case kClassNumber:
            if (variation == kNumberPassword) {
                secret(scope::kNumericPassword);
            } else {
                p.scopes.push_back((input_type & (kNumberFlagSigned | kNumberFlagDecimal)) ? scope::kNumber
                                                                                           : scope::kDigits);
            }
            break;
        case kClassPhone: p.scopes.push_back(scope::kTelephone); break;
        case kClassDatetime:
            if (variation == kDatetimeDate) {
                p.scopes.push_back(scope::kDate);
            } else if (variation == kDatetimeTime) {
                p.scopes.push_back(scope::kTime);
            } else {
                p.scopes.push_back(scope::kDate);
                p.scopes.push_back(scope::kTime);
            }
            break;
        default: p.scopes.push_back(scope::kDefault); break;
    }
    // An incognito field: Windows' IS_PRIVATE asks the same of its text services.
    if (ime_options & kImeFlagNoPersonalizedLearning) p.scopes.push_back(scope::kPrivate);
    return p;
}

// ---- the document --------------------------------------------------------------------------

namespace {

int32_t clamp_to(int32_t v, int32_t lo, int32_t hi) { return std::max(lo, std::min(v, hi)); }

// Where position p goes when [start, old_end) became [start, new_end): before it, unchanged;
// after it, shifted; inside it, to the new text's end (to_start: its start, which also stays
// where it is when the new text goes right at it).
int32_t map_through(int32_t p, int32_t start, int32_t old_end, int32_t new_end, bool to_start = false) {
    if (p < start || (to_start && p == start)) return p;  // a start stays before what is put at it
    if (p >= old_end) return p + (new_end - old_end);
    return to_start ? start : new_end;
}

}  // namespace

void Document::reset(const Snapshot& s) {
    epoch_ = s.epoch;
    offset_ = s.offset;
    text_ = s.text;
    const int32_t n = length();
    const int32_t a = clamp_to(s.anchor - offset_, 0, n), c = clamp_to(s.caret - offset_, 0, n);
    sel_start_ = std::min(a, c);
    sel_end_ = std::max(a, c);
    caret_at_start_ = c < a;
    comp_start_ = comp_end_ = -1;
    runs_.clear();
    sent_ = acked_ = s.acked;
    dirty_ = false;
    base_sel_start_ = sel_start_;
    base_sel_end_ = sel_end_;
    base_caret_at_start_ = caret_at_start_;
    base_comp_start_ = base_comp_end_ = -1;
    base_runs_.clear();
}

bool Document::replace(int32_t start, int32_t end, std::u16string_view text) {
    if (start < 0 || end < start || end > length()) return false;
    const int32_t n = static_cast<int32_t>(text.size()), new_end = start + n;
    // The region rewritten since the last edit taken, in the text before it and now: a change
    // outside the old region widens it on that side (where the text before and now agree).
    if (!dirty_) {
        dirty_ = true;
        dirty_start_ = start;
        dirty_old_end_ = end;
        dirty_new_end_ = new_end;
    } else {
        const int32_t beyond = std::max(0, end - dirty_new_end_);
        dirty_old_end_ += beyond;
        dirty_new_end_ = std::max(dirty_new_end_, end) + (n - (end - start));
        dirty_start_ = std::min(dirty_start_, start);
    }
    text_.replace(static_cast<size_t>(start), static_cast<size_t>(end - start), text);
    sel_start_ = map_through(sel_start_, start, end, new_end);
    sel_end_ = map_through(sel_end_, start, end, new_end);
    if (comp_start_ >= 0) {
        comp_start_ = map_through(comp_start_, start, end, new_end, true);  // a composition grows at its end
        comp_end_ = map_through(comp_end_, start, end, new_end);
    }
    runs_.clear();  // the next attributes the text services set are for the new text
    return true;
}

bool Document::select(int32_t start, int32_t end, bool caret_at_start) {
    if (start < 0 || end < start || end > length()) return false;
    sel_start_ = start;
    sel_end_ = end;
    caret_at_start_ = caret_at_start && start != end;
    return true;
}

void Document::compose(int32_t start, int32_t end) {
    if (start < 0 || end < start) {
        comp_start_ = comp_end_ = -1;
        runs_.clear();
        return;
    }
    comp_start_ = start;
    comp_end_ = end;
    clamp_composition();
}

void Document::style(std::vector<Run> runs_acp) {
    runs_.clear();
    if (comp_start_ < 0) return;
    for (Run r : runs_acp) {
        r.start = clamp_to(r.start, comp_start_, comp_end_);
        r.end = clamp_to(r.end, r.start, comp_end_);
        if (r.end > r.start && r.flags != 0) runs_.push_back(r);
    }
}

void Document::clamp_composition() {
    if (comp_start_ < 0) return;
    comp_start_ = clamp_to(comp_start_, 0, length());
    comp_end_ = clamp_to(comp_end_, comp_start_, length());
}

std::optional<Edit> Document::take_edit() {
    const bool selection_moved =
        sel_start_ != base_sel_start_ || sel_end_ != base_sel_end_ || caret_at_start_ != base_caret_at_start_;
    const bool composition_moved = comp_start_ != base_comp_start_ || comp_end_ != base_comp_end_;
    const bool restyled = !(runs_ == base_runs_);
    if (!dirty_ && !selection_moved && !composition_moved && !restyled) return std::nullopt;
    Edit e;
    e.epoch = epoch_;
    e.seq = ++sent_;
    // A composition that changed in any way is written whole, so that the editor's composing
    // region is exactly the text services' (as an Android input method's setComposingText
    // makes it): the replaced range grows over it (outside the rewritten region the text
    // before and now agree). One that only stays as it was is not touched.
    const bool write_composition = comp_start_ >= 0 && (dirty_ || composition_moved || restyled);
    if (dirty_ || write_composition) {
        int32_t start = dirty_ ? dirty_start_ : comp_start_, old_end = dirty_ ? dirty_old_end_ : comp_start_,
                new_end = dirty_ ? dirty_new_end_ : comp_start_;
        if (write_composition) {
            const int32_t lo = std::min(start, comp_start_), hi_new = std::max(new_end, comp_end_);
            old_end = hi_new - (new_end - old_end);
            start = lo;
            new_end = hi_new;
        }
        e.start = start + offset_;
        e.end = old_end + offset_;
        e.text = text_.substr(static_cast<size_t>(start), static_cast<size_t>(new_end - start));
    } else {
        e.start = e.end = sel_start_ + offset_;  // nothing replaced
    }
    if (comp_start_ >= 0) {
        e.comp_start = comp_start_ + offset_;
        e.comp_end = comp_end_ + offset_;
        for (Run r : runs_) {
            r.start += offset_;
            r.end += offset_;
            e.runs.push_back(r);
        }
    }
    e.anchor = (caret_at_start_ ? sel_end_ : sel_start_) + offset_;
    e.caret = (caret_at_start_ ? sel_start_ : sel_end_) + offset_;
    dirty_ = false;
    base_sel_start_ = sel_start_;
    base_sel_end_ = sel_end_;
    base_caret_at_start_ = caret_at_start_;
    base_comp_start_ = comp_start_;
    base_comp_end_ = comp_end_;
    base_runs_ = runs_;
    return e;
}

std::optional<External> Document::accept(const Snapshot& s) {
    if (s.epoch != epoch_) return std::nullopt;
    acked_ = std::max(acked_, s.acked);
    if (s.acked < sent_) return std::nullopt;  // the editor has not seen every edit yet
    External out;
    const int32_t old_len = length(), new_len = static_cast<int32_t>(s.text.size());
    if (s.offset != offset_) {
        // The window over the editor's text moved: every position is another one now.
        out.text = TextChange{0, old_len, new_len};
        const int32_t shift = s.offset - offset_;
        if (comp_start_ >= 0) {
            comp_start_ -= shift;
            comp_end_ -= shift;
            if (comp_end_ < 0 || comp_start_ > new_len) comp_start_ = comp_end_ = -1;
        }
        text_ = s.text;
        offset_ = s.offset;
        runs_.clear();
    } else if (s.text != text_) {
        int32_t prefix = 0;
        const int32_t most = std::min(old_len, new_len);
        while (prefix < most && text_[prefix] == s.text[prefix]) prefix++;
        int32_t suffix = 0;
        while (suffix < most - prefix && text_[old_len - 1 - suffix] == s.text[new_len - 1 - suffix]) suffix++;
        const TextChange c{prefix, old_len - suffix, new_len - suffix};
        out.text = c;
        if (comp_start_ >= 0) {
            comp_start_ = map_through(comp_start_, c.start, c.old_end, c.new_end, true);
            comp_end_ = map_through(comp_end_, c.start, c.old_end, c.new_end);
        }
        text_ = s.text;
        runs_.clear();
    }
    clamp_composition();
    const int32_t a = clamp_to(s.anchor - offset_, 0, new_len), c = clamp_to(s.caret - offset_, 0, new_len);
    const int32_t lo = std::min(a, c), hi = std::max(a, c);
    const bool at_start = c < a;
    out.selection = lo != sel_start_ || hi != sel_end_ || at_start != caret_at_start_;
    sel_start_ = lo;
    sel_end_ = hi;
    caret_at_start_ = at_start;
    dirty_ = false;
    base_sel_start_ = sel_start_;
    base_sel_end_ = sel_end_;
    base_caret_at_start_ = caret_at_start_;
    base_comp_start_ = comp_start_;
    base_comp_end_ = comp_end_;
    base_runs_ = runs_;
    return out;
}

// ---- display attributes --------------------------------------------------------------------

namespace {

constexpr int kColorHighlight = 13, kColorHighlightText = 14;  // GetSysColor's COLOR_HIGHLIGHT(TEXT)

bool argb(int type, uint32_t value, const std::function<uint32_t(int)>& sys_color, uint32_t* out) {
    if (type == DisplayAttribute::kColorRef) {
        // COLORREF is 0x00bbggrr.
        *out = 0xff000000u | ((value & 0xffu) << 16) | (value & 0xff00u) | ((value >> 16) & 0xffu);
        return true;
    }
    if (type == DisplayAttribute::kSysColor && sys_color) {
        *out = sys_color(static_cast<int>(value));
        return true;
    }
    return false;
}

}  // namespace

Run style_run(const DisplayAttribute& a, int32_t start, int32_t end, const std::function<uint32_t(int)>& sys_color) {
    Run r;
    r.start = start;
    r.end = end;
    if (a.line_style != DisplayAttribute::kLineNone) r.flags |= Run::kUnderline;
    if (a.line_style != DisplayAttribute::kLineNone && a.bold_line) r.flags |= Run::kThick;
    if (argb(a.text_type, a.text, sys_color, &r.foreground)) r.flags |= Run::kForeground;
    if (argb(a.back_type, a.back, sys_color, &r.background)) r.flags |= Run::kBackground;
    if (a.attr == DisplayAttribute::kTargetConverted || a.attr == DisplayAttribute::kTargetNotConverted) {
        r.flags |= Run::kTarget;
        if (!(r.flags & Run::kBackground) && sys_color) {
            r.background = sys_color(kColorHighlight);
            r.flags |= Run::kBackground;
            if (!(r.flags & Run::kForeground)) {
                r.foreground = sys_color(kColorHighlightText);
                r.flags |= Run::kForeground;
            }
        }
    }
    return r;
}

// ---- where the editor drew its text ----------------------------------------------------------

std::optional<Box> extent(const Layout& l, int32_t start, int32_t end, bool* clipped) {
    if (clipped) *clipped = false;
    if (end < start) std::swap(start, end);
    const int32_t n = static_cast<int32_t>(l.boxes.size());
    auto rtl = [&](int32_t i) { return i < static_cast<int32_t>(l.rtl.size()) && l.rtl[i]; };
    std::optional<Box> out;
    if (start == end) {
        const int32_t i = start - l.first;
        if (i >= 0 && i < n) {
            const Box& b = l.boxes[i];
            const float x = rtl(i) ? b.right : b.left;  // the character's leading edge
            out = Box{x, b.top, x, b.bottom};
        } else if (i == n && n > 0) {
            const Box& b = l.boxes[n - 1];
            const float x = rtl(n - 1) ? b.left : b.right;  // after the last one: its trailing edge
            out = Box{x, b.top, x, b.bottom};
        }
    } else {
        for (int32_t p = std::max(start, l.first); p < std::min(end, l.first + n); ++p) {
            const Box& b = l.boxes[p - l.first];
            if (b.bottom <= b.top) continue;  // not drawn
            if (!out) {
                out = b;
            } else {
                out->left = std::min(out->left, b.left);
                out->top = std::min(out->top, b.top);
                out->right = std::max(out->right, b.right);
                out->bottom = std::max(out->bottom, b.bottom);
            }
        }
    }
    if (!out || !l.has_editor) return out;
    const Box& e = l.editor;
    const bool inside = out->left >= e.left && out->right <= e.right && out->top >= e.top && out->bottom <= e.bottom;
    if (inside) return out;
    if (clipped) *clipped = true;
    const Box cut{std::max(out->left, e.left), std::max(out->top, e.top), std::min(out->right, e.right),
                  std::min(out->bottom, e.bottom)};
    if (cut.right >= cut.left && cut.bottom > cut.top) return cut;
    return out;  // all of it scrolled away: where it would be
}

int32_t position_at(const Layout& l, float x, float y, bool round, bool nearest) {
    const int32_t n = static_cast<int32_t>(l.boxes.size());
    auto rtl = [&](int32_t i) { return i < static_cast<int32_t>(l.rtl.size()) && l.rtl[i]; };
    auto pick = [&](int32_t i) {
        if (!round) return l.first + i;
        const Box& b = l.boxes[i];
        const float mid = (b.left + b.right) / 2;
        const bool past = rtl(i) ? x < mid : x >= mid;  // the half after the character's start
        return l.first + i + (past ? 1 : 0);
    };
    for (int32_t i = 0; i < n; ++i) {
        const Box& b = l.boxes[i];
        if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) return pick(i);
    }
    if (!nearest) return -1;
    int32_t best = -1;
    float best_d = std::numeric_limits<float>::max();
    for (int32_t i = 0; i < n; ++i) {
        const Box& b = l.boxes[i];
        if (b.bottom <= b.top) continue;
        const float dx = x < b.left ? b.left - x : (x > b.right ? x - b.right : 0.0f);
        const float dy = y < b.top ? b.top - y : (y > b.bottom ? y - b.bottom : 0.0f);
        const float d = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best < 0 ? -1 : pick(best);
}

}  // namespace winapk::host::textinput
