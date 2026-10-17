/* SPDX-License-Identifier: MIT
 * MultiExynoos — Phase 1 passthrough Vulkan layer for Samsung Xclipse GPUs.
 *
 * Purpose (Phase 1): prove the per-app driver/layer loading path
 * (meta.json + VkLayer_MultiExynoos.json + VK_INSTANCE_LAYERS) end to
 * end, with ZERO behaviour change: every call is forwarded to the next
 * layer / ICD (stock vulkan.samsung.so) untouched.
 *
 * Phase 2 adds intercepts: on-disk pipeline cache, truthful extension
 * advertisement, compute BCn decode.
 *
 * License note: written from scratch against the public Vulkan-Loader
 * layer interface. No code was copied from ExynosTools (that repo ships
 * no license file — treat as unlicensed), from Mesa, or from any
 * vendor driver.
 */
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstdlib>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#define MX_LOG(fmt, ...)                                                       \
    __android_log_print(ANDROID_LOG_INFO, "MultiExynoos", fmt, ##__VA_ARGS__)
#else
#include <cstdio>
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
#define MX_VERSION_STR "0.0.11-dev"
#endif

/* Loader layer interface version we speak. v2 = current negotiate+
 * vkGetInstanceProcAddr(NULL, ...) semantics. */
static constexpr uint32_t kMaxLoaderLayerInterface = 2;

/* Forward declarations: the loader receives these entry points through the
 * negotiate struct, so they must be declared before it is filled in. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
mx_GetInstanceProcAddr(VkInstance instance, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
mx_GetDeviceProcAddr(VkDevice device, const char *name);

namespace {

PFN_vkGetInstanceProcAddr next_gipa = nullptr;
PFN_vkCreateInstance real_create_instance = nullptr;
PFN_vkGetInstanceProcAddr real_gipa = nullptr;
PFN_vkGetDeviceProcAddr real_gdpa = nullptr;

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

VKAPI_ATTR VkResult VKAPI_CALL mx_CreateInstance(const VkInstanceCreateInfo *ci,
                                                 const VkAllocationCallbacks *alloc,
                                                 VkInstance *out_instance) {
    const VkLayerInstanceCreateInfo *link = find_instance_link(ci);
    if (!link) {
        MX_LOG("vkCreateInstance without VK_LAYER_LINK_INFO");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;

    real_create_instance = reinterpret_cast<PFN_vkCreateInstance>(
        next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!real_create_instance)
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkResult r = real_create_instance(ci, alloc, out_instance);
    MX_LOG("vkCreateInstance -> %d  (MultiExynoos passthrough " MX_VERSION_STR ")",
           static_cast<int>(r));

    if (r == VK_SUCCESS) {
        real_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            next_gipa(*out_instance, "vkGetInstanceProcAddr"));
        real_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            next_gipa(*out_instance, "vkGetDeviceProcAddr"));
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
    v->pfnGetInstanceProcAddr = mx_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = mx_GetDeviceProcAddr;
    return VK_SUCCESS;
}

} // namespace

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
mx_GetInstanceProcAddr(VkInstance instance, const char *name) {
    if (!name)
        return nullptr;
    if (!std::strcmp(name, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(mx_GetInstanceProcAddr);
    if (!std::strcmp(name, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(mx_GetDeviceProcAddr);
    if (!std::strcmp(name, "vkNegotiateLoaderLayerInterfaceVersion"))
        return reinterpret_cast<PFN_vkVoidFunction>(
            mx_NegotiateLoaderLayerInterfaceVersion);
    if (!std::strcmp(name, "vkCreateInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(mx_CreateInstance);
    /* Pre-instance queries the loader makes for anything else belong to
     * the loader/ICD; after instance creation, forward down the chain. */
    if (instance == VK_NULL_HANDLE)
        return nullptr;
    return real_gipa ? real_gipa(instance, name) : nullptr;
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

} // extern "C"
