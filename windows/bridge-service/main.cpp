#include <windows.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <filesystem>
#include <string>

namespace {

constexpr wchar_t kServiceName[] = L"ScrcpyBridgeService";
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stopEvent = nullptr;
PROCESS_INFORMATION g_mic{};
PROCESS_INFORMATION g_cam{};

std::wstring exeDirectory() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!len || len >= MAX_PATH) {
        return L".";
    }
    return std::filesystem::path(std::wstring(path, len)).parent_path().wstring();
}

void closeProcess(PROCESS_INFORMATION& pi) {
    if (pi.hThread) {
        CloseHandle(pi.hThread);
        pi.hThread = nullptr;
    }
    if (pi.hProcess) {
        CloseHandle(pi.hProcess);
        pi.hProcess = nullptr;
    }
    pi.dwProcessId = 0;
    pi.dwThreadId = 0;
}

bool isAlive(PROCESS_INFORMATION& pi) {
    if (!pi.hProcess) {
        return false;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &code) || code != STILL_ACTIVE) {
        closeProcess(pi);
        return false;
    }
    return true;
}

bool launchInActiveSession(const std::wstring& commandLine, PROCESS_INFORMATION& pi) {
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        return false;
    }

    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &userToken)) {
        return false;
    }

    HANDLE primaryToken = nullptr;
    bool ok = DuplicateTokenEx(userToken, TOKEN_ALL_ACCESS, nullptr,
                               SecurityImpersonation, TokenPrimary, &primaryToken) != FALSE;
    CloseHandle(userToken);
    if (!ok) {
        return false;
    }

    void* environment = nullptr;
    CreateEnvironmentBlock(&environment, primaryToken, FALSE);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    std::wstring mutableCommand = commandLine;
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    PROCESS_INFORMATION child{};
    ok = CreateProcessAsUserW(primaryToken, nullptr, mutableCommand.data(),
                              nullptr, nullptr, FALSE, flags,
                              environment, exeDirectory().c_str(), &si, &child) != FALSE;

    if (environment) {
        DestroyEnvironmentBlock(environment);
    }
    CloseHandle(primaryToken);

    if (!ok) {
        return false;
    }

    closeProcess(pi);
    pi = child;
    return true;
}

void terminateWorker(PROCESS_INFORMATION& pi) {
    if (!pi.hProcess) {
        return;
    }
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    closeProcess(pi);
}

void reportStatus(DWORD state, DWORD win32Exit = NO_ERROR, DWORD waitHint = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = win32Exit;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted = (state == SERVICE_START_PENDING)
        ? 0
        : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
    SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI controlHandler(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        reportStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
        SetEvent(g_stopEvent);
    }
    return NO_ERROR;
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, controlHandler, nullptr);
    if (!g_statusHandle) {
        return;
    }

    reportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        reportStatus(SERVICE_STOPPED, GetLastError());
        return;
    }

    const std::wstring dir = exeDirectory();
    const std::wstring micExe = (std::filesystem::path(dir) / L"scrcpy-vmic-bridge.exe").wstring();
    const std::wstring camExe = (std::filesystem::path(dir) / L"scrcpy-vcam-register.exe").wstring();
    const std::wstring micCmd = L"\"" + micExe + L"\" \"CABLE Input\"";
    const std::wstring camCmd = L"\"" + camExe + L"\" --headless";

    reportStatus(SERVICE_RUNNING);

    while (WaitForSingleObject(g_stopEvent, 2000) == WAIT_TIMEOUT) {
        if (!std::filesystem::exists(micExe) || !std::filesystem::exists(camExe)) {
            continue;
        }

        if (!isAlive(g_cam)) {
            launchInActiveSession(camCmd, g_cam);
        }
        if (!isAlive(g_mic)) {
            // This worker may exit while scrcpy is not running. Retrying every
            // two seconds is intentional; once the Local\\ shared mapping exists,
            // it attaches automatically and feeds VB-CABLE's render endpoint.
            launchInActiveSession(micCmd, g_mic);
        }
    }

    terminateWorker(g_mic);
    terminateWorker(g_cam);
    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
    reportStatus(SERVICE_STOPPED);
}

} // namespace

int wmain() {
    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(kServiceName), serviceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(table)) {
        return static_cast<int>(GetLastError());
    }
    return 0;
}
