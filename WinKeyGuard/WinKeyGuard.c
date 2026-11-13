#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

/*
 * WinKeyGuard 0.3 - Event Tracker (diagnostic only)
 *
 * Observes every LWIN/RWIN transition through WH_KEYBOARD_LL and, where
 * available, records the corresponding Raw Input keyboard event and device
 * interface path. It never blocks, swallows, remaps, or synthesizes input.
 *
 * Attribution limits:
 * - KBDLLHOOKSTRUCT identifies events Windows marks as injected, but does not
 *   expose the PID of the process that injected them.
 * - The foreground PID/image is context at event time, NOT the injector PID.
 * - Raw Input identifies the device handle/path that supplied a Raw Input
 *   event. Virtual HID devices can also appear as Raw Input devices.
 * - HOOK and RAW records are independent observations. Matching by key,
 *   direction and QPC time is temporal correlation, not guaranteed 1:1 pairing.
 * - No nearby RAW record is not proof of injection or of a kernel-mode source.
 *
 * Architecture:
 * - Dedicated hook thread: WH_KEYBOARD_LL + its own message pump.
 * - Dedicated Raw Input thread: message-only window + keyboard Raw Input.
 * - Separate single-producer/single-consumer ring buffers for the two threads.
 * - Main thread: sole consumer, process-image resolver and file logger.
 * - The low-level hook callback performs no I/O, process lookup, allocation,
 *   waits, or locks. It only snapshots small fields and enqueues a record.
 */

#define APP_NAME              L"WinKeyGuard"
#define APP_VERSION           L"0.3"
#define LOG_FILE_NAME         L"WinKeyGuard-0.3.log"
#define WORKER_READY_TIMEOUT  10000
#define MAIN_POLL_INTERVAL_MS 20
#define RING_CAPACITY         512
#define DEVICE_PATH_CHARS     512
#define PROCESS_PATH_CHARS    1024
#define ARRAY_COUNT(array)    (sizeof(array) / sizeof((array)[0]))

#if (RING_CAPACITY & (RING_CAPACITY - 1)) != 0
#error RING_CAPACITY must be a power of two
#endif

typedef enum EVENT_KIND {
    EVENT_HOOK = 1,
    EVENT_RAW = 2,
    EVENT_DEVICE = 3
} EVENT_KIND;

typedef enum WIN_KEY_INDEX {
    WIN_KEY_LEFT = 0,
    WIN_KEY_RIGHT = 1
} WIN_KEY_INDEX;

typedef enum KEY_DIRECTION {
    KEY_DIRECTION_DOWN = 0,
    KEY_DIRECTION_UP = 1
} KEY_DIRECTION;

typedef struct EVENT_RECORD {
    DWORD kind;
    DWORD key_index;
    DWORD direction;

    DWORD virtual_key;
    DWORD scan_code;
    DWORD hook_flags;
    DWORD hook_message_time;
    ULONG_PTR extra_info;

    USHORT raw_make_code;
    USHORT raw_flags;
    USHORT raw_virtual_key;
    DWORD raw_message;

    HANDLE raw_device;
    DWORD raw_device_change;

    HWND foreground_window;
    DWORD foreground_pid;

    LARGE_INTEGER qpc;
    FILETIME utc_time;

    wchar_t device_path[DEVICE_PATH_CHARS];
} EVENT_RECORD;

/* Each queue has exactly one producer thread and the main thread as consumer. */
static EVENT_RECORD g_hook_ring[RING_CAPACITY];
static volatile LONG g_hook_write = 0;
static volatile LONG g_hook_read = 0;
static volatile LONG g_hook_dropped = 0;

static EVENT_RECORD g_raw_ring[RING_CAPACITY];
static volatile LONG g_raw_write = 0;
static volatile LONG g_raw_read = 0;
static volatile LONG g_raw_dropped = 0;

static HANDLE g_stop_event = NULL;
static HANDLE g_hook_ready_event = NULL;
static HANDLE g_raw_ready_event = NULL;
static HANDLE g_hook_thread = NULL;
static HANDLE g_raw_thread = NULL;
static DWORD g_hook_thread_id = 0;
static DWORD g_raw_thread_id = 0;

static volatile LONG g_hook_active = 0;
static volatile LONG g_hook_error = 0;
static volatile LONG g_hook_runtime_error = 0;
static volatile LONG g_raw_active = 0;
static volatile LONG g_raw_error = 0;
static volatile LONG g_raw_runtime_error = 0;
static volatile LONG g_raw_window_error = 0;

static HHOOK g_keyboard_hook = NULL; /* Hook-thread-owned. */
static HWND g_raw_window = NULL;     /* Raw-thread-owned. */
static HINSTANCE g_instance = NULL;
static LARGE_INTEGER g_qpc_frequency;

static HANDLE g_log_file = INVALID_HANDLE_VALUE;
static HANDLE g_console_input = INVALID_HANDLE_VALUE;
static DWORD g_original_console_mode = 0;
static BOOL g_console_mode_saved = FALSE;
static BOOL g_console_mode_changed = FALSE;

static const wchar_t *KeyName(DWORD key_index)
{
    return (key_index == WIN_KEY_LEFT) ? L"LWIN" : L"RWIN";
}

static const wchar_t *DirectionName(DWORD direction)
{
    return (direction == KEY_DIRECTION_UP) ? L"UP" : L"DOWN";
}

static int GetWinKeyIndex(DWORD virtual_key)
{
    if (virtual_key == VK_LWIN)
        return WIN_KEY_LEFT;
    if (virtual_key == VK_RWIN)
        return WIN_KEY_RIGHT;
    return -1;
}

static int GetHookDirection(WPARAM message)
{
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        return KEY_DIRECTION_DOWN;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        return KEY_DIRECTION_UP;
    default:
        return -1;
    }
}

static BOOL RingPush(
    EVENT_RECORD *ring,
    volatile LONG *write_counter,
    volatile LONG *read_counter,
    volatile LONG *dropped_counter,
    const EVENT_RECORD *record)
{
    LONG write_index = InterlockedCompareExchange(write_counter, 0, 0);
    LONG read_index = InterlockedCompareExchange(read_counter, 0, 0);
    DWORD used = (DWORD)write_index - (DWORD)read_index;

    if (used >= RING_CAPACITY) {
        InterlockedIncrement(dropped_counter);
        return FALSE;
    }

    ring[(DWORD)write_index & (RING_CAPACITY - 1)] = *record;
    MemoryBarrier();
    InterlockedExchange(write_counter, (LONG)((DWORD)write_index + 1u));
    return TRUE;
}

static BOOL RingPop(
    EVENT_RECORD *ring,
    volatile LONG *write_counter,
    volatile LONG *read_counter,
    EVENT_RECORD *record)
{
    LONG read_index = InterlockedCompareExchange(read_counter, 0, 0);
    LONG write_index = InterlockedCompareExchange(write_counter, 0, 0);

    if (read_index == write_index)
        return FALSE;

    MemoryBarrier();
    *record = ring[(DWORD)read_index & (RING_CAPACITY - 1)];
    MemoryBarrier();
    InterlockedExchange(read_counter, (LONG)((DWORD)read_index + 1u));
    return TRUE;
}

static void CaptureCommonFields(EVENT_RECORD *record)
{
    QueryPerformanceCounter(&record->qpc);
    GetSystemTimeAsFileTime(&record->utc_time);
}

/* Called only by the hook callback; these are short window-manager queries. */
static void CaptureForegroundContext(EVENT_RECORD *record)
{
    DWORD process_id = 0;
    HWND foreground = GetForegroundWindow();

    record->foreground_window = foreground;
    if (foreground != NULL)
        GetWindowThreadProcessId(foreground, &process_id);

    record->foreground_pid = process_id;
}

static LRESULT CALLBACK LowLevelKeyboardProc(
    int code,
    WPARAM message,
    LPARAM data)
{
    if (code == HC_ACTION && data != 0) {
        const KBDLLHOOKSTRUCT *key_data =
            (const KBDLLHOOKSTRUCT *)data;
        const int key_index = GetWinKeyIndex(key_data->vkCode);
        const int direction = GetHookDirection(message);

        if (key_index >= 0 && direction >= 0) {
            EVENT_RECORD record;

            ZeroMemory(&record, sizeof(record));
            record.kind = EVENT_HOOK;
            record.key_index = (DWORD)key_index;
            record.direction = (DWORD)direction;
            record.virtual_key = key_data->vkCode;
            record.scan_code = key_data->scanCode;
            record.hook_flags = key_data->flags;
            record.hook_message_time = key_data->time;
            record.extra_info = key_data->dwExtraInfo;

            CaptureCommonFields(&record);
            CaptureForegroundContext(&record);
            (void)RingPush(
                g_hook_ring,
                &g_hook_write,
                &g_hook_read,
                &g_hook_dropped,
                &record
            );
        }
    }

    /* Diagnostic-only: always forward the event unchanged. */
    return CallNextHookEx(g_keyboard_hook, code, message, data);
}

static DWORD WINAPI HookThreadProc(LPVOID parameter)
{
    MSG message;
    int result;

    (void)parameter;
    PeekMessageW(&message, NULL, WM_USER, WM_USER, PM_NOREMOVE);

    g_keyboard_hook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        LowLevelKeyboardProc,
        g_instance,
        0
    );

    if (g_keyboard_hook == NULL) {
        InterlockedExchange(&g_hook_error, (LONG)GetLastError());
        SetEvent(g_hook_ready_event);
        return 1;
    }

    InterlockedExchange(&g_hook_active, 1);
    SetEvent(g_hook_ready_event);

    for (;;) {
        result = GetMessageW(&message, NULL, 0, 0);
        if (result == 0)
            break; /* WM_QUIT */
        if (result == -1) {
            InterlockedExchange(&g_hook_runtime_error, (LONG)GetLastError());
            break;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_keyboard_hook != NULL) {
        if (!UnhookWindowsHookEx(g_keyboard_hook)) {
            InterlockedCompareExchange(
                &g_hook_runtime_error,
                (LONG)GetLastError(),
                0
            );
        }
        g_keyboard_hook = NULL;
    }

    InterlockedExchange(&g_hook_active, 0);
    return (InterlockedCompareExchange(&g_hook_runtime_error, 0, 0) == 0) ? 0 : 2;
}

static void ReadRawDevicePath(HANDLE device, wchar_t *path, UINT path_chars)
{
    UINT chars = path_chars;
    UINT result;

    if (path == NULL || path_chars == 0)
        return;

    path[0] = L'\0';
    if (device == NULL)
        return;

    result = GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, path, &chars);
    if (result == (UINT)-1) {
        path[0] = L'\0';
        return;
    }

    path[path_chars - 1] = L'\0';
}

static void CaptureRawKeyboard(HRAWINPUT raw_handle)
{
    RAWINPUT raw_input;
    UINT size = (UINT)sizeof(raw_input);
    UINT result;
    int key_index;
    EVENT_RECORD record;
    const RAWKEYBOARD *keyboard;

    ZeroMemory(&raw_input, sizeof(raw_input));
    result = GetRawInputData(
        raw_handle,
        RID_INPUT,
        &raw_input,
        &size,
        (UINT)sizeof(RAWINPUTHEADER)
    );

    if (result == (UINT)-1 || raw_input.header.dwType != RIM_TYPEKEYBOARD)
        return;

    keyboard = &raw_input.data.keyboard;
    key_index = GetWinKeyIndex(keyboard->VKey);
    if (key_index < 0)
        return;

    ZeroMemory(&record, sizeof(record));
    record.kind = EVENT_RAW;
    record.key_index = (DWORD)key_index;
    record.direction = (keyboard->Flags & RI_KEY_BREAK)
        ? KEY_DIRECTION_UP
        : KEY_DIRECTION_DOWN;
    record.virtual_key = keyboard->VKey;
    record.raw_virtual_key = keyboard->VKey;
    record.raw_make_code = keyboard->MakeCode;
    record.raw_flags = keyboard->Flags;
    record.raw_message = keyboard->Message;
    record.extra_info = (ULONG_PTR)keyboard->ExtraInformation;
    record.raw_device = raw_input.header.hDevice;

    CaptureCommonFields(&record);
    ReadRawDevicePath(
        raw_input.header.hDevice,
        record.device_path,
        DEVICE_PATH_CHARS
    );

    (void)RingPush(
        g_raw_ring,
        &g_raw_write,
        &g_raw_read,
        &g_raw_dropped,
        &record
    );
}

static void CaptureRawDeviceChange(WPARAM change, HANDLE device)
{
    RID_DEVICE_INFO device_info;
    UINT size = (UINT)sizeof(device_info);
    UINT result;
    EVENT_RECORD record;

    ZeroMemory(&device_info, sizeof(device_info));
    device_info.cbSize = (DWORD)sizeof(device_info);

    result = GetRawInputDeviceInfoW(device, RIDI_DEVICEINFO, &device_info, &size);
    if (result == (UINT)-1 || device_info.dwType != RIM_TYPEKEYBOARD)
        return;

    ZeroMemory(&record, sizeof(record));
    record.kind = EVENT_DEVICE;
    record.raw_device = device;
    record.raw_device_change = (DWORD)change;
    CaptureCommonFields(&record);
    ReadRawDevicePath(device, record.device_path, DEVICE_PATH_CHARS);

    (void)RingPush(
        g_raw_ring,
        &g_raw_write,
        &g_raw_read,
        &g_raw_dropped,
        &record
    );
}

static LRESULT CALLBACK RawInputWindowProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam)
{
    switch (message) {
    case WM_INPUT:
        CaptureRawKeyboard((HRAWINPUT)lparam);
        /* Required by the Raw Input cleanup model for foreground input. */
        return DefWindowProcW(window, message, wparam, lparam);

    case WM_INPUT_DEVICE_CHANGE:
        CaptureRawDeviceChange(wparam, (HANDLE)lparam);
        return DefWindowProcW(window, message, wparam, lparam);

    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

static BOOL RegisterRawKeyboard(HWND target_window)
{
    RAWINPUTDEVICE device;

    ZeroMemory(&device, sizeof(device));
    device.usUsagePage = 0x01; /* HID_USAGE_PAGE_GENERIC */
    device.usUsage = 0x06;     /* HID_USAGE_GENERIC_KEYBOARD */
    device.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    device.hwndTarget = target_window;

    if (!RegisterRawInputDevices(&device, 1, (UINT)sizeof(device))) {
        InterlockedExchange(&g_raw_error, (LONG)GetLastError());
        return FALSE;
    }

    InterlockedExchange(&g_raw_active, 1);
    return TRUE;
}

static void UnregisterRawKeyboard(void)
{
    RAWINPUTDEVICE device;

    if (InterlockedCompareExchange(&g_raw_active, 0, 0) == 0)
        return;

    ZeroMemory(&device, sizeof(device));
    device.usUsagePage = 0x01;
    device.usUsage = 0x06;
    device.dwFlags = RIDEV_REMOVE;
    device.hwndTarget = NULL;

    if (!RegisterRawInputDevices(&device, 1, (UINT)sizeof(device))) {
        InterlockedCompareExchange(&g_raw_error, (LONG)GetLastError(), 0);
    }

    InterlockedExchange(&g_raw_active, 0);
}

static DWORD WINAPI RawInputThreadProc(LPVOID parameter)
{
    MSG message;
    WNDCLASSW window_class;
    BOOL class_registered = FALSE;
    BOOL raw_ready = FALSE;
    BOOL message_error = FALSE;
    int result;

    (void)parameter;
    PeekMessageW(&message, NULL, WM_USER, WM_USER, PM_NOREMOVE);

    ZeroMemory(&window_class, sizeof(window_class));
    window_class.lpfnWndProc = RawInputWindowProc;
    window_class.hInstance = g_instance;
    window_class.lpszClassName = L"WinKeyGuardRawInputWindow_03";

    if (RegisterClassW(&window_class) != 0) {
        class_registered = TRUE;
    } else if (GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        class_registered = TRUE;
    }

    if (class_registered) {
        g_raw_window = CreateWindowExW(
            0,
            window_class.lpszClassName,
            L"WinKeyGuard Raw Input",
            0,
            0, 0, 0, 0,
            HWND_MESSAGE,
            NULL,
            g_instance,
            NULL
        );
    }

    if (g_raw_window == NULL) {
        InterlockedExchange(&g_raw_window_error, (LONG)GetLastError());
        InterlockedExchange(&g_raw_error, (LONG)GetLastError());
        SetEvent(g_raw_ready_event);
        goto cleanup;
    }

    raw_ready = RegisterRawKeyboard(g_raw_window);
    SetEvent(g_raw_ready_event);

    if (!raw_ready)
        goto cleanup;

    for (;;) {
        result = GetMessageW(&message, NULL, 0, 0);
        if (result == 0)
            break; /* WM_QUIT */
        if (result == -1) {
            InterlockedExchange(&g_raw_runtime_error, (LONG)GetLastError());
            message_error = TRUE;
            break;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

cleanup:
    UnregisterRawKeyboard();

    if (g_raw_window != NULL) {
        DestroyWindow(g_raw_window);
        g_raw_window = NULL;
    }

    if (class_registered)
        UnregisterClassW(L"WinKeyGuardRawInputWindow_03", g_instance);

    return message_error ? 2 : 0;
}

static BOOL WINAPI ConsoleControlHandler(DWORD control_type)
{
    switch (control_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        if (g_stop_event != NULL)
            SetEvent(g_stop_event);
        return TRUE;
    default:
        return FALSE;
    }
}

static void DisableQuickEdit(void)
{
    DWORD mode;
    DWORD new_mode;

    g_console_input = GetStdHandle(STD_INPUT_HANDLE);
    if (g_console_input == NULL || g_console_input == INVALID_HANDLE_VALUE)
        return;

    if (!GetConsoleMode(g_console_input, &mode))
        return;

    g_original_console_mode = mode;
    g_console_mode_saved = TRUE;
    new_mode = (mode | (DWORD)ENABLE_EXTENDED_FLAGS) & ~(DWORD)ENABLE_QUICK_EDIT_MODE;

    if (new_mode != mode && SetConsoleMode(g_console_input, new_mode))
        g_console_mode_changed = TRUE;
}

static void RestoreConsoleMode(void)
{
    if (g_console_mode_saved && g_console_mode_changed &&
        g_console_input != NULL && g_console_input != INVALID_HANDLE_VALUE) {
        SetConsoleMode(g_console_input, g_original_console_mode);
    }
}

static BOOL BuildLogPath(wchar_t *path, size_t path_capacity)
{
    DWORD length;
    wchar_t *last_separator;
    size_t current_length;
    const size_t file_name_length = wcslen(LOG_FILE_NAME);

    if (path == NULL || path_capacity == 0 || path_capacity > 0xFFFFFFFFu)
        return FALSE;

    length = GetModuleFileNameW(NULL, path, (DWORD)path_capacity);
    if (length == 0 || length >= path_capacity)
        return FALSE;

    last_separator = wcsrchr(path, L'\\');
    if (last_separator == NULL)
        last_separator = wcsrchr(path, L'/');

    if (last_separator != NULL)
        last_separator[1] = L'\0';
    else
        path[0] = L'\0';

    current_length = wcslen(path);
    if (current_length + file_name_length + 1 > path_capacity)
        return FALSE;

    return wcscat_s(path, path_capacity, LOG_FILE_NAME) == 0;
}

static BOOL OpenLogFile(void)
{
    wchar_t path[MAX_PATH];

    if (!BuildLogPath(path, ARRAY_COUNT(path)))
        return FALSE;

    g_log_file = CreateFileW(
        path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (g_log_file == INVALID_HANDLE_VALUE)
        return FALSE;

    wprintf(L"Log file: %ls\n", path);
    return TRUE;
}

static BOOL WriteLogLine(const wchar_t *format, ...)
{
    wchar_t wide_line[4096];
    char utf8_line[16384];
    va_list arguments;
    int wide_result;
    int utf8_bytes;
    DWORD written;
    DWORD offset = 0;
    DWORD total_bytes;

    if (g_log_file == INVALID_HANDLE_VALUE)
        return FALSE;

    va_start(arguments, format);
    wide_result = _vsnwprintf_s(
        wide_line,
        ARRAY_COUNT(wide_line),
        _TRUNCATE,
        format,
        arguments
    );
    va_end(arguments);

    if (wide_result < 0 && wide_line[0] == L'\0')
        return FALSE;

    utf8_bytes = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide_line,
        -1,
        utf8_line,
        (int)sizeof(utf8_line),
        NULL,
        NULL
    );

    if (utf8_bytes <= 1)
        return FALSE;

    total_bytes = (DWORD)(utf8_bytes - 1);
    while (offset < total_bytes) {
        if (!WriteFile(g_log_file, utf8_line + offset, total_bytes - offset, &written, NULL) || written == 0)
            return FALSE;
        offset += written;
    }

    if (!WriteFile(g_log_file, "\r\n", 2, &written, NULL) || written != 2)
        return FALSE;

    return TRUE;
}

static void FormatEventTime(const FILETIME *utc_time, wchar_t *output, size_t capacity)
{
    FILETIME local_time;
    SYSTEMTIME system_time;

    if (utc_time == NULL || output == NULL || capacity == 0)
        return;

    if (!FileTimeToLocalFileTime(utc_time, &local_time) ||
        !FileTimeToSystemTime(&local_time, &system_time)) {
        wcscpy_s(output, capacity, L"time-unavailable");
        return;
    }

    swprintf_s(
        output,
        capacity,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        (unsigned)system_time.wYear,
        (unsigned)system_time.wMonth,
        (unsigned)system_time.wDay,
        (unsigned)system_time.wHour,
        (unsigned)system_time.wMinute,
        (unsigned)system_time.wSecond,
        (unsigned)system_time.wMilliseconds
    );
}

static void ResolveForegroundImage(DWORD process_id, wchar_t *path, DWORD path_chars)
{
    HANDLE process;
    DWORD size = path_chars;

    if (path == NULL || path_chars == 0)
        return;

    path[0] = L'\0';
    if (process_id == 0) {
        wcscpy_s(path, path_chars, L"<no-foreground-pid>");
        return;
    }

    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == NULL) {
        swprintf_s(path, path_chars, L"<unavailable:%lu>", GetLastError());
        return;
    }

    if (!QueryFullProcessImageNameW(process, 0, path, &size))
        swprintf_s(path, path_chars, L"<unavailable:%lu>", GetLastError());

    CloseHandle(process);
}

static void LogHookRecord(const EVENT_RECORD *record)
{
    wchar_t timestamp[64];
    wchar_t foreground_image[PROCESS_PATH_CHARS];
    BOOL injected = (record->hook_flags & LLKHF_INJECTED) != 0;
    BOOL lower_il = (record->hook_flags & LLKHF_LOWER_IL_INJECTED) != 0;
    BOOL alt_down = (record->hook_flags & LLKHF_ALTDOWN) != 0;
    BOOL extended = (record->hook_flags & LLKHF_EXTENDED) != 0;

    FormatEventTime(&record->utc_time, timestamp, ARRAY_COUNT(timestamp));
    ResolveForegroundImage(record->foreground_pid, foreground_image, ARRAY_COUNT(foreground_image));

    WriteLogLine(
        L"%ls QPC=%I64d HOOK %ls %ls vk=0x%02lX scan=0x%02lX flags=0x%08lX injected=%ls lowerIL=%ls altDown=%ls extended=%ls hookTime=%lu extra=0x%p fgHwnd=0x%p fgPid=%lu fgImage=\"%ls\"",
        timestamp,
        record->qpc.QuadPart,
        KeyName(record->key_index),
        DirectionName(record->direction),
        record->virtual_key,
        record->scan_code,
        record->hook_flags,
        injected ? L"YES" : L"NO",
        lower_il ? L"YES" : L"NO",
        alt_down ? L"YES" : L"NO",
        extended ? L"YES" : L"NO",
        record->hook_message_time,
        (void *)record->extra_info,
        (void *)record->foreground_window,
        record->foreground_pid,
        foreground_image
    );
}

static void LogRawRecord(const EVENT_RECORD *record)
{
    wchar_t timestamp[64];

    FormatEventTime(&record->utc_time, timestamp, ARRAY_COUNT(timestamp));

    WriteLogLine(
        L"%ls QPC=%I64d RAW  %ls %ls vkey=0x%02X make=0x%02X flags=0x%04X message=0x%04lX device=0x%p extra=0x%08lX path=\"%ls\"",
        timestamp,
        record->qpc.QuadPart,
        KeyName(record->key_index),
        DirectionName(record->direction),
        (unsigned)record->raw_virtual_key,
        (unsigned)record->raw_make_code,
        (unsigned)record->raw_flags,
        record->raw_message,
        record->raw_device,
        (DWORD)record->extra_info,
        record->device_path[0] ? record->device_path : L"<path-unavailable>"
    );
}

static void LogDeviceRecord(const EVENT_RECORD *record)
{
    wchar_t timestamp[64];
    const wchar_t *change_name = (record->raw_device_change == GIDC_ARRIVAL)
        ? L"ARRIVAL"
        : L"REMOVAL";

    FormatEventTime(&record->utc_time, timestamp, ARRAY_COUNT(timestamp));

    WriteLogLine(
        L"%ls QPC=%I64d RAW_DEVICE %ls handle=0x%p path=\"%ls\"",
        timestamp,
        record->qpc.QuadPart,
        change_name,
        record->raw_device,
        record->device_path[0] ? record->device_path : L"<path-unavailable>"
    );
}

static void LogRecord(const EVENT_RECORD *record)
{
    switch (record->kind) {
    case EVENT_HOOK:
        LogHookRecord(record);
        break;
    case EVENT_RAW:
        LogRawRecord(record);
        break;
    case EVENT_DEVICE:
        LogDeviceRecord(record);
        break;
    default:
        break;
    }
}

static void DrainRings(void)
{
    EVENT_RECORD hook_record;
    EVENT_RECORD raw_record;
    BOOL have_hook = RingPop(g_hook_ring, &g_hook_write, &g_hook_read, &hook_record);
    BOOL have_raw = RingPop(g_raw_ring, &g_raw_write, &g_raw_read, &raw_record);

    /* Merge both per-thread queues by QPC so the log remains time ordered. */
    while (have_hook || have_raw) {
        if (!have_raw || (have_hook && hook_record.qpc.QuadPart <= raw_record.qpc.QuadPart)) {
            LogRecord(&hook_record);
            have_hook = RingPop(g_hook_ring, &g_hook_write, &g_hook_read, &hook_record);
        } else {
            LogRecord(&raw_record);
            have_raw = RingPop(g_raw_ring, &g_raw_write, &g_raw_read, &raw_record);
        }
    }
}

static void ReportDroppedEvents(void)
{
    static LONG hook_reported = 0;
    static LONG raw_reported = 0;
    LONG hook_total = InterlockedCompareExchange(&g_hook_dropped, 0, 0);
    LONG raw_total = InterlockedCompareExchange(&g_raw_dropped, 0, 0);

    if (hook_total != hook_reported) {
        WriteLogLine(L"WARNING hook_ring_dropped=%ld total=%ld capacity=%u",
            hook_total - hook_reported, hook_total, (unsigned)RING_CAPACITY);
        hook_reported = hook_total;
    }

    if (raw_total != raw_reported) {
        WriteLogLine(L"WARNING raw_ring_dropped=%ld total=%ld capacity=%u",
            raw_total - raw_reported, raw_total, (unsigned)RING_CAPACITY);
        raw_reported = raw_total;
    }
}

static void WriteStartupInformation(void)
{
    LONG hook_error = InterlockedCompareExchange(&g_hook_error, 0, 0);
    LONG raw_error = InterlockedCompareExchange(&g_raw_error, 0, 0);
    LONG window_error = InterlockedCompareExchange(&g_raw_window_error, 0, 0);

    WriteLogLine(L"============================================================");
    WriteLogLine(L"%ls %ls - EVENT TRACKER / DIAGNOSTIC ONLY", APP_NAME, APP_VERSION);
    WriteLogLine(L"No keyboard events are blocked, swallowed, remapped or synthesized.");
    WriteLogLine(L"Capturing every LWIN/RWIN DOWN and UP seen by the low-level hook and Raw Input.");
    WriteLogLine(L"Foreground PID/image is context only; it is NOT the injecting process PID.");
    WriteLogLine(L"A Raw Input device path identifies the Raw Input source, not necessarily physical hardware.");
    WriteLogLine(L"Hook thread: %ls (error=%ld)",
        InterlockedCompareExchange(&g_hook_active, 0, 0) ? L"ACTIVE" : L"UNAVAILABLE",
        hook_error);
    WriteLogLine(L"Raw Input thread: %ls (error=%ld; window_error=%ld)",
        InterlockedCompareExchange(&g_raw_active, 0, 0) ? L"ACTIVE" : L"UNAVAILABLE",
        raw_error,
        window_error);
    WriteLogLine(L"Log records only LWIN/RWIN transitions and keyboard-device arrival/removal; typed text is not recorded.");
    WriteLogLine(L"HOOK and RAW records are separate observations. Correlate by QPC/key/direction; delivery times may differ.");
    WriteLogLine(L"QPC frequency=%I64d ticks/second.", g_qpc_frequency.QuadPart);
    WriteLogLine(L"============================================================");
}

static void DrainRingsAndReport(void)
{
    DrainRings();
    ReportDroppedEvents();
}

static BOOL StartWorker(
    HANDLE *thread_handle,
    DWORD *thread_id,
    DWORD (WINAPI *entry)(LPVOID),
    HANDLE ready_event,
    volatile LONG *error_slot)
{
    if (thread_id != NULL)
        *thread_id = 0;

    *thread_handle = CreateThread(NULL, 0, entry, NULL, 0, thread_id);
    if (*thread_handle == NULL) {
        InterlockedExchange(error_slot, (LONG)GetLastError());
        SetEvent(ready_event);
        return FALSE;
    }
    return TRUE;
}

static void StopWorker(HANDLE thread_handle, DWORD thread_id, const wchar_t *name)
{
    DWORD wait_result;
    DWORD exit_code = 0;
    DWORD attempts = 0;
    BOOL quit_posted = FALSE;

    if (thread_handle == NULL)
        return;

    while (WaitForSingleObject(thread_handle, 0) != WAIT_OBJECT_0 &&
           thread_id != 0 && attempts < 250) {
        if (PostThreadMessageW(thread_id, WM_QUIT, 0, 0)) {
            quit_posted = TRUE;
            break;
        }
        ++attempts;
        WaitForSingleObject(thread_handle, 20);
    }

    wait_result = WaitForSingleObject(thread_handle, 5000);
    if (wait_result == WAIT_TIMEOUT) {
        WriteLogLine(L"WARNING %ls thread did not stop within 5000 ms (quit_posted=%ls); waiting for orderly cleanup.",
            name, quit_posted ? L"YES" : L"NO");
        if (!quit_posted && thread_id != 0)
            PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
        wait_result = WaitForSingleObject(thread_handle, INFINITE);
    }

    if (wait_result == WAIT_OBJECT_0) {
        GetExitCodeThread(thread_handle, &exit_code);
        WriteLogLine(L"%ls thread stopped: exit_code=%lu", name, exit_code);
    } else {
        WriteLogLine(L"ERROR waiting for %ls thread: wait=%lu error=%lu", name, wait_result, GetLastError());
    }
}

int wmain(void)
{
    DWORD wait_result;
    DWORD hook_exit_code = 0;
    DWORD raw_exit_code = 0;
    BOOL hook_finished = FALSE;
    BOOL raw_finished = FALSE;
    BOOL hook_thread_created;
    BOOL raw_thread_created;
    int app_exit_code = 0;

    g_instance = GetModuleHandleW(NULL);
    QueryPerformanceFrequency(&g_qpc_frequency);
    DisableQuickEdit();

    if (!OpenLogFile()) {
        fwprintf(stderr, L"ERROR: Cannot create log file next to executable (error %lu).\n", GetLastError());
        RestoreConsoleMode();
        return 1;
    }

    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_hook_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_raw_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (g_stop_event == NULL || g_hook_ready_event == NULL || g_raw_ready_event == NULL) {
        fwprintf(stderr, L"ERROR: Cannot create synchronization events (error %lu).\n", GetLastError());
        if (g_stop_event != NULL) CloseHandle(g_stop_event);
        if (g_hook_ready_event != NULL) CloseHandle(g_hook_ready_event);
        if (g_raw_ready_event != NULL) CloseHandle(g_raw_ready_event);
        CloseHandle(g_log_file);
        RestoreConsoleMode();
        return 1;
    }

    if (!SetConsoleCtrlHandler(ConsoleControlHandler, TRUE)) {
        fwprintf(stderr, L"ERROR: Cannot install console control handler (error %lu).\n", GetLastError());
        CloseHandle(g_stop_event);
        CloseHandle(g_hook_ready_event);
        CloseHandle(g_raw_ready_event);
        CloseHandle(g_log_file);
        RestoreConsoleMode();
        return 1;
    }

    hook_thread_created = StartWorker(
        &g_hook_thread, &g_hook_thread_id, HookThreadProc,
        g_hook_ready_event, &g_hook_error);
    raw_thread_created = StartWorker(
        &g_raw_thread, &g_raw_thread_id, RawInputThreadProc,
        g_raw_ready_event, &g_raw_error);

    if (WaitForSingleObject(g_hook_ready_event, WORKER_READY_TIMEOUT) != WAIT_OBJECT_0)
        WriteLogLine(L"ERROR hook thread initialization timed out (error=%lu).", GetLastError());
    if (WaitForSingleObject(g_raw_ready_event, WORKER_READY_TIMEOUT) != WAIT_OBJECT_0)
        WriteLogLine(L"ERROR Raw Input thread initialization timed out (error=%lu).", GetLastError());

    WriteStartupInformation();

    wprintf(L"============================================================\n");
    wprintf(L"WinKeyGuard 0.3 - EVENT TRACKER (DIAGNOSTIC ONLY)\n");
    wprintf(L"Hook: %ls | Raw Input: %ls\n",
        InterlockedCompareExchange(&g_hook_active, 0, 0) ? L"ACTIVE" : L"UNAVAILABLE",
        InterlockedCompareExchange(&g_raw_active, 0, 0) ? L"ACTIVE" : L"UNAVAILABLE");
    wprintf(L"Only LWIN/RWIN transitions are recorded. Typed text is not captured.\n");
    wprintf(L"See WinKeyGuard-0.3.log next to the executable.\n");
    wprintf(L"Press Ctrl+C to stop.\n");
    wprintf(L"============================================================\n");

    if (!InterlockedCompareExchange(&g_hook_active, 0, 0)) {
        WriteLogLine(L"FATAL: the low-level keyboard hook is unavailable; global LWIN/RWIN event tracking cannot operate.");
        app_exit_code = 1;
        goto shutdown;
    }

    if (!InterlockedCompareExchange(&g_raw_active, 0, 0)) {
        WriteLogLine(L"WARNING: Raw Input is unavailable; hook events will still be recorded, but device correlation is disabled.");
    }

    for (;;) {
        DrainRingsAndReport();

        if (!hook_finished && g_hook_thread != NULL &&
            WaitForSingleObject(g_hook_thread, 0) == WAIT_OBJECT_0) {
            GetExitCodeThread(g_hook_thread, &hook_exit_code);
            hook_finished = TRUE;
            WriteLogLine(L"NOTICE hook thread ended: exit_code=%lu runtime_error=%ld",
                hook_exit_code,
                InterlockedCompareExchange(&g_hook_runtime_error, 0, 0));
            if (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
                WriteLogLine(L"FATAL: hook worker exited unexpectedly; subsequent keyboard events cannot be tracked.");
                app_exit_code = 1;
                break;
            }
        }

        if (!raw_finished && g_raw_thread != NULL &&
            WaitForSingleObject(g_raw_thread, 0) == WAIT_OBJECT_0) {
            GetExitCodeThread(g_raw_thread, &raw_exit_code);
            raw_finished = TRUE;
            WriteLogLine(L"NOTICE Raw Input thread ended: exit_code=%lu runtime_error=%ld",
                raw_exit_code,
                InterlockedCompareExchange(&g_raw_runtime_error, 0, 0));
        }

        if ((hook_finished || !hook_thread_created) &&
            (raw_finished || !raw_thread_created)) {
            WriteLogLine(L"NOTICE: both input workers have ended.");
            break;
        }

        wait_result = WaitForSingleObject(g_stop_event, MAIN_POLL_INTERVAL_MS);
        if (wait_result == WAIT_OBJECT_0)
            break;
        if (wait_result == WAIT_FAILED) {
            WriteLogLine(L"ERROR main wait failed: %lu", GetLastError());
            break;
        }
    }

shutdown:
    StopWorker(g_hook_thread, g_hook_thread_id, L"Hook");
    StopWorker(g_raw_thread, g_raw_thread_id, L"Raw Input");
    DrainRingsAndReport();
    WriteLogLine(L"Session ended.");
    FlushFileBuffers(g_log_file);

    if (g_hook_thread != NULL) CloseHandle(g_hook_thread);
    if (g_raw_thread != NULL) CloseHandle(g_raw_thread);
    if (g_stop_event != NULL) CloseHandle(g_stop_event);
    if (g_hook_ready_event != NULL) CloseHandle(g_hook_ready_event);
    if (g_raw_ready_event != NULL) CloseHandle(g_raw_ready_event);
    if (g_log_file != INVALID_HANDLE_VALUE) CloseHandle(g_log_file);

    RestoreConsoleMode();
    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);

    return app_exit_code;
}
