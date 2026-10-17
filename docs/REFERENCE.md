# Reference & Evidence Base

Every architectural claim in this repo traces to a tag/asset/file below.
All third-party artifacts were treated as read-only evidence; no code was
copied from any of them except where the license is verified compatible
(Mesa/MIT, mxxme-dev/MIT, Vulkan-Headers/Apache-2.0-or-MIT).

## Supplied: Exynos/Xclipse stack (read-only evidence)
- https://github.com/WearyConcern1165/ExynosTools/releases
  - tag `2.1` (commit `3dcdbcb`), assets `MdiEx_2200_Vortek_v3.2.zip`,
    `MdiEx_2400_Vortek_v3.2.zip`, `MdiEx_2600_Vortek_v3.2.zip`,
    `MdiEx_Nativo_v3.2.zip` — **no 1580/540 asset**.
  - repo `main`: `meta.json` (`libraryName: vulkan.samsung.so`,
    `layerLibrary`, `layerName`), `VkLayer_vortek_xclipse.json`,
    `src/layer/`, `third_party/bcdec` — **no LICENSE file**.
  - tag `1.6.0`: extension spoofing (`VK_EXT_transform_feedback`,
    `VK_EXT_custom_border_color`, `VK_EXT_depth_clip_enable`,
    `VK_EXT_provoking_vertex`); watchdog converts
    `VK_ERROR_DEVICE_LOST`→`VK_SUCCESS` in `vkWaitForFences`/`vkQueueSubmit`.
  - tag `1.5.0`: `vkGetPhysicalDeviceFormatProperties` interception; 16 MB
    staging ring; specialization-constant workgroup scaling per RDNA gen.
  - tag `2.0` (commit `fa0d842`): VMA adoption, persistent `VkPipelineCache`,
    descriptor-set optimization, compute ("Granite") BCn decode.

## Supplied: Snapdragon driver builds (packaging/deployment mechanics)
- https://github.com/K11MCH1/AdrenoToolsDrivers/releases — tag `v26.0.0-rc08`
  (commit `735d88a`), Mesa base `e9a83d530cce`, Vulkan 1.4.335, assets
  `Turnip_v26.0.0_R8_Gmem.zip` / `_Autotuner.zip`; `TU_DEBUG=gmem|sysmem`;
  `FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1`; autotuner = per-shader
  sysmem/gmem benchmarking; DECK_EMU via mesa MR `38808`; CB fix MR `38378`.
- https://github.com/Weab-chan/freedreno_turnip-CI/releases — tag
  `25.3.0-devel_bbdd688` (commit `7185728`), assets
  `turnip_Oct-11-2025_bbdd688.zip`, `..._patched.zip`; base-commit-pinned
  naming; patch queue `force_sysmem_no_autotuner.patch` with churn policy.
- https://github.com/MrPurple666/purple-turnip/releases — `turnip_mrpurple_T30-toasted.adpkg`,
  Mesa 26.3.0-devel, Vulkan 1.4.359, unified A6XX/A7XX/A8XX; T29 adds
  drm-shim GPU entries (810/825/829).
- https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases — tag `v37`
  (commit `50cbd61`): built from `whitebelyash/mesa-unified` (`turnip/gen8`);
  WinNative patches from `github.com/WinNative-Emu/Drivers/tree/main/patches`;
  a740 OneUI tearing fix.
- https://github.com/whitebelyash/AdrenoToolsDrivers/releases — `tu_v32`,
  `stu_v2`: `TU_DEBUG=deck_emu` Steam-Deck spoof; GMEM page faults on A830 →
  `TU_DEBUG=sysmem`; `tu_v27` ships unmerged timeline-sync MR (Zink/VulkanMod).
- https://github.com/StevenMXZ/freedreno_turnip-CI/releases → redirects to
  StevenMXZ/Adreno-Tools-Drivers (same lineage).

## Community Xclipse ICD attempt
- https://github.com/mxxme-dev/radv-xclipse-patches (MIT, 16 commits) —
  RADV on Xclipse 920 in Termux: compiles, inits, shaders compile,
  "submit ip=0" reaches GPU; blocked on WSI ("Android's sandbox blocks
  [DRI3/modifiers] → BadDrawable"; fallbacks "freeze the phone or result in
  'ghost rendering'"); loads via
  `VK_ICD_FILENAMES=.../radeon_icd.aarch64.json`.

## Mesa references (Phase 3 base)
- https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/freedreno
- https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/amd
  (`src/amd/vulkan` = RADV, `src/amd/compiler` = ACO — verified via
  https://docs.mesa3d.org/drivers/radv.html)
- https://docs.mesa3d.org/

## Pinned build/tooling
- Vulkan-Headers `v1.3.296` (Khronos, Apache-2.0/MIT) — fetched by CMake.
