// SPDX-License-Identifier: GPL-2.0-or-later
// lavapipe by itself, under the Vulkan loader the bundle ships (scripts/test-lavapipe.sh): test/vecadd.comp over N floats.
// The first of three layers that add the same two vectors: this one, test_m2v_compute.c (through the bridge, from
// vecadd.metallib) and test_bundle.mm (through the plugin). The lowest one that fails says where to look.
//
//   test_lavapipe_compute vecadd.spv          (VK_ICD_FILENAMES names the driver)
//
// N is not a multiple of the shader's 64 threads, so the dispatch has more invocations than there are sums: the shader
// stops at the length of the range bound as c, and the floats that follow that range in the buffer must keep their value.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define N 100000u
#define GUARD 64u
#define SENTINEL (-12345.0f)
#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "FAIL: %s returned %d\n", #call, (int)r_); return 1; } } while (0)

static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *p = n > 0 ? malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *len = p ? (size_t)n : 0;
    return p;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <vecadd.spv>\n", argv[0]); return 2; }
    size_t spv_len; uint32_t *spv = read_file(argv[1], &spv_len);
    if (!spv || spv_len % 4) { fprintf(stderr, "FAIL: %s is not a SPIR-V module\n", argv[1]); return 1; }

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));

    // the software device: with VK_ICD_FILENAMES naming lavapipe's driver it is the only one, and on a machine whose
    // loader lists others as well this is still a test of that one
    VkPhysicalDevice all[16], pd = VK_NULL_HANDLE; uint32_t count = 16;
    VkResult er = vkEnumeratePhysicalDevices(inst, &count, all);
    if (er != VK_SUCCESS && er != VK_INCOMPLETE) { fprintf(stderr, "FAIL: vkEnumeratePhysicalDevices returned %d\n", (int)er); return 1; }
    VkPhysicalDeviceProperties props;
    for (uint32_t i = 0; i < count && pd == VK_NULL_HANDLE; i++) {
        vkGetPhysicalDeviceProperties(all[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) pd = all[i];
    }
    if (pd == VK_NULL_HANDLE) {
        const char *icd = getenv("VK_ICD_FILENAMES");
        fprintf(stderr, "FAIL: no software device among the %u the loader lists (VK_ICD_FILENAMES=%s)\n", count, icd ? icd : "");
        return 1;
    }
    printf("Device: %s (Vulkan %u.%u.%u)\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    VkQueueFamilyProperties qf[16]; uint32_t nqf = 16, family = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qf);
    for (uint32_t i = 0; i < nqf && family == UINT32_MAX; i++)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) family = i;
    if (family == UINT32_MAX) { fprintf(stderr, "FAIL: the device has no compute queue\n"); return 1; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev; CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, family, 0, &queue);

    // one buffer: a, b, c (each N floats, at the device's storage buffer alignment), then the guard right after c's last float
    VkDeviceSize align = props.limits.minStorageBufferOffsetAlignment ? props.limits.minStorageBufferOffsetAlignment : 1;
    VkDeviceSize bytes = (VkDeviceSize)N * sizeof(float), stride = (bytes + align - 1) / align * align;
    VkDeviceSize total = 2 * stride + bytes + GUARD * sizeof(float);
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = total,
                               .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf; CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t type = UINT32_MAX, want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) type = i;
    if (type == UINT32_MAX) { fprintf(stderr, "FAIL: no host-visible memory for a storage buffer\n"); return 1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type };
    VkDeviceMemory mem; CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
    CHECK(vkBindBufferMemory(dev, buf, mem, 0));
    void *map; CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &map));
    float *a = map, *b = (float *)((char *)map + stride), *c = (float *)((char *)map + 2 * stride);
    for (uint32_t i = 0; i < N; i++) { a[i] = (float)i * 0.5f; b[i] = (float)(N - i) * 0.25f; }   // every sum is exact in a float
    for (uint32_t i = 0; i < N + GUARD; i++) c[i] = SENTINEL;

    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_len, .pCode = spv };
    VkShaderModule sm; CHECK(vkCreateShaderModule(dev, &smci, NULL, &sm));
    VkDescriptorSetLayoutBinding lb[3];
    for (uint32_t i = 0; i < 3; i++)
        lb[i] = (VkDescriptorSetLayoutBinding){ .binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                               .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = lb };
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = sm, .pName = "main" } };
    VkPipeline pipe; CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkDescriptorPoolSize dps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
    VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
                                         .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet ds; CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));
    VkDescriptorBufferInfo dbi[3]; VkWriteDescriptorSet w[3];
    for (uint32_t i = 0; i < 3; i++) {
        dbi[i] = (VkDescriptorBufferInfo){ .buffer = buf, .offset = i * stride, .range = bytes };
        w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = i,
                                       .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[i] };
    }
    vkUpdateDescriptorSets(dev, 3, w, 0, NULL);

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family };
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, (N + 63) / 64, 1, 1);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));

    uint32_t wrong = 0, first = 0, touched = 0;
    for (uint32_t i = 0; i < N; i++)
        if (c[i] != a[i] + b[i] && !wrong++) first = i;
    for (uint32_t i = N; i < N + GUARD; i++) touched += c[i] != SENTINEL;
    if (wrong) printf("FAIL: %u of %u sums wrong, the first at %u: %g + %g gave %g\n", wrong, N, first, a[first], b[first], c[first]);
    if (touched) printf("FAIL: %u of the %u floats after the range bound as c were written\n", touched, GUARD);
    if (!wrong && !touched) printf("PASS: %u sums right, the %u floats after them untouched (%u invocations for %u sums)\n",
                                   N, GUARD, (N + 63) / 64 * 64, N);

    vkDestroyCommandPool(dev, pool, NULL); vkDestroyDescriptorPool(dev, dp, NULL); vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL); vkDestroyDescriptorSetLayout(dev, dsl, NULL); vkDestroyShaderModule(dev, sm, NULL);
    vkUnmapMemory(dev, mem); vkDestroyBuffer(dev, buf, NULL); vkFreeMemory(dev, mem, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL); free(spv);
    return wrong || touched ? 1 : 0;
}
