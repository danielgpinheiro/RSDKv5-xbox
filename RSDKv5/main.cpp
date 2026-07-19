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
INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, INT nShowCmd)
{
    return RSDK_main(1, &lpCmdLine, LinkGameLogic);
}
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

    setLoading = jni->env->GetMethodID(jni->clazz, "setLoadingIcon", "([B)V");
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
static int SCREEN_WIDTH;
static int SCREEN_HEIGHT;

void SetXboxResolution()
{
    SCREEN_WIDTH  = 640;
    SCREEN_HEIGHT = 480;
    XVideoSetMode(SCREEN_WIDTH, SCREEN_HEIGHT, 32, REFRESH_DEFAULT);
#if RETRO_PLATFORM == RETRO_XBOX
    debugPrint("[RSDK] SetXboxResolution: %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);
#endif
}

extern "C" {
double atof(const char *s)
{
    double a = 0.0;
    int e    = 0;
    int c;
    while ((c = *s++) != '\0' && (c >= '0' && c <= '9'))
        a = a * 10.0 + (c - '0');
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
#if RETRO_PLATFORM == RETRO_XBOX
    debugPrint("[RSDK] RSDK_main: entry\n");
#endif
    RSDK::linkGameLogic = (RSDK::LogicLinkHandle)linkLogicPtr;

#if RETRO_PLATFORM == RETRO_XBOX
    SetXboxResolution();
    debugPrint("[RSDK] RSDK_main: calling InitCoreAPI\n");
#endif

    RSDK::InitCoreAPI();

#if RETRO_PLATFORM == RETRO_XBOX
    debugPrint("[RSDK] RSDK_main: calling RunRetroEngine\n");
#endif
    int32 exitCode = RSDK::RunRetroEngine(argc, argv);

    RSDK::ReleaseCoreAPI();

#if RETRO_PLATFORM == RETRO_XBOX
    debugPrint("[RSDK] RSDK_main: exit code=%d\n", exitCode);
#endif
    return exitCode;
}