#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static HHOOK g_hook = NULL;
static DWORD g_main_thread_id = 0;

/* [LWIN/RWIN][DOWN/UP] */
static volatile LONG g_blocked[2][2] = {
    { 0, 0 },
    { 0, 0 }
};

enum {
    KEY_LWIN = 0,
    KEY_RWIN = 1,
    EVENT_DOWN = 0,
    EVENT_UP = 1
};

static int GetWinIndex(DWORD vk)
{
    if (vk == VK_LWIN)  return KEY_LWIN;
    if (vk == VK_RWIN)  return KEY_RWIN;
    return -1;
}

static LRESULT CALLBACK KeyboardProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT *key =
            (const KBDLLHOOKSTRUCT *)lParam;

        int index = GetWinIndex(key->vkCode);
        int direction = -1;

        if (wParam == WM_KEYDOWN ||
            wParam == WM_SYSKEYDOWN)
        {
            direction = EVENT_DOWN;
        }
        else if (wParam == WM_KEYUP ||
                 wParam == WM_SYSKEYUP)
        {
            direction = EVENT_UP;
        }

        /*
         * Block ONLY injected Windows-key events.
         * Physical/unmarked events pass through.
         */
        if (index >= 0 &&
            direction >= 0 &&
            (key->flags & LLKHF_INJECTED))
        {
            InterlockedIncrement(
                &g_blocked[index][direction]
            );

            return 1;
        }
    }

    return CallNextHookEx(
        g_hook,
        nCode,
        wParam,
        lParam
    );
}

static void PrintCounters(void)
{
    LONG ld = InterlockedExchange(
        &g_blocked[KEY_LWIN][EVENT_DOWN], 0);

    LONG lu = InterlockedExchange(
        &g_blocked[KEY_LWIN][EVENT_UP], 0);

    LONG rd = InterlockedExchange(
        &g_blocked[KEY_RWIN][EVENT_DOWN], 0);

    LONG ru = InterlockedExchange(
        &g_blocked[KEY_RWIN][EVENT_UP], 0);

    if (ld || lu || rd || ru)
    {
        printf(
            "[BLOCKED] LWIN DOWN=%ld UP=%ld | "
            "RWIN DOWN=%ld UP=%ld\n",
            ld, lu, rd, ru
        );

        fflush(stdout);
    }
}

static BOOL WINAPI ConsoleHandler(DWORD type)
{
    if (type == CTRL_C_EVENT ||
        type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT)
    {
        if (g_main_thread_id != 0)
        {
            PostThreadMessageW(
                g_main_thread_id,
                WM_QUIT,
                0,
                0
            );

            return TRUE;
        }
    }

    return FALSE;
}

int main(void)
{
    MSG msg;
    UINT_PTR timer_id;

    g_main_thread_id = GetCurrentThreadId();

    /* Ensure the thread message queue exists. */
    PeekMessageW(
        &msg, NULL,
        WM_USER, WM_USER,
        PM_NOREMOVE
    );

    SetConsoleCtrlHandler(
        ConsoleHandler,
        TRUE
    );

    g_hook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        KeyboardProc,
        GetModuleHandleW(NULL),
        0
    );

    if (!g_hook)
    {
        DWORD error = GetLastError();

        fprintf(
            stderr,
            "ERROR: SetWindowsHookEx failed: %lu\n",
            error
        );

        return 1;
    }

    timer_id = SetTimer(NULL, 1, 1000, NULL);

    if (timer_id == 0)
    {
        DWORD error = GetLastError();

        fprintf(
            stderr,
            "ERROR: SetTimer failed: %lu\n",
            error
        );

        UnhookWindowsHookEx(g_hook);
        return 1;
    }

    puts("==================================");
    puts("       WIN STATE GUARD 0.1");
    puts("==================================");
    puts("Injected LWIN/RWIN events: BLOCKED");
    puts("Non-injected keyboard events: ALLOWED");
    puts("Ctrl+C to exit.");
    puts("");

    /*
     * WH_KEYBOARD_LL requires a message loop.
     */
    for (;;)
    {
        BOOL result = GetMessageW(
            &msg, NULL, 0, 0
        );

        if (result <= 0)
            break;

        if (msg.message == WM_TIMER &&
            msg.wParam == timer_id)
        {
            PrintCounters();
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(NULL, timer_id);

    UnhookWindowsHookEx(g_hook);
    g_hook = NULL;

    PrintCounters();

    puts("WinStateGuard stopped.");

    return 0;
}
