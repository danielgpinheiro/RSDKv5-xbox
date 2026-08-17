
// ============================================================================
// PBKit render device (Tier 1 hardware renderer) — implementation.
//
// Drives the NV2A directly (pbkit + xgu) and owns the frame; SDL3 is used only
// for the window handle + event pump (input). See PBKitRenderDevice.hpp.
//
// Stage 1: the engine still software-rasterizes into screens[].frameBuffer
// (RGB565); we upload that to a linear RGB565 NV2A texture and present it as one
// fullscreen quad via pbkit. The GPU-init / combiner / present sequences here are
// ported faithfully from the proven nxdk-sdl3 "nxdk_xgu" renderer
// (dependencies/xbox/nxdk-sdl3/nxdk_glue/render/SDL_render_xgu.c).
// ============================================================================

#include <pbkit/pbkit.h>
#include <xboxkrnl/xboxkrnl.h>
#include "xgu/xgu.h"
#include "xgu/xgux.h"
// swizzle.h has no C++ linkage guard, but swizzle.c is compiled as C — declare its
// prototypes with C linkage so the names match at link time.
extern "C" {
#include "swizzle.h"
}

// -----------------------------------------------------------------------------
// Static members
// -----------------------------------------------------------------------------
SDL_Window *RenderDevice::window = nullptr;
// NOTE: RenderDevice::displayInfo is defined in the shared Drawing.cpp, not here.

bool RenderDevice::gpu3DEnabled = true; // Scene3D face offload (Stage 3)

uint32 RenderDevice::displayModeIndex = 0;
int32 RenderDevice::displayModeCount  = 0;

unsigned long long RenderDevice::targetFreq = 0;
unsigned long long RenderDevice::curTicks   = 0;
unsigned long long RenderDevice::prevTicks  = 0;

namespace {

// pbkit push-buffer pointer — used by the combiner helpers copied verbatim below.
uint32_t *p = nullptr;

// Forward declarations (defined further down; used by FlushSpriteBatches above them).
static inline void texture_combiner_apply(void);
static inline void unlit_combiner_apply(void);

// The present texture: a POT, linear RGB565 container that the software framebuffer
// is uploaded into each frame, then drawn as one fullscreen quad. 512x256 fits the
// pinned 320x240 4:3 internal resolution.
#define PB_FB_TEX_W (512)
#define PB_FB_TEX_H (256)

struct PBPresentTex {
    uint8 *data = nullptr; // CPU-visible (write-combined) contiguous alloc
    uint8 *phys = nullptr; // physical address the GPU reads
    int32 pitch = 0;       // bytes per row
};
PBPresentTex fbTex;

// Fullscreen present quad, 6 verts (2 triangles). Kept in contiguous memory so the
// GPU can DMA it (xgux_set_attrib_pointer masks to the AGP offset).
struct PBVertex {
    float pos[2];
    uint8 color[4];
    float tex[2];
};
PBVertex *presentVerts = nullptr; // 6 verts

// Render-target DMA context (channel 3, spans all RAM). The GPU DMAs vertex/texture
// data through this; without it, rendering reads garbage VRAM -> full-screen noise.
struct s_CtxDma renderTargetDmaCtx;

// ===========================================================================
// Stage 2: paletted sprite path — 8bpp I8 textures + hardware CLUT.
//
// Each GFXSurface's already-8bpp pixels are uploaded once as a swizzled I8 texture;
// the game's 8 palette banks (fullPalette, RGB565) are synced into a single 8x256
// ARGB CLUT each frame (index 0 -> alpha 0 = transparent), and a batch selects its
// bank via the NV2A palette OFFSET (bank*256*4, passed >>6). Sprite quads accumulate
// during the frame (batched by texture+bank+blend) and are drawn over the software
// framebuffer at present. Palette cycling is a free CLUT rewrite — no texture re-bake.
// ===========================================================================
// Round up to a power of two (swizzled textures need a POT container).
inline uint32 npot2pot(uint32 num)
{
    if (num < 2)
        return 1;
    uint32 msb;
    __asm__("bsr %1, %0" : "=r"(msb) : "r"(num));
    if ((1u << msb) == num)
        return num;
    return 1u << (msb + 1);
}

inline uint32 RGB565toARGB(uint16 c)
{
    uint32 r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
    uint32 r8 = (r5 << 3) | (r5 >> 2), g8 = (g6 << 2) | (g6 >> 4), b8 = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
}

// One 8x256 ARGB CLUT (contiguous, 64B-aligned): bank b at byte offset b*256*4.
uint32 *pbClut     = nullptr;
uint8 *pbClutPhys  = nullptr;

void PBSyncCLUT()
{
    if (!pbClut)
        return;
    // Sync all 8 banks from fullPalette. Index 0 -> alpha 0 (transparent), matching the
    // software blit's `if (*pixels > 0)`. Cheap enough to do every frame (2048 entries);
    // dirty-tracking is a later optimization.
    for (int32 bank = 0; bank < PALETTE_BANK_COUNT; ++bank) {
        uint16 *src = fullPalette[bank];
        uint32 *dst = &pbClut[bank * 256];
        dst[0]      = 0; // transparent
        for (int32 i = 1; i < 256; ++i) dst[i] = RGB565toARGB(src[i]);
    }
}

// --- per-surface I8 texture cache (indexed by sheetID) -----------------------
struct PBSurfTex {
    uint8 *builtFrom = nullptr; // surface->pixels this was built from (invalidate on change)
    uint8 *data      = nullptr;
    uint8 *phys      = nullptr;
    int32 texW = 0, texH = 0; // POT swizzled container
    int32 w = 0, h = 0;       // logical
};
PBSurfTex pbSurfTex[SURFACE_COUNT];

PBSurfTex *GetSurfaceTexture(int32 sheetID)
{
    if (sheetID < 0 || sheetID >= SURFACE_COUNT)
        return nullptr;
    GFXSurface *surface = &gfxSurface[sheetID];
    if (!surface->pixels || surface->width <= 0 || surface->height <= 0)
        return nullptr;

    PBSurfTex *t = &pbSurfTex[sheetID];
    if (t->data && t->builtFrom == surface->pixels && t->w == surface->width && t->h == surface->height)
        return t; // still valid

    if (t->data) { // surface was reloaded/replaced — drop the stale texture
        MmFreeContiguousMemory(t->data);
        t->data = nullptr;
    }

    int32 texW = (int32)npot2pot((uint32)surface->width);
    int32 texH = (int32)npot2pot((uint32)surface->height);
    uint8 *data = (uint8 *)MmAllocateContiguousMemoryEx((SIZE_T)texW * texH, 0, 0xFFFFFFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
    if (!data)
        return nullptr;
    // Swizzle the 8bpp indices (bpp=1). Source pitch = surface->width; the swizzler reads
    // width x height from the source into the POT container.
    memset(data, 0, (size_t)texW * texH);
    swizzle_rect(surface->pixels, surface->width, surface->height, data, surface->width, 1);

    t->builtFrom = surface->pixels;
    t->data      = data;
    t->phys      = (uint8 *)MmGetPhysicalAddress(data);
    t->texW      = texW;
    t->texH      = texH;
    t->w         = surface->width;
    t->h         = surface->height;
    return t;
}

// --- sprite quad batches -----------------------------------------------------
#define MAX_SPR_VERTS   (12288) // 2048 quads
#define MAX_SPR_BATCHES (1024)
PBVertex *sprVerts = nullptr;
int32 sprVertCount = 0;
struct SprBatch {
    int32 start, count;
    uint8 *texPhys;
    int32 texW, texH;
    int32 bank;
    XguBlendFactor sfactor, dfactor;
};
SprBatch sprBatches[MAX_SPR_BATCHES];
int32 sprBatchCount = 0;

// Only offload sprites during live gameplay. GPU sprites composite over the software
// framebuffer (drawn last, on top), which breaks the interleaved draw order of overlay
// screens — the pause menu, results, dev menu — where panels are software (fb) and text
// is GPU. Keeping those on the CPU path preserves their order; gameplay (where the perf
// matters) stays on the GPU. Mirrors the SDL3 special-stage offload's ENGINESTATE gate.
inline bool PBSpriteOffloadOK()
{
    return sprVerts && pbClut && videoSettings.screenCount == 1 && sceneInfo.state == ENGINESTATE_REGULAR;
}

// Map an ink effect to a GPU blend + vertex alpha. false = unsupported (software path).
inline bool SprInkToBlend(int32 inkEffect, int32 alpha, XguBlendFactor *sf, XguBlendFactor *df, float *a)
{
    switch (inkEffect) {
        case INK_NONE: *sf = XGU_FACTOR_SRC_ALPHA; *df = XGU_FACTOR_ONE_MINUS_SRC_ALPHA; *a = 1.0f; return true;
        case INK_BLEND: *sf = XGU_FACTOR_SRC_ALPHA; *df = XGU_FACTOR_ONE_MINUS_SRC_ALPHA; *a = 0.5f; return true;
        case INK_ALPHA: *sf = XGU_FACTOR_SRC_ALPHA; *df = XGU_FACTOR_ONE_MINUS_SRC_ALPHA; *a = (alpha > 0xFF ? 0xFF : (alpha < 0 ? 0 : alpha)) / 255.0f; return true;
        case INK_ADD: *sf = XGU_FACTOR_SRC_ALPHA; *df = XGU_FACTOR_ONE; *a = (alpha > 0xFF ? 0xFF : (alpha < 0 ? 0 : alpha)) / 255.0f; return true;
        default: return false; // SUB/TINT/MASKED -> software for now
    }
}

// Append one textured quad from 4 arbitrary corners (px/py order TL,TR,BL,BR in
// back-buffer pixels; UVs normalized to the POT container u0,v0..u1,v1), coalescing
// with the previous batch when texture+bank+blend match. Handles rotated/scaled quads.
bool EmitSpriteQuadCorners(PBSurfTex *t, int32 bank, XguBlendFactor sf, XguBlendFactor df, float a, const float px[4], const float py[4], float u0,
                           float v0, float u1, float v1)
{
    if (!sprVerts || sprVertCount + 6 > MAX_SPR_VERTS)
        return false;
    bool coalesce = sprBatchCount > 0 && sprBatches[sprBatchCount - 1].texPhys == t->phys && sprBatches[sprBatchCount - 1].bank == bank
                    && sprBatches[sprBatchCount - 1].sfactor == sf && sprBatches[sprBatchCount - 1].dfactor == df
                    && sprBatches[sprBatchCount - 1].start + sprBatches[sprBatchCount - 1].count == sprVertCount;
    if (!coalesce && sprBatchCount >= MAX_SPR_BATCHES)
        return false;

    uint8 alpha8 = (uint8)(a * 255.0f);
    const float tu[4] = { u0, u1, u0, u1 };
    const float tv[4] = { v0, v0, v1, v1 };
    const int32 order[6] = { 0, 1, 2, 1, 3, 2 };
    int32 start = sprVertCount;
    for (int32 i = 0; i < 6; ++i) {
        int32 c              = order[i];
        PBVertex *v          = &sprVerts[sprVertCount++];
        v->pos[0]            = px[c];
        v->pos[1]            = py[c];
        v->color[0]          = 0xFF;
        v->color[1]          = 0xFF;
        v->color[2]          = 0xFF;
        v->color[3]          = alpha8;
        v->tex[0]            = tu[c];
        v->tex[1]            = tv[c];
    }
    if (coalesce)
        sprBatches[sprBatchCount - 1].count += 6;
    else
        sprBatches[sprBatchCount++] = { start, 6, t->phys, t->texW, t->texH, bank, sf, df };
    return true;
}

// Axis-aligned convenience wrapper (unscaled sprites).
bool EmitSpriteQuad(PBSurfTex *t, int32 bank, XguBlendFactor sf, XguBlendFactor df, float a, float x0, float y0, float x1, float y1, float u0,
                    float v0, float u1, float v1)
{
    const float px[4] = { x0, x1, x0, x1 };
    const float py[4] = { y0, y0, y1, y1 };
    return EmitSpriteQuadCorners(t, bank, sf, df, a, px, py, u0, v0, u1, v1);
}

// Append an UNTEXTURED colored polygon (fan-triangulated), coalescing with the previous
// untextured batch of the same blend. Vertices are in 16.16 fixed-point logical pixels
// (Scene3D face coords); per-vertex RGB from colors[], alpha shared. Same ordered list as
// sprites, so draw order between faces and sprites is preserved.
bool EmitColoredPoly(RSDK::Vector2 *vertices, uint32 *colors, int32 vertCount, uint8 alpha8, XguBlendFactor sf, XguBlendFactor df)
{
    if (vertCount < 3 || !sprVerts)
        return false;
    int32 needed = (vertCount - 2) * 3;
    if (sprVertCount + needed > MAX_SPR_VERTS)
        return false;
    bool coalesce = sprBatchCount > 0 && sprBatches[sprBatchCount - 1].texPhys == nullptr && sprBatches[sprBatchCount - 1].sfactor == sf
                    && sprBatches[sprBatchCount - 1].dfactor == df
                    && sprBatches[sprBatchCount - 1].start + sprBatches[sprBatchCount - 1].count == sprVertCount;
    if (!coalesce && sprBatchCount >= MAX_SPR_BATCHES)
        return false;

    float sx = (float)pb_back_buffer_width() / (float)videoSettings.pixWidth;
    float sy = (float)pb_back_buffer_height() / (float)SCREEN_YSIZE;
    int32 start = sprVertCount;
    for (int32 tri = 1; tri + 1 < vertCount; ++tri) {
        int32 idx[3] = { 0, tri, tri + 1 };
        for (int32 k = 0; k < 3; ++k) {
            int32 vi    = idx[k];
            PBVertex *v = &sprVerts[sprVertCount++];
            v->pos[0]   = (vertices[vi].x / 65536.0f) * sx;
            v->pos[1]   = (vertices[vi].y / 65536.0f) * sy;
            uint32 c    = colors[vi];
            v->color[0] = (uint8)((c >> 16) & 0xFF);
            v->color[1] = (uint8)((c >> 8) & 0xFF);
            v->color[2] = (uint8)(c & 0xFF);
            v->color[3] = alpha8;
            v->tex[0]   = 0.0f;
            v->tex[1]   = 0.0f;
        }
    }
    int32 count = sprVertCount - start;
    if (coalesce)
        sprBatches[sprBatchCount - 1].count += count;
    else
        sprBatches[sprBatchCount++] = { start, count, nullptr, 0, 0, 0, sf, df };
    return true;
}

// Draw the accumulated GPU 2D batches over the presented framebuffer, in draw order
// (called in FlipScreen). A batch with texPhys != NULL is an I8 paletted sprite; texPhys
// == NULL is an untextured colored poly (Scene3D faces / 2D primitives).
void FlushSpriteBatches()
{
    if (!sprBatchCount)
        return;
    int32 lastTextured = -1; // -1 = unknown, forces the first combiner set
    for (int32 i = 0; i < sprBatchCount; ++i) {
        SprBatch *b       = &sprBatches[i];
        int32 isTextured  = b->texPhys != nullptr;

        p = pb_begin();
        p = xgu_set_blend_func_sfactor(p, b->sfactor);
        p = xgu_set_blend_func_dfactor(p, b->dfactor);
        if (isTextured != lastTextured) {
            if (isTextured)
                texture_combiner_apply();
            else
                unlit_combiner_apply();
            lastTextured = isTextured;
        }
        if (isTextured) {
            p = xgu_set_texture_offset(p, 0, b->texPhys);
            p = xgu_set_texture_format(p, 0, 2, false, XGU_SOURCE_COLOR, 2, XGU_TEXTURE_FORMAT_I8_A8R8G8B8_SWIZZLED, 1, __builtin_ctz(b->texW),
                                       __builtin_ctz(b->texH), 0);
            p = xgu_set_texture_control0(p, 0, true, 0, 0);
            p = xgu_set_texture_control1(p, 0, b->texW);
            p = xgu_set_texture_image_rect(p, 0, b->texW, b->texH);
            // Select the palette bank: byte offset bank*256*4 into the CLUT, passed >>6.
            p = xgu_set_texture_palette(p, 0, true, XGU_PALETTE_LENGTH_256, (void *)(((uint32_t)pbClutPhys + (uint32_t)b->bank * 256u * 4u) >> 6));
            p = xgu_set_texture_filter(p, 0, 0, XGU_TEXTURE_CONVOLUTION_GAUSSIAN, XGU_TEXTURE_FILTER_NEAREST, XGU_TEXTURE_FILTER_NEAREST, false, false,
                                       false, false);
            p = xgu_set_texture_address(p, 0, XGU_CLAMP_TO_EDGE, false, XGU_CLAMP_TO_EDGE, false, XGU_CLAMP_TO_EDGE, false, false);
        }
        pb_end(p);

        xgux_set_attrib_pointer(XGU_VERTEX_ARRAY, XGU_FLOAT, 2, sizeof(PBVertex), sprVerts[b->start].pos);
        xgux_set_attrib_pointer(XGU_COLOR_ARRAY, XGU_UNSIGNED_BYTE_OGL, 4, sizeof(PBVertex), sprVerts[b->start].color);
        if (isTextured)
            xgux_set_attrib_pointer(XGU_TEXCOORD0_ARRAY, XGU_FLOAT, 2, sizeof(PBVertex), sprVerts[b->start].tex);
        xgux_draw_arrays(XGU_TRIANGLES, 0, b->count);
    }
    // Restore the texture combiner for next frame's framebuffer present quad.
    if (lastTextured == 0) {
        p = pb_begin();
        texture_combiner_apply();
        pb_end(p);
    }
}


// --- GPU-init helpers, copied verbatim from SDL_render_xgu.c ------------------
// clang-format off
// (npot2pot is defined earlier, above the Stage 2 texture cache that first uses it.)
static void set_surface_color_format(const int bpp)
{
    if (bpp == 16)
        pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5, false);
    else if (bpp == 15)
        pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5, false);
    else
        pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, false);
}

static inline void combiner_init(void)
{
    pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT,
        XGU_MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE1, 0)
        | XGU_MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE2, 0)
        | XGU_MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE3, 0));
    p += 2;
    pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM,
        XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_PROGRAM_NONE)
        | XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE1, NV097_SET_SHADER_STAGE_PROGRAM_STAGE1_PROGRAM_NONE)
        | XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE2, NV097_SET_SHADER_STAGE_PROGRAM_STAGE2_PROGRAM_NONE)
        | XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE3, NV097_SET_SHADER_STAGE_PROGRAM_STAGE3_PROGRAM_NONE));
    p += 2;

    pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, 0x1)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, 0x0));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST, 0x4)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_MUX_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DOT_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DOT_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_OP, NV097_SET_COMBINER_COLOR_OCW_OP_NOSHIFT));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, 0x1)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, 0x0));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_AB_DST, 0x4)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_CD_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_SUM_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_MUX_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_OP, NV097_SET_COMBINER_ALPHA_OCW_OP_NOSHIFT));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_CONTROL,
        XGU_MASK(NV097_SET_COMBINER_CONTROL_FACTOR0, NV097_SET_COMBINER_CONTROL_FACTOR0_SAME_FACTOR_ALL)
        | XGU_MASK(NV097_SET_COMBINER_CONTROL_FACTOR1, NV097_SET_COMBINER_CONTROL_FACTOR1_SAME_FACTOR_ALL)
        | XGU_MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, 1));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0,
        XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_INVERSE, 0));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1,
        XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_SPECULAR_CLAMP, 0));
    p += 2;
}

// Combiner stage 0 = texture * diffuse (vertex color). Used for the present quad.
static inline void texture_combiner_apply(void)
{
    p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
    p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE));

    p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, 0x8) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, 0x0));

    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, 0x8) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, 0x0));
}

// Untextured: output = diffuse (vertex color). Used for colored polys (Scene3D faces,
// 2D primitives).
static inline void unlit_combiner_apply(void)
{
    p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
    p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, 0);

    p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, 0x1)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, 0x0));

    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, 0x1)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, 0x0));
}
// clang-format on

#ifdef PBKIT_I8_TEST
// ===========================================================================
// Stage 0 hardware checkpoint: draw an 8bpp I8 texture through the NV2A CLUT and
// animate the palette, proving (a) the I8 paletted format samples, (b) the CLUT
// binds, and (c) palette cycling is a cheap CLUT rewrite — the whole basis of the
// Tier 1 renderer. Enable with I8TEST=y (implies -DPBKIT_I8_TEST). If it renders a
// moving rainbow square, the paletted path works on this NV2A; a black/garbage or
// static square means one of the two hardware-uncertain knobs below is wrong.
//
//  * OFFSET: XGU_MASK left-shifts by the field position (ffs(0xFFFFFFC0)-1 = 6), so
//    the raw physical address would be double-shifted — we pass (phys >> 6). This
//    resolves the xgu.h `//FIXME` on xgu_set_texture_palette's offset.
//  * CONTEXT_DMA: which DMA object the palette offset is relative to — unverified;
//    flip PBKIT_I8_PAL_CTXDMA if the colors are wrong.
// ===========================================================================
#ifndef PBKIT_I8_PAL_CTXDMA
#define PBKIT_I8_PAL_CTXDMA (true)
#endif
#define I8T_DIM (128)

uint8 *i8texData  = nullptr, *i8texPhys = nullptr; // swizzled 8bpp indices
uint32 *i8clut    = nullptr;
uint8 *i8clutPhys = nullptr; // 256-entry ARGB CLUT (contiguous, 64B-aligned)
PBVertex *i8verts = nullptr; // 6
int32 i8phase     = 0;

inline uint8 i8tri(int32 x) // 0..255 triangle wave
{
    x &= 0xFF;
    return (uint8)(x < 128 ? x * 2 : (255 - x) * 2);
}

void I8Test_Init()
{
    if (i8texData)
        return;
    // Source indices: a horizontal ramp 0..255 across the width, so the full CLUT is
    // exercised left-to-right. Swizzled into the GPU texture (bpp=1).
    uint8 *src = (uint8 *)malloc(I8T_DIM * I8T_DIM);
    if (!src)
        return;
    for (int32 y = 0; y < I8T_DIM; ++y)
        for (int32 x = 0; x < I8T_DIM; ++x) src[y * I8T_DIM + x] = (uint8)((x * 256) / I8T_DIM);

    i8texData = (uint8 *)MmAllocateContiguousMemoryEx(I8T_DIM * I8T_DIM, 0, 0xFFFFFFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
    if (i8texData) {
        swizzle_rect(src, I8T_DIM, I8T_DIM, i8texData, I8T_DIM /* src pitch */, 1 /* bpp */);
        i8texPhys = (uint8 *)MmGetPhysicalAddress(i8texData);
    }
    free(src);

    i8clut     = (uint32 *)MmAllocateContiguousMemoryEx(256 * sizeof(uint32), 0, 0xFFFFFFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
    i8clutPhys = i8clut ? (uint8 *)MmGetPhysicalAddress(i8clut) : nullptr;
    i8verts    = (PBVertex *)MmAllocateContiguousMemoryEx(6 * sizeof(PBVertex), 0, 0xFFFFFFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
}

void I8Test_Draw()
{
    I8Test_Init();
    if (!i8texData || !i8clut || !i8verts)
        return;

    // Rewrite the CLUT each frame (rotating rainbow) — all entries opaque so the whole
    // ramp is visible. This is exactly the per-frame CLUT update the real renderer uses
    // for palette cycling; if the square animates, cycling is free (no texture re-bake).
    for (int32 i = 0; i < 256; ++i) {
        uint8 r  = i8tri(i + i8phase);
        uint8 g  = i8tri(i + i8phase + 85);
        uint8 b  = i8tri(i + i8phase + 170);
        i8clut[i] = 0xFF000000u | ((uint32)r << 16) | ((uint32)g << 8) | b;
    }
    i8phase = (i8phase + 2) & 0xFF;

    const float x0 = 256.0f, y0 = 176.0f, x1 = 384.0f, y1 = 304.0f; // centered on 640x480
    const float px[4] = { x0, x1, x0, x1 };
    const float py[4] = { y0, y0, y1, y1 };
    const float tu[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    const float tv[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    const int32 order[6] = { 0, 1, 2, 1, 3, 2 };
    for (int32 i = 0; i < 6; ++i) {
        int32 c            = order[i];
        i8verts[i].pos[0]  = px[c];
        i8verts[i].pos[1]  = py[c];
        i8verts[i].color[0] = 0xFF;
        i8verts[i].color[1] = 0xFF;
        i8verts[i].color[2] = 0xFF;
        i8verts[i].color[3] = 0xFF;
        i8verts[i].tex[0]  = tu[c];
        i8verts[i].tex[1]  = tv[c];
    }

    p = pb_begin();
    texture_combiner_apply();
    p = xgu_set_texture_offset(p, 0, i8texPhys);
    p = xgu_set_texture_format(p, 0, 2, false, XGU_SOURCE_COLOR, 2, XGU_TEXTURE_FORMAT_I8_A8R8G8B8_SWIZZLED, 1, __builtin_ctz(I8T_DIM),
                               __builtin_ctz(I8T_DIM), 0);
    p = xgu_set_texture_control0(p, 0, true, 0, 0);
    p = xgu_set_texture_control1(p, 0, I8T_DIM /* bpp=1 */);
    p = xgu_set_texture_image_rect(p, 0, I8T_DIM, I8T_DIM);
    // OFFSET passed pre-shifted (>>6) to counter XGU_MASK's left-shift; see header note.
    p = xgu_set_texture_palette(p, 0, PBKIT_I8_PAL_CTXDMA, XGU_PALETTE_LENGTH_256, (void *)((uint32_t)i8clutPhys >> 6));
    p = xgu_set_texture_filter(p, 0, 0, XGU_TEXTURE_CONVOLUTION_GAUSSIAN, XGU_TEXTURE_FILTER_NEAREST, XGU_TEXTURE_FILTER_NEAREST, false, false, false,
                               false);
    p = xgu_set_texture_address(p, 0, XGU_CLAMP_TO_EDGE, false, XGU_CLAMP_TO_EDGE, false, XGU_CLAMP_TO_EDGE, false, false);
    pb_end(p);

    xgux_set_attrib_pointer(XGU_VERTEX_ARRAY, XGU_FLOAT, 2, sizeof(PBVertex), i8verts->pos);
    xgux_set_attrib_pointer(XGU_COLOR_ARRAY, XGU_UNSIGNED_BYTE_OGL, 4, sizeof(PBVertex), i8verts->color);
    xgux_set_attrib_pointer(XGU_TEXCOORD0_ARRAY, XGU_FLOAT, 2, sizeof(PBVertex), i8verts->tex);
    xgux_draw_arrays(XGU_TRIANGLES, 0, 6);
}
#endif // PBKIT_I8_TEST

} // namespace

// -----------------------------------------------------------------------------
// Init / teardown
// -----------------------------------------------------------------------------
static void SDLCALL PBSDLLogOutput(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    PrintLog(PRINT_NORMAL, "SDL: %s", message);
}

bool RenderDevice::Init()
{
    SDL_SetLogOutputFunction(PBSDLLogOutput, NULL);

    // Events + video are needed for the window/event pump the input device rides on.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        PrintLog(PRINT_NORMAL, "ERROR: SDL_InitSubSystem failed: %s", SDL_GetError());
        return false;
    }

    videoSettings.windowed = false;

    // Keep an SDL window (RetroEngine waits on RenderDevice::window and the input
    // device rides the SDL event pump), but create NO SDL_Renderer — we present via
    // pbkit ourselves. The nxdk video mode was already set in main.cpp before pb_init.
    VIDEO_MODE vm = XVideoGetMode();
    window        = SDL_CreateWindow(gameVerInfo.gameTitle, vm.width, vm.height, SDL_WINDOW_FULLSCREEN);
    if (!window) {
        PrintLog(PRINT_NORMAL, "ERROR: failed to create window: %s", SDL_GetError());
        return false;
    }

    SDL_GetWindowSize(window, &videoSettings.windowWidth, &videoSettings.windowHeight);
    PrintLog(PRINT_NORMAL, "pbkit renderer: w %d h %d", videoSettings.windowWidth, videoSettings.windowHeight);

    if (!SetupRendering() || !AudioDevice::Init())
        return false;

    InitInputDevices();
    return true;
}

bool RenderDevice::SetupRendering()
{
    // pbkit was already initialized in main.cpp (before pool allocation fragmented the
    // low-64MB contiguous region). Re-assert the surface format + full NV2A pipeline
    // state, matching the proven nxdk_xgu setup.
    const VIDEO_MODE vm = XVideoGetMode();
    set_surface_color_format(vm.bpp);

    XVideoSetVideoEnable(true);
    pb_show_front_screen();
    pb_target_back_buffer();

    const float mIdentity[4 * 4] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    p = pb_begin();
    combiner_init();
    texture_combiner_apply();

    p = xgu_set_blend_enable(p, true);
    p = xgu_set_depth_test_enable(p, false);
    p = xgu_set_blend_func_sfactor(p, XGU_FACTOR_SRC_ALPHA);
    p = xgu_set_blend_func_dfactor(p, XGU_FACTOR_ONE_MINUS_SRC_ALPHA);
    p = xgu_set_depth_func(p, XGU_FUNC_LESS_OR_EQUAL);

    p = xgu_set_skin_mode(p, XGU_SKIN_MODE_OFF);
    p = xgu_set_normalization_enable(p, false);
    p = xgu_set_lighting_enable(p, false);
    p = xgu_set_cull_face_enable(p, false);
    pb_end(p);

    for (int32 i = 0; i < XGU_TEXTURE_COUNT; ++i) {
        p = pb_begin();
        p = xgu_set_texgen_s(p, i, XGU_TEXGEN_DISABLE);
        p = xgu_set_texgen_t(p, i, XGU_TEXGEN_DISABLE);
        p = xgu_set_texgen_r(p, i, XGU_TEXGEN_DISABLE);
        p = xgu_set_texgen_q(p, i, XGU_TEXGEN_DISABLE);
        p = xgu_set_texture_matrix_enable(p, i, false);
        p = xgu_set_texture_matrix(p, i, mIdentity);
        pb_end(p);
    }

    for (int32 i = 0; i < XGU_WEIGHT_COUNT; ++i) {
        p = pb_begin();
        p = xgu_set_model_view_matrix(p, i, mIdentity);
        p = xgu_set_inverse_model_view_matrix(p, i, mIdentity);
        pb_end(p);
    }

    for (int32 i = 0; i < XGU_ATTRIBUTE_COUNT; ++i)
        xgux_set_attrib_pointer((XguVertexArray)i, XGU_FLOAT, 0, 0, NULL);

    // Fixed-function transform, identity projection + unit viewport: vertex positions
    // are taken as screen-space (back-buffer) pixels — a fullscreen quad is 0..640/0..480.
    p = pb_begin();
    p = xgu_set_transform_execution_mode(p, XGU_FIXED, XGU_RANGE_MODE_PRIVATE);
    p = xgu_set_projection_matrix(p, mIdentity);
    p = xgu_set_composite_matrix(p, mIdentity);
    p = xgu_set_viewport_offset(p, 0.0f, 0.0f, 0.0f, 0.0f);
    p = xgu_set_viewport_scale(p, 1.0f, 1.0f, 1.0f, 1.0f);
    p = xgu_set_scissor_rect(p, false, 0, 0, pb_back_buffer_width(), pb_back_buffer_height());
    pb_end(p);

    // Bind a render-target DMA context spanning all of RAM (channel 3). Mandatory — the
    // GPU DMAs vertex/texture data through it; omitting it renders garbage VRAM (the
    // full-screen noise). Matches the nxdk_xgu renderer's setup.
    pb_create_dma_ctx(3, DMA_CLASS_3D, 0, MAXRAM, &renderTargetDmaCtx);
    pb_bind_channel(&renderTargetDmaCtx);

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

bool RenderDevice::InitGraphicsAPI()
{
    videoSettings.shaderSupport = false;

    // Pin true 4:3 (see SDL3 device / [[xbox-4-3-rendering]]): 320x240 maps to the full
    // 640x480 output at 2x with no letterbox bars.
    videoSettings.pixWidth = 320;

    viewSize.x = videoSettings.windowWidth;
    viewSize.y = videoSettings.windowHeight;

    for (int32 s = 0; s < 4; ++s) {
        screens[s].size.y = videoSettings.pixHeight;
        int32 screenWidth = videoSettings.pixWidth;
        if (customSettings.maxPixWidth && screenWidth > customSettings.maxPixWidth)
            screenWidth = customSettings.maxPixWidth;

        memset(&screens[s].frameBuffer, 0, sizeof(screens[s].frameBuffer));
        SetScreenSize(s, screenWidth, screens[s].size.y);
    }

    pixelSize.x   = screens[0].size.x;
    pixelSize.y   = screens[0].size.y;
    textureSize.x = PB_FB_TEX_W;
    textureSize.y = PB_FB_TEX_H;

    // Present texture: linear RGB565, POT container. Write-combined contiguous memory
    // so the GPU can sample it and CPU writes stream efficiently.
    fbTex.pitch = PB_FB_TEX_W * 2;
    if (!fbTex.data) {
        fbTex.data = (uint8 *)MmAllocateContiguousMemoryEx((SIZE_T)PB_FB_TEX_H * fbTex.pitch, 0, 0xFFFFFFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
        if (!fbTex.data) {
            PrintLog(PRINT_NORMAL, "ERROR: pbkit present texture alloc failed");
            return false;
        }
        fbTex.phys = (uint8 *)MmGetPhysicalAddress(fbTex.data);
        memset(fbTex.data, 0, (size_t)PB_FB_TEX_H * fbTex.pitch);
    }
    if (!presentVerts)
        presentVerts = (PBVertex *)MmAllocateContiguousMemoryEx(6 * sizeof(PBVertex), 0, 0xFFFFFFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);

    // Stage 2: 8x256 ARGB CLUT (64B-aligned contiguous) + the sprite quad vertex ring.
    if (!pbClut) {
        pbClut     = (uint32 *)MmAllocateContiguousMemoryEx(PALETTE_BANK_COUNT * 256 * sizeof(uint32), 0, 0xFFFFFFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
        pbClutPhys = pbClut ? (uint8 *)MmGetPhysicalAddress(pbClut) : nullptr;
    }
    if (!sprVerts)
        sprVerts = (PBVertex *)MmAllocateContiguousMemoryEx(MAX_SPR_VERTS * sizeof(PBVertex), 0, 0xFFFFFFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);

    lastShaderID = -1;
    InitVertexBuffer();
    engine.inFocus          = 1;
    videoSettings.viewportX = 0;
    videoSettings.viewportY = 0;
    videoSettings.viewportW = 1.0 / viewSize.x;
    videoSettings.viewportH = 1.0 / viewSize.y;

    return true;
}

void RenderDevice::InitVertexBuffer() {} // pbkit present quad is built per-frame in FlipScreen

void RenderDevice::Release(bool32 isRefresh)
{
    if (fbTex.data) {
        MmFreeContiguousMemory(fbTex.data);
        fbTex.data = nullptr;
        fbTex.phys = nullptr;
    }

    if (!isRefresh) {
        if (presentVerts) {
            MmFreeContiguousMemory(presentVerts);
            presentVerts = nullptr;
        }
        if (pbClut) {
            MmFreeContiguousMemory(pbClut);
            pbClut     = nullptr;
            pbClutPhys = nullptr;
        }
        if (sprVerts) {
            MmFreeContiguousMemory(sprVerts);
            sprVerts = nullptr;
        }
        for (int32 i = 0; i < SURFACE_COUNT; ++i) {
            if (pbSurfTex[i].data) {
                MmFreeContiguousMemory(pbSurfTex[i].data);
                pbSurfTex[i].data      = nullptr;
                pbSurfTex[i].builtFrom = nullptr;
            }
        }
        if (displayInfo.displays)
            free(displayInfo.displays);
        displayInfo.displays = nullptr;

        if (window)
            SDL_DestroyWindow(window);
        window = nullptr;

        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);

        if (scanlines)
            free(scanlines);
        scanlines = nullptr;
    }
}

void RenderDevice::RefreshWindow()
{
    videoSettings.windowState = WINDOWSTATE_UNINITIALIZED;
    Release(true);
    GetDisplays();
    if (!InitGraphicsAPI() || !InitShaders())
        return;
    videoSettings.windowState = WINDOWSTATE_ACTIVE;
}

// -----------------------------------------------------------------------------
// Frame: upload framebuffer -> present as one fullscreen pbkit quad
// -----------------------------------------------------------------------------
void RenderDevice::CopyFrameBuffer()
{
    // Stage 1: upload screen 0's RGB565 framebuffer into the linear present texture.
    // (Splitscreen present is Stage 7; menus/gameplay are single-screen.)
    uint16 *src = screens[0].frameBuffer;
    uint16 *dst = (uint16 *)fbTex.data;
    int32 w     = screens[0].size.x;
    int32 h     = screens[0].size.y;
    int32 dstStride = fbTex.pitch / (int32)sizeof(uint16);
    for (int32 y = 0; y < h; ++y) {
        memcpy(dst, src, w * sizeof(uint16));
        src += screens[0].pitch;
        dst += dstStride;
    }

}

void RenderDevice::FlipScreen()
{
    if (windowRefreshDelay > 0) {
        windowRefreshDelay--;
        if (!windowRefreshDelay)
            RefreshWindow();
        return;
    }

    const float bw = (float)pb_back_buffer_width();
    const float bh = (float)pb_back_buffer_height();

    // UVs in TEXELS: the NV2A samples LINEAR textures (this RGB565 present texture) with
    // texel coordinates, not normalized [0,1] (swizzled textures use normalized — that's
    // why the I8 sprite path uses 0..1). The framebuffer occupies the top-left
    // size.x x size.y texels of the POT container.
    const float u1 = (float)screens[0].size.x;
    const float v1 = (float)screens[0].size.y;

    // Dimming (fades): modulate the present quad's vertex color.
    float dimAmount = videoSettings.dimMax * videoSettings.dimPercent;
    if (dimAmount > 1.0f)
        dimAmount = 1.0f;
    const uint8 lum = (uint8)(dimAmount * 0xFF);

    // Fullscreen quad, two triangles (TL,TR,BL / TR,BR,BL) in back-buffer pixels.
    const float px[4] = { 0.0f, bw, 0.0f, bw };
    const float py[4] = { 0.0f, 0.0f, bh, bh };
    const float tu[4] = { 0.0f, u1, 0.0f, u1 };
    const float tv[4] = { 0.0f, 0.0f, v1, v1 };
    const int32 order[6] = { 0, 1, 2, 1, 3, 2 };
    for (int32 i = 0; i < 6; ++i) {
        int32 c              = order[i];
        presentVerts[i].pos[0] = px[c];
        presentVerts[i].pos[1] = py[c];
        presentVerts[i].color[0] = lum;
        presentVerts[i].color[1] = lum;
        presentVerts[i].color[2] = lum;
        presentVerts[i].color[3] = 0xFF;
        presentVerts[i].tex[0] = tu[c];
        presentVerts[i].tex[1] = tv[c];
    }

    pb_target_back_buffer();

    // Defensive clear so any area the present quad doesn't cover is black, not garbage.
    pb_fill(0, 0, (int)bw, (int)bh, 0xFF000000);

    // Bind the present texture (linear RGB565) and draw the quad. Reset the blend func
    // to standard alpha-over (sprite batches leave it at their last mode).
    p = pb_begin();
    texture_combiner_apply();
    p = xgu_set_blend_func_sfactor(p, XGU_FACTOR_SRC_ALPHA);
    p = xgu_set_blend_func_dfactor(p, XGU_FACTOR_ONE_MINUS_SRC_ALPHA);
    p = xgu_set_texture_offset(p, 0, fbTex.phys);
    p = xgu_set_texture_format(p, 0, 2, false, XGU_SOURCE_COLOR, 2, XGU_TEXTURE_FORMAT_R5G6B5, 1, __builtin_ctz(PB_FB_TEX_W), __builtin_ctz(PB_FB_TEX_H), 0);
    p = xgu_set_texture_control0(p, 0, true, 0, 0);
    p = xgu_set_texture_control1(p, 0, fbTex.pitch);
    p = xgu_set_texture_image_rect(p, 0, PB_FB_TEX_W, PB_FB_TEX_H);
    p = xgu_set_texture_filter(p, 0, 0, XGU_TEXTURE_CONVOLUTION_GAUSSIAN, XGU_TEXTURE_FILTER_NEAREST, XGU_TEXTURE_FILTER_NEAREST, false, false, false, false);
    p = xgu_set_texture_address(p, 0, XGU_CLAMP_TO_EDGE, false, XGU_CLAMP_TO_EDGE, false, XGU_CLAMP_TO_EDGE, false, false);
    pb_end(p);

    xgux_set_attrib_pointer(XGU_VERTEX_ARRAY, XGU_FLOAT, 2, sizeof(PBVertex), presentVerts->pos);
    xgux_set_attrib_pointer(XGU_COLOR_ARRAY, XGU_UNSIGNED_BYTE_OGL, 4, sizeof(PBVertex), presentVerts->color);
    xgux_set_attrib_pointer(XGU_TEXCOORD0_ARRAY, XGU_FLOAT, 2, sizeof(PBVertex), presentVerts->tex);
    xgux_draw_arrays(XGU_TRIANGLES, 0, 6);

    // Stage 2: sync the 8-bank CLUT from the (possibly cycled) palette, then draw the
    // accumulated GPU sprite quads over the framebuffer background, in draw order.
    PBSyncCLUT();
    FlushSpriteBatches();

#ifdef PBKIT_I8_TEST
    I8Test_Draw(); // Stage 0 hardware checkpoint: paletted I8 + animated CLUT overlay
#endif

    // Present + pace: wait for the GPU, swap on vblank, reset the push buffer.
    while (pb_busy())
        Sleep(0);
    while (pb_finished())
        Sleep(0);
    pb_wait_for_vbl();
    pb_reset();

    // Reset sprite accumulation for the next frame.
    sprVertCount  = 0;
    sprBatchCount = 0;
}

// -----------------------------------------------------------------------------
// Displays / events / FPS cap (SDL — input path unchanged)
// -----------------------------------------------------------------------------
void RenderDevice::GetDisplays()
{
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

void RenderDevice::GetWindowSize(int32 *width, int32 *height)
{
    if (width)
        *width = videoSettings.windowWidth;
    if (height)
        *height = videoSettings.windowHeight;
}

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

void RenderDevice::InitFPSCap()
{
    targetFreq = SDL_GetPerformanceFrequency() / videoSettings.refreshRate;
    curTicks   = 0;
    prevTicks  = 0;
}
bool RenderDevice::CheckFPSCap()
{
    curTicks = SDL_GetPerformanceCounter();
    if (curTicks >= prevTicks + targetFreq)
        return true;
    return false;
}
void RenderDevice::UpdateFPSCap() { prevTicks = curTicks; }
void RenderDevice::SetFPSTarget(int32 fps)
{
    if (fps < 1)
        fps = 1;
    targetFreq = SDL_GetPerformanceFrequency() / fps;
}

// -----------------------------------------------------------------------------
// Shaders (unsupported) / image + video textures (Stage 6) / GPU offload (Stage 2/3)
// -----------------------------------------------------------------------------
void RenderDevice::LoadShader(const char *fileName, bool32 linear) { (void)fileName; (void)linear; }

bool RenderDevice::InitShaders()
{
    videoSettings.shaderSupport = false;
    for (int32 s = 0; s < SHADER_COUNT; ++s) shaderList[s].linear = true;
    shaderList[0].linear = videoSettings.windowed ? false : shaderList[0].linear;
    shaderCount          = 1;
    videoSettings.shaderID = videoSettings.shaderID >= 1 ? 0 : videoSettings.shaderID;
    return true;
}

// FMV / image present is ported to pbkit in Stage 6; no-op stubs for now (FMV skips).
void RenderDevice::SetupImageTexture(int32 width, int32 height, uint8 *imagePixels) { (void)width; (void)height; (void)imagePixels; }
void RenderDevice::SetupVideoTexture_YUV420(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 sy, int32 su, int32 sv)
{
    (void)width; (void)height; (void)yPlane; (void)uPlane; (void)vPlane; (void)sy; (void)su; (void)sv;
}
void RenderDevice::SetupVideoTexture_YUV422(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 sy, int32 su, int32 sv)
{
    (void)width; (void)height; (void)yPlane; (void)uPlane; (void)vPlane; (void)sy; (void)su; (void)sv;
}
void RenderDevice::SetupVideoTexture_YUV444(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 sy, int32 su, int32 sv)
{
    (void)width; (void)height; (void)yPlane; (void)uPlane; (void)vPlane; (void)sy; (void)su; (void)sv;
}

// Scene3D solid-face offload (Stage 3): Draw3DScene routes its sorted solid faces here
// (via the EMIT_FACE / EMIT_BLENDED macros) when Use3DOffload() is true; they become
// untextured GPU polys in the same ordered list as sprites, composited over the fb.
// Gameplay-only (same gate as sprites) — Add3DFace is only called from Draw3DScene anyway.
bool RenderDevice::Use3DOffload()
{
    return gpu3DEnabled && sprVerts && videoSettings.screenCount == 1 && sceneInfo.state == ENGINESTATE_REGULAR;
}

// Ink -> blend for faces; unsupported inks fall back to opaque (rather than dropping).
static inline void FaceInkToBlend(int32 inkEffect, int32 alpha, XguBlendFactor *sf, XguBlendFactor *df, float *a)
{
    if (!SprInkToBlend(inkEffect, alpha, sf, df, a)) {
        *sf = XGU_FACTOR_SRC_ALPHA;
        *df = XGU_FACTOR_ONE_MINUS_SRC_ALPHA;
        *a  = 1.0f;
    }
}

void RenderDevice::Add3DFace(Vector2 *vertices, int32 vertCount, int32 r, int32 g, int32 b, int32 alpha, int32 inkEffect)
{
    if (vertCount < 3 || !sprVerts)
        return;
    if (vertCount > 4)
        vertCount = 4;
    uint32 rgb       = ((uint32)(r & 0xFF) << 16) | ((uint32)(g & 0xFF) << 8) | (uint32)(b & 0xFF);
    uint32 colors[4] = { rgb, rgb, rgb, rgb };
    XguBlendFactor sf, df;
    float a;
    FaceInkToBlend(inkEffect, alpha, &sf, &df, &a);
    EmitColoredPoly(vertices, colors, vertCount, (uint8)(a * 255.0f), sf, df);
}

void RenderDevice::Add3DBlendedFace(Vector2 *vertices, uint32 *colors, int32 vertCount, int32 alpha, int32 inkEffect)
{
    if (vertCount < 3 || !sprVerts)
        return;
    if (vertCount > 4)
        vertCount = 4;
    XguBlendFactor sf, df;
    float a;
    FaceInkToBlend(inkEffect, alpha, &sf, &df, &a);
    EmitColoredPoly(vertices, colors, vertCount, (uint8)(a * 255.0f), sf, df);
}
// Rotozoom / scaled sprites (Stage 2): DrawSpriteRotozoom hands us the 4 transformed
// corners (rotation + scale + flip already applied); draw them as an I8 textured GPU
// quad instead of the software fill. Returns true = handled on GPU.
bool RenderDevice::DrawSpriteGPU(int32 *posX, int32 *posY, int32 sprX, int32 sprY, int32 width, int32 height, int32 sheetID, int32 inkEffect,
                                 int32 alpha)
{
#ifdef PBKIT_NO_GPU_SPRITES
    return false;
#endif
    if (!PBSpriteOffloadOK() || width <= 0 || height <= 0)
        return false;
    XguBlendFactor sf, df;
    float a;
    if (!SprInkToBlend(inkEffect, alpha, &sf, &df, &a))
        return false;
    PBSurfTex *t = GetSurfaceTexture(sheetID);
    if (!t)
        return false;

    // Sanity: DrawSpriteRotozoom only fills valid corners for FLIP_NONE/FLIP_X; FLIP_Y/
    // FLIP_XY leave them unset. We don't get `direction`, so reject wildly out-of-range
    // corners (a garbage screen-covering quad) and let the software path handle them.
    for (int32 i = 0; i < 4; ++i)
        if (posX[i] < -2048 || posX[i] > 2048 || posY[i] < -2048 || posY[i] > 2048)
            return false;

    // Palette bank at the topmost corner.
    int32 topY = posY[0];
    for (int32 i = 1; i < 4; ++i)
        if (posY[i] < topY)
            topY = posY[i];
    topY       = topY < 0 ? 0 : (topY >= SCREEN_YSIZE ? SCREEN_YSIZE - 1 : topY);
    int32 bank = gfxLineBuffer[topY] & (PALETTE_BANK_COUNT - 1);

    // The corners span the sprite PLUS a 2px margin on each edge (DrawSpriteRotozoom's
    // pivot-2 .. pivot+2+width corner math — extra coverage for the software rotate/fill).
    // Feeding that margin to the GPU sampled adjacent sheet texels -> a dark fringe around
    // rotated sprites. Instead INSET the quad to the exact sprite via bilinear interpolation
    // of the 4 corners, and map UVs to exactly the sprite (no margin).
    float sx = (float)pb_back_buffer_width() / (float)videoSettings.pixWidth;
    float sy = (float)pb_back_buffer_height() / (float)SCREEN_YSIZE;
    // Corner order matches DrawSpriteRotozoom: [0]=TL, [1]=TR, [2]=BL, [3]=BR. Bilinear:
    // c(u,v) = (1-u)(1-v)TL + u(1-v)TR + (1-u)v BL + uv BR.
    const float cx[4] = { posX[0] * sx, posX[1] * sx, posX[2] * sx, posX[3] * sx };
    const float cy[4] = { posY[0] * sy, posY[1] * sy, posY[2] * sy, posY[3] * sy };
    float fu = 2.0f / (float)(width + 4);
    float fv = 2.0f / (float)(height + 4);
    const float cu[4] = { fu, 1.0f - fu, fu, 1.0f - fu };       // TL TR BL BR
    const float cv[4] = { fv, fv, 1.0f - fv, 1.0f - fv };
    float px[4], py[4];
    for (int32 i = 0; i < 4; ++i) {
        float u = cu[i], v = cv[i];
        float w0 = (1 - u) * (1 - v), w1 = u * (1 - v), w2 = (1 - u) * v, w3 = u * v;
        px[i]    = w0 * cx[0] + w1 * cx[1] + w2 * cx[2] + w3 * cx[3];
        py[i]    = w0 * cy[0] + w1 * cy[1] + w2 * cy[2] + w3 * cy[3];
    }
    // Half-texel inset: at heavy minification (far billboards) the edge UVs land on the
    // frame boundary and NEAREST bleeds the adjacent sheet frame -> a thin border. Keep
    // the edge samples strictly inside this frame.
    float u0 = (float)(sprX + 0.5f) / (float)t->texW, u1 = (float)(sprX + width - 0.5f) / (float)t->texW;
    float v0 = (float)(sprY + 0.5f) / (float)t->texH, v1 = (float)(sprY + height - 0.5f) / (float)t->texH;
    if (EmitSpriteQuadCorners(t, bank, sf, df, a, px, py, u0, v0, u1, v1)) {
        validDraw = true;
        return true;
    }
    return false;
}
// Real paletted-quad path (Stage 2): draw an unscaled sprite as an I8 textured GPU quad
// instead of the software blit. Returns true = handled on GPU.
bool RenderDevice::DrawSpriteFlippedGPU(int32 x, int32 y, int32 width, int32 height, int32 sprX, int32 sprY, int32 direction, int32 sheetID,
                                        int32 inkEffect, int32 alpha)
{
#ifdef PBKIT_NO_GPU_SPRITES
    // Diagnostic (NOSPR=y): force the software path -> full Stage-1 all-in-framebuffer
    // rendering, to isolate whether the GPU sprite path is causing a regression.
    return false;
#endif
    // Gameplay-only (see PBSpriteOffloadOK); single-screen; GPU-supported ink.
    if (!PBSpriteOffloadOK() || width <= 0 || height <= 0)
        return false;
    XguBlendFactor sf, df;
    float a;
    if (!SprInkToBlend(inkEffect, alpha, &sf, &df, &a))
        return false;
    PBSurfTex *t = GetSurfaceTexture(sheetID);
    if (!t)
        return false;

    // Palette bank at the sprite's top scanline (per-line palette assumed uniform over
    // the sprite — same approximation the software per-scanline path collapses to here).
    int32 topY = y < 0 ? 0 : (y >= SCREEN_YSIZE ? SCREEN_YSIZE - 1 : y);
    int32 bank = gfxLineBuffer[topY] & (PALETTE_BANK_COUNT - 1);

    float u0 = (float)sprX / (float)t->texW, u1 = (float)(sprX + width) / (float)t->texW;
    float v0 = (float)sprY / (float)t->texH, v1 = (float)(sprY + height) / (float)t->texH;
    if (direction & FLIP_X) {
        float tmp = u0;
        u0        = u1;
        u1        = tmp;
    }
    if (direction & FLIP_Y) {
        float tmp = v0;
        v0        = v1;
        v1        = tmp;
    }

    // Logical (screen) pixels -> back-buffer pixels (the fb present maps pixWidth x
    // SCREEN_YSIZE onto the full 640x480, so GPU sprites use the same scale to align).
    float sx = (float)pb_back_buffer_width() / (float)videoSettings.pixWidth;
    float sy = (float)pb_back_buffer_height() / (float)SCREEN_YSIZE;
    if (EmitSpriteQuad(t, bank, sf, df, a, x * sx, y * sy, (x + width) * sx, (y + height) * sy, u0, v0, u1, v1)) {
        validDraw = true; // mirror the software path so entity on-screen tracking works
        return true;
    }
    return false;
}
