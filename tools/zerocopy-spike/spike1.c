// Phase 0 spike, step B: import a host page range as a VkBuffer (zero-copy),
// verify bytes CPU->GPU and GPU->CPU. Prints IMPORT-OK or the failing step.
#define VK_USE_PLATFORM_METAL_EXT
#define VK_ENABLE_BETA_EXTENSIONS
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(x, what)                                                             \
    do {                                                                           \
        VkResult r = (x);                                                          \
        if (r != VK_SUCCESS) {                                                     \
            printf("FAIL: %s -> %d\n", what, r);                                    \
            return 1;                                                              \
        }                                                                          \
    } while (0)

int main(void) {
    const size_t size = 16u << 20; // 16 MiB, page-aligned
    void* host = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (host == MAP_FAILED) {
        printf("FAIL: mmap\n");
        return 1;
    }
    memset(host, 0xAB, size);

    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = VK_API_VERSION_1_3};
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                          VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
                          VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app,
                                .enabledExtensionCount = 3,
                                .ppEnabledExtensionNames = exts,
                                .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR};
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");
    uint32_t ndev = 1;
    VkPhysicalDevice pdev;
    CHECK(vkEnumeratePhysicalDevices(inst, &ndev, &pdev), "enum phys");
    if (!ndev) {
        printf("FAIL: no device\n");
        return 1;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = 0,
                                  .queueCount = 1,
                                  .pQueuePriorities = &prio};
    const char* dexts[] = {"VK_KHR_swapchain", "VK_EXT_external_memory_host"};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qi,
                              .enabledExtensionCount = 2,
                              .ppEnabledExtensionNames = dexts};
    VkDevice dev;
    CHECK(vkCreateDevice(pdev, &dci, NULL, &dev), "vkCreateDevice+external_memory_host");

    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    printf("buffer memory requirements: size %llu alignment %llu\n",
           (unsigned long long)req.size, (unsigned long long)req.alignment);

    VkImportMemoryHostPointerInfoEXT import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = host};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = &import,
                               .allocationSize = req.size,
                               .memoryTypeIndex = 0};
    // Pick a host-visible memory type (MoltenVK: shared/system).
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
    int picked = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            picked = (int)i;
            break;
        }
    }
    if (picked < 0) {
        printf("FAIL: no host-visible memory type (bits 0x%x)\n", req.memoryTypeBits);
        return 1;
    }
    ai.memoryTypeIndex = (uint32_t)picked;
    printf("importing into memory type %d (flags 0x%x)\n", picked,
           mp.memoryTypes[picked].propertyFlags);
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &ai, NULL, &mem), "vkAllocateMemory(import host pointer)");
    CHECK(vkBindBufferMemory(dev, buf, mem, 0), "vkBindBufferMemory");

    // GPU-visible mapping check: map and compare with the host pattern.
    void* mapped = NULL;
    CHECK(vkMapMemory(dev, mem, 0, size, 0, &mapped), "vkMapMemory");
    unsigned long long sum = 0;
    for (size_t i = 0; i < size; i += 4096)
        sum += ((unsigned char*)mapped)[i];
    unsigned long long expect = (unsigned long long)0xAB * (size / 4096);
    printf("mapped check: sum 0x%llx expect 0x%llx %s\n", sum, expect,
           sum == expect ? "MATCH" : "MISMATCH");
    // Write through the mapping, verify through the original host pointer.
    memset(mapped, 0xCD, size);
    vkUnmapMemory(dev, mem);
    int ok = 1;
    for (size_t i = 0; i < size; i += 4096) {
        if (((unsigned char*)host)[i] != 0xCD) {
            ok = 0;
            break;
        }
    }
    printf("host-pointer check after GPU-side write: %s\n", ok ? "MATCH" : "MISMATCH");
    printf(ok && sum == expect ? "IMPORT-OK\n" : "IMPORT-FAILED\n");
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return 0;
}
