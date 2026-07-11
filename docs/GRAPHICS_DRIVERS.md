# Android graphics-driver findings

Research date: 2026-07-11.

## Adreno 710 / Vauzi-17

The APK bundles the unmodified driver binary from Vauzi-17/710 release 2.7 as
`vulkan.vauzi710_v2_7.so`. The upstream release describes the build as
experimental Mesa 26.2.0 / Vulkan 1.4.354, focused on Adreno 710, 720 and 722,
and recommends `TU_DEBUG=sysmem`. The published ZIP SHA-256 was verified before
the binary was copied into the project.

The driver is a separate explicit choice. Auto remains on the project's
universal Turnip build so existing devices do not silently change driver. When
the Vauzi choice and Render Mode Auto are combined, the app applies Sysmem;
users can still force GMEM for comparison.

## Samsung Xclipse / ExynosTools

ExynosTools 2.1 ships different packages for Exynos 2200, 2400 and 2600, plus a
native variant. A checked Exynos 2400 Vortek package contains:

- `vulkan.xclipse24.3.9.so` as `meta.json`'s `libraryName`;
- `libVkLayer_VortekXclipse.so` and its layer manifest;
- `libnramv.so`.

The current ZIP importer safely extracts those files and can select the main
ICD. Full Vortek compatibility is not yet integrated, however: the game opens a
custom ICD directly through libadrenotools, while ExynosTools' BCn and quirk
logic lives in a Vulkan layer that must participate in the instance/device call
chain. Merely packaging the ZIP does not prove that layer is active.

A built-in Samsung choice should therefore wait for all of the following:

1. an explicit-layer loading path compatible with the direct custom ICD;
2. SoC detection and selection among the 2200/2400/2600 packages;
3. confirmation that redistribution of the packaged Samsung backend is allowed;
4. real-device testing on each supported Xclipse generation.

Until then, the stock system Vulkan driver is the safe Auto choice on Xclipse.
ZIP import remains available for experiments, but should not be described as
full ExynosTools layer support.

## Mesa PanVK on Android

Mesa can now compile PanVK with Android WSI/AHardwareBuffer support, and current
PanVK exposes Vulkan 1.4 on supported Mali generations. That is only the
userspace half of the stack. PanVK communicates with the upstream Linux
`panfrost` or `panthor` DRM kernel uAPI; normal Android phones ship an Arm/vendor
kernel driver instead. Unlike Turnip's Android KGSL path, PanVK cannot use that
proprietary kernel interface through libadrenotools.

Consequences:

- building an Android ARM64 `libvulkan_panvk.so` is feasible;
- loading it on an ordinary stock Mali phone is not expected to enumerate a GPU;
- it becomes useful only on a device/ROM kernel exposing the matching panfrost
  or panthor DRM render node, with Android buffer-sharing support wired up;
- the game's system-driver path is currently more practical for recent Mali
  devices whose vendor driver provides the required Vulkan features.

For a development build, Mesa's relevant shape is `platforms=android`,
`vulkan-drivers=panfrost`, and an Android ARM64 cross file. Producing and
shipping that binary in the APK is intentionally deferred until a compatible
Android kernel target exists; otherwise it adds size without helping stock
devices.

## Sysmem on Adreno 6xx

Turnip supports two render-pass paths: tiled on-chip GMEM and direct rendering
to system memory. `TU_DEBUG=sysmem` forces the latter. Reports that Sysmem helps
Adreno 6xx are plausible for workload- or driver-specific GMEM bugs and the
option is useful for diagnosing corruption, hangs and performance cliffs.

It should not be forced for every Adreno 6xx device: Mesa normally selects
between GMEM and Sysmem using its autotuner, and forcing either path can regress
other scenes. The launcher therefore exposes Sysmem as an explicit Render Mode;
only the dedicated Vauzi 710 choice maps Auto to Sysmem because that upstream
release specifically recommends it.
