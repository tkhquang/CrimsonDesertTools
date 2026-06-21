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
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Swap-chain-free transparent overlay.
//
// OptiScaler hooks all DXGI swap chain creation and asserts on its ImGui re-init. This approach creates no swap chain
// at all:
//   1. D3D11 WARP device (CreateDevice only, no swap chain)
//   2. Offscreen Texture2D render target
//   3. ImGui renders to the texture via ImGui_ImplDX11
//   4. Texture pixels copied to a DIB section
//   5. UpdateLayeredWindow composites the DIB onto the screen
// Zero DXGI swap chain means zero OptiScaler interference.

namespace Transmog
{
    static std::atomic<bool> s_overlayVisible{false};
    static std::atomic<bool> s_shutdownRequested{false};
    static bool s_initialised = false;

    static HWND s_overlayHwnd = nullptr;
    static HWND s_gameHwnd = nullptr;

    // D3D11 WARP device, no swap chain.
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

    // --- Render target + staging + DIB management ---

    static void release_targets()
    {
        if (s_rtv)
        {
            s_rtv->Release();
            s_rtv = nullptr;
        }
        if (s_rtTex)
        {
            s_rtTex->Release();
            s_rtTex = nullptr;
        }
        if (s_stagingTex)
        {
            s_stagingTex->Release();
            s_stagingTex = nullptr;
        }
        if (s_dib)
        {
            DeleteObject(s_dib);
            s_dib = nullptr;
        }
        if (s_memDC)
        {
            DeleteDC(s_memDC);
            s_memDC = nullptr;
        }
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
        s_dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &s_dibPixels, nullptr, 0);
        ReleaseDC(nullptr, screenDC);

        if (!s_dib || !s_dibPixels)
            return false;

        SelectObject(s_memDC, s_dib);
        return true;
    }

    // Compute the union of all non-empty draw cmd clip rects from this frame's ImDrawData, clamped to the target
    // surface. Returns false when ImGui drew nothing (e.g. the standalone window's Begin returned collapsed/clipped);
    // caller falls back to full-surface.
    static bool compute_draw_bounds(ImDrawData *dd, RECT &out)
    {
        if (!dd || dd->CmdListsCount == 0)
            return false;
        const float fW = static_cast<float>(s_width);
        const float fH = static_cast<float>(s_height);
        float mnx = fW, mny = fH, mxx = 0.0f, mxy = 0.0f;
        bool any = false;
        for (int i = 0; i < dd->CmdListsCount; ++i)
        {
            const ImDrawList *cl = dd->CmdLists[i];
            for (int c = 0; c < cl->CmdBuffer.Size; ++c)
            {
                const ImDrawCmd &cmd = cl->CmdBuffer[c];
                if (cmd.ElemCount == 0)
                    continue;
                // Paren-around-name defeats the Windows.h min/max macros for IntelliSense's parse, even when the build
                // already has NOMINMAX defined upstream.
                float x0 = (std::max)(0.0f, cmd.ClipRect.x);
                float y0 = (std::max)(0.0f, cmd.ClipRect.y);
                float x1 = (std::min)(fW, cmd.ClipRect.z);
                float y1 = (std::min)(fH, cmd.ClipRect.w);
                if (x1 <= x0 || y1 <= y0)
                    continue;
                if (x0 < mnx)
                    mnx = x0;
                if (y0 < mny)
                    mny = y0;
                if (x1 > mxx)
                    mxx = x1;
                if (y1 > mxy)
                    mxy = y1;
                any = true;
            }
        }
        if (!any)
            return false;
        out.left = static_cast<LONG>(std::floor(mnx));
        out.top = static_cast<LONG>(std::floor(mny));
        out.right = static_cast<LONG>(std::ceil(mxx));
        out.bottom = static_cast<LONG>(std::ceil(mxy));
        return true;
    }

    // Copy the rendered texture to the DIB and composite onto the screen. Only pixels inside `dirty` are CPU-touched
    // and pushed via ULWI's prcDirty: ImGui's window typically occupies < 5% of a 4K screen, so limiting the staging
    // readback + per-pixel premultiply + GDI composite to that region is the bulk of the standalone-overlay
    // responsiveness win. `dirty` must be the UNION of the previous frame's drawn rect and this frame's drawn rect so
    // pixels the popup just vacated get cleared in the layered surface.
    static void blit_to_screen(const RECT &dirty)
    {
        const LONG W = static_cast<LONG>(s_width);
        const LONG H = static_cast<LONG>(s_height);
        const LONG x0 = std::clamp<LONG>(dirty.left, 0, W);
        const LONG y0 = std::clamp<LONG>(dirty.top, 0, H);
        const LONG x1 = std::clamp<LONG>(dirty.right, 0, W);
        const LONG y1 = std::clamp<LONG>(dirty.bottom, 0, H);
        if (x1 <= x0 || y1 <= y0)
            return;

        // GPU -> staging: only the dirty box. CopySubresourceRegion is
        // free where CopyResource would have read the whole texture.
        D3D11_BOX box{};
        box.left = static_cast<UINT>(x0);
        box.top = static_cast<UINT>(y0);
        box.front = 0;
        box.right = static_cast<UINT>(x1);
        box.bottom = static_cast<UINT>(y1);
        box.back = 1;
        s_context->CopySubresourceRegion(s_stagingTex, 0, static_cast<UINT>(x0), static_cast<UINT>(y0), 0, s_rtTex, 0,
                                         &box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(s_context->Map(s_stagingTex, 0, D3D11_MAP_READ, 0, &mapped)))
            return;

        const UINT rowBytes = s_width * 4;
        const UINT spanBytes = static_cast<UINT>(x1 - x0) * 4;
        auto *srcBase = static_cast<const uint8_t *>(mapped.pData);
        auto *dstBase = static_cast<uint8_t *>(s_dibPixels);
        // memcpy + premultiply each dirty row in one pass. Outside the dirty rect the DIB retains last frame's bytes;
        // ULWI's prcDirty ignores them.
        for (LONG y = y0; y < y1; ++y)
        {
            auto *src = srcBase + static_cast<size_t>(y) * mapped.RowPitch + static_cast<size_t>(x0) * 4;
            auto *dst = dstBase + static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x0) * 4;
            memcpy(dst, src, spanBytes);
            for (LONG xi = 0; xi < x1 - x0; ++xi)
            {
                uint8_t *px = dst + xi * 4;
                const uint8_t a = px[3];
                if (a == 0)
                {
                    px[0] = px[1] = px[2] = 0;
                }
                else if (a < 255)
                {
                    px[0] = static_cast<uint8_t>((px[0] * a + 127) / 255);
                    px[1] = static_cast<uint8_t>((px[1] * a + 127) / 255);
                    px[2] = static_cast<uint8_t>((px[2] * a + 127) / 255);
                }
            }
        }
        s_context->Unmap(s_stagingTex, 0);

        // Composite onto screen with a dirty-rect hint so GDI only touches the changed region of the layered surface.
        RECT gr{};
        GetWindowRect(s_gameHwnd, &gr);
        POINT ptPos = {gr.left, gr.top};
        SIZE sz = {W, H};
        POINT ptSrc = {0, 0};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        RECT dirtyClamped{x0, y0, x1, y1};
        UPDATELAYEREDWINDOWINFO ulwi{};
        ulwi.cbSize = sizeof(ulwi);
        ulwi.pptDst = &ptPos;
        ulwi.psize = &sz;
        ulwi.hdcSrc = s_memDC;
        ulwi.pptSrc = &ptSrc;
        ulwi.pblend = &blend;
        ulwi.dwFlags = ULW_ALPHA;
        ulwi.prcDirty = &dirtyClamped;
        UpdateLayeredWindowIndirect(s_overlayHwnd, &ulwi);
    }

    // --- Helpers ---

    static HWND find_game_hwnd()
    {
        struct Ctx
        {
            HWND result;
        } ctx{nullptr};
        EnumWindows(
            [](HWND hwnd, LPARAM lp) -> BOOL
            {
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

    // --- Overlay WndProc ---

    static LRESULT CALLBACK overlay_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (s_initialised && s_overlayVisible.load(std::memory_order_relaxed))
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

    // --- Render loop ---

    static DWORD WINAPI render_thread(LPVOID)
    {
        auto &logger = DMK::Logger::get_instance();

        // Wait for game world. Slow boots, intro cinematics, OS-level game updates and similar can all push this past
        // the old 5-minute cap; with no upper bound the overlay simply waits until the world resolves OR the mod is
        // shutting down. Heart-beat log every 60s so the wait is visible to the user, with no warning noise from a hard
        // timeout that never made sense.
        logger.info("[dx_overlay] Waiting for game world...");
        for (int tick = 0;; ++tick)
        {
            if (s_shutdownRequested.load(std::memory_order_relaxed))
                return 0;
            if (Transmog::is_world_ready())
                break;
            // 600 * 100ms == 60s. Log a heartbeat at each minute mark so a user looking at the log can confirm the
            // overlay thread is still alive while they sit on a long load.
            if (tick > 0 && (tick % 600) == 0)
                logger.info("[dx_overlay] Still waiting for game world ({} min)", tick / 600);
            Sleep(100);
        }

        Sleep(2000);
        s_gameHwnd = find_game_hwnd();
        if (!s_gameHwnd)
        {
            logger.error("[dx_overlay] Game window not found");
            return 0;
        }

        RECT gr{};
        GetClientRect(s_gameHwnd, &gr);
        const UINT gw = static_cast<UINT>(gr.right);
        const UINT gh = static_cast<UINT>(gr.bottom);
        logger.info("[dx_overlay] Game {}x{}", gw, gh);

        // --- Overlay window (layered, never receives its own
        //     D3D/DXGI objects, purely a GDI composite target) ---
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = overlay_wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"TransmogOverlay";
        RegisterClassExW(&wc);

        GetWindowRect(s_gameHwnd, &gr);
        // No WS_EX_TOPMOST: we position relative to the game window each frame so the overlay doesn't cover the taskbar
        // or appear above other apps when the game isn't focused.
        s_overlayHwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                        wc.lpszClassName, L"", WS_POPUP, gr.left, gr.top, static_cast<int>(gw),
                                        static_cast<int>(gh), nullptr, nullptr, wc.hInstance, nullptr);
        if (!s_overlayHwnd)
        {
            logger.error("[dx_overlay] Window creation failed");
            return 0;
        }

        ShowWindow(s_overlayHwnd, SW_SHOWNOACTIVATE);

        // --- D3D11 WARP device (NO swap chain) ---
        const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &s_device,
                                     nullptr, &s_context)))
        {
            logger.error("[dx_overlay] WARP device failed");
            DestroyWindow(s_overlayHwnd);
            return 0;
        }

        if (!create_targets(gw, gh))
        {
            logger.error("[dx_overlay] Render targets failed");
            s_context->Release();
            s_device->Release();
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

        // DPI-aware scaling: base size targets 1080p, scale up for higher resolutions so the UI stays readable at 1440p
        // / 4K.
        const float dpiScale = static_cast<float>(gh) / 1080.0f;
        ImGui::StyleColorsDark();

        // Build the default font at the target pixel size instead of stretching ImGui's 13px built-in bitmap via
        // FontGlobalScale. The stretch path leaves glyphs blurry and breaks the row-vs-glyph ratio: rows scale via
        // ScaleAllSizes but the bitmap font does not, which is what made the standalone overlay look thin and cramped.
        //
        // 14px base reads better than ImGui's default at 1080p; dpiScale lifts it linearly for 1440p / 4K.
        // ScaleAllSizes is then driven by the font/13px ratio so padding, frame heights, and column widths stay
        // proportional to the glyph size.
        ImFontConfig fontCfg;
        fontCfg.SizePixels = 14.0f * dpiScale;
        io.Fonts->AddFontDefault(&fontCfg);
        io.FontGlobalScale = 1.0f;
        ImGui::GetStyle().ScaleAllSizes(fontCfg.SizePixels / 13.0f);

        ImGui_ImplWin32_Init(s_overlayHwnd);
        ImGui_ImplDX11_Init(s_device, s_context);

        s_initialised = true;
        logger.info("[dx_overlay] Overlay ready (WARP + GDI blit, no swap chain)");

        // Raise the system timer resolution to 1ms so the adaptive
        // Sleep() at the bottom of the render loop is actually honored. Without this, Sleep(4) rounds up to the default
        // ~15.6 ms scheduler tick and the overlay caps at ~60 Hz no matter what we ask for, which is what made
        // hover/click feel sluggish on the standalone path. Paired timeEndPeriod() runs before the thread exits below.
        timeBeginPeriod(1);

        // Previous frame's drawn rect, so we can union it with this frame's rect and clear pixels that ImGui vacated
        // (popup closed, tooltip dismissed, etc.) -- ULWI's prcDirty only refreshes inside the supplied rect.
        RECT prevDirty{0, 0, 0, 0};

        // --- Render loop ---
        while (!s_shutdownRequested.load(std::memory_order_relaxed))
        {
            MSG msg;
            while (PeekMessageW(&msg, s_overlayHwnd, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            if (!IsWindow(s_gameHwnd))
                break;

            // Only show the overlay when the game is the foreground window (or our overlay itself is focused). Hide
            // otherwise so the overlay doesn't linger over other apps when alt-tabbed.
            const HWND fg = GetForegroundWindow();
            const bool gameActive = (fg == s_gameHwnd || fg == s_overlayHwnd);

            const bool wantVisible = gameActive && s_overlayVisible.load(std::memory_order_relaxed);

            if (gameActive && wantVisible)
            {
                // Only reposition when the game window actually moved.
                static RECT s_lastGR{};
                RECT ngr{};
                GetWindowRect(s_gameHwnd, &ngr);
                if (ngr.left != s_lastGR.left || ngr.top != s_lastGR.top || ngr.right != s_lastGR.right ||
                    ngr.bottom != s_lastGR.bottom)
                {
                    SetWindowPos(s_overlayHwnd, HWND_TOP, ngr.left, ngr.top, ngr.right - ngr.left, ngr.bottom - ngr.top,
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
                    SetWindowLongPtrW(s_overlayHwnd, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
                    SetForegroundWindow(s_overlayHwnd);
                }
                else if (!visible && wasVisible)
                {
                    LONG_PTR ex = GetWindowLongPtrW(s_overlayHwnd, GWL_EXSTYLE);
                    SetWindowLongPtrW(s_overlayHwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
                    SetForegroundWindow(s_gameHwnd);
                }
                wasVisible = visible;
            }

            if (!visible)
            {
                // Nothing to draw, sleep longer to save CPU.
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

            // Bridge mouse-button state into ImGui via polling.
            //
            // The game uses SetCapture / RawInput, so WM_LBUTTONDOWN and WM_LBUTTONUP both route to the game window
            // even when the cursor is over our overlay. ImGui_ImplWin32 polls cursor position via GetCursorPos as a
            // fallback (so hover and tooltips keep working without messages) but does NOT poll button state -- buttons
            // rely on wndproc messages we never see. Without this bridge, io.MouseDown is stuck at false and every
            // click on the overlay is ignored.
            //
            // Edge-detect the press: a button only counts as down for ImGui if the up->down transition happened while
            // the cursor was over the overlay. That suppresses two phantom-click classes:
            //   1. The user clicks in the game with LMB held and drags onto the overlay. Raw state is "down" the whole
            //      time but no click was meant for our UI.
            //   2. The user clicks on the overlay, drags out, then releases. The latch stays "down" while dragged out
            //      (so widget drag keeps working); release anywhere clears the latch so ImGui sees the up event.
            //
            // `overOverlay` is hoisted out of the polling block so the adaptive-sleep code below can use it as one of
            // the "user is interacting" signals (cursor over the overlay means we want fast frames even before they
            // click).
            bool overOverlay = false;
            {
                POINT pt;
                GetCursorPos(&pt);
                RECT wr;
                GetWindowRect(s_overlayHwnd, &wr);
                overOverlay = pt.x >= wr.left && pt.x < wr.right && pt.y >= wr.top && pt.y < wr.bottom;

                // Per-button latches survive across render frames. The "was" pair is the previous-frame raw state and
                // is the basis for rising-edge detection; the "latch" pair is the cooked value forwarded to ImGui (see
                // comment block above for the state machine).
                static bool s_lLatch = false, s_lWas = false;
                static bool s_rLatch = false, s_rWas = false;
                static bool s_mLatch = false, s_mWas = false;
                auto poll = [&](int idx, int vk, bool &latch, bool &was) -> void
                {
                    const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
                    if (!now)
                        latch = false;
                    else if (!was && overOverlay)
                        latch = true;
                    was = now;
                    io.AddMouseButtonEvent(idx, latch);
                };
                poll(0, VK_LBUTTON, s_lLatch, s_lWas);
                poll(1, VK_RBUTTON, s_rLatch, s_rWas);
                poll(2, VK_MBUTTON, s_mLatch, s_mWas);
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            draw_overlay();

            // Snapshot interactivity inside the frame -- IsAnyItem* is only defined between NewFrame and EndFrame, and
            // ImGui::Render() below calls EndFrame internally.
            const bool uiActive = ImGui::IsAnyItemActive() || ImGui::IsAnyItemHovered() || io.WantTextInput;

            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            // Tight dirty rect = union of last frame's drawn rect and this frame's drawn rect, with a small border for
            // anti-aliased edges. Falls back to full surface when ImGui drew nothing (collapsed window). Bringing the
            // staging readback + premultiply + ULWI down to that area is the bulk of the standalone responsiveness win
            // on high-res displays.
            RECT cur{};
            const bool gotBounds = compute_draw_bounds(ImGui::GetDrawData(), cur);
            if (!gotBounds)
            {
                cur.left = 0;
                cur.top = 0;
                cur.right = static_cast<LONG>(s_width);
                cur.bottom = static_cast<LONG>(s_height);
            }
            RECT dirty = cur;
            if (prevDirty.right > prevDirty.left && prevDirty.bottom > prevDirty.top)
            {
                UnionRect(&dirty, &cur, &prevDirty);
            }
            constexpr LONG kEdgePad = 4;
            dirty.left = (std::max<LONG>)(0, dirty.left - kEdgePad);
            dirty.top = (std::max<LONG>)(0, dirty.top - kEdgePad);
            dirty.right = (std::min<LONG>)(static_cast<LONG>(s_width), dirty.right + kEdgePad);
            dirty.bottom = (std::min<LONG>)(static_cast<LONG>(s_height), dirty.bottom + kEdgePad);
            blit_to_screen(dirty);
            prevDirty = cur;

            // Adaptive sleep.
            //   - Interactive (cursor over overlay, active item, hover, text input, or cursor moved this frame): 4 ms
            //     -> ~200 Hz cap so clicks and hovers feel native.
            //   - Idle (overlay visible but user looking elsewhere):
            //     33 ms -> ~30 Hz, plenty for the static UI while
            //     keeping CPU/GDI use low.
            // timeBeginPeriod(1) above is what lets Sleep(4) actually be 4 ms instead of rounding to a scheduler tick.
            const bool mouseMoved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
            const bool interactive = overOverlay || uiActive || mouseMoved;
            Sleep(interactive ? 4 : 33);
        }

        timeEndPeriod(1);

        // Cleanup.
        s_initialised = false;
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        release_targets();
        if (s_context)
        {
            s_context->Release();
            s_context = nullptr;
        }
        if (s_device)
        {
            s_device->Release();
            s_device = nullptr;
        }
        DestroyWindow(s_overlayHwnd);
        UnregisterClassW(L"TransmogOverlay", GetModuleHandleW(nullptr));
        s_overlayHwnd = nullptr;
        return 0;
    }

    // --- Public API ---

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
        // Debounce: ignore toggles within 300ms of the last one. Prevents key-repeat and polling overlap from rapidly
        // flipping the overlay on/off/on.
        static std::atomic<int64_t> s_lastToggleMs{0};
        const int64_t now = steady_ms();
        const int64_t last = s_lastToggleMs.load(std::memory_order_relaxed);
        if (now - last < 300)
            return;
        s_lastToggleMs.store(now, std::memory_order_relaxed);

        s_overlayVisible.store(!s_overlayVisible.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

} // namespace Transmog
