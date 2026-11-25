// uia_tree_test.cpp -- what a screen reader is told of an app's views, with no window and no
// UI Automation (host/uia_tree.h): the snapshot's text both ways, the names TalkBack would speak,
// control types and patterns, the events between two snapshots, and the Text pattern's units
// (Hebrew points kept with their letter, the app's word breaks, the Layout's lines, rectangles).
// Runs anywhere: CI's Windows unit tests, and on Linux as
//   g++ -std=c++17 -I windows/host windows/test/uia_tree_test.cpp windows/host/uia_tree.cpp
#include "uia_tree.h"

#include <cstdio>
#include <string>

using namespace winapk::host::uia;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

std::string narrow(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) out += c < 0x80 ? static_cast<char>(c) : '?';
    return out;
}

void check_eq(const std::wstring& got, const std::wstring& want, const char* what) {
    if (got != want) {
        std::printf("FAIL: %s: got \"%s\", want \"%s\"\n", what, narrow(got).c_str(), narrow(want).c_str());
        ++g_failures;
    }
}

void check_int(long long got, long long want, const char* what) {
    if (got != want) {
        std::printf("FAIL: %s: got %lld, want %lld\n", what, got, want);
        ++g_failures;
    }
}

Node node(int64_t id, int64_t parent, const wchar_t* cls, uint32_t flags = kEnabled | kVisible) {
    Node n;
    n.id = id;
    n.parent = parent;
    n.class_name = cls;
    n.flags = flags;
    n.bounds = Box{0, 0, 100, 40};
    return n;
}

void add_action(Node* n, int id) {
    n->action_ids.push_back(id);
    n->can |= can_of_action(id);
}

// A settings-like screen: a window root, a heading, a clickable row with a title and a summary
// and a switch inside, a labelled editor, a seek bar, a list with selectable items, a scroll view.
Tree screen() {
    Tree t;
    t.focused = 40;
    t.locale = L"he-IL";
    t.windows.push_back(Window{1, 1, L"Settings", true});
    Node root = node(1, 0, L"com.android.internal.policy.DecorView");
    root.children = {10, 20, 30, 40, 50, 60, 70};
    root.bounds = Box{0, 0, 720, 1280};
    Node heading = node(10, 1, L"android.widget.TextView", kEnabled | kVisible | kHeading);
    heading.text = L"Network";
    Node row = node(20, 1, L"android.widget.LinearLayout", kEnabled | kVisible | kClickable | kFocusable);
    add_action(&row, action::kClick);
    row.children = {21, 22, 23};
    Node title = node(21, 20, L"android.widget.TextView");
    title.text = L"Wi-Fi";
    Node summary = node(22, 20, L"android.widget.TextView");
    summary.text = L"Connected";
    Node sw = node(23, 20, L"android.widget.Switch", kEnabled | kVisible | kCheckable | kChecked | kFocusable | kClickable);
    sw.state = L"On";
    add_action(&sw, action::kClick);
    Node label = node(30, 1, L"android.widget.TextView");
    label.text = L"Name";
    Node edit = node(40, 1, L"android.widget.EditText",
                     kEnabled | kVisible | kEditable | kFocusable | kFocused | kClickable);
    edit.text = L"Dana";
    edit.hint = L"Your name";
    edit.labeled_by = 30;
    edit.sel_start = edit.sel_end = 4;
    add_action(&edit, action::kSetText);
    add_action(&edit, action::kSetSelection);
    add_action(&edit, action::kClick);
    Node seek = node(50, 1, L"android.widget.SeekBar", kEnabled | kVisible | kFocusable);
    seek.range = RangeInfo{true, 0, 0, 100, 40};
    add_action(&seek, action::kSetProgress);
    add_action(&seek, action::kScrollForward);
    Node list = node(60, 1, L"androidx.recyclerview.widget.RecyclerView", kEnabled | kVisible | kScrollable);
    list.collection = CollectionInfo{true, 2, 1, 1, false};
    add_action(&list, action::kScrollForward);
    add_action(&list, action::kScrollDown);
    list.children = {61, 62};
    Node a = node(61, 60, L"android.widget.FrameLayout", kEnabled | kVisible | kClickable);
    a.item = ItemInfo{true, 0, 1, 0, 1, false, true};
    a.text = L"Apples";
    add_action(&a, action::kClick);
    add_action(&a, action::kShowOnScreen);
    Node b = node(62, 60, L"android.widget.FrameLayout", kEnabled | kVisible | kClickable);
    b.item = ItemInfo{true, 1, 1, 0, 1, false, false};
    b.text = L"Pears";
    add_action(&b, action::kClick);
    Node pane = node(70, 1, L"android.widget.ScrollView", kEnabled | kVisible | kScrollable | kFocusable);
    pane.children = {71};
    Node plain = node(71, 70, L"android.widget.LinearLayout");
    plain.children = {72};
    Node pic = node(72, 71, L"android.widget.ImageView");
    for (Node* n : {&root, &heading, &row, &title, &summary, &sw, &label, &edit, &seek, &list, &a, &b, &pane, &plain, &pic}) {
        t.nodes.push_back(*n);
    }
    derive(&t);
    return t;
}

void test_round_trip() {
    Tree t = screen();
    t.nodes[t.index[40]].text = L"line one\nline \x1e two\twith a tab";
    t.nodes[t.index[40]].line_starts = {0, 9};
    t.nodes[t.index[40]].line_boxes = {Box{1, 2, 3, 4}, Box{5, 6, 7, 8}};
    t.nodes[t.index[50]].range.now = 12.5;
    std::wstring text = encode(t);
    Tree back;
    check(parse(text, &back), "a written snapshot parses");
    check_int(back.nodes.size(), t.nodes.size(), "every node comes back");
    check_int(back.focused, 40, "the focused node");
    check_eq(back.locale, L"he-IL", "the locale");
    check_int(back.windows.size(), 1, "one window");
    check_eq(back.windows[0].title, L"Settings", "the window's title");
    const Node* e = back.find(40);
    check(e != nullptr, "the editor is found by id");
    if (e) {
        check_eq(e->text, L"line one\nline   two\twith a tab", "a line break and a tab survive; separators become spaces");
        check_int(e->line_starts.size(), 2, "line starts");
        check(e->line_boxes.size() == 2 && e->line_boxes[1] == Box{5, 6, 7, 8}, "line boxes");
        check_int(e->labeled_by, 30, "labelled by");
        check(e->can_do(kCanSetText) && e->can_do(kCanSetSelection), "actions from their ids");
    }
    const Node* s = back.find(50);
    check(s && s->range.present && s->range.now == 12.5 && s->range.max == 100, "a range with a fraction");
    const Node* l = back.find(60);
    check(l && l->collection.present && l->collection.rows == 2 && l->collection.selection_mode == 1, "a collection");
    check(encode(back) == encode(t), "writing it again gives the same text");

    Tree bad;
    check(!parse(L"", &bad), "no header: not a snapshot");
    check(!parse(L"S\x1f" L"2\x1f" L"0\x1f" L"en", &bad), "another version: not a snapshot");
    check(!parse(L"S\x1f" L"1\x1f" L"0\x1f" L"en\x1eN\x1f" L"5", &bad), "a short node record: not a snapshot");
    Tree empty;
    check(parse(L"S\x1f" L"1\x1f" L"0\x1f" L"en\x1eX\x1fwhatever", &empty), "an unknown record is skipped");
}

void test_names_and_types() {
    Tree t = screen();
    const Node* root = t.find(1);
    check_eq(root->name, L"Settings", "a window's root is named by its title");
    check_int(root->control_type, control::kPane, "a window's root is a pane");

    const Node* heading = t.find(10);
    check_eq(heading->name, L"Network", "a heading's name");
    check_int(heading->control_type, control::kText, "a heading is text");

    const Node* row = t.find(20);
    check_eq(row->name, L"Wi-Fi, Connected", "a clickable row speaks its plain texts, not the switch's");
    check_int(row->control_type, control::kButton, "a clickable layout is a button");
    check(row->patterns & kPatInvoke, "a clickable row invokes");
    const Node* title = t.find(21);
    check(!title->control_element && !title->content_element, "a text the row speaks leaves the control view");
    const Node* sw = t.find(23);
    check(sw->control_element, "the switch inside the row stays");
    check_int(sw->control_type, control::kButton, "a switch is a button");
    check((sw->patterns & kPatToggle) && !(sw->patterns & kPatInvoke), "a switch toggles, it does not invoke");
    check_eq(sw->status, L"", "a checkable's state description is not said twice");

    const Node* edit = t.find(40);
    check_eq(edit->name, L"Name", "an editor is named by the view labelling it");
    check_eq(edit->help, L"Your name", "its hint is its help");
    check_int(edit->control_type, control::kEdit, "an editor is an edit");
    check((edit->patterns & kPatValue) && (edit->patterns & kPatText), "an editor has Value and Text");
    check(!(edit->patterns & kPatInvoke), "an editor does not invoke");

    const Node* seek = t.find(50);
    check_int(seek->control_type, control::kSlider, "a seek bar is a slider");
    check(seek->patterns & kPatRangeValue, "a seek bar has RangeValue");
    check(seek->patterns & kPatScroll, "a seek bar's scroll action shows");

    const Node* list = t.find(60);
    check_int(list->control_type, control::kList, "a recycler view is a list");
    check((list->patterns & kPatScroll) && (list->patterns & kPatSelection), "a list scrolls and selects");
    const Node* a = t.find(61);
    check_int(a->control_type, control::kListItem, "a collection item is a list item");
    check((a->patterns & kPatSelectionItem) && (a->patterns & kPatScrollItem), "an item selects and scrolls into view");

    const Node* pane = t.find(70);
    check_eq(pane->name, L"", "a scroll view does not take its content's text");
    const Node* plain = t.find(71);
    check(!plain->control_element, "a layout with nothing to say leaves the control view");
    const Node* pic = t.find(72);
    check(pic->control_element && !pic->content_element, "an unlabelled picture is not content");

    // A description wins over everything under it.
    Tree u = screen();
    u.nodes[u.index[20]].description = L"Wireless";
    derive(&u);
    check_eq(u.find(20)->name, L"Wireless", "a content description names the row");
    check(u.find(21)->control_element, "a row with a description does not take its children's text");

    // A password's text is what the app gives (dots); an editor showing its hint has no value.
    Tree v = screen();
    Node& e = v.nodes[v.index[40]];
    e.labeled_by = 0;
    e.text = L"";
    e.flags |= kShowingHint;
    derive(&v);
    check_eq(v.find(40)->name, L"Your name", "an unlabelled editor is named by its hint");
    check_eq(v.find(40)->help, L"", "and its hint is not its help too");

    // Roles by class.
    struct {
        const wchar_t* cls;
        uint32_t flags;
        int want;
    } cases[] = {
        {L"android.widget.CheckBox", kCheckable, control::kCheckBox},
        {L"android.widget.RadioButton", kCheckable, control::kRadioButton},
        {L"android.widget.Spinner", kClickable, control::kComboBox},
        {L"android.widget.ProgressBar", 0, control::kProgressBar},
        {L"android.widget.ImageView", 0, control::kImage},
        {L"android.widget.ImageView", kClickable, control::kButton},
        {L"android.app.ActionBar$Tab", kClickable, control::kTabItem},
        {L"android.widget.Toolbar", 0, control::kToolBar},
        {L"android.webkit.WebView", 0, control::kPane},
        {L"android.view.View", kClickable, control::kButton},
        {L"android.widget.TextView", 0, control::kText},
    };
    for (const auto& c : cases) {
        Tree w;
        w.windows.push_back(Window{1, 1, L"", true});
        Node r = node(1, 0, L"android.widget.FrameLayout");
        r.children = {2};
        Node n = node(2, 1, c.cls, kEnabled | kVisible | c.flags);
        n.text = L"x";
        w.nodes = {r, n};
        derive(&w);
        if (w.find(2)->control_type != c.want) {
            std::printf("FAIL: %s is control type %d, want %d\n", narrow(c.cls).c_str(), w.find(2)->control_type, c.want);
            ++g_failures;
        }
    }
    // A radio button selects (SelectionItem), it does not toggle; its group is a selection.
    Tree g;
    g.windows.push_back(Window{1, 1, L"", true});
    Node r = node(1, 0, L"android.widget.RadioGroup");
    r.children = {2, 3};
    Node r1 = node(2, 1, L"android.widget.RadioButton", kEnabled | kVisible | kCheckable | kChecked | kClickable);
    Node r2 = node(3, 1, L"android.widget.RadioButton", kEnabled | kVisible | kCheckable | kClickable);
    g.nodes = {r, r1, r2};
    derive(&g);
    check((g.find(2)->patterns & kPatSelectionItem) && !(g.find(2)->patterns & kPatToggle), "a radio button selects");
    check(g.find(1)->patterns & kPatSelection, "a radio group is a selection");
}

void test_events() {
    Tree a = screen();
    check(diff(nullptr, a).empty(), "the first snapshot raises nothing");
    check(diff(&a, a).empty(), "the same snapshot raises nothing");

    Tree b = screen();
    b.focused = 50;
    b.nodes[b.index[40]].flags &= ~kFocused;
    b.nodes[b.index[23]].flags &= ~kChecked;
    b.nodes[b.index[50]].range.now = 60;
    b.nodes[b.index[40]].text = L"Danah";
    b.nodes[b.index[40]].sel_start = b.nodes[b.index[40]].sel_end = 5;
    b.nodes[b.index[62]].item.selected = true;
    b.nodes[b.index[61]].item.selected = false;
    b.nodes[b.index[22]].text = L"Off";
    derive(&b);
    std::vector<Event> ev = diff(&a, b);
    auto has = [&](EventKind k, int64_t id) {
        for (const Event& e : ev) {
            if (e.kind == k && e.id == id) return true;
        }
        return false;
    };
    check(has(EventKind::kFocus, 50), "focus moved");
    check(has(EventKind::kToggle, 23), "the switch toggled");
    check(has(EventKind::kRangeValue, 50), "the seek bar moved");
    check(has(EventKind::kValue, 40) && has(EventKind::kTextChanged, 40), "the editor's text changed");
    check(!has(EventKind::kTextSelection, 40), "no selection event for an editor that lost focus");
    check(has(EventKind::kSelected, 62) && !has(EventKind::kSelected, 61), "the newly selected item");
    check(has(EventKind::kName, 20), "the row's name follows its summary");
    check(ev.back().kind == EventKind::kFocus || ev.back().kind == EventKind::kLiveRegion ||
              ev.back().kind == EventKind::kNotification,
          "focus comes after the property changes");

    // The caret moved in the focused editor.
    Tree c = screen();
    c.nodes[c.index[40]].sel_start = 0;
    derive(&c);
    ev = diff(&a, c);
    check(has(EventKind::kTextSelection, 40), "the caret moved in the focused editor");

    // A live region's text changed; a node left; a list's children changed.
    Tree d = screen();
    d.nodes[d.index[10]].live = 1;
    Tree e = d;
    e.nodes[e.index[10]].text = L"Network (2)";
    e.nodes[e.index[60]].children = {61};
    e.nodes.erase(e.nodes.begin() + static_cast<long>(e.index[62]));
    derive(&d);
    derive(&e);
    ev = diff(&d, e);
    bool live = false;
    for (const Event& x : ev) {
        if (x.kind == EventKind::kLiveRegion && x.id == 10 && x.text == L"Network (2)" && !x.assertive) live = true;
    }
    check(live, "a polite live region is spoken with its new text");
    check(has(EventKind::kStructure, 60), "the list's children changed");
    check(has(EventKind::kGone, 62), "the removed item is gone");

    // A toast appears: spoken. A new window: the root's children changed.
    Tree f = screen();
    f.windows.push_back(Window{900, kWindowTypeToast, L"Toast", false});
    Node toast = node(900, 0, L"android.widget.LinearLayout");
    toast.children = {901};
    Node msg = node(901, 900, L"android.widget.TextView");
    msg.text = L"Saved";
    f.nodes.push_back(toast);
    f.nodes.push_back(msg);
    derive(&f);
    ev = diff(&a, f);
    bool spoken = false;
    for (const Event& x : ev) {
        if (x.kind == EventKind::kNotification && x.text == L"Saved") spoken = true;
    }
    check(spoken, "a toast is spoken");
    check(has(EventKind::kStructure, 0), "a new window changes the root's children");

    // Many lists at once: one change of the whole tree.
    Tree g = screen(), h = screen();
    for (int i = 0; i < 20; ++i) {
        g.nodes[0].children.push_back(1000 + i);
        h.nodes[0].children.push_back(1000 + i);
        g.nodes.push_back(node(1000 + i, 1, L"android.widget.FrameLayout"));
        Node with = node(1000 + i, 1, L"android.widget.FrameLayout");
        with.children = {2000 + i};
        h.nodes.push_back(with);
        h.nodes.push_back(node(2000 + i, 1000 + i, L"android.view.View"));
    }
    derive(&g);
    derive(&h);
    ev = diff(&g, h);
    int structures = 0;
    for (const Event& x : ev) structures += x.kind == EventKind::kStructure;
    check(structures == 1 && has(EventKind::kStructure, 0), "twenty lists changing are one change of the root");
}

void test_text_units() {
    Node n = node(5, 1, L"android.widget.EditText", kEnabled | kVisible | kEditable);
    // "שָׁלוֹם עולם" : shin + qamats + shin dot, lamed, vav + holam, final mem; a space; four letters.
    n.text = L"\x05E9\x05B8\x05C1\x05DC\x05D5\x05B9\x05DD \x05E2\x05D5\x05DC\x05DD";
    TextModel m(n);
    check_int(m.length(), 12, "the text's length");
    check_int(m.next(Unit::kCharacter, 0), 3, "a letter keeps its vowel point and dot");
    check_int(m.next(Unit::kCharacter, 3), 4, "a plain letter is one character");
    check_int(m.next(Unit::kCharacter, 4), 6, "vav with holam is one character");
    check_int(m.prev(Unit::kCharacter, 3), 0, "back over a pointed letter");
    check(!m.is_boundary(Unit::kCharacter, 1), "no boundary before a point");
    // Words, with the fallback breaks: the space joins the first word.
    check_int(m.next(Unit::kWord, 0), 8, "the second word starts after the space");
    int s = 2, e = 2;
    m.expand(Unit::kWord, &s, &e);
    check(s == 0 && e == 8, "a word with its trailing space");
    s = 1, e = 1;
    m.expand(Unit::kCharacter, &s, &e);
    check(s == 0 && e == 3, "the character around an offset inside it");

    // The app's word breaks (BreakIterator's) win.
    Node w = node(6, 1, L"android.widget.EditText", kEnabled | kVisible | kEditable);
    w.text = L"don't stop, ok";
    w.word_breaks = {0, 5, 6, 10, 11, 12, 14};
    TextModel wm(w);
    check_int(wm.next(Unit::kWord, 0), 6, "\"don't \" is one word");
    check_int(wm.next(Unit::kWord, 6), 10, "\"stop\"");
    check_int(wm.next(Unit::kWord, 10), 12, "\", \": punctuation with its space");
    check_int(wm.next(Unit::kWord, 12), 14, "\"ok\" to the end");

    // Lines from the layout; paragraphs from line breaks.
    Node l = node(7, 1, L"android.widget.EditText", kEnabled | kVisible | kEditable | kMultiLine);
    l.text = L"one two three\nfour";
    l.line_starts = {0, 8, 14};
    l.line_boxes = {Box{10, 100, 90, 120}, Box{10, 120, 70, 140}, Box{10, 140, 50, 160}};
    for (int i = 0; i <= 18; ++i) l.char_x.push_back(10 + 10 * (i < 8 ? i : i < 14 ? i - 8 : i - 14));
    TextModel lm(l);
    check_int(lm.next(Unit::kLine, 0), 8, "a wrapped line");
    check_int(lm.next(Unit::kParagraph, 0), 14, "a paragraph runs to the line break");
    s = 9, e = 9;
    lm.expand(Unit::kLine, &s, &e);
    check(s == 8 && e == 14, "the line around an offset");
    s = 18, e = 18;
    lm.expand(Unit::kLine, &s, &e);
    check(s == 14 && e == 18, "at the end, the last line");
    s = 0, e = 18;
    lm.expand(Unit::kDocument, &s, &e);
    check(s == 0 && e == 18, "the document");
    std::vector<Box> boxes = lm.boxes(4, 16);
    check_int(boxes.size(), 3, "a range over three lines has three rectangles");
    if (boxes.size() == 3) {
        check(boxes[0].l == 50 && boxes[0].r == 90 && boxes[0].t == 100, "the first line from offset 4");
        check(boxes[1].l == 10 && boxes[1].r == 70, "the whole second line");
        check(boxes[2].l == 10 && boxes[2].r == 30, "the third line to offset 16");
    }
    boxes = lm.boxes(9, 9);
    check(boxes.size() == 1 && boxes[0].l == 20 && boxes[0].r == 21, "an empty range is a caret");
    check_int(lm.offset_at(31, 125), 10, "the offset nearest a point");
    check_int(lm.offset_at(0, 0), 0, "a point above the text: the first line's start");

    // Move: a degenerate range moves by units; a range moves and covers the unit.
    s = e = 0;
    check_int(lm.move(Unit::kWord, 2, &s, &e), 2, "two words forward");
    check(s == 8 && e == 8, "the range stays empty");
    s = 0, e = 4;
    check_int(lm.move(Unit::kWord, 1, &s, &e), 1, "a word range to the next word");
    check(s == 4 && e == 8, "it covers that word");
    s = 14, e = 18;
    check_int(lm.move(Unit::kWord, 1, &s, &e), 0, "no word after the last");
    s = e = 9;
    check_int(lm.move(Unit::kWord, -1, &s, &e), -1, "inside a word, back to its start is one");
    check(s == 8, "at the word's start");
    s = e = 3;
    check_int(lm.move(Unit::kCharacter, -10, &s, &e), -3, "back no further than the start");
    s = e = 3;
    check_int(lm.move(Unit::kCharacter, 100, &s, &e), 15, "forward no further than the end");
    check(s == 18, "an empty range may sit at the end");
    int p = 0;
    check_int(lm.move_endpoint(Unit::kLine, 2, &p), 2, "an endpoint by two lines");
    check_int(p, 14, "at the third line");

    // FindText.
    check_int(lm.find(L"two", 0, 18, false, false), 4, "find forward");
    check_int(lm.find(L"O", 0, 18, true, true), 15, "find backward, ignoring case");
    check_int(lm.find(L"zzz", 0, 18, false, false), -1, "not found");
    check_int(lm.find(L"one", 1, 18, false, false), -1, "only inside the range");

    // A layout from an older text (the editor changed after the walk) is not used.
    Node stale = l;
    stale.text = L"ab";
    TextModel sm(stale);
    check_int(sm.next(Unit::kLine, 0), 2, "a stale layout gives way to paragraphs");
    check_int(sm.boxes(0, 1).size(), 1, "and the node's bounds");
}

}  // namespace

int main() {
    test_round_trip();
    test_names_and_types();
    test_events();
    test_text_units();
    if (g_failures) {
        std::printf("%d FAILED\n", g_failures);
        return 1;
    }
    std::printf("uia_tree: ALL PASSED\n");
    return 0;
}
