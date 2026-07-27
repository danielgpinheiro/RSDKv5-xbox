
// SDL3 render device for the original Xbox (nxdk-sdl3) — presents via the "nxdk_xgu"
// hardware renderer (pbkit + xgu) so the NV2A GPU handles RGB565 conversion, scaling,
// letterboxing and dimming that the SDL2 backend did on the CPU via XVideoGetFB().

SDL_Window *RenderDevice::window     = nullptr;
SDL_Renderer *RenderDevice::renderer = nullptr;
SDL_Texture *RenderDevice::screenTexture[SCREEN_COUNT];

SDL_Texture *RenderDevice::imageTexture = nullptr;

uint32 RenderDevice::displayModeIndex = 0;
int32 RenderDevice::displayModeCount  = 0;

unsigned long long RenderDevice::targetFreq = 0;
unsigned long long RenderDevice::curTicks   = 0;
unsigned long long RenderDevice::prevTicks  = 0;

RenderVertex RenderDevice::vertexBuffer[!RETRO_REV02 ? 24 : 60];

uint8 RenderDevice::lastTextureFormat = -1;

#define NORMALIZE(val, minVal, maxVal) ((float)(val) - (float)(minVal)) / ((float)(maxVal) - (float)(minVal))

static void SDLCALL SDLLogOutput(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    PrintLog(PRINT_NORMAL, "SDL: %s", message);
}

bool RenderDevice::Init()
{
    SDL_SetLogOutputFunction(SDLLogOutput, NULL);

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        PrintLog(PRINT_NORMAL, "ERROR: SDL_InitSubSystem failed: %s", SDL_GetError());
        return false;
    }

    videoSettings.windowed = false;

    // Request the 640x480 mode SetXboxResolution() already established — the nxdk
    // video driver forces fullscreen and re-asserts the display mode itself
    VIDEO_MODE vm = XVideoGetMode();
    window        = SDL_CreateWindow(gameVerInfo.gameTitle, vm.width, vm.height, SDL_WINDOW_FULLSCREEN);
    if (!window) {
        PrintLog(PRINT_NORMAL, "ERROR: failed to create window: %s", SDL_GetError());
        return false;
    }

    SDL_GetWindowSize(window, &videoSettings.windowWidth, &videoSettings.windowHeight);

    PrintLog(PRINT_NORMAL, "w: %d h: %d windowed: %d", videoSettings.windowWidth, videoSettings.windowHeight, videoSettings.windowed);
    if (!SetupRendering() || !AudioDevice::Init())
        return false;

    InitInputDevices();
    return true;
}

void RenderDevice::CopyFrameBuffer()
{
    int32 pitch    = 0;
    uint16 *pixels = NULL;

    for (int32 s = 0; s < videoSettings.screenCount; ++s) {
        if (screenTexture[s] && SDL_LockTexture(screenTexture[s], NULL, (void **)&pixels, &pitch)) {
            uint16 *frameBuffer = screens[s].frameBuffer;
            for (int32 y = 0; y < SCREEN_YSIZE; ++y) {
                memcpy(pixels, frameBuffer, screens[s].size.x * sizeof(uint16));
                frameBuffer += screens[s].pitch;
                pixels += pitch / sizeof(uint16);
            }

            SDL_UnlockTexture(screenTexture[s]);
        }
    }
}

#if RETRO_PLATFORM == RETRO_XBOX
extern "C" unsigned int SDL_XBOXAUDIO_underruns; // from the nxdk-sdl3 audio driver

// Once-per-second perf stats over serial (visible in xbwatson): frames presented,
// worst frame-to-frame gap, and audio underrun delta — data for slowdown hunting
static void LogFrameStats()
{
    static uint64 windowStart   = 0;
    static uint64 lastFlip      = 0;
    static uint32 frames        = 0;
    static uint64 maxGap        = 0;
    static uint32 lastUnderruns = 0;

    uint64 now = SDL_GetTicks();
    if (!windowStart) {
        windowStart = now;
        lastFlip    = now;
        return;
    }

    ++frames;
    if (now - lastFlip > maxGap)
        maxGap = now - lastFlip;
    lastFlip = now;

    if (now - windowStart >= 1000) {
        uint32 underruns = SDL_XBOXAUDIO_underruns;
        PrintLog(PRINT_NORMAL, "perf: %u fps, max frame gap %u ms, audio underruns +%u", frames, (uint32)maxGap, underruns - lastUnderruns);
        lastUnderruns = underruns;
        windowStart   = now;
        frames        = 0;
        maxGap        = 0;
    }
}
#endif

void RenderDevice::FlipScreen()
{
#if RETRO_PLATFORM == RETRO_XBOX
    LogFrameStats();
#endif

    if (windowRefreshDelay > 0) {
        windowRefreshDelay--;
        if (!windowRefreshDelay)
            UpdateGameWindow();
        return;
    }

    float dimAmount = videoSettings.dimMax * videoSettings.dimPercent;

    // Clear the screen. This is needed to keep the
    // pillarboxes in fullscreen from displaying garbage data.
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF);
    SDL_RenderClear(renderer);

    int32 startVert = 0;
    SDL_FRect src, dst;

#define _SET_RECTS                                                                                                                                   \
    dst.x = vertexBuffer[startVert].pos.x;                                                                                                           \
    dst.y = vertexBuffer[startVert].pos.y;                                                                                                           \
    dst.w = vertexBuffer[startVert + 2].pos.x - dst.x;                                                                                               \
    dst.h = vertexBuffer[startVert + 2].pos.y - dst.y;                                                                                               \
    src.x = vertexBuffer[startVert].tex.x * textureSize.x;                                                                                           \
    src.y = vertexBuffer[startVert].tex.y * textureSize.y;                                                                                           \
    src.w = vertexBuffer[startVert + 2].tex.x * textureSize.x - src.x;                                                                               \
    src.h = vertexBuffer[startVert + 2].tex.y * textureSize.y - src.y;

    switch (videoSettings.screenCount) {
        default:
        case 0:
#if RETRO_REV02
            startVert = 54;
#else
            startVert = 18;
#endif
            _SET_RECTS;

            // imageTexture is created at the image/video's native size and its UVs span
            // the full texture, so sample all of it (NULL src)
            if (imageTexture)
                SDL_RenderTexture(renderer, imageTexture, NULL, &dst);
            break;

        case 1:
            startVert = 0;
            _SET_RECTS;

            SDL_RenderTexture(renderer, screenTexture[0], &src, &dst);
            break;

        case 2:
#if RETRO_REV02
            startVert = startVertex_2P[0];
#else
            startVert = 6;
#endif
            _SET_RECTS;

            SDL_RenderTexture(renderer, screenTexture[0], &src, &dst);

#if RETRO_REV02
            startVert = startVertex_2P[1];
#else
            startVert = 12;
#endif
            _SET_RECTS;

            SDL_RenderTexture(renderer, screenTexture[1], &src, &dst);
            break;

#if RETRO_REV02
        case 3:
            startVert = startVertex_3P[0];
            _SET_RECTS;

            SDL_RenderTexture(renderer, screenTexture[0], &src, &dst);

            startVert = startVertex_3P[1];
            _SET_RECTS;

            SDL_RenderTexture(renderer, screenTexture[1], &src, &dst);

            startVert = startVertex_3P[2];
            _SET_RECTS;

            SDL_RenderTexture(renderer, screenTexture[2], &src, &dst);

            break;

        case 4:
            startVert = 30;
            _SET_RECTS;

            SDL_RenderTexture(renderer, screenTexture[0], &src, &dst);

            startVert = 36;
            _SET_RECTS;

            SDL_RenderTexture(renderer, screenTexture[1], &src, &dst);

            startVert = 42;
            _SET_RECTS;

            SDL_RenderTexture(renderer, screenTexture[2], &src, &dst);

            startVert = 48;
            _SET_RECTS;

            SDL_RenderTexture(renderer, screenTexture[3], &src, &dst);

            break;
#endif
    }
#undef _SET_RECTS

    if (dimAmount < 1.0f) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF - (uint8)(dimAmount * 0xFF));
        SDL_RenderFillRect(renderer, NULL);
    }

    SDL_RenderPresent(renderer); // nxdk_xgu: triple-buffered, blocks on vblank (60Hz)
}

void RenderDevice::Release(bool32 isRefresh)
{
    for (int32 s = 0; s < SCREEN_COUNT; ++s) {
        if (screenTexture[s])
            SDL_DestroyTexture(screenTexture[s]);
        screenTexture[s] = NULL;
    }

    if (imageTexture)
        SDL_DestroyTexture(imageTexture);
    imageTexture = NULL;

    if (!isRefresh) {
        if (displayInfo.displays)
            free(displayInfo.displays);
        displayInfo.displays = NULL;

        if (renderer)
            SDL_DestroyRenderer(renderer);

        if (window)
            SDL_DestroyWindow(window);

        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);

        if (scanlines)
            free(scanlines);
        scanlines = NULL;
    }
}

void RenderDevice::RefreshWindow()
{
    // Fixed-mode fullscreen console — nothing to re-create at the window level
    videoSettings.windowState = WINDOWSTATE_UNINITIALIZED;

    Release(true);

    GetDisplays();

    if (!InitGraphicsAPI() || !InitShaders())
        return;

    videoSettings.windowState = WINDOWSTATE_ACTIVE;
}

void RenderDevice::InitFPSCap()
{
    targetFreq = SDL_GetPerformanceFrequency() / videoSettings.refreshRate;
    curTicks   = 0;
    prevTicks  = 0;
}
bool RenderDevice::CheckFPSCap()
{
    // Keep the busy-wait cap: it's the only reliable pacing. The XGU present nominally
    // blocks on vblank, but on xemu the GPU interrupt hookup is faked (pbkit
    // KeConnectInterrupt patch), so the present may not block — without this cap the
    // whole game runs at uncapped speed.
    curTicks = SDL_GetPerformanceCounter();
    if (curTicks >= prevTicks + targetFreq)
        return true;

    return false;
}
void RenderDevice::UpdateFPSCap() { prevTicks = curTicks; }

void RenderDevice::InitVertexBuffer()
{
    RenderVertex vertBuffer[sizeof(rsdkVertexBuffer) / sizeof(RenderVertex)];
    memcpy(vertBuffer, rsdkVertexBuffer, sizeof(rsdkVertexBuffer));

    // ignore the last 6 verts, they're scaled to the 1024x512 textures already!
    int32 vertCount = (RETRO_REV02 ? 60 : 24) - 6;

    // Regular in-game screen de-normalization stuff
    for (int32 v = 0; v < vertCount; ++v) {
        RenderVertex *vertex = &vertBuffer[v];
        vertex->pos.x        = NORMALIZE(vertex->pos.x, -1.0, 1.0) * videoSettings.pixWidth;
        vertex->pos.y        = (1.0 - NORMALIZE(vertex->pos.y, -1.0, 1.0)) * SCREEN_YSIZE;

        if (vertex->tex.x)
            vertex->tex.x = screens[0].size.x * (1.0 / textureSize.x);

        if (vertex->tex.y)
            vertex->tex.y = screens[0].size.y * (1.0 / textureSize.y);
    }

    // Fullscreen Image/Video de-normalization stuff
    for (int32 v = 0; v < 6; ++v) {
        RenderVertex *vertex = &vertBuffer[vertCount + v];
        vertex->pos.x        = NORMALIZE(vertex->pos.x, -1.0, 1.0) * videoSettings.pixWidth;
        vertex->pos.y        = (1.0 - NORMALIZE(vertex->pos.y, -1.0, 1.0)) * SCREEN_YSIZE;

        // Set the texture to fill the entire screen
        if (vertex->tex.x)
            vertex->tex.x = 1.0f;

        if (vertex->tex.y)
            vertex->tex.y = 1.0f;
    }

    memcpy(vertexBuffer, vertBuffer, sizeof(vertBuffer));
}

bool RenderDevice::InitGraphicsAPI()
{
    videoSettings.shaderSupport = false;

    viewSize.x = videoSettings.windowWidth;
    viewSize.y = videoSettings.windowHeight;

    int32 maxPixHeight = 0;
#if !RETRO_USE_ORIGINAL_CODE
    int32 screenWidth = 0;
#endif
    for (int32 s = 0; s < 4; ++s) {
        if (videoSettings.pixHeight > maxPixHeight)
            maxPixHeight = videoSettings.pixHeight;

        screens[s].size.y = videoSettings.pixHeight;

        float viewAspect = viewSize.x / viewSize.y;
#if !RETRO_USE_ORIGINAL_CODE
        screenWidth = (int32)((viewAspect * videoSettings.pixHeight) + 3) & 0xFFFFFFFC;
#else
        int32 screenWidth = (int32)((viewAspect * videoSettings.pixHeight) + 3) & 0xFFFFFFFC;
#endif
        // Pin the internal width to pixWidth regardless of the window aspect: the
        // logical presentation size is pixWidth x SCREEN_YSIZE
        screenWidth = videoSettings.pixWidth;

#if !RETRO_USE_ORIGINAL_CODE
        if (customSettings.maxPixWidth && screenWidth > customSettings.maxPixWidth)
            screenWidth = customSettings.maxPixWidth;
#else
        if (screenWidth > DEFAULT_PIXWIDTH)
            screenWidth = DEFAULT_PIXWIDTH;
#endif

        memset(&screens[s].frameBuffer, 0, sizeof(screens[s].frameBuffer));
        SetScreenSize(s, screenWidth, screens[s].size.y);
    }

    pixelSize.x = screens[0].size.x;
    pixelSize.y = screens[0].size.y;

    // 480p LETTERBOX preserves the game's aspect ratio (uniform x/y scale): it
    // fills the 640px width (424x240 -> 640x362, ~1.51x) with bars top/bottom;
    // a 2x integer scale (848 wide) wouldn't fit
    if (!SDL_SetRenderLogicalPresentation(renderer, videoSettings.pixWidth, SCREEN_YSIZE, SDL_LOGICAL_PRESENTATION_LETTERBOX))
        PrintLog(PRINT_NORMAL, "ERROR: SDL_SetRenderLogicalPresentation failed: %s", SDL_GetError());

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

#if !RETRO_USE_ORIGINAL_CODE
    if (screenWidth <= 512 && maxPixHeight <= 256) {
#else
    if (maxPixHeight <= 256) {
#endif
        textureSize.x = 512.0;
        textureSize.y = 256.0;
    }
    else {
        textureSize.x = 1024.0;
        textureSize.y = 512.0;
    }

    // Only 2 of the SCREEN_COUNT (4) screens are ever used on Xbox (2P splitscreen max)
    // — skip the other textures to save RAM. CopyFrameBuffer/FlipScreen tolerate NULLs.
    const int32 maxScreenTextures = 2;
    for (int32 s = 0; s < SCREEN_COUNT; ++s) {
        if (s >= maxScreenTextures) {
            screenTexture[s] = NULL;
            continue;
        }

        screenTexture[s] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, textureSize.x, textureSize.y);

        if (!screenTexture[s]) {
            PrintLog(PRINT_NORMAL, "ERROR: failed to create screen buffer!\nerror msg: %s", SDL_GetError());
            return false;
        }

        SDL_SetTextureScaleMode(screenTexture[s], SDL_SCALEMODE_NEAREST);
    }

    // imageTexture (video/image playback) is created lazily by SetupImageTexture /
    // SetupVideoTexture_* — RAM is too tight on Xbox to keep a 2MB texture around
    imageTexture      = NULL;
    lastTextureFormat = -1;

    lastShaderID = -1;
    InitVertexBuffer();
    engine.inFocus          = 1;
    videoSettings.viewportX = 0;
    videoSettings.viewportY = 0;
    videoSettings.viewportW = 1.0 / viewSize.x;
    videoSettings.viewportH = 1.0 / viewSize.y;

    return true;
}

void RenderDevice::LoadShader(const char *fileName, bool32 linear) { PrintLog(PRINT_NORMAL, "This render device does not support shaders!"); }

bool RenderDevice::InitShaders()
{
    int32 maxShaders = 0;

    if (videoSettings.shaderSupport) {
        LoadShader("None", false);
        LoadShader("Clean", true);
        LoadShader("CRT-Yeetron", true);
        LoadShader("CRT-Yee64", true);

        LoadShader("YUV-420", true);
        LoadShader("YUV-422", true);
        LoadShader("YUV-444", true);
        LoadShader("RGB-Image", true);
        maxShaders = shaderCount;
    }
    else {
        for (int32 s = 0; s < SHADER_COUNT; ++s) shaderList[s].linear = true;

        shaderList[0].linear = videoSettings.windowed ? false : shaderList[0].linear;
        maxShaders           = 1;
        shaderCount          = 1;
    }

    videoSettings.shaderID = videoSettings.shaderID >= maxShaders ? 0 : videoSettings.shaderID;

    return true;
}

bool RenderDevice::SetupRendering()
{
    renderer = SDL_CreateRenderer(window, "nxdk_xgu");
    if (!renderer) {
        PrintLog(PRINT_NORMAL, "ERROR: nxdk_xgu renderer failed (%s), falling back to software", SDL_GetError());
        renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    }

    if (!renderer) {
        PrintLog(PRINT_NORMAL, "ERROR: failed to create renderer: %s", SDL_GetError());
        return false;
    }

    PrintLog(PRINT_NORMAL, "SDL3 renderer: %s", SDL_GetRendererName(renderer));

    GetDisplays();

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

void RenderDevice::GetDisplays()
{
    // Fixed-mode console — one display, one mode, 60Hz
    displayModeIndex = 0;
    displayModeCount = 1;
    displayCount     = 1;

    displayWidth[0]  = videoSettings.windowWidth;
    displayHeight[0] = videoSettings.windowHeight;

    if (displayInfo.displays)
        free(displayInfo.displays);

    displayInfo.displays                 = (decltype(displayInfo.displays))malloc(sizeof(*displayInfo.displays));
    displayInfo.displays[0].width        = videoSettings.windowWidth;
    displayInfo.displays[0].height       = videoSettings.windowHeight;
    displayInfo.displays[0].refresh_rate = 60;

    videoSettings.fsWidth     = 0;
    videoSettings.fsHeight    = 0;
    videoSettings.refreshRate = 60;
}

void RenderDevice::GetWindowSize(int32 *width, int32 *height) { SDL_GetCurrentRenderOutputSize(renderer, width, height); }

void RenderDevice::ProcessEvent(SDL_Event event)
{
    switch (event.type) {
        case SDL_EVENT_GAMEPAD_ADDED: {
            SDL_Gamepad *gamepad = SDL_OpenGamepad(event.gdevice.which);

            if (gamepad != NULL) {
                uint32 id;
                char idBuffer[0x20];
                sprintf_s(idBuffer, sizeof(idBuffer), "SDLDevice%d", (int32)event.gdevice.which);
                GenerateHashCRC(&id, idBuffer);

                if (SKU::InitSDL3InputDevice(id, gamepad) == NULL)
                    SDL_CloseGamepad(gamepad);
            }

            break;
        }

        case SDL_EVENT_GAMEPAD_REMOVED: {
            uint32 id;
            char idBuffer[0x20];
            sprintf_s(idBuffer, sizeof(idBuffer), "SDLDevice%d", (int32)event.gdevice.which);
            GenerateHashCRC(&id, idBuffer);

            RemoveInputDevice(InputDeviceFromID(id));
            break;
        }

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WILL_ENTER_FOREGROUND:
#if RETRO_REV02
            SKU::userCore->focusState = 0;
#endif
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
#if RETRO_REV02
            SKU::userCore->focusState = 1;
#endif
            break;

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        case SDL_EVENT_TERMINATING:
        case SDL_EVENT_QUIT: isRunning = false; break;
    }
}

bool RenderDevice::ProcessEvents()
{
    SDL_Event sdlEvent;

    while (SDL_PollEvent(&sdlEvent)) {
        ProcessEvent(sdlEvent);

        if (!isRunning)
            return false;
    }

    return isRunning;
}

void RenderDevice::SetupImageTexture(int32 width, int32 height, uint8 *imagePixels)
{
    if (lastTextureFormat != SHADER_RGB_IMAGE) {
        if (imageTexture)
            SDL_DestroyTexture(imageTexture);

        imageTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        SDL_SetTextureScaleMode(imageTexture, SDL_SCALEMODE_LINEAR);

        lastTextureFormat = SHADER_RGB_IMAGE;
    }

    int32 texPitch = 0;
    uint32 *pixels = NULL;
    if (SDL_LockTexture(imageTexture, NULL, (void **)&pixels, &texPitch)) {
        int32 pitch           = (texPitch >> 2) - width;
        uint32 *imagePixels32 = (uint32 *)imagePixels;
        for (int32 y = 0; y < height; ++y) {
            for (int32 x = 0; x < width; ++x) {
                *pixels++ = *imagePixels32++;
            }

            pixels += pitch;
        }

        SDL_UnlockTexture(imageTexture);
    }
}

// The nxdk_xgu renderer has no YUV texture formats, so convert on the CPU (BT.601
// integer math) into an RGB565 image texture — RGB565 halves the write-combined
// texture writes vs ARGB8888 and matches the engine's own framebuffer depth.
void RenderDevice::ConvertYUVToImageTexture(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                            int32 strideV, int32 chromaShiftX, int32 chromaShiftY, uint8 format)
{
    // Clamp table covering the BT.601 pre-clamp range (values land in roughly
    // [-282, 537] for valid YUV input)
    static uint8 clampTable[864];
    static bool32 clampReady = false;
    if (!clampReady) {
        for (int32 i = 0; i < 864; ++i) {
            int32 v       = i - 288;
            clampTable[i] = v < 0 ? 0 : (v > 255 ? 255 : (uint8)v);
        }
        clampReady = true;
    }
    const uint8 *clamp = &clampTable[288];

    // Downsample large videos (Mania.ogv is 1024x512) to <=512 wide: a full-size
    // texture doesn't fit in RAM, the display is only ~424x240 logical anyway, and
    // quarter-resolution conversion is 4x cheaper on the CPU.
    int32 downShift = 0;
    while ((width >> downShift) > 512) downShift++;

    const int32 texWidth  = width >> downShift;
    const int32 texHeight = height >> downShift;

    if (lastTextureFormat != format) {
        if (imageTexture)
            SDL_DestroyTexture(imageTexture);

        imageTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, texWidth, texHeight);
        if (!imageTexture) {
            PrintLog(PRINT_NORMAL, "ERROR: video texture %dx%d failed: %s", texWidth, texHeight, SDL_GetError());
            lastTextureFormat = -1;
            return;
        }
        PrintLog(PRINT_NORMAL, "video texture %dx%d created (format %d)", texWidth, texHeight, format);
        SDL_SetTextureScaleMode(imageTexture, SDL_SCALEMODE_LINEAR);

        lastTextureFormat = format;
    }

    if (!imageTexture)
        return;

    int32 texPitch = 0;
    uint16 *pixels = NULL;
    if (!SDL_LockTexture(imageTexture, NULL, (void **)&pixels, &texPitch))
        return;

    int32 pitch16 = texPitch >> 1;

    for (int32 y = 0; y < texHeight; ++y) {
        const int32 srcY  = y << downShift;
        const uint8 *yRow = yPlane + srcY * strideY;
        const uint8 *uRow = uPlane + (srcY >> chromaShiftY) * strideU;
        const uint8 *vRow = vPlane + (srcY >> chromaShiftY) * strideV;
        uint16 *dst       = pixels + y * pitch16;

        for (int32 x = 0; x < texWidth; ++x) {
            const int32 srcX = x << downShift;

            int32 c = ((int32)yRow[srcX] - 16) * 298;
            int32 d = (int32)uRow[srcX >> chromaShiftX] - 128;
            int32 e = (int32)vRow[srcX >> chromaShiftX] - 128;

            uint16 r = clamp[(c + 409 * e + 128) >> 8];
            uint16 g = clamp[(c - 100 * d - 208 * e + 128) >> 8];
            uint16 b = clamp[(c + 516 * d + 128) >> 8];

            dst[x] = (uint16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }

    SDL_UnlockTexture(imageTexture);
}

void RenderDevice::SetupVideoTexture_YUV420(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                            int32 strideV)
{
    ConvertYUVToImageTexture(width, height, yPlane, uPlane, vPlane, strideY, strideU, strideV, 1, 1, SHADER_YUV_420);
}
void RenderDevice::SetupVideoTexture_YUV422(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                            int32 strideV)
{
    ConvertYUVToImageTexture(width, height, yPlane, uPlane, vPlane, strideY, strideU, strideV, 1, 0, SHADER_YUV_422);
}
void RenderDevice::SetupVideoTexture_YUV444(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                            int32 strideV)
{
    ConvertYUVToImageTexture(width, height, yPlane, uPlane, vPlane, strideY, strideU, strideV, 0, 0, SHADER_YUV_444);
}
