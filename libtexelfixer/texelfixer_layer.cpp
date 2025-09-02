#include <android/log.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vk_layer.h>
#include <cstring>
#include <cassert>

#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, "texelfixer", __VA_ARGS__)
#define VK_LAYER_EXPORT __attribute__((visibility("default")))

// -----------------------------------------------------------------------------
// Globals holding the “next” layer’s function pointers
static PFN_vkGetInstanceProcAddr  next_gipa = nullptr;
static PFN_vkGetDeviceProcAddr    next_gdpa = nullptr;
static PFN_vkGetPhysicalDeviceProperties2 next_vkGetPhysicalDeviceProperties2 = nullptr;

__attribute__((constructor))
static void texelfixer_ctor() {
    __android_log_print(ANDROID_LOG_INFO,"texelfixer","vulkan layer .so initialised");
}

#define TRACE(MSG, ...) \
    __android_log_print(ANDROID_LOG_INFO, "texelfixer", \
                        "%s: " MSG, __FUNCTION__, ##__VA_ARGS__)


// -----------------------------------------------------------------------------
// Layer ► Loader handshake
extern "C" VK_LAYER_EXPORT VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* iface)
{
    TRACE("called");
    __android_log_print(ANDROID_LOG_INFO,"texelfixer","assbuttface");
    ALOGI("vkNegotiateLoaderLayerInterfaceVersion: iface=%p", (void*)iface);
    if (!iface || iface->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;

    /* Tell the loader which interface version we implement */
    if (iface->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;

    /* hand our hooks to the loader */
    iface->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    iface->pfnGetDeviceProcAddr   = vkGetDeviceProcAddr;
    iface->pfnGetPhysicalDeviceProcAddr = nullptr;   // not used here
    ALOGI("negotiated loader interface v%u", iface->loaderLayerInterfaceVersion);
    return VK_SUCCESS;
}

// -----------------------------------------------------------------------------
// Our patched vkGetPhysicalDeviceProperties2
static VKAPI_ATTR void VKAPI_CALL
my_vkGetPhysicalDeviceProperties2(VkPhysicalDevice phy,
                                  VkPhysicalDeviceProperties2 *hdr)
{
    TRACE("called");
    ALOGI("my_vkGetPhysicalDeviceProperties2: begin");
    next_vkGetPhysicalDeviceProperties2(phy, hdr);            // call down first

    for (void* p = hdr->pNext; p; p = *reinterpret_cast<void**>(p)) {
        auto sType = *reinterpret_cast<VkStructureType*>(p);
        switch (sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES: {
            auto* t =
              reinterpret_cast<VkPhysicalDeviceTexelBufferAlignmentProperties*>(p);
            t->storageTexelBufferOffsetAlignmentBytes = 4;
            t->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
            t->uniformTexelBufferOffsetAlignmentBytes  = 4;
            t->uniformTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
            ALOGI("patched: TEXEL_BUFFER_ALIGNMENT_PROPERTIES");
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES: {
            auto* v13 = reinterpret_cast<VkPhysicalDeviceVulkan13Properties*>(p);
            v13->storageTexelBufferOffsetAlignmentBytes = 4;
            v13->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
            ALOGI("patched: VULKAN_1_3_PROPERTIES");
            break;
        }
        default: break;
        }
    }
    ALOGI("my_vkGetPhysicalDeviceProperties2: end");
}

// -----------------------------------------------------------------------------
// vkGetInstanceProcAddr  – decides what we expose
extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* name)
{
    TRACE("called");
    ALOGI("vkGetInstanceProcAddr(%s)", name ? name : "<null>");
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (!strcmp(name, "vkCreateInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance);

    if (!strcmp(name, "vkGetPhysicalDeviceProperties2") ||
        !strcmp(name, "vkGetPhysicalDeviceProperties2KHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(my_vkGetPhysicalDeviceProperties2);

    // otherwise ask the next layer / ICD
    return next_gipa ? next_gipa(instance, name) : nullptr;
}

// -----------------------------------------------------------------------------
// vkGetDeviceProcAddr  – trivial pass-through for this layer
extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* name)
{
    ALOGI("vkGetDeviceProcAddr(%s)", name ? name : "<null>");
    return next_gdpa ? next_gdpa(device, name) : nullptr;
}

// -----------------------------------------------------------------------------
// vkCreateInstance trampoline (sets up the chain)
extern "C" VK_LAYER_EXPORT VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo*  pCreateInfo,
                 const VkAllocationCallbacks* pAllocator,
                 VkInstance*                  pInstance)
{
    TRACE("called");
    ALOGI("vkCreateInstance: begin");
    // Walk pNext until we hit the loader’s link-info struct -------------------
    const VkLayerInstanceCreateInfo* chain =
        reinterpret_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext);
    while (chain && (chain->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                     chain->function != VK_LAYER_LINK_INFO))
        chain = reinterpret_cast<const VkLayerInstanceCreateInfo*>(chain->pNext);
    assert(chain && "Loader link info missing");

    auto* mutableChain = const_cast<VkLayerInstanceCreateInfo*>(chain);

    // First element in the link-info chain is the *next* vkGetInstanceProcAddr
    next_gipa = mutableChain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    // Advance the list head so that lower layers see the right tail
    mutableChain->u.pLayerInfo = mutableChain->u.pLayerInfo->pNext;

    // Grab the real vkCreateInstance & call it
    auto fpCreateInstance =
        reinterpret_cast<PFN_vkCreateInstance>(next_gipa(nullptr, "vkCreateInstance"));
    VkResult r = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (r != VK_SUCCESS) {
        ALOGI("vkCreateInstance: down-chain failed: %d", r);
        return r;
    }

    // Resolve the funcs we need from *this* instance
    next_vkGetPhysicalDeviceProperties2 =
    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        next_gipa(*pInstance, "vkGetPhysicalDeviceProperties2"));
    if (!next_vkGetPhysicalDeviceProperties2)
        next_vkGetPhysicalDeviceProperties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                next_gipa(*pInstance, "vkGetPhysicalDeviceProperties2KHR"));

    // Resolve vkGetDeviceProcAddr for later chains
    next_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            next_gipa(*pInstance, "vkGetDeviceProcAddr"));

    ALOGI("texelfixer layer active");
    return VK_SUCCESS;
}

static const VkLayerProperties g_props = {
    /* layerName          */ "VK_LAYER_VORTEK_texelfixer",
    /* specVersion        */ VK_MAKE_VERSION(1,3,0),
    /* implementationVersion */ 1,
    /* description        */ "Force 4-byte texel-buffer alignment on Adreno"
};

extern "C" VK_LAYER_EXPORT VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t* pCount,
                                   VkLayerProperties* pProps)
{
    if (!pCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pProps) { *pCount = 1; return VK_SUCCESS; }  // size query
    if (*pCount == 0) return VK_INCOMPLETE;
    *pProps = g_props;
    *pCount = 1;
    return VK_SUCCESS;
}

extern "C" VK_LAYER_EXPORT VkResult VKAPI_CALL
vkEnumerateDeviceLayerProperties(VkPhysicalDevice,
                                 uint32_t* pCount,
                                 VkLayerProperties* pProps)
{
    return vkEnumerateInstanceLayerProperties(pCount, pProps);
}

extern "C" VK_LAYER_EXPORT
VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char*, uint32_t* pCount, VkExtensionProperties* pProps)
{
    TRACE("called");
    *pCount = 0;             // the layer adds no instance-level extensions
    return VK_SUCCESS;
}

extern "C" VK_LAYER_EXPORT
VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice, const char*, uint32_t* pCount, VkExtensionProperties* pProps)
{
    TRACE("called");
    *pCount = 0;             // no device-level extensions either
    return VK_SUCCESS;
}
