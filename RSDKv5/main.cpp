#include "RSDK/Core/RetroEngine.hpp"
#include "main.hpp"

#if RETRO_STANDALONE
#define LinkGameLogic RSDK::LinkGameLogic
#else
#define EngineInfo RSDK::EngineInfo
#include <GameMain.h>
#define LinkGameLogic LinkGameLogicDLL
#endif

#if RETRO_PLATFORM == RETRO_WIN && !RETRO_RENDERDEVICE_SDL2

#if RETRO_RENDERDEVICE_DIRECTX9 || RETRO_RENDERDEVICE_DIRECTX11
INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, INT nShowCmd)
{
    RSDK::RenderDevice::hInstance     = hInstance;
    RSDK::RenderDevice::hPrevInstance = hPrevInstance;
    RSDK::RenderDevice::nShowCmd      = nShowCmd;

    return RSDK_main(1, &lpCmdLine, LinkGameLogic);
}
#else
INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, INT nShowCmd) { return RSDK_main(1, &lpCmdLine, LinkGameLogic); }
#endif

#elif RETRO_PLATFORM == RETRO_ANDROID
extern "C" {
void android_main(struct android_app *app);
}

void android_main(struct android_app *ap)
{
    app                                 = ap;
    app->onAppCmd                       = AndroidCommandCallback;
    app->activity->callbacks->onKeyDown = AndroidKeyDownCallback;
    app->activity->callbacks->onKeyUp   = AndroidKeyUpCallback;

    JNISetup *jni = GetJNISetup();
    // we make sure we do it here so init can chill safely before any callbacks occur
    Paddleboat_init(jni->env, jni->thiz);

    SwappyGL_init(jni->env, jni->thiz);
    SwappyGL_setAutoSwapInterval(false);
    SwappyGL_setSwapIntervalNS(SWAPPY_SWAP_60FPS);
    SwappyGL_setMaxAutoSwapIntervalNS(SWAPPY_SWAP_60FPS);

    getFD    = jni->env->GetMethodID(jni->clazz, "getFD", "([BB)I");
    writeLog = jni->env->GetMethodID(jni->clazz, "writeLog", "([BI)V");

    setLoading  = jni->env->GetMethodID(jni->clazz, "setLoadingIcon", "([B)V");
    showLoading = jni->env->GetMethodID(jni->clazz, "showLoadingIcon", "()V");
    hideLoading = jni->env->GetMethodID(jni->clazz, "hideLoadingIcon", "()V");

    setPixSize = jni->env->GetMethodID(jni->clazz, "setPixSize", "(II)V");

#if RETRO_USE_MOD_LOADER
    fsExists      = jni->env->GetMethodID(jni->clazz, "fsExists", "([B)Z");
    fsIsDir       = jni->env->GetMethodID(jni->clazz, "fsIsDir", "([B)Z");
    fsDirIter     = jni->env->GetMethodID(jni->clazz, "fsDirIter", "([B)[Ljava/lang/String;");
    fsRecurseIter = jni->env->GetMethodID(jni->clazz, "fsRecurseIter", "([B)Ljava/lang/String;");
#endif

    GameActivity_setWindowFlags(app->activity,
                                AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_TURN_SCREEN_ON | AWINDOW_FLAG_LAYOUT_NO_LIMITS | AWINDOW_FLAG_FULLSCREEN
                                    | AWINDOW_FLAG_SHOW_WHEN_LOCKED,
                                0);

    RSDK_main(0, NULL, (void *)LinkGameLogic);

    Paddleboat_destroy(jni->env);
    SwappyGL_destroy();
}
#else
int32 main(int32 argc, char *argv[]) { return RSDK_main(argc, argv, (void *)LinkGameLogic); }
#endif

#if RETRO_PLATFORM == RETRO_XBOX
#ifdef RSDK_USE_SDL3
#include <pbkit/pbkit.h>
#endif

static int SCREEN_WIDTH;
static int SCREEN_HEIGHT;

void SetXboxResolution()
{
    // Prefer 720p when the dashboard has it enabled (HD AV pack + 720p flag);
    // XVideoSetMode fails otherwise, so falling back to 640x480 respects the
    // console's video settings (same approach as LithiumX).
    // Both modes use 16bpp (RGB565): at 32bpp pbkit's 4 full-frame buffers
    // (front + 2 back + depth) waste RAM the storage pools need — ~1.7MB at
    // 480p, ~7.4MB at 720p — and the game renders RGB565 internally anyway.
    SCREEN_WIDTH  = 1280;
    SCREEN_HEIGHT = 720;
    if (!XVideoSetMode(SCREEN_WIDTH, SCREEN_HEIGHT, 16, REFRESH_DEFAULT)) {
        SCREEN_WIDTH  = 640;
        SCREEN_HEIGHT = 480;
        XVideoSetMode(SCREEN_WIDTH, SCREEN_HEIGHT, 16, REFRESH_DEFAULT);
    }

#ifdef RSDK_USE_SDL3
    pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5, false); // must precede pb_init
#endif

#ifdef RSDK_USE_SDL3
    // Initialize pbkit NOW, before the engine allocates its storage pools.
    // pb_init() needs several MB of contiguous physical memory below 64MB for its
    // framebuffers; allocating pools first fragments that region and pb_init fails
    // with -11 inside SDL_CreateRenderer. The SDL3 XGU renderer detects the running
    // pbkit instance (pb_init == -8) and reuses it.
    pb_size(128 * 1024); // we submit tiny command buffers; default 512KB wastes RAM
    int pbStatus = pb_init();
    if (pbStatus < 0 && pbStatus != -8)
        debugPrint("pb_init failed early: %d\n", pbStatus);
#endif
}

extern "C" {
double atof(const char *s)
{
    double a = 0.0;
    int e    = 0;
    int c;
    while ((c = *s++) != '\0' && (c >= '0' && c <= '9')) a = a * 10.0 + (c - '0');
    if (c == '.') {
        while ((c = *s++) != '\0' && (c >= '0' && c <= '9')) {
            a = a * 10.0 + (c - '0');
            e = e - 1;
        }
    }
    if (c == 'e' || c == 'E') {
        int sign = 1;
        int i    = 0;
        c        = *s++;
        if (c == '+')
            c = *s++;
        else if (c == '-') {
            c    = *s++;
            sign = -1;
        }
        while (c >= '0' && c <= '9') {
            i = i * 10 + (c - '0');
            c = *s++;
        }
        e += i * sign;
    }
    while (e > 0) {
        a *= 10.0;
        --e;
    }
    while (e < 0) {
        a *= 0.1;
        ++e;
    }
    return a;
}
}
#endif

int32 RSDK_main(int32 argc, char **argv, void *linkLogicPtr)
{
    RSDK::linkGameLogic = (RSDK::LogicLinkHandle)linkLogicPtr;

#if RETRO_PLATFORM == RETRO_XBOX
    SetXboxResolution();
#endif

    RSDK::InitCoreAPI();

    int32 exitCode = RSDK::RunRetroEngine(argc, argv);

    RSDK::ReleaseCoreAPI();

#if RETRO_PLATFORM == RETRO_XBOX
#endif
    return exitCode;
}