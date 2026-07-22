#define NORMALIZE(val, minVal, maxVal) ((float)(val) - (float)(minVal)) / ((float)(maxVal) - (float)(minVal))

IDirect3DTexture8 *RenderDevice::imageTexture   = NULL;
IDirect3D8 *RenderDevice::d3dContext             = NULL;
IDirect3DDevice8 *RenderDevice::d3dDevice        = NULL;
IDirect3DVertexBuffer8 *RenderDevice::vertexBuffer = NULL;
IDirect3DTexture8 *RenderDevice::screenTextures[SCREEN_COUNT];
D3DVIEWPORT8 RenderDevice::viewport;

bool RenderDevice::useFrequency = false;
LARGE_INTEGER RenderDevice::performanceCount, RenderDevice::frequency, RenderDevice::initialFrequency, RenderDevice::curFrequency;

bool RenderDevice::Init()
{
    if (!SetupRendering())
        return false;
    if (!AudioDevice::Init())
        return false;
    InitInputDevices();

    /* Bare-minimum rendering test: FlipScreen in tight loop */
    while (1) {
        FlipScreen();
    }
    return true;
}

bool RenderDevice::SetupRendering()
{
    d3dContext = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3dContext) {
        PrintLog(PRINT_NORMAL, "ERROR: Direct3DCreate8 failed!");
        return false;
    }

    PrintLog(PRINT_NORMAL, "[XBOX] Direct3DCreate8 OK");

    if (!InitGraphicsAPI() || !InitShaders())
        return false;

    int32 size = videoSettings.pixWidth >= SCREEN_YSIZE ? videoSettings.pixWidth : SCREEN_YSIZE;
    scanlines  = (ScanlineInfo *)malloc(size * sizeof(ScanlineInfo));
    memset(scanlines, 0, size * sizeof(ScanlineInfo));

    videoSettings.windowState = WINDOWSTATE_ACTIVE;
    videoSettings.dimMax      = 1.0;
    videoSettings.dimPercent  = 1.0;

    return true;
}

bool RenderDevice::InitGraphicsAPI()
{
    videoSettings.shaderSupport = false;

    viewSize.x = 640;
    viewSize.y = 480;

    int32 maxPixHeight = 0;
    int32 screenWidth  = 0;
    for (int32 s = 0; s < 4; ++s) {
        if (videoSettings.pixHeight > maxPixHeight)
            maxPixHeight = videoSettings.pixHeight;

        screens[s].size.y = videoSettings.pixHeight;

        float viewAspect = viewSize.x / viewSize.y;
        screenWidth = (int32)((viewAspect * videoSettings.pixHeight) + 3) & 0xFFFFFFFC;
        if (screenWidth < videoSettings.pixWidth)
            screenWidth = videoSettings.pixWidth;

        if (customSettings.maxPixWidth && screenWidth > customSettings.maxPixWidth)
            screenWidth = customSettings.maxPixWidth;

        memset(&screens[s].frameBuffer, 0, sizeof(screens[s].frameBuffer));
        SetScreenSize(s, screenWidth, screens[s].size.y);
    }

    pixelSize.x = screens[0].size.x;
    pixelSize.y = screens[0].size.y;

    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth  = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount  = 1;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.FullScreen_RefreshRateInHz      = D3DPRESENT_RATE_DEFAULT;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    HRESULT hr = IDirect3D8_CreateDevice(d3dContext, 0, D3DDEVTYPE_HAL, NULL,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d3dDevice);
    if (FAILED(hr) || !d3dDevice) {
        PrintLog(PRINT_NORMAL, "ERROR: CreateDevice failed!");
        return false;
    }

    PrintLog(PRINT_NORMAL, "[XBOX] CreateDevice OK");

    IDirect3DDevice8_SetRenderState(d3dDevice, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(d3dDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice8_SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice8_SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice8_SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    IDirect3DDevice8_SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice8_SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    IDirect3DDevice8_SetTextureStageState(d3dDevice, 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice8_SetTextureStageState(d3dDevice, 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice8_SetTextureStageState(d3dDevice, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice8_SetTextureStageState(d3dDevice, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);

    D3DMATRIX matrix;
    ZeroMemory(&matrix, sizeof(matrix));
    matrix._11 = 1.0f; matrix._22 = 1.0f; matrix._33 = 1.0f; matrix._44 = 1.0f;
    IDirect3DDevice8_SetTransform(d3dDevice, D3DTS_WORLD, &matrix);
    IDirect3DDevice8_SetTransform(d3dDevice, D3DTS_VIEW, &matrix);

    float2 textureSize = { 1024.0f, 512.0f };
    if (screenWidth <= 512 && maxPixHeight <= 256) {
        textureSize.x = 512.0f;
        textureSize.y = 256.0f;
    }

    for (int32 s = 0; s < SCREEN_COUNT; ++s) {
        hr = IDirect3DDevice8_CreateTexture(d3dDevice, (UINT)textureSize.x, (UINT)textureSize.y, 1,
            D3DUSAGE_DYNAMIC, D3DFMT_R5G6B5, D3DPOOL_DEFAULT, &screenTextures[s]);
        if (FAILED(hr) || !screenTextures[s]) {
            PrintLog(PRINT_NORMAL, "ERROR: failed to create screen texture %d", s);
            return false;
        }
    }

    hr = IDirect3DDevice8_CreateTexture(d3dDevice, RETRO_VIDEO_TEXTURE_W, RETRO_VIDEO_TEXTURE_H, 1,
        D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &imageTexture);
    if (FAILED(hr) || !imageTexture) {
        PrintLog(PRINT_NORMAL, "ERROR: failed to create image texture");
        return false;
    }

    PrintLog(PRINT_NORMAL, "[XBOX] Textures created OK");

    HRESULT vbResult = IDirect3DDevice8_CreateVertexBuffer(d3dDevice,
        sizeof(rsdkVertexBuffer), D3DUSAGE_WRITEONLY,
        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1,
        D3DPOOL_DEFAULT, &vertexBuffer);
    if (FAILED(vbResult) || !vertexBuffer) {
        PrintLog(PRINT_NORMAL, "ERROR: failed to create vertex buffer (0x%08X)", vbResult);
        return false;
    }
    PrintLog(PRINT_NORMAL, "[XBOX] VertexBuffer OK: size=%d bytes", sizeof(rsdkVertexBuffer));

    viewport.X      = 0;
    viewport.Y      = 0;
    viewport.Width  = 640;
    viewport.Height = 480;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    IDirect3DDevice8_SetViewport(d3dDevice, &viewport);

    float pixAspect = (float)pixelSize.x / pixelSize.y;
    if ((viewSize.x / viewSize.y) <= (pixAspect + 0.1f)) {
        viewSize.y = (pixelSize.y / pixelSize.x) * viewSize.x;
        viewport.Y = (long)((480.0f - viewSize.y) * 0.5f);
        viewport.Height = (DWORD)viewSize.y;
    }
    else {
        viewSize.x = pixAspect * viewSize.y;
        viewport.X = (long)((640.0f - viewSize.x) * 0.5f);
        viewport.Width = (DWORD)viewSize.x;
    }
    IDirect3DDevice8_SetViewport(d3dDevice, &viewport);

    D3DMATRIX proj;
    ZeroMemory(&proj, sizeof(proj));
    proj._11 = 2.0f / viewSize.x;
    proj._22 = -2.0f / viewSize.y;
    proj._33 = 1.0f;
    proj._41 = -1.0f;
    proj._42 = 1.0f;
    proj._44 = 1.0f;
    IDirect3DDevice8_SetTransform(d3dDevice, D3DTS_PROJECTION, &proj);

    lastShaderID = -1;
    InitVertexBuffer();
    engine.inFocus          = 1;
    videoSettings.viewportX = viewport.X;
    videoSettings.viewportY = viewport.Y;
    videoSettings.viewportW = 1.0f / viewSize.x;
    videoSettings.viewportH = 1.0f / viewSize.y;

    PrintLog(PRINT_NORMAL, "[XBOX] InitGraphicsAPI complete");
    return true;
}

void RenderDevice::InitVertexBuffer()
{
    RenderVertex vertBuffer[sizeof(rsdkVertexBuffer) / sizeof(RenderVertex)];
    memcpy(vertBuffer, rsdkVertexBuffer, sizeof(rsdkVertexBuffer));

    int32 vertCount = (RETRO_REV02 ? 60 : 24) - 6;

    for (int32 v = 0; v < vertCount; ++v) {
        RenderVertex *vertex = &vertBuffer[v];
        vertex->pos.x        = NORMALIZE(vertex->pos.x, -1.0, 1.0) * videoSettings.pixWidth;
        vertex->pos.y        = (1.0 - NORMALIZE(vertex->pos.y, -1.0, 1.0)) * SCREEN_YSIZE;

        if (vertex->tex.x)
            vertex->tex.x = screens[0].size.x * (1.0 / textureSize.x);

        if (vertex->tex.y)
            vertex->tex.y = screens[0].size.y * (1.0 / textureSize.y);
    }

    for (int32 v = 0; v < 6; ++v) {
        RenderVertex *vertex = &vertBuffer[vertCount + v];
        vertex->pos.x = NORMALIZE(vertex->pos.x, -1.0, 1.0) * videoSettings.pixWidth;
        vertex->pos.y = (1.0 - NORMALIZE(vertex->pos.y, -1.0, 1.0)) * SCREEN_YSIZE;

        if (vertex->tex.x)
            vertex->tex.x = 1.0f;
        if (vertex->tex.y)
            vertex->tex.y = 1.0f;
    }

    void *data = NULL;
    if (SUCCEEDED(IDirect3DVertexBuffer8_Lock(vertexBuffer, 0, 0, (BYTE **)&data, 0))) {
        memcpy(data, vertBuffer, sizeof(rsdkVertexBuffer));
        IDirect3DVertexBuffer8_Unlock(vertexBuffer);
    }
}

void RenderDevice::CopyFrameBuffer()
{
#if RETRO_PLATFORM == RETRO_XBOX
    return;
#endif

    static bool firstFrame = true;
    IDirect3DDevice8_SetTexture(d3dDevice, 0, NULL);

    for (int32 s = 0; s < videoSettings.screenCount; ++s) {
        D3DLOCKED_RECT lr;
        HRESULT hr = IDirect3DTexture8_LockRect(screenTextures[s], 0, &lr, NULL, 0);
        if (FAILED(hr)) {
            if (firstFrame) PrintLog(PRINT_NORMAL, "[XBOX] LockRect(%d) FAIL: 0x%08X", s, hr);
            continue;
        }

        uint16 *frameBuffer = screens[s].frameBuffer;
        uint16 *pixels      = (uint16 *)lr.pBits;
        int32 pitch         = lr.Pitch >> 1;

        if (firstFrame) {
            PrintLog(PRINT_NORMAL, "[XBOX] LockRect(%d) OK: pBits=%p pitch=%d screenCount=%d",
                s, lr.pBits, lr.Pitch, videoSettings.screenCount);
        }

        for (int32 y = 0; y < SCREEN_YSIZE; ++y) {
            int32 pixelCount = screens[s].size.x >> 4;
            for (int32 x = 0; x < pixelCount; ++x) {
                pixels[0]  = frameBuffer[0];  pixels[1]  = frameBuffer[1];
                pixels[2]  = frameBuffer[2];  pixels[3]  = frameBuffer[3];
                pixels[4]  = frameBuffer[4];  pixels[5]  = frameBuffer[5];
                pixels[6]  = frameBuffer[6];  pixels[7]  = frameBuffer[7];
                pixels[8]  = frameBuffer[8];  pixels[9]  = frameBuffer[9];
                pixels[10] = frameBuffer[10]; pixels[11] = frameBuffer[11];
                pixels[12] = frameBuffer[12]; pixels[13] = frameBuffer[13];
                pixels[14] = frameBuffer[14]; pixels[15] = frameBuffer[15];
                frameBuffer += 16;
                pixels += 16;
            }
            pixels += pitch - screens[s].size.x;
        }

        IDirect3DTexture8_UnlockRect(screenTextures[s], 0);
    }
    firstFrame = false;
}

static int32 g_flipFrame = 0;
static bool g_sceneActive = false;

void RenderDevice::FlipScreen()
{
    HRESULT hr;
    static const DWORD colors[] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFF00FFFF, 0xFFFF00FF };
    DWORD color = colors[g_flipFrame % 6];

    /* Test: try BeginScene twice to see if it fails after EndScene */
    hr = IDirect3DDevice8_BeginScene(d3dDevice);
    if (g_flipFrame < 3) PrintLog(PRINT_NORMAL, "[XBOX] BS1 frame %d: 0x%08X", g_flipFrame, hr);
    if (FAILED(hr)) { g_flipFrame++; return; }

    IDirect3DDevice8_EndScene(d3dDevice);

    hr = IDirect3DDevice8_BeginScene(d3dDevice);
    if (g_flipFrame < 3) PrintLog(PRINT_NORMAL, "[XBOX] BS2 frame %d: 0x%08X", g_flipFrame, hr);
    if (FAILED(hr)) { g_flipFrame++; return; }

    IDirect3DDevice8_Clear(d3dDevice, 0, NULL, D3DCLEAR_TARGET, color, 1.0f, 0);

    IDirect3DDevice8_EndScene(d3dDevice);
    IDirect3DDevice8_Present(d3dDevice, NULL, NULL, NULL, NULL);

    g_flipFrame++;
}

void RenderDevice::Release(bool32 isRefresh)
{
    for (int32 s = 0; s < shaderCount; ++s) {
        ShaderEntry *shader = &shaderList[s];
        shader->unused = NULL;
    }
    shaderCount = 0;

    if (imageTexture) {
        IDirect3DTexture8_Release(imageTexture);
        imageTexture = NULL;
    }

    for (int32 s = 0; s < SCREEN_COUNT; ++s) {
        if (screenTextures[s]) {
            IDirect3DTexture8_Release(screenTextures[s]);
            screenTextures[s] = NULL;
        }
    }

    if (vertexBuffer) {
        IDirect3DVertexBuffer8_Release(vertexBuffer);
        vertexBuffer = NULL;
    }

    if (d3dDevice) {
        IDirect3DDevice8_Release(d3dDevice);
        d3dDevice = NULL;
    }

    if (d3dContext) {
        IDirect3D8_Release(d3dContext);
        d3dContext = NULL;
    }

    if (!isRefresh) {
        free(scanlines);
        scanlines = NULL;
    }
}

bool RenderDevice::InitShaders()
{
    shaderCount = 0;
    int32 maxShaders = 0;

    return true;
}

void RenderDevice::LoadShader(const char *fileName, bool32 linear) {}

void RenderDevice::SetupImageTexture(int32 width, int32 height, uint8 *imagePixels)
{
    if (!imageTexture)
        return;

    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture8_LockRect(imageTexture, 0, &lr, NULL, 0)))
        return;

    uint32 *pixels = (uint32 *)lr.pBits;
    for (int32 y = 0; y < height; ++y) {
        memcpy((uint8 *)pixels + y * lr.Pitch, &imagePixels[y * width * sizeof(uint32)], width * sizeof(uint32));
    }

    IDirect3DTexture8_UnlockRect(imageTexture, 0);
}

void RenderDevice::SetupVideoTexture_YUV420(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                             int32 strideV) {}
void RenderDevice::SetupVideoTexture_YUV422(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                             int32 strideV) {}
void RenderDevice::SetupVideoTexture_YUV444(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                             int32 strideV) {}

void RenderDevice::InitFPSCap()
{
    useFrequency = true;
    if (!QueryPerformanceFrequency(&frequency))
        useFrequency = false;

    initialFrequency.QuadPart = frequency.QuadPart / videoSettings.refreshRate;
    QueryPerformanceCounter(&performanceCount);
}

bool RenderDevice::CheckFPSCap()
{
    LARGE_INTEGER newCount;

    if (useFrequency) {
        if (!QueryPerformanceCounter(&newCount))
            return true;

        if (performanceCount.QuadPart > newCount.QuadPart)
            return false;

        curFrequency.QuadPart = newCount.QuadPart;
    }

    return true;
}

void RenderDevice::UpdateFPSCap()
{
    performanceCount.QuadPart = curFrequency.QuadPart + initialFrequency.QuadPart;
}

bool RenderDevice::ProcessEvents()
{
    static SDL_Event sdlEvent;
    while (SDL_PollEvent(&sdlEvent)) {
        switch (sdlEvent.type) {
            case SDL_QUIT:
                isRunning = false;
                return false;
            default: break;
        }
    }
    return isRunning;
}
