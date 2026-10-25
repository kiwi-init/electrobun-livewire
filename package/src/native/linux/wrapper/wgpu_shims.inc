// ----------------------- WGPU Main-Thread Shims -----------------------

typedef void* (*PFN_wgpuInstanceCreateSurface)(void* instance, const void* descriptor);
typedef void (*PFN_wgpuSurfaceConfigure)(void* surface, const void* config);
typedef void (*PFN_wgpuSurfaceGetCurrentTexture)(void* surface, void* surfaceTexture);
typedef int32_t (*PFN_wgpuSurfacePresent)(void* surface);
typedef WGPUFuture (*PFN_wgpuQueueOnSubmittedWorkDone)(WGPUQueue queue, WGPUQueueWorkDoneCallbackInfo callbackInfo);
typedef WGPUFuture (*PFN_wgpuBufferMapAsync)(WGPUBuffer buffer, WGPUMapMode mode, size_t offset, size_t size, WGPUBufferMapCallbackInfo callbackInfo);
typedef WGPUWaitStatus (*PFN_wgpuInstanceWaitAny)(WGPUInstance instance, size_t futureCount, WGPUFutureWaitInfo* futures, uint64_t timeoutNS);
typedef void* (*PFN_wgpuBufferGetMappedRange)(WGPUBuffer buffer, size_t offset, size_t size);
typedef void* (*PFN_wgpuBufferGetConstMappedRange)(WGPUBuffer buffer, size_t offset, size_t size);
typedef void (*PFN_wgpuBufferUnmap)(WGPUBuffer buffer);

static void* wgpuLibHandle = nullptr;
static PFN_wgpuInstanceCreateSurface p_wgpuInstanceCreateSurface = nullptr;
static PFN_wgpuSurfaceConfigure p_wgpuSurfaceConfigure = nullptr;
static PFN_wgpuSurfaceGetCurrentTexture p_wgpuSurfaceGetCurrentTexture = nullptr;
static PFN_wgpuSurfacePresent p_wgpuSurfacePresent = nullptr;
static PFN_wgpuQueueOnSubmittedWorkDone p_wgpuQueueOnSubmittedWorkDone = nullptr;
static PFN_wgpuBufferMapAsync p_wgpuBufferMapAsync = nullptr;
static PFN_wgpuInstanceWaitAny p_wgpuInstanceWaitAny = nullptr;
static PFN_wgpuBufferGetMappedRange p_wgpuBufferGetMappedRange = nullptr;
static PFN_wgpuBufferGetConstMappedRange p_wgpuBufferGetConstMappedRange = nullptr;
static PFN_wgpuBufferUnmap p_wgpuBufferUnmap = nullptr;

static void* loadWgpuLibrary() {
    if (wgpuLibHandle) return wgpuLibHandle;
    std::string execDir = getExecutableDir();
    std::vector<std::string> candidates = {
        execDir + "/libwebgpu_dawn.so",
        execDir + "/../Resources/libwebgpu_dawn.so",
    };
    for (const auto& path : candidates) {
        wgpuLibHandle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (wgpuLibHandle) break;
    }
    if (!wgpuLibHandle) {
        wgpuLibHandle = dlopen("libwebgpu_dawn.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!wgpuLibHandle) {
        fprintf(stderr, "WGPU: failed to load libwebgpu_dawn.so: %s\n", dlerror());
    }
    return wgpuLibHandle;
}

static bool ensureWgpuSymbols() {
    if (p_wgpuInstanceCreateSurface && p_wgpuSurfaceConfigure && p_wgpuSurfaceGetCurrentTexture && p_wgpuSurfacePresent
        && p_wgpuQueueOnSubmittedWorkDone && p_wgpuBufferMapAsync && p_wgpuInstanceWaitAny
        && p_wgpuBufferGetMappedRange && p_wgpuBufferUnmap) {
        return true;
    }
    void* handle = loadWgpuLibrary();
    if (!handle) return false;
    p_wgpuInstanceCreateSurface = (PFN_wgpuInstanceCreateSurface)dlsym(handle, "wgpuInstanceCreateSurface");
    p_wgpuSurfaceConfigure = (PFN_wgpuSurfaceConfigure)dlsym(handle, "wgpuSurfaceConfigure");
    p_wgpuSurfaceGetCurrentTexture = (PFN_wgpuSurfaceGetCurrentTexture)dlsym(handle, "wgpuSurfaceGetCurrentTexture");
    p_wgpuSurfacePresent = (PFN_wgpuSurfacePresent)dlsym(handle, "wgpuSurfacePresent");
    p_wgpuQueueOnSubmittedWorkDone = (PFN_wgpuQueueOnSubmittedWorkDone)dlsym(handle, "wgpuQueueOnSubmittedWorkDone");
    p_wgpuBufferMapAsync = (PFN_wgpuBufferMapAsync)dlsym(handle, "wgpuBufferMapAsync");
    p_wgpuInstanceWaitAny = (PFN_wgpuInstanceWaitAny)dlsym(handle, "wgpuInstanceWaitAny");
    p_wgpuBufferGetMappedRange = (PFN_wgpuBufferGetMappedRange)dlsym(handle, "wgpuBufferGetMappedRange");
    p_wgpuBufferGetConstMappedRange = (PFN_wgpuBufferGetConstMappedRange)dlsym(handle, "wgpuBufferGetConstMappedRange");
    p_wgpuBufferUnmap = (PFN_wgpuBufferUnmap)dlsym(handle, "wgpuBufferUnmap");
    if (!p_wgpuInstanceCreateSurface || !p_wgpuSurfaceConfigure || !p_wgpuSurfaceGetCurrentTexture || !p_wgpuSurfacePresent
        || !p_wgpuQueueOnSubmittedWorkDone || !p_wgpuBufferMapAsync || !p_wgpuInstanceWaitAny
        || !p_wgpuBufferGetMappedRange || !p_wgpuBufferUnmap) {
        fprintf(stderr, "WGPU: missing symbols (create=%p configure=%p getTexture=%p present=%p)\n",
                p_wgpuInstanceCreateSurface, p_wgpuSurfaceConfigure, p_wgpuSurfaceGetCurrentTexture, p_wgpuSurfacePresent);
        return false;
    }
    return true;
}

static void wgpu_log(const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    fprintf(stderr, "[WGPU] %s\n", buffer);
    fflush(stderr);
}

typedef WGPUInstance (*PFN_wgpuCreateInstance)(WGPUInstanceDescriptor const* descriptor);
typedef WGPUFuture (*PFN_wgpuInstanceRequestAdapter)(WGPUInstance instance, WGPURequestAdapterOptions const* options, WGPURequestAdapterCallbackInfo callbackInfo);
typedef WGPUFuture (*PFN_wgpuAdapterRequestDevice)(WGPUAdapter adapter, WGPUDeviceDescriptor const* descriptor, WGPURequestDeviceCallbackInfo callbackInfo);
typedef WGPUQueue (*PFN_wgpuDeviceGetQueue)(WGPUDevice device);
typedef void (*PFN_wgpuSurfaceGetCapabilities2)(WGPUSurface surface, WGPUAdapter adapter, WGPUSurfaceCapabilities* capabilities);
typedef void (*PFN_wgpuSurfaceCapabilitiesFreeMembers2)(WGPUSurfaceCapabilities capabilities);
typedef WGPUShaderModule (*PFN_wgpuDeviceCreateShaderModule)(WGPUDevice device, WGPUShaderModuleDescriptor const* descriptor);
typedef WGPURenderPipeline (*PFN_wgpuDeviceCreateRenderPipeline)(WGPUDevice device, WGPURenderPipelineDescriptor const* descriptor);
typedef void (*PFN_wgpuDeviceSetLabel)(WGPUDevice device, WGPUStringView label);
typedef WGPUBuffer (*PFN_wgpuDeviceCreateBuffer)(WGPUDevice device, WGPUBufferDescriptor const* descriptor);
typedef void (*PFN_wgpuQueueWriteBuffer)(WGPUQueue queue, WGPUBuffer buffer, uint64_t bufferOffset, void const* data, size_t size);
typedef WGPUCommandEncoder (*PFN_wgpuDeviceCreateCommandEncoder)(WGPUDevice device, WGPUCommandEncoderDescriptor const* descriptor);
typedef WGPURenderPassEncoder (*PFN_wgpuCommandEncoderBeginRenderPass)(WGPUCommandEncoder encoder, WGPURenderPassDescriptor const* descriptor);
typedef void (*PFN_wgpuRenderPassEncoderSetPipeline)(WGPURenderPassEncoder pass, WGPURenderPipeline pipeline);
typedef void (*PFN_wgpuRenderPassEncoderSetVertexBuffer)(WGPURenderPassEncoder pass, uint32_t slot, WGPUBuffer buffer, uint64_t offset, uint64_t size);
typedef void (*PFN_wgpuRenderPassEncoderDraw)(WGPURenderPassEncoder pass, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
typedef void (*PFN_wgpuRenderPassEncoderEnd)(WGPURenderPassEncoder pass);
typedef WGPUCommandBuffer (*PFN_wgpuCommandEncoderFinish)(WGPUCommandEncoder encoder, WGPUCommandBufferDescriptor const* descriptor);
typedef void (*PFN_wgpuQueueSubmit)(WGPUQueue queue, size_t commandCount, WGPUCommandBuffer const* commands);
typedef WGPUTextureView (*PFN_wgpuTextureCreateView)(WGPUTexture texture, WGPUTextureViewDescriptor const* descriptor);
typedef void (*PFN_wgpuTextureViewRelease)(WGPUTextureView view);
typedef void (*PFN_wgpuTextureRelease)(WGPUTexture texture);
typedef void (*PFN_wgpuCommandBufferRelease)(WGPUCommandBuffer buffer);
typedef void (*PFN_wgpuCommandEncoderRelease)(WGPUCommandEncoder encoder);

static PFN_wgpuCreateInstance p_wgpuCreateInstance = nullptr;
static PFN_wgpuInstanceRequestAdapter p_wgpuInstanceRequestAdapter = nullptr;
static PFN_wgpuAdapterRequestDevice p_wgpuAdapterRequestDevice = nullptr;
static PFN_wgpuDeviceGetQueue p_wgpuDeviceGetQueue = nullptr;
static PFN_wgpuSurfaceGetCapabilities2 p_wgpuSurfaceGetCapabilities = nullptr;
static PFN_wgpuSurfaceCapabilitiesFreeMembers2 p_wgpuSurfaceCapabilitiesFreeMembers = nullptr;
static PFN_wgpuDeviceCreateShaderModule p_wgpuDeviceCreateShaderModule = nullptr;
static PFN_wgpuDeviceCreateRenderPipeline p_wgpuDeviceCreateRenderPipeline = nullptr;
static PFN_wgpuDeviceSetLabel p_wgpuDeviceSetLabel = nullptr;
static PFN_wgpuDeviceCreateBuffer p_wgpuDeviceCreateBuffer = nullptr;
static PFN_wgpuQueueWriteBuffer p_wgpuQueueWriteBuffer = nullptr;
static PFN_wgpuDeviceCreateCommandEncoder p_wgpuDeviceCreateCommandEncoder = nullptr;
static PFN_wgpuCommandEncoderBeginRenderPass p_wgpuCommandEncoderBeginRenderPass = nullptr;
static PFN_wgpuRenderPassEncoderSetPipeline p_wgpuRenderPassEncoderSetPipeline = nullptr;
static PFN_wgpuRenderPassEncoderSetVertexBuffer p_wgpuRenderPassEncoderSetVertexBuffer = nullptr;
static PFN_wgpuRenderPassEncoderDraw p_wgpuRenderPassEncoderDraw = nullptr;
static PFN_wgpuRenderPassEncoderEnd p_wgpuRenderPassEncoderEnd = nullptr;
static PFN_wgpuCommandEncoderFinish p_wgpuCommandEncoderFinish = nullptr;
static PFN_wgpuQueueSubmit p_wgpuQueueSubmit = nullptr;
static PFN_wgpuTextureCreateView p_wgpuTextureCreateView = nullptr;
static PFN_wgpuTextureViewRelease p_wgpuTextureViewRelease = nullptr;
static PFN_wgpuTextureRelease p_wgpuTextureRelease = nullptr;
static PFN_wgpuCommandBufferRelease p_wgpuCommandBufferRelease = nullptr;
static PFN_wgpuCommandEncoderRelease p_wgpuCommandEncoderRelease = nullptr;

static bool ensureWgpuTestSymbols() {
    if (!ensureWgpuSymbols()) return false;
    void* handle = loadWgpuLibrary();
    if (!handle) return false;
#define LOAD_TEST_SYM(name) \
    p_##name = (decltype(p_##name))dlsym(handle, #name); \
    if (!p_##name) { \
        wgpu_log("missing symbol: " #name); \
        return false; \
    }
    LOAD_TEST_SYM(wgpuCreateInstance);
    LOAD_TEST_SYM(wgpuInstanceRequestAdapter);
    LOAD_TEST_SYM(wgpuAdapterRequestDevice);
    LOAD_TEST_SYM(wgpuDeviceGetQueue);
    LOAD_TEST_SYM(wgpuSurfaceGetCapabilities);
    LOAD_TEST_SYM(wgpuSurfaceCapabilitiesFreeMembers);
    LOAD_TEST_SYM(wgpuDeviceCreateShaderModule);
    LOAD_TEST_SYM(wgpuDeviceCreateRenderPipeline);
    LOAD_TEST_SYM(wgpuDeviceSetLabel);
    LOAD_TEST_SYM(wgpuDeviceCreateBuffer);
    LOAD_TEST_SYM(wgpuQueueWriteBuffer);
    LOAD_TEST_SYM(wgpuDeviceCreateCommandEncoder);
    LOAD_TEST_SYM(wgpuCommandEncoderBeginRenderPass);
    LOAD_TEST_SYM(wgpuRenderPassEncoderSetPipeline);
    LOAD_TEST_SYM(wgpuRenderPassEncoderSetVertexBuffer);
    LOAD_TEST_SYM(wgpuRenderPassEncoderDraw);
    LOAD_TEST_SYM(wgpuRenderPassEncoderEnd);
    LOAD_TEST_SYM(wgpuCommandEncoderFinish);
    LOAD_TEST_SYM(wgpuQueueSubmit);
    LOAD_TEST_SYM(wgpuTextureCreateView);
    LOAD_TEST_SYM(wgpuTextureViewRelease);
    LOAD_TEST_SYM(wgpuTextureRelease);
    LOAD_TEST_SYM(wgpuCommandBufferRelease);
    LOAD_TEST_SYM(wgpuCommandEncoderRelease);
#undef LOAD_TEST_SYM
    return true;
}

ELECTROBUN_EXPORT void wgpuSurfaceCapabilitiesFreeMembersShim(void* capabilitiesPtr) {
    if (!capabilitiesPtr || !ensureWgpuTestSymbols()) return;
    WGPUSurfaceCapabilities* capabilities = (WGPUSurfaceCapabilities*)capabilitiesPtr;
    p_wgpuSurfaceCapabilitiesFreeMembers(*capabilities);
    *capabilities = {};
}

struct GPUTestState {
    WGPUInstance instance = nullptr;
    WGPUSurface surface = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPURenderPipeline pipelineA = nullptr;
    WGPURenderPipeline pipelineB = nullptr;
    WGPUBuffer vertexBuffer = nullptr;
    WGPUTextureFormat surfaceFormat = WGPUTextureFormat_BGRA8Unorm;
    WGPUCompositeAlphaMode alphaMode = WGPUCompositeAlphaMode_Opaque;
    Display* display = nullptr;
    Window window = 0;
    guint timerId = 0;
    float angle = 0.0f;
    uint32_t lastWidth = 0;
    uint32_t lastHeight = 0;
    bool surfaceConfigured = false;
    bool useAlt = false;
    bool running = false;
    WGPUViewImpl* view = nullptr;
};

static GPUTestState g_gpuTest;

static const float kCubeVertices[] = {
    -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f,
    -0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f,
    -0.5f,-0.5f,-0.5f, -0.5f, 0.5f,-0.5f,  0.5f, 0.5f,-0.5f,
    -0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f,-0.5f,-0.5f,
    -0.5f,-0.5f,-0.5f, -0.5f,-0.5f, 0.5f, -0.5f, 0.5f, 0.5f,
    -0.5f,-0.5f,-0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f,-0.5f,
     0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f, 0.5f, 0.5f,
     0.5f,-0.5f,-0.5f,  0.5f, 0.5f, 0.5f,  0.5f,-0.5f, 0.5f,
    -0.5f, 0.5f,-0.5f, -0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,
    -0.5f, 0.5f,-0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f,-0.5f,
    -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f,
    -0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f, -0.5f,-0.5f, 0.5f,
};

static constexpr size_t kCubeFloatCount = sizeof(kCubeVertices) / sizeof(float);
static constexpr size_t kCubeVertexCount = kCubeFloatCount / 3;
static constexpr size_t kGpuTestStrideFloats = 7;

static void buildRotatedVertices(float angle, float* out, size_t count) {
    const float sinY = sinf(angle);
    const float cosY = cosf(angle);
    const float sinX = sinf(angle * 0.7f);
    const float cosX = cosf(angle * 0.7f);
    for (size_t i = 0; i < count; i += 3) {
        float x = kCubeVertices[i];
        float y = kCubeVertices[i + 1];
        float z = kCubeVertices[i + 2];
        float x1 = x * cosY + z * sinY;
        float z1 = -x * sinY + z * cosY;
        float y1 = y * cosX - z1 * sinX;
        float z2 = y * sinX + z1 * cosX;
        float depth = z2 + 2.5f;
        float proj = 1.2f / depth;
        out[i] = x1 * proj;
        out[i + 1] = y1 * proj;
        out[i + 2] = 0.0f;
    }
}

static float clamp01f(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static void gpuTestGetMouseState(GPUTestState* state, float* outX, float* outY, float* outDown) {
    if (outX) *outX = 0.5f;
    if (outY) *outY = 0.5f;
    if (outDown) *outDown = 0.0f;
    if (!state || !state->display || !state->window) return;

    Window root = 0;
    Window child = 0;
    int rootX = 0;
    int rootY = 0;
    int winX = 0;
    int winY = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(state->display, state->window, &root, &child, &rootX, &rootY, &winX, &winY, &mask)) {
        return;
    }

    const uint32_t width = std::max(1u, state->lastWidth);
    const uint32_t height = std::max(1u, state->lastHeight);
    if (outX) *outX = clamp01f((float)winX / (float)width);
    if (outY) *outY = clamp01f((float)winY / (float)height);
    if (outDown) *outDown = (mask & Button1Mask) ? 1.0f : 0.0f;
}

static void buildInterleavedVertices(
    float angle,
    float mouseX,
    float mouseY,
    float mouseDown,
    float timeValue,
    float* out
) {
    float positions[kCubeFloatCount];
    buildRotatedVertices(angle, positions, kCubeFloatCount);
    for (size_t vertexIndex = 0; vertexIndex < kCubeVertexCount; vertexIndex++) {
        const size_t positionIndex = vertexIndex * 3;
        const size_t outputIndex = vertexIndex * kGpuTestStrideFloats;
        out[outputIndex] = positions[positionIndex];
        out[outputIndex + 1] = positions[positionIndex + 1];
        out[outputIndex + 2] = positions[positionIndex + 2];
        out[outputIndex + 3] = mouseX;
        out[outputIndex + 4] = mouseY;
        out[outputIndex + 5] = mouseDown;
        out[outputIndex + 6] = timeValue;
    }
}

static bool gpuTestGetWindowSize(Display* display, Window window, uint32_t* outWidth, uint32_t* outHeight) {
    XWindowAttributes attrs;
    if (!x11GetWindowAttributesSafe(display, window, &attrs)) {
        return false;
    }
    if (outWidth) *outWidth = (uint32_t)std::max(1, attrs.width);
    if (outHeight) *outHeight = (uint32_t)std::max(1, attrs.height);
    return true;
}

void stopWgpuTestForWindow(Window window) {
    if (window && g_gpuTest.window != window) return;
    if (g_gpuTest.timerId) {
        g_source_remove(g_gpuTest.timerId);
        g_gpuTest.timerId = 0;
    }
    g_gpuTest.running = false;
    if (!window || g_gpuTest.window == window) {
        g_gpuTest.window = 0;
        g_gpuTest.display = nullptr;
        g_gpuTest.view = nullptr;
    }
}

static void gpuTestConfigureSurface(GPUTestState* state) {
    if (!state || !state->surface || !state->device || !state->display || !state->window) return;
    state->surfaceConfigured = false;

    WGPUSurfaceCapabilities caps = {};
    p_wgpuSurfaceGetCapabilities(state->surface, state->adapter, &caps);
    if (caps.formatCount > 0 && caps.formats) {
        state->surfaceFormat = caps.formats[0];
    }
    if (caps.alphaModeCount > 0 && caps.alphaModes) {
        state->alphaMode = caps.alphaModes[0];
        for (size_t i = 0; i < caps.alphaModeCount; i++) {
            if (caps.alphaModes[i] == WGPUCompositeAlphaMode_Opaque) {
                state->alphaMode = WGPUCompositeAlphaMode_Opaque;
                break;
            }
        }
    }
    p_wgpuSurfaceCapabilitiesFreeMembers(caps);

    uint32_t width = 1;
    uint32_t height = 1;
    if (!gpuTestGetWindowSize(state->display, state->window, &width, &height)) {
        stopWgpuTestForWindow(state->window);
        return;
    }
    state->lastWidth = width;
    state->lastHeight = height;

    WGPUSurfaceConfiguration config = {};
    config.device = state->device;
    config.format = state->surfaceFormat;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = width;
    config.height = height;
    config.presentMode = WGPUPresentMode_Fifo;
    config.alphaMode = state->alphaMode;
    p_wgpuSurfaceConfigure(state->surface, &config);
    state->surfaceConfigured = true;
}

static WGPURenderPipeline gpuTestCreatePipeline(GPUTestState* state, const char* shaderSrc) {
    if (!state || !state->device) return nullptr;
    WGPUShaderSourceWGSL wgsl = {};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code.data = shaderSrc;
    wgsl.code.length = WGPU_STRLEN;

    WGPUShaderModuleDescriptor shaderDesc = {};
    shaderDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wgsl);
    WGPUShaderModule shader = p_wgpuDeviceCreateShaderModule(state->device, &shaderDesc);
    if (!shader) {
        wgpu_log("failed to create shader module");
        return nullptr;
    }

    WGPUStringView vsEntry = {"vs_main", WGPU_STRLEN};
    WGPUStringView fsEntry = {"fs_main", WGPU_STRLEN};

    WGPUVertexAttribute attrs[2] = {};
    attrs[0].format = WGPUVertexFormat_Float32x3;
    attrs[0].offset = 0;
    attrs[0].shaderLocation = 0;
    attrs[1].format = WGPUVertexFormat_Float32x4;
    attrs[1].offset = sizeof(float) * 3;
    attrs[1].shaderLocation = 1;

    WGPUVertexBufferLayout vbuf = {};
    vbuf.arrayStride = sizeof(float) * kGpuTestStrideFloats;
    vbuf.attributeCount = 2;
    vbuf.attributes = attrs;
    vbuf.stepMode = WGPUVertexStepMode_Vertex;

    WGPUVertexState vstate = {};
    vstate.module = shader;
    vstate.entryPoint = vsEntry;
    vstate.bufferCount = 1;
    vstate.buffers = &vbuf;

    WGPUColorTargetState colorTarget = {};
    colorTarget.format = state->surfaceFormat;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fstate = {};
    fstate.module = shader;
    fstate.entryPoint = fsEntry;
    fstate.targetCount = 1;
    fstate.targets = &colorTarget;

    WGPUPrimitiveState prim = {};
    prim.topology = WGPUPrimitiveTopology_TriangleList;
    prim.stripIndexFormat = WGPUIndexFormat_Undefined;
    prim.frontFace = WGPUFrontFace_CCW;
    prim.cullMode = WGPUCullMode_None;

    WGPUMultisampleState ms = {};
    ms.count = 1;
    ms.mask = 0xFFFFFFFF;
    ms.alphaToCoverageEnabled = false;

    WGPURenderPipelineDescriptor rpDesc = {};
    rpDesc.vertex = vstate;
    rpDesc.primitive = prim;
    rpDesc.multisample = ms;
    rpDesc.fragment = &fstate;

    WGPURenderPipeline pipeline = p_wgpuDeviceCreateRenderPipeline(state->device, &rpDesc);
    if (!pipeline) {
        wgpu_log("failed to create render pipeline");
        return nullptr;
    }
    return pipeline;
}

static void gpuTestSetupPipeline(GPUTestState* state) {
    if (!state || !state->device) return;

    const char* shaderSrcA = R"WGSL(
struct VSOut {
  @builtin(position) position : vec4<f32>,
};

@vertex
fn vs_main(@location(0) position: vec3<f32>) -> VSOut {
  var out: VSOut;
  out.position = vec4<f32>(position, 1.0);
  return out;
}

@fragment
fn fs_main() -> @location(0) vec4<f32> {
  return vec4<f32>(0.1, 0.9, 0.4, 1.0);
}
)WGSL";

    const char* shaderSrcB = R"WGSL(
struct VSOut {
  @builtin(position) position : vec4<f32>,
  @location(0) local_pos : vec3<f32>,
  @location(1) mouse_state : vec4<f32>,
};

@vertex
fn vs_main(
  @location(0) position: vec3<f32>,
  @location(1) mouse_state: vec4<f32>
) -> VSOut {
  var out: VSOut;
  out.position = vec4<f32>(position, 1.0);
  out.local_pos = position;
  out.mouse_state = mouse_state;
  return out;
}

@fragment
fn fs_main(
  @location(0) local_pos: vec3<f32>,
  @location(1) mouse_state: vec4<f32>
) -> @location(0) vec4<f32> {
  let cursor = vec2<f32>(mouse_state.x * 2.0 - 1.0, (1.0 - mouse_state.y) * 2.0 - 1.0);
  let dist = distance(local_pos.xy, cursor);
  let wave = 0.5 + 0.5 * sin(mouse_state.w * 3.0 - dist * 14.0);
  let pulse = select(wave, 1.0 - wave, mouse_state.z > 0.5);
  let base = vec3<f32>(0.25 + cursor.x * 0.35, 0.35 + cursor.y * 0.25, 0.75);
  let highlight = vec3<f32>(1.0, 0.45, 0.15);
  let color = max(mix(base, highlight, pulse), vec3<f32>(0.05));
  let alpha = 0.7 + 0.3 * pulse;
  return vec4<f32>(color, alpha);
}
)WGSL";

    state->pipelineA = gpuTestCreatePipeline(state, shaderSrcA);
    state->pipelineB = gpuTestCreatePipeline(state, shaderSrcB);

    WGPUBufferDescriptor bufDesc = {};
    bufDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    bufDesc.size = kCubeVertexCount * kGpuTestStrideFloats * sizeof(float);
    bufDesc.mappedAtCreation = false;
    state->vertexBuffer = p_wgpuDeviceCreateBuffer(state->device, &bufDesc);
    if (!state->vertexBuffer) {
        wgpu_log("failed to create vertex buffer");
        return;
    }

    float initialVerts[kCubeVertexCount * kGpuTestStrideFloats];
    buildInterleavedVertices(0.0f, 0.5f, 0.5f, 0.0f, 0.0f, initialVerts);
    p_wgpuQueueWriteBuffer(state->queue, state->vertexBuffer, 0, initialVerts, sizeof(initialVerts));
}

static void gpuTestRenderFrame(GPUTestState* state) {
    if (!state || !state->device || !state->surface || !state->queue) return;
    WGPURenderPipeline pipeline = state->useAlt && state->pipelineB ? state->pipelineB : state->pipelineA;
    if (!pipeline) return;

    uint32_t width = 1;
    uint32_t height = 1;
    if (!gpuTestGetWindowSize(state->display, state->window, &width, &height)) {
        stopWgpuTestForWindow(state->window);
        return;
    }
    if (width != state->lastWidth || height != state->lastHeight) {
        gpuTestConfigureSurface(state);
    }
    if (!state->surfaceConfigured) {
        gpuTestConfigureSurface(state);
        if (!state->surfaceConfigured) return;
    }

    state->angle += 0.02f;
    float mouseX = 0.5f;
    float mouseY = 0.5f;
    float mouseDown = 0.0f;
    gpuTestGetMouseState(state, &mouseX, &mouseY, &mouseDown);
    float verts[kCubeVertexCount * kGpuTestStrideFloats];
    buildInterleavedVertices(state->angle, mouseX, mouseY, mouseDown, state->angle * 1.5f, verts);
    p_wgpuQueueWriteBuffer(state->queue, state->vertexBuffer, 0, verts, sizeof(verts));

    WGPUSurfaceTexture surfaceTexture = {};
    p_wgpuSurfaceGetCurrentTexture(state->surface, &surfaceTexture);
    if (surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        state->surfaceConfigured = false;
        gpuTestConfigureSurface(state);
        return;
    }
    if (!surfaceTexture.texture) return;

    WGPUTextureView view = p_wgpuTextureCreateView(surfaceTexture.texture, nullptr);

    WGPURenderPassColorAttachment colorAtt = {};
    colorAtt.view = view;
    colorAtt.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    colorAtt.loadOp = WGPULoadOp_Clear;
    colorAtt.storeOp = WGPUStoreOp_Store;
    colorAtt.clearValue = {0.05, 0.05, 0.1, 1.0};

    WGPURenderPassDescriptor passDesc = {};
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &colorAtt;

    WGPUCommandEncoder encoder = p_wgpuDeviceCreateCommandEncoder(state->device, nullptr);
    WGPURenderPassEncoder pass = p_wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);
    p_wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    p_wgpuRenderPassEncoderSetVertexBuffer(
        pass,
        0,
        state->vertexBuffer,
        0,
        kCubeVertexCount * kGpuTestStrideFloats * sizeof(float)
    );
    p_wgpuRenderPassEncoderDraw(pass, (uint32_t)kCubeVertexCount, 1, 0, 0);
    p_wgpuRenderPassEncoderEnd(pass);

    WGPUCommandBuffer cmd = p_wgpuCommandEncoderFinish(encoder, nullptr);
    p_wgpuQueueSubmit(state->queue, 1, &cmd);
    p_wgpuSurfacePresent(state->surface);

    p_wgpuTextureViewRelease(view);
    p_wgpuTextureRelease(surfaceTexture.texture);
    p_wgpuCommandBufferRelease(cmd);
    p_wgpuCommandEncoderRelease(encoder);
}

static void logWgpuStringView(const char* prefix, WGPUStringView sv) {
    if (!sv.data) {
        wgpu_log("%s (null)", prefix);
        return;
    }
    size_t len = sv.length == WGPU_STRLEN ? strlen(sv.data) : (size_t)sv.length;
    std::string msg(sv.data, sv.data + len);
    wgpu_log("%s %s", prefix, msg.c_str());
}

static void gpuTestUncapturedErrorCallback(WGPUDevice const* /*device*/, WGPUErrorType type, WGPUStringView message, void* /*userdata1*/, void* /*userdata2*/) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "uncaptured error type=%d:", (int)type);
    logWgpuStringView(prefix, message);
}

static gboolean gpuTestTimerProc(gpointer data) {
    GPUTestState* state = static_cast<GPUTestState*>(data);
    if (!state || !state->running) return G_SOURCE_REMOVE;
    gpuTestRenderFrame(state);
    return state->running ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void gpuTestRequestDeviceCallback(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void* userdata2);

static void gpuTestRequestAdapterCallback(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* userdata1, void* userdata2) {
    (void)userdata2;
    GPUTestState* state = (GPUTestState*)userdata1;
    if (status != WGPURequestAdapterStatus_Success) {
        logWgpuStringView("adapter error:", message);
    }
    if (!state || status != WGPURequestAdapterStatus_Success || !adapter) {
        return;
    }
    state->adapter = adapter;

    WGPURequestDeviceCallbackInfo cbInfo = {};
    cbInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    cbInfo.callback = gpuTestRequestDeviceCallback;
    cbInfo.userdata1 = state;
    WGPUDeviceDescriptor deviceDesc = {};
    deviceDesc.uncapturedErrorCallbackInfo.callback = gpuTestUncapturedErrorCallback;
    deviceDesc.uncapturedErrorCallbackInfo.userdata1 = state;
    p_wgpuAdapterRequestDevice(adapter, &deviceDesc, cbInfo);
}

static void gpuTestRequestDeviceCallback(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void* userdata2) {
    (void)userdata2;
    GPUTestState* state = (GPUTestState*)userdata1;
    if (status != WGPURequestDeviceStatus_Success) {
        logWgpuStringView("device error:", message);
    }
    if (!state || status != WGPURequestDeviceStatus_Success || !device) {
        return;
    }
    // Match the public WGPU bridge path: surface lifecycle calls must stay on the GTK/X11 main thread.
    dispatch_sync_main_void([state, device]() {
        state->device = device;
        if (p_wgpuDeviceSetLabel) {
            WGPUStringView label = {"Electrobun WGPU Device", WGPU_STRLEN};
            p_wgpuDeviceSetLabel(device, label);
        }
        state->queue = p_wgpuDeviceGetQueue(device);

        gpuTestConfigureSurface(state);
        gpuTestSetupPipeline(state);
        if (state->timerId) {
            g_source_remove(state->timerId);
            state->timerId = 0;
        }
        state->running = true;
        state->timerId = g_timeout_add(16, gpuTestTimerProc, state);
        gpuTestRenderFrame(state);
    });
}

static void* runOnMainThreadSyncPtr(std::function<void*()> fn) {
    return dispatch_sync_main([&]() -> void* { return fn(); });
}

static void runOnMainThreadSyncVoid(std::function<void()> fn) {
    dispatch_sync_main_void([&]() { fn(); });
}

static void runOnMainThreadAsyncVoid(std::function<void()> fn) {
    dispatch_async_main_void([fn = std::move(fn)]() { fn(); });
}

ELECTROBUN_EXPORT void* wgpuInstanceCreateSurfaceMainThread(void* instance, void* descriptor) {
    if (!ensureWgpuSymbols()) return nullptr;
    return runOnMainThreadSyncPtr([&]() -> void* {
        return p_wgpuInstanceCreateSurface(instance, descriptor);
    });
}

ELECTROBUN_EXPORT void* wgpuCreateSurfaceForView(void* wgpuInstance, AbstractView* abstractView) {
    if (!wgpuInstance || !abstractView) return nullptr;
    if (!ensureWgpuSymbols()) return nullptr;

    return runOnMainThreadSyncPtr([&]() -> void* {
        WGPUViewImpl* view = dynamic_cast<WGPUViewImpl*>(abstractView);
        if (!view) return nullptr;

        Display* display = nullptr;
        Window window = 0;

        if (view->xDisplay && view->xWindow) {
            display = view->xDisplay;
            window = view->xWindow;
        } else if (view->viewWidget) {
            GdkWindow* gdkWindow = gtk_widget_get_window(view->viewWidget);
            if (!gdkWindow) return nullptr;
            display = gdk_x11_display_get_xdisplay(gdk_window_get_display(gdkWindow));
            window = GDK_WINDOW_XID(gdkWindow);
        }

        if (!display || !window) return nullptr;

        WGPUSurfaceSourceXlibWindow xlibSource = {};
        xlibSource.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
        xlibSource.display = display;
        xlibSource.window = static_cast<uint64_t>(window);

        WGPUSurfaceDescriptor surfaceDesc = {};
        surfaceDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&xlibSource);
        return p_wgpuInstanceCreateSurface(wgpuInstance, &surfaceDesc);
    });
}

ELECTROBUN_EXPORT void wgpuSurfaceConfigureMainThread(void* surface, void* config) {
    if (!ensureWgpuSymbols()) return;
    runOnMainThreadSyncVoid([&]() { p_wgpuSurfaceConfigure(surface, config); });
}

ELECTROBUN_EXPORT void wgpuSurfaceGetCurrentTextureMainThread(void* surface, void* surfaceTexture) {
    if (!ensureWgpuSymbols()) return;
    runOnMainThreadSyncVoid([&]() { p_wgpuSurfaceGetCurrentTexture(surface, surfaceTexture); });
}

ELECTROBUN_EXPORT int32_t wgpuSurfacePresentMainThread(void* surface) {
    if (!ensureWgpuSymbols()) return 0;
    return (int32_t)(intptr_t)runOnMainThreadSyncPtr([&]() -> void* {
        return (void*)(intptr_t)p_wgpuSurfacePresent(surface);
    });
}

ELECTROBUN_EXPORT uint64_t wgpuQueueOnSubmittedWorkDoneShim(void* queue, void* callbackInfo) {
    if (!ensureWgpuSymbols()) return 0;
    if (!callbackInfo) return 0;
    WGPUQueueWorkDoneCallbackInfo info = *(WGPUQueueWorkDoneCallbackInfo*)callbackInfo;
    WGPUFuture future = p_wgpuQueueOnSubmittedWorkDone((WGPUQueue)queue, info);
    return future.id;
}

ELECTROBUN_EXPORT uint64_t wgpuBufferMapAsyncShim(void* buffer, uint64_t mode, uint64_t offset, uint64_t size, void* callbackInfo) {
    if (!ensureWgpuSymbols()) return 0;
    if (!callbackInfo) return 0;
    WGPUBufferMapCallbackInfo info = *(WGPUBufferMapCallbackInfo*)callbackInfo;
    WGPUFuture future = p_wgpuBufferMapAsync((WGPUBuffer)buffer, (WGPUMapMode)mode, (size_t)offset, (size_t)size, info);
    return future.id;
}

ELECTROBUN_EXPORT int32_t wgpuInstanceWaitAnyShim(void* instance, uint64_t futureId, uint64_t timeoutNS) {
    if (!ensureWgpuSymbols()) return 0;
    if (!instance || !futureId) return 0;
    WGPUFutureWaitInfo info;
    info.future.id = futureId;
    info.completed = WGPU_FALSE;
    WGPUWaitStatus status = p_wgpuInstanceWaitAny((WGPUInstance)instance, 1, &info, timeoutNS);
    if (status == WGPUWaitStatus_Success && info.completed) return 1;
    return 0;
}

ELECTROBUN_EXPORT uint8_t* wgpuBufferReadSyncShim(
    void* instance,
    void* buffer,
    uint64_t offset,
    uint64_t size,
    uint64_t timeoutNS,
    uint64_t* outSize
) {
    if (!ensureWgpuSymbols()) return nullptr;
    if (!instance || !buffer || size == 0) return nullptr;

    WGPUBufferMapCallbackInfo mapInfo = {};
    mapInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    mapInfo.callback = nullptr;
    mapInfo.userdata1 = nullptr;
    mapInfo.userdata2 = nullptr;

    WGPUFuture mapFuture = p_wgpuBufferMapAsync(
        (WGPUBuffer)buffer,
        WGPUMapMode_Read,
        (size_t)offset,
        (size_t)size,
        mapInfo
    );

    WGPUFutureWaitInfo waitInfo;
    waitInfo.future = mapFuture;
    waitInfo.completed = WGPU_FALSE;
    WGPUWaitStatus status = p_wgpuInstanceWaitAny(
        (WGPUInstance)instance,
        1,
        &waitInfo,
        timeoutNS
    );

    if (status != WGPUWaitStatus_Success || !waitInfo.completed) {
        return nullptr;
    }

    void* mapped = nullptr;
    if (p_wgpuBufferGetConstMappedRange) {
        mapped = p_wgpuBufferGetConstMappedRange((WGPUBuffer)buffer, (size_t)offset, (size_t)size);
    }
    if (!mapped) {
        mapped = p_wgpuBufferGetMappedRange((WGPUBuffer)buffer, (size_t)offset, (size_t)size);
    }
    if (!mapped) return nullptr;

    uint8_t* out = (uint8_t*)malloc((size_t)size);
    if (!out) return nullptr;
    memcpy(out, mapped, (size_t)size);
    p_wgpuBufferUnmap((WGPUBuffer)buffer);

    if (outSize) *outSize = size;
    return out;
}

ELECTROBUN_EXPORT int32_t wgpuBufferReadSyncIntoShim(
    void* instance,
    void* buffer,
    uint64_t offset,
    uint64_t size,
    uint64_t timeoutNS,
    void* dst
) {
    if (!ensureWgpuSymbols()) return 0;
    if (!instance || !buffer || !dst || size == 0) return 0;

    WGPUBufferMapCallbackInfo mapInfo = {};
    mapInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    mapInfo.callback = nullptr;
    mapInfo.userdata1 = nullptr;
    mapInfo.userdata2 = nullptr;

    WGPUFuture mapFuture = p_wgpuBufferMapAsync(
        (WGPUBuffer)buffer,
        WGPUMapMode_Read,
        (size_t)offset,
        (size_t)size,
        mapInfo
    );

    WGPUFutureWaitInfo waitInfo;
    waitInfo.future = mapFuture;
    waitInfo.completed = WGPU_FALSE;
    WGPUWaitStatus status = p_wgpuInstanceWaitAny(
        (WGPUInstance)instance,
        1,
        &waitInfo,
        timeoutNS
    );

    if (status != WGPUWaitStatus_Success || !waitInfo.completed) {
        return 0;
    }

    void* mapped = nullptr;
    if (p_wgpuBufferGetConstMappedRange) {
        mapped = p_wgpuBufferGetConstMappedRange((WGPUBuffer)buffer, (size_t)offset, (size_t)size);
    }
    if (!mapped) {
        mapped = p_wgpuBufferGetMappedRange((WGPUBuffer)buffer, (size_t)offset, (size_t)size);
    }
    if (!mapped) return 0;
    memcpy(dst, mapped, (size_t)size);
    p_wgpuBufferUnmap((WGPUBuffer)buffer);
    return 1;
}

struct WGPUReadbackJob {
    std::atomic<int> done;
    std::atomic<int> ok;
    std::atomic<int> status;
    uint8_t* dst;
    size_t size;
    WGPUBuffer buffer;
    size_t offset;
};

static void wgpuReadbackCallback(
    WGPUMapAsyncStatus status,
    WGPUStringView /*message*/,
    void* userdata1,
    void* /*userdata2*/
) {
    WGPUReadbackJob* job = (WGPUReadbackJob*)userdata1;
    if (!job) return;
    if (status != WGPUMapAsyncStatus_Success) {
        job->ok.store(0);
        job->status.store(2);
        job->done.store(1);
        return;
    }
    void* mapped = nullptr;
    if (p_wgpuBufferGetConstMappedRange) {
        mapped = p_wgpuBufferGetConstMappedRange(job->buffer, job->offset, job->size);
    }
    if (!mapped) {
        mapped = p_wgpuBufferGetMappedRange(job->buffer, job->offset, job->size);
    }
    if (mapped && job->dst) {
        memcpy(job->dst, mapped, job->size);
        job->ok.store(1);
        job->status.store(1);
    } else {
        job->ok.store(0);
        job->status.store(3);
    }
    p_wgpuBufferUnmap(job->buffer);
    job->done.store(1);
}

ELECTROBUN_EXPORT void* wgpuBufferReadbackBeginShim(
    void* buffer,
    uint64_t offset,
    uint64_t size,
    void* dst
) {
    if (!ensureWgpuSymbols()) return nullptr;
    if (!buffer || !dst || size == 0) return nullptr;

    WGPUReadbackJob* job = (WGPUReadbackJob*)malloc(sizeof(WGPUReadbackJob));
    if (!job) return nullptr;
    job->done.store(0);
    job->ok.store(0);
    job->status.store(0);
    job->dst = (uint8_t*)dst;
    job->size = (size_t)size;
    job->buffer = (WGPUBuffer)buffer;
    job->offset = (size_t)offset;

    WGPUBufferMapCallbackInfo mapInfo = {};
    mapInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    mapInfo.callback = wgpuReadbackCallback;
    mapInfo.userdata1 = job;
    mapInfo.userdata2 = nullptr;

    p_wgpuBufferMapAsync(
        (WGPUBuffer)buffer,
        WGPUMapMode_Read,
        (size_t)offset,
        (size_t)size,
        mapInfo
    );

    return job;
}

ELECTROBUN_EXPORT int32_t wgpuBufferReadbackStatusShim(void* jobPtr) {
    if (!jobPtr) return 2;
    WGPUReadbackJob* job = (WGPUReadbackJob*)jobPtr;
    if (job->done.load() == 0) return 0;
    return job->status.load();
}

ELECTROBUN_EXPORT void wgpuBufferReadbackFreeShim(void* jobPtr) {
    if (!jobPtr) return;
    WGPUReadbackJob* job = (WGPUReadbackJob*)jobPtr;
    free(job);
}

ELECTROBUN_EXPORT void wgpuRunGPUTest(void* abstractView) {
    if (!abstractView) return;
    if (!ensureWgpuTestSymbols()) return;

    runOnMainThreadSyncVoid([&]() {
        WGPUViewImpl* view = dynamic_cast<WGPUViewImpl*>((AbstractView*)abstractView);
        if (!view) {
            wgpu_log("run test called with non-WGPU view");
            return;
        }

        Display* display = nullptr;
        Window window = 0;
        if (view->xDisplay && view->xWindow) {
            display = view->xDisplay;
            window = view->xWindow;
        } else if (view->viewWidget) {
            GdkWindow* gdkWindow = gtk_widget_get_window(view->viewWidget);
            if (gdkWindow) {
                display = gdk_x11_display_get_xdisplay(gdk_window_get_display(gdkWindow));
                window = GDK_WINDOW_XID(gdkWindow);
            }
        }

        if (!display || !window) {
            wgpu_log("failed to resolve native window for test");
            return;
        }

        stopWgpuTestForWindow(0);
        g_gpuTest = GPUTestState{};
        g_gpuTest.display = display;
        g_gpuTest.window = window;
        g_gpuTest.view = view;
        g_gpuTest.useAlt = false;

        if (!g_gpuTest.instance) {
            g_gpuTest.instance = p_wgpuCreateInstance(nullptr);
        }
        if (!g_gpuTest.instance) {
            wgpu_log("failed to create WGPU instance");
            return;
        }

        WGPUSurfaceSourceXlibWindow xlibSource = {};
        xlibSource.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
        xlibSource.display = display;
        xlibSource.window = static_cast<uint64_t>(window);

        WGPUSurfaceDescriptor surfaceDesc = {};
        surfaceDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&xlibSource);
        g_gpuTest.surface = (WGPUSurface)p_wgpuInstanceCreateSurface(g_gpuTest.instance, &surfaceDesc);
        if (!g_gpuTest.surface) {
            wgpu_log("failed to create surface");
            return;
        }

        WGPURequestAdapterOptions opts = {};
        opts.compatibleSurface = g_gpuTest.surface;
        WGPURequestAdapterCallbackInfo cbInfo = {};
        cbInfo.mode = WGPUCallbackMode_AllowSpontaneous;
        cbInfo.callback = gpuTestRequestAdapterCallback;
        cbInfo.userdata1 = &g_gpuTest;
        p_wgpuInstanceRequestAdapter(g_gpuTest.instance, &opts, cbInfo);
    });
}

ELECTROBUN_EXPORT void wgpuToggleGPUTestShader(void* abstractView) {
    if (!abstractView) return;
    if (!ensureWgpuTestSymbols()) return;

    runOnMainThreadSyncVoid([abstractView]() {
        WGPUViewImpl* view = dynamic_cast<WGPUViewImpl*>((AbstractView*)abstractView);
        if (!view || !g_gpuTest.view) return;
        if (g_gpuTest.view == view) {
            g_gpuTest.useAlt = !g_gpuTest.useAlt;
        }
    });
}

ELECTROBUN_EXPORT void wgpuCreateAdapterDeviceMainThread(void* instancePtr, void* surfacePtr, void* outAdapterDevice) {
    if (!ensureWgpuTestSymbols()) return;
    runOnMainThreadSyncVoid([&]() {
        WGPUInstance instance = (WGPUInstance)instancePtr;
        WGPUSurface surface = (WGPUSurface)surfacePtr;
        if (!instance) return;

        struct AdapterRequestCtx {
            std::mutex* mutex;
            std::condition_variable* cv;
            bool* done;
            WGPUAdapter* adapter;
        };
        struct DeviceRequestCtx {
            std::mutex* mutex;
            std::condition_variable* cv;
            bool* done;
            WGPUDevice* device;
        };

        WGPUAdapter adapter = nullptr;
        WGPUDevice device = nullptr;

        std::mutex adapterMutex;
        std::condition_variable adapterCv;
        bool adapterDone = false;

        WGPURequestAdapterOptions opts = {};
        // Only set compatibleSurface if we have a valid surface (for rendering)
        // For compute-only operations, surface can be null
        if (surface) {
            opts.compatibleSurface = surface;
        }
        WGPURequestAdapterCallbackInfo adapterInfo = {};
        adapterInfo.mode = WGPUCallbackMode_AllowSpontaneous;
        adapterInfo.callback = [](WGPURequestAdapterStatus status, WGPUAdapter cbAdapter, WGPUStringView /*message*/, void* userdata1, void* /*userdata2*/) {
            AdapterRequestCtx* ctx = (AdapterRequestCtx*)userdata1;
            {
                std::lock_guard<std::mutex> lock(*ctx->mutex);
                if (status == WGPURequestAdapterStatus_Success) {
                    *ctx->adapter = cbAdapter;
                }
                *ctx->done = true;
            }
            ctx->cv->notify_one();
        };
        AdapterRequestCtx adapterCtx = {&adapterMutex, &adapterCv, &adapterDone, &adapter};
        adapterInfo.userdata1 = &adapterCtx;
        p_wgpuInstanceRequestAdapter(instance, &opts, adapterInfo);

        {
            std::unique_lock<std::mutex> lock(adapterMutex);
            adapterCv.wait(lock, [&]() { return adapterDone; });
        }
        if (!adapter) {
            return;
        }

        std::mutex deviceMutex;
        std::condition_variable deviceCv;
        bool deviceDone = false;

        WGPURequestDeviceCallbackInfo deviceInfo = {};
        deviceInfo.mode = WGPUCallbackMode_AllowSpontaneous;
        deviceInfo.callback = [](WGPURequestDeviceStatus status, WGPUDevice cbDevice, WGPUStringView /*message*/, void* userdata1, void* /*userdata2*/) {
            DeviceRequestCtx* ctx = (DeviceRequestCtx*)userdata1;
            {
                std::lock_guard<std::mutex> lock(*ctx->mutex);
                if (status == WGPURequestDeviceStatus_Success) {
                    *ctx->device = cbDevice;
                }
                *ctx->done = true;
            }
            ctx->cv->notify_one();
        };
        DeviceRequestCtx deviceCtx = {&deviceMutex, &deviceCv, &deviceDone, &device};
        deviceInfo.userdata1 = &deviceCtx;

        WGPUDeviceDescriptor deviceDesc = {};
        deviceDesc.uncapturedErrorCallbackInfo.callback = gpuTestUncapturedErrorCallback;
        deviceDesc.uncapturedErrorCallbackInfo.userdata1 = &deviceCtx;
        p_wgpuAdapterRequestDevice(adapter, &deviceDesc, deviceInfo);

        {
            std::unique_lock<std::mutex> lock(deviceMutex);
            deviceCv.wait(lock, [&]() { return deviceDone; });
        }

        if (outAdapterDevice) {
            uint64_t* out = (uint64_t*)outAdapterDevice;
            out[0] = (uint64_t)adapter;
            out[1] = (uint64_t)device;
        }
    });
}

