# MultiExynoos

**Open Vulkan driver stack for Samsung Xclipse (RDNA-based) GPUs — Exynos 1580 / Xclipse 540 first, Exynos 2500 / Xclipse 950 second.**
Target workload: heavy Android emulation (PS3-class / RPCS3 first; PS4-class / shadPS4 gated on measured feasibility).

> **Status: Phase 1 scaffold.** This is not yet a driver. The current tree ships a
> **pure passthrough Vulkan shim** whose only job is to prove the per-app
> loading path end to end (`meta.json` → launcher → layer or direct `dlopen` →
> stock `vulkan.samsung.so`). It changes zero behaviour by design.
> Since `0.0.2` the same `.so` serves two hosts: a Vulkan loader chains it as a
> layer (`VK_LAYER_LINK_INFO`), and adrenotools-style hosts that `dlopen` it and
> use it as the single driver get the **standalone-ICD** path — it opens the
> driver it wraps itself, no layer link info required.
> The actual driver track is a **RADV-based ICD** (Mesa, MIT) — see [docs/ROADMAP.md](docs/ROADMAP.md).

## Why this shape

Two reference points define the project (full citations: [docs/REFERENCE.md](docs/REFERENCE.md):

1. **Turnip (Adreno) wins because it is a real ICD**, not a shim: full control of
   compiler, memory, sync — plus commit-pinned CI builds, documented patch
   queues and per-app driver packaging (`K11MCH1/AdrenoToolsDrivers` `v26.0.0-rc08`,
   `Weab-chan/freedreno_turnip-CI` `25.3.0-devel_bbdd688`).
2. **Existing Xclipse community stacks are shims** (`ExynosTools` tags `1.6.0`–`2.1`
   wrap stock `vulkan.samsung.so` via a layer and spoof extensions) and therefore
   cannot fix compiler/memory/sync. The one attempt at a real ICD
   (`mxxme-dev/radv-xclipse-patches`, MIT) proves RADV initializes and submits
   on Xclipse but dies at Android WSI.

MultiExynoos = **Phase C+**: truthful compatibility layer now, RADV ICD after.

## Repository layout

| Path | Contents |
|---|---|
| `layer/` | `libVkLayer_MultiExynoos.so` + `VkLayer_MultiExynoos.json` (Phase 1 passthrough — loader layer *and* standalone ICD; Phase 2 intercepts) |
| `icd/` | `multiexynoos_icd.aarch64.json` — ICD manifest for the future RADV-based `libmultiexynoos_vulkan.so` (Phase 3) |
| `packaging/` | `meta.json` — launcher-facing package descriptor (schema-compatible keys with AdrenoTools-style driver managers) |
| `scripts/` | cross-build (`build-layer.ps1/.sh`), release packaging (`package.ps1`), on-device Phase-0 probe (`probe.sh`) |
| `.github/workflows/` | CI: host syntax check + NDK build + commit-pinned asset |
| `docs/` | [ROADMAP.md](docs/ROADMAP.md) (phases + binary acceptance gates), [REFERENCE.md](docs/REFERENCE.md) (evidence base) |

## Build (NDK required)

```powershell
# Windows — CMake path (CI uses this)
$env:ANDROID_NDK_HOME = "C:\path\to\ndk"
./scripts/build-layer.ps1 -Version 0.0.2
./scripts/package.ps1    # -> dist/MultiExynoos-layer-0.0.2_g<sha>.zip

# Windows — no-cmake fallback (direct NDK clang++)
git clone --depth 1 --branch v1.3.296 https://github.com/KhronosGroup/Vulkan-Headers.git external/Vulkan-Headers
./scripts/build-layer-direct.ps1 -Ndk "C:\path\to\ndk" -Version 0.0.2
./scripts/package.ps1
```

```bash
# Linux / CI
./scripts/build-layer.sh "$ANDROID_NDK_HOME"
pwsh ./scripts/package.ps1
```

## Package contents (release asset)

```
MultiExynoos-layer-<version>_g<gitsha>.zip
├── libVkLayer_MultiExynoos.so
├── VkLayer_MultiExynoos.json
└── meta.json
```

## Importing into an adrenotools host (ARMSX3, Yuzu-likes)

`libraryName` in `meta.json` **must name the `.so` that is present at the zip
root** — that is the file the host `dlopen`s and uses as the driver. It must
**not** be the vendor driver this layer *wraps* (`vulkan.samsung.so`); ARMSX3's
import validator (`CustomDriver.installFromStream`) reads `libraryName`, looks
for `<zip root>/<libraryName>`, and rejects the package with *"Import failed"*
when it is absent. `layerName` is the loader-layer identity and is ignored by
adrenotools hosts.

Since `0.0.2` the packaged `.so` is also a **standalone passthrough ICD** in
that situation: with no loader there is no `VK_LAYER_LINK_INFO`, so the shim
opens the driver it wraps itself and forwards every call. Set
`MX_UNDERLYING_DRIVER=/absolute/path/to/libvulkan.so` to control which driver is
wrapped; otherwise `libvulkan.so` is tried first, then `vulkan.samsung.so` by
soname and under `/vendor/lib64/hw/` and `/vendor/lib/hw/`.

## Deploy (per-app path — no root, no flash)

1. Extract the zip inside the emulator/launcher driver directory.
2. Set `VK_INSTANCE_LAYERS=VK_LAYER_MX_MULTIEXYNOOS` (debuggable containers may
   instead use `VK_LAYER_PATH` pointing at the extracted dir).
3. Disable escape hatch: `DISABLE_MULTIEXYNOOS_LAYER=1`.
4. `MX_DEBUG=1` logs layer load + `vkCreateInstance` passthrough (logcat tag
   `MultiExynoos`; on Android the shim always logs its `vkCreateInstance`
   result there — useful when a black screen needs explaining).

**Difference vs ExynosTools:** their `meta.json` also points at stock
`vulkan.samsung.so` + a layer — same wrapping model — but their layer spoofs
extensions and masks `VK_ERROR_DEVICE_LOST` (tag `1.6.0`); this layer does
neither, and the end state of this repo (Phase 3) *replaces* the ICD with a
RADV build rather than wrapping the closed driver forever.

## Phase-0 probe (before anything else)

```bash
adb shell sh < scripts/probe.sh
```

Captures GPU id, DRM nodes, stock extension surface → the baseline that fills
the capability-gap matrix with measured values.

## License

MIT (see [LICENSE](LICENSE)). Dependencies: Vulkan-Headers (Apache-2.0/MIT,
fetched at build time, pinned `v1.3.296`). **No code from ExynosTools** (that
repository ships no license file — ideas may be studied, code must not be
copied), no code from Mesa yet (Mesa is MIT — compatible — and will be used
under its own license when the Phase-3 ICD lands), no vendor blobs.

## Naming conventions (binding)

- Repo / project: `MultiExynoos`
- Layer asset: `MultiExynoos-layer-<version>_g<sha>.zip`
- ICD asset (Phase 3+): `MultiExynoos-vulkan-icd-<version>_g<sha>.zip`
- ICD manifest: `multiexynoos_icd.aarch64.json`
- Layer manifest: `VkLayer_MultiExynoos.json` / layer name `VK_LAYER_MX_MULTIEXYNOOS`
- Runtime knobs: `MX_DEBUG`, `DISABLE_MULTIEXYNOOS_LAYER`, `MX_UNDERLYING_DRIVER` (future: `MX_QUIRKS`)
