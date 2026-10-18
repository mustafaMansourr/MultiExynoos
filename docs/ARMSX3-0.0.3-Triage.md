# ARMSX3 black screen on Demon's Souls (BLES00932): triage, fix, and validation plan (0.0.3)

Report driving these notes:

- v0.0.1 boots Demon's Souls in ARMSX3 with the MultiExynoos custom driver
  selected; a Switch emulator and Winlator work on every build.
- v0.0.2 (package `local-multiexynoos-layer-0.0.2_g0c4fffb`) black-screens the
  same title in ARMSX3 with the MultiExynoos driver selected; the bundled
  driver (`vulkan.samsung.so`) boots the title on the same device and build.

Evidence base: one full 40 MB logcat capture from the failing run
(`log_emulator.txt`, pid 2961, com.armsx3 "Release 1.2.1 (versionCode 91)",
game BLES00932, Samsung erd8855 / Xclipse 540 / Vulkan 1.3.279), the AOSP
sources `frameworks/native/vulkan/libvulkan/driver.cpp` (main) and
`hardware/libhardware` headers `hardware.h` / `hwvulkan.h`, and the ARMSX3
RPCS3 sources (`VKGSRender.cpp`, `vk_android_loader.cpp`). The custom-driver
hook source lives on a feature branch and could not be fetched; every hook
behaviour below is proven from the log, not from its source.

## 1. Log triage - the death window (pid 2961)

| Line | Tag | Content (abridged) | Reading |
| --- | --- | --- | --- |
| 247273 | `I/ARMSX3-Driver` | `custom Vulkan driver applied: libVkLayer_MultiExynoos.so` | ARMSX3 bound our .so as its custom driver. |
| 247826 | hook | `hook_android_load_sphal_library: filename: vulkan.samsung.so` | The system Vulkan loader asked sphal for the vendor driver by soname. |
| 247827 | hook | `hook_android_dlopen_ext: filename: vulkan.samsung.so` | The same load attempt seen through the dlopen_ext hook. |
| 247828 | hook | `loading custom driver: .../libVkLayer_MultiExynoos.so` | The hook substituted OUR .so for `vulkan.samsung.so` inside the system loader's HAL path. |
| 247829 | `E/vulkan` | `couldn't find symbol 'HMI' in HAL library` | AOSP loader `LoadDriver()` (`driver.cpp:190-194`): `dlsym(so, "HMI")` failed, library dlclose'd, -EINVAL. |
| 247832 | `F/ARMSX3` | `No compatible GPU devices found` | `VKGSRender.cpp:445` fatal: `vkEnumeratePhysicalDevices` returned zero devices (stub HAL took over). |

Decisive negative evidence: **zero** log lines with tag `MultiExynoos` in the
entire capture. None of our exported functions ran before the fatal. The
process died inside the system loader's HMI check, before any Vulkan instance
existed, which is why nothing downstream (shader compile, pipeline cache, PPU
module load, SPU cache) ever logged.

Also present: ~131 `E/vulkan` "failed to dlopen" lines before the fatal -
ARMSX3's `resolve_all` probing the system loader with null-handle queries.
Benign; the same spam appears on the working bundled-driver boot. `V/XGL`
lines at process start suggest the real vendor driver may already have been
mapped into the process before the hook fired. The double slash in
`..._g0c4fffb//libVkLayer_MultiExynoos.so` is cosmetic.

## 2. ARMSX3 Vulkan boot stage map

| Stage | Who | Must succeed | v0.0.2 + MultiExynoos |
| --- | --- | --- | --- |
| S0 process start | ARMSX3 | link, resolve libvulkan | OK |
| S1 driver bind | rpcs3 loader | dlopen custom driver .so, resolve entry points | OK (line 247273) |
| S2 instance create | rpcs3 via system loader | vkCreateInstance -> loader `Hal::Open` -> `LoadDriver` | **DEAD**: hook substituted our .so for `vulkan.samsung.so` |
| S3 HAL load | AOSP loader | `dlsym(HMI)`, `id == "vulkan"`, `methods->open("vk0")` | **DEAD**: no `HMI` export -> stub HAL, zero devices |
| S4 enumerate GPUs | loader -> RSX | `vkEnumeratePhysicalDevices` >= 1 | **DEAD**: zero devices -> `VKGSRender.cpp:445` fatal |
| S5 device create | RSX | vkCreateDevice, queues | not reached |
| S6 shaders/pipelines | RSX | SPIR-V compile, pipeline cache | not reached (black screen, no log) |
| S7 PPU/SPU | Emu | PPU module load, SPU cache | not reached |

Working bundled-driver path: S2 loads `vulkan.samsung.so` (which exports
`HMI`) -> S3 `open("vk0")` -> real device -> S4..S7 proceed and log normally.

## 3. Why only ARMSX3 (the divergence)

- Eden (Switch emulation) and Winlator `dlopen` our .so directly and call
  `vkGetInstanceProcAddr`. No HAL/HMI contract is involved; our
  standalone-ICD mode (added in v0.0.2) serves them, which is why they work.
- ARMSX3's custom-driver hook intercepts the *system* Vulkan loader's sphal /
  dlopen_ext load of `vulkan.samsung.so` and substitutes our .so. That places
  us under the Android hwvulkan HAL ABI, which v0.0.2 did not implement.
- The bundled `vulkan.samsung.so` exports `HMI`, so the identical hook path
  succeeds when the bundled driver is selected - hence driver-select-only,
  title-specific failure.

## 4. Ranked suspects (triage order)

1. **Our .so lacks the Android Vulkan HAL `HMI` export.** PROVEN by log line
   247829 plus the AOSP `LoadDriver()` source. Fixed in this release.
2. Hook substituting the wrong file (substitution itself is correct - the
   bundled driver boots through the same hook).
3. Instance-extension mismatch (moot: the loader never got past `HMI`).
4. Layer manifest / `VkLayer_MultiExynoos.json` mismatch (ARMSX3 uses the .so
   directly as a driver, not the manifest).
5. `VK_LAYER_LINK_INFO` chain break (ARMSX3 does not chain layers; no link
   info exists on this path).
6. Loader/ICD version negotiation failure (never reached).
7. Xclipse-specific vendor quirk (ruled out: bundled driver works on the same
   GPU, same loader, same title).

## 5. Diagnostic bundle (apples-to-apples)

Run each command twice on the device - once with the MultiExynoos driver
selected, once with the bundled driver - and diff the outputs.

Paired log capture:

```
adb logcat -c
adb logcat -v threadtime > pair-multidriver.txt   # while booting BLES00932
# ...repeat with the bundled driver selected -> pair-bundled.txt

# Focused filters (death-window tags):
adb logcat -v threadtime -s MultiExynoos:V ARMSX3:V ARMSX3-Driver:V vulkan:E
```

Device facts (identical across the pair by definition, recorded to prove it):

```
adb shell getprop ro.board.platform          # erd8855
adb shell getprop ro.hardware.vulkan         # samsung -> loader asks for vulkan.samsung.so
adb shell getprop ro.hardware
adb shell getprop ro.product.model
adb shell getprop ro.build.version.release
adb shell dumpsys gpu                        # Vulkan device/driver report
adb shell cmd gpu vkjson                     # full Vulkan capabilities JSON <uncertain: present from Android 12>
```

Validation layers (optional, needs the Khronos validation layer APK installed;
the settings keys below are the documented mechanism but were not verified on
this device - `<uncertain>`):

```
adb shell settings put global enable_gpu_debug_layers 1
adb shell settings put global gpu_debug_app com.armsx3
adb shell settings put global gpu_debug_layers VK_LAYER_KHRONOS_validation
```

Loader debug: `VK_LOADER_DEBUG` is a desktop-loader environment knob; the
Android system loader has no equivalent environment variable - its diagnostics
are the `E/vulkan` logcat lines plus `setprop debug.vulkan.layers <layer>` for
layer enumeration. GAPID/RenderDoc capture requires a debuggable APK or root;
the release ARMSX3 build is not debuggable, so GPU trace capture is only
possible on a debug build of the emulator - and only after the instance exists
(it never did in v0.0.2). Bundled-driver revert is the always-available
baseline: select the bundled driver and confirm boot, isolating our package as
the only variable.

## 6. Smallest fix first

Export the `HMI` hardware-module symbol the AOSP loader requires, implementing
the hwvulkan ABI as a pure forwarder to the vendor ICD (see
`layer/src/layer.cpp`, section "Android Vulkan HAL (hwvulkan) contract"). No
rendering behaviour changes: the same vendor driver creates the same instance
with the same dispatch magic (`HWVULKAN_DISPATCH_MAGIC = 0x01CDC0DE`, which
matches the loader ICD magic by AOSP design).

## 7. Gated driver-side improvements

Required for ARMSX3 (shipped in 0.0.3):

- `HMI` export + hwvulkan ABI structs (local, NDK ships neither header).
- HAL entry points forwarding to the vendor's own HMI device, with
  gipa/dlsym fallback.
- Recursion guards: HAL-mode underlying ladder never includes `libvulkan.so`
  (it is the caller above us); self-load guard via `dladdr` base comparison on
  every candidate, also added to the standalone ladder.
- Named error on failure: if no vendor ICD is reachable, `open()` returns
  nonzero with a `MultiExynoos` log line; the loader reports "failed to open
  Vulkan HAL device" and ARMSX3 shows its own fatal. No silent stub success.

Nice-to-have (additive, gated, shipped):

- Property-derived driver sonames (`ro.hardware.vulkan`, `ro.board.platform`)
  mirroring the loader's own resolution.
- `/proc/self/maps` already-mapped candidate (unhookable path form).
- `android_load_sphal_library` candidate via `dlsym(RTLD_DEFAULT)`.
- One-time mode banner naming entry mode and wrapped driver path.
- Per-candidate failure tracing in both ladders.

## 8. Fix patch list (0.0.3 commits)

1. `docs: ARMSX3 black-screen triage - boot map, divergence, suspects, diagnostics`
   -> this file.
2. `fix: export Android Vulkan HAL (HMI) and forward to the vendor driver`
   -> `layer/src/layer.cpp`: hwvulkan ABI structs, `HMI`, `mx_hal_open` and
   the three HAL forwarders, HAL-mode underlying ladder, recursion/self-load
   guards, `g_entered_as_hal` state, reuse of the HAL vendor ICD by the
   standalone ladder in the same process.
3. `logging: one-time mode banner naming entry mode and driver path`
   -> `layer/src/layer.cpp`.
4. `chore: bump version to 0.0.3` -> `layer/CMakeLists.txt`, four script
   defaults, `packaging/meta.json`, layer fallback version string,
   `CHANGELOG.md`, `dist/RELEASE_NOTES_v0.0.3.md`.

## 9. Regression matrix (verified before release where marked, on-device where marked)

| Case | Expectation | Status |
| ARMSX3 + MultiExynoos driver, BLES00932 | `HMI` found -> HAL open -> vendor ICD -> GPUs enumerated -> shaders/pipelines/PPU/SPU log normally | on-device required |
| ARMSX3 + bundled driver, BLES00932 | unchanged boot (protected baseline) | on-device required |
| ARMSX3 + intentionally incompatible driver package | clear on-screen error, no silent fallback (protected baseline) | on-device required |
| Switch emulator (Eden) title | unchanged boot; standalone path untouched apart from self-load guard | on-device required |
| Winlator app | unchanged behaviour | on-device required |
| Loader-layer chaining (v0.0.1 parity path) | pre-instance queries still left to the host loader; unchanged | verified in code |
| Exported symbols | `HMI` present as writable data; 6 VK exports intact | verified via llvm-nm |
| DT_NEEDED | unchanged (liblog, libm, libdl, libc); no new deps | verified via readelf |
| Recursion safety | HAL ladder never dlopens `libvulkan.so`; self-load guard rejects hook-bounced candidates | verified in code |

Post-fix on-device verification pattern: logcat tag `MultiExynoos` shows the
one-time banner (HAL mode + vendor path) followed by `HAL vkCreateInstance ->
0`; the `E/vulkan: couldn't find symbol 'HMI'` line is absent; ARMSX3 proceeds
to shader/pipeline/PPU/SPU stage lines. If every ladder candidate fails,
expect our named error plus the loader's "failed to open Vulkan HAL device"
plus the ARMSX3 line-431 fatal - and `MX_UNDERLYING_DRIVER=<absolute vendor
path>` is the documented escape hatch.
