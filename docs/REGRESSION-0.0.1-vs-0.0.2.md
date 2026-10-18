# Regression notes: v0.0.1 (known-good) vs v0.0.2

Every row below was verified by unpacking the shipped zips
(`MultiExynoos-layer-0.0.1_g33fe876.zip`,
`MultiExynoos-layer-0.0.2_g6a78732.zip`) and by `git show` of both tags.
Nothing here is inferred from memory.

## Report driving these notes

- v0.0.1 boots Demon's Souls in ARMSX3 with the custom driver selected and
  also works in a Switch emulator.
- v0.0.2 black-screens the same setup: no shader compile, no pipeline cache,
  no PPU module load, no SPU cache, nothing reported.
- Bundled driver unaffected on both.

## Package deltas (zip-root layout identical in both: `.so` + `meta.json` +
`VkLayer_MultiExynoos.json`)

| Aspect | v0.0.1 | v0.0.2 | Boot-relevant? |
| --- | --- | --- | --- |
| `meta.json.libraryName` | `vulkan.samsung.so` | `libVkLayer_MultiExynoos.so` | **Yes.** Decides which file the host dlopens as its driver handle. v0.0.1 named the stock driver (the package could not import at all until this was fixed); v0.0.2 names our shim, so the host loads the shim as the driver. |
| `meta.json.layerLibrary` | `libVkLayer_MultiExynoos.so` | **absent** | **Yes.** The layer-package signal v0.0.1 shipped is gone. A host that keys on it loses the layer-mode path it used with v0.0.1. |
| `meta.json.name` | `MultiExynoos Layer` | `MultiExynoos` | Display only. |
| `meta.json.description` | ASCII | contained a mangled em-dash byte on disk (fixed: ASCII now) | No (valid JSON either way). |
| `VkLayer_MultiExynoos.json` `implementation_version` | `1` | `2` | Only if a host gates on it; none known. |
| `.so` size | 8440 | 12272 | Implementation grew (standalone mode). |
| `.so` exports | `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`, `vkNegotiateLoaderLayerInterfaceVersion` (+2 `mx_` internals) | same + `vk_icdNegotiateLoaderICDInterfaceVersion`, `vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr` | Additive; loader/ICD interface symbols. |
| `DT_NEEDED` | liblog, libm, libdl, libc | same set | No. |
| SoC/Android gating | none in either | none | No. |

## Code deltas (layer.cpp, v0.0.1 source vs v0.0.2 source)

1. `mx_CreateInstance` without `VK_LAYER_LINK_INFO`:
   v0.0.1 logs and returns `VK_ERROR_INITIALIZATION_FAILED`.
   v0.0.2 enters standalone mode: self-loads `libvulkan.so` / `vulkan.samsung.so`
   and forwards. (Additive: only reachable when no loader chained us.)
2. **`vkGetInstanceProcAddr(VK_NULL_HANDLE, <pre-instance name>)`**:
   v0.0.1 returns `nullptr` (the host loader answers).
   v0.0.2 calls `ensure_underlying()` — dlopens `libvulkan.so` and returns the
   *system loader's* function — **even when we are chained as a layer**.
   This is a v0.0.1 behavioural break on the layer path: a host that asks the
   chain for `vkEnumerateInstanceExtensionProperties` / `vkEnumerateInstanceVersion`
   before `vkCreateInstance` gets the system graphics stack initialised out from
   under a session whose driver handle the host may have replaced.
3. Layer-mode `vkCreateInstance`, negotiate, and all forwarding after instance
   creation are line-for-line identical to v0.0.1.

## Most likely single cause (ranked)

1. **Delta 2 — pre-instance queries now self-load the system loader while
   chained as a layer.** Hits every host that used v0.0.1 in layer mode, which
   is the only mode v0.0.1 supported, so it can explain "worked on 0.0.1,
   black screen on 0.0.2" in both hosts at once.
2. **`libraryName`/`layerLibrary` flip** — host may now invoke the shim as the
   sole driver (standalone mode) where it previously chained it as a layer; the
   standalone self-load of `libvulkan.so` can fail inside an adrenotools driver
   namespace, producing exactly the silent `VK_ERROR_INITIALIZATION_FAILED`
   path that black-screens ARMSX3.

## Decisive on-device line

`adb logcat -s MultiExynoos` during the black-screen launch shows either
`vkCreateInstance -> 0  (MultiExynoos layer ...)` (chain path; then suspect 1)
or `... (MultiExynoos standalone ICD ...)` / `underlying '<path>' dlopen failed`
(standalone path; then suspect 2). v0.0.1 printed
`vkCreateInstance -> 0  (MultiExynoos passthrough ...)`.

## Fixed in this build

- Pre-instance queries return `nullptr` (v0.0.1 parity) whenever a loader
  negotiated the layer interface with us; only a non-layer host (direct
  dlopen / ICD entry) gets self-loaded forwarding.
- `meta.json` carries `layerLibrary` again (v0.0.1 parity, additive).
- Standalone candidate selection now requires the candidate to resolve
  `vkCreateInstance` before it is accepted, so a loader-shaped but
  instance-incapable `libvulkan.so` no longer blocks trying the vendor ICD.
