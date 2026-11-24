// uia_tree.cpp -- the snapshot, what each node is for UI Automation, events, text units.
// See uia_tree.h. No Windows call: test/uia_tree_test.cpp runs it anywhere.
#include "uia_tree.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <functional>
#include <type_traits>
#include <unordered_set>

namespace winapk::host::uia {
namespace {

constexpr wchar_t kRecord = L'\x1e';
constexpr wchar_t kField = L'\x1f';

std::vector<std::wstring> split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t from = 0;
    for (;;) {
        size_t at = s.find(sep, from);
        if (at == std::wstring::npos) {
            out.push_back(s.substr(from));
            return out;
        }
        out.push_back(s.substr(from, at - from));
        from = at + 1;
    }
}

int64_t to_i64(const std::wstring& s) { return s.empty() ? 0 : std::wcstoll(s.c_str(), nullptr, 10); }
int to_int(const std::wstring& s) { return static_cast<int>(to_i64(s)); }
double to_double(const std::wstring& s) { return s.empty() ? 0.0 : std::wcstod(s.c_str(), nullptr); }

template <typename T>
std::vector<T> numbers(const std::wstring& s) {
    std::vector<T> out;
    if (s.empty()) return out;
    for (const std::wstring& part : split(s, L',')) {
        if constexpr (std::is_floating_point_v<T>) {
            out.push_back(static_cast<T>(to_double(part)));
        } else {
            out.push_back(static_cast<T>(to_i64(part)));
        }
    }
    return out;
}

std::wstring clean(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& c : out) {
        if (c == kRecord || c == kField) c = L' ';
    }
    return out;
}

template <typename T>
std::wstring join_numbers(const std::vector<T>& v) {
    std::wstring out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += L',';
        out += std::to_wstring(v[i]);
    }
    return out;
}

std::wstring number(double d) {
    if (d == std::floor(d) && std::fabs(d) < 1e15) return std::to_wstring(static_cast<long long>(d));
    std::wstring s = std::to_wstring(d);
    while (!s.empty() && s.back() == L'0') s.pop_back();
    return s;
}

// White space, whatever the C library's locale (glibc's "C" knows only ASCII).
bool is_space(wchar_t c) {
    const unsigned u = static_cast<unsigned>(c);
    return u == 0x20 || (u >= 0x09 && u <= 0x0D) || u == 0xA0 || (u >= 0x2000 && u <= 0x200A) || u == 0x2028 ||
           u == 0x2029 || u == 0x202F || u == 0x205F || u == 0x3000;
}

bool blank(const std::wstring& s) {
    for (wchar_t c : s) {
        if (!is_space(c)) return false;
    }
    return true;
}

std::wstring trimmed(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) ++a;
    while (b > a && is_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// The class's simple name: "android.widget.Button" -> "Button", "android.app.ActionBar$Tab" -> "ActionBar$Tab".
std::wstring simple_name(const std::wstring& cls) {
    size_t dot = cls.rfind(L'.');
    return dot == std::wstring::npos ? cls : cls.substr(dot + 1);
}

bool one_of(const std::wstring& s, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names) {
        if (s == n) return true;
    }
    return false;
}

// A node TalkBack stops on by itself: it speaks for itself, so a row around it does not take its text.
bool stops_reading(const Node& n) {
    return n.has(kClickable) || n.has(kLongClickable) || n.has(kCheckable) || n.has(kEditable) ||
           n.has(kFocusable) || n.has(kScreenReaderFocusable) || n.has(kScrollable) || n.collection.present ||
           n.range.present;
}

}  // namespace

uint32_t can_of_action(int id) {
    switch (id) {
        case action::kClick: return kCanClick;
        case action::kLongClick: return kCanLongClick;
        case action::kFocus: return kCanFocus;
        case action::kSetText: return kCanSetText;
        case action::kSetSelection: return kCanSetSelection;
        case action::kScrollForward: return kCanScrollForward;
        case action::kScrollBackward: return kCanScrollBackward;
        case action::kScrollUp: return kCanScrollUp;
        case action::kScrollDown: return kCanScrollDown;
        case action::kScrollLeft: return kCanScrollLeft;
        case action::kScrollRight: return kCanScrollRight;
        case action::kShowOnScreen: return kCanShowOnScreen;
        case action::kSetProgress: return kCanSetProgress;
        case action::kExpand: return kCanExpand;
        case action::kCollapse: return kCanCollapse;
        case action::kSelect: return kCanSelect;
        case action::kClearSelection: return kCanClearSelection;
        case action::kDismiss: return kCanDismiss;
        case action::kPageUp: return kCanPageUp;
        case action::kPageDown: return kCanPageDown;
        default: return 0;
    }
}

// ---- Parsing and writing ------------------------------------------------------------------------

bool parse(const std::wstring& text, Tree* out) {
    Tree tree;
    bool header = false;
    for (const std::wstring& record : split(text, kRecord)) {
        if (record.empty()) continue;
        std::vector<std::wstring> f = split(record, kField);
        if (f[0] == L"S") {
            if (f.size() < 4 || f[1] != L"1") return false;
            tree.focused = to_i64(f[2]);
            tree.locale = f[3];
            header = true;
        } else if (f[0] == L"W") {
            if (f.size() < 5) return false;
            Window w;
            w.root = to_i64(f[1]);
            w.type = to_int(f[2]);
            w.title = f[3];
            w.focused = f[4] == L"1";
            tree.windows.push_back(std::move(w));
        } else if (f[0] == L"N") {
            if (f.size() < 27) return false;
            Node n;
            n.id = to_i64(f[1]);
            n.parent = to_i64(f[2]);
            n.flags = static_cast<uint32_t>(to_i64(f[3]));
            std::vector<int> b = numbers<int>(f[4]);
            if (b.size() == 4) n.bounds = Box{b[0], b[1], b[2], b[3]};
            n.class_name = f[5];
            n.role = f[6];
            n.description = f[7];
            n.text = f[8];
            n.hint = f[9];
            n.tooltip = f[10];
            n.state = f[11];
            n.pane_title = f[12];
            n.error = f[13];
            n.resource_id = f[14];
            n.live = to_int(f[15]);
            std::vector<double> r = numbers<double>(f[16]);
            if (r.size() == 4) n.range = RangeInfo{true, static_cast<int>(r[0]), r[1], r[2], r[3]};
            std::vector<int> c = numbers<int>(f[17]);
            if (c.size() == 4) n.collection = CollectionInfo{true, c[0], c[1], c[3], c[2] != 0};
            std::vector<int> it = numbers<int>(f[18]);
            if (it.size() == 6) n.item = ItemInfo{true, it[0], it[1], it[2], it[3], it[4] != 0, it[5] != 0};
            std::vector<int> sel = numbers<int>(f[19]);
            if (sel.size() == 2) {
                n.sel_start = sel[0];
                n.sel_end = sel[1];
            }
            n.line_starts = numbers<int>(f[20]);
            std::vector<int> lb = numbers<int>(f[21]);
            for (size_t i = 0; i + 3 < lb.size(); i += 4) n.line_boxes.push_back(Box{lb[i], lb[i + 1], lb[i + 2], lb[i + 3]});
            n.char_x = numbers<int>(f[22]);
            n.word_breaks = numbers<int>(f[23]);
            n.children = numbers<int64_t>(f[24]);
            n.action_ids = numbers<int>(f[25]);
            for (int a : n.action_ids) n.can |= can_of_action(a);
            n.labeled_by = to_i64(f[26]);
            if (n.id == 0) return false;
            tree.nodes.push_back(std::move(n));
        }
        // An unknown record kind is a newer writer's: skipped.
    }
    if (!header) return false;
    derive(&tree);
    *out = std::move(tree);
    return true;
}

std::wstring encode(const Tree& tree) {
    std::wstring out;
    auto field = [&](const std::wstring& s) {
        out += kField;
        out += s;
    };
    out += L"S";
    field(L"1");
    field(std::to_wstring(tree.focused));
    field(clean(tree.locale));
    for (const Window& w : tree.windows) {
        out += kRecord;
        out += L"W";
        field(std::to_wstring(w.root));
        field(std::to_wstring(w.type));
        field(clean(w.title));
        field(w.focused ? L"1" : L"0");
    }
    for (const Node& n : tree.nodes) {
        out += kRecord;
        out += L"N";
        field(std::to_wstring(n.id));
        field(std::to_wstring(n.parent));
        field(std::to_wstring(n.flags));
        field(join_numbers(std::vector<int>{n.bounds.l, n.bounds.t, n.bounds.r, n.bounds.b}));
        for (const std::wstring* s : {&n.class_name, &n.role, &n.description, &n.text, &n.hint, &n.tooltip, &n.state,
                                      &n.pane_title, &n.error, &n.resource_id}) {
            field(clean(*s));
        }
        field(std::to_wstring(n.live));
        field(n.range.present ? std::to_wstring(n.range.type) + L"," + number(n.range.min) + L"," +
                                    number(n.range.max) + L"," + number(n.range.now)
                              : std::wstring());
        field(n.collection.present ? join_numbers(std::vector<int>{n.collection.rows, n.collection.columns,
                                                                   n.collection.hierarchical ? 1 : 0,
                                                                   n.collection.selection_mode})
                                   : std::wstring());
        field(n.item.present ? join_numbers(std::vector<int>{n.item.row, n.item.row_span, n.item.column,
                                                             n.item.column_span, n.item.heading ? 1 : 0,
                                                             n.item.selected ? 1 : 0})
                             : std::wstring());
        field(n.sel_start >= 0 || n.sel_end >= 0 ? join_numbers(std::vector<int>{n.sel_start, n.sel_end}) : std::wstring());
        field(join_numbers(n.line_starts));
        std::vector<int> lb;
        for (const Box& b : n.line_boxes) lb.insert(lb.end(), {b.l, b.t, b.r, b.b});
        field(join_numbers(lb));
        field(join_numbers(n.char_x));
        field(join_numbers(n.word_breaks));
        field(join_numbers(n.children));
        field(join_numbers(n.action_ids));
        field(std::to_wstring(n.labeled_by));
    }
    return out;
}

// ---- What each node is --------------------------------------------------------------------------

std::wstring own_label(const Tree& tree, const Node& n) {
    if (!blank(n.description)) return trimmed(n.description);
    if (!n.has(kEditable)) {
        if (!blank(n.text) && !n.has(kShowingHint)) return trimmed(n.text);
        if (!blank(n.hint)) return trimmed(n.hint);
        return std::wstring();
    }
    // An editor: its value is its text; what names it is the view labelling it, else its hint.
    if (n.labeled_by) {
        if (const Node* label = tree.find(n.labeled_by)) {
            if (label != &n) {
                std::wstring l = !blank(label->description) ? label->description : label->text;
                if (!blank(l)) return trimmed(l);
            }
        }
    }
    return trimmed(n.hint);
}

namespace {

int control_type_of(const Tree& tree, const Node& n, const Node* parent) {
    const std::wstring s = simple_name(n.class_name);
    if (n.has(kEditable)) return control::kEdit;
    if (one_of(s, {L"RadioButton", L"AppCompatRadioButton", L"MaterialRadioButton"})) return control::kRadioButton;
    if (one_of(s, {L"CheckBox", L"AppCompatCheckBox", L"MaterialCheckBox"})) return control::kCheckBox;
    if (s == L"CheckedTextView") return n.has(kCheckable) ? control::kCheckBox : control::kText;
    if (one_of(s, {L"Switch", L"SwitchCompat", L"SwitchMaterial", L"MaterialSwitch", L"ToggleButton",
                   L"CompoundButton"})) {
        return control::kButton;
    }
    if (one_of(s, {L"Button", L"ImageButton", L"AppCompatButton", L"AppCompatImageButton", L"MaterialButton",
                   L"FloatingActionButton", L"ExtendedFloatingActionButton", L"Chip"})) {
        return control::kButton;
    }
    if (one_of(s, {L"SeekBar", L"AbsSeekBar", L"RatingBar", L"AppCompatSeekBar", L"AppCompatRatingBar", L"Slider",
                   L"RangeSlider"})) {
        return control::kSlider;
    }
    if (s == L"ProgressBar") return n.can_do(kCanSetProgress) ? control::kSlider : control::kProgressBar;
    if (one_of(s, {L"Spinner", L"AppCompatSpinner"})) return control::kComboBox;
    if (one_of(s, {L"ListView", L"GridView", L"RecyclerView", L"ExpandableListView", L"AbsListView"})) {
        return n.collection.present && n.collection.rows > 1 && n.collection.columns > 1 ? control::kDataGrid
                                                                                          : control::kList;
    }
    if (s == L"TabWidget") return control::kTab;
    if (s == L"ActionBar$Tab") return control::kTabItem;  // TabLayout's tabs say they are ActionBar tabs
    if (s == L"Toolbar") return control::kToolBar;
    if (one_of(s, {L"ImageView", L"AppCompatImageView", L"ShapeableImageView"})) {
        return n.has(kClickable) ? control::kButton : control::kImage;
    }
    if (one_of(s, {L"WebView", L"ScrollView", L"HorizontalScrollView", L"NestedScrollView", L"ViewPager",
                   L"ViewPager2"})) {
        return control::kPane;
    }
    if (s == L"RadioGroup") return control::kGroup;
    if (n.range.present) return n.can_do(kCanSetProgress) ? control::kSlider : control::kProgressBar;
    if (n.has(kCheckable)) return control::kCheckBox;
    if (parent && (parent->control_type == control::kTab)) return control::kTabItem;
    if (n.item.present || (parent && parent->collection.present)) {
        return parent && parent->control_type == control::kDataGrid ? control::kDataItem : control::kListItem;
    }
    if (n.collection.present) return n.collection.rows > 1 && n.collection.columns > 1 ? control::kDataGrid : control::kList;
    if (n.has(kClickable) || n.has(kLongClickable)) return control::kButton;
    if (s == L"TextView" || s == L"AppCompatTextView" || s == L"MaterialTextView") return control::kText;
    if (!n.children.empty()) return control::kGroup;
    if (!blank(n.text) || !blank(n.description)) return control::kText;
    (void)tree;
    return control::kPane;
}

std::wstring join_parts(const std::vector<std::wstring>& parts) {
    std::wstring out;
    for (const std::wstring& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out += L", ";
        out += p;
    }
    return out;
}

}  // namespace

void derive(Tree* tree) {
    tree->index.clear();
    for (size_t i = 0; i < tree->nodes.size(); ++i) tree->index[tree->nodes[i].id] = i;
    // Drop children that are not in the snapshot (a walk cut short) and parents that point outside.
    for (Node& n : tree->nodes) {
        n.children.erase(std::remove_if(n.children.begin(), n.children.end(),
                                        [&](int64_t c) { return !tree->find(c) || c == n.id; }),
                         n.children.end());
    }

    // Parents before children: the windows' roots, depth first, in reading order.
    std::vector<size_t> order;
    order.reserve(tree->nodes.size());
    std::unordered_set<int64_t> seen;
    std::function<void(int64_t, int64_t)> visit = [&](int64_t id, int64_t window) {
        auto it = tree->index.find(id);
        if (it == tree->index.end() || !seen.insert(id).second) return;
        tree->nodes[it->second].window = window;
        order.push_back(it->second);
        const std::vector<int64_t> kids = tree->nodes[it->second].children;
        for (int64_t c : kids) visit(c, window);
    };
    for (const Window& w : tree->windows) visit(w.root, w.root);
    std::vector<bool> absorbed(tree->nodes.size(), false);

    for (size_t i : order) {
        Node& n = tree->nodes[i];
        const Node* parent = n.parent ? tree->find(n.parent) : nullptr;
        const bool root = tree->window_of_root(n.id) != nullptr;
        n.control_type = root ? control::kPane : control_type_of(*tree, n, parent);
        n.patterns = 0;
        n.control_element = n.content_element = true;

        // The name, as TalkBack speaks the node: its description; else its own text, and for a node
        // TalkBack stops on that has no text of its own, the text of the plain views inside it.
        std::wstring label = own_label(*tree, n);
        const bool reads_inside = !root && blank(n.description) && !n.has(kEditable) && !n.has(kScrollable) &&
                                  !n.collection.present &&
                                  (n.has(kClickable) || n.has(kLongClickable) || n.has(kCheckable) ||
                                   n.has(kFocusable) || n.has(kScreenReaderFocusable) || n.has(kHeading));
        if (reads_inside) {
            std::vector<std::wstring> parts{label};
            std::function<void(const Node&)> gather = [&](const Node& at) {
                for (int64_t cid : at.children) {
                    const Node* c = tree->find(cid);
                    if (!c || stops_reading(*c) || !c->has(kVisible)) continue;
                    std::wstring l = own_label(*tree, *c);
                    if (!l.empty()) {
                        parts.push_back(l);
                        absorbed[tree->index[c->id]] = true;
                    }
                    if (blank(c->description)) {
                        if (l.empty()) absorbed[tree->index[c->id]] = true;
                        gather(*c);
                    }
                }
            };
            gather(n);
            label = join_parts(parts);
        }
        if (root) {
            const Window* w = tree->window_of_root(n.id);
            label = !blank(n.pane_title) ? trimmed(n.pane_title) : !blank(w->title) ? trimmed(w->title) : label;
        }
        n.name = label;

        // Help: the tooltip; for an editor named by a label, its hint too.
        std::vector<std::wstring> help{trimmed(n.tooltip)};
        if (n.has(kEditable) && !blank(n.hint) && trimmed(n.hint) != n.name) help.push_back(trimmed(n.hint));
        n.help = join_parts(help);
        // Status: what the app says of the node's state (a checkbox's checked state is the Toggle
        // pattern's, so its state description would say it twice), and an error.
        n.status = join_parts({n.has(kCheckable) ? std::wstring() : trimmed(n.state), trimmed(n.error)});

        const int ct = n.control_type;
        if (n.can_do(kCanClick) && !n.has(kCheckable) && !n.has(kEditable) && ct != control::kRadioButton) {
            n.patterns |= kPatInvoke;
        }
        if (n.has(kCheckable) && ct != control::kRadioButton) n.patterns |= kPatToggle;
        if (n.has(kEditable) || ct == control::kComboBox) n.patterns |= kPatValue;
        if (n.range.present) n.patterns |= kPatRangeValue;
        if (n.can_do(kCanExpand) || n.can_do(kCanCollapse)) n.patterns |= kPatExpandCollapse;
        if (n.has(kScrollable) || n.can_do(kCanScrollForward | kCanScrollBackward | kCanScrollUp | kCanScrollDown |
                                           kCanScrollLeft | kCanScrollRight)) {
            n.patterns |= kPatScroll;
        }
        if (n.can_do(kCanShowOnScreen)) n.patterns |= kPatScrollItem;
        const bool in_selection = parent && parent->collection.present && parent->collection.selection_mode != 0;
        if (ct == control::kRadioButton || ct == control::kTabItem || n.can_do(kCanSelect) ||
            (n.item.present && (n.item.selected || in_selection)) ||
            (n.has(kSelected) && (ct == control::kListItem || ct == control::kDataItem))) {
            n.patterns |= kPatSelectionItem;
        }
        if (n.has(kEditable) || (n.has(kTextSelectable) && !n.text.empty())) n.patterns |= kPatText;
    }
    // Containers of selectable items: the Selection pattern.
    for (Node& n : tree->nodes) {
        bool items = false;
        for (int64_t c : n.children) {
            const Node* k = tree->find(c);
            if (k && (k->patterns & kPatSelectionItem)) items = true;
        }
        if (items || (n.collection.present && n.collection.selection_mode != 0) || n.control_type == control::kTab) {
            n.patterns |= kPatSelection;
        }
    }
    for (size_t i : order) {
        Node& n = tree->nodes[i];
        if (absorbed[i]) {
            // Its words are its row's: out of UIA's control and content views, as TalkBack does
            // not stop on it.
            n.control_element = n.content_element = false;
        } else if (n.name.empty() && n.patterns == 0 && !n.has(kFocusable) && !n.has(kHeading) &&
                   !tree->window_of_root(n.id) &&
                   (n.control_type == control::kPane || n.control_type == control::kGroup)) {
            // A layout with nothing to say: its children stand in its place.
            n.control_element = n.content_element = false;
        } else if (n.control_type == control::kImage && n.name.empty()) {
            n.content_element = false;  // a decorative picture, which TalkBack skips
        }
    }
}

// ---- Events -------------------------------------------------------------------------------------

namespace {

int toggle_state(const Node& n) { return n.has(kChecked) ? 1 : 0; }

int expand_state(const Node& n) {
    const bool e = n.can_do(kCanExpand), c = n.can_do(kCanCollapse);
    if (e && c) return 2;  // partially expanded
    if (c) return 1;       // expanded
    if (e) return 0;       // collapsed
    return 3;              // leaf
}

std::wstring value_of(const Node& n) { return n.has(kShowingHint) ? std::wstring() : n.text; }

std::wstring spoken_text_of_window(const Tree& t, int64_t root) {
    std::vector<std::wstring> parts;
    std::function<void(int64_t)> walk = [&](int64_t id) {
        const Node* n = t.find(id);
        if (!n) return;
        std::wstring l = own_label(t, *n);
        if (!l.empty()) parts.push_back(l);
        if (blank(n->description)) {
            for (int64_t c : n->children) walk(c);
        }
    };
    walk(root);
    return join_parts(parts);
}

}  // namespace

std::vector<Event> diff(const Tree* before, const Tree& after) {
    std::vector<Event> out;
    if (!before) return out;
    std::vector<Event> structure, props, selection, spoken;

    // Windows came or went: the root's children changed.
    std::vector<int64_t> wb, wa;
    for (const Window& w : before->windows) wb.push_back(w.root);
    for (const Window& w : after.windows) wa.push_back(w.root);
    if (wb != wa) structure.push_back({EventKind::kStructure, 0});
    for (const Window& w : after.windows) {
        if (w.type == kWindowTypeToast && !before->window_of_root(w.root)) {
            std::wstring said = spoken_text_of_window(after, w.root);
            if (!said.empty()) spoken.push_back({EventKind::kNotification, 0, said, false});
        }
    }

    for (const Node& n : after.nodes) {
        const Node* o = before->find(n.id);
        if (!o) {
            // New: a live region that appears is spoken; so is a pane that appears with a title.
            if (n.live && !n.name.empty()) spoken.push_back({EventKind::kLiveRegion, n.id, n.name, n.live == 2});
            if (!blank(n.pane_title) && !after.window_of_root(n.id)) {
                spoken.push_back({EventKind::kNotification, n.id, trimmed(n.pane_title), false});
            }
            continue;
        }
        if (o->children != n.children) structure.push_back({EventKind::kStructure, n.id});
        if (o->name != n.name) {
            props.push_back({EventKind::kName, n.id, n.name});
            if (n.live && !n.name.empty()) spoken.push_back({EventKind::kLiveRegion, n.id, n.name, n.live == 2});
        }
        if ((n.patterns & kPatValue) && value_of(*o) != value_of(n)) {
            props.push_back({EventKind::kValue, n.id, value_of(n)});
            if (n.patterns & kPatText) selection.push_back({EventKind::kTextChanged, n.id});
        }
        if ((n.patterns & kPatToggle) && toggle_state(*o) != toggle_state(n)) props.push_back({EventKind::kToggle, n.id});
        if ((n.patterns & kPatExpandCollapse) && expand_state(*o) != expand_state(n)) {
            props.push_back({EventKind::kExpandCollapse, n.id});
        }
        if (n.range.present && (!o->range.present || o->range.now != n.range.now)) {
            props.push_back({EventKind::kRangeValue, n.id});
        }
        if (o->has(kEnabled) != n.has(kEnabled)) props.push_back({EventKind::kEnabled, n.id});
        const bool was_selected = o->has(kSelected) || o->item.selected;
        const bool is_selected = n.has(kSelected) || n.item.selected;
        if ((n.patterns & kPatSelectionItem) && is_selected && !was_selected) {
            selection.push_back({EventKind::kSelected, n.id});
        }
        if ((n.patterns & kPatText) && (o->sel_start != n.sel_start || o->sel_end != n.sel_end) &&
            (n.has(kFocused) || n.id == after.focused)) {
            selection.push_back({EventKind::kTextSelection, n.id});
        }
    }
    for (const Node& o : before->nodes) {
        if (!after.find(o.id)) out.push_back({EventKind::kGone, o.id});
    }
    // Many lists changed at once (a new screen): one change of the whole tree says it.
    if (structure.size() > 16) structure.assign(1, Event{EventKind::kStructure, 0});
    out.insert(out.end(), structure.begin(), structure.end());
    out.insert(out.end(), props.begin(), props.end());
    out.insert(out.end(), selection.begin(), selection.end());
    if (after.focused != before->focused && after.find(after.focused)) {
        out.push_back({EventKind::kFocus, after.focused});
    }
    out.insert(out.end(), spoken.begin(), spoken.end());
    return out;
}

// ---- Text units ---------------------------------------------------------------------------------

bool is_combining(wchar_t c) {
    const unsigned u = static_cast<unsigned>(c);
    return (u >= 0x0300 && u <= 0x036F) ||                                      // combining diacritics
           (u >= 0x0483 && u <= 0x0489) ||                                      // Cyrillic
           (u >= 0x0591 && u <= 0x05BD) || u == 0x05BF || u == 0x05C1 ||        // Hebrew cantillation, points
           u == 0x05C2 || u == 0x05C4 || u == 0x05C5 || u == 0x05C7 ||
           (u >= 0x0610 && u <= 0x061A) || (u >= 0x064B && u <= 0x065F) ||      // Arabic
           u == 0x0670 || (u >= 0x06D6 && u <= 0x06DC) || (u >= 0x06DF && u <= 0x06E4) ||
           u == 0x06E7 || u == 0x06E8 || (u >= 0x06EA && u <= 0x06ED) ||
           (u >= 0x0900 && u <= 0x0903) || (u >= 0x093A && u <= 0x094F) ||      // Devanagari signs
           (u >= 0x1AB0 && u <= 0x1AFF) || (u >= 0x1DC0 && u <= 0x1DFF) ||
           u == 0x200C || u == 0x200D ||                                        // ZWNJ, ZWJ
           (u >= 0x20D0 && u <= 0x20FF) || (u >= 0xFE00 && u <= 0xFE0F) ||      // symbols' marks, VS
           (u >= 0xFE20 && u <= 0xFE2F) || (u >= 0xDC00 && u <= 0xDFFF);        // a pair's second half
}

namespace {

// A letter or digit for the fallback word breaks (the app's own come from Java's BreakIterator).
// Outside ASCII, anything but spaces and punctuation: glibc's "C" locale classifies only ASCII.
bool word_char(wchar_t c) {
    const unsigned u = static_cast<unsigned>(c);
    if (u < 0x80) return (u >= L'0' && u <= L'9') || (u >= L'a' && u <= L'z') || (u >= L'A' && u <= L'Z') || u == L'_' || u == L'\'';
    if (is_space(c)) return false;
    return !((u >= 0xA1 && u <= 0xBF) || u == 0xD7 || u == 0xF7 || u == 0x05BE || u == 0x05C0 || u == 0x05C3 ||
             u == 0x05C6 || (u >= 0x2010 && u <= 0x206F) || (u >= 0x3000 && u <= 0x303F) || (u >= 0xFF01 && u <= 0xFF0F));
}

}  // namespace

TextModel::TextModel(const Node& node)
    : text_(node.has(kShowingHint) ? std::wstring() : node.text), bounds_(node.bounds), rtl_(node.has(kRtl)) {
    const int len = length();
    // Lines: the Layout's when it matches this text, else paragraphs.
    bool layout = !node.line_starts.empty() && node.line_starts.front() == 0 && !node.has(kShowingHint) &&
                  node.line_starts.back() <= len && std::is_sorted(node.line_starts.begin(), node.line_starts.end());
    if (layout) {
        lines_ = node.line_starts;
        if (node.line_boxes.size() == node.line_starts.size()) line_boxes_ = node.line_boxes;
        if (static_cast<int>(node.char_x.size()) == len + 1) char_x_ = node.char_x;
    } else {
        lines_.push_back(0);
        for (int i = 0; i < len; ++i) {
            if (text_[i] == L'\n' || text_[i] == 0x2029) lines_.push_back(i + 1);
        }
    }
    // Words: the app's breaks, where a run of spaces belongs to the word before it.
    std::vector<int> breaks;
    bool app = !node.word_breaks.empty() && !node.has(kShowingHint);
    if (app) {
        for (int b : node.word_breaks) {
            if (b >= 0 && b <= len) breaks.push_back(b);
        }
        std::sort(breaks.begin(), breaks.end());
    } else {
        for (int i = 1; i < len; ++i) {
            if (word_char(text_[i]) != word_char(text_[i - 1]) || (!word_char(text_[i]) && !is_space(text_[i])))
                breaks.push_back(i);
        }
    }
    words_.push_back(0);
    for (int b : breaks) {
        if (b <= 0 || b >= len) continue;
        if (is_space(text_[b])) continue;  // spaces (a line break too) join the word before
        if (b < len && is_combining(text_[b])) continue;
        if (b == words_.back()) continue;
        words_.push_back(b);
    }
    // A line start is always a word start.
    for (int l : lines_) {
        if (l > 0 && l < len) words_.push_back(l);
    }
    std::sort(words_.begin(), words_.end());
    words_.erase(std::unique(words_.begin(), words_.end()), words_.end());
    if (words_.back() != len) words_.push_back(len);
}

bool TextModel::is_boundary(Unit unit, int pos) const {
    const int len = length();
    if (pos <= 0 || pos >= len) return true;
    switch (unit) {
        case Unit::kCharacter: return !is_combining(text_[pos]);
        case Unit::kWord: return std::binary_search(words_.begin(), words_.end(), pos);
        case Unit::kLine: return std::binary_search(lines_.begin(), lines_.end(), pos);
        case Unit::kParagraph: return text_[pos - 1] == L'\n' || text_[pos - 1] == 0x2029;
        default: return false;  // Format, Page, Document: the whole text is one
    }
}

int TextModel::next(Unit unit, int pos) const {
    const int len = length();
    if (pos >= len) return len;
    if (unit == Unit::kWord) return *std::upper_bound(words_.begin(), words_.end(), pos);
    if (unit == Unit::kLine) {
        auto it = std::upper_bound(lines_.begin(), lines_.end(), pos);
        return it == lines_.end() ? len : *it;
    }
    for (int p = std::max(pos + 1, 0); p < len; ++p) {
        if (is_boundary(unit, p)) return p;
    }
    return len;
}

int TextModel::prev(Unit unit, int pos) const {
    if (pos <= 0) return 0;
    const int len = length();
    if (pos > len) pos = len;
    if (unit == Unit::kWord) return *(std::lower_bound(words_.begin(), words_.end(), pos) - 1);
    if (unit == Unit::kLine) return *(std::lower_bound(lines_.begin(), lines_.end(), pos) - 1);
    for (int p = pos - 1; p > 0; --p) {
        if (is_boundary(unit, p)) return p;
    }
    return 0;
}

void TextModel::expand(Unit unit, int* start, int* end) const {
    const int len = length();
    int s = std::clamp(*start, 0, len);
    if (!is_boundary(unit, s)) s = prev(unit, s);
    // An empty range at the end of the text takes the last unit (the last line, the last word), so
    // that "read the current line" there reads it.
    if (s == len && len > 0 && unit != Unit::kDocument && unit != Unit::kPage && unit != Unit::kFormat) {
        if (unit != Unit::kLine || lines_.back() != len) s = prev(unit, s);
    }
    if (unit == Unit::kDocument || unit == Unit::kPage || unit == Unit::kFormat) s = 0;
    *start = s;
    *end = s >= len ? len : next(unit, s);
}

int TextModel::move(Unit unit, int count, int* start, int* end) const {
    if (count == 0) return 0;
    const int len = length();
    const bool degenerate = *start == *end;
    int pos = std::clamp(*start, 0, len);
    if (!degenerate) {
        int s = pos, e = *end;
        expand(unit, &s, &e);
        pos = s;
    } else if (!is_boundary(unit, pos)) {
        // Inside a unit: the step to its edge is the first unit moved.
        const int dir = count > 0 ? 1 : -1;
        pos = dir > 0 ? next(unit, pos) : prev(unit, pos);
        const int more = count - dir ? move(unit, count - dir, &pos, &pos) : 0;
        *start = *end = pos;
        return dir + more;
    }
    int moved = 0;
    while (count > 0 && moved < count) {
        if (pos >= len) break;
        int n = next(unit, pos);
        if (!degenerate && n >= len) break;  // no unit starts there for the range to cover
        pos = n;
        ++moved;
    }
    while (count < 0 && moved > count) {
        if (pos <= 0) break;
        pos = prev(unit, pos);
        --moved;
    }
    *start = pos;
    *end = degenerate ? pos : next(unit, pos);
    return moved;
}

int TextModel::move_endpoint(Unit unit, int count, int* pos) const {
    const int len = length();
    int p = std::clamp(*pos, 0, len);
    int moved = 0;
    while (count > 0 && moved < count && p < len) {
        p = next(unit, p);
        ++moved;
    }
    while (count < 0 && moved > count && p > 0) {
        p = prev(unit, p);
        --moved;
    }
    *pos = p;
    return moved;
}

int TextModel::line_of(int pos) const {
    auto it = std::upper_bound(lines_.begin(), lines_.end(), pos);
    return static_cast<int>(it - lines_.begin()) - 1;
}

std::vector<Box> TextModel::boxes(int start, int end) const {
    std::vector<Box> out;
    const int len = length();
    start = std::clamp(start, 0, len);
    end = std::clamp(end, start, len);
    if (line_boxes_.empty()) {
        if (!bounds_.empty()) out.push_back(bounds_);
        return out;
    }
    const int first = std::max(0, line_of(start));
    const int last = std::max(first, line_of(start == end ? end : end - 1));
    for (int l = first; l <= last && l < static_cast<int>(line_boxes_.size()); ++l) {
        Box b = line_boxes_[l];
        const int ls = lines_[l];
        const int le = l + 1 < static_cast<int>(lines_.size()) ? lines_[l + 1] : len;
        if (!char_x_.empty()) {
            const int a = std::max(start, ls), z = std::min(end, le);
            // A line's end offset is the next line's start, whose x is over there: the line's edge.
            const bool at_end = z == le && l + 1 < static_cast<int>(lines_.size());
            int x1 = char_x_[a], x2 = at_end ? (rtl_ ? b.l : b.r) : char_x_[z];
            if (start == end) x2 = x1 + 1;  // the caret
            if (x2 < x1) std::swap(x1, x2);
            b.l = x1;
            b.r = std::max(x2, x1 + 1);
        }
        out.push_back(b);
    }
    return out;
}

int TextModel::offset_at(int x, int y) const {
    const int len = length();
    if (line_boxes_.empty()) return 0;
    int line = 0;
    for (int l = 0; l < static_cast<int>(line_boxes_.size()); ++l) {
        if (y >= line_boxes_[l].t) line = l;
    }
    const int ls = lines_[line];
    const int le = line + 1 < static_cast<int>(lines_.size()) ? lines_[line + 1] : len;
    if (char_x_.empty()) return (x >= (line_boxes_[line].l + line_boxes_[line].r) / 2) != rtl_ ? le : ls;
    int best = ls, dist = INT32_MAX;
    for (int p = ls; p <= le; ++p) {
        const int d = std::abs(char_x_[p] - x);
        if (d < dist) {
            dist = d;
            best = p;
        }
    }
    return best;
}

int TextModel::find(const std::wstring& needle, int start, int end, bool backward, bool ignore_case) const {
    const int len = length();
    start = std::clamp(start, 0, len);
    end = std::clamp(end, start, len);
    const int n = static_cast<int>(needle.size());
    if (n == 0 || n > end - start) return -1;
    auto eq = [&](wchar_t a, wchar_t b) { return ignore_case ? std::towlower(a) == std::towlower(b) : a == b; };
    auto at = [&](int p) {
        for (int i = 0; i < n; ++i) {
            if (!eq(text_[p + i], needle[i])) return false;
        }
        return true;
    };
    if (backward) {
        for (int p = end - n; p >= start; --p) {
            if (at(p)) return p;
        }
    } else {
        for (int p = start; p + n <= end; ++p) {
            if (at(p)) return p;
        }
    }
    return -1;
}

}  // namespace winapk::host::uia
