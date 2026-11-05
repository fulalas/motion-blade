#include "vk_pipeline_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

enum { PATH_BYTES = 1024 };

static const char CACHE_DIRECTORY[] = "motionblade";
static const char CACHE_FILE[] = "pipelines.bin";

static void make_directory(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static bool cache_root(char *root, size_t size)
{
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    return base && snprintf(root, size, "%s", base) < (int)size;
#else
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0]) {
        return snprintf(root, size, "%s", xdg) < (int)size;
    }
    const char *home = getenv("HOME");
    if (!home) {
        return false;
    }
    int length = snprintf(root, size, "%s/.cache", home);
    if (length < (int)size) {
        make_directory(root);
    }
    return length < (int)size;
#endif
}

static bool cache_path(char *path, size_t size)
{
    char root[PATH_BYTES];
    if (!cache_root(root, sizeof(root))) {
        return false;
    }
    char directory[PATH_BYTES];
    if (snprintf(directory, sizeof(directory), "%s/%s", root, CACHE_DIRECTORY) >= (int)sizeof(directory)) {
        return false;
    }
    make_directory(directory);
    return snprintf(path, size, "%s/%s", directory, CACHE_FILE) < (int)size;
}

static void *read_file(const char *path, size_t *bytes)
{
    *bytes = 0;
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    void *data = NULL;
    long length = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    if (length > 0 && fseek(file, 0, SEEK_SET) == 0) {
        data = malloc((size_t)length);
    }
    if (data && fread(data, 1, (size_t)length, file) == (size_t)length) {
        *bytes = (size_t)length;
    } else {
        free(data);
        data = NULL;
    }
    fclose(file);
    return data;
}

static bool matches_device(const mbvk_backend *vk, const void *data, size_t bytes)
{
    VkPipelineCacheHeaderVersionOne header;
    if (bytes < sizeof(header)) {
        return false;
    }
    memcpy(&header, data, sizeof(header));
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(vk->physical_device, &p);
    return header.headerSize >= sizeof(header) && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           header.vendorID == p.vendorID && header.deviceID == p.deviceID &&
           memcmp(header.pipelineCacheUUID, p.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

void pipeline_cache_open(mbvk_backend *vk)
{
    char path[PATH_BYTES];
    size_t bytes = 0;
    void *data = cache_path(path, sizeof(path)) ? read_file(path, &bytes) : NULL;
    if (data && !matches_device(vk, data, bytes)) {
        bytes = 0;
    }
    VkPipelineCacheCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, bytes, data};
    if (vkCreatePipelineCache(vk->device, &info, NULL, &vk->pipeline_cache) != VK_SUCCESS) {
        vk->pipeline_cache = VK_NULL_HANDLE;
    }
    vk->pipeline_cache_bytes = bytes;
    free(data);
}

static void write_file(const char *path, const void *data, size_t bytes)
{
    char temporary[PATH_BYTES + 8];
    snprintf(temporary, sizeof(temporary), "%s.part", path);
    FILE *file = fopen(temporary, "wb");
    if (!file) {
        return;
    }
    bool written = fwrite(data, 1, bytes, file) == bytes;
    written = fclose(file) == 0 && written;
#ifdef _WIN32
    remove(path);
#endif
    if (!written || rename(temporary, path) != 0) {
        remove(temporary);
    }
}

static void save_cache(const mbvk_backend *vk)
{
    size_t bytes = 0;
    if (vkGetPipelineCacheData(vk->device, vk->pipeline_cache, &bytes, NULL) != VK_SUCCESS ||
        bytes == vk->pipeline_cache_bytes) {
        return;
    }
    char path[PATH_BYTES];
    void *data = malloc(bytes);
    if (data && vkGetPipelineCacheData(vk->device, vk->pipeline_cache, &bytes, data) == VK_SUCCESS &&
        cache_path(path, sizeof(path))) {
        write_file(path, data, bytes);
    }
    free(data);
}

void pipeline_cache_close(mbvk_backend *vk)
{
    if (!vk->pipeline_cache) {
        return;
    }
    save_cache(vk);
    vkDestroyPipelineCache(vk->device, vk->pipeline_cache, NULL);
    vk->pipeline_cache = VK_NULL_HANDLE;
}
