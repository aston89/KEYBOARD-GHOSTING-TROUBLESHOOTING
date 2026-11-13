# WinKeyGuard 0.3 - Event Tracker

A native Win32 x64 diagnostic utility written in C. This build records every Left/Right Windows-key transition observed by the low-level keyboard hook and, where available, the corresponding Raw Input keyboard event and device interface path.

### Recorded events

- Every `VK_LWIN` and `VK_RWIN` DOWN/UP event observed by `WH_KEYBOARD_LL`.
- Hook flags, including `LLKHF_INJECTED` and `LLKHF_LOWER_IL_INJECTED`.
- Virtual key, scan code, hook message timestamp and `dwExtraInfo`.
- High-resolution `QueryPerformanceCounter` timestamp and local wall-clock timestamp.
- Foreground HWND and PID at hook time, plus executable image path where process access permits.
- LWIN/RWIN Raw Input transitions, when delivered through Raw Input.
- Raw Input device handle, make code, flags, keyboard message and device interface path.
- Keyboard Raw Input device arrival/removal notifications when Windows provides enough device information to identify the device.
- Ring-buffer overflow counters, if any records have to be dropped.

The utility records only LWIN/RWIN transitions and keyboard-device arrival/removal notifications. It does not record ordinary typed text or other key presses.

### Architecture and reliability

- A dedicated thread owns `WH_KEYBOARD_LL` and its message loop.
- A separate dedicated thread owns the Raw Input message-only window and its message loop. Raw Input device-path queries therefore do not run on the low-level hook thread.
- Each producer thread writes to its own fixed-size single-producer/single-consumer ring buffer.
- The main thread drains both queues, resolves foreground executable paths and writes the log. Records available in each drain pass are emitted in QPC order; events observed through the hook and Raw Input are still independent observations with potentially different delivery times.
- The low-level hook callback performs no file I/O, process-image lookup, heap allocation, waits, or locks.
- The program does not use `RIDEV_NOLEGACY` and does not request suppression of normal keyboard messages.
- Console Quick Edit is disabled while running and the original console mode is restored on normal shutdown.
- If the low-level hook cannot be installed, the program exits with an error rather than running as a misleading partial tracker. Raw Input is optional; if unavailable, hook events are still logged and the startup log records that device correlation is disabled.
- If the hook worker thread exits unexpectedly, the tracker records a fatal notice and shuts down.

### Understanding attribution

The log deliberately distinguishes three different things:

1. **Injected flag:** `injected=YES` means Windows marked the low-level keyboard event as injected. `lowerIL=YES` identifies the lower-integrity injected case. These flags do not reveal the PID of the process that called `SendInput` or `keybd_event`.
2. **Foreground process:** `fgPid` and `fgImage` identify the process owning the foreground window at the time of the hook callback. This is context only. It is **not** the process that injected the event.
3. **Raw Input device:** `RAW` records include the device handle and device interface path associated with that Raw Input record. This identifies the Raw Input source, which can be a physical keyboard or a virtual HID device.

`HOOK` and `RAW` are independent observations. Their timestamps use QPC, but a matching key and direction near the same time is temporal correlation, not a guaranteed one-to-one pairing. Raw Input delivery time and low-level-hook callback time can differ. A device-removal notification may have a handle but no readable path if Windows has already removed the device node.

An event with `injected=NO` is not proof of a physical keypress. Likewise, a hook event without a nearby Raw Input record is not proof of software injection or of a kernel-mode source.

### Example log records

```text
2026-11-09 12:00:00.100 QPC=123456789 HOOK LWIN DOWN vk=0x5B scan=0x5B flags=0x00000001 injected=NO ... fgPid=1234 fgImage="C:\\...\\app.exe"
2026-11-09 12:00:00.101 QPC=123456900 RAW  LWIN DOWN vkey=0x5B make=0x5B flags=0x0002 ... path="\\\\?\\HID#VID_..."
```

These lines are illustrative examples, not captured events.

## Build

Automated compilation through GitHub Actions:

- Toolchain: Microsoft Visual C++ (MSVC).
- Target: Windows x64 / AMD64.
- Binary format: PE32+.
- Runtime: static MSVC runtime (`/MT`).
- Additional link library: `user32.lib`.
- Source: `WinKeyGuard.c`.
- Executable: `WinKeyGuard.exe`.
- Workflow: `.github/workflows/build.yml`.

The workflow verifies the PE signature and x64 architecture, then uploads the executable as `WinKeyGuard-0.3-Event-Tracker-Windows-x64`.

## Usage

1. Download and extract the Windows x64 artifact.
2. Run `WinKeyGuard.exe` normally. Administrator privileges are not normally required for the hook or Raw Input registration.
3. Keep it running while reproducing the unexpected Windows-key behavior.
4. Inspect `WinKeyGuard-0.3.log` next to the executable. The file is opened in append mode, so new sessions are added to the existing log. Remove or rename the old log before a fresh test if you want a clean capture.
5. Press `Ctrl+C` to stop the utility and flush the log.

For the cleanest first diagnostic capture, run the tracker by itself. If you run it alongside a protective low-level hook, hook-chain order can affect which hooks see an event that another hook suppresses.

## Limitations

- The process that called `SendInput` or `keybd_event` cannot be identified directly from `KBDLLHOOKSTRUCT`.
- Foreground PID/image is not the injector PID.
- Raw Input identifies a device source but cannot guarantee that the device is physical rather than virtual.
- Kernel-mode filters or virtual HID paths may generate events that are not marked with `LLKHF_INJECTED`.
- Raw Input is a separate input path. A missing Raw Input match does not establish the origin of a hook event.
- Windows can silently remove a low-level keyboard hook after a callback timeout. The callback is deliberately minimal and runs on its own thread, but Windows provides no supported direct query to prove that the hook remains installed after silent removal.
- Both ring buffers are finite (512 records each). Any overflow is reported in the log.
- Raw Input registration is per device class within this process. This standalone utility registers its own keyboard Raw Input window and does not request `RIDEV_NOLEGACY`.
- Like other user-mode hooks, the tool operates on the current interactive desktop, not secure desktops such as the UAC secure desktop.

## Design principle

Record every observed LWIN/RWIN transition, preserve the available provenance and timing information, and never claim process attribution when Windows does not expose it.
