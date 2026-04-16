#include "dx_overlay.hpp"
#include "overlay.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"

#include <DetourModKit.hpp>

#pragma warning(push, 0)
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#pragma warning(pop)

#include <d3d11.h>

#include <Windows.h>

#include <atomic>
#include <cstring>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ====================================================================== //
//  Swap-chain-free transparent overlay                                    //
//                                                                         //
//  OptiScaler hooks ALL DXGI swap chain creation and asserts on its        //
//  ImGui re-init.  This approach creates NO swap chain at all:             //
//    1. D3D11 WARP device (no swap chain — only CreateDevice)              //
//    2. Offscreen Texture2D render target                                  //
//    3. ImGui renders to the texture via ImGui_ImplDX11                    //
//    4. Texture pixels copied to a DIB section                             //
//    5. UpdateLayeredWindow composites the DIB onto the screen             //
//                                                                         //
//  Zero DXGI swap chain → zero OptiScaler interference.                   //
// ====================================================================== //

namespace Transmog
{
    static std::atomic<bool> s_overlayVisible{false};
    static std::atomic<bool> s_shutdownRequested{false};
    static bool s_initialised = false;

    static HWND s_overlayHwnd = nullptr;
    static HWND s_gameHwnd = nullptr;

    // D3D11 WARP device — no swap chain.
    static ID3D11Device *s_device = nullptr;
    static ID3D11DeviceContext *s_context = nullptr;

    // Offscreen render target.
    static ID3D11Texture2D *s_rtTex = nullptr;
    static ID3D11RenderTargetView *s_rtv = nullptr;
    static ID3D11Texture2D *s_stagingTex = nullptr;
    static UINT s_width = 0;
    static UINT s_height = 0;

    // GDI blitting.
    static HDC s_memDC = nullptr;
    static HBITMAP s_dib = nullptr;
    static void *s_dibPixels = nullptr;

    static HANDLE s_renderThread = nullptr;

    // ------------------------------------------------------------------ //
    //  Render target + staging + DIB management                           //
    // ------------------------------------------------------------------ //

    static void release_targets()
    {
        if (s_rtv)        { s_rtv->Release();        s_rtv = nullptr; }
        if (s_rtTex)      { s_rtTex->Release();      s_rtTex = nullptr; }
        if (s_stagingTex) { s_stagingTex->Release();  s_stagingTex = nullptr; }
        if (s_dib)        { DeleteObject(s_dib);      s_dib = nullptr; }
        if (s_memDC)      { DeleteDC(s_memDC);        s_memDC = nullptr; }
        s_dibPixels = nullptr;
    }

    [[nodiscard]] static bool create_targets(UINT w, UINT h)
    {
        release_targets();
        s_width = w;
        s_height = h;

        // Render target texture (BGRA for GDI compatibility).
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(s_device->CreateTexture2D(&td, nullptr, &s_rtTex)))
            return false;

        if (FAILED(s_device->CreateRenderTargetView(s_rtTex, nullptr, &s_rtv)))
            return false;

        // Staging texture (CPU-readable copy).
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(s_device->CreateTexture2D(&td, nullptr, &s_stagingTex)))
            return false;

        // DIB section for UpdateLayeredWindow.
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(w);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(h); // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        HDC screenDC = GetDC(nullptr);
        s_memDC = CreateCompatibleDC(screenDC);
        s_dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS,
                                 &s_dibPixels, nullptr, 0);
        ReleaseDC(nullptr, screenDC);

        if (!s_dib || !s_dibPixels)
            return false;

        SelectObject(s_memDC, s_dib);
        return true;
    }

    // Copy the rendered texture to the DIB and composite onto the screen.
    static void blit_to_screen()
    {
        // GPU → staging texture.
        s_context->CopyResource(s_stagingTex, s_rtTex);

        // Map staging → CPU.
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(s_context->Map(s_stagingTex, 0, D3D11_MAP_READ, 0, &mapped)))
            return;

        // Copy rows to DIB (pitch may differ).
        const UINT rowBytes = s_width * 4;
        auto *src = static_cast<const uint8_t *>(mapped.pData);
        auto *dst = static_cast<uint8_t *>(s_dibPixels);
        for (UINT y = 0; y < s_height; ++y)
        {
            memcpy(dst, src, rowBytes);
            src += mapped.RowPitch;
            dst += rowBytes;
        }
        s_context->Unmap(s_stagingTex, 0);

        // Premultiply alpha for UpdateLayeredWindow (expects PARGB).
        auto *px = static_cast<uint8_t *>(s_dibPixels);
        const UINT total = s_width * s_height;
        for (UINT i = 0; i < total; ++i, px += 4)
        {
            const uint8_t a = px[3];
            if (a == 0)      { px[0] = px[1] = px[2] = 0; }
            else if (a < 255)
            {
                px[0] = static_cast<uint8_t>((px[0] * a + 127) / 255);
                px[1] = static_cast<uint8_t>((px[1] * a + 127) / 255);
                px[2] = static_cast<uint8_t>((px[2] * a + 127) / 255);
            }
        }

        // Composite onto screen.
        RECT gr{};
        GetWindowRect(s_gameHwnd, &gr);
        POINT ptPos = {gr.left, gr.top};
        SIZE sz = {static_cast<LONG>(s_width), static_cast<LONG>(s_height)};
        POINT ptSrc = {0, 0};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        UpdateLayeredWindow(s_overlayHwnd, nullptr, &ptPos, &sz,
                            s_memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    }

    // ------------------------------------------------------------------ //
    //  Helpers                                                            //
    // ------------------------------------------------------------------ //

    static HWND find_game_hwnd()
    {
        struct Ctx { HWND result; } ctx{nullptr};
        EnumWindows(
            [](HWND hwnd, LPARAM lp) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd))
                    return TRUE;
                RECT rc{};
                GetClientRect(hwnd, &rc);
                if ((rc.right - rc.left) < 640 || (rc.bottom - rc.top) < 480)
                    return TRUE;
                char cls[128]{};
                GetClassNameA(hwnd, cls, sizeof(cls));
                if (strcmp(cls, "SplashWindow") == 0)
                    return TRUE;
                reinterpret_cast<Ctx *>(lp)->result = hwnd;
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        return ctx.result;
    }

    // ------------------------------------------------------------------ //
    //  Overlay WndProc                                                    //
    // ------------------------------------------------------------------ //

    static LRESULT CALLBACK overlay_wndproc(HWND hWnd, UINT msg,
                                            WPARAM wParam, LPARAM lParam)
    {
        if (s_initialised &&
            s_overlayVisible.load(std::memory_order_relaxed))
        {
            // Esc closes the overlay and returns focus to the game.
            if (msg == WM_KEYDOWN && wParam == VK_ESCAPE)
            {
                toggle_overlay_visible();
                return 0;
            }

            if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
                return 1;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    // ------------------------------------------------------------------ //
    //  Render loop                                                        //
    // ------------------------------------------------------------------ //

    static DWORD WINAPI render_thread(LPVOID)
    {
        auto &logger = DMK::Logger::get_instance();

        // Wait for game world.
        logger.info("[dx_overlay] Waiting for game world...");
        for (int i = 0; i < 3000; ++i)
        {
            if (s_shutdownRequested.load(std::memory_order_relaxed)) return 0;
            if (Transmog::is_world_ready()) break;
            Sleep(100);
        }
        if (!Transmog::is_world_ready())
        { logger.warning("[dx_overlay] World timeout"); return 0; }

        Sleep(2000);
        s_gameHwnd = find_game_hwnd();
        if (!s_gameHwnd)
        { logger.error("[dx_overlay] Game window not found"); return 0; }

        RECT gr{};
        GetClientRect(s_gameHwnd, &gr);
        const UINT gw = static_cast<UINT>(gr.right);
        const UINT gh = static_cast<UINT>(gr.bottom);
        logger.info("[dx_overlay] Game {}x{}", gw, gh);

        // --- Overlay window (layered, never receives its own
        //     D3D/DXGI objects — purely a GDI composite target) ---
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = overlay_wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"TransmogOverlay";
        RegisterClassExW(&wc);

        GetWindowRect(s_gameHwnd, &gr);
        // No WS_EX_TOPMOST — we position relative to the game window
        // each frame so the overlay doesn't cover the taskbar or appear
        // above other apps when the game isn't focused.
        s_overlayHwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            wc.lpszClassName, L"",
            WS_POPUP,
            gr.left, gr.top, static_cast<int>(gw), static_cast<int>(gh),
            nullptr, nullptr, wc.hInstance, nullptr);
        if (!s_overlayHwnd)
        { logger.error("[dx_overlay] Window creation failed"); return 0; }

        ShowWindow(s_overlayHwnd, SW_SHOWNOACTIVATE);

        // --- D3D11 WARP device (NO swap chain) ---
        const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                &fl, 1, D3D11_SDK_VERSION,
                &s_device, nullptr, &s_context)))
        {
            logger.error("[dx_overlay] WARP device failed");
            DestroyWindow(s_overlayHwnd);
            return 0;
        }

        if (!create_targets(gw, gh))
        {
            logger.error("[dx_overlay] Render targets failed");
            s_context->Release(); s_device->Release();
            DestroyWindow(s_overlayHwnd);
            return 0;
        }

        // --- ImGui ---
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(static_cast<float>(gw), static_cast<float>(gh));

        // DPI-aware scaling: base size targets 1080p, scale up for higher
        // resolutions so the UI stays readable at 1440p / 4K.
        const float dpiScale = static_cast<float>(gh) / 1080.0f;
        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(dpiScale);
        io.FontGlobalScale = dpiScale;

        ImGui_ImplWin32_Init(s_overlayHwnd);
        ImGui_ImplDX11_Init(s_device, s_context);

        s_initialised = true;
        logger.info("[dx_overlay] Overlay ready (WARP + GDI blit, no swap chain)");

        // --- Render loop ---
        while (!s_shutdownRequested.load(std::memory_order_relaxed))
        {
            MSG msg;
            while (PeekMessageW(&msg, s_overlayHwnd, 0, 0, PM_REMOVE))
            { TranslateMessage(&msg); DispatchMessageW(&msg); }

            if (!IsWindow(s_gameHwnd)) break;

            // Only show the overlay when the game is the foreground window
            // (or our overlay itself is focused).  Hide otherwise so the
            // overlay doesn't linger over other apps when alt-tabbed.
            const HWND fg = GetForegroundWindow();
            const bool gameActive = (fg == s_gameHwnd || fg == s_overlayHwnd);

            const bool wantVisible = gameActive &&
                s_overlayVisible.load(std::memory_order_relaxed);

            if (gameActive && wantVisible)
            {
                // Only reposition when the game window actually moved.
                static RECT s_lastGR{};
                RECT ngr{};
                GetWindowRect(s_gameHwnd, &ngr);
                if (ngr.left != s_lastGR.left || ngr.top != s_lastGR.top ||
                    ngr.right != s_lastGR.right || ngr.bottom != s_lastGR.bottom)
                {
                    SetWindowPos(s_overlayHwnd, HWND_TOP,
                                 ngr.left, ngr.top,
                                 ngr.right - ngr.left, ngr.bottom - ngr.top,
                                 SWP_NOACTIVATE);
                    s_lastGR = ngr;
                }

                if (!IsWindowVisible(s_overlayHwnd))
                    ShowWindow(s_overlayHwnd, SW_SHOWNOACTIVATE);
            }
            else
            {
                if (IsWindowVisible(s_overlayHwnd))
                    ShowWindow(s_overlayHwnd, SW_HIDE);
            }

            const bool visible = wantVisible;

            // Toggle click-through.
            {
                static bool wasVisible = false;
                if (visible && !wasVisible)
                {
                    LONG_PTR ex = GetWindowLongPtrW(s_overlayHwnd, GWL_EXSTYLE);
                    SetWindowLongPtrW(s_overlayHwnd, GWL_EXSTYLE,
                                      ex & ~WS_EX_TRANSPARENT);
                    SetForegroundWindow(s_overlayHwnd);
                }
                else if (!visible && wasVisible)
                {
                    LONG_PTR ex = GetWindowLongPtrW(s_overlayHwnd, GWL_EXSTYLE);
                    SetWindowLongPtrW(s_overlayHwnd, GWL_EXSTYLE,
                                      ex | WS_EX_TRANSPARENT);
                    SetForegroundWindow(s_gameHwnd);
                }
                wasVisible = visible;
            }

            if (!visible)
            {
                // Nothing to draw — sleep longer to save CPU.
                Sleep(50);
                continue;
            }

            // Clear render target to transparent black.
            const float clear[4] = {0, 0, 0, 0};
            s_context->OMSetRenderTargets(1, &s_rtv, nullptr);
            s_context->ClearRenderTargetView(s_rtv, clear);

            D3D11_VIEWPORT vp{};
            vp.Width = static_cast<float>(s_width);
            vp.Height = static_cast<float>(s_height);
            vp.MaxDepth = 1.0f;
            s_context->RSSetViewports(1, &vp);

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            draw_overlay();

            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            blit_to_screen();

            Sleep(16); // ~60fps — sufficient for UI, reduces CPU/GDI load
        }

        // Cleanup.
        s_initialised = false;
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        release_targets();
        if (s_context) { s_context->Release(); s_context = nullptr; }
        if (s_device)  { s_device->Release();  s_device = nullptr; }
        DestroyWindow(s_overlayHwnd);
        UnregisterClassW(L"TransmogOverlay", GetModuleHandleW(nullptr));
        s_overlayHwnd = nullptr;
        return 0;
    }

    // ------------------------------------------------------------------ //
    //  Public API                                                         //
    // ------------------------------------------------------------------ //

    bool init_dx_overlay()
    {
        s_renderThread = CreateThread(nullptr, 0, render_thread, nullptr, 0, nullptr);
        return s_renderThread != nullptr;
    }

    void shutdown_dx_overlay() noexcept
    {
        s_shutdownRequested.store(true, std::memory_order_release);
        if (s_renderThread)
        {
            WaitForSingleObject(s_renderThread, 5000);
            CloseHandle(s_renderThread);
            s_renderThread = nullptr;
        }
    }

    void toggle_overlay_visible() noexcept
    {
        // Debounce: ignore toggles within 300ms of the last one.
        // Prevents key-repeat and polling overlap from rapidly
        // flipping the overlay on/off/on.
        static std::atomic<int64_t> s_lastToggleMs{0};
        const int64_t now = steady_ms();
        const int64_t last = s_lastToggleMs.load(std::memory_order_relaxed);
        if (now - last < 300)
            return;
        s_lastToggleMs.store(now, std::memory_order_relaxed);

        s_overlayVisible.store(
            !s_overlayVisible.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    bool is_overlay_visible() noexcept
    {
        return s_overlayVisible.load(std::memory_order_relaxed);
    }

} // namespace Transmog
