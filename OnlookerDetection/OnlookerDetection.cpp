#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Sensors.h>

#pragma comment(lib, "RuntimeObject.lib")
#pragma comment(lib, "WindowsApp.lib")

using namespace winrt;
using namespace Windows::Devices::Sensors;

// ============================================================
// Application rules
// ============================================================

enum class WindowAction
{
    Minimise,
    Close
};

struct ApplicationRule
{
    std::wstring executableName;
    WindowAction action;
};

/*
 * Add or remove programmes here.
 *
 * Use the executable name shown in Task Manager, under Details.
 * Matching is case-insensitive.
 */
static const std::vector<ApplicationRule> Rules
{
    { L"notepad.exe",       WindowAction::Minimise },
    { L"ms-teams.exe",      WindowAction::Minimise },
    { L"Teams.exe",         WindowAction::Minimise },
    { L"outlook.exe",       WindowAction::Minimise }
};

// ============================================================
// Global state
// ============================================================

static std::mutex actionMutex;
static std::chrono::steady_clock::time_point lastAction{};

static std::atomic_bool onlookerCurrentlyPresent{ false };

/*
 * These are stored globally so the sensor and event subscription
 * remain valid for the lifetime of the application.
 */
static HumanPresenceSensor presenceSensor{ nullptr };
static event_token readingChangedToken{};

/*
 * The identifier of the main thread. The sensor callback runs on a
 * worker thread, so it must post its request to THIS thread, which
 * owns the message loop in wWinMain.
 */
static DWORD mainThreadId = 0;

// Prevent repeated application actions within this interval.
static constexpr auto ActionCooldown = std::chrono::seconds(5);

// Request the latest sensor reading ten times per second.
static constexpr auto SensorCheckInterval =
    std::chrono::milliseconds(100);

// Custom message used to perform window actions on the main thread.
static constexpr UINT WM_PROTECT_APPLICATIONS = WM_APP + 1;
static constexpr UINT_PTR PresenceCheckTimerId = 1;

// ============================================================
// Utility functions
// ============================================================

std::wstring ToLower(std::wstring value)
{
    for (auto& character : value)
    {
        character = static_cast<wchar_t>(
            std::towlower(static_cast<wint_t>(character)));
    }

    return value;
}

std::wstring GetProcessName(DWORD processId)
{
    std::wstring result;

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPPROCESS,
        0);

    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (entry.th32ProcessID == processId)
            {
                result = entry.szExeFile;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);

    return result;
}

const ApplicationRule* FindRule(
    const std::wstring& executableName)
{
    const std::wstring normalisedName = ToLower(executableName);

    for (const auto& rule : Rules)
    {
        if (ToLower(rule.executableName) == normalisedName)
        {
            return &rule;
        }
    }

    return nullptr;
}

// ============================================================
// Window handling
// ============================================================

BOOL CALLBACK HandleWindow(
    HWND window,
    LPARAM)
{
    if (!IsWindow(window) || !IsWindowVisible(window))
    {
        return TRUE;
    }

    /*
     * Ignore child windows. EnumWindows normally returns top-level
     * windows, but this provides an additional safeguard.
     */
    if (GetWindow(window, GW_OWNER) != nullptr)
    {
        return TRUE;
    }

    DWORD processId = 0;

    GetWindowThreadProcessId(
        window,
        &processId);

    if (processId == 0 ||
        processId == GetCurrentProcessId())
    {
        return TRUE;
    }

    const std::wstring processName =
        GetProcessName(processId);

    if (processName.empty())
    {
        return TRUE;
    }

    const ApplicationRule* rule =
        FindRule(processName);

    if (rule == nullptr)
    {
        return TRUE;
    }

    switch (rule->action)
    {
    case WindowAction::Minimise:
    {
        ShowWindowAsync(window, SW_MINIMIZE);

        std::wstring debugMessage =
            L"Minimised: " + processName + L"\n";

        OutputDebugStringW(debugMessage.c_str());
        break;
    }

    case WindowAction::Close:
    {
        /*
         * WM_CLOSE requests a normal application shutdown.
         * It does not forcibly terminate the process.
         */
        PostMessageW(window, WM_CLOSE, 0, 0);

        std::wstring debugMessage =
            L"Requested close: " + processName + L"\n";

        OutputDebugStringW(debugMessage.c_str());
        break;
    }
    }

    return TRUE;
}

void ProtectApplications()
{
    std::scoped_lock lock(actionMutex);

    const auto now =
        std::chrono::steady_clock::now();

    if (lastAction.time_since_epoch().count() != 0 &&
        now - lastAction < ActionCooldown)
    {
        OutputDebugStringW(
            L"Protection action ignored because of cooldown.\n");

        return;
    }

    lastAction = now;

    OutputDebugStringW(
        L"Protecting configured applications.\n");

    EnumWindows(HandleWindow, 0);
}

// ============================================================
// Presence sensor handling
// ============================================================

void ProcessPresenceReading(
    HumanPresenceSensorReading const& reading)
{
    const HumanPresence state =
        reading.OnlookerPresence();

    if (state == HumanPresence::Present)
    {
        /*
         * exchange() returns the previous state.
         * Applications are protected only when the
         * state changes to Present.
         */
        const bool wasAlreadyPresent =
            onlookerCurrentlyPresent.exchange(true);

        if (!wasAlreadyPresent)
        {
            OutputDebugStringW(
                L"Onlooker detected.\n");

            /*
             * Sensor callbacks occur on a worker thread.
             * Post the action to the MAIN thread, which
             * owns the message loop. Using
             * GetCurrentThreadId() here would target the
             * worker thread, so the message would be lost.
             */
            PostThreadMessageW(
                mainThreadId,
                WM_PROTECT_APPLICATIONS,
                0,
                0);
        }
    }
    else if (state == HumanPresence::NotPresent)
    {
        const bool wasPresent =
            onlookerCurrentlyPresent.exchange(false);

        if (wasPresent)
        {
            OutputDebugStringW(
                L"Onlooker no longer detected.\n");
        }
    }
    else
    {
        OutputDebugStringW(
            L"Onlooker state is unknown.\n");
    }
}

void CheckCurrentPresence()
{
    try
    {
        const auto reading =
            presenceSensor.GetCurrentReading();

        if (reading)
        {
            ProcessPresenceReading(reading);
        }
    }
    catch (winrt::hresult_error const& error)
    {
        std::wstring debugMessage =
            L"Presence reading error: " +
            std::wstring(error.message()) +
            L"\n";

        OutputDebugStringW(debugMessage.c_str());
    }
}

void StopPresenceMonitoring()
{
    if (presenceSensor)
    {
        /*
         * Remove the event handler using the token returned when
         * the handler was registered.
         */
        if (readingChangedToken.value != 0)
        {
            presenceSensor.ReadingChanged(
                readingChangedToken);

            readingChangedToken = {};
        }

        presenceSensor = nullptr;
    }
}

bool StartPresenceMonitoring()
{
    presenceSensor =
        HumanPresenceSensor::GetDefault();

    if (!presenceSensor)
    {
        MessageBoxW(
            nullptr,
            L"No compatible Human Presence Sensor was found.",
            L"Onlooker detection",
            MB_OK | MB_ICONWARNING);

        return false;
    }

    OutputDebugStringW(
        L"Human Presence Sensor found.\n");

    /*
     * OnlookerPresence is the relevant reading rather than normal
     * Presence. It reports whether someone other than the primary
     * user is detected and engaged with the device.
     *
     * NOTE: Do NOT use 'auto' here. That would create a new local
     * variable and leave the global token empty, breaking cleanup.
     */
    readingChangedToken = presenceSensor.ReadingChanged(
        [](auto const& sender,
            HumanPresenceSensorReadingChangedEventArgs const& args)
        {
            try
            {
                ProcessPresenceReading(args.Reading());
            }
            catch (winrt::hresult_error const& error)
            {
                std::wstring debugMessage =
                    L"Presence reading error: " +
                    std::wstring(error.message()) +
                    L"\n";

                OutputDebugStringW(debugMessage.c_str());
            }
        });

    OutputDebugStringW(
        L"Listening for presence events.\n");

    return true;
}

// ============================================================
// Entry point
// ============================================================

int WINAPI wWinMain(
    _In_ HINSTANCE,
    _In_opt_ HINSTANCE,
    _In_ PWSTR,
    _In_ int)
{
    winrt::init_apartment(
        winrt::apartment_type::multi_threaded);

    int exitCode = 0;

    try
    {
        /*
         * Record the main thread identifier so the sensor callback
         * (running on a worker thread) can post messages here.
         */
        mainThreadId = GetCurrentThreadId();

        /*
         * Ensure this thread has a message queue before the sensor
         * callback attempts to call PostThreadMessageW.
         */
        MSG message{};

        PeekMessageW(
            &message,
            nullptr,
            WM_USER,
            WM_USER,
            PM_NOREMOVE);

        if (!StartPresenceMonitoring())
        {
            winrt::uninit_apartment();
            return 1;
        }

        if (SetTimer(
                nullptr,
                PresenceCheckTimerId,
                static_cast<UINT>(SensorCheckInterval.count()),
                nullptr) == 0)
        {
            StopPresenceMonitoring();
            winrt::uninit_apartment();

            MessageBoxW(
                nullptr,
                L"Unable to start the presence sensor timer.",
                L"Onlooker detection error",
                MB_OK | MB_ICONERROR);

            return 1;
        }

        BOOL result = 0;

        while ((result = GetMessageW(
            &message,
            nullptr,
            0,
            0)) != 0)
        {
            if (result == -1)
            {
                exitCode =
                    static_cast<int>(GetLastError());

                break;
            }

            /*
             * Thread messages arrive with a null window handle, so
             * check the message value directly before dispatching.
             */
            if (message.message == WM_PROTECT_APPLICATIONS)
            {
                ProtectApplications();
                continue;
            }

            if (message.message == WM_TIMER &&
                message.wParam == PresenceCheckTimerId)
            {
                CheckCurrentPresence();
                continue;
            }

            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        KillTimer(nullptr, PresenceCheckTimerId);
        StopPresenceMonitoring();
    }
    catch (winrt::hresult_error const& error)
    {
        KillTimer(nullptr, PresenceCheckTimerId);
        StopPresenceMonitoring();

        std::wstring errorMessage =
            L"Windows Runtime error:\n" +
            std::wstring(error.message());

        MessageBoxW(
            nullptr,
            errorMessage.c_str(),
            L"Onlooker detection error",
            MB_OK | MB_ICONERROR);

        exitCode =
            static_cast<int>(error.code());
    }
    catch (...)
    {
        KillTimer(nullptr, PresenceCheckTimerId);
        StopPresenceMonitoring();

        MessageBoxW(
            nullptr,
            L"An unexpected error occurred.",
            L"Onlooker detection error",
            MB_OK | MB_ICONERROR);

        exitCode = 1;
    }

    winrt::uninit_apartment();

    return exitCode;
}