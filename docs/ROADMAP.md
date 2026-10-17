# MultiExynoos Roadmap

Ordered by emulation payoff per unit of effort. Every phase has binary
acceptance criteria; nothing advances on vibes. **All phases 0–5 are
userspace-only** — the only brick-risk work (kernel/firmware) is explicitly
out of scope until a separate, opt-in decision.

## Phase 0 — Probe & baseline
**Work:** run `scripts/probe.sh` on the Exynos 1580 device; `vulkaninfo --json`
on stock and on ExynosTools `2.1 MdiEx_Nativo_v3.2.zip`; attempt to load a
RADV ICD (mxxme-dev build) via `VK_ICD_FILENAMES` inside an emulator container;
record RPCS3/Eden/shadPS4 boot logs on stock.
**Acceptance:** committed `baseline-<sha>.json` containing (a) full stock
extension list, (b) pass/fail for all 11 capability-matrix rows (descriptor
indexing, timeline semaphores, sync2, dynamic rendering/renderpass2,
robustness2, float controls, draw indirect count, geometry, tessellation,
aliasing/format-list, BCn), (c) GPUID string, (d) RADV-load result.
**Exit gate:** no row of the matrix says "unknown".

## Phase 1 — Deployment plumbing *(current)*
**Work:** this repo — passthrough layer + meta.json + ICD manifest skeleton +
CI producing `MultiExynoos-layer-<v>_g<sha>.zip`.
**Acceptance:** (a) CI artifact builds green on host + NDK; (b) on-device:
`vulkaninfo` through the package equals stock extension count ±0;
(c) 30-min vkcube via the layer, 0 crashes, RSS delta <5%; (d) installs via
the launcher's driver selector with no root.
**Exit gate:** asset loads in Winlator/Eden/`VK_INSTANCE_LAYERS` test app.

## Phase 2 — Truthful gap-fill layer
**Work:** intercepts in `layer/`: extension advertisement limited to
probe-verified features; persistent on-disk `VkPipelineCache` (app dir, keyed
by build sha); descriptor-update fast paths; compute BCn decode (BC1–BC7,
reimplemented from public techniques — not copied from the unlicensed
ExynosTools tree); device-loss reported honestly, never masked.
**Acceptance:** (a) Eden boots 3 retail Switch titles to gameplay, 30 min,
0 device-lost; (b) BC7 decode ≥2× faster than `MdiEx_Nativo_v3.2.zip`
(same scene, p50 frame time); (c) pipeline-cache hit rate >80% on second run
(log counter).
**Exit gate:** `MultiExynoos-layer-0.2_g<sha>.zip` + published benchmark CSV.

## Phase 3 — RADV ICD on Xclipse 540 (the real driver)
**Work:** pin a Mesa base commit; carry the mxxme-dev RADV-on-Xclipse work
forward (MIT): Android WSI (gralloc/AHB + dma-buf — the mxxme BadDrawable
blocker), gfxip quirk table enabling only probed features, drm-shim GPUID
entries for off-device CI, ACO enabled; ship as
`MultiExynoos-vulkan-icd-<v>_g<sha>.zip` with `multiexynoos_icd.aarch64.json`.
**Acceptance:** (a) vkcube on the phone display (not Termux-X11) 60 min,
0 crashes; (b) all 11 matrix rows implemented per `vulkaninfo`;
(c) vkd3d-proton `d3d12info` enumerates clean; (d) Aztec Ruins (normal)
≥90% of stock-driver FPS; (e) RPCS3 boots one PS3 title to in-game.
**Exit gate:** Phase-4 hardening starts only with (e) met.
**Risk flag:** GPU hangs on this track may require a device reboot; stock
driver must remain selectable.

## Phase 4 — Emulator hardening
**Work:** robustness2 codegen, float controls (denorm policy),
`draw_indirect_count`, aliasing/maintenance4 correctness, memory-type tuning
for DXVK/vkd3d heaps.
**Acceptance:** (a) `VK_EXT_robustness2` reported + vkd3d-proton robustness
test passes; (b) RPCS3 3 titles, 60 min, p99 frame time regression <5% vs
Phase 3, 0 device-lost; (c) shadPS4 reaches menu (PS4 feasibility gate).
**Exit gate:** `MultiExynoos-vulkan-icd-0.4_g<sha>.zip` + regression CSV.

## Phase 5 — Performance & conformance (rolling)
**Work:** `MX_DEBUG` tuning knobs (wave32/64, denorm, tiling analogs of
Turnip's `TU_DEBUG`); dEQP-VK subset on device (`api.info`, `renderpass`,
`draw`); upstream MRs for Xclipse device-info to Mesa (MIT-compliant flow).
**Acceptance:** (a) dEQP subset 100% pass; (b) Aztec Ruins ≥110% of stock;
(c) RPCS3 target title p50 FPS ≥ stock baseline +15%.
**Exit gate:** build labeled conformance-subset.

## Phase 6 — Xclipse 950 (conditional)
**Gate:** PS4-class feasibility decided from Phase-4 shadPS4 data; requires
Exynos 2500 hardware.
**Work:** GPUID/firmware quirks only — no architectural fork.
**Acceptance:** Phase-4 gates met on 2500, or project publishes the negative
result and declares PS4-class out of scope.
