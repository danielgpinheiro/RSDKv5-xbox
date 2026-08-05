using ShaderEntry = ShaderEntryBase;

class RenderDevice : public RenderDeviceBase
{
public:
    struct WindowInfo {
        // SDL3's SDL_DisplayMode layout differs from SDL2's, so no union trickery here —
        // core code (Drawing.cpp) only reads width/height/refresh_rate
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

    static inline void SetWindowTitle() { SDL_SetWindowTitle(window, gameVerInfo.gameTitle); };

    static SDL_Window *window;
    static SDL_Renderer *renderer;
    static SDL_Texture *screenTexture[SCREEN_COUNT];

    static SDL_Texture *imageTexture;

    // --- 3D GPU offload (special stages) --------------------------------------
    // Draw3DScene emits its faces here (as GPU triangles) instead of software-
    // rasterizing into the framebuffer; they're composited over the framebuffer at
    // present time (background -> GPU 3D -> foreground). Compile-time fallback: set
    // false to force the original software DrawFace path everywhere.
    static bool gpu3DEnabled;
    // True when the 3D layer should be offloaded this frame/screen (single-screen,
    // screen 0). Draw3DScene checks this once to pick the GPU or CPU path.
    static bool Use3DOffload();
    // Mirror DrawFace / DrawBlendedFace, but append triangles to the 3D batch.
    static void Add3DFace(Vector2 *vertices, int32 vertCount, int32 r, int32 g, int32 b, int32 alpha, int32 inkEffect);
    static void Add3DBlendedFace(Vector2 *vertices, uint32 *colors, int32 vertCount, int32 alpha, int32 inkEffect);
    // Special-stage scaled-billboard offload: bake the frame and draw a textured GPU
    // quad from the software rasterizer's 4 transformed corners. True = handled on GPU.
    static bool DrawSpriteGPU(int32 *posX, int32 *posY, int32 sprX, int32 sprY, int32 width, int32 height, int32 sheetID, int32 inkEffect,
                              int32 alpha);
    // Unscaled (DrawSpriteFlipped) variant — axis-aligned quad. True = handled on GPU.
    static bool DrawSpriteFlippedGPU(int32 x, int32 y, int32 width, int32 height, int32 sprX, int32 sprY, int32 direction, int32 sheetID,
                                     int32 inkEffect, int32 alpha);

private:
    static bool SetupRendering();
    static void InitVertexBuffer();
    static bool InitGraphicsAPI();

    static void GetDisplays();

    static void ProcessEvent(SDL_Event event);

    static void ConvertYUVToImageTexture(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                         int32 strideV, int32 chromaShiftX, int32 chromaShiftY, uint8 format);

    static uint32 displayModeIndex;
    static int32 displayModeCount;

    static unsigned long long targetFreq;
    static unsigned long long curTicks;
    static unsigned long long prevTicks;

    static RenderVertex vertexBuffer[!RETRO_REV02 ? 24 : 60];

    // thingo majigo for handling video/image swapping
    static uint8 lastTextureFormat;
};
