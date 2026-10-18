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

#include <cstdlib>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#include <dlfcn.h>
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
    MX_LOG("underlying driver loaded: %s", path);
    return true;
}

bool ensure_underlying() {
    if (underlying_gipa)
        return true;

    /* Explicit override wins, so an on-device experiment needs no rebuild. */
    if (const char *override_path = std::getenv("MX_UNDERLYING_DRIVER")) {
        if (load_underlying_from(override_path))
            return true;
    }

    /* Direct-dlopen hosts (adrenotools/ARMSX3) want the system stack, and here
     * libvulkan.so is safe: nothing has loaded us through it, so there is no
     * recursion. */
    if (!g_entered_as_icd && load_underlying_from("libvulkan.so"))
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

} // namespace

VKAPI_ATTR VkResult VKAPI_CALL mx_CreateInstance(
    const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
    VkInstance *out_instance) {
    /* ---- Mode 1: chained by a loader (VK_LAYER_LINK_INFO present) ---- */
    if (const VkLayerInstanceCreateInfo *link = find_instance_link(ci)) {
        next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;

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
