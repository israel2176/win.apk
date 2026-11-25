// The last snapshot AccessibilityTreeTest's views gave (AOSP 14's views on a plain JVM), as the host
// reads it (windows/host/uia_tree.h): what each node is for UI Automation and how its text reads.
// The Java side and the host's model meet here on a real tree rather than a hand-made one.
//
//   c++ -std=c++17 -I windows/host snapshot_check.cpp windows/host/uia_tree.cpp -o snapshot_check
//   snapshot_check <snapshot.txt>
#include "uia_tree.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace winapk::host::uia;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s%s\n", ok ? "  ok   " : "  FAIL ", what);
    if (!ok) ++g_failures;
}

// UTF-8 to UTF-16 (the snapshot is the Java side's text, written as UTF-8).
std::wstring wide(const std::string& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        unsigned c = static_cast<unsigned char>(s[i]);
        unsigned cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0f; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        i += static_cast<size_t>(n);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (cp >> 10));
            out += static_cast<wchar_t>(0xDC00 + (cp & 0x3ff));
        } else {
            out += static_cast<wchar_t>(cp);
        }
    }
    return out;
}

const Node* by_name(const Tree& t, const std::wstring& name) {
    for (const Node& n : t.nodes) {
        if (n.name == name) return &n;
    }
    return nullptr;
}

const Node* by_class(const Tree& t, const std::wstring& cls) {
    for (const Node& n : t.nodes) {
        if (n.class_name == cls) return &n;
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: snapshot_check <snapshot.txt>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    Tree t;
    check(parse(wide(ss.str()), &t), "the Java side's snapshot parses");
    check(t.windows.size() == 1, "one window");
    if (t.windows.empty()) return 1;
    const Node* root = t.find(t.windows[0].root);
    check(root && root->name == L"Settings" && root->control_type == control::kPane, "the window: a pane named by its title");

    const Node* row = by_name(t, L"Wi-Fi, Connected");
    check(row && row->control_type == control::kButton && (row->patterns & kPatInvoke),
          "the row: a button that speaks its two texts and invokes");
    const Node* sw = by_class(t, L"android.widget.Switch");
    check(sw && sw->control_type == control::kButton && (sw->patterns & kPatToggle) && sw->control_element,
          "the switch: a toggle button, kept in the control view");
    const Node* wifi = nullptr;
    for (const Node& n : t.nodes) {
        if (n.text == L"Wi-Fi") wifi = &n;
    }
    check(wifi && !wifi->control_element && !wifi->content_element, "the row's own text leaves the control view");

    const Node* edit = by_class(t, L"android.widget.EditText");
    check(edit && edit->control_type == control::kEdit, "the editor: an edit");
    if (edit) {
        check(edit->name == L"Name", "named by its label (setLabelFor)");
        check(edit->help == L"First and last name", "its hint is its help");
        check((edit->patterns & kPatValue) && (edit->patterns & kPatText), "Value and Text");
        TextModel m(*edit);
        check(m.text() == L"Noa Cohen", "the text the client set");
        check(m.next(Unit::kWord, 0) == 4, "a word with its space, by BreakIterator's breaks");
        int s = 5, e = 5;
        m.expand(Unit::kLine, &s, &e);
        check(s == 0 && e == 9, "one line from the layout");
        std::vector<Box> caret = m.boxes(3, 3);
        check(caret.size() == 1 && caret[0].r - caret[0].l == 1 && caret[0].l > edit->bounds.l &&
                  caret[0].l < edit->bounds.r,
              "the caret's box inside the editor");
        check(edit->sel_start == 0 && edit->sel_end == 3, "the selection the client set");
    }
    const Node* box = by_class(t, L"android.widget.CheckBox");
    check(box && box->control_type == control::kCheckBox && box->name == L"Remember me" && (box->patterns & kPatToggle),
          "the checkbox");
    const Node* seek = by_class(t, L"android.widget.SeekBar");
    check(seek && seek->control_type == control::kSlider && (seek->patterns & kPatRangeValue) && seek->range.now == 7,
          "the seek bar: a slider at 7");
    const Node* key = by_name(t, L"Key B");
    check(key && key->control_type == control::kButton && (key->patterns & kPatInvoke), "a virtual key: a button");
    const Node* hebrew = nullptr;
    for (const Node& n : t.nodes) {
        if (!n.text.empty() && n.text[0] == 0x05E9) hebrew = &n;
    }
    check(hebrew && hebrew->has(kRtl) && (hebrew->patterns & kPatText), "the Hebrew text: right to left, with Text");
    if (hebrew) {
        TextModel m(*hebrew);
        check(m.next(Unit::kWord, 0) == 5, "its first word with its space");
    }
    check(t.focused != 0 && t.find(t.focused) != nullptr, "the focused node is in the tree");
    if (g_failures) {
        std::printf("%d FAILED\n", g_failures);
        return 1;
    }
    std::printf("snapshot check: ALL PASSED\n");
    return 0;
}
