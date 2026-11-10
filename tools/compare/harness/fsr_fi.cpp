#include <vulkan/vulkan.h>
#include <FidelityFX/host/ffx_interface.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
#define FFX_CHECK(x) do { FfxErrorCode e_ = (x); if (e_ != FFX_OK) { fprintf(stderr, "%s failed: 0x%x (line %d)\n", #x, (unsigned)e_, __LINE__); exit(1); } } while (0)

extern "C" FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const*)
{
    return FFX_ERROR_INVALID_ARGUMENT;
}

static VkInstance       g_instance;
static VkPhysicalDevice g_phys;
static VkDevice         g_dev;
static VkQueue          g_queue;
static uint32_t         g_qfam;
static VkPhysicalDeviceMemoryProperties g_memProps;

static uint32_t findMemType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0)
{
    for (uint32_t i = 0; i < g_memProps.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_memProps.memoryTypes[i].propertyFlags & want) == want &&
            !(g_memProps.memoryTypes[i].propertyFlags & avoid))
            return i;
    if (avoid) return findMemType(bits, want, 0);
    fprintf(stderr, "no memory type\n");
    exit(1);
}

struct Image {
    VkImage           image = VK_NULL_HANDLE;
    VkDeviceMemory    mem = VK_NULL_HANDLE;
    VkImageCreateInfo info{};
};

static Image createImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage)
{
    Image img;
    img.info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img.info.imageType = VK_IMAGE_TYPE_2D;
    img.info.format = fmt;
    img.info.extent = {w, h, 1};
    img.info.mipLevels = 1;
    img.info.arrayLayers = 1;
    img.info.samples = VK_SAMPLE_COUNT_1_BIT;
    img.info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img.info.usage = usage;
    img.info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img.info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(g_dev, &img.info, nullptr, &img.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_dev, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(g_dev, &ai, nullptr, &img.mem));
    VK_CHECK(vkBindImageMemory(g_dev, img.image, img.mem, 0));
    return img;
}

static void destroyImage(Image& img)
{
    vkDestroyImage(g_dev, img.image, nullptr);
    vkFreeMemory(g_dev, img.mem, nullptr);
}

struct Buffer {
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void*          ptr = nullptr;
};

static Buffer createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool readback)
{
    Buffer b;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    VK_CHECK(vkCreateBuffer(g_dev, &ci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, b.buf, &req);
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (readback) want |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    uint32_t type;
    bool found = false;
    for (uint32_t i = 0; i < g_memProps.memoryTypeCount && !found; i++)
        if ((req.memoryTypeBits & (1u << i)) && (g_memProps.memoryTypes[i].propertyFlags & want) == want) { type = i; found = true; }
    if (!found) type = findMemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VK_CHECK(vkAllocateMemory(g_dev, &ai, nullptr, &b.mem));
    VK_CHECK(vkBindBufferMemory(g_dev, b.buf, b.mem, 0));
    VK_CHECK(vkMapMemory(g_dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.ptr));
    return b;
}

static void destroyBuffer(Buffer& b)
{
    vkUnmapMemory(g_dev, b.mem);
    vkDestroyBuffer(g_dev, b.buf, nullptr);
    vkFreeMemory(g_dev, b.mem, nullptr);
}

static void imageBarrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                         VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static PFN_vkGetPhysicalDeviceMemoryProperties g_realGetMemProps;

static VKAPI_ATTR void VKAPI_CALL getMemPropsNoProtected(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties* props)
{
    g_realGetMemProps(pd, props);
    for (uint32_t i = 0; i < props->memoryTypeCount; i++)
        if (props->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT)
            props->memoryTypes[i].propertyFlags = 0;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ffxGetInstanceProcAddr(VkInstance instance, const char* name)
{
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties")) {
        g_realGetMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)vkGetInstanceProcAddr(instance, name);
        return (PFN_vkVoidFunction)getMemPropsNoProtected;
    }
    return vkGetInstanceProcAddr(instance, name);
}

static void ffxMessage(uint32_t type, const wchar_t* msg)
{
    fprintf(stderr, "[ffx %s] %ls\n", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", msg);
}

static bool hasExt(const std::vector<VkExtensionProperties>& exts, const char* name)
{
    for (auto& e : exts)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

static void initVulkan()
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "fsr_fi";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &g_instance));

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_instance, &n, nullptr);
    if (!n) { fprintf(stderr, "no Vulkan device\n"); exit(1); }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(g_instance, &n, devs.data());
    g_phys = VK_NULL_HANDLE;
    const char* sel = getenv("FSR_FI_DEVICE");
    for (auto d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (sel ? (strstr(p.deviceName, sel) != nullptr) : (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)) { g_phys = d; break; }
    }
    if (!g_phys) g_phys = devs[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_phys, &props);
    fprintf(stderr, "device: %s\n", props.deviceName);
    vkGetPhysicalDeviceMemoryProperties(g_phys, &g_memProps);

    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &n, qf.data());
    g_qfam = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++)
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { g_qfam = i; break; }
    if (g_qfam == UINT32_MAX) { fprintf(stderr, "no graphics+compute queue\n"); exit(1); }

    vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &n, exts.data());
    std::vector<const char*> enable;
    for (const char* e : {VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
                          VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                          VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME})
        if (hasExt(exts, e)) enable.push_back(e);

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(g_phys, &f2);
    f2.features.robustBufferAccess = VK_FALSE;
    f13.robustImageAccess = VK_FALSE;
    f11.protectedMemory = VK_FALSE;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)enable.size();
    dci.ppEnabledExtensionNames = enable.data();
    VK_CHECK(vkCreateDevice(g_phys, &dci, nullptr, &g_dev));
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
}

static void usage()
{
    fprintf(stderr,
            "usage: fsr_fi <width> <height> <in.rgba> <out.rgba> [--warmup N]\n"
            "  writes the t=0.5 frame for each input pair (i-1,i), i>=1: out has count-1 frames\n"
            "  --warmup N: dispatches of frame 0 before the first pair (default 10; FSR blends without optical flow for 10 frames after a reset)\n");
    exit(1);
}

int main(int argc, char** argv)
{
    if (argc < 5) usage();
    const uint32_t W = (uint32_t)atoi(argv[1]);
    const uint32_t H = (uint32_t)atoi(argv[2]);
    const char* inPath = argv[3];
    const char* outPath = argv[4];
    int warmup = 10;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = atoi(argv[++i]);
        else usage();
    }
    if (!W || !H || warmup < 1) usage();

    const size_t frameBytes = (size_t)W * H * 4;
    FILE* fin = fopen(inPath, "rb");
    if (!fin) { perror(inPath); return 1; }
    fseek(fin, 0, SEEK_END);
    const long fileSize = ftell(fin);
    const int frameCount = (int)(fileSize / (long)frameBytes);
    if (frameCount < 2) { fprintf(stderr, "need at least 2 frames of %ux%u\n", W, H); return 1; }
    if ((size_t)fileSize % frameBytes) fprintf(stderr, "warning: trailing bytes ignored\n");
    FILE* fout = fopen(outPath, "wb");
    if (!fout) { perror(outPath); return 1; }

    initVulkan();

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = g_qfam;
    VkCommandPool pool;
    VK_CHECK(vkCreateCommandPool(g_dev, &cpci, nullptr, &pool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(g_dev, &cbai, &cb));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK_CHECK(vkCreateFence(g_dev, &fci, nullptr, &fence));

    Image color = createImage(W, H, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Image output = createImage(W, H, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Image depth = createImage(W, H, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Image motion = createImage(W, H, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Buffer upload = createHostBuffer(frameBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    Buffer readback = createHostBuffer(frameBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);

    auto submitAndWait = [&]() {
        VK_CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(g_queue, 1, &si, fence));
        VK_CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(g_dev, 1, &fence));
    };
    auto beginCmd = [&]() {
        VK_CHECK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    };

    {
        beginCmd();
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        imageBarrier(cb, color.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, 0);
        imageBarrier(cb, output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0);
        for (Image* img : {&depth, &motion})
            imageBarrier(cb, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue farDepth{};
        farDepth.float32[0] = 1.0f;
        VkClearColorValue zeroMv{};
        vkCmdClearColorImage(cb, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &farDepth, 1, &range);
        vkCmdClearColorImage(cb, motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zeroMv, 1, &range);
        for (Image* img : {&depth, &motion})
            imageBarrier(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        submitAndWait();
    }

    VkDeviceContext devCtx{};
    devCtx.vkDevice = g_dev;
    devCtx.vkPhysicalDevice = g_phys;
    devCtx.vkDeviceProcAddr = vkGetDeviceProcAddr;
    devCtx.vkInstanceProcAddr = ffxGetInstanceProcAddr;
    devCtx.vkInstance = g_instance;
    FfxDevice ffxDevice = ffxGetDeviceVK(&devCtx);

    FfxInterface ifShared{}, ifFi{};
    const size_t scratchShared = ffxGetScratchMemorySizeVK(g_phys, 1);
    const size_t scratchFi = ffxGetScratchMemorySizeVK(g_phys, 2);
    void* scratchSharedMem = calloc(1, scratchShared);
    void* scratchFiMem = calloc(1, scratchFi);
    FFX_CHECK(ffxGetInterfaceVK(&ifShared, ffxDevice, scratchSharedMem, scratchShared, 1));
    FFX_CHECK(ffxGetInterfaceVK(&ifFi, ffxDevice, scratchFiMem, scratchFi, 2));
    FfxUInt32 sharedCtxId = 0;
    FFX_CHECK(ifShared.fpCreateBackendContext(&ifShared, FFX_EFFECT_SHAREDAPIBACKEND, nullptr, &sharedCtxId));
    ffxFrameInterpolationSetGlobalDebugMessage(ffxMessage, FFX_MESSAGE_TYPE_COUNT);

    FfxOpticalflowContextDescription ofDesc{};
    ofDesc.backendInterface = ifFi;
    ofDesc.resolution = {W, H};
    FfxOpticalflowContext* ofCtx = new FfxOpticalflowContext{};
    FFX_CHECK(ffxOpticalflowContextCreate(ofCtx, &ofDesc));

    FfxFrameInterpolationContextDescription fiDesc{};
    fiDesc.backendInterface = ifFi;
    fiDesc.flags = 0;
    fiDesc.maxRenderSize = {W, H};
    fiDesc.displaySize = {W, H};
    fiDesc.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    fiDesc.previousInterpolationSourceFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    FfxFrameInterpolationContext* fiCtx = new FfxFrameInterpolationContext{};
    FFX_CHECK(ffxFrameInterpolationContextCreate(fiCtx, &fiDesc));

    FfxOpticalflowSharedResourceDescriptions ofRes{};
    FFX_CHECK(ffxOpticalflowGetSharedResourceDescriptions(ofCtx, &ofRes));
    FfxResourceInternal ofVector{}, ofScd{};
    FFX_CHECK(ifShared.fpCreateResource(&ifShared, &ofRes.opticalFlowVector, sharedCtxId, &ofVector));
    FFX_CHECK(ifShared.fpCreateResource(&ifShared, &ofRes.opticalFlowSCD, sharedCtxId, &ofScd));
    const FfxDimensions2D ofBufferSize = {ofRes.opticalFlowVector.resourceDescription.width, ofRes.opticalFlowVector.resourceDescription.height};

    FfxFrameInterpolationSharedResourceDescriptions fiRes{};
    FFX_CHECK(ffxFrameInterpolationGetSharedResourceDescriptions(fiCtx, &fiRes));
    FfxResourceInternal dilatedDepth[2], dilatedMv[2], prevDepth[2];
    for (int i = 0; i < 2; i++) {
        wchar_t name[3][64];
        FfxCreateResourceDescription d = fiRes.dilatedDepth;
        swprintf(name[0], 64, L"%ls%d", d.name, i);
        d.name = name[0];
        FFX_CHECK(ifShared.fpCreateResource(&ifShared, &d, sharedCtxId, &dilatedDepth[i]));
        d = fiRes.dilatedMotionVectors;
        swprintf(name[1], 64, L"%ls%d", d.name, i);
        d.name = name[1];
        FFX_CHECK(ifShared.fpCreateResource(&ifShared, &d, sharedCtxId, &dilatedMv[i]));
        d = fiRes.reconstructedPrevNearestDepth;
        swprintf(name[2], 64, L"%ls%d", d.name, i);
        d.name = name[2];
        FFX_CHECK(ifShared.fpCreateResource(&ifShared, &d, sharedCtxId, &prevDepth[i]));
    }

    const FfxResource colorRes = ffxGetResourceVK((void*)color.image, ffxGetImageResourceDescriptionVK(color.image, color.info),
                                                  L"fsr_fi_color", FFX_RESOURCE_STATE_COMPUTE_READ);
    const FfxResource outputRes = ffxGetResourceVK((void*)output.image, ffxGetImageResourceDescriptionVK(output.image, output.info, FFX_RESOURCE_USAGE_UAV),
                                                   L"fsr_fi_output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    const FfxResource depthRes = ffxGetResourceVK((void*)depth.image, ffxGetImageResourceDescriptionVK(depth.image, depth.info),
                                                  L"fsr_fi_depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    const FfxResource motionRes = ffxGetResourceVK((void*)motion.image, ffxGetImageResourceDescriptionVK(motion.image, motion.info),
                                                   L"fsr_fi_motion", FFX_RESOURCE_STATE_COMPUTE_READ);

    const float cameraNear = 0.1f;
    const float cameraFar = 100.0f;
    const float cameraFovY = 1.0471976f;
    const float frameTimeMs = 1000.0f / 60.0f;

    uint64_t frameID = 0;
    int toggle = 0;

    {
        const int steps = warmup + frameCount - 1;
        for (int step = 0; step < steps; step++) {
            const int frameIdx = step < warmup ? 0 : step - warmup + 1;
            const bool real = step >= warmup;
            const bool reset = step == 0;

            fseek(fin, (long)((size_t)frameIdx * frameBytes), SEEK_SET);
            if (fread(upload.ptr, 1, frameBytes, fin) != frameBytes) { fprintf(stderr, "read error\n"); return 1; }

            beginCmd();
            imageBarrier(cb, color.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {W, H, 1};
            vkCmdCopyBufferToImage(cb, upload.buf, color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            imageBarrier(cb, color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);

            FfxOpticalflowDispatchDescription ofd{};
            ofd.commandList = ffxGetCommandListVK(cb);
            ofd.color = colorRes;
            ofd.opticalFlowVector = ifShared.fpGetResource(&ifShared, ofVector);
            ofd.opticalFlowSCD = ifShared.fpGetResource(&ifShared, ofScd);
            ofd.reset = reset;
            ofd.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
            ofd.minMaxLuminance = {0.0f, 1.0f};
            FFX_CHECK(ffxOpticalflowContextDispatch(ofCtx, &ofd));

            toggle ^= 1;
            FfxFrameInterpolationPrepareDescription prep{};
            prep.flags = 0;
            prep.commandList = ffxGetCommandListVK(cb);
            prep.renderSize = {W, H};
            prep.jitterOffset = {0.0f, 0.0f};
            prep.motionVectorScale = {(float)W, (float)H};
            prep.frameTimeDelta = frameTimeMs;
            prep.cameraNear = cameraNear;
            prep.cameraFar = cameraFar;
            prep.viewSpaceToMetersFactor = 1.0f;
            prep.cameraFovAngleVertical = cameraFovY;
            prep.depth = depthRes;
            prep.motionVectors = motionRes;
            prep.frameID = frameID;
            prep.dilatedDepth = ifShared.fpGetResource(&ifShared, dilatedDepth[toggle]);
            prep.dilatedMotionVectors = ifShared.fpGetResource(&ifShared, dilatedMv[toggle]);
            prep.reconstructedPrevDepth = ifShared.fpGetResource(&ifShared, prevDepth[toggle]);
            prep.cameraPosition[0] = prep.cameraPosition[1] = prep.cameraPosition[2] = 0.0f;
            prep.cameraUp[1] = 1.0f;
            prep.cameraRight[0] = 1.0f;
            prep.cameraForward[2] = -1.0f;
            FFX_CHECK(ffxFrameInterpolationPrepare(fiCtx, &prep));

            FfxFrameInterpolationDispatchDescription fid{};
            fid.flags = 0;
            fid.commandList = ffxGetCommandListVK(cb);
            fid.displaySize = {W, H};
            fid.renderSize = {W, H};
            fid.currentBackBuffer = colorRes;
            fid.output = outputRes;
            fid.interpolationRect = {0, 0, (int)W, (int)H};
            fid.opticalFlowVector = ofd.opticalFlowVector;
            fid.opticalFlowSceneChangeDetection = ofd.opticalFlowSCD;
            fid.opticalFlowBufferSize = ofBufferSize;
            fid.opticalFlowScale = {1.0f / W, 1.0f / H};
            fid.opticalFlowBlockSize = 8;
            fid.cameraNear = cameraNear;
            fid.cameraFar = cameraFar;
            fid.cameraFovAngleVertical = cameraFovY;
            fid.viewSpaceToMetersFactor = 1.0f;
            fid.frameTimeDelta = frameTimeMs;
            fid.reset = reset;
            fid.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
            fid.minMaxLuminance[0] = 0.0f;
            fid.minMaxLuminance[1] = 1.0f;
            fid.frameID = frameID;
            fid.dilatedDepth = prep.dilatedDepth;
            fid.dilatedMotionVectors = prep.dilatedMotionVectors;
            fid.reconstructedPrevDepth = prep.reconstructedPrevDepth;
            FFX_CHECK(ffxFrameInterpolationDispatch(fiCtx, &fid));
            frameID++;

            if (real) {
                imageBarrier(cb, output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                vkCmdCopyImageToBuffer(cb, output.image, VK_IMAGE_LAYOUT_GENERAL, readback.buf, 1, &region);
                VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            }
            imageBarrier(cb, output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            submitAndWait();

            if (real) {
                if (fwrite(readback.ptr, 1, frameBytes, fout) != frameBytes) { fprintf(stderr, "write error\n"); return 1; }
            }
        }
    }
    fclose(fout);
    fclose(fin);


    VK_CHECK(vkDeviceWaitIdle(g_dev));
    for (int i = 0; i < 2; i++) {
        ifShared.fpDestroyResource(&ifShared, dilatedDepth[i], sharedCtxId);
        ifShared.fpDestroyResource(&ifShared, dilatedMv[i], sharedCtxId);
        ifShared.fpDestroyResource(&ifShared, prevDepth[i], sharedCtxId);
    }
    ifShared.fpDestroyResource(&ifShared, ofVector, sharedCtxId);
    ifShared.fpDestroyResource(&ifShared, ofScd, sharedCtxId);
    ffxFrameInterpolationContextDestroy(fiCtx);
    ffxOpticalflowContextDestroy(ofCtx);
    ifShared.fpDestroyBackendContext(&ifShared, sharedCtxId);
    delete fiCtx;
    delete ofCtx;
    free(scratchSharedMem);
    free(scratchFiMem);

    destroyBuffer(upload);
    destroyBuffer(readback);
    for (Image* img : {&color, &output, &depth, &motion}) destroyImage(*img);
    vkDestroyFence(g_dev, fence, nullptr);
    vkDestroyCommandPool(g_dev, pool, nullptr);
    vkDestroyDevice(g_dev, nullptr);
    vkDestroyInstance(g_instance, nullptr);
    return 0;
}
