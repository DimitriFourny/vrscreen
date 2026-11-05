// Notification-area icon and its popup menu (Win32). UI thread only.
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace vr {

// Builds a popup menu whose items run std::functions.
class MenuBuilder {
public:
    MenuBuilder();
    ~MenuBuilder();
    MenuBuilder(const MenuBuilder&) = delete;

    void item(const std::string& text, std::function<void()> action, bool checked = false, bool enabled = true,
              bool isDefault = false);
    void radio(const std::string& text, bool checked, std::function<void()> action, bool enabled = true);
    void label(const std::string& text);  // disabled text
    void separator();
    // Starts a submenu; items go into it until `endSubmenu`.
    void submenu(const std::string& text, bool enabled = true);
    void endSubmenu();
    // Shows the menu at the cursor; runs the chosen action.
    void show(HWND owner);

private:
    HMENU current() const { return stack_.back(); }
    std::vector<HMENU> stack_;
    std::vector<std::function<void()>> actions_;
    HMENU root_;
};

class TrayIcon {
public:
    static constexpr UINT kCallbackMessage = WM_APP + 10;
    bool add(HWND hwnd, HICON icon, const std::string& tooltip);
    void update(HICON icon, const std::string& tooltip);
    void balloon(const std::string& title, const std::string& text);
    void remove();
    // Explorer restarted: add the icon again.
    void readd();

private:
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    std::string tooltip_;
    bool added_ = false;
};

// The tray icon: a headset glyph drawn at the system's small-icon size; `badge` adds a "!".
HICON makeTrayIcon(bool badge, bool active);

}  // namespace vr
