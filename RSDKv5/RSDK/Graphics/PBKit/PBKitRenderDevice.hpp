
// ============================================================================
// PBKit render device for the original Xbox — Tier 1 hardware renderer.
//
// Unlike the SDL3 device (which presents the software framebuffer through SDL's
// "nxdk_xgu" renderer), this device drives the NV2A directly via pbkit + xgu and
// OWNS the frame: it targets the back buffer, pushes geometry, and swaps on vblank
// itself — there is NO SDL_Renderer. SDL3 is retained ONLY for input/events (the
// window handle + SDL_PollEvent pump) and nxdk-audio is unchanged.
//
// Stage 1 (this file): stand up that pbkit frame ownership. The engine still
// software-rasterizes into screens[].frameBuffer (RGB565); here we upload that
// framebuffer to a linear RGB565 NV2A texture and present it as one fullscreen
// quad. Later stages progressively move drawing onto the GPU (8bpp I8 + CLUT
// paletted textures) and stop touching the framebuffer. The GPU-offload entry
// points below are Stage-1 stubs so the existing #if RETRO_PLATFORM==RETRO_XBOX
// hooks in Drawing.cpp / Scene3D.cpp keep compiling and fall back to software.
// ============================================================================

using ShaderEntry = ShaderEntryBase;

struct TileLayer; // defined later in Scene.hpp; used by DrawLayerGPU below (pointer only)

class RenderDevice : public RenderDeviceBase
{
public:
    // Mirrors the SDL3 device layout — shared Drawing.cpp (GetDisplays / refresh-rate
    // enumeration) reads RenderDevice::displayInfo.displays[].width/height/refresh_rate.
    struct WindowInfo {
        struct {
            int32 width;
            int32 height;
            int32 refresh_rate;
        } *displays;
        SDL_Rect viewport;
    };
    static WindowInfo displayInfo;

    static bool Init();
    static void CopyFrameBuffer();
    static void FlipScreen();
    static void Release(bool32 isRefresh);

    static void RefreshWindow();
    static void GetWindowSize(int32 *width, int32 *height);

    static void SetupImageTexture(int32 width, int32 height, uint8 *imagePixels);
    static void SetupVideoTexture_YUV420(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                         int32 strideV);
    static void SetupVideoTexture_YUV422(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                         int32 strideV);
    static void SetupVideoTexture_YUV444(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                         int32 strideV);

    static bool ProcessEvents();

    static void InitFPSCap();
    static bool CheckFPSCap();
    static void UpdateFPSCap();
    // Retarget the busy-wait present cadence (e.g. 30 in the special stage, 60 elsewhere).
    static void SetFPSTarget(int32 fps);

    static bool InitShaders();
    static void LoadShader(const char *fileName, bool32 linear);

    // No cursor on Xbox
    static inline void ShowCursor(bool32 shown) { (void)shown; }
    static inline bool GetCursorPos(Vector2 *pos) { return false; };

    static inline void SetWindowTitle()
    {
        if (window)
            SDL_SetWindowTitle(window, gameVerInfo.gameTitle);
    };

    // SDL3 window kept for input/events only (no SDL_Renderer)
    static SDL_Window *window;

    // --- GPU offload (Stage 1 stubs; implemented in Stage 2/3) -----------------
    // Kept so the existing special-stage offload hooks (Drawing.cpp DrawSprite*,
    // Scene3D.cpp Add3DFace) compile and fall back to the software rasterizer.
    static bool gpu3DEnabled;
    static bool Use3DOffload();
    static void Add3DFace(Vector2 *vertices, int32 vertCount, int32 r, int32 g, int32 b, int32 alpha, int32 inkEffect);
    static void Add3DBlendedFace(Vector2 *vertices, uint32 *colors, int32 vertCount, int32 alpha, int32 inkEffect);
    static bool DrawSpriteGPU(int32 *posX, int32 *posY, int32 sprX, int32 sprY, int32 width, int32 height, int32 sheetID, int32 inkEffect,
                              int32 alpha);
    static bool DrawSpriteFlippedGPU(int32 x, int32 y, int32 width, int32 height, int32 sprX, int32 sprY, int32 direction, int32 sheetID,
                                     int32 inkEffect, int32 alpha);
    // 2D primitives as untextured colored GPU polys (Stage 3). Return true = handled.
    static bool DrawRectangleGPU(int32 x, int32 y, int32 width, int32 height, uint32 color, int32 alpha, int32 inkEffect);
    // Fullscreen per-channel alpha fade (Mania zone transition; Zone.c FillScreen). Handled = true.
    static bool DrawFillScreenGPU(uint32 color, int32 alphaR, int32 alphaG, int32 alphaB);
    // Line (thin quad) + filled circle (fan) + circle outline (ring). Endpoints/center are
    // already in screen pixels. Return true = handled (else software fallback).
    static bool DrawLineGPU(int32 x1, int32 y1, int32 x2, int32 y2, uint32 color, int32 alpha, int32 inkEffect);
    static bool DrawCircleGPU(int32 x, int32 y, int32 radius, uint32 color, int32 alpha, int32 inkEffect);
    static bool DrawCircleOutlineGPU(int32 x, int32 y, int32 innerRadius, int32 outerRadius, uint32 color, int32 alpha, int32 inkEffect);
    static bool DrawFaceGPU(Vector2 *vertices, int32 vertCount, int32 r, int32 g, int32 b, int32 alpha, int32 inkEffect);
    static bool DrawBlendedFaceGPU(Vector2 *vertices, uint32 *colors, int32 vertCount, int32 alpha, int32 inkEffect);
    // Re-upload animated tiles (DrawAniTile) into the GPU tileset atlas so they animate.
    static void UpdateAniTileGPU(int32 tileIndex, int32 cnt);
    // Tile layers as GPU quads (Stage 4). Return true = handled (else software fallback).
    static bool DrawLayerGPU(TileLayer *layer);
    // Rotozoom (Mode-7) floor as GPU strip quads (Stage 5). Return true = handled.
    static bool DrawLayerRotozoomGPU(TileLayer *layer);
    // Deformed sprite (water/heat-haze) as GPU per-scanline strips. Return true = handled.
    static bool DrawDeformedSpriteGPU(int32 sheetID, int32 inkEffect, int32 alpha);

private:
    static bool SetupRendering();
    static void InitVertexBuffer();
    static bool InitGraphicsAPI();

    static void GetDisplays();

    static void ProcessEvent(SDL_Event event);

    static uint32 displayModeIndex;
    static int32 displayModeCount;

    static unsigned long long targetFreq;
    static unsigned long long curTicks;
    static unsigned long long prevTicks;
};
