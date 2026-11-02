/* N5b: prueba Vulkan sobre la RTX 4060 (ICD de NVIDIA via el loader).
 * Etapa 1: instancia, dispositivo, cola, buffer en memoria visible, vkCmdFillBuffer en la GPU y
 *          lectura del resultado desde la CPU. Etapa 2 (con Wayland): superficie + swapchain + 3 cuadros.
 * Cada llamada imprime su VkResult: un solo ciclo de prueba diagnostica. */
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define STEP(...) do { printf("[aw_vk] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(call) do { VkResult r_ = (call); STEP("%s -> %d", #call, (int)r_); if (r_ != VK_SUCCESS) { STEP("FAIL"); return 1; } } while (0)

static struct wl_compositor *g_comp;
static struct xdg_wm_base *g_wm;
static int g_configured;
static void reg_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
  (void)d;
  if (!strcmp(iface, "wl_compositor")) g_comp = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
  else if (!strcmp(iface, "xdg_wm_base")) g_wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};
static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener wm_l = {wm_ping};
static void xs_conf(void *d, struct xdg_surface *xs, uint32_t s) { (void)d; xdg_surface_ack_configure(xs, s); g_configured = 1; }
static const struct xdg_surface_listener xs_l = {xs_conf};
static void tl_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) { (void)d; (void)t; (void)w; (void)h; (void)s; }
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; exit(0); }
static const struct xdg_toplevel_listener tl_l = {tl_conf, tl_close, NULL, NULL};

static int find_mem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
  return -1;
}

int main(int argc, char **argv) {
  int want_wl = !(argc > 1 && !strcmp(argv[1], "--no-wayland"));
  setvbuf(stdout, NULL, _IONBF, 0);
  uint32_t ver = 0;
  vkEnumerateInstanceVersion(&ver);
  STEP("loader: instancia Vulkan %u.%u.%u", VK_VERSION_MAJOR(ver), VK_VERSION_MINOR(ver), VK_VERSION_PATCH(ver));
  uint32_t ne = 0;
  vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
  VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof(*ep));
  vkEnumerateInstanceExtensionProperties(NULL, &ne, ep);
  int has_surface = 0, has_wl = 0;
  for (uint32_t i = 0; i < ne; i++) {
    if (!strcmp(ep[i].extensionName, "VK_KHR_surface")) has_surface = 1;
    if (!strcmp(ep[i].extensionName, "VK_KHR_wayland_surface")) has_wl = 1;
  }
  STEP("%u extensiones de instancia; VK_KHR_surface=%d VK_KHR_wayland_surface=%d", ne, has_surface, has_wl);
  want_wl = want_wl && has_surface && has_wl;

  const char *iext[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
  VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "aw_vk_test",
                          .apiVersion = VK_API_VERSION_1_1};
  VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai,
                              .enabledExtensionCount = want_wl ? 2 : 0, .ppEnabledExtensionNames = iext};
  VkInstance inst;
  CHECK(vkCreateInstance(&ici, NULL, &inst));
  uint32_t npd = 0;
  CHECK(vkEnumeratePhysicalDevices(inst, &npd, NULL));
  STEP("dispositivos fisicos: %u", npd);
  if (!npd) { STEP("FAIL sin dispositivos (ICD no cargo o no encontro la GPU)"); return 1; }
  VkPhysicalDevice pds[8];
  if (npd > 8) npd = 8;
  CHECK(vkEnumeratePhysicalDevices(inst, &npd, pds));
  VkPhysicalDevice pd = pds[0];
  VkPhysicalDeviceProperties pp;
  vkGetPhysicalDeviceProperties(pd, &pp);
  STEP("GPU: %s tipo=%d api=%u.%u.%u driver=0x%x vendor=0x%x device=0x%x", pp.deviceName, pp.deviceType,
       VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion),
       pp.driverVersion, pp.vendorID, pp.deviceID);
  uint32_t nq = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
  VkQueueFamilyProperties qf[16];
  if (nq > 16) nq = 16;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
  int gq = -1;
  for (uint32_t i = 0; i < nq; i++) {
    STEP("  cola %u: flags=0x%x count=%u", i, qf[i].queueFlags, qf[i].queueCount);
    if (gq < 0 && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) gq = (int)i;
  }
  if (gq < 0) { STEP("FAIL sin cola grafica"); return 1; }

  float prio = 1.f;
  VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = (uint32_t)gq,
                                 .queueCount = 1, .pQueuePriorities = &prio};
  const char *dext[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                            .pQueueCreateInfos = &qci, .enabledExtensionCount = want_wl ? 1 : 0, .ppEnabledExtensionNames = dext};
  VkDevice dev;
  CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
  VkQueue q;
  vkGetDeviceQueue(dev, (uint32_t)gq, 0, &q);

  /* ---- etapa 1: la GPU escribe un buffer y la CPU lo lee */
  VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
  VkBuffer buf;
  CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(dev, buf, &mr);
  int mt = find_mem(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  STEP("memoria visible: tipo %d (size=%llu align=%llu)", mt, (unsigned long long)mr.size, (unsigned long long)mr.alignment);
  if (mt < 0) { STEP("FAIL sin memoria host-visible"); return 1; }
  VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt};
  VkDeviceMemory mem;
  CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
  CHECK(vkBindBufferMemory(dev, buf, mem, 0));
  VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = (uint32_t)gq,
                                 .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
  VkCommandPool pool;
  CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
  VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
  VkCommandBuffer cb;
  CHECK(vkAllocateCommandBuffers(dev, &cbi, &cb));
  VkCommandBufferBeginInfo cbb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHECK(vkBeginCommandBuffer(cb, &cbb));
  vkCmdFillBuffer(cb, buf, 0, 4096, 0xCAFEBABEu);
  CHECK(vkEndCommandBuffer(cb));
  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  CHECK(vkCreateFence(dev, &fci, NULL, &fence));
  VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
  CHECK(vkQueueSubmit(q, 1, &si, fence));
  VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
  STEP("vkWaitForFences(5 s) -> %d", (int)wr);
  if (wr != VK_SUCCESS) { STEP("FAIL la GPU no completo el trabajo"); return 1; }
  uint32_t *p = NULL;
  CHECK(vkMapMemory(dev, mem, 0, 4096, 0, (void **)&p));
  STEP("lectura CPU tras vkCmdFillBuffer: 0x%08x 0x%08x (esperado 0xcafebabe)", p[0], p[1023]);
  if (p[0] != 0xCAFEBABEu || p[1023] != 0xCAFEBABEu) { STEP("FAIL contenido distinto"); return 1; }
  STEP("etapa 1 (computo/transferencia en la GPU) OK");

  if (!want_wl) { STEP("ALL PASS (sin Wayland)"); return 0; }

  /* ---- etapa 2: superficie Wayland + swapchain */
  struct wl_display *dpy = wl_display_connect(NULL);
  if (!dpy) { STEP("FAIL wl_display_connect"); return 1; }
  struct wl_registry *reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(reg, &reg_l, NULL);
  wl_display_roundtrip(dpy);
  if (!g_comp || !g_wm) { STEP("FAIL globales"); return 1; }
  xdg_wm_base_add_listener(g_wm, &wm_l, NULL);
  struct wl_surface *surf = wl_compositor_create_surface(g_comp);
  struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g_wm, surf);
  xdg_surface_add_listener(xs, &xs_l, NULL);
  struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
  xdg_toplevel_add_listener(tl, &tl_l, NULL);
  xdg_toplevel_set_title(tl, "AW VK Test");
  xdg_toplevel_set_app_id(tl, "aurora.vktest");
  wl_surface_commit(surf);
  wl_display_roundtrip(dpy);
  VkWaylandSurfaceCreateInfoKHR wsi = {.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, .display = dpy, .surface = surf};
  VkSurfaceKHR vs;
  CHECK(vkCreateWaylandSurfaceKHR(inst, &wsi, NULL, &vs));
  VkBool32 sup = 0;
  CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, (uint32_t)gq, vs, &sup));
  STEP("soporte de presentacion en la cola %d: %u", gq, sup);
  VkSurfaceCapabilitiesKHR caps;
  CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, vs, &caps));
  STEP("caps: min=%u max=%u extent=%ux%u usage=0x%x", caps.minImageCount, caps.maxImageCount, caps.currentExtent.width,
       caps.currentExtent.height, caps.supportedUsageFlags);
  uint32_t nf = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(pd, vs, &nf, NULL);
  VkSurfaceFormatKHR fm[32];
  if (nf > 32) nf = 32;
  CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, vs, &nf, fm));
  for (uint32_t i = 0; i < nf && i < 4; i++) STEP("  formato %u: %d cs=%d", i, fm[i].format, fm[i].colorSpace);
  uint32_t w = caps.currentExtent.width == 0xFFFFFFFFu ? 320 : caps.currentExtent.width;
  uint32_t h = caps.currentExtent.height == 0xFFFFFFFFu ? 200 : caps.currentExtent.height;
  VkSwapchainCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vs,
      .minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount, .imageFormat = fm[0].format, .imageColorSpace = fm[0].colorSpace,
      .imageExtent = {w, h}, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
  VkSwapchainKHR sc;
  CHECK(vkCreateSwapchainKHR(dev, &sci, NULL, &sc));
  uint32_t nimg = 0;
  vkGetSwapchainImagesKHR(dev, sc, &nimg, NULL);
  VkImage imgs[8];
  if (nimg > 8) nimg = 8;
  CHECK(vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs));
  VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore acq, rel;
  CHECK(vkCreateSemaphore(dev, &semi, NULL, &acq));
  CHECK(vkCreateSemaphore(dev, &semi, NULL, &rel));
  static const float col[3][4] = {{1, .2f, .2f, 1}, {.2f, 1, .2f, 1}, {.8f, .3f, .9f, 1}};
  for (int f = 0; f < 3; f++) {
    uint32_t idx = 0;
    VkResult ar = vkAcquireNextImageKHR(dev, sc, 3000000000ull, acq, VK_NULL_HANDLE, &idx);
    STEP("frame %d: vkAcquireNextImageKHR -> %d idx=%u", f, (int)ar, idx);
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { STEP("FAIL acquire"); return 1; }
    vkResetCommandBuffer(cb, 0);
    vkBeginCommandBuffer(cb, &cbb);
    VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = imgs[idx], .subresourceRange = rng};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    VkClearColorValue cc;
    memcpy(cc.float32, col[f], sizeof(cc.float32));
    vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rng);
    VkImageMemoryBarrier b2 = b1;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = 0;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo s2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acq,
        .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &rel};
    vkResetFences(dev, 1, &fence);
    VkResult sr = vkQueueSubmit(q, 1, &s2, fence);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &rel,
        .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx};
    VkResult pr = vkQueuePresentKHR(q, &pi);
    STEP("  submit=%d present=%d", (int)sr, (int)pr);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ull);
    wl_display_flush(dpy);
    struct pollfd pf = {.fd = wl_display_get_fd(dpy), .events = POLLIN};
    if (wl_display_prepare_read(dpy) == 0) { if (poll(&pf, 1, 300) > 0) wl_display_read_events(dpy); else wl_display_cancel_read(dpy); }
    wl_display_dispatch_pending(dpy);
    sleep(1);
  }
  STEP("ALL PASS (3 cuadros presentados)");
  sleep(3);
  return 0;
}
