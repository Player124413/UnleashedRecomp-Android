# Unleashed Recompiled for Android

Play the Xbox 360 version of *Sonic Unleashed* natively on a supported Android device.

This is an unofficial Android port of [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp). It runs the game through static recompilation rather than emulating an Xbox 360, and includes an Android app, touch controls, gamepad support, a mod manager, and a custom Vulkan driver for many Qualcomm Adreno GPUs.

> [!IMPORTANT]
> This project does not include the game. You must supply files from your own legally acquired copy of *Sonic Unleashed* for Xbox 360.

> [!WARNING]
> This is still an experimental community port. Performance and stability vary by phone, Android version, and GPU driver. Keep a copy of your saves and game files.

## What works

- The full game, including title screens and regular gameplay
- ARM64 Android devices
- On-screen touch controls with multi-touch
- Bluetooth and USB controllers
- Sound through speakers, wired headphones, and Bluetooth devices
- HMM and UMM-style mods through the included **Unleashed Mods** app
- Built-in Turnip driver plus importing another compatible Turnip `.so`
- Game-file access through Android's system Files app
- Logs and hang diagnostics that can be shared without `adb`

The main development targets are Adreno 710, 725, 732, and 750. Adreno 720 and 722 are included in the bundled driver but need more testing. Other Adreno 6xx/7xx devices may work, but are not guaranteed.

**Mali support is experimental.** Recent Mali GPUs (Valhall generation and newer with a Vulkan 1.3 driver — e.g. G610/G615/G710/G715/G720) run the game through the stock system driver: the app detects a non-Adreno GPU automatically and skips the bundled Adreno driver. Textures are decompressed on the CPU when the driver lacks BC format support, which increases memory usage. Confirmed working on a Dimensity 8300 Ultra (Mali-G615). Older Mali generations (Bifrost and earlier) cannot work.

PowerVR and Samsung Xclipse are untested. On Xclipse devices the system driver is used; community compatibility packages exist at [ExynosTools](https://github.com/WearyConcern1165/ExynosTools) but cannot be imported into this app yet.

## Before you install

You need:

- A 64-bit Android device
- Android 9 or newer
- A supported Qualcomm Adreno GPU (or a recent Mali GPU — experimental)
- Several gigabytes of free storage
- Your own Xbox 360 game dump

For the smoothest first run, start with the default graphics settings. The Android build defaults to a 50% resolution scale, no anti-aliasing, 4× anisotropic filtering, and motion blur disabled.

## Installation

1. Download the latest APK from the repository's [Releases](https://github.com/SansNope/UnleashedRecomp-Android/releases) page.
2. Allow your browser or file manager to install apps from unknown sources when Android asks.
3. Install and launch **UnleashedRecomp** once. The first launch creates the app's folders and prepares the bundled graphics driver.
4. If the game reports that its files are missing, open Android's Files app and choose **Unleashed Recomp game files**.
5. Copy the `game`, `update`, `patched`, and optional `dlc` folders from your legal dump into the folder shown by the app.
6. Close and reopen the game.

Do not use `adb push` directly into `Android/data`. Files created there by the shell can receive ownership that prevents the app from reading them. Use the system Files interface exposed by the app instead.

## Controls

The touch controller appears automatically when you touch the screen. It provides:

- Left analog stick
- A, B, X, and Y
- LB and RB
- LT and RT
- Start and Back

When a physical controller sends input, the touch overlay hides itself. Touch the screen again to bring it back.

USB and Bluetooth controllers supported by SDL should work without additional setup. In-game prompts may not match every third-party controller exactly.

## Installing mods

The APK installs a second launcher entry named **Unleashed Mods**.

1. Open **Unleashed Mods**.
2. Tap **Open Files** and create or open the `mods` folder.
3. Copy each mod into its own folder. A compatible mod contains a `mod.ini` file.
4. Return to the mod manager and tap **Refresh**.
5. Enable the mods you want and arrange their priority.
6. Tap **Save mod list**.
7. Restart the game.

The manager writes standard `cpkredir.ini` and `ModsDB.ini` files. Relative paths from desktop mod packs are also supported, so most HMM/UMM layouts can be copied without manually rewriting every path.

If a mod does not appear, check that `mod.ini` is not buried inside an extra nested folder. Desktop-only code mods or mods that depend on Windows DLLs will not work on Android.

## Graphics drivers

The app includes a community-built Mesa Turnip driver tuned for the Adreno devices listed above. It is selected automatically on a fresh installation.

You can choose the driver and render mode from the game's options menu. If you want to try another driver:

1. Open the app's transfer folder in Android Files.
2. Copy one compatible ARM64 Turnip `.so` into `driver_import`.
3. Start the game and select the imported driver.

Only import drivers from a source you trust. A bad or incompatible Vulkan driver can cause graphical corruption, freezes, or startup crashes. The built-in recovery path lets you return to the bundled driver if an imported one fails.

### Device notes

- **Adreno 710 / 725:** use the bundled driver. It includes a synchronization fix for the one-frame shimmer seen on early a7xx hardware.
- **Adreno 732:** supported through a community device profile based on the closely related Adreno 735.
- **Adreno 750:** disable MSAA if you see corruption. The known issue is in the Turnip MSAA path; the default Android settings already leave anti-aliasing off.
- **Adreno 720 / 722:** driver entries are included, but real-device feedback is still needed.

## Troubleshooting

### The game cannot find my files

Make sure the selected directory contains `game`, `update`, and, if available, `dlc`. Use the Files location exposed by **UnleashedRecomp**, not an arbitrary folder with the same name.

### The game opens to a black screen or corrupted graphics

- Return to the bundled driver.
- Disable anti-aliasing/MSAA.
- Keep the resolution scale at 50% while testing.
- Restart the app after changing the driver.

### Sound crackles or stops after using Bluetooth

Pause for a moment after connecting or disconnecting the device, then return to the game. If audio does not recover, restart the app and attach the latest log when reporting the issue.

### The app freezes

Close it normally if possible, reopen the app's transfer folder, and retrieve:

- `log.txt` — the latest run
- `log_prev.txt` — the previous run

The logger includes a hang watchdog, so `log.txt` may contain thread-state information even when no debugger was connected.

When reporting a problem, include your phone model, SoC/GPU, Android version, selected driver, render mode, and the exact point where the problem occurred.

## For developers

Building the port requires Windows, Visual Studio 2022 Build Tools, CMake, Ninja, Android SDK/NDK r29, JDK 17, and vcpkg. Host recompilation tools must be built for Windows before cross-compiling the Android ARM64 target.

The repository contains helper scripts, but some still contain machine-specific paths and should be reviewed before use. A checkout path without spaces is strongly recommended.

No generated or copyrighted game code may be committed. In particular, keep these local:

- `UnleashedRecompLib/private/`
- `UnleashedRecompLib/ppc/`

The Android-specific implementation lives primarily in:

- `UnleashedRecomp/os/android/`
- `android-apk/`
- `thirdparty/libadrenotools/`
- `thirdparty/plume/`
- `thirdparty/SDL/`
- `UnleashedRecomp/ui/touch_controls.*`

## Legal

This repository contains no *Sonic Unleashed* game assets. It is not affiliated with or endorsed by SEGA, Microsoft, the upstream Unleashed Recompiled team, Mesa, or Qualcomm.

The project is distributed under GPL-3.0, following the upstream project. Third-party components retain their own licenses.

## Credits

This port exists because many people shared code, testing time, traces, hardware access, and patient debugging:

- [hedge-dev and the Unleashed Recompiled contributors](https://github.com/hedge-dev/UnleashedRecomp) — the original static recompilation project
- [SansNope](https://github.com/SansNope) — Android port stewardship, builds, Turnip integration, and the public home of this fork
- [ITSeniy](https://github.com/ITSeniy) — lifecycle and audio stabilization, Vulkan recovery, touch controls, Android file access, driver management, and the in-app mod workflow
- [GdGohan](https://github.com/GdGohan) — ModLoader compatibility work and Android build contributions
- [renderbag/plume](https://github.com/renderbag/plume) — the Vulkan renderer used by Unleashed Recompiled
- Mesa's Freedreno/Turnip developers — the open-source Vulkan driver that makes the port practical on Adreno hardware
- [bylaws/libadrenotools](https://github.com/bylaws/libadrenotools) and the Android driver community — runtime custom-driver loading
- [Vauzi-17/710](https://github.com/Vauzi-17/710) and whitebelyash/AdrenoToolsDrivers — community Adreno 710/720/722 device information
- Everyone who tested unfinished builds, captured logs, reproduced GPU bugs, and reported what happened
