
# Mechanical Keyboard Ghosting with High Polling Rate on Windows

---

## Table of Contents

- [1a. Introduction & disclaimer](#1a-introduction--disclaimer)
- [1b. Symptoms & quick solution (fast startup)](#1b-symptoms--quick-solution-fast-startup)
- [2. Understanding "KeyboardDataQueueSize" and the Myth of Latency](#2-understanding-keyboarddataqueuesize-and-the-myth-of-latency)
- [3. Different Types of Keyboard Input and Why Ghosting Differs in Games](#3-different-types-of-keyboard-input-and-why-ghosting-differs-in-games)
- [4. The Hidden Culprit: Accessibility Flags That Stay Active (Even When You Think They're Off)](#4-the-hidden-culprit-accessibility-flags-that-stay-active-even-when-you-think-theyre-off)
- [5. keyboardfilter.sys: The Silent Hardware Filter You (Probably) Don’t Need](#5-keyboardfiltersys-the-silent-hardware-filter-you-probably-dont-need)
- [6. Preventing USB Polling Rate Drifting and Power Management Workarounds to Reduce Input Lag](#6-preventing-usb-polling-rate-drifting-and-power-management-workarounds-to-reduce-input-lag)
- [7. Other Potential Causes and Considerations](#7-other-potential-causes-and-considerations)
- [8. 5KRO & 6KRO vs NKRO: Why "Less" Is Sometimes Better](#8-5kro--6kro-vs-nkro-why-less-is-sometimes-better)
- [9. EXTRA step: Set TextInputHost.exe to High Priority at User Logon](#9-extra-step--set-textinputhostexe-to-high-priority-at-user-logon)
- [10. Problem still persists? Check the keyboard](#10-problem-still-persists-check-the-keyboard)
- [11. Problem still persists? Check your hardware (peripherals and more)](#11-problem-still-persists-check-your-hardware-peripherals-and-more)
- [12. Problem still persists? Try Windows FilterKeys to reduce mechanical switch chatter](#12-problem-still-persists-try-windows-filterkeys-to-reduce-mechanical-switch-hattering)

---

## 1a. Introduction & disclaimer

Mechanical keyboards, especially those running at high polling rates (1000 Hz or above), offer extremely fast and precise input. However, they can also expose subtle issues within the Windows input stack that lead to “ghosting”, unintended or spurious key events that confuse the system and cause erratic behavior.

This repository is primarily a collection of **workarounds discovered while troubleshooting a very specific legacy hardware configuration**, not a universal explanation of keyboard ghosting on Windows.
Some of the procedures described here are intentionally aggressive and were useful because they mitigated symptoms in a system operating close to the limits of an older PCI/USB/firmware stack. They should therefore be treated as **diagnostic and/or situational workarounds**, not as generally recommended optimizations.

The original failure involved at least two distinct hardware-related factors:
1. **Legacy PCI / X-Fi DMA interaction** - Certain PCI X-Fi sound cards were interfering with DMA behavior on the affected system. Switching from legacy BIOS/CSM to pure UEFI reduced the problem in some configurations, but did not always eliminate it. As a result, several Windows and device-level workarounds could appear to “fix” keyboard input even though they were actually mitigating a deeper PCI/DMA interaction.
2. **Keyboard electrical / HID interaction** - The keyboard itself exhibited a reproducible failure under a particular combination of NKRO enabled, additional HID functionality and all keyboard LEDs forced continuously to green (try to change them to another color, green is considered not safe in some keyboard models).

---

## 1b. Symptoms & quick solution (fast startup)

Users experiencing this issue typically report:
- Random or stuck **WinKey activation**, causing the Start Menu to open unexpectedly.  
- Intermittent **ghosted keypresses** or repeated inputs that don’t correspond to actual physical key activity.  
- Problems more common in browsers and traditional desktop apps that rely on standard Windows keyboard input.  
- Games that use RawInput or DirectInput bypass the problem entirely, explaining why these glitches do not appear in most modern titles.

### Windows Fast Startup
For older systems with legacy USB/HID controllers and firmware, **Windows Fast Startup should also be disabled during troubleshooting**.
Fast Startup does not perform a completely fresh kernel initialization on every shutdown. After major Windows/security updates, this can occasionally leave legacy USB/HID devices in an inconsistent initialization or enumeration state on older hardware.
If USB devices behave differently after an update, or if the keyboard works correctly after a full power-off but not after a normal shutdown/restart cycle, disable Fast Startup and perform a **true cold boot** before changing drivers or registry settings.
This is particularly relevant when troubleshooting systems where USB enumeration, HID devices, PCI devices, and legacy firmware interact in unusual ways.
**Fast Startup is therefore a troubleshooting precaution for legacy configurations, not a claim that it normally causes keyboard ghosting.**

---

## 2. Understanding "KeyboardDataQueueSize" and the Myth of Latency

The KeyboardDataQueueSize registry flag defines the size of the FIFO buffer used by kbdclass.sys, the Windows keyboard class driver. When left on automatic (default), Windows dynamically sizes the buffer based on outdated assumptions, typically expecting a 125Hz polling rate like legacy PS/2 office keyboards.

On modern mechanical keyboards running at 1000Hz polling rate, this auto-sizing becomes buggy, windows thinks it's a PS/2 keyboard and set a wrong value. The buffer can overflow or drop events, especially under fast or repeated keystrokes, causing ghosting, stuck keys, or phantom modifiers like WinKey triggering on its own.

Despite common gaming folklore, increasing this buffer size does not add latency. It's just a bigger pipe, not a slower one. The myth likely came from gamers misinterpreting responsiveness with overflow delays caused by a buffer too small. Ironically, setting it lower "for gaming" increases the chance of input corruption.

Increasing the buffer (e.g., to a fixed value 128 decimal) ensures headroom without affecting latency or input precision it's a safety margin, not a throttle.

---

## 3. Different Types of Keyboard Input and Why Ghosting Differs in Games

Standard Windows Input (kbdclass.sys + User32 API):
This is the classic keyboard input pipeline used by most desktop applications, browsers, editors, and IDEs. Input events pass through the kernel driver’s buffer and accessibility filters, which can introduce ghosting or stuck keys if misconfigured.

RawInput / DirectInput / XInput (Common in Games):
Many modern games bypass the standard Windows keyboard queue by reading input directly from the hardware device via these APIs. This direct access avoids most software filters, buffering quirks, and accessibility layers, greatly reducing ghosting and input anomalies.

Because of this, the ghosting and input glitches described in this guide mainly affect users who type fast in traditional desktop environments, such as programmers, writers, or anyone working extensively in browsers and editors. Games that use RawInput or similar APIs typically do not exhibit these problems, which explains why some users never see ghosting during gameplay but experience it heavily during fast typing.

---

## 4. The Hidden Culprit: Accessibility Flags That Stay Active (Even When You Think They're Off)

Windows includes several built-in accessibility features like StickyKeys, FilterKeys, BounceKeys, and SlowKeys originally designed to help users with motor impairments. However, these features can remain partially active at the driver level even if they appear disabled in the Control Panel or Settings UI.

One critical registry path is:
HKCU\Control Panel\Accessibility\Keyboard Response
Inside this key:
The Flags DWORD controls whether filtering behavior is enabled.
Even when all individual delay/repeat values are set to 0 (disabled), a non-zero Flags value can silently activate input filtering.
This filtering can interfere with rapid key sequences, sometimes introducing artificial key repeats, phantom key releases, or delayed inputs i.e., exactly the kind of ghosting and glitching issues fast typists encounter.

Why It Happens Silently:
Windows Updates or system repairs can occasionally reset this flag or preserve it across reinstalls without user consent.
No clear UI toggle exists for fully disabling this behavior at the driver level; only registry-level edits work.

Check registry key for hidden accessibility flag :

powershell : 
Get-ItemProperty -Path "HKCU:\Control Panel\Accessibility\Keyboard Response" -Name "Flags"

regedit : 
HKEY_CURRENT_USER\Control Panel\Accessibility\Keyboard Response
Flags keyword must be zero to ensure filtering is truly disabled.

---

## 5. keyboardfilter.sys: The Silent Hardware Filter You (Probably) Don’t Need

keyboardfilter.sys is a Windows driver designed for security lockdown scenarios, most commonly found in:
    Kiosk systems
    POS terminals
    Enterprise-managed environments
    Secure boot configurations
    Windows Defender Application Control (WDAC) lockdown profiles

This driver allows administrators to block or remap specific keys at a very low level, even before the standard kbdclass.sys processes the input.

Why It’s Useless (or Harmful) for Gamers and Developers:
For everyday users, especially gamers, developers, or fast typists, keyboardfilter does nothing beneficial and can introduce:
    Unpredictable key handling behavior
    Dropped or reordered keystrokes
    Conflicts with mechanical keyboards using high polling rates (500Hz–1000Hz)

Moreover, it runs silently in the background if ever enabled it has no user interface, no control panel, and doesn’t always show up in the Device Manager.
It must be manually disabled or removed using "sc delete keyboardfilter" (from cmd with admin privilege)
or via Group Policy / Registry depending on how it was deployed.

Even after removing the service a virtual HID device may still linger in your system, acting as a proxy or filter for your physical keyboard and potentially interfering with normal input : Device Manager / Keyboards / "Keyboard Filter Device" (or similar).

This virtual device doesn’t correspond to a physical port, it’s an abstract layer used by the OS to intercept and manage keystrokes before they reach kbdclass.sys
It can remain silently active even when the service has been deleted.
It may reappear after system updates or on reboot, depending on the policy state.
It contributes to ghosting, input lag, or inexplicable modifier behavior on high-speed mechanical keyboards.

The virtual keyboard filter device can’t be disabled manually from Device Manager, the option is greyed out or blocked due to system protections.
However, you can force-disable it via the registry by targeting the device’s specific instance path.

check for the the properties, read the hardware ID, look it in the registry under : 
"HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\HID"

check for the matching vendor id and add this keyword : 
"ConfigFlags"=dword:00000001
(this will disable the virtual keyboardfilter hardware)

---

## 6. Preventing USB Polling Rate Drifting and Power Management workarounds to reduce input lag

Windows may degrade performance or introduce input delay due to aggressive power management features. To maintain stable performance, it's essential to disable USB selective suspend and related options in both system-wide settings and device-level flags.
(These two workarounds have low to negligible impact on energy saving)

### Disable "USB Selective Suspend" 
Open an elevated Command Prompt or PowerShell and run:
powercfg -attributes SUB_USB 2a737441-1930-4402-8d77-b2bebba308a3 -ATTRIB_HIDE
(This command unhides the USB selective suspend setting under your active power plan.)

Go to Control Panel - Power Options.
Click on your active plan - "Change plan settings" - "Change advanced power settings".
Expand USB settings - USB selective suspend setting.
Set both On battery and Plugged in to Disabled.

This prevents the OS from pausing or lowering polling rates on idle HID/USB devices, which is critical for mechanical keyboards at 1000Hz. 

### Disable “Allow the computer to turn off this device” 
In addition to global settings, Windows can suspend USB input devices individually using a per-device setting. This can lead to erratic behavior like missed keystrokes or polling rate degradation.

Open Device Manager - Expand Universal Serial Bus controllers or Human Interface Devices.
Right-click your keyboard or relevant USB Input Device - Properties.
Go to the Power Management tab (if it doesnt exist, skip).
Uncheck: “Allow the computer to turn off this device to save power”
(Repeat this for every USB device your keyboard might enumerate as (HID-compliant device, composite USB device, etc.)

Disabling these settings ensures constant power and polling stability, preventing Windows from interfering silently with your device's real-time communication.

p.s. you will still be able to suspend your pc, this settings is "ambiguous" and many people believe it causes problems on stand-by)

---

## 7. Other Potential Causes and Considerations

While this guide focuses on core Windows-level causes of ghosting and input issues on high-polling mechanical keyboards, several other factors can contribute to or exacerbate problems:

NKRO (N-Key Rollover) support and layers: Some keyboards use complex firmware layers or emulation modes that may interact oddly with Windows, especially if layers like NKRO or Fn key mappings are not properly handled. Disabling or toggling these layers can affect input reliability.

Multiple HID devices for the same keyboard: Some keyboards expose multiple device interfaces (e.g., a separate HID for media keys, macro buttons, or lighting control). If these additional devices are not disabled or managed correctly, they can cause input conflicts or unexpected behavior.

Third-party software and filters: Software like key loggers, input remappers, antivirus tools, or keyboard customization utilities (e.g., AutoHotkey, gaming suites) may install hooks or filters that interfere with raw input processing, causing latency or ghosting effects.

--- 

## 8. 5KRO & 6KRO vs NKRO: Why "Less" Is Sometimes Better

Most modern mechanical keyboards advertise NKRO (N-Key Rollover), which theoretically allows you to press an unlimited number of keys simultaneously without ghosting or blocking. However, in practice, 6KRO (6Key+1modifier) or even 5KRO (5key+2modifier) modes are often more stable and efficient, especially under Windows.

Why?

USB Bandwidth & Grouping: USB keyboards typically transmit key events in grouped fixed size frame reports. NKRO on USB often relies on tricks like multiple HID interfaces with special protocols which can lead to inconsistencies or even ghosting when improperly handled by drivers or the OS (the higher the polling rate, the more it could happen).

Latency vs Complexity: Simpler rollover modes (5KRO-6KRO) reduce the overhead involved in interpreting input matrices and avoid driver-level hacks. This can improve latency and minimize error in high-polling-rate environments.

Real-World Use Cases: Even in demanding games like StarCraft II, it's rare to press more than 6 keys + modifiers at the exact same time. Unless you're using macros or complex chorded inputs, NKRO is generally overkill.

Fallback Mode Benefits: Many high-end keyboards allow a switch back to 6KRO or 5+2 mode. This fallback can help ensure cleaner input parsing, especially on systems where compatibility, input stability, or fast typing are priorities (e.g., for coders, writers, or competitive gamers avoiding bloated input stacks).

Unless your use case explicitly needs extreme input concurrency, 5KRO/6KRO base mode on NKRO offers a safer, faster, and more consistent user experience, particularly when paired with a high polling rate and disabling the additional filtering layers.

## 9. EXTRA step : Set TextInputHost.exe to High Priority at User Logon

Windows dynamically adjusts CPU scheduling between foreground and background processes, often assigning a much smaller time quantum to background tasks to save energy.
This aggressive prioritization can reduce CPU time for input-related processes like TextInputHost.exe, causing input lag or glitches.
Forcing TextInputHost.exe to run at high priority ensures it receives adequate CPU scheduling time despite these system default optimizations. (alternatively you can check "[melody tweaker](https://github.com/SheMelody/melodys-tweaker/releases)" on github for more info on how to tweak scheduling and ratio's)

---

## 10. Problem still persists? Check the keyboard:

If you've already applied the software tweaks but the issue occasionally returns, the root cause might be **mechanical resonance or switch-level defects**.  
Here’s a checklist of keyboard hardware-related factors to consider:

- **Switch resonance:**  
  Some mechanical switches, especially *unlubricated linear or tactile types*, can resonate like a drum when keys are pressed rapidly.  
  This is more common in switches with **nylon housings** or loose stems. (Hot swap socket loose, pin on switch half broken etc etc)

- **Echo chamber under keys:**  
  Large keys like the spacebar or Winkey may sit above a hollow gap in the PCB or case. This space can amplify internal vibrations and cause unintended signals.  (In this case, its more likely an half-broken metal contact broken inside the switch that still work)
  *Fix: add dampening foam (e.g., Poron, silicone) under the keycap or inside the housing.*

- **Defective or misaligned switches:**  
  Occasionally, a single faulty switch may produce **phantom keypresses** due to a worn contact leaf or irregular stem movement.  
  *Fix: replace with a higher-tolerance or heavier switch (e.g., POM/Nylon mix).*

- **Lack or excess of lubrication:**  
  Dry or drenched switches can create micro-friction or spring recoil artifacts detectable by sensitive firmware.  
  *Fix: switch to pre-lubed silent variants or apply controlled lubing (only if you're experienced).*

- **Keycap fit & acoustic feedback:**  
  Loose or poorly fitted keycaps can amplify vibration or transmit it across the plate.  
  *Fix: use keycaps with snug cross-stem fitting or add dampening o-rings if needed.*

- **Actuation weight matters:**  
  Avoid ultra-light actuation switches on high-risk keys (like Winkey or spacebar).  
  *Heavier switches offer better mechanical isolation and reduce false activations due to micro-oscillations.*

---

## 11. Problem still persists? Check your hardware (peripherals and more):

- **Bridged PCI/PCIe Sound Cards**
Creative Sound Blaster (especially X-Fi, Audigy with PCI bridging or legacy emulation)
Some ASUS Essence or other PCIe cards with multiple controllers that interfere with shared IRQs
Typical issue: drivers holding IRQ lines even when disabled → unstable NKRO/6KRO, key jitter

- **External USB Controllers or PCI USB Hubs**
Older PCIe USB 3.0 hubs or non-certified hubs → selective suspend and polling issues
USB expansion cards with VIA/NEC/Fresco Logic chips that poorly handle the host controller
Symptom: mechanical keyboards miss keystrokes during fast combos or intensive macros

- **Graphics Cards with Aggressive Passthrough or Overlay Software**
Older AMD/NVIDIA GPUs using DirectInput/RawInput hooks (e.g., Radeon Overlay, RivaTuner, Steam Overlay)
Problem: input can conflict with app-level reading → ghosting or dropped keys in some apps

- **PCIe Capture/Acquisition Cards**
Elgato HD60/HD60 Pro, some Blackmagic Decklink cards
High-polling drivers or aggressive IRQ handling may “steal” interrupts from HID devices

- **Legacy PS/2 Controllers & Hybrid Keyboards**
PS/2 keyboards with USB adapters → some Intel ICH/AMD southbridge controllers mishandle rollover above 6KRO
Symptom: lost key combinations, ghosting, or “jumping” input

- **Virtual Input-Handling Devices or Gaming Software**
Keyboards and gaming peripherals using Razer Synapse, Logitech Gaming Software, Corsair iCUE
Virtual drivers intercepting input may cause conflicts in non-standard applications

- **Legacy PCIe Network Cards (Wi-Fi / LAN)**
Older Intel/Realtek NICs or beta drivers generating interrupt storms under load
Symptom: high IRQ load → dropped keystrokes on high-polling mechanical keyboards

> **Note:** Any PCI/PCIe device generating high interrupts or using shared buses can interfere with high-priority input devices. The X-Fi (emu20k2) is the most well-known case up to date expecially under win10 22h2 and upper (completely broken drivers), but these other hardware types can cause similar issues.
> These hardware tweaks, combined with software-side priority adjustments and input filtering, can dramatically improve stability on high-polling-rate keyboards.

--- 


## 12. Problem still persists? try Windows FilterKeys to reduce mechanical switch hattering:

Mechanical keyboard switches use physical electrical contacts. During actuation or release, these contacts may temporarily oscillate between open and closed states instead of producing one clean transition. This phenomenon is known as **contact bounce** or **switch chatter**.

A single physical keypress can therefore produce multiple electrical transitions:

```text
Ideal:
KEY DOWN ---------------------- KEY UP

Contact chatter:
KEY DOWN -> UP -> DOWN -> UP -> DOWN -> KEY UP
```

The duration and intensity of contact bounce can vary depending on the switch design, manufacturing tolerances, wear, actuation speed and pressing technique.

Normally, the keyboard's microcontroller and firmware should debounce the switch matrix before transmitting stable HID reports to Windows. If the firmware does not adequately suppress these transitions, Windows may receive multiple key events corresponding to a single physical action.

### 12.1 Using Windows FilterKeys as a software mitigation

Windows provides an accessibility feature called **FilterKeys**, which includes three relevant functions:

- **SlowKeys:** delays acceptance of a keypress.
- **RepeatKeys:** controls the initial repeat delay and subsequent key-repeat interval.
- **BounceKeys:** rejects a subsequent press of the same key if it occurs too soon after the previous release.

BounceKeys can be useful as a software-level mitigation when repeated press/release transitions are occurring within a very short interval.

For example, with a 32 ms BounceKeys interval:

```text
KEY DOWN       Accepted
KEY UP         Accepted
KEY DOWN       Rejected (inside 32 ms window)
KEY UP
KEY DOWN       Accepted (after the interval)
```

This can suppress certain rapid repeated transitions before they become additional accepted keystrokes at the Windows accessibility layer.

**Important:** BounceKeys is not an electrical debounce circuit and does not modify the keyboard firmware. It cannot clean raw electrical signals, change USB polling behaviour, prevent HID/KBDCLASS queue overflows, or guarantee that an isolated spurious key-down event will be rejected.

Its effectiveness depends on the actual sequence and timing of the events received by Windows.

### 12.2 RepeatKeys and BounceKeys are alternative operating modes

The Windows `FILTERKEYS` structure exposes the following parameters:

| Parameter | Description |
|---|---|
| `iWaitMSec` | Minimum key-hold duration before acceptance (SlowKeys) |
| `iDelayMSec` | Time before the first automatic key repetition |
| `iRepeatMSec` | Interval between subsequent repetitions |
| `iBounceMSec` | Minimum interval after release before another press of the same key is accepted |

RepeatKeys and BounceKeys use different configurations and cannot be combined through the documented `FILTERKEYS` structure.

RepeatKeys is intended for users who want customized key-repeat behaviour while holding a key. BounceKeys is intended for suppressing rapid repeated presses of the same key.

The supplied configurator allows the user to switch between these modes without manually editing registry values.

### 12.3 Included configuration utility

Two files are provided:

```text
FilterKeys.cmd
Set-FilterKeys.ps1
```

Keep both files in the same directory.

Run `FilterKeys.cmd` from the Windows desktop session. Administrative privileges are normally unnecessary because the configuration applies to the current user.

The CMD interface provides the following options:

| Option | Description |
|---|---|
| Windows Standard | Disables FilterKeys and returns to normal Windows keyboard-repeat handling |
| RepeatKeys | Configures key-repeat timing |
| BounceKeys | Enables the software anti-bounce interval |
| Read Current Configuration | Displays the actual runtime FilterKeys configuration |
| Exit | Exits without changing the current configuration |

The CMD file provides editable defaults for each mode.

**RepeatKeys example:**

```text
iWaitMSec   = 0 ms
iDelayMSec  = 250 ms
iRepeatMSec = 50 ms
iBounceMSec = 0 ms
```

This configuration accepts keypresses immediately, starts automatic repetition after 250 ms, and repeats every 50 ms.

The supplied implementation allows `iWaitMSec = 0` and verifies the resulting runtime values through `SPI_GETFILTERKEYS`. Windows documentation describes stricter nonzero requirements for some combinations; therefore, the actual runtime read-back is authoritative for verifying whether the requested configuration was retained on a particular system.

**BounceKeys example:**

```text
iWaitMSec   = 0 ms
iDelayMSec  = 0 ms
iRepeatMSec = 0 ms
iBounceMSec = 32 ms
```

This disables RepeatKeys and SlowKeys and applies a 32 ms interval for subsequent presses of the same key.

The interval can be customized directly in the CMD interface, without modifying the PowerShell implementation.

### 12.4 Audible feedback for input diagnostics

The utility preserves the existing `FKF_CLICKON` flag when changing FilterKeys parameters.

When enabled, Windows produces an audible click when a key is pressed or accepted.

This feedback can be useful during troubleshooting:

- A single click during a short physical tap is the expected behaviour.
- Multiple closely spaced clicks during one short physical action may indicate multiple key events being processed.
- Additional clicks during a sustained press may simply correspond to normal RepeatKeys activity.

For diagnostic testing, setting `iWaitMSec = 0` avoids the additional SlowKeys acceptance stage and its associated second click.

**Audible feedback is an indication of Windows-level key processing, not a direct measurement of the switch's electrical contacts.** Multiple clicks alone do not prove that a physical switch is defective.

### 12.5 Recommended troubleshooting procedure

1. Enable FilterKeys and audible feedback.
2. Start with RepeatKeys or BounceKeys according to the desired operating mode.
3. Test individual keys using both fast and slow actuation.
4. Compare the audible feedback between different switches.
5. Pay particular attention to switches that produce multiple clicks during a single short press.
6. If an unintended modifier combination occurs, correlate the event with the observed key behaviour.

If BounceKeys reduces the occurrence of repeated inputs, this suggests that the symptom is sensitive to rapid repeated key transitions. It does not independently establish whether the original source is the mechanical switch, keyboard firmware, USB receiver or Windows input stack.

For a definitive hardware-level correction, proper switch debouncing should take place in the keyboard's own electronics or firmware, before HID reports are transmitted to the host.

### 12.6 Technical references

- [Microsoft Learn – FILTERKEYS structure](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-filterkeys)
- [Microsoft Learn – SystemParametersInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfow)
- [QMK Firmware – Contact bounce and debounce algorithms](https://docs.qmk.fm/feature_debounce_type)





