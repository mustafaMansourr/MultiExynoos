/* SPDX-License-Identifier: MIT
 * MultiExynoos — passthrough Vulkan shim for Samsung Xclipse GPUs.
 *
 * Dual-mode by design, because two very different hosts load this one .so:
 *
 *   1. Loader layer. A Vulkan loader chains us via VK_LAYER_LINK_INFO and we
 *      forward to the next layer/ICD. This is the Phase-1 behaviour; it is
 *      unchanged, including pre-instance queries, which the host loader
 *      answers (v0.0.1 parity — see docs/REGRESSION-0.0.1-vs-0.0.2.md).
 *
 *   2. Standalone ICD. adrenotools hosts (ARMSX3 and the Yuzu-likes) dlopen
 *      this .so and use it as the *single* Vulkan driver. They call
 *      vkGetInstanceProcAddr(NULL, "vkCreateInstance"); there is no loader
 *      chain and therefore no VK_LAYER_LINK_INFO. In this mode we self-load
 *      the real driver and forward to it.
 *
 *   3. Android Vulkan HAL (hwvulkan) module - added in v0.0.3 to fix the
 *      ARMSX3-only black screen on Demon's Souls (BLES00932). ARMSX3's
 *      custom-driver hook intercepts the *system* Vulkan loader's sphal load
 *      of "vulkan.samsung.so" and substitutes this .so. The AOSP loader then
 *      requires the HMI hardware-module symbol (dlsym(so, "HMI")) and opens
 *      the device through hmi->methods->open("vk0"); every dispatch goes
 *      through the returned hwvulkan_device_t. v0.0.2 exported no HMI, so
 *      the loader fell back to the stub HAL (zero GPUs) and the emulator
 *      fataled with "No compatible GPU devices found". v0.0.3 exports HMI
 *      and forwards the three HAL entry points to the vendor ICD. See
 *      docs/ARMSX3-0.0.3-Triage.md.
 *
 * Why mode 2 exists: before it, mode 2 hit the "vkCreateInstance without
 * VK_LAYER_LINK_INFO" branch and returned VK_ERROR_INITIALIZATION_FAILED. On
 * ARMSX3 that surfaced as a permanent black screen with nothing in the log,
 * because the emulator's Vulkan instance never came up: its RSX thread then
 * died in on_init_thread() before it released the PPU thread, so no shader
 * compilation, no pipeline compilation, no PPU module load and no SPU cache
 * ever ran. Returning a real instance here is what unblocks that.
 *
 * Phase 2 adds intercepts (on-disk pipeline cache, truthful extension
 * advertisement, compute BCn decode). Phase 3 replaces the wrapped driver
 * with a RADV-based ICD (see docs/ROADMAP.md). This file stays the shim.
 *
 * License note: written from scratch against the public Vulkan-Loader layer
 * and loader/ICD interfaces. No code was copied from ExynosTools (that repo
 * ships no license file — treat as unlicensed), from Mesa, or from any
 * vendor driver.
 */
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vk_icd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#include <dlfcn.h>
#include <cstdio>
#include <sys/system_properties.h>
#define MX_LOG(fmt, ...)                                                       \
    __android_log_print(ANDROID_LOG_INFO, "MultiExynoos", fmt, ##__VA_ARGS__)
#else
#include <cstdio>
#include <dlfcn.h>
static bool mx_log_enabled() {
    static const bool enabled = std::getenv("MX_DEBUG") != nullptr;
    return enabled;
}
#define MX_LOG(fmt, ...)                                                       \
    do {                                                                       \
        if (mx_log_enabled())                                                  \
            std::fprintf(stderr, "[MultiExynoos] " fmt "\n", ##__VA_ARGS__);   \
    } while (0)
#endif

/* One-time mode banner: names how we were entered and what we forward to, so
 * a device log shows the active mode without digging. First entry wins. */
static void mx_log_banner_once(const char *mode, const char *target) {
    static bool s_logged = false;
    if (s_logged)
        return;
    s_logged = true;
    MX_LOG("MultiExynoos " MX_VERSION_STR " entered as %s, forwarding to %s",
           mode, target ? target : "(none)");
}

#ifndef MX_VERSION_STR
#define MX_VERSION_STR "0.0.2-dev"
#endif

/* Loader layer interface version we speak. v2 = current negotiate+
 * vkGetInstanceProcAddr(NULL, ...) semantics. */
static constexpr uint32_t kMaxLoaderLayerInterface = 2;

/* Forward declarations: both the loader (via the negotiate struct) and the
 * ICD host (via vkGetInstanceProcAddr) reach these, so they are declared
 * before either entry point is filled in. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
mx_GetInstanceProcAddr(VkInstance instance, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
mx_GetDeviceProcAddr(VkDevice device, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
mx_GetPhysicalDeviceProcAddr(VkInstance instance, const char *name);
VKAPI_ATTR VkResult VKAPI_CALL
mx_CreateInstance(const VkInstanceCreateInfo *ci,
                  const VkAllocationCallbacks *alloc, VkInstance *out_instance);
VKAPI_ATTR VkResult VKAPI_CALL
mx_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v);
VKAPI_ATTR VkResult VKAPI_CALL
mx_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion);

#ifdef __ANDROID__
/* ---- Android Vulkan HAL (hwvulkan) contract ------------------------------
 * Added in v0.0.3 to fix the ARMSX3-only black screen on Demon's Souls
 * (docs/ARMSX3-0.0.3-Triage.md). ARMSX3's custom-driver hook intercepts the
 * *system* Vulkan loader's sphal load of "vulkan.samsung.so" and substitutes
 * this .so. The AOSP loader (frameworks/native/vulkan/libvulkan/driver.cpp
 * LoadDriver) then does, in order:
 *
 *   1. dlsym(so, "HMI")  -> if absent: ALOGE("couldn't find symbol 'HMI' in
 *      HAL library"), dlclose, -EINVAL. The loader falls back to the stub
 *      HAL, which enumerates zero GPUs, and the emulator fataled with
 *      "No compatible GPU devices found" (VKGSRender.cpp:445) -> the
 *      v0.0.2 black screen this release fixes.
 *   2. strcmp(hmi->id, "vulkan")  -> must match.
 *   3. hmi->dso = so  -> the loader WRITES into HMI, so it must be a
 *      writable (non-const) data symbol.
 *   4. module->common.methods->open(&common, "vk0", &device) -> then every
 *      dispatch (driver.cpp:572/585/980 extension enumeration, 858/1257/
 *      1294/1297/1307 proc-addr, 1283 CreateInstance) goes through the
 *      returned hwvulkan_device_t, whose common.close must be callable.
 *
 * The NDK ships neither hardware/hardware.h nor hardware/hwvulkan.h, so the
 * minimal ABI subset is declared below, field-for-field against AOSP main
 * (LP64 layouts; verified - hw_module_t reserves uint64_t[32-7],
 * hw_device_t reserves uint64_t[12]). This is a pure forwarder to the vendor
 * ICD; it changes no rendering behaviour. --------------------------------- */

struct mx_hw_module_t;
struct mx_hw_device_t;

struct mx_hw_module_methods_t {
    int (*open)(const mx_hw_module_t *module, const char *id,
                mx_hw_device_t **device);
};

struct mx_hw_module_t {
    uint32_t tag;
    uint16_t module_api_version; /* HWVULKAN_MODULE_API_VERSION_0_1 = 0x0001 */
    uint16_t hal_api_version;
    const char *id;              /* "vulkan" (HWVULKAN_HARDWARE_MODULE_ID) */
    const char *name;
    const char *author;
    mx_hw_module_methods_t *methods;
    void *dso;                   /* loader writes this (hmi->dso = so) */
#ifdef __LP64__
    uint64_t reserved[32 - 7];
#else
    uint32_t reserved[32 - 7];
#endif
};

struct mx_hw_device_t {
    uint32_t tag;   /* HARDWARE_DEVICE_TAG */
    uint32_t version; /* HWVULKAN_DEVICE_API_VERSION_0_1 = 0x00010000 */
    mx_hw_module_t *module;
#ifdef __LP64__
    uint64_t reserved[12];
#else
    uint32_t reserved[12];
#endif
    int (*close)(mx_hw_device_t *device);
};

struct mx_hwvulkan_module_t {
    mx_hw_module_t common;
};

struct mx_hwvulkan_device_t {
    mx_hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};

/* HARDWARE_MODULE_TAG = 'HWMT', HARDWARE_DEVICE_TAG = 'HWDT'. */
static constexpr uint32_t kHwModuleTag = 0x48574D54u;
static constexpr uint32_t kHwDeviceTag = 0x48574454u;
static constexpr uint16_t kHwvulkanModuleApi01 = 0x0001u;
static constexpr uint32_t kHwvulkanDeviceApi01 = 0x00010000u;
#endif /* __ANDROID__ */

namespace {

PFN_vkGetInstanceProcAddr next_gipa = nullptr; /* layer mode: from link info */
PFN_vkGetInstanceProcAddr real_gipa = nullptr; /* post-instance forwarder    */
PFN_vkGetDeviceProcAddr real_gdpa = nullptr;

/* Standalone-ICD mode: the driver we wrap, opened by us rather than supplied
 * by a loader. Held for the process lifetime on purpose — a Vulkan driver
 * cannot be unloaded while any resolved pointer is still reachable. */
void *g_underlying = nullptr;
PFN_vkGetInstanceProcAddr underlying_gipa = nullptr;
PFN_vkCreateInstance underlying_create = nullptr;
/* Path of the driver we wrapped, for the one-time mode banner. Static buffer:
 * the candidate list is fixed, so only the chosen path is ever copied. */
const char *g_underlying_path = nullptr;
char g_underlying_path_buf[256];

/* Set once a Vulkan loader has negotiated the ICD interface with us. Used to
 * avoid dlopen("libvulkan.so") recursing into the loader that just loaded us. */
bool g_entered_as_icd = false;

/* Set once a Vulkan loader has negotiated the *layer* interface with us.
 * v0.0.1 parity: in that situation pre-instance queries
 * (vkEnumerateInstanceVersion / vkEnumerateInstanceExtensionProperties /
 * vkEnumerateInstanceLayerProperties) belong to the loader, and answering
 * them by self-loading the system libvulkan.so would initialise the system
 * graphics stack underneath a chained session — the v0.0.2 regression
 * against v0.0.1 (see docs/REGRESSION-0.0.1-vs-0.0.2.md). */
bool g_entered_as_layer = false;

#ifdef __ANDROID__
/* HAL mode (v0.0.3): set once the system loader opened us through HMI. In
 * that situation the caller above us IS the system Vulkan loader, so the
 * driver we forward to must be the real vendor ICD - never libvulkan.so,
 * which would recurse (loader -> HAL(us) -> loader CreateInstance ->
 * HAL dispatch(us) -> ...). The HAL-mode underlying ladder below therefore
 * never includes libvulkan.so. */
bool g_entered_as_hal = false;
void *g_hal_underlying = nullptr;
PFN_vkGetInstanceProcAddr hal_gipa = nullptr;
PFN_vkCreateInstance hal_create = nullptr;
PFN_vkEnumerateInstanceExtensionProperties hal_enum_ext = nullptr;
/* Re-entrancy guard: open() must not recurse into itself. */
bool g_hal_open_in_progress = false;

/* Reject a candidate that resolves back to us: a host hook that redirects
 * driver loads (ARMSX3's custom-driver hook matches "vulkan.samsung.so") can
 * bounce our own dlopen of the vendor driver back to this very .so. Calling
 * through that would recurse forever (we would forward every call to
 * ourselves). dladdr compares the DSO base, so even a separate copy of this
 * library installed at another path is rejected. */
bool gipa_is_self(PFN_vkGetInstanceProcAddr gipa) {
    if (!gipa)
        return false;
    if (gipa == &mx_GetInstanceProcAddr)
        return true;
    Dl_info self_info{};
    Dl_info cand_info{};
    if (!dladdr(reinterpret_cast<void *>(gipa), &cand_info) ||
        !dladdr(reinterpret_cast<void *>(&mx_GetInstanceProcAddr), &self_info))
        return false;
    return cand_info.dli_fbase && self_info.dli_fbase &&
           cand_info.dli_fbase == self_info.dli_fbase;
}
#endif /* __ANDROID__ */

const VkLayerInstanceCreateInfo *find_instance_link(
    const VkInstanceCreateInfo *ci) {
    auto *chain =
        reinterpret_cast<const VkLayerInstanceCreateInfo *>(ci->pNext);
    while (chain &&
           !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
             chain->function == VK_LAYER_LINK_INFO)) {
        chain = reinterpret_cast<const VkLayerInstanceCreateInfo *>(chain->pNext);
    }
    return chain;
}

/* Try one dlopen candidate. Accepts either loader-style
 * vkGetInstanceProcAddr or the ICD-style vk_icdGetInstanceProcAddr, since a
 * stock vendor ICD (vulkan.samsung.so) exports only the latter. */
bool load_underlying_from(const char *path) {
    if (!path || !*path)
        return false;

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *err = dlerror();
        MX_LOG("underlying '%s' dlopen failed: %s", path,
               err ? err : "unknown");
        return false;
    }

    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(handle, "vkGetInstanceProcAddr"));
    if (!gipa) {
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(handle, "vk_icdGetInstanceProcAddr"));
    }
    if (!gipa) {
        MX_LOG("underlying '%s' exports no vkGetInstanceProcAddr / "
               "vk_icdGetInstanceProcAddr",
               path);
        dlclose(handle);
        return false;
    }

#ifdef __ANDROID__
    /* Self-load guard: the same hook that redirects the system loader's
     * vendor-driver load can redirect ours; loading ourselves as the
     * "underlying driver" would recurse forever. */
    if (gipa_is_self(gipa)) {
        MX_LOG("underlying '%s' resolved to MultiExynoos itself (host "
               "redirected the load); rejecting", path);
        dlclose(handle);
        return false;
    }
#endif

    /* A candidate that cannot create an instance is useless to us; reject it
     * here so the search continues with the next candidate (e.g. a
     * loader-shaped but instance-incapable libvulkan.so must not block the
     * vendor ICD). */
    auto create = reinterpret_cast<PFN_vkCreateInstance>(
        gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create)
        create = reinterpret_cast<PFN_vkCreateInstance>(
            dlsym(handle, "vkCreateInstance"));
    if (!create) {
        MX_LOG("underlying '%s' exposes no vkCreateInstance; trying next "
               "candidate", path);
        dlclose(handle);
        return false;
    }

    g_underlying = handle;
    underlying_gipa = gipa;
    std::snprintf(g_underlying_path_buf, sizeof(g_underlying_path_buf), "%s",
                  path);
    g_underlying_path = g_underlying_path_buf;
    MX_LOG("underlying driver loaded: %s", path);
    return true;
}

bool ensure_underlying() {
    if (underlying_gipa)
        return true;

#ifdef __ANDROID__
    /* HAL mode: the vendor ICD is already open (that is what HAL mode * loaded); reuse it rather than loading anything else. In this * situation libvulkan.so is the caller above us and must not be used. */
    if (g_entered_as_hal && hal_gipa) {
        g_underlying = g_hal_underlying;
        underlying_gipa = hal_gipa;
        underlying_create = hal_create;
        MX_LOG("reusing HAL vendor ICD for the standalone path");
        return true;
    }
#endif

    /* Explicit override wins, so an on-device experiment needs no rebuild. */
    if (const char *override_path = std::getenv("MX_UNDERLYING_DRIVER")) {
        if (load_underlying_from(override_path))
            return true;
    }

    /* Direct-dlopen hosts (adrenotools/ARMSX3) want the system stack, and here
     * libvulkan.so is safe: nothing has loaded us through it, so there is no
     * recursion - unless the system loader has meanwhile opened us as its HAL
     * (v0.0.3): then libvulkan.so sits above us and would recurse back down. */
    if (!g_entered_as_icd && !g_entered_as_hal &&
        load_underlying_from("libvulkan.so"))
        return true;

    /* Wrapped-vendor targets, by soname and by the two vendor lib paths. These
     * are the correct targets when we were entered as an ICD by a loader: the
     * vendor ICD does not loop back into that loader. */
    if (load_underlying_from("vulkan.samsung.so"))
        return true;
    if (load_underlying_from("/vendor/lib64/hw/vulkan.samsung.so"))
        return true;
    if (load_underlying_from("/vendor/lib/hw/vulkan.samsung.so"))
        return true;

    /* Deliberately NOT falling back to libvulkan.so when g_entered_as_icd:
     * that dlopen would recurse (loader -> us -> loader) and crash. Fail with
     * a clear log instead. */
    return false;
}

#ifdef __ANDROID__
/* ---- HAL-mode underlying ladder -----------------------------------------
 * Never includes libvulkan.so: in HAL mode the system loader is the caller
 * above us, and forwarding into it would recurse (loader -> HAL(us) ->
 * loader CreateInstance -> HAL dispatch(us) -> ...). The target is always
 * the real vendor ICD, which does not loop back into the loader. */

/* Adopt an already-opened vendor handle. Prefers the vendor's own HMI device
 * (byte-identical to the un-hooked loader path: same functions, same handles,
 * same dispatch magic), falling back to ICD-style entry points. */
bool adopt_hal_handle(void *handle, const char *path) {
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(handle, "vkGetInstanceProcAddr"));
    if (!gipa) {
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(handle, "vk_icdGetInstanceProcAddr"));
    }
    if (!gipa) {
        MX_LOG("HAL: '%s' exports no gipa entry point; trying next candidate",
               path);
        dlclose(handle);
        return false;
    }

    /* Self-load guard: reject a candidate that resolved back to us. */
    if (gipa_is_self(gipa)) {
        MX_LOG("HAL: '%s' resolved to MultiExynoos itself (host redirected "
               "the load); rejecting", path);
        dlclose(handle);
        return false;
    }

    /* Preferred: the vendor's own hwvulkan device, opened exactly as the
     * un-hooked loader would open it. */
    auto *vhmi = reinterpret_cast<mx_hwvulkan_module_t *>(
        dlsym(handle, "HMI"));
    if (vhmi && vhmi->common.id &&
        !std::strcmp(vhmi->common.id, "vulkan") && vhmi->common.methods &&
        vhmi->common.methods->open) {
        vhmi->common.dso = handle;
        mx_hw_device_t *vdev = nullptr;
        if (vhmi->common.methods->open(&vhmi->common, "vk0", &vdev) == 0 &&
            vdev) {
            auto *vvd = reinterpret_cast<mx_hwvulkan_device_t *>(vdev);
            if (vvd->EnumerateInstanceExtensionProperties && vvd->CreateInstance &&
                vvd->GetInstanceProcAddr) {
                hal_enum_ext = vvd->EnumerateInstanceExtensionProperties;
                hal_create = vvd->CreateInstance;
                hal_gipa = vvd->GetInstanceProcAddr;
                g_hal_underlying = handle;
                std::snprintf(g_underlying_path_buf,
                              sizeof(g_underlying_path_buf), "%s", path);
                g_underlying_path = g_underlying_path_buf;
                MX_LOG("HAL: vendor ICD loaded via its HMI: %s", path);
                return true;
            }
        }
        MX_LOG("HAL: vendor HMI present but open failed for '%s'; falling "
               "back to ICD entry points", path);
    }

    /* Fallback: ICD-style entry points. */
    auto create = reinterpret_cast<PFN_vkCreateInstance>(
        gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create)
        create = reinterpret_cast<PFN_vkCreateInstance>(
            dlsym(handle, "vkCreateInstance"));
    auto enum_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    if (!enum_ext) {
        enum_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            dlsym(handle, "vkEnumerateInstanceExtensionProperties"));
    }
    if (!create || !enum_ext) {
        MX_LOG("HAL: '%s' exposes no %s; trying next candidate", path,
               !create ? "vkCreateInstance"
                       : "vkEnumerateInstanceExtensionProperties");
        dlclose(handle);
        return false;
    }

    g_hal_underlying = handle;
    hal_gipa = gipa;
    hal_create = create;
    hal_enum_ext = enum_ext;
    std::snprintf(g_underlying_path_buf, sizeof(g_underlying_path_buf), "%s",
                  path);
    g_underlying_path = g_underlying_path_buf;
    MX_LOG("HAL: vendor ICD loaded via ICD entry points: %s", path);
    return true;
}

bool load_hal_underlying_from(const char *path) {
    if (!path || !*path)
        return false;
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *err = dlerror();
        MX_LOG("HAL: '%s' dlopen failed: %s", path, err ? err : "unknown");
        return false;
    }
    return adopt_hal_handle(handle, path);
}

/* Already-mapped vendor driver: /proc/self/maps names the exact path that
 * was loaded, which a soname hook cannot redirect (it matches the requested
 * name, not a resolved path). libvulkan.so is skipped: it is the caller. */
bool load_hal_underlying_from_maps() {
    std::FILE *f = std::fopen("/proc/self/maps", "r");
    if (!f)
        return false;
    char line[512];
    bool ok = false;
    while (!ok && std::fgets(line, sizeof(line), f)) {
        char *path = std::strchr(line, '/');
        if (!path)
            continue;
        char *nl = std::strpbrk(path, "\n\r");
        if (nl)
            *nl = '\0';
        if (!std::strstr(path, "vulkan.") || !std::strstr(path, ".so"))
            continue;
        if (std::strstr(path, "libvulkan.so"))
            continue; /* the caller above us; recursing */
        ok = load_hal_underlying_from(path);
    }
    std::fclose(f);
    return ok;
}

bool ensure_hal_underlying() {
    if (hal_gipa)
        return true;

    /* Explicit override wins; refuse the system loader (it would recurse). */
    if (const char *override_path = std::getenv("MX_UNDERLYING_DRIVER")) {
        if (std::strstr(override_path, "libvulkan.so")) {
            MX_LOG("HAL: MX_UNDERLYING_DRIVER=%s is the system loader; it "
                   "must not be the HAL-mode underlying", override_path);
        } else if (load_hal_underlying_from(override_path)) {
            return true;
        }
    }

    /* Property-derived sonames, mirroring the loader's own resolution
     * (driver.cpp LoadDriver: "vulkan." + prop + ".so"). */
    char prop[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.hardware.vulkan", prop) > 0 && prop[0]) {
        char soname[64];
        std::snprintf(soname, sizeof(soname), "vulkan.%s.so", prop);
        if (load_hal_underlying_from(soname))
            return true;
    }
    if (__system_property_get("ro.board.platform", prop) > 0 && prop[0]) {
        char soname[64];
        std::snprintf(soname, sizeof(soname), "vulkan.%s.so", prop);
        if (load_hal_underlying_from(soname))
            return true;
    }

    /* Already-mapped vendor path (unhookable form). */
    if (load_hal_underlying_from_maps())
        return true;

    /* Absolute vendor paths, then the bare soname, then a sphal load. */
    if (load_hal_underlying_from("/vendor/lib64/hw/vulkan.samsung.so"))
        return true;
    if (load_hal_underlying_from("/vendor/lib/hw/vulkan.samsung.so"))
        return true;
    if (load_hal_underlying_from("vulkan.samsung.so"))
        return true;

    using load_sphal_fn = void *(*)(const char *, int);
    static load_sphal_fn load_sphal = reinterpret_cast<load_sphal_fn>(
        dlsym(RTLD_DEFAULT, "android_load_sphal_library"));
    if (load_sphal) {
        void *handle = load_sphal("vulkan.samsung.so", RTLD_NOW);
        if (handle && adopt_hal_handle(handle, "sphal vulkan.samsung.so"))
            return true;
    }

    /* libvulkan.so is deliberately NEVER a candidate here. */
    return false;
}

/* No-op close: the loader may call common.close on driver unload paths; the
 * vendor ICD stays mapped for the process lifetime by design (a Vulkan
 * driver cannot be unloaded while any resolved pointer is reachable). */
int mx_hal_close(mx_hw_device_t *device) {
    (void)device;
    return 0;
}

/* The three HAL entry points the AOSP loader dispatches through. Pure
 * forwarders to the vendor ICD; no behaviour is added or changed. */
VkResult mx_hal_EnumerateInstanceExtensionProperties(
    const char *layer_name, uint32_t *count, VkExtensionProperties *props) {
    return hal_enum_ext(layer_name, count, props);
}

VkResult mx_hal_CreateInstance(const VkInstanceCreateInfo *ci,
                               const VkAllocationCallbacks *alloc,
                               VkInstance *out_instance) {
    const VkResult r = hal_create(ci, alloc, out_instance);
    MX_LOG("HAL vkCreateInstance -> %d  (MultiExynoos HAL " MX_VERSION_STR ")",
           static_cast<int>(r));
    return r;
}

PFN_vkVoidFunction mx_hal_GetInstanceProcAddr(VkInstance instance,
                                              const char *name) {
    return hal_gipa ? hal_gipa(instance, name) : nullptr;
}

int mx_hal_open(const mx_hw_module_t *module, const char *id,
                mx_hw_device_t **device);

mx_hw_module_methods_t g_mx_hal_methods = {&mx_hal_open};

#endif /* __ANDROID__ */

} // namespace

#ifdef __ANDROID__
/* HAL_MODULE_INFO_SYM: the hardware-module table the Android system Vulkan
 * loader dlsyms (driver.cpp LoadDriver, dlsym(so, "HMI")). MUST be writable
 * data: the loader stores the library handle in hmi->dso, so this cannot be
 * const. Inert for every host that never dlsyms HMI: adrenotools/ARMSX3
 * direct dlopen and loader-layer chaining keep using vkGetInstanceProcAddr
 * exactly as before. */
extern "C" {
__attribute__((visibility("default"))) mx_hwvulkan_module_t HMI = {
    kHwModuleTag,          /* common.tag                            */
    kHwvulkanModuleApi01,  /* common.module_api_version             */
    0,                     /* common.hal_api_version                */
    "vulkan",              /* common.id  (checked by the loader)    */
    "MultiExynoos",        /* common.name                           */
    "MultiExynoos",        /* common.author                         */
    &g_mx_hal_methods,     /* common.methods -> mx_hal_open         */
    nullptr,               /* common.dso (the loader writes this)   */
    {}                     /* common.reserved                       */
};
} // extern "C"

namespace {

/* methods->open(HMI.common, "vk0", &device): the one call the system loader
 * makes into the module table. A missing vendor ICD is a hard, named failure
 * - no silent stub success. The loader then reports "failed to open Vulkan
 * HAL device" and the emulator shows its own fatal. */
int mx_hal_open(const mx_hw_module_t *module, const char *id,
                mx_hw_device_t **device) {
    (void)module;
    if (!device)
        return -EINVAL;
    if (!id || std::strcmp(id, "vk0") != 0) {
        MX_LOG("HAL: open with unexpected device id '%s' (want 'vk0')",
               id ? id : "(null)");
        return -EINVAL;
    }
    if (g_hal_open_in_progress) {
        MX_LOG("HAL: re-entrant open refused");
        return -EBUSY;
    }

    g_hal_open_in_progress = true;
    g_entered_as_hal = true;

    if (!ensure_hal_underlying()) {
        g_hal_open_in_progress = false;
        g_entered_as_hal = false;
        MX_LOG("HAL mode failed: no vendor Vulkan driver reachable. Set "
               "MX_UNDERLYING_DRIVER to the vendor ICD absolute path.");
        return -ENODEV;
    }
    g_hal_open_in_progress = false;

    mx_log_banner_once("HAL (hwvulkan)", g_underlying_path);

    static mx_hwvulkan_device_t dev;
    dev.common.tag = kHwDeviceTag;
    dev.common.version = kHwvulkanDeviceApi01;
    dev.common.module = const_cast<mx_hw_module_t *>(&HMI.common);
    dev.common.close = &mx_hal_close;
    dev.EnumerateInstanceExtensionProperties =
        &mx_hal_EnumerateInstanceExtensionProperties;
    dev.CreateInstance = &mx_hal_CreateInstance;
    dev.GetInstanceProcAddr = &mx_hal_GetInstanceProcAddr;

    *device = &dev.common;
    MX_LOG("HAL mode active: MultiExynoos " MX_VERSION_STR " forwards to the "
           "vendor ICD");
    return 0;
}

} // namespace
#endif /* __ANDROID__ */

VKAPI_ATTR VkResult VKAPI_CALL mx_CreateInstance(
    const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
    VkInstance *out_instance) {
    /* ---- Mode 1: chained by a loader (VK_LAYER_LINK_INFO present) ---- */
    if (const VkLayerInstanceCreateInfo *link = find_instance_link(ci)) {
        next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;

        mx_log_banner_once("loader layer", "the next layer/ICD in the chain");

        auto create = reinterpret_cast<PFN_vkCreateInstance>(
            next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
        if (!create) {
            MX_LOG("layer mode: next layer exposes no vkCreateInstance");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const VkResult r = create(ci, alloc, out_instance);
        MX_LOG("vkCreateInstance -> %d  (MultiExynoos layer " MX_VERSION_STR ")",
               static_cast<int>(r));

        if (r == VK_SUCCESS) {
            real_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                next_gipa(*out_instance, "vkGetInstanceProcAddr"));
            real_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                next_gipa(*out_instance, "vkGetDeviceProcAddr"));
        }
        return r;
    }

    /* ---- Mode 2: we ARE the driver (adrenotools/ARMSX3 direct dlopen) ---- */
    if (!ensure_underlying()) {
        MX_LOG("no underlying Vulkan driver available; set MX_UNDERLYING_DRIVER "
               "to an absolute .so path");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    mx_log_banner_once("standalone ICD", g_underlying_path);

    if (!underlying_create) {
        underlying_create = reinterpret_cast<PFN_vkCreateInstance>(
            underlying_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    }
    if (!underlying_create) {
        underlying_create = reinterpret_cast<PFN_vkCreateInstance>(
            dlsym(g_underlying, "vkCreateInstance"));
    }
    if (!underlying_create) {
        MX_LOG("underlying driver exposes no vkCreateInstance");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const VkResult r = underlying_create(ci, alloc, out_instance);
    MX_LOG("vkCreateInstance -> %d  (MultiExynoos standalone ICD " MX_VERSION_STR
           ")",
           static_cast<int>(r));

    if (r == VK_SUCCESS) {
        real_gipa = underlying_gipa;
        real_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            underlying_gipa(*out_instance, "vkGetDeviceProcAddr"));
        if (!real_gdpa) {
            real_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                dlsym(g_underlying, "vkGetDeviceProcAddr"));
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL
mx_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
    if (!v)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion < MIN_SUPPORTED_LOADER_LAYER_INTERFACE_VERSION)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion > kMaxLoaderLayerInterface)
        v->loaderLayerInterfaceVersion = kMaxLoaderLayerInterface;
    /* Hand the loader our dispatch entry points; without these the layer is
     * loaded but never called. */
    g_entered_as_layer = true;
    v->pfnGetInstanceProcAddr = mx_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = mx_GetDeviceProcAddr;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
mx_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion) {
    if (!pVersion)
        return VK_ERROR_INITIALIZATION_FAILED;

    /* Remember we were entered as an ICD so ensure_underlying() does not
     * dlopen the very loader that is calling us. */
    g_entered_as_icd = true;

    if (*pVersion > CURRENT_LOADER_ICD_INTERFACE_VERSION)
        *pVersion = CURRENT_LOADER_ICD_INTERFACE_VERSION;
    if (*pVersion < MIN_SUPPORTED_LOADER_ICD_INTERFACE_VERSION)
        return VK_ERROR_INITIALIZATION_FAILED;

    MX_LOG("ICD interface negotiated (version %u)", *pVersion);
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
mx_GetDeviceProcAddr(VkDevice device, const char *name) {
    if (!name)
        return nullptr;
    if (!std::strcmp(name, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(mx_GetDeviceProcAddr);
    /* Phase 1: intercept nothing — straight passthrough. */
    return real_gdpa ? real_gdpa(device, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
mx_GetPhysicalDeviceProcAddr(VkInstance instance, const char *name) {
    if (!name)
        return nullptr;
    /* Best-effort: a loader forwards physical-device extension queries here.
     * The instance/device split is not needed for a passthrough. */
    if (real_gipa)
        return real_gipa(instance, name);
    if (ensure_underlying())
        return underlying_gipa(instance, name);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
mx_GetInstanceProcAddr(VkInstance instance, const char *name) {
    if (!name)
        return nullptr;
    if (!std::strcmp(name, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(mx_GetInstanceProcAddr);
    if (!std::strcmp(name, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(mx_GetDeviceProcAddr);
    if (!std::strcmp(name, "vkCreateInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(mx_CreateInstance);
    if (!std::strcmp(name, "vkNegotiateLoaderLayerInterfaceVersion"))
        return reinterpret_cast<PFN_vkVoidFunction>(
            mx_NegotiateLoaderLayerInterfaceVersion);
    if (!std::strcmp(name, "vk_icdGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(mx_GetInstanceProcAddr);
    if (!std::strcmp(name, "vk_icdGetPhysicalDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(mx_GetPhysicalDeviceProcAddr);
    if (!std::strcmp(name, "vk_icdNegotiateLoaderICDInterfaceVersion"))
        return reinterpret_cast<PFN_vkVoidFunction>(
            mx_icdNegotiateLoaderICDInterfaceVersion);

    /* Pre-instance queries (vkEnumerateInstanceVersion,
     * vkEnumerateInstanceExtensionProperties, vkEnumerateInstanceLayerProperties):
     *   - A loader negotiated the layer interface with us (v0.0.1 behaviour):
     *     these belong to that loader; return nullptr so it answers them.
     *     Self-loading libvulkan.so here would regress v0.0.1.
     *   - No loader chain (adrenotools direct dlopen, ICD entry): forward to
     *     the driver we wrap, because nothing else will answer for us. */
    if (instance == VK_NULL_HANDLE) {
        if (g_entered_as_layer && !g_entered_as_icd) {
            static bool s_logged = false;
            if (!s_logged) {
                s_logged = true;
                MX_LOG("layer mode: pre-instance query '%s' left to the "
                       "loader (v0.0.1 parity)", name);
            }
            return nullptr;
        }
        if (ensure_underlying())
            return underlying_gipa(VK_NULL_HANDLE, name);
        return nullptr;
    }

    if (real_gipa)
        return real_gipa(instance, name);
    if (ensure_underlying())
        return underlying_gipa(instance, name);
    return nullptr;
}

extern "C" {

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface *v) {
    return mx_NegotiateLoaderLayerInterfaceVersion(v);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    return mx_GetInstanceProcAddr(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name) {
    return mx_GetDeviceProcAddr(device, name);
}

/* Loader/ICD interface (vk_icd.h). Exported so this .so can be used as a
 * standalone ICD by a Vulkan loader as well as by adrenotools hosts. */
VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion) {
    return mx_icdNegotiateLoaderICDInterfaceVersion(pVersion);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
    return mx_GetInstanceProcAddr(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *name) {
    return mx_GetPhysicalDeviceProcAddr(instance, name);
}

} // extern "C"
