// HertzyEVS.exe: launcher at the top of the portable package (ship_hertzy.ps1). Starts app\hertzyevs.exe next to it,
// so the person who unzips the package sees one icon instead of a folder of Qt/Arena DLLs. The app itself finds the
// recordings folder one level up (defaultRoot in main.cpp).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    std::wstring dir(buf, n);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    const std::wstring appDir = dir + L"\\app";
    const std::wstring exe = appDir + L"\\hertzyevs.exe";
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, appDir.c_str(), &si, &pi)) {
        const std::wstring msg = L"Could not start\n" + exe + L"\n\nUnzip the whole package first (do not run it from inside the zip).";
        MessageBoxW(nullptr, msg.c_str(), L"HertzyEVS", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
