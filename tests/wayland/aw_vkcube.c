/* N5b: cubo Vulkan giratorio sobre Wayland (RTX 4060): render pass con profundidad, pipeline con
 * shaders SPIR-V (tests/wayland/shaders), vertex buffer, push constants (MVP) y swapchain.
 * Uso: aw_vkcube [segundos] [fps_max]  -> imprime el FPS medio al terminar. */
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "shaders/cube_vert.h"
#include "shaders/cube_frag.h"

#define STEP(...) do { printf("[aw_vkcube] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { STEP("FAIL %s -> %d (linea %d)", #call, (int)r_, __LINE__); return 1; } } while (0)

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

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static int find_mem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
  return -1;
}

typedef struct { float m[16]; } mat4;
static mat4 mat_mul(mat4 a, mat4 b) { /* columna-mayor: r = a * b */
  mat4 r;
  for (int c = 0; c < 4; c++)
    for (int rr = 0; rr < 4; rr++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
      r.m[c * 4 + rr] = s;
    }
  return r;
}
static mat4 mat_id(void) { mat4 r = {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}}; return r; }
static mat4 mat_rot_y(float a) { mat4 r = mat_id(); r.m[0] = cosf(a); r.m[8] = sinf(a); r.m[2] = -sinf(a); r.m[10] = cosf(a); return r; }
static mat4 mat_rot_x(float a) { mat4 r = mat_id(); r.m[5] = cosf(a); r.m[9] = -sinf(a); r.m[6] = sinf(a); r.m[10] = cosf(a); return r; }
static mat4 mat_trans(float z) { mat4 r = mat_id(); r.m[14] = z; return r; }
static mat4 mat_persp(float fovy, float asp, float n, float f) { /* Vulkan: Y hacia abajo, z en [0,1] */
  float t = 1.0f / tanf(fovy / 2);
  mat4 r = {{0}};
  r.m[0] = t / asp; r.m[5] = -t; r.m[10] = f / (n - f); r.m[11] = -1; r.m[14] = (n * f) / (n - f);
  return r;
}

typedef struct { float p[3], c[3]; } Vtx;
static void add_face(Vtx *v, int *n, const float a[3], const float b[3], const float c[3], const float d[3], const float col[3]) {
  const float *q[6] = {a, b, c, a, c, d};
  for (int i = 0; i < 6; i++) { memcpy(v[*n].p, q[i], 12); memcpy(v[*n].c, col, 12); (*n)++; }
}

int main(int argc, char **argv) {
  double secs = argc > 1 ? atof(argv[1]) : 10, fps_max = argc > 2 ? atof(argv[2]) : 0;
  setvbuf(stdout, NULL, _IONBF, 0);
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
  xdg_toplevel_set_title(tl, "AW Vulkan Cube");
  xdg_toplevel_set_app_id(tl, "aurora.vkcube");
  wl_surface_commit(surf);
  wl_display_roundtrip(dpy);

  const char *iext[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
  VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "aw_vkcube", .apiVersion = VK_API_VERSION_1_1};
  VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext};
  VkInstance inst;
  CHECK(vkCreateInstance(&ici, NULL, &inst));
  uint32_t npd = 1;
  VkPhysicalDevice pd;
  VkResult er = vkEnumeratePhysicalDevices(inst, &npd, &pd);
  if (er != VK_SUCCESS && er != VK_INCOMPLETE) { STEP("FAIL enumerar dispositivos %d", (int)er); return 1; }
  VkPhysicalDeviceProperties pp;
  vkGetPhysicalDeviceProperties(pd, &pp);
  STEP("GPU: %s", pp.deviceName);
  float prio = 1.f;
  VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
  const char *dext[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                            .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext};
  VkDevice dev;
  CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
  VkQueue q;
  vkGetDeviceQueue(dev, 0, 0, &q);

  VkWaylandSurfaceCreateInfoKHR wsi = {.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, .display = dpy, .surface = surf};
  VkSurfaceKHR vs;
  CHECK(vkCreateWaylandSurfaceKHR(inst, &wsi, NULL, &vs));
  VkSurfaceCapabilitiesKHR caps;
  CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, vs, &caps));
  uint32_t nf = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(pd, vs, &nf, NULL);
  VkSurfaceFormatKHR fm[32];
  if (nf > 32) nf = 32;
  CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, vs, &nf, fm));
  uint32_t W = caps.currentExtent.width == 0xFFFFFFFFu ? 480 : caps.currentExtent.width;
  uint32_t H = caps.currentExtent.height == 0xFFFFFFFFu ? 360 : caps.currentExtent.height;
  VkFormat cfmt = fm[0].format;
  VkSwapchainCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vs,
      .minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount, .imageFormat = cfmt, .imageColorSpace = fm[0].colorSpace,
      .imageExtent = {W, H}, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
  VkSwapchainKHR sc;
  CHECK(vkCreateSwapchainKHR(dev, &sci, NULL, &sc));
  uint32_t nimg = 0;
  vkGetSwapchainImagesKHR(dev, sc, &nimg, NULL);
  VkImage imgs[8];
  if (nimg > 8) nimg = 8;
  CHECK(vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs));
  STEP("swapchain %ux%u formato=%d imagenes=%u", W, H, (int)cfmt, nimg);

  /* profundidad */
  VkImageCreateInfo dii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
      .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  VkImage dimg;
  CHECK(vkCreateImage(dev, &dii, NULL, &dimg));
  VkMemoryRequirements dmr;
  vkGetImageMemoryRequirements(dev, dimg, &dmr);
  int dmt = find_mem(pd, dmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkMemoryAllocateInfo dmai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = dmr.size, .memoryTypeIndex = (uint32_t)dmt};
  VkDeviceMemory dmem;
  CHECK(vkAllocateMemory(dev, &dmai, NULL, &dmem));
  CHECK(vkBindImageMemory(dev, dimg, dmem, 0));
  VkImageViewCreateInfo dvi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = dimg, .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_D32_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
  VkImageView dview;
  CHECK(vkCreateImageView(dev, &dvi, NULL, &dview));

  /* render pass */
  VkAttachmentDescription att[2] = {
    {.format = cfmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
     .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
     .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR},
    {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
     .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
     .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
  VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref, .pDepthStencilAttachment = &dref};
  VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = att, .subpassCount = 1, .pSubpasses = &sp};
  VkRenderPass rp;
  CHECK(vkCreateRenderPass(dev, &rpi, NULL, &rp));
  VkImageView cviews[8];
  VkFramebuffer fbs[8];
  for (uint32_t i = 0; i < nimg; i++) {
    VkImageViewCreateInfo cvi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = imgs[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = cfmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(dev, &cvi, NULL, &cviews[i]));
    VkImageView at[2] = {cviews[i], dview};
    VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 2, .pAttachments = at, .width = W, .height = H, .layers = 1};
    CHECK(vkCreateFramebuffer(dev, &fbi, NULL, &fbs[i]));
  }

  /* shaders + pipeline */
  VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(cube_vert_spv), .pCode = cube_vert_spv};
  VkShaderModule vsm, fsm;
  CHECK(vkCreateShaderModule(dev, &smi, NULL, &vsm));
  smi.codeSize = sizeof(cube_frag_spv); smi.pCode = cube_frag_spv;
  CHECK(vkCreateShaderModule(dev, &smi, NULL, &fsm));
  VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
  VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
  VkPipelineLayout pl;
  CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
  VkPipelineShaderStageCreateInfo stg[2] = {
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main"},
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main"}};
  VkVertexInputBindingDescription vb = {0, sizeof(Vtx), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}};
  VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &vb, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = va};
  VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
  VkRect2D sc2 = {{0, 0}, {W, H}};
  VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc2};
  VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.f};
  VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
  VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS};
  VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
  VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
  VkGraphicsPipelineCreateInfo gpi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stg, .pVertexInputState = &vi,
      .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
      .pColorBlendState = &cb, .layout = pl, .renderPass = rp, .subpass = 0};
  VkPipeline pipe;
  CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));
  STEP("pipeline grafica creada");

  /* geometria del cubo */
  Vtx verts[36];
  int nv = 0;
  const float P[8][3] = {{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
  const float C[6][3] = {{1,.25f,.2f},{.2f,1,.3f},{.25f,.4f,1},{1,.9f,.2f},{.9f,.3f,.9f},{.2f,.9f,.9f}};
  add_face(verts, &nv, P[0], P[1], P[2], P[3], C[0]);
  add_face(verts, &nv, P[5], P[4], P[7], P[6], C[1]);
  add_face(verts, &nv, P[4], P[0], P[3], P[7], C[2]);
  add_face(verts, &nv, P[1], P[5], P[6], P[2], C[3]);
  add_face(verts, &nv, P[3], P[2], P[6], P[7], C[4]);
  add_face(verts, &nv, P[4], P[5], P[1], P[0], C[5]);
  VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof(verts), .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
  VkBuffer vbuf;
  CHECK(vkCreateBuffer(dev, &bci, NULL, &vbuf));
  VkMemoryRequirements bmr;
  vkGetBufferMemoryRequirements(dev, vbuf, &bmr);
  int bmt = find_mem(pd, bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkMemoryAllocateInfo bmai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bmr.size, .memoryTypeIndex = (uint32_t)bmt};
  VkDeviceMemory bmem;
  CHECK(vkAllocateMemory(dev, &bmai, NULL, &bmem));
  CHECK(vkBindBufferMemory(dev, vbuf, bmem, 0));
  void *mp;
  CHECK(vkMapMemory(dev, bmem, 0, sizeof(verts), 0, &mp));
  memcpy(mp, verts, sizeof(verts));
  vkUnmapMemory(dev, bmem);

  VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
  VkCommandPool pool;
  CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
  VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
  VkCommandBuffer cmd;
  CHECK(vkAllocateCommandBuffers(dev, &cbi, &cmd));
  VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore acq, rel;
  CHECK(vkCreateSemaphore(dev, &semi, NULL, &acq));
  CHECK(vkCreateSemaphore(dev, &semi, NULL, &rel));
  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  CHECK(vkCreateFence(dev, &fci, NULL, &fence));

  double t0 = now(), tlast = t0, tstat = t0;
  int frames = 0, fstat = 0;
  while (now() - t0 < secs) {
    uint32_t idx = 0;
    VkResult ar = vkAcquireNextImageKHR(dev, sc, 3000000000ull, acq, VK_NULL_HANDLE, &idx);
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { STEP("FAIL acquire %d (frame %d)", (int)ar, frames); return 1; }
    float a = (float)(now() - t0);
    mat4 mvp = mat_mul(mat_persp(0.9f, (float)W / (float)H, 0.1f, 20.f), mat_mul(mat_trans(-5.f), mat_mul(mat_rot_x(a * 0.7f), mat_rot_y(a * 1.1f))));
    VkCommandBufferBeginInfo bb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &bb);
    VkClearValue cv[2] = {{.color = {{.08f, .09f, .12f, 1.f}}}, {.depthStencil = {1.f, 0}}};
    VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fbs[idx], .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 2, .pClearValues = cv};
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &off);
    vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp.m);
    vkCmdDraw(cmd, 36, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acq, .pWaitDstStageMask = &ws,
        .commandBufferCount = 1, .pCommandBuffers = &cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &rel};
    vkResetFences(dev, 1, &fence);
    VkResult sr = vkQueueSubmit(q, 1, &si, fence);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &rel, .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx};
    VkResult pr = vkQueuePresentKHR(q, &pi);
    if (sr != VK_SUCCESS || (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR)) { STEP("FAIL submit=%d present=%d (frame %d)", (int)sr, (int)pr, frames); return 1; }
    if (vkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ull) != VK_SUCCESS) { STEP("FAIL fence (frame %d)", frames); return 1; }
    frames++; fstat++;
    wl_display_flush(dpy);
    if (wl_display_prepare_read(dpy) == 0) {
      struct pollfd pf = {.fd = wl_display_get_fd(dpy), .events = POLLIN};
      if (poll(&pf, 1, 0) > 0) wl_display_read_events(dpy); else wl_display_cancel_read(dpy);
    }
    wl_display_dispatch_pending(dpy);
    double t = now();
    if (fps_max > 0) { double want = 1.0 / fps_max; if (t - tlast < want) usleep((useconds_t)((want - (t - tlast)) * 1e6)); tlast = now(); }
    if (t - tstat >= 2.0) { STEP("  %.1f s: %.1f fps", t - t0, fstat / (t - tstat)); tstat = t; fstat = 0; }
  }
  double el = now() - t0;
  STEP("ALL PASS: %d cuadros en %.1f s = %.1f fps medios", frames, el, frames / el);
  vkDeviceWaitIdle(dev);
  return 0;
}
