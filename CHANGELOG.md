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
- `libdl` is linked explicitly (`${CMAKE_DL_LIBS}` / `-ldl`) for the
  self-loading path.

### Carried over from 0.0.11

- `libraryName` in `meta.json` names the packaged `.so`
  (`libVkLayer_MultiExynoos.so`); ARMSX3's import validator rejected the
  package when it named the wrapped vendor driver instead.

## 0.0.1

- First release: Phase-1 passthrough loader layer, packaging, CI blueprint,
  roadmap.
