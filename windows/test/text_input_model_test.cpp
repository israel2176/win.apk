// text_input_model_test.cpp -- the rules of host/text_input_model.h, with no window and no JVM:
// what Windows is told about each kind of Android field, the document the text services edit
// (one edit per lock for the editor, a stale copy of the editor dropped, the editor's own
// changes reported back as TSF's text changes), display attributes as Android spans, and
// where characters are for candidate windows, right to left too. Plain C++: it builds and runs
// on any host (CI's Windows build runs it; `c++ -std=c++17 -Iwindows/host
// windows/test/text_input_model_test.cpp windows/host/text_input_model.cpp` anywhere else).

#include "text_input_model.h"

#include <cstdio>
#include <string>

using namespace winapk::host::textinput;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

std::string utf8(const std::u16string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < s.size()) c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
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

std::string show(const Edit& e) {
    return "[" + std::to_string(e.start) + "," + std::to_string(e.end) + ")=\"" + utf8(e.text) + "\" comp " +
           std::to_string(e.comp_start) + "-" + std::to_string(e.comp_end) + " sel " + std::to_string(e.anchor) +
           ">" + std::to_string(e.caret) + " seq " + std::to_string(e.seq);
}

Snapshot snap(int epoch, int acked, int offset, const std::u16string& text, int anchor, int caret) {
    Snapshot s;
    s.epoch = epoch;
    s.acked = acked;
    s.offset = offset;
    s.text = text;
    s.anchor = anchor;
    s.caret = caret;
    return s;
}

bool has(const FieldPolicy& p, int32_t s) {
    for (int32_t v : p.scopes) {
        if (v == s) return true;
    }
    return false;
}

void field_policies() {
    using namespace android;
    FieldPolicy p = field_policy(kClassText, 0);
    check(p.document && p.compose && !p.hide_text && p.prediction && !p.autocorrect && p.scopes.size() == 1 &&
              p.scopes[0] == scope::kDefault,
          "a plain text field composes, with suggestions, default scope");
    p = field_policy(kClassText | kTextFlagAutoCorrect, 0);
    check(p.autocorrect && p.prediction, "FLAG_AUTO_CORRECT: the touch keyboard may correct");
    p = field_policy(kClassText | kTextFlagAutoCorrect | kTextFlagNoSuggestions, 0);
    check(!p.autocorrect && !p.prediction, "FLAG_NO_SUGGESTIONS: no prediction, no correction");
    p = field_policy(kClassText | kTextPassword, 0);
    check(!p.compose && p.hide_text && !p.prediction && has(p, scope::kPassword),
          "a password: IS_PASSWORD, no composition, no text shown to text services");
    p = field_policy(kClassText | kTextWebPassword, 0);
    check(!p.compose && p.hide_text && has(p, scope::kPassword), "a web password too");
    p = field_policy(kClassText | kTextVisiblePassword, 0);
    check(!p.compose && p.hide_text && has(p, scope::kAlphanumericHalfwidth),
          "a visible password: plain characters, still no composition");
    p = field_policy(kClassNumber | kNumberPassword, 0);
    check(!p.compose && p.hide_text && has(p, scope::kNumericPassword), "a PIN: IS_NUMERIC_PASSWORD");
    p = field_policy(kClassText | kTextUri | kTextFlagAutoCorrect, 0);
    check(has(p, scope::kUrl) && !p.autocorrect && p.compose, "a URL: IS_URL, never corrected");
    p = field_policy(kClassText | kTextEmailAddress, 0);
    check(has(p, scope::kEmailAddress), "an e-mail address: IS_EMAIL_SMTPEMAILADDRESS");
    p = field_policy(kClassText | kTextWebEmailAddress, 0);
    check(has(p, scope::kEmailAddress), "a web e-mail address too");
    p = field_policy(kClassText | kTextPersonName, 0);
    check(has(p, scope::kPersonName), "a person's name");
    p = field_policy(kClassText | kTextPostalAddress, 0);
    check(has(p, scope::kPostalAddress), "a postal address");
    p = field_policy(kClassText | kTextShortMessage, 0);
    check(has(p, scope::kChat), "a short message: IS_CHAT");
    p = field_policy(kClassText | kTextFilter, 0);
    check(has(p, scope::kSearchIncremental), "a filter: IS_SEARCH_INCREMENTAL");
    p = field_policy(kClassText | kTextPhonetic, 0);
    check(has(p, scope::kYomi), "a phonetic field: IS_YOMI");
    p = field_policy(kClassText, kImeActionSearch);
    check(has(p, scope::kSearch), "a text field whose action is Search: IS_SEARCH");
    p = field_policy(kClassNumber, 0);
    check(has(p, scope::kDigits) && p.compose, "a number: IS_DIGITS");
    p = field_policy(kClassNumber | kNumberFlagDecimal, 0);
    check(has(p, scope::kNumber), "a decimal number: IS_NUMBER");
    p = field_policy(kClassNumber | kNumberFlagSigned, 0);
    check(has(p, scope::kNumber), "a signed number: IS_NUMBER");
    p = field_policy(kClassPhone, 0);
    check(has(p, scope::kTelephone), "a phone number: IS_TELEPHONE_FULLTELEPHONENUMBER");
    p = field_policy(kClassDatetime | kDatetimeDate, 0);
    check(has(p, scope::kDate) && !has(p, scope::kTime), "a date");
    p = field_policy(kClassDatetime | kDatetimeTime, 0);
    check(has(p, scope::kTime) && !has(p, scope::kDate), "a time");
    p = field_policy(kClassDatetime, 0);
    check(has(p, scope::kTime) && has(p, scope::kDate), "a date and time");
    p = field_policy(0, 0);
    check(!p.document && !p.compose, "TYPE_NULL: no document, keys only");
    p = field_policy(kClassText, kImeFlagNoPersonalizedLearning);
    check(has(p, scope::kPrivate) && p.compose, "IME_FLAG_NO_PERSONALIZED_LEARNING: IS_PRIVATE as well");
}

void compositions() {
    Document d;
    d.reset(snap(1, 0, 0, u"ab", 2, 2));
    check(!d.take_edit(), "no lock change, no edit");

    // A Japanese composition at the caret: typed, converted, committed.
    d.replace(2, 2, u"に");
    d.compose(2, 3);
    d.select(3, 3, false);
    auto e = d.take_edit();
    check(e && e->start == 2 && e->end == 2 && e->text == u"に" && e->comp_start == 2 && e->comp_end == 3 &&
              e->anchor == 3 && e->caret == 3 && e->seq == 1,
          "a composition starts at the caret: " + (e ? show(*e) : "none"));
    check(d.in_flight(), "until the editor answers, the edit is in flight");
    d.replace(2, 3, u"にほん");
    d.compose(2, 5);
    d.select(5, 5, false);
    e = d.take_edit();
    check(e && e->start == 2 && e->end == 3 && e->text == u"にほん" && e->comp_start == 2 && e->comp_end == 5 &&
              e->seq == 2,
          "it grows in place: " + (e ? show(*e) : "none"));
    d.replace(2, 5, u"日本");
    d.compose(2, 4);
    d.select(4, 4, false);
    e = d.take_edit();
    check(e && e->start == 2 && e->end == 5 && e->text == u"日本" && e->comp_end == 4,
          "a conversion replaces it: " + (e ? show(*e) : "none"));
    d.compose(-1, -1);
    e = d.take_edit();
    check(e && e->start == e->end && e->text.empty() && e->comp_start == -1 && e->caret == 4,
          "its end finishes it, with nothing replaced: " + (e ? show(*e) : "none"));

    // The editor answers the first edits only: dropped, it would undo the newer ones.
    check(!d.accept(snap(1, 2, 0, u"abにほん", 5, 5)), "a copy older than the newest edit is dropped");
    check(d.text() == u"ab日本", "and the document keeps the text services' text");
    auto x = d.accept(snap(1, 4, 0, u"ab日本", 4, 4));
    check(x && !x->text && !x->selection && !d.in_flight(), "the editor's answer to every edit changes nothing");
    check(!d.accept(snap(2, 4, 0, u"zz", 0, 0)), "another editor's copy is dropped");
}

void partial_commit_and_sequences() {
    Document d;
    d.reset(snap(3, 0, 0, u"x", 1, 1));
    d.replace(1, 1, u"にほんご");
    d.compose(1, 5);
    d.select(5, 5, false);
    d.take_edit();
    // A partial commit: the first clause leaves the composition, the rest stays composing.
    d.replace(1, 5, u"日本ご");
    d.compose(3, 4);
    d.select(4, 4, false);
    auto e = d.take_edit();
    check(e && e->start == 1 && e->end == 5 && e->text == u"日本ご" && e->comp_start == 3 && e->comp_end == 4,
          "a partial commit: the replaced range covers the clause left composing: " + (e ? show(*e) : "none"));

    // Two writes in one lock make one edit.
    Document f;
    f.reset(snap(4, 0, 0, u"hello world", 5, 5));
    f.replace(0, 1, u"J");
    f.replace(6, 11, u"there!");
    f.select(12, 12, false);
    e = f.take_edit();
    check(e && e->start == 0 && e->end == 11 && e->text == u"Jello there!" && e->comp_start == -1 &&
              f.text() == u"Jello there!",
          "two writes in one lock are one edit over both: " + (e ? show(*e) : "none"));
    // A write before an earlier one in the same lock.
    Document g;
    g.reset(snap(5, 0, 0, u"0123456789", 0, 0));
    g.replace(5, 5, u"ab");
    g.replace(8, 9, u"xyz");
    g.replace(2, 3, u"");
    e = g.take_edit();
    check(e && e->start == 2 && e->end == 7 && e->text == u"34ab5xyz" && g.text() == u"0134ab5xyz789",
          "writes on both sides of an earlier one: " + (e ? show(*e) : "none"));
}

void selection_and_reconversion() {
    Document d;
    d.reset(snap(1, 0, 0, u"かんじ漢字", 5, 5));
    d.select(0, 3, true);
    auto e = d.take_edit();
    check(e && e->start == e->end && e->text.empty() && e->anchor == 3 && e->caret == 0,
          "a selection alone: nothing replaced, the caret at its start: " + (e ? show(*e) : "none"));
    // Reconversion: the text service makes committed text a composition again, unchanged.
    d.compose(3, 5);
    e = d.take_edit();
    check(e && e->start == 3 && e->end == 5 && e->text == u"漢字" && e->comp_start == 3 && e->comp_end == 5,
          "reconversion composes committed text in place: " + (e ? show(*e) : "none"));
    d.select(5, 5, false);
    e = d.take_edit();
    check(e && e->start == e->end && e->text.empty() && e->comp_start == 3 && e->comp_end == 5,
          "a composition that stays as it was is not rewritten: " + (e ? show(*e) : "none"));
}

void offsets_and_emoji() {
    // An editor that answers getSurroundingText: the document starts at editor position 100.
    Document d;
    d.reset(snap(7, 0, 100, u"abc", 103, 103));
    check(d.sel_start() == 3 && d.sel_end() == 3, "editor positions become ACP");
    d.replace(3, 3, u"\U0001F600");  // an emoji: a surrogate pair, two UTF-16 units
    d.select(5, 5, false);
    auto e = d.take_edit();
    check(e && e->start == 103 && e->end == 103 && e->text.size() == 2 && e->caret == 105,
          "an emoji is two units, at the editor's positions: " + (e ? show(*e) : "none"));
    // Text written right at an empty composition's start goes into it (the start stays).
    Document g;
    g.reset(snap(9, 0, 0, u"ab", 2, 2));
    g.compose(2, 2);
    g.replace(2, 2, u"x");
    check(g.comp_start() == 2 && g.comp_end() == 3 && g.sel_start() == 3, "a composition grows from its start");
    g.replace(0, 0, u"Z");
    check(g.comp_start() == 3 && g.comp_end() == 4, "and moves with text put before it");
    // Hebrew after Hebrew, composed (a Hebrew handwriting panel or dictation composes too).
    Document h;
    h.reset(snap(8, 0, 0, u"שלום ", 5, 5));
    h.replace(5, 5, u"עולם");
    h.compose(5, 9);
    h.select(9, 9, false);
    e = h.take_edit();
    check(e && e->start == 5 && e->text == u"עולם" && e->comp_end == 9, "Hebrew composes after Hebrew");
}

void external_changes() {
    Document d;
    d.reset(snap(1, 0, 0, u"hello", 5, 5));
    // The app filtered what was typed: the editor's answer differs from the document.
    d.replace(5, 5, u"!!");
    d.select(7, 7, false);
    d.take_edit();
    auto x = d.accept(snap(1, 1, 0, u"hello!", 6, 6));
    check(x && x->text && x->text->start == 6 && x->text->old_end == 7 && x->text->new_end == 6 && x->selection,
          "the editor kept less than was typed: a text change for TSF");
    check(d.text() == u"hello!" && d.sel_start() == 6, "and the document is the editor's");
    // A key typed with no text service: nothing in flight, the copy is taken as it comes.
    x = d.accept(snap(1, 1, 0, u"hello!?", 7, 7));
    check(x && x->text && x->text->start == 6 && x->text->old_end == 6 && x->text->new_end == 7,
          "a character typed by a key: an insertion");
    x = d.accept(snap(1, 1, 0, u"hello!?", 0, 7));
    check(x && !x->text && x->selection && !d.caret_at_start(), "a selection made by the mouse");
    x = d.accept(snap(1, 1, 0, u"hello!?", 7, 0));
    check(x && x->selection && d.caret_at_start(), "reversed: the caret at the start");
    // The window over a long text moved.
    x = d.accept(snap(1, 1, 3, u"lo!?", 6, 6));
    check(x && x->text && x->text->start == 0 && x->text->old_end == 7 && x->text->new_end == 4 &&
              d.sel_start() == 3,
          "the document's window moved: all of it changed");
    // The composition follows an outside change before it.
    Document c;
    c.reset(snap(2, 0, 0, u"ab", 2, 2));
    c.replace(2, 2, u"か");
    c.compose(2, 3);
    c.select(3, 3, false);
    c.take_edit();
    x = c.accept(snap(2, 1, 0, u"Zabか", 4, 4));
    check(x && x->text && c.comp_start() == 3 && c.comp_end() == 4, "the composition moves with text put before it");
}

void attributes() {
    auto sys = [](int index) -> uint32_t { return index == 13 ? 0xff0078d7u : 0xffffffffu; };
    DisplayAttribute input;
    input.line_style = DisplayAttribute::kDot;
    input.attr = DisplayAttribute::kInput;
    Run r = style_run(input, 2, 5, sys);
    check(r.flags == Run::kUnderline && r.start == 2 && r.end == 5, "text being typed: underlined");
    DisplayAttribute target;
    target.line_style = DisplayAttribute::kSolid;
    target.bold_line = true;
    target.attr = DisplayAttribute::kTargetConverted;
    r = style_run(target, 0, 2, sys);
    check((r.flags & Run::kTarget) && (r.flags & Run::kThick) && (r.flags & Run::kBackground) &&
              r.background == 0xff0078d7u && r.foreground == 0xffffffffu,
          "the clause being converted: highlighted, as selected text is");
    DisplayAttribute colored;
    colored.text_type = DisplayAttribute::kColorRef;
    colored.text = 0x000000ffu;  // COLORREF red
    colored.back_type = DisplayAttribute::kColorRef;
    colored.back = 0x00ff0000u;  // COLORREF blue
    colored.attr = DisplayAttribute::kTargetConverted;
    r = style_run(colored, 0, 1, sys);
    check(r.foreground == 0xffff0000u && r.background == 0xff0000ffu, "a text service's own colors, as ARGB");

    Document d;
    d.reset(snap(1, 0, 0, u"", 0, 0));
    d.replace(0, 0, u"きょうは");
    d.compose(0, 4);
    d.style({style_run(target, 0, 2, sys), style_run(input, 2, 4, sys), style_run(input, 3, 9, sys)});
    auto e = d.take_edit();
    check(e && e->runs.size() == 3 && e->runs[2].end == 4, "runs travel with the edit, inside the composition");
    d.style({style_run(input, 0, 2, sys), style_run(target, 2, 4, sys)});
    e = d.take_edit();
    check(e && e->text == u"きょうは" && e->start == 0 && e->end == 4 && e->runs.size() == 2 &&
              (e->runs[1].flags & Run::kTarget),
          "the target clause moving rewrites the composition with its new look");
}

void geometry() {
    // "ab" left to right at x 10..30, then a Hebrew word right to left: "של" at x 60..40.
    Layout l;
    l.has_editor = true;
    l.editor = Box{0, 0, 100, 40};
    l.first = 10;
    l.boxes = {Box{10, 5, 20, 25}, Box{20, 5, 30, 25}, Box{30, 5, 30, 25}, Box{50, 5, 60, 25}, Box{40, 5, 50, 25}};
    l.rtl = {false, false, false, true, true};
    bool clipped = true;
    auto b = extent(l, 10, 12, &clipped);
    check(b && b->left == 10 && b->right == 30 && b->top == 5 && b->bottom == 25 && !clipped, "a range's box");
    b = extent(l, 13, 15, &clipped);
    check(b && b->left == 40 && b->right == 60, "a right-to-left word's box");
    b = extent(l, 11, 11, &clipped);
    check(b && b->left == 20 && b->right == 20, "a caret before a left-to-right character: its left edge");
    b = extent(l, 13, 13, &clipped);
    check(b && b->left == 60 && b->right == 60, "before a right-to-left character: its right edge");
    b = extent(l, 15, 15, &clipped);
    check(b && b->left == 40, "after the last, right-to-left: its left edge");
    check(!extent(l, 30, 32, &clipped), "characters not measured: no box");
    b = extent(l, 5, 11, &clipped);
    check(b && b->left == 10 && b->right == 20, "a range half measured: the measured part");
    l.editor = Box{0, 0, 15, 40};
    b = extent(l, 10, 12, &clipped);
    check(b && clipped && b->right == 15, "scrolled partly out of the editor: clipped to it");

    l.editor = Box{0, 0, 100, 40};
    check(position_at(l, 12, 10, false, false) == 10, "a point on a character");
    check(position_at(l, 18, 10, true, false) == 11, "rounded to its nearer edge");
    check(position_at(l, 58, 10, true, false) == 13, "right to left: the right half is the character's start");
    check(position_at(l, 42, 10, true, false) == 15, "and the left half its end");
    check(position_at(l, 90, 30, false, false) == -1, "nowhere");
    check(position_at(l, 90, 10, false, true) == 13, "nowhere, nearest: the closest character");
}

}  // namespace

int main() {
    field_policies();
    compositions();
    partial_commit_and_sequences();
    selection_and_reconversion();
    offsets_and_emoji();
    external_changes();
    attributes();
    geometry();
    std::printf("%d text input model check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
