#include <vulkan/vulkan.h>
#include "lsfg-vk/lsfgvk.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkInstance       g_instance;
static VkPhysicalDevice g_phys;
static VkDevice         g_dev;
static VkQueue          g_queue;
static uint32_t         g_qfam;
static VkPhysicalDeviceMemoryProperties g_memProps;
static PFN_vkImportSemaphoreFdKHR g_importSemaphoreFd;

static uint32_t findMemType(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < g_memProps.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_memProps.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "no memory type\n");
    exit(1);
}

static std::string initVulkan()
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "lsfg_fi";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &g_instance));

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_instance, &n, nullptr);
    if (!n) { fprintf(stderr, "no Vulkan device\n"); exit(1); }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(g_instance, &n, devs.data());
    g_phys = VK_NULL_HANDLE;
    const char* sel = getenv("LSFG_FI_DEVICE");
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
        if (qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { g_qfam = i; break; }
    if (g_qfam == UINT32_MAX) { fprintf(stderr, "no graphics/compute queue\n"); exit(1); }

    const char* exts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f12;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = exts;
    VK_CHECK(vkCreateDevice(g_phys, &dci, nullptr, &g_dev));
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    g_importSemaphoreFd = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(g_dev, "vkImportSemaphoreFdKHR");
    if (!g_importSemaphoreFd) { fprintf(stderr, "vkImportSemaphoreFdKHR missing\n"); exit(1); }

    char id[16];
    snprintf(id, sizeof(id), "%04x:%04x", props.vendorID, props.deviceID);
    return id;
}

struct Image {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
};

static Image importImage(uint32_t w, uint32_t h, uint32_t layers, VkImageUsageFlags extraUsage, int fd)
{
    Image img;
    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.pNext = &ext;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extraUsage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(g_dev, &info, nullptr, &img.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_dev, img.image, &req);
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = img.image;
    VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    imp.pNext = &ded;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    imp.fd = fd;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &imp;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(g_dev, &ai, nullptr, &img.mem));
    VK_CHECK(vkBindImageMemory(g_dev, img.image, img.mem, 0));
    return img;
}

struct Buffer {
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void*          ptr = nullptr;
};

static Buffer createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags extra)
{
    Buffer b;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VK_CHECK(vkCreateBuffer(g_dev, &bi, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    const VkMemoryPropertyFlags base = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < g_memProps.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (g_memProps.memoryTypes[i].propertyFlags & (base | extra)) == (base | extra))
            type = i;
    ai.memoryTypeIndex = type != UINT32_MAX ? type : findMemType(req.memoryTypeBits, base);
    VK_CHECK(vkAllocateMemory(g_dev, &ai, nullptr, &b.mem));
    VK_CHECK(vkBindBufferMemory(g_dev, b.buf, b.mem, 0));
    VK_CHECK(vkMapMemory(g_dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.ptr));
    return b;
}

static void imageBarrier(VkCommandBuffer cb, VkImage img, uint32_t layer, VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                         VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static void usage()
{
    fprintf(stderr,
            "usage: lsfg_fi <width> <height> <in.rgba> <out.rgba> [--flow F] [--perf]\n"
            "               [--warmup N] [--no-fp16] [--dll PATH]\n"
            "  writes the t=0.5 frame for each input pair (i-1,i), i>=1: out has count-1 frames\n"
            "  --flow F    flow scale 0.25..1.0 (default 1.0)\n"
            "  --perf      performance mode (default off)\n"
            "  --warmup N  dispatches of frame 0 before the first pair (default 1; lsfg needs one\n"
            "              previous frame, the output of the first dispatch is discarded)\n"
            "  --no-fp16   disable fp16 shaders (default: allowed)\n"
            "  --dll PATH  lsfg-vk.dll (default: $LSFGVK_DLL_PATH)\n"
            "  env LSFG_FI_DEVICE=<substring of device name> selects the GPU\n");
    exit(1);
}

int main(int argc, char** argv)
{
    if (argc < 5) usage();
    const uint32_t W = (uint32_t)atoi(argv[1]);
    const uint32_t H = (uint32_t)atoi(argv[2]);
    const char* inPath = argv[3];
    const char* outPath = argv[4];
    float flow = 1.0f;
    bool perf = false;
    bool fp16 = true;
    int warmup = 1;
    const char* dll = getenv("LSFGVK_DLL_PATH");
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--flow") && i + 1 < argc) flow = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--perf")) perf = true;
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-fp16")) fp16 = false;
        else if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
        else usage();
    }
    if (!W || !H || warmup < 1 || flow < 0.25f || flow > 1.0f) usage();
    if (!dll || !*dll) { fprintf(stderr, "no DLL: pass --dll or set LSFGVK_DLL_PATH\n"); return 1; }

    const size_t frameBytes = (size_t)W * H * 4;
    FILE* fin = fopen(inPath, "rb");
    if (!fin) { perror(inPath); return 1; }
    fseek(fin, 0, SEEK_END);
    const long fileSize = ftell(fin);
    const int frameCount = (int)((size_t)fileSize / frameBytes);
    if (frameCount < 2) { fprintf(stderr, "need at least 2 frames of %ux%u\n", W, H); return 1; }
    if ((size_t)fileSize % frameBytes) fprintf(stderr, "warning: trailing bytes ignored\n");
    FILE* fout = fopen(outPath, "wb");
    if (!fout) { perror(outPath); return 1; }

    const std::string devId = initVulkan();
    fprintf(stderr, "lsfg: flow %.2f, perf %s, fp16 %s, warmup %d\n", flow, perf ? "on" : "off", fp16 ? "allowed" : "off", warmup);

    lsfgvk::Instance* lsfg = nullptr;
    lsfgvk::Context* ctx = nullptr;
    lsfgvk::FileDescriptors fds{};
    try {
        lsfg = new lsfgvk::Instance(devId, dll, fp16, false,
                                    getenv("LSFG_FI_VERBOSE") ? [](const std::string& m) { fprintf(stderr, "[lsfg] %s\n", m.c_str()); }
                                                              : std::function<void(const std::string&)>());
        ctx = new lsfgvk::Context(*lsfg, W, H, flow, perf);
        fds = ctx->exportFds();
    } catch (const std::exception& e) {
        fprintf(stderr, "lsfg-vk init failed: %s\n", e.what());
        if (strstr(e.what(), "DLL"))
            fprintf(stderr, "lsfg-vk 2.x needs lsfg-vk.dll (SPIR-V, shipped with Lossless Scaling), not Lossless.dll\n");
        return 1;
    }

    Image src = importImage(W, H, 2, VK_IMAGE_USAGE_TRANSFER_DST_BIT, fds.sourceFd);
    Image dst = importImage(W, H, 1, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, fds.destinationFd);

    VkSemaphoreTypeCreateInfo sti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &sti;
    VkSemaphore sync;
    VK_CHECK(vkCreateSemaphore(g_dev, &sci, nullptr, &sync));
    VkImportSemaphoreFdInfoKHR isi{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    isi.semaphore = sync;
    isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    isi.fd = fds.syncFd;
    VK_CHECK(g_importSemaphoreFd(g_dev, &isi));

    Buffer upload = createHostBuffer(frameBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0);
    Buffer readback = createHostBuffer(frameBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = g_qfam;
    VkCommandPool pool;
    VK_CHECK(vkCreateCommandPool(g_dev, &cpi, nullptr, &pool));
    VkCommandBuffer cbUp[2], cbRead;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 2;
    VK_CHECK(vkAllocateCommandBuffers(g_dev, &cai, cbUp));
    cai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(g_dev, &cai, &cbRead));

    VkBufferImageCopy region{};
    region.imageExtent = {W, H, 1};
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    for (uint32_t l = 0; l < 2; l++) {
        VK_CHECK(vkBeginCommandBuffer(cbUp[l], &cbi));
        imageBarrier(cbUp[l], src.image, l, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1};
        vkCmdCopyBufferToImage(cbUp[l], upload.buf, src.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        imageBarrier(cbUp[l], src.image, l, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        VK_CHECK(vkEndCommandBuffer(cbUp[l]));
    }
    VK_CHECK(vkBeginCommandBuffer(cbRead, &cbi));
    imageBarrier(cbRead, dst.image, 0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    vkCmdCopyImageToBuffer(cbRead, dst.image, VK_IMAGE_LAYOUT_GENERAL, readback.buf, 1, &region);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cbRead, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VK_CHECK(vkEndCommandBuffer(cbRead));

    VkFence fence;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(g_dev, &fci, nullptr, &fence));

    uint64_t iteration = 0;

    {
        const int steps = warmup + frameCount - 1;
        for (int step = 0; step < steps; step++) {
            const int frameIdx = step < warmup ? 0 : step - warmup + 1;
            const bool real = step >= warmup;
            const uint32_t layer = (uint32_t)(iteration % 2);
            const uint64_t startValue = 2 * iteration + 1;
            const uint64_t doneValue = 2 * iteration + 2;

            fseek(fin, (long)((size_t)frameIdx * frameBytes), SEEK_SET);
            if (fread(upload.ptr, 1, frameBytes, fin) != frameBytes) { fprintf(stderr, "read error\n"); return 1; }

            VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            tsi.signalSemaphoreValueCount = 1;
            tsi.pSignalSemaphoreValues = &startValue;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.pNext = &tsi;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cbUp[layer];
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &sync;
            VK_CHECK(vkQueueSubmit(g_queue, 1, &si, fence));
            VK_CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_MAX));
            VK_CHECK(vkResetFences(g_dev, 1, &fence));

            try {
                ctx->dispatch(1);
                ctx->acquire();
            } catch (const std::exception& e) {
                fprintf(stderr, "lsfg-vk dispatch failed: %s\n", e.what());
                return 1;
            }
            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &sync;
            wi.pValues = &doneValue;
            VK_CHECK(vkWaitSemaphores(g_dev, &wi, 5'000'000'000ull));
            iteration++;

            if (real) {
                VkSubmitInfo rsi{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                rsi.commandBufferCount = 1;
                rsi.pCommandBuffers = &cbRead;
                VK_CHECK(vkQueueSubmit(g_queue, 1, &rsi, fence));
                VK_CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_MAX));
                VK_CHECK(vkResetFences(g_dev, 1, &fence));
                if (fwrite(readback.ptr, 1, frameBytes, fout) != frameBytes) { fprintf(stderr, "write error\n"); return 1; }
            }
        }
    }
    fclose(fout);
    fclose(fin);

    VK_CHECK(vkDeviceWaitIdle(g_dev));
    delete ctx;
    delete lsfg;
    vkDestroyFence(g_dev, fence, nullptr);
    vkDestroyCommandPool(g_dev, pool, nullptr);
    for (Buffer* b : {&upload, &readback}) { vkDestroyBuffer(g_dev, b->buf, nullptr); vkFreeMemory(g_dev, b->mem, nullptr); }
    for (Image* i : {&src, &dst}) { vkDestroyImage(g_dev, i->image, nullptr); vkFreeMemory(g_dev, i->mem, nullptr); }
    vkDestroySemaphore(g_dev, sync, nullptr);
    vkDestroyDevice(g_dev, nullptr);
    vkDestroyInstance(g_instance, nullptr);
    return 0;
}
