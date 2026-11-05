#include "tray.h"

#include <shellapi.h>

#include "util.h"

namespace vr {

MenuBuilder::MenuBuilder() {
    root_ = CreatePopupMenu();
    stack_.push_back(root_);
}

MenuBuilder::~MenuBuilder() { DestroyMenu(root_); }

static std::wstring menuText(const std::string& text) {
    // '&' would underline the next character.
    std::wstring w = widen(text);
    std::wstring out;
    for (wchar_t c : w) {
        if (c == L'&') out += L"&&";
        else out += c;
    }
    return out;
}

void MenuBuilder::item(const std::string& text, std::function<void()> action, bool checked, bool enabled, bool isDefault) {
    actions_.push_back(std::move(action));
    const UINT id = UINT(actions_.size());
    UINT flags = MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED);
    AppendMenuW(current(), flags, id, menuText(text).c_str());
    if (isDefault) SetMenuDefaultItem(current(), id, FALSE);
}

void MenuBuilder::radio(const std::string& text, bool checked, std::function<void()> action, bool enabled) {
    actions_.push_back(std::move(action));
    const UINT id = UINT(actions_.size());
    AppendMenuW(current(), MF_STRING | (enabled ? 0 : MF_GRAYED), id, menuText(text).c_str());
    if (checked) {
        MENUITEMINFOW info{sizeof info};
        info.fMask = MIIM_FTYPE | MIIM_STATE;
        info.fType = MFT_RADIOCHECK;
        info.fState = MFS_CHECKED | (enabled ? 0 : MFS_DISABLED);
        SetMenuItemInfoW(current(), id, FALSE, &info);
    }
}

void MenuBuilder::label(const std::string& text) {
    actions_.push_back(nullptr);
    AppendMenuW(current(), MF_STRING | MF_GRAYED, UINT(actions_.size()), menuText(text).c_str());
}

void MenuBuilder::separator() { AppendMenuW(current(), MF_SEPARATOR, 0, nullptr); }

void MenuBuilder::submenu(const std::string& text, bool enabled) {
    HMENU sub = CreatePopupMenu();
    AppendMenuW(current(), MF_POPUP | (enabled ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(sub), menuText(text).c_str());
    stack_.push_back(sub);
}

void MenuBuilder::endSubmenu() {
    if (stack_.size() > 1) stack_.pop_back();
}

void MenuBuilder::show(HWND owner) {
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(owner);  // so the menu closes when clicking elsewhere
    const UINT id = UINT(TrackPopupMenuEx(root_, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                          pt.x, pt.y, owner, nullptr));
    PostMessageW(owner, WM_NULL, 0, 0);
    if (id >= 1 && id <= actions_.size() && actions_[id - 1]) actions_[id - 1]();
}

// MARK: TrayIcon

static void fillTip(wchar_t* dest, size_t capacity, const std::string& text) {
    std::wstring w = widen(text);
    if (w.size() >= capacity) w.resize(capacity - 1);
    wcscpy_s(dest, capacity, w.c_str());
}

bool TrayIcon::add(HWND hwnd, HICON icon, const std::string& tooltip) {
    hwnd_ = hwnd;
    icon_ = icon;
    tooltip_ = tooltip;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kCallbackMessage;
    nid.hIcon = icon;
    fillTip(nid.szTip, ARRAYSIZE(nid.szTip), tooltip);
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    return added_;
}

void TrayIcon::update(HICON icon, const std::string& tooltip) {
    icon_ = icon;
    tooltip_ = tooltip;
    if (!added_) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = icon;
    fillTip(nid.szTip, ARRAYSIZE(nid.szTip), tooltip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::balloon(const std::string& title, const std::string& text) {
    if (!added_) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    fillTip(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
    fillTip(nid.szInfo, ARRAYSIZE(nid.szInfo), text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::remove() {
    if (!added_) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

void TrayIcon::readd() {
    added_ = false;
    add(hwnd_, icon_, tooltip_);
}

}  // namespace vr
