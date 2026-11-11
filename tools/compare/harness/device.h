#pragma once

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct HarnessDevice {
    VkPhysicalDevice handle;
    VkPhysicalDeviceProperties props;
};

static inline bool isCpuDevice(const HarnessDevice& d)
{
    return d.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
}

static inline HarnessDevice chooseDevice(VkInstance instance, const char* selectVar)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    if (!n) { fprintf(stderr, "no Vulkan device\n"); exit(1); }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance, &n, devs.data());
    const char* sel = getenv(selectVar);
    for (auto handle : devs) {
        HarnessDevice d{handle, {}};
        vkGetPhysicalDeviceProperties(handle, &d.props);
        if (sel ? strstr(d.props.deviceName, sel) != nullptr : !isCpuDevice(d)) return d;
    }
    if (sel) { fprintf(stderr, "%s=%s matches no Vulkan device\n", selectVar, sel); exit(1); }
    HarnessDevice d{devs[0], {}};
    vkGetPhysicalDeviceProperties(devs[0], &d.props);
    return d;
}

static inline void printDevice(const HarnessDevice& d)
{
    printf("%s%s\n", d.props.deviceName, isCpuDevice(d) ? " (cpu)" : "");
}

static inline void requireGpu(const HarnessDevice& d, const char* verboseVar)
{
    const char* allow = getenv("ALLOW_CPU");
    if (isCpuDevice(d) && !(allow && !strcmp(allow, "1"))) {
        fprintf(stderr, "device: %s is the CPU, not the GPU (set ALLOW_CPU=1 to run it anyway)\n", d.props.deviceName);
        exit(1);
    }
    if (isCpuDevice(d) || getenv(verboseVar))
        fprintf(stderr, "device: %s%s\n", d.props.deviceName, isCpuDevice(d) ? " (cpu)" : "");
}
