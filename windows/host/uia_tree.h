// uia_tree.h -- what a screen reader is told of an Android app's views, with no Windows call.
//
// The app's side (runtime/java/com/macapk/loader/HostAccessibility.java, with the tree walk in
// windows/java/aosp_views/com/winapk/aospviews/AccessibilityTree.java) reads every window's
// AccessibilityNodeInfo tree on the app's main thread, as TalkBack would through Android's
// accessibility interaction connection: AOSP's own views, an app's own views, and the virtual
// nodes behind an AccessibilityNodeProvider (Compose's semantics, a custom view's). It sends the
// whole tree as one snapshot. This header is everything the host decides from a snapshot alone:
//
//   - the snapshot's text and its parser (parse) and writer (encode, for tests);
//   - what each node is for UI Automation (derive): its control type, its name as TalkBack would
//     speak it (a clickable row without its own label takes the text of the plain views inside
//     it, which then leave UIA's control and content views so that they are not read twice), its
//     help text, its status, the control patterns it supports;
//   - which UIA events the change from one snapshot to the next makes (diff);
//   - text units for the Text pattern (TextModel): characters that keep a combining mark (a
//     Hebrew vowel point) with its letter, words from the app's own word breaks (Java's
//     BreakIterator, which knows Hebrew, Thai or Japanese), lines as the TextView's Layout broke
//     them, paragraphs, and the rectangles of a range.
//
// uia_provider.cpp turns this into COM providers; tests (test/uia_tree_test.cpp) drive it as is.
//
// The snapshot's text: records separated by U+001E, fields by U+001F. Strings never hold either
// (the writer turns them into spaces); lists inside a field are comma-separated numbers.
//   S <version 1> <input-focused node id> <locale, BCP 47>
//   W <root node id> <WindowManager.LayoutParams.type> <title> <1: has input focus>
//   N <id> <parent id, 0 for a window's root> <flags> <bounds l,t,r,b> <class name>
//     <role description> <content description> <text> <hint> <tooltip> <state description>
//     <pane title> <error> <view id resource name> <live region 0/1/2>
//     <range: type,min,max,now or empty> <collection: rows,columns,hierarchical,selection mode>
//     <collection item: row,row span,column,column span,heading,selected> <selection start,end>
//     <line starts> <line boxes l,t,r,b,...> <x of each offset 0..length> <word breaks>
//     <children ids> <action ids (AccessibilityNodeInfo.AccessibilityAction.getId)> <labeled-by id>
// Records come windows first (bottom to top), then nodes; ids are AccessibilityNodeInfo source
// node ids (the view's accessibility id and the virtual descendant's), unique in the process.
// Coordinates are pixels of the app's frame, its surface, origin at the top left.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace winapk::host::uia {

struct Box {
    int32_t l = 0, t = 0, r = 0, b = 0;
    bool empty() const { return r <= l || b <= t; }
    bool contains(int32_t x, int32_t y) const { return x >= l && x < r && y >= t && y < b; }
    bool operator==(const Box& o) const { return l == o.l && t == o.t && r == o.r && b == o.b; }
    bool operator!=(const Box& o) const { return !(*this == o); }
};

// A node's state, AccessibilityNodeInfo's booleans.
enum Flag : uint32_t {
    kEnabled = 1u << 0,
    kFocusable = 1u << 1,
    kFocused = 1u << 2,  // input focus
    kClickable = 1u << 3,
    kLongClickable = 1u << 4,
    kCheckable = 1u << 5,
    kChecked = 1u << 6,
    kEditable = 1u << 7,
    kPassword = 1u << 8,
    kVisible = 1u << 9,  // isVisibleToUser
    kScrollable = 1u << 10,
    kHeading = 1u << 11,
    kSelected = 1u << 12,
    kMultiLine = 1u << 13,
    kContentInvalid = 1u << 14,
    kShowingHint = 1u << 15,
    kRtl = 1u << 16,  // the node's text (or its layout) runs right to left
    kScreenReaderFocusable = 1u << 17,
    kTextSelectable = 1u << 18,
};

// What a node's actions let UIA do (from its action ids).
enum Can : uint32_t {
    kCanClick = 1u << 0,
    kCanLongClick = 1u << 1,
    kCanFocus = 1u << 2,
    kCanSetText = 1u << 3,
    kCanSetSelection = 1u << 4,
    kCanScrollForward = 1u << 5,
    kCanScrollBackward = 1u << 6,
    kCanScrollUp = 1u << 7,
    kCanScrollDown = 1u << 8,
    kCanScrollLeft = 1u << 9,
    kCanScrollRight = 1u << 10,
    kCanShowOnScreen = 1u << 11,
    kCanSetProgress = 1u << 12,
    kCanExpand = 1u << 13,
    kCanCollapse = 1u << 14,
    kCanSelect = 1u << 15,
    kCanClearSelection = 1u << 16,
    kCanDismiss = 1u << 17,
    kCanPageUp = 1u << 18,
    kCanPageDown = 1u << 19,
};

// AccessibilityNodeInfo's action ids (AOSP 14): what the host asks the app to perform.
namespace action {
constexpr int kFocus = 0x1, kSelect = 0x4, kClearSelection = 0x8, kClick = 0x10, kLongClick = 0x20;
constexpr int kScrollForward = 0x1000, kScrollBackward = 0x2000, kSetSelection = 0x20000, kExpand = 0x40000;
constexpr int kCollapse = 0x80000, kDismiss = 0x100000, kSetText = 0x200000;
// android.R.id.accessibilityAction*
constexpr int kShowOnScreen = 0x01020036, kScrollUp = 0x01020038, kScrollLeft = 0x01020039;
constexpr int kScrollDown = 0x0102003a, kScrollRight = 0x0102003b, kSetProgress = 0x0102003d;
constexpr int kPageUp = 0x01020046, kPageDown = 0x01020047;
}  // namespace action

// The Can bit of an action id (0 for one the host does not use).
uint32_t can_of_action(int id);

// UI Automation's control type ids (UIAutomationClient.h), so that this header needs no Windows
// header; uia_provider.cpp checks them against the SDK's.
namespace control {
constexpr int kButton = 50000, kCheckBox = 50002, kComboBox = 50003, kEdit = 50004, kHyperlink = 50005;
constexpr int kImage = 50006, kListItem = 50007, kList = 50008, kProgressBar = 50012, kRadioButton = 50013;
constexpr int kSlider = 50015, kTab = 50018, kTabItem = 50019, kText = 50020, kToolBar = 50021, kGroup = 50026;
constexpr int kDataGrid = 50028, kDataItem = 50029, kDocument = 50030, kWindow = 50032, kPane = 50033;
}  // namespace control

// The control patterns a node supports.
enum Pattern : uint32_t {
    kPatInvoke = 1u << 0,
    kPatToggle = 1u << 1,
    kPatValue = 1u << 2,
    kPatRangeValue = 1u << 3,
    kPatExpandCollapse = 1u << 4,
    kPatScroll = 1u << 5,
    kPatScrollItem = 1u << 6,
    kPatSelectionItem = 1u << 7,
    kPatSelection = 1u << 8,
    kPatText = 1u << 9,
};

struct RangeInfo {
    bool present = false;
    int type = 0;  // RANGE_TYPE_INT, _FLOAT, _PERCENT
    double min = 0, max = 0, now = 0;
};

struct CollectionInfo {
    bool present = false;
    int rows = -1, columns = -1, selection_mode = 0;  // SELECTION_MODE_NONE, _SINGLE, _MULTIPLE
    bool hierarchical = false;
};

struct ItemInfo {
    bool present = false;
    int row = 0, row_span = 1, column = 0, column_span = 1;
    bool heading = false, selected = false;
};

struct Node {
    int64_t id = 0, parent = 0;
    uint32_t flags = 0, can = 0;
    Box bounds;
    std::wstring class_name, role, description, text, hint, tooltip, state, pane_title, error, resource_id;
    int live = 0;  // View.ACCESSIBILITY_LIVE_REGION_*: 0 none, 1 polite, 2 assertive
    RangeInfo range;
    CollectionInfo collection;
    ItemInfo item;
    int sel_start = -1, sel_end = -1;
    std::vector<int> line_starts;  // the Layout's lines (empty: none known)
    std::vector<Box> line_boxes;   // one per line start
    std::vector<int> char_x;       // x of each offset 0..text.size(), when known
    std::vector<int> word_breaks;  // the app's word boundaries, ascending
    std::vector<int64_t> children;
    std::vector<int> action_ids;
    int64_t labeled_by = 0;

    // derive(): what UIA is told.
    int control_type = control::kPane;
    std::wstring name, help, status;
    bool control_element = true, content_element = true;
    uint32_t patterns = 0;
    int64_t window = 0;  // the root of the window it is in

    bool has(uint32_t f) const { return (flags & f) != 0; }
    bool can_do(uint32_t c) const { return (can & c) != 0; }
};

struct Window {
    int64_t root = 0;
    int type = 0;
    std::wstring title;
    bool focused = false;
};

constexpr int kWindowTypeToast = 2005;  // WindowManager.LayoutParams.TYPE_TOAST

struct Tree {
    int64_t focused = 0;
    std::wstring locale;
    std::vector<Window> windows;  // bottom to top
    std::vector<Node> nodes;
    std::unordered_map<int64_t, size_t> index;

    const Node* find(int64_t id) const {
        auto it = index.find(id);
        return it == index.end() ? nullptr : &nodes[it->second];
    }
    const Window* window_of_root(int64_t root) const {
        for (const Window& w : windows) {
            if (w.root == root) return &w;
        }
        return nullptr;
    }
};

// Parses a snapshot (and derives it); false when the text is not one.
bool parse(const std::wstring& text, Tree* out);
// The snapshot's text of a tree (its raw fields; derived ones are not written).
std::wstring encode(const Tree& tree);
// Fills each node's derived fields and the index. parse calls it; a test building a tree by hand
// calls it too.
void derive(Tree* tree);

// The label a node speaks by itself (no descendants'): its content description, else its text
// (an editor's: the field labelling it, else its hint).
std::wstring own_label(const Tree& tree, const Node& node);

// ---- Events: what changed from one snapshot to the next --------------------------------------
enum class EventKind {
    kFocus,              // UIA_AutomationFocusChangedEventId on id
    kStructure,          // UIA_StructureChangedEventId, ChildrenInvalidated, on id (0: the root)
    kName,               // UIA_NamePropertyId changed
    kValue,              // UIA_ValueValuePropertyId
    kToggle,             // UIA_ToggleToggleStatePropertyId
    kExpandCollapse,     // UIA_ExpandCollapseExpandCollapseStatePropertyId
    kRangeValue,         // UIA_RangeValueValuePropertyId
    kEnabled,            // UIA_IsEnabledPropertyId
    kSelected,           // UIA_SelectionItem_ElementSelectedEventId
    kLiveRegion,         // UIA_LiveRegionChangedEventId
    kTextChanged,        // UIA_Text_TextChangedEventId
    kTextSelection,      // UIA_Text_TextSelectionChangedEventId
    kNotification,       // UiaRaiseNotificationEvent on id (0: the root) with text
    kGone,               // a node left: its provider is disconnected
};

struct Event {
    EventKind kind;
    int64_t id = 0;
    std::wstring text;
    bool assertive = false;

    Event(EventKind k, int64_t i = 0, std::wstring t = std::wstring(), bool a = false)
        : kind(k), id(i), text(std::move(t)), assertive(a) {}
};

// The events from `before` (null: the first snapshot, which raises none) to `after`, in the order
// a client should hear them: structure, then properties, then selection and text, then focus and
// what is spoken.
std::vector<Event> diff(const Tree* before, const Tree& after);

// ---- Text units -------------------------------------------------------------------------------
// UIA's TextUnit values.
enum class Unit { kCharacter = 0, kFormat = 1, kWord = 2, kLine = 3, kParagraph = 4, kPage = 5, kDocument = 6 };

// The text of an editor (or of selectable text) as the Text pattern reads it.
class TextModel {
public:
    explicit TextModel(const Node& node);
    int length() const { return static_cast<int>(text_.size()); }
    const std::wstring& text() const { return text_; }
    // Whether pos starts a unit (0 and length always do).
    bool is_boundary(Unit unit, int pos) const;
    // The next boundary after pos (length when none before it), the one before pos (0).
    int next(Unit unit, int pos) const;
    int prev(Unit unit, int pos) const;
    // ITextRangeProvider::ExpandToEnclosingUnit.
    void expand(Unit unit, int* start, int* end) const;
    // ITextRangeProvider::Move: the units moved (negative backward).
    int move(Unit unit, int count, int* start, int* end) const;
    // ITextRangeProvider::MoveEndpointByUnit for one endpoint: the units moved.
    int move_endpoint(Unit unit, int count, int* pos) const;
    // The line of an offset (0 when no lines are known).
    int line_of(int pos) const;
    // The rectangles (frame pixels) a range covers: a part of each line it touches, a caret for an
    // empty range; the node's bounds when no layout is known.
    std::vector<Box> boxes(int start, int end) const;
    // The offset nearest a frame point.
    int offset_at(int x, int y) const;
    // FindText: the match's start, or -1.
    int find(const std::wstring& needle, int start, int end, bool backward, bool ignore_case) const;

private:
    std::wstring text_;
    std::vector<int> lines_;  // line starts, at least {0}
    std::vector<Box> line_boxes_;
    std::vector<int> char_x_;
    std::vector<int> words_;  // word starts, ascending, 0 and length included
    Box bounds_;
    bool rtl_ = false;
};

// Whether a UTF-16 unit is a combining mark kept with the character before it (Hebrew points
// and cantillation, Arabic harakat, the general combining blocks, variation selectors, ZWJ/ZWNJ).
bool is_combining(wchar_t c);

}  // namespace winapk::host::uia
