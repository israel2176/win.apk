// toast_xml.h -- what a Windows notification (a toast) says for an Android notification, and what a
// click on it names: the XML native_notifications.cpp hands Windows, and the arguments Windows hands
// back. No Windows call, so test/toast_xml_test.cpp checks both anywhere.
//
// An Android notification's actions are buttons under it, at most three in the shade
// (NotificationTemplateViewWrapper's MAX_ACTIONS); an action with a free-form RemoteInput opens a text
// field when tapped, and its button sends what was typed. A toast has the same: up to five
// <action>s, each of which may name an <input> of type text drawn above the buttons, whose text
// comes back with the click. So each Android action is a toast action, a reply action with its own
// input whose placeholder is the RemoteInput's label.
//
// A click comes back as the arguments of what was clicked: "n:<key>" for the toast itself (the
// launch attribute), "a:<index>:<key>" for an action, its index among those shown.

#pragma once

#include <cwchar>
#include <string>
#include <vector>

namespace winapk::host {

struct ToastAction {
    std::wstring title;
    bool reply = false;        // a free-form RemoteInput: a text field, its text sent with the click
    std::wstring reply_label;  // the field's placeholder (RemoteInput.getLabel)
};

constexpr size_t kToastMaxActions = 3;  // Android's shade shows three

// Text into the XML: the five entities, and without the control characters XML 1.0 has no place
// for (the parser would refuse the document).
inline std::wstring xml_escape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            default:
                if (c >= 0x20 || c == L'\n' || c == L'\t') out += c;
                else if (c == L'\r') out += L'\n';
        }
    }
    return out;
}

inline std::wstring toast_reply_id(size_t action) { return L"reply" + std::to_wstring(action); }
inline std::wstring toast_launch_args(const std::wstring& key) { return L"n:" + key; }
inline std::wstring toast_action_args(const std::wstring& key, size_t action) {
    return L"a:" + std::to_wstring(action) + L":" + key;
}

// What a click's arguments name: the notification's key, and the action (-1: the toast itself).
// False for arguments of no toast of ours.
inline bool parse_toast_args(const std::wstring& args, std::wstring* key, int* action) {
    if (args.rfind(L"n:", 0) == 0) {
        *key = args.substr(2);
        *action = -1;
        return true;
    }
    if (args.rfind(L"a:", 0) != 0) return false;
    const size_t colon = args.find(L':', 2);
    if (colon == std::wstring::npos || colon == 2 || colon - 2 > 2) return false;
    int index = 0;
    for (size_t i = 2; i < colon; ++i) {
        if (args[i] < L'0' || args[i] > L'9') return false;
        index = index * 10 + (args[i] - L'0');
    }
    if (static_cast<size_t>(index) >= kToastMaxActions) return false;
    *key = args.substr(colon + 1);
    *action = index;
    return true;
}

// The toast (the "ToastGeneric" template): picture, title, text, attribution, progress, then the
// actions. A local picture is given as a file URL. Without `popup` the sound is off as well as the
// banner.
inline std::wstring toast_xml(const std::wstring& key, const std::wstring& title, const std::wstring& text,
                              const std::wstring& sub, const std::wstring& icon, int max, int progress,
                              bool indeterminate, bool popup, const std::vector<ToastAction>& actions = {}) {
    std::wstring x = L"<toast launch=\"" + xml_escape(toast_launch_args(key)) + L"\" activationType=\"foreground\">";
    x += L"<visual><binding template=\"ToastGeneric\">";
    if (!icon.empty()) {
        std::wstring url = L"file:///" + icon;
        for (wchar_t& c : url) {
            if (c == L'\\') c = L'/';
        }
        x += L"<image placement=\"appLogoOverride\" src=\"" + xml_escape(url) + L"\"/>";
    }
    if (!title.empty()) x += L"<text>" + xml_escape(title) + L"</text>";
    if (!text.empty()) x += L"<text>" + xml_escape(text) + L"</text>";
    if (!sub.empty()) x += L"<text placement=\"attribution\">" + xml_escape(sub) + L"</text>";
    if (indeterminate || max > 0) {
        std::wstring value = L"indeterminate", status;
        if (!indeterminate) {
            const int permille = max > 0 ? static_cast<int>(1000LL * (progress < 0 ? 0 : progress) / max) : 0;
            const int p = permille > 1000 ? 1000 : permille;
            value = p == 1000 ? L"1" : L"0." + std::wstring{static_cast<wchar_t>(L'0' + p / 100),
                                                             static_cast<wchar_t>(L'0' + p / 10 % 10),
                                                             static_cast<wchar_t>(L'0' + p % 10)};
            status = std::to_wstring(p / 10) + L"%";
        }
        x += L"<progress value=\"" + value + L"\" status=\"" + status + L"\"/>";
    }
    x += L"</binding></visual>";
    const size_t n = actions.size() < kToastMaxActions ? actions.size() : kToastMaxActions;
    if (n > 0) {
        x += L"<actions>";
        // The inputs first: a toast draws every input above the buttons.
        for (size_t i = 0; i < n; ++i) {
            if (!actions[i].reply) continue;
            x += L"<input id=\"" + toast_reply_id(i) + L"\" type=\"text\" placeHolderContent=\"" +
                 xml_escape(actions[i].reply_label) + L"\"/>";
        }
        for (size_t i = 0; i < n; ++i) {
            x += L"<action content=\"" + xml_escape(actions[i].title) + L"\" arguments=\"" +
                 xml_escape(toast_action_args(key, i)) + L"\" activationType=\"foreground\"";
            if (actions[i].reply) x += L" hint-inputId=\"" + toast_reply_id(i) + L"\"";
            x += L"/>";
        }
        x += L"</actions>";
    }
    if (!popup) x += L"<audio silent=\"true\"/>";
    x += L"</toast>";
    return x;
}

}  // namespace winapk::host
