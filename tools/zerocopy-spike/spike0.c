// Phase 0 spike: can we import host memory into Vulkan (MoltenVK) zero-copy?
// Step A (this file): extension present? minImportedHostPointerAlignment?
// Step B (next): import a page-aligned range, verify bytes both directions.
#define VK_USE_PLATFORM_METAL_EXT
#define VK_ENABLE_BETA_EXTENSIONS
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
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
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL: vkCreateInstance\n");
        return 1;
    }
    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(inst, &ndev, NULL);
    VkPhysicalDevice* devs = malloc(ndev * sizeof *devs);
    vkEnumeratePhysicalDevices(inst, &ndev, devs);
    for (uint32_t d = 0; d < ndev; ++d) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[d], &props);
        printf("device %u: %s\n", d, props.deviceName);
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(devs[d], NULL, &ne, NULL);
        VkExtensionProperties* es = malloc(ne * sizeof *es);
        vkEnumerateDeviceExtensionProperties(devs[d], NULL, &ne, es);
        int host_mem = 0, metal_objs = 0;
        for (uint32_t i = 0; i < ne; ++i) {
            if (!strcmp(es[i].extensionName, "VK_EXT_external_memory_host"))
                host_mem = 1;
            if (!strcmp(es[i].extensionName, "VK_EXT_metal_objects"))
                metal_objs = 1;
        }
        printf("  VK_EXT_external_memory_host: %s\n", host_mem ? "YES" : "NO");
        printf("  VK_EXT_metal_objects: %s\n", metal_objs ? "YES" : "NO");
        if (host_mem) {
            VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 p2 = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp};
            vkGetPhysicalDeviceProperties2(devs[d], &p2);
            printf("  minImportedHostPointerAlignment: 0x%llx (%llu)\n",
                   (unsigned long long)hp.minImportedHostPointerAlignment,
                   (unsigned long long)hp.minImportedHostPointerAlignment);
        }
        free(es);
    }
    free(devs);
    vkDestroyInstance(inst, NULL);
    printf("SPIKE-A-DONE\n");
    return 0;
}
