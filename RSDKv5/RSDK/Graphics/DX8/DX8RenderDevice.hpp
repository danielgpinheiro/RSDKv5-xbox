class RenderDevice : public RenderDeviceBase
{
public:
    struct DisplayModeEntry {
        UINT width;
        UINT height;
        UINT refresh_rate;
    };

    struct WindowInfo {
        DisplayModeEntry *displays;
        D3DVIEWPORT8 viewport;
    };
    static WindowInfo displayInfo;

    static bool Init();
    static void CopyFrameBuffer();
    static void FlipScreen();
    static void Release(bool32 isRefresh);

    static void RefreshWindow() {}
    static void GetWindowSize(int32 *width, int32 *height) { *width = 640; *height = 480; }

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

    static bool InitShaders();
    static void LoadShader(const char *fileName, bool32 linear);

    static inline void SetWindowTitle() {}
    static inline void ShowCursor(bool32 shown) {}
    static inline bool GetCursorPos(Vector2 *pos) { return false; }

    static IDirect3DTexture8 *imageTexture;

    static IDirect3D8 *d3dContext;
    static IDirect3DDevice8 *d3dDevice;

private:
    static bool SetupRendering();
    static void InitVertexBuffer();
    static bool InitGraphicsAPI();

    static bool useFrequency;

    static LARGE_INTEGER performanceCount, frequency, initialFrequency, curFrequency;

    static IDirect3DVertexBuffer8 *vertexBuffer;
    static IDirect3DTexture8 *screenTextures[SCREEN_COUNT];
    static D3DVIEWPORT8 viewport;
};

struct ShaderEntry : public ShaderEntryBase {
    void *unused;
};
