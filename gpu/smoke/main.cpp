// plume cleared-window smoke test (high-cut engine M0c).
//
// Minimal proof that the vendored plume RHI initializes a device, creates a
// swapchain on a Win32 window, clears the backbuffer, and presents — on both
// the D3D12 and Vulkan backends. Self-contained (no SDL, no recomp). Headless-
// verifiable: runs N frames then prints "SMOKE OK ..." and exits 0.
//
// Usage: plume_smoke.exe [--vulkan] [--frames N]   (N=0 => run until closed)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "plume_render_interface.h"

namespace plume {
    extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
    extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

using namespace plume;

namespace {

constexpr uint32_t kBufferCount = 2;
constexpr RenderFormat kSwapFormat = RenderFormat::B8G8R8A8_UNORM;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:   DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0);  return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

HWND CreateAppWindow(int w, int h, const char* title) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "PlumeSmokeWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd) ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

struct Ctx {
    std::unique_ptr<RenderDevice> device;
    std::unique_ptr<RenderCommandQueue> queue;
    std::unique_ptr<RenderCommandList> cmd;
    std::unique_ptr<RenderCommandFence> fence;
    std::unique_ptr<RenderSwapChain> swap;
    std::unique_ptr<RenderCommandSemaphore> acquireSem;
    std::vector<std::unique_ptr<RenderCommandSemaphore>> releaseSems;
    std::vector<std::unique_ptr<RenderFramebuffer>> fbs;
};

void CreateFramebuffers(Ctx& c) {
    c.fbs.clear();
    for (uint32_t i = 0; i < c.swap->getTextureCount(); ++i) {
        const RenderTexture* color = c.swap->getTexture(i);
        RenderFramebufferDesc d;
        d.colorAttachments = &color;
        d.colorAttachmentsCount = 1;
        d.depthAttachment = nullptr;
        c.fbs.push_back(c.device->createFramebuffer(d));
    }
}

bool Init(Ctx& c, RenderInterface* ri, HWND hwnd) {
    c.device = ri->createDevice();
    if (!c.device) { fprintf(stderr, "createDevice failed\n"); return false; }
    c.queue = c.device->createCommandQueue(RenderCommandListType::DIRECT);
    c.fence = c.device->createCommandFence();
    c.swap = c.queue->createSwapChain(RenderSwapChainDesc(hwnd, kSwapFormat, kBufferCount));
    c.swap->resize();
    c.cmd = c.queue->createCommandList();
    c.acquireSem = c.device->createCommandSemaphore();
    CreateFramebuffers(c);
    printf("  device + swapchain ready: %ux%u, %u buffers\n",
           c.swap->getWidth(), c.swap->getHeight(), c.swap->getTextureCount());
    return true;
}

void RenderFrame(Ctx& c, int frame) {
    uint32_t idx = 0;
    if (!c.swap->acquireTexture(c.acquireSem.get(), &idx)) return;

    c.cmd->begin();
    RenderTexture* tex = c.swap->getTexture(idx);
    c.cmd->barriers(RenderBarrierStage::GRAPHICS,
                    RenderTextureBarrier(tex, RenderTextureLayout::COLOR_WRITE));
    c.cmd->setFramebuffer(c.fbs[idx].get());

    const uint32_t w = c.swap->getWidth(), h = c.swap->getHeight();
    c.cmd->setViewports(RenderViewport(0.0f, 0.0f, float(w), float(h)));
    c.cmd->setScissors(RenderRect(0, 0, w, h));

    // Animated clear so a visible window is obviously alive.
    float t = (frame % 120) / 120.0f;
    c.cmd->clearColor(0, RenderColor(0.1f, t, 0.2f + 0.3f * t, 1.0f));

    c.cmd->barriers(RenderBarrierStage::NONE,
                    RenderTextureBarrier(tex, RenderTextureLayout::PRESENT));
    c.cmd->end();

    while (c.releaseSems.size() < c.swap->getTextureCount())
        c.releaseSems.emplace_back(c.device->createCommandSemaphore());

    const RenderCommandList* cl = c.cmd.get();
    RenderCommandSemaphore* wait = c.acquireSem.get();
    RenderCommandSemaphore* signal = c.releaseSems[idx].get();
    c.queue->executeCommandLists(&cl, 1, &wait, 1, &signal, 1, c.fence.get());
    c.swap->present(idx, &signal, 1);
    c.queue->waitForCommandFence(c.fence.get());
}

}  // namespace

int main(int argc, char** argv) {
    bool useVulkan = false;
    int frames = 120;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--vulkan")) useVulkan = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    }
    const char* backend = useVulkan ? "Vulkan" : "D3D12";
    printf("[plume-smoke] backend=%s frames=%d\n", backend, frames);

    HWND hwnd = CreateAppWindow(1280, 720, (std::string("plume smoke - ") + backend).c_str());
    if (!hwnd) { fprintf(stderr, "window creation failed\n"); return 2; }

    std::unique_ptr<RenderInterface> ri = useVulkan ? CreateVulkanInterface() : CreateD3D12Interface();
    if (!ri) { fprintf(stderr, "CreateInterface(%s) failed\n", backend); return 3; }

    Ctx c;
    if (!Init(c, ri.get(), hwnd)) return 4;

    int rendered = 0;
    bool running = true;
    while (running) {
        MSG m;
        while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) running = false;
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
        if (!running) break;
        RenderFrame(c, rendered);
        ++rendered;
        if (rendered % 30 == 0) printf("  frame %d\n", rendered);
        if (frames > 0 && rendered >= frames) break;
    }

    // Drain: move the active backbuffer out of PRESENT before teardown.
    uint32_t idx = 0;
    if (!c.swap->isEmpty() && c.swap->acquireTexture(c.acquireSem.get(), &idx)) {
        c.cmd->begin();
        c.cmd->barriers(RenderBarrierStage::NONE,
                        RenderTextureBarrier(c.swap->getTexture(idx), RenderTextureLayout::COLOR_WRITE));
        c.cmd->end();
        const RenderCommandList* cl = c.cmd.get();
        RenderCommandSemaphore* wait = c.acquireSem.get();
        c.queue->executeCommandLists(&cl, 1, &wait, 1, nullptr, 0, c.fence.get());
        c.queue->waitForCommandFence(c.fence.get());
    }

    printf("SMOKE OK: cleared+presented %d frames on %s\n", rendered, backend);
    DestroyWindow(hwnd);
    return 0;
}
