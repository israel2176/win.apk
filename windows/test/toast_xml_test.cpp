// toast_xml_test.cpp -- winapk-toast-xml-test: the toast an Android notification becomes, and the
// click arguments that come back (host/toast_xml.h). No Windows call: it builds and runs anywhere.
//
// Checks: a plain notification's toast has no actions; a notification's actions become buttons,
// at most three, a reply action with a text field (listed before the buttons, named by the
// button's hint-inputId, its placeholder the RemoteInput's label); text is escaped; the toast's
// own arguments and each button's parse back to the key and the action; foreign or broken
// arguments are refused.

#include <cstdio>
#include <string>
#include <vector>

#include "../host/toast_xml.h"

using winapk::host::parse_toast_args;
using winapk::host::toast_xml;
using winapk::host::ToastAction;

namespace {

int g_failures = 0;
void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}
bool has(const std::wstring& s, const std::wstring& part) { return s.find(part) != std::wstring::npos; }
size_t count(const std::wstring& s, const std::wstring& part) {
    size_t n = 0;
    for (size_t at = s.find(part); at != std::wstring::npos; at = s.find(part, at + 1)) ++n;
    return n;
}

}  // namespace

int main() {
    std::printf("toast XML\n");
    const std::wstring plain = toast_xml(L"|1", L"Title", L"Text", L"", L"", 0, 0, false, true);
    check("a plain notification: no actions", !has(plain, L"<actions>"));
    check("its launch arguments name the key", has(plain, L"launch=\"n:|1\""));

    std::vector<ToastAction> actions = {
        {L"Reply", true, L"Message"}, {L"Mark as read", false, L""}, {L"Archive", false, L""}, {L"Fourth", false, L""}};
    const std::wstring x = toast_xml(L"chat|7", L"Dana", L"See you at <8> & \"later\"", L"", L"", 0, 0, false, true,
                                     actions);
    check("three buttons at most, as in the shade", count(x, L"<action ") == 3 && !has(x, L"Fourth"));
    check("the reply action's field, its placeholder the label",
          has(x, L"<input id=\"reply0\" type=\"text\" placeHolderContent=\"Message\"/>"));
    check("only the reply action has a field", count(x, L"<input ") == 1);
    check("the field before the buttons", x.find(L"<input ") < x.find(L"<action "));
    check("the reply button names its field",
          has(x, L"content=\"Reply\" arguments=\"a:0:chat|7\" activationType=\"foreground\" hint-inputId=\"reply0\""));
    check("another button names its action", has(x, L"content=\"Mark as read\" arguments=\"a:1:chat|7\""));
    check("text escaped", has(x, L"See you at &lt;8&gt; &amp; &quot;later&quot;"));
    check("the actions inside the toast, after the visual",
          x.find(L"</visual>") < x.find(L"<actions>") && x.find(L"</actions>") < x.find(L"</toast>"));
    const std::wstring quiet = toast_xml(L"k", L"T", L"", L"", L"", 0, 0, false, false, actions);
    check("silent toasts keep their actions", has(quiet, L"<actions>") && has(quiet, L"<audio silent=\"true\"/>"));

    std::printf("arguments\n");
    std::wstring key;
    int action = 0;
    check("the toast's own: the key, no action",
          parse_toast_args(L"n:chat|7", &key, &action) && key == L"chat|7" && action == -1);
    check("an action's: the key and its index",
          parse_toast_args(L"a:2:chat|7", &key, &action) && key == L"chat|7" && action == 2);
    check("a key with colons in it", parse_toast_args(L"a:1:a:b|3", &key, &action) && key == L"a:b|3" && action == 1);
    check("an empty key (tag-less id 0 is \"|0\", but the parser takes any)",
          parse_toast_args(L"n:", &key, &action) && key.empty());
    check("an index past three refused", !parse_toast_args(L"a:3:k", &key, &action));
    check("no index refused", !parse_toast_args(L"a::k", &key, &action));
    check("a non-number refused", !parse_toast_args(L"a:x:k", &key, &action));
    check("foreign arguments refused", !parse_toast_args(L"launch=1", &key, &action));

    std::printf(g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
