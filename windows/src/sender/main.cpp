// vrscreen-sender.exe: VRScreen for Windows. Mirrors a display (and the PC's sound) to the
// VRScreen headset app over the same wire protocol as the Mac sender (docs/protocol.md).
#include <winsock2.h>
#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>

#include <cstdio>
#include <stdexcept>

#include "app.h"
#include "d3d.h"
#include "log.h"
#include "options.h"
#include "util.h"

namespace {
vr::App* g_app = nullptr;

BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_app) g_app->requestQuit();
        return TRUE;
    }
    return FALSE;
}

bool attachConsole(bool allocate) {
    // Output already redirected to a file or pipe: keep it.
    const DWORD type = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) return true;
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !(allocate && AllocConsole())) return false;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    SetConsoleOutputCP(CP_UTF8);
    return true;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(vr::narrow(argv[i]));
    LocalFree(argv);

    vr::Options options;
    try {
        options = vr::parseOptions(args);
    } catch (const std::exception& e) {
        attachConsole(false);
        std::fprintf(stderr, "error: %s\n\n%s", e.what(), vr::kUsage);
        if (!GetConsoleWindow()) MessageBoxW(nullptr, vr::widen(e.what()).c_str(), L"VRScreen", MB_ICONERROR);
        return 2;
    }
    if (options.help || options.version || options.listDisplays) {
        attachConsole(false);
        if (options.help) std::printf("%s", vr::kUsage);
        if (options.version) std::printf("%s\n", VRSCREEN_VERSION);
        if (options.listDisplays) {
            const auto displays = vr::listDisplays();
            for (size_t i = 0; i < displays.size(); ++i)
                std::printf("%zu: %s %s on %s\n", i, displays[i].deviceName.c_str(), displays[i].label().c_str(),
                            displays[i].adapterName.c_str());
        }
        return 0;
    }
    if (options.console || !options.menu) attachConsole(options.console);
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    vr::openLogFile(vr::localDataDirectory() + L"\\Logs\\vrscreen.log");
    std::string commandLine;
    for (const auto& a : args) commandLine += " " + a;
    vr::logf("VRScreen %s for Windows (pid %lu, %s)%s", VRSCREEN_VERSION, GetCurrentProcessId(),
             vr::narrow(vr::exePath()).c_str(), commandLine.c_str());

    // One sender per port.
    const std::wstring mutexName = L"Local\\VRScreen.sender." + std::to_wstring(options.port);
    HANDLE instance = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        vr::logf("VRScreen is already running on port %u; exiting", options.port);
        if (options.menu)
            MessageBoxW(nullptr, L"VRScreen is already running: look for its icon in the notification area.", L"VRScreen",
                        MB_ICONINFORMATION);
        return 0;
    }

    // Every adb client (and a server it starts) gets ADB_MDNS=0: adb's mDNS backend can crash the server.
    SetEnvironmentVariableW(L"ADB_MDNS", L"0");
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);

    int code;
    {
        vr::App app(std::move(options));
        g_app = &app;
        code = app.run();
        g_app = nullptr;
    }
    MFShutdown();
    CoUninitialize();
    WSACleanup();
    vr::log("bye");
    if (instance) CloseHandle(instance);
    return code;
}
