# Changelog

## 0.0.2 — 2026-09-20

Combined build: contains everything from `0.0.11` plus standalone-ICD mode.
This is the version to import into an adrenotools host.

### Added

- **Standalone-ICD mode.** When a host `dlopen`s the packaged `.so` and uses it
  as the Vulkan driver (adrenotools hosts such as ARMSX3, Yuzu-likes), the shim
  no longer requires `VK_LAYER_LINK_INFO`. It opens the driver it wraps itself
  and forwards every call. Loader-layer chaining is unchanged when a loader
  supplies the link info.
- `MX_UNDERLYING_DRIVER` env knob: absolute path of the driver to wrap
  (overrides auto-discovery: `libvulkan.so`, then `vulkan.samsung.so` by soname
  and under `/vendor/lib64/hw/` + `/vendor/lib/hw/`).
- Exports the loader/ICD interface symbols
  (`vk_icdNegotiateLoaderICDInterfaceVersion`, `vk_icdGetInstanceProcAddr`,
  `vk_icdGetPhysicalDeviceProcAddr`), so a Vulkan loader can also use the `.so`
  as an ICD.
- `vkCreateInstance` result and the wrapped driver's path are logged to logcat
  (tag `MultiExynoos`) in both modes.

### Fixed

- **v0.0.1 regression.** v0.0.2 made `vkGetInstanceProcAddr(VK_NULL_HANDLE,
  "vkEnumerateInstance*")` self-load `libvulkan.so` and answer with the system
  loader's functions even when a Vulkan loader had negotiated the layer
  interface with us. v0.0.1 returned `nullptr` there so the host loader
  answered and only its graphics stack was in play; the v0.0.2 behaviour
  initialised the system stack underneath chained sessions and black-screened
  the boot pipeline before any stage logged. Layer-negotiated sessions now get
  the v0.0.1 `nullptr` again; direct-dlopen/ICD hosts still get forwarded
  answers. See `docs/REGRESSION-0.0.1-vs-0.0.2.md`.
- Standalone candidates must resolve `vkCreateInstance` before being accepted,
  so a loader-shaped but instance-incapable `libvulkan.so` no longer blocks
  trying the vendor ICD.
- ARMSX3 permanent black screen with nothing in the log when this package was
  selected as a custom driver. `vkCreateInstance` returned
  `VK_ERROR_INITIALIZATION_FAILED` because an adrenotools host never supplies
  `VK_LAYER_LINK_INFO`; ARMSX3's Vulkan instance therefore never came up, its
  RSX thread died in `on_init_thread()` before `Emu.RunPPU()`, and the whole
  boot pipeline stayed silent (no shader/pipeline compilation, no PPU module
  load, no SPU cache, no progress overlay). The instance now comes up, so the
  boot pipeline runs.

### Changed

- Package `name` is now `MultiExynoos` (was `MultiExynoos Layer`); layer
  manifest `implementation_version` bumped to `2`.
- `layerLibrary` restored in `meta.json` (present in v0.0.1, dropped in the
  first 0.0.2 build) so hosts keying on it regain the layer-package signal.
- `libdl` is linked explicitly (`${CMAKE_DL_LIBS}` / `-ldl`) for the
  self-loading path.

### Carried over from 0.0.11

- `libraryName` in `meta.json` names the packaged `.so`
  (`libVkLayer_MultiExynoos.so`); ARMSX3's import validator rejected the
  package when it named the wrapped vendor driver instead.

## 0.0.1

- First release: Phase-1 passthrough loader layer, packaging, CI blueprint,
  roadmap.
