#include "RSDK/Core/RetroEngine.hpp"

using namespace RSDK;

FileInfo VideoManager::file;

ogg_sync_state VideoManager::oy;
ogg_page VideoManager::og;
ogg_packet VideoManager::op;
ogg_stream_state VideoManager::vo;
ogg_stream_state VideoManager::to;
th_info VideoManager::ti;
th_comment VideoManager::tc;
th_dec_ctx *VideoManager::td    = NULL;
th_setup_info *VideoManager::ts = NULL;

th_pixel_fmt VideoManager::pixelFormat;
ogg_int64_t VideoManager::granulePos = 0;
bool32 VideoManager::initializing    = false;

#if RETRO_PLATFORM == RETRO_XBOX
#define PLM_NO_STDIO // match pl_mpeg.c: no fopen/FILE prototypes (nxdk has no usable stdio files)
#include "pl_mpeg/pl_mpeg.h"

// Xbox FMV path: MPEG-1 via pl_mpeg, streamed from a loose D:\Videos\<name>.mpg,
// replacing the Theora path. XMV was ruled out (ffmpeg can't mux it, nxdk has no
// WMV decoder); MPEG-1 is ffmpeg-encodable, its Y/Cr/Cb output feeds the existing
// YUV420 GPU upload untouched, and its small decode footprint fits the 64MB budget
// where the 1024x512 Theora decoder OOMed. Video-only — pl_mpeg audio is disabled
// (the FMV files carry no audio the engine uses; cf. the Theora path below which
// also ignores the audio stream). State is file-static so the shared VideoManager
// header — and every other platform — is untouched.
static plm_t *plmVideo        = NULL;
static plm_frame_t *plmFrame  = NULL;
static bool32 plmInitializing = false;

// pl_mpeg pulls more compressed data through this when its ring buffer runs low;
// feed it from the already-open RSDK FileInfo so the whole .mpg never sits in RAM.
static void VideoLoadCallback(plm_buffer_t *buffer, void *user)
{
    (void)user;
    uint8 chunk[0x1000];
    int32 got = (int32)ReadBytes(&VideoManager::file, chunk, sizeof(chunk));
    if (got > 0)
        plm_buffer_write(buffer, chunk, (size_t)got);
    else
        plm_buffer_signal_end(buffer);
}
#endif

bool32 RSDK::LoadVideo(const char *filename, double startDelay, bool32 (*skipCallback)())
{
    if (ENGINE_VERSION == 5 && sceneInfo.state == ENGINESTATE_VIDEOPLAYBACK)
        return false;
#if RETRO_REV0U
    if (ENGINE_VERSION == 3 && RSDK::Legacy::gameMode == RSDK::Legacy::v3::ENGINE_VIDEOWAIT)
        return false;
#endif

#if RETRO_PLATFORM == RETRO_XBOX
    // Map the requested name (e.g. "Mania.ogv") to the loose MPEG-1 re-encode
    // "D:\Videos\Mania.mpg". Absolute D:\ path — nxdk has no CWD. If the loose
    // file is absent, skip the FMV rather than fail loudly (matches prior behaviour).
    char base[0x40];
    int32 b = 0;
    for (; filename[b] && b < (int32)sizeof(base) - 1; ++b) base[b] = filename[b];
    base[b] = '\0';
    for (int32 i = b - 1; i >= 0; --i) {
        if (base[i] == '.') {
            base[i] = '\0';
            break;
        }
    }

    char fullFilePath[0x80];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "D:\\Videos\\%s.mpg", base);

    InitFileInfo(&VideoManager::file);
    VideoManager::file.externalFile = true; // plain fOpen of D:\Videos\...; never the data pack
    if (!LoadFile(&VideoManager::file, fullFilePath, FMODE_RB))
        return false;

    plm_buffer_t *buffer = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
    if (!buffer) {
        CloseFile(&VideoManager::file);
        return false;
    }
    plm_buffer_set_load_callback(buffer, VideoLoadCallback, NULL);

    plmVideo = plm_create_with_buffer(buffer, TRUE); // destroy the buffer with the decoder
    if (!plmVideo || !plm_has_headers(plmVideo)) {
        if (plmVideo) {
            plm_destroy(plmVideo);
            plmVideo = NULL;
        }
        else {
            plm_buffer_destroy(buffer);
        }
        CloseFile(&VideoManager::file);
        return false;
    }

    plm_set_audio_enabled(plmVideo, FALSE);
    plm_set_video_enabled(plmVideo, TRUE);
    plm_set_loop(plmVideo, FALSE);

    engine.storedShaderID     = videoSettings.shaderID;
    videoSettings.screenCount = 0;

    if (ENGINE_VERSION == 5)
        engine.storedState = sceneInfo.state;
#if RETRO_REV0U
    else if (ENGINE_VERSION == 3)
        engine.storedState = RSDK::Legacy::gameMode;
#endif

    engine.displayTime     = 0.0;
    engine.videoStartDelay = 0.0;
    if (AudioDevice::audioState == 1)
        engine.videoStartDelay = startDelay;

    videoSettings.shaderID = SHADER_YUV_420; // MPEG-1 is always 4:2:0
    plmInitializing        = true;
    plmFrame               = NULL;

    engine.skipCallback = NULL;
    ProcessVideo();
    engine.skipCallback = skipCallback;

    changedVideoSettings = false;
    if (ENGINE_VERSION == 5)
        sceneInfo.state = ENGINESTATE_VIDEOPLAYBACK;
#if RETRO_REV0U
    else if (ENGINE_VERSION == 3)
        RSDK::Legacy::gameMode = RSDK::Legacy::v3::ENGINE_VIDEOWAIT;
#endif

    return true;
#else
    char fullFilePath[0x80];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/Video/%s", filename);

    InitFileInfo(&VideoManager::file);
    if (LoadFile(&VideoManager::file, fullFilePath, FMODE_RB)) {
        // Init
        ogg_sync_init(&VideoManager::oy);

        th_comment_init(&VideoManager::tc);
        th_info_init(&VideoManager::ti);

        int32 theora_p = 0;
        char *buffer   = NULL;

        // Parse header stuff
        bool32 finishedHeader = false;
        while (!finishedHeader) {
            buffer    = ogg_sync_buffer(&VideoManager::oy, 0x1000);
            int32 ret = (int32)ReadBytes(&VideoManager::file, buffer, 0x1000);
            ogg_sync_wrote(&VideoManager::oy, 0x1000);

            if (ret == 0)
                break;

            while (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) {
                ogg_stream_state test;

                /* is this a mandated initial header? If not, stop parsing */
                if (!ogg_page_bos(&VideoManager::og)) {
                    /* don't leak the page; get it into the appropriate stream */
                    ogg_stream_pagein(&VideoManager::to, &VideoManager::og);
                    finishedHeader = true;
                    break;
                }

                ogg_stream_init(&test, ogg_page_serialno(&VideoManager::og));
                ogg_stream_pagein(&test, &VideoManager::og);
                ogg_stream_packetout(&test, &VideoManager::op);

                // identify codec
                if (!theora_p && th_decode_headerin(&VideoManager::ti, &VideoManager::tc, &VideoManager::ts, &VideoManager::op) >= 0) {
                    // theora
                    memcpy(&VideoManager::to, &test, sizeof(test));
                    theora_p = 1;
                }
                else {
                    // we dont care (possibly vorbis)
                    ogg_stream_clear(&test);
                }
            }
        }

        if (theora_p) {
            VideoManager::ts = NULL;
            theora_p         = 1;
            while (theora_p && theora_p < 3) {
                int32 ret;

                while (theora_p && (theora_p < 3) && (ret = ogg_stream_packetout(&VideoManager::to, &VideoManager::op))) {
                    if (ret < 0) {
#if !RETRO_USE_ORIGINAL_CODE
                        PrintLog(PRINT_NORMAL, "ERROR: failed to parse theora stream headers. corrupted stream?");
#endif

                        theora_p = 0;
                        break;
                    }

                    if (!th_decode_headerin(&VideoManager::ti, &VideoManager::tc, &VideoManager::ts, &VideoManager::op)) {
#if !RETRO_USE_ORIGINAL_CODE
                        PrintLog(PRINT_NORMAL, "ERROR: failed to parse theora stream headers. corrupted stream?");
#endif

                        theora_p = 0;
                        break;
                    }

                    theora_p++;
                }

                if (!theora_p)
                    break;

                /* The header pages/packets will arrive before anything else we
                   care about, or the stream is not obeying spec */

                if (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) {
                    ogg_stream_pagein(&VideoManager::to, &VideoManager::og);
                }
                else {
                    buffer    = ogg_sync_buffer(&VideoManager::oy, 0x1000);
                    int32 ret = (int32)ReadBytes(&VideoManager::file, buffer, 0x1000);
                    ogg_sync_wrote(&VideoManager::oy, 0x1000);
                    if (ret == 0) {
#if !RETRO_USE_ORIGINAL_CODE
                        PrintLog(PRINT_NORMAL, "ERROR: Reached end of file while searching for codec headers.");
#endif

                        theora_p = 0;
                    }
                }
            }

            if (!theora_p) {
                th_info_clear(&VideoManager::ti);
                th_comment_clear(&VideoManager::tc);
                th_setup_free(VideoManager::ts);
            }
            else {
                VideoManager::td          = th_decode_alloc(&VideoManager::ti, VideoManager::ts);
                VideoManager::pixelFormat = VideoManager::ti.pixel_fmt;

                int32 ppLevelMax = 0;
                th_decode_ctl(VideoManager::td, TH_DECCTL_GET_PPLEVEL_MAX, &ppLevelMax, sizeof(int32));
                int32 ppLevel = 0;
                th_decode_ctl(VideoManager::td, TH_DECCTL_SET_PPLEVEL, &ppLevel, sizeof(int32));

                th_setup_free(VideoManager::ts);

                engine.storedShaderID     = videoSettings.shaderID;
                videoSettings.screenCount = 0;

                if (ENGINE_VERSION == 5)
                    engine.storedState = sceneInfo.state;
#if RETRO_REV0U
                else if (ENGINE_VERSION == 3)
                    engine.storedState = RSDK::Legacy::gameMode;
#endif
                engine.displayTime         = 0.0;
                VideoManager::initializing = true;
                VideoManager::granulePos   = 0;

                engine.displayTime     = 0.0;
                engine.videoStartDelay = 0.0;
                if (AudioDevice::audioState == 1)
                    engine.videoStartDelay = startDelay;

                switch (VideoManager::pixelFormat) {
                    default: break;
                    case TH_PF_420: videoSettings.shaderID = SHADER_YUV_420; break;
                    case TH_PF_422: videoSettings.shaderID = SHADER_YUV_422; break;
                    case TH_PF_444: videoSettings.shaderID = SHADER_YUV_444; break;
                }

                engine.skipCallback = NULL;
                ProcessVideo();
                engine.skipCallback = skipCallback;

                changedVideoSettings = false;
                if (ENGINE_VERSION == 5)
                    sceneInfo.state = ENGINESTATE_VIDEOPLAYBACK;
#if RETRO_REV0U
                else if (ENGINE_VERSION == 3)
                    RSDK::Legacy::gameMode = RSDK::Legacy::v3::ENGINE_VIDEOWAIT;
#endif

                return true;
            }
        }

        CloseFile(&VideoManager::file);
    }

    return false;
#endif
}

void RSDK::ProcessVideo()
{
#if RETRO_PLATFORM == RETRO_XBOX
    if (!plmVideo)
        return;

    bool32 finished = false;
    double curTime  = plmFrame ? (double)plmFrame->time : 0.0;

    if (!plmInitializing) {
        double streamPos = GetVideoStreamPos();

        if (streamPos <= -1.0)
            engine.displayTime += (1.0 / 60.0); // deltaTime frame-step (FMV audio is disabled)
        else
            engine.displayTime = streamPos;

#if RETRO_USE_MOD_LOADER
        RunModCallbacks(MODCB_ONVIDEOSKIPCB, (void *)engine.skipCallback);
#endif
        if (engine.skipCallback && engine.skipCallback())
            finished = true;
    }

    if (!finished && (plmInitializing || engine.displayTime >= engine.videoStartDelay + curTime)) {
        plm_frame_t *frame = plm_decode_video(plmVideo);
        if (!frame) {
            finished = true; // source ended (or corrupt) — tear down below
        }
        else {
            plmFrame = frame;
            // MPEG-1 is 4:2:0. plm plane widths are the (macroblock-padded) strides;
            // frame->width/height are the display crop the upload honours. cb=U, cr=V.
            RenderDevice::SetupVideoTexture_YUV420((int32)frame->width, (int32)frame->height, frame->y.data, frame->cb.data, frame->cr.data,
                                                   (int32)frame->y.width, (int32)frame->cb.width, (int32)frame->cr.width);
        }

        plmInitializing = false;
    }

    if (finished) {
        CloseFile(&VideoManager::file);

        plm_destroy(plmVideo); // also destroys the buffer (destroy_when_done)
        plmVideo = NULL;
        plmFrame = NULL;

        videoSettings.shaderID    = engine.storedShaderID;
        videoSettings.screenCount = 1;
        if (ENGINE_VERSION == 5)
            sceneInfo.state = engine.storedState;
#if RETRO_REV0U
        else if (ENGINE_VERSION == 3)
            RSDK::Legacy::gameMode = engine.storedState;
#endif
    }
#else
    bool32 finished = false;
    double curTime  = 0;
    if (!VideoManager::initializing) {
        double streamPos = GetVideoStreamPos();

        if (streamPos <= -1.0)
            engine.displayTime += (1.0 / 60.0); // deltaTime frame-step
        else
            engine.displayTime = streamPos;

        curTime = th_granule_time(VideoManager::td, VideoManager::granulePos);

#if RETRO_USE_MOD_LOADER
        RunModCallbacks(MODCB_ONVIDEOSKIPCB, (void *)engine.skipCallback);
#endif
        if (engine.skipCallback && engine.skipCallback()) {
            finished = true;
        }
    }

    if (!finished && (VideoManager::initializing || engine.displayTime >= engine.videoStartDelay + curTime)) {
        while (ogg_stream_packetout(&VideoManager::to, &VideoManager::op) <= 0) {
            char *buffer = ogg_sync_buffer(&VideoManager::oy, 0x1000);
            // if we're playing and reached the end of file
            if (!ReadBytes(&VideoManager::file, buffer, 0x1000) && !VideoManager::initializing) {
                finished = true;
                break;
            }

            ogg_sync_wrote(&VideoManager::oy, 0x1000);

            while (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) ogg_stream_pagein(&VideoManager::to, &VideoManager::og);
        }

        if (!finished && !th_decode_packetin(VideoManager::td, &VideoManager::op, &VideoManager::granulePos)) {
            th_ycbcr_buffer yuv;
            th_decode_ycbcr_out(VideoManager::td, yuv);

            int32 dataPos = (VideoManager::ti.pic_x & 0xFFFFFFFE) + (VideoManager::ti.pic_y & 0xFFFFFFFE) * yuv[0].stride;

            int32 vidWidth  = yuv[0].width;
            int32 vidHeight = yuv[0].height;

            switch (VideoManager::pixelFormat) {
                default: break;

                case TH_PF_444:
                    RenderDevice::SetupVideoTexture_YUV444(vidWidth, vidHeight, &yuv[0].data[dataPos], &yuv[1].data[dataPos], &yuv[2].data[dataPos],
                                                           yuv[0].stride, yuv[1].stride, yuv[2].stride);
                    break;

                case TH_PF_422:
                    RenderDevice::SetupVideoTexture_YUV422(vidWidth, vidHeight, &yuv[0].data[dataPos],
                                                           &yuv[1].data[yuv[1].stride * VideoManager::ti.pic_y + (VideoManager::ti.pic_x >> 1)],
                                                           &yuv[2].data[yuv[1].stride * VideoManager::ti.pic_y + (VideoManager::ti.pic_x >> 1)],
                                                           yuv[0].stride, yuv[1].stride, yuv[2].stride);
                    break;

                case TH_PF_420:
                    RenderDevice::SetupVideoTexture_YUV420(
                        vidWidth, vidHeight, &yuv[0].data[dataPos],
                        &yuv[1].data[yuv[1].stride * (VideoManager::ti.pic_y >> 1) + (VideoManager::ti.pic_x >> 1)],
                        &yuv[2].data[yuv[1].stride * (VideoManager::ti.pic_y >> 1) + (VideoManager::ti.pic_x >> 1)], yuv[0].stride, yuv[1].stride,
                        yuv[2].stride);
                    break;
            }
        }

        VideoManager::initializing = false;
    }

    if (finished) {
        CloseFile(&VideoManager::file);

        // Flush everything out
        while (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) ogg_stream_pagein(&VideoManager::to, &VideoManager::og);

        ogg_stream_clear(&VideoManager::to);
        th_decode_free(VideoManager::td);
        th_comment_clear(&VideoManager::tc);
        th_info_clear(&VideoManager::ti);
        ogg_sync_clear(&VideoManager::oy);

        videoSettings.shaderID    = engine.storedShaderID;
        videoSettings.screenCount = 1;
        if (ENGINE_VERSION == 5)
            sceneInfo.state = engine.storedState;
#if RETRO_REV0U
        else if (ENGINE_VERSION == 3)
            RSDK::Legacy::gameMode = engine.storedState;
#endif
    }
#endif
}
