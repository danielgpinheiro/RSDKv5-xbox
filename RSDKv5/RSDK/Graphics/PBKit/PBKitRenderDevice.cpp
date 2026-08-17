
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

// FMV / image present (Stage 6): a linear RGB565 texture in a POT container the game's
// image/video frame is converted into, drawn as one fullscreen quad when screenCount==0.
uint8 *imgTexData = nullptr, *imgTexPhys = nullptr;
int32 imgTexW = 0, imgTexH = 0; // POT container
int32 imgW = 0, imgH = 0;       // actual frame size (top-left of the container)

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
#define MAX_SPR_VERTS   (49152) // 8192 quads — tile layers push far more geometry than sprites
#define MAX_SPR_BATCHES (4096)
PBVertex *sprVerts = nullptr;
int32 sprVertCount = 0;
struct SprBatch {
    int32 start, count;
    uint8 *texPhys;
    int32 texW, texH;
    int32 bank;
    XguBlendFactor sfactor, dfactor;
    // Scissor in back-buffer pixels (tile-layer strips clip to their scanline band). A
    // full-screen clip means "no clip"; -1 width = unset (use full screen).
    int32 clipX, clipY, clipW, clipH;
    // Per-channel constant blend color (ARGB) for FillScreen fades: when sfactor is
    // XGU_FACTOR_CONSTANT_COLOR the flush pushes this via NV097_SET_BLEND_COLOR so the
    // R/G/B channels each blend by their own alpha. 0 on every normal batch (zero-filled
    // by the 12-field aggregate initializers, which is why it lives last).
    uint32 blendColor;
};
SprBatch sprBatches[MAX_SPR_BATCHES];
int32 sprBatchCount = 0;
// Current clip applied to newly-emitted batches (back-buffer px; clipW<0 = full screen).
int32 curClipX = 0, curClipY = 0, curClipW = -1, curClipH = -1;
inline bool BatchClipMatches(const SprBatch &b) { return b.clipX == curClipX && b.clipY == curClipY && b.clipW == curClipW && b.clipH == curClipH; }

// Dimming (fades / pause): the framebuffer present quad already modulates by this; GPU
// sprites/tiles/polys must too, or they stay bright while the fb darkens. 0..1.
inline float PBDim()
{
    float d = videoSettings.dimMax * videoSettings.dimPercent;
    return d > 1.0f ? 1.0f : (d < 0.0f ? 0.0f : d);
}

// Only offload sprites during live gameplay. GPU sprites composite over the software
// framebuffer (drawn last, on top), which breaks the interleaved draw order of overlay
// screens — the pause menu, results, dev menu — where panels are software (fb) and text
// is GPU. Keeping those on the CPU path preserves their order; gameplay (where the perf
// matters) stays on the GPU. Mirrors the SDL3 special-stage offload's ENGINESTATE gate.
inline bool PBSpriteOffloadOK()
{
    return sprVerts && pbClut && videoSettings.screenCount == 1 && sceneInfo.state == ENGINESTATE_REGULAR;
}

// Is the current scene a special stage? Its layers use effects our GPU tile path doesn't
// model (the background garbles); keep them on the software path until Stage 5 (Mode-7).
inline bool PBInSpecialStage()
{
    return sceneInfo.listCategory && sceneInfo.listCategory[sceneInfo.activeCategory].name
           && strstr(sceneInfo.listCategory[sceneInfo.activeCategory].name, "Special");
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
                    && BatchClipMatches(sprBatches[sprBatchCount - 1])
                    && sprBatches[sprBatchCount - 1].start + sprBatches[sprBatchCount - 1].count == sprVertCount;
    if (!coalesce && sprBatchCount >= MAX_SPR_BATCHES)
        return false;

    uint8 alpha8 = (uint8)(a * 255.0f);
    uint8 lum    = (uint8)(255.0f * PBDim()); // fade/dim modulation (tex * this)
    const float tu[4] = { u0, u1, u0, u1 };
    const float tv[4] = { v0, v0, v1, v1 };
    const int32 order[6] = { 0, 1, 2, 1, 3, 2 };
    int32 start = sprVertCount;
    for (int32 i = 0; i < 6; ++i) {
        int32 c              = order[i];
        PBVertex *v          = &sprVerts[sprVertCount++];
        v->pos[0]            = px[c];
        v->pos[1]            = py[c];
        v->color[0]          = lum;
        v->color[1]          = lum;
        v->color[2]          = lum;
        v->color[3]          = alpha8;
        v->tex[0]            = tu[c];
        v->tex[1]            = tv[c];
    }
    if (coalesce)
        sprBatches[sprBatchCount - 1].count += 6;
    else
        sprBatches[sprBatchCount++] = { start, 6, t->phys, t->texW, t->texH, bank, sf, df, curClipX, curClipY, curClipW, curClipH };
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

// Textured quad with fully independent per-corner UVs (Mode-7 floor strips / deformed sprite
// strips) and an explicit texture (not a PBSurfTex). `bankWrap` low 3 bits = palette bank; bit
// 8 (0x100) = WRAP address. Corner order TL,TR,BL,BR. Vertex color = white*dim, alpha = alpha8
// (opaque floor passes 0xFF; deform passes the ink alpha for BLEND/ALPHA/ADD).
bool EmitTexQuadUV(uint8 *texPhys, int32 texW, int32 texH, int32 bankWrap, XguBlendFactor sf, XguBlendFactor df, float dim, uint8 alpha8,
                   const float px[4], const float py[4], const float cu[4], const float cv[4])
{
    if (!sprVerts || sprVertCount + 6 > MAX_SPR_VERTS)
        return false;
    bool coalesce = sprBatchCount > 0 && sprBatches[sprBatchCount - 1].texPhys == texPhys && sprBatches[sprBatchCount - 1].bank == bankWrap
                    && sprBatches[sprBatchCount - 1].sfactor == sf && sprBatches[sprBatchCount - 1].dfactor == df
                    && BatchClipMatches(sprBatches[sprBatchCount - 1])
                    && sprBatches[sprBatchCount - 1].start + sprBatches[sprBatchCount - 1].count == sprVertCount;
    if (!coalesce && sprBatchCount >= MAX_SPR_BATCHES)
        return false;

    uint8 lum            = (uint8)(255.0f * dim);
    const int32 order[6] = { 0, 1, 2, 1, 3, 2 };
    int32 start          = sprVertCount;
    for (int32 i = 0; i < 6; ++i) {
        int32 c     = order[i];
        PBVertex *v = &sprVerts[sprVertCount++];
        v->pos[0]   = px[c];
        v->pos[1]   = py[c];
        v->color[0] = lum;
        v->color[1] = lum;
        v->color[2] = lum;
        v->color[3] = alpha8;
        v->tex[0]   = cu[c];
        v->tex[1]   = cv[c];
    }
    if (coalesce)
        sprBatches[sprBatchCount - 1].count += 6;
    else
        sprBatches[sprBatchCount++] = { start, 6, texPhys, texW, texH, bankWrap, sf, df, curClipX, curClipY, curClipW, curClipH };
    return true;
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
                    && sprBatches[sprBatchCount - 1].dfactor == df && BatchClipMatches(sprBatches[sprBatchCount - 1])
                    && sprBatches[sprBatchCount - 1].start + sprBatches[sprBatchCount - 1].count == sprVertCount;
    if (!coalesce && sprBatchCount >= MAX_SPR_BATCHES)
        return false;

    float sx = (float)pb_back_buffer_width() / (float)videoSettings.pixWidth;
    float sy = (float)pb_back_buffer_height() / (float)SCREEN_YSIZE;
    float dim   = PBDim(); // fade/dim modulation
    int32 start = sprVertCount;
    for (int32 tri = 1; tri + 1 < vertCount; ++tri) {
        int32 idx[3] = { 0, tri, tri + 1 };
        for (int32 k = 0; k < 3; ++k) {
            int32 vi    = idx[k];
            PBVertex *v = &sprVerts[sprVertCount++];
            v->pos[0]   = (vertices[vi].x / 65536.0f) * sx;
            v->pos[1]   = (vertices[vi].y / 65536.0f) * sy;
            uint32 c    = colors[vi];
            v->color[0] = (uint8)(((c >> 16) & 0xFF) * dim);
            v->color[1] = (uint8)(((c >> 8) & 0xFF) * dim);
            v->color[2] = (uint8)((c & 0xFF) * dim);
            v->color[3] = alpha8;
            v->tex[0]   = 0.0f;
            v->tex[1]   = 0.0f;
        }
    }
    int32 count = sprVertCount - start;
    if (coalesce)
        sprBatches[sprBatchCount - 1].count += count;
    else
        sprBatches[sprBatchCount++] = { start, count, nullptr, 0, 0, 0, sf, df, curClipX, curClipY, curClipW, curClipH };
    return true;
}

// --- tileset atlas + tile layers (Stage 4) -----------------------------------
// tilesetPixels is tile-major (each tile = 256 contiguous bytes = 16 rows of 16) with 4
// pre-flipped copies; a layout entry's low 12 bits index directly into that 0..4095 space.
// Re-arrange into one 1024x1024 I8 atlas (64x64 tiles) so tiles draw as GPU quads, rebuilt
// on scene change. (Animated tiles / DrawAniTile are not yet re-uploaded — deferred.)
#define TILE_ATLAS_DIM  (1024)
#define TILE_ATLAS_COLS (64)
uint8 *tileAtlasData = nullptr, *tileAtlasPhys = nullptr;
int32 tileAtlasSceneKey = -1;

// Mode-7 floor (Stage 5): the rotozoom layer's tilemap composed into one POT I8 texture,
// WRAP-addressed + swizzled, so per-scanline strip quads can sample across tiles. Rebuilt
// per (scene + layer); capped to protect the 64MB budget.
#define FLOOR_TEX_MAX (1024)
uint8 *floorTexData = nullptr, *floorTexPhys = nullptr;
int32 floorW = 0, floorH = 0, floorKey = -1;

void BuildTilesetAtlas()
{
    if (!tileAtlasData) {
        tileAtlasData =
            (uint8 *)MmAllocateContiguousMemoryEx((SIZE_T)TILE_ATLAS_DIM * TILE_ATLAS_DIM, 0, 0xFFFFFFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
        if (!tileAtlasData)
            return;
        tileAtlasPhys = (uint8 *)MmGetPhysicalAddress(tileAtlasData);
    }
    uint8 *tmp = (uint8 *)malloc((size_t)TILE_ATLAS_DIM * TILE_ATLAS_DIM);
    if (!tmp)
        return;
    for (int32 idx = 0; idx < TILE_ATLAS_COLS * TILE_ATLAS_COLS; ++idx) {
        int32 gx   = (idx % TILE_ATLAS_COLS) * TILE_SIZE;
        int32 gy   = (idx / TILE_ATLAS_COLS) * TILE_SIZE;
        uint8 *src = &tilesetPixels[idx * TILE_DATASIZE];
        for (int32 row = 0; row < TILE_SIZE; ++row) memcpy(&tmp[(gy + row) * TILE_ATLAS_DIM + gx], &src[row * TILE_SIZE], TILE_SIZE);
    }
    swizzle_rect(tmp, TILE_ATLAS_DIM, TILE_ATLAS_DIM, tileAtlasData, TILE_ATLAS_DIM, 1);
    free(tmp);
}

// Swizzled base (byte offset) of a 16-aligned tile at atlas grid (col,row). The NV2A swizzle
// of a square texture interleaves X into the even address bits and Y into the odd bits (the
// generate_swizzle_masks "yxyx" pattern in swizzle.c); a 16x16 tile therefore occupies one
// contiguous 256-byte Morton block at interleave(col,row) << 8 — matching what BuildTilesetAtlas
// wrote for that tile, so a single tile can be re-swizzled in place.
static inline uint32 TileSwizzleBase(uint32 col, uint32 row)
{
    uint32 m = 0;
    for (uint32 b = 0; b < 6; ++b) {
        m |= ((col >> b) & 1u) << (2 * b);
        m |= ((row >> b) & 1u) << (2 * b + 1);
    }
    return m << 8;
}

// Re-upload animated tiles into the GPU atlas. DrawAniTile rewrites tilesetPixels for `cnt`
// consecutive tiles plus their three pre-flipped copies (at +FLIP_*·TILESET_SIZE); the atlas
// was built once, so without this the tiles (waterfalls, conveyors, lava) freeze on the GPU.
// Re-swizzle just those tiles' 16x16 blocks — cheap (a handful of 256-byte blocks per frame).
// (Free function in the anon namespace; RenderDevice::UpdateAniTileGPU below forwards to it.)
void UpdateAniTileAtlas(int32 tileIndex, int32 cnt)
{
    if (!tileAtlasData || cnt <= 0)
        return;
    for (int32 f = 0; f < 4; ++f) {
        for (int32 t = 0; t < cnt; ++t) {
            int32 ti = tileIndex + t + f * TILE_COUNT; // base + flip block (0..4095)
            if (ti < 0 || ti >= TILE_ATLAS_COLS * TILE_ATLAS_COLS)
                continue;
            uint8 *src   = &tilesetPixels[ti * TILE_DATASIZE]; // linear 16x16
            uint32 base  = TileSwizzleBase((uint32)(ti % TILE_ATLAS_COLS), (uint32)(ti / TILE_ATLAS_COLS));
            swizzle_rect(src, TILE_SIZE, TILE_SIZE, tileAtlasData + base, TILE_SIZE, 1);
        }
    }
}

bool DrawLayerHScrollStripGPU(TileLayer *layer); // defined below; used for high-parallax layers

// One HScroll tile layer as GPU quads. Screen scanlines are grouped into constant-X bands
// (parallax); each band's visible tiles draw clipped (scissor) to its screen-Y range. Many
// bands (per-scanline parallax) -> per-scanline composed-layer strips (or software if too big).
bool DrawLayerHScrollGPU(TileLayer *layer)
{
    if (!layer->xsize || !layer->ysize)
        return true;
    if (!tileAtlasData)
        return false;

    int32 clipY1 = currentScreen->clipBound_Y1, clipY2 = currentScreen->clipBound_Y2;
    int32 clipX2 = currentScreen->clipBound_X2;

    // Pre-scan: count constant-X bands. Few bands -> the cheap per-tile band path below. Many
    // bands = per-scanline parallax; render those as composed-layer strips (S6.7), falling back
    // to software only if the layer is too big to compose.
    int32 bands = 1;
    for (int32 y = clipY1 + 1; y < clipY2; ++y)
        if (scanlines[y].position.x != scanlines[y - 1].position.x)
            ++bands;
    if (bands > 48)
        return DrawLayerHScrollStripGPU(layer);

    PBSurfTex atlas;
    atlas.phys = tileAtlasPhys;
    atlas.texW = TILE_ATLAS_DIM;
    atlas.texH = TILE_ATLAS_DIM;

    float sx          = (float)pb_back_buffer_width() / (float)videoSettings.pixWidth;
    float sy          = (float)pb_back_buffer_height() / (float)SCREEN_YSIZE;
    int32 pixelWidth  = TILE_SIZE * layer->xsize;
    const float hu    = 0.5f / TILE_ATLAS_DIM; // half-texel inset (avoid atlas bleed)

    int32 cy = clipY1;
    while (cy < clipY2) {
        int32 bandX16 = scanlines[cy].position.x;
        int32 cy0     = cy;
        while (cy < clipY2 && scanlines[cy].position.x == bandX16) ++cy;
        int32 cy1 = cy;

        curClipX = 0;
        curClipY = (int32)(cy0 * sy);
        curClipW = pb_back_buffer_width();
        curClipH = (int32)((cy1 - cy0) * sy);

        int32 srcX = FROM_FIXED(bandX16) % pixelWidth;
        if (srcX < 0)
            srcX += pixelWidth;
        int32 srcY = FROM_FIXED(scanlines[cy0].position.y);
        int32 subX = srcX & 0xF, subY = srcY & 0xF;
        int32 tx0 = srcX >> 4, ty0 = srcY >> 4;

        int32 rowTopY = cy0 - subY;
        for (int32 screenY = rowTopY; screenY < cy1; screenY += TILE_SIZE) {
            int32 ty = (ty0 + (screenY - rowTopY) / TILE_SIZE) % layer->ysize;
            if (ty < 0)
                ty += layer->ysize;
            int32 colTx = tx0;
            for (int32 screenX = -subX; screenX < clipX2; screenX += TILE_SIZE) {
                int32 tx = colTx++ % layer->xsize;
                if (tx < 0)
                    tx += layer->xsize;
                uint16 entry = layer->layout[tx + (ty << layer->widthShift)];
                if (entry < 0xFFFF) {
                    int32 idx  = entry & 0xFFF;
                    int32 gx   = (idx % TILE_ATLAS_COLS) * TILE_SIZE, gy = (idx / TILE_ATLAS_COLS) * TILE_SIZE;
                    float u0   = (float)gx / TILE_ATLAS_DIM + hu, u1 = (float)(gx + TILE_SIZE) / TILE_ATLAS_DIM - hu;
                    float v0   = (float)gy / TILE_ATLAS_DIM + hu, v1 = (float)(gy + TILE_SIZE) / TILE_ATLAS_DIM - hu;
                    int32 py   = screenY < 0 ? 0 : (screenY >= SCREEN_YSIZE ? SCREEN_YSIZE - 1 : screenY);
                    int32 bank = gfxLineBuffer[py] & (PALETTE_BANK_COUNT - 1);
                    EmitSpriteQuad(&atlas, bank, XGU_FACTOR_SRC_ALPHA, XGU_FACTOR_ONE_MINUS_SRC_ALPHA, 1.0f, screenX * sx, screenY * sy,
                                   (screenX + TILE_SIZE) * sx, (screenY + TILE_SIZE) * sy, u0, v0, u1, v1);
                }
            }
        }
    }
    curClipW = -1; // reset to full screen for subsequent batches
    return true;
}

// Emit a rectangular run of atlas tiles for a constant-scroll region. At screen pixel
// (originX,originY) the layer samples layer-space pixel (srcX,srcY); tiles then tile the
// region [originX,clipRight) x [originY,clipBottom) with wrap. All tiles use `bank`
// (Basic/VScroll select a single bank; the HScroll path picks per-scanline itself). The
// caller sets the scissor (curClip*) so partial edge tiles are clipped.
static void EmitTileGridRegion(TileLayer *layer, int32 srcX, int32 srcY, int32 originX, int32 originY, int32 clipRight, int32 clipBottom,
                               int32 bank, float sx, float sy)
{
    PBSurfTex atlas;
    atlas.phys       = tileAtlasPhys;
    atlas.texW       = TILE_ATLAS_DIM;
    atlas.texH       = TILE_ATLAS_DIM;
    const float hu   = 0.5f / TILE_ATLAS_DIM; // half-texel inset (avoid atlas bleed)
    int32 pixelW     = TILE_SIZE * layer->xsize, pixelH = TILE_SIZE * layer->ysize;
    srcX %= pixelW;
    if (srcX < 0)
        srcX += pixelW;
    srcY %= pixelH;
    if (srcY < 0)
        srcY += pixelH;
    int32 subX = srcX & 0xF, subY = srcY & 0xF;
    int32 tx0 = srcX >> 4, ty0 = srcY >> 4;
    bank &= (PALETTE_BANK_COUNT - 1);
    int32 colI = 0;
    for (int32 screenX = originX - subX; screenX < clipRight; screenX += TILE_SIZE, ++colI) {
        int32 tx = (tx0 + colI) % layer->xsize;
        if (tx < 0)
            tx += layer->xsize;
        int32 rowI = 0;
        for (int32 screenY = originY - subY; screenY < clipBottom; screenY += TILE_SIZE, ++rowI) {
            int32 ty = (ty0 + rowI) % layer->ysize;
            if (ty < 0)
                ty += layer->ysize;
            uint16 entry = layer->layout[tx + (ty << layer->widthShift)];
            if (entry >= 0xFFFF)
                continue;
            int32 idx = entry & 0xFFF;
            int32 gx  = (idx % TILE_ATLAS_COLS) * TILE_SIZE, gy = (idx / TILE_ATLAS_COLS) * TILE_SIZE;
            float u0  = (float)gx / TILE_ATLAS_DIM + hu, u1 = (float)(gx + TILE_SIZE) / TILE_ATLAS_DIM - hu;
            float v0  = (float)gy / TILE_ATLAS_DIM + hu, v1 = (float)(gy + TILE_SIZE) / TILE_ATLAS_DIM - hu;
            EmitSpriteQuad(&atlas, bank, XGU_FACTOR_SRC_ALPHA, XGU_FACTOR_ONE_MINUS_SRC_ALPHA, 1.0f, screenX * sx, screenY * sy,
                           (screenX + TILE_SIZE) * sx, (screenY + TILE_SIZE) * sy, u0, v0, u1, v1);
        }
    }
}

// One Basic tile layer: a single uniform scroll over the whole clip rect, palette bank 0
// (matching the software DrawLayerBasic, which hardcodes fullPalette[0]).
bool DrawLayerBasicGPU(TileLayer *layer)
{
    if (!layer->xsize || !layer->ysize)
        return true;
    if (!tileAtlasData)
        return false;
    int32 cX1 = currentScreen->clipBound_X1, cX2 = currentScreen->clipBound_X2;
    int32 cY1 = currentScreen->clipBound_Y1, cY2 = currentScreen->clipBound_Y2;
    if (cX1 >= cX2 || cY1 >= cY2)
        return true;

    ScanlineInfo *s = &scanlines[cY1];
    int32 srcX      = cX1 + FROM_FIXED(s->position.x); // software adds clipBound_X1 to the scroll
    int32 srcY      = FROM_FIXED(s->position.y);
    float sx = (float)pb_back_buffer_width() / (float)videoSettings.pixWidth, sy = (float)pb_back_buffer_height() / (float)SCREEN_YSIZE;

    curClipX = (int32)(cX1 * sx);
    curClipY = (int32)(cY1 * sy);
    curClipW = (int32)((cX2 - cX1) * sx);
    curClipH = (int32)((cY2 - cY1) * sy);
    EmitTileGridRegion(layer, srcX, srcY, cX1, cY1, cX2, cY2, 0, sx, sy);
    curClipW = -1;
    return true;
}

// One VScroll tile layer. Columns carry per-column position (scanlines[] is indexed by X
// here); position.x increments per column (absolute source X, 1:1), while position.y is the
// per-column vertical scroll (the parallax). Group columns into constant-position.y bands and
// draw each band's full-height tile run, scissored to the band's X range. Too many bands
// (smooth per-column parallax) -> software. Single palette bank = gfxLineBuffer[0] (matching
// the software path). The column runs the full screen height from y=0 (software ignores
// clipBound_Y for VScroll).
bool DrawLayerVScrollGPU(TileLayer *layer)
{
    if (!layer->xsize || !layer->ysize)
        return true;
    if (!tileAtlasData)
        return false;
    int32 cX1 = currentScreen->clipBound_X1, cX2 = currentScreen->clipBound_X2;
    if (cX1 >= cX2)
        return true;

    int32 bands = 1;
    for (int32 x = cX1 + 1; x < cX2; ++x)
        if (scanlines[x].position.y != scanlines[x - 1].position.y)
            ++bands;
    if (bands > 48)
        return false;

    float sx = (float)pb_back_buffer_width() / (float)videoSettings.pixWidth, sy = (float)pb_back_buffer_height() / (float)SCREEN_YSIZE;
    int32 bank    = gfxLineBuffer[0];
    int32 fullH   = currentScreen->size.y;
    int32 cx      = cX1;
    while (cx < cX2) {
        int32 bandY = scanlines[cx].position.y;
        int32 cx0   = cx;
        while (cx < cX2 && scanlines[cx].position.y == bandY) ++cx;
        int32 cx1 = cx;

        int32 srcX = FROM_FIXED(scanlines[cx0].position.x); // absolute source X at screen col cx0
        int32 srcY = FROM_FIXED(bandY);

        curClipX = (int32)(cx0 * sx);
        curClipY = 0;
        curClipW = (int32)((cx1 - cx0) * sx);
        curClipH = pb_back_buffer_height();
        EmitTileGridRegion(layer, srcX, srcY, cx0, 0, cx1, fullH, bank, sx, sy);
    }
    curClipW = -1;
    return true;
}

// Compose a rotozoom layer's tilemap into the POT I8 floor texture (swizzled). Returns false
// if too big (cap) or alloc fails. Rebuilt when the (scene+layer) key changes.
bool BuildFloorTexture(TileLayer *layer)
{
    int32 fw = TILE_SIZE << layer->widthShift, fh = TILE_SIZE << layer->heightShift;
    if (fw > FLOOR_TEX_MAX || fh > FLOOR_TEX_MAX)
        return false;

    if (floorTexData && (floorW != fw || floorH != fh)) {
        MmFreeContiguousMemory(floorTexData);
        floorTexData = nullptr;
    }
    if (!floorTexData) {
        floorTexData = (uint8 *)MmAllocateContiguousMemoryEx((SIZE_T)fw * fh, 0, 0xFFFFFFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
        if (!floorTexData)
            return false;
        floorTexPhys = (uint8 *)MmGetPhysicalAddress(floorTexData);
    }
    floorW = fw;
    floorH = fh;

    uint8 *tmp = (uint8 *)malloc((size_t)fw * fh);
    if (!tmp)
        return false;
    int32 tilesX = 1 << layer->widthShift, tilesY = 1 << layer->heightShift;
    for (int32 ty = 0; ty < tilesY; ++ty) {
        for (int32 tx = 0; tx < tilesX; ++tx) {
            uint16 entry = layer->layout[tx + (ty << layer->widthShift)] & 0xFFF;
            uint8 *src   = &tilesetPixels[entry * TILE_DATASIZE];
            for (int32 row = 0; row < TILE_SIZE; ++row) memcpy(&tmp[(ty * TILE_SIZE + row) * fw + tx * TILE_SIZE], &src[row * TILE_SIZE], TILE_SIZE);
        }
    }
    swizzle_rect(tmp, fw, fh, floorTexData, fw, 1);
    free(tmp);
    return true;
}

// One rotozoom (Mode-7) floor as GPU quads: a 1px-tall strip per scanline, UV interpolated
// linearly from the scanline's affine params (posX/Y stepped by deform.x/.y per pixel). WRAP
// addressing repeats the tilemap. Reproduces DrawLayerRotozoom's per-pixel affine walk.
bool RotozoomLayerToGPU(TileLayer *layer)
{
    if (!layer->xsize || !layer->ysize)
        return true;
    if (!floorTexData || floorW <= 0)
        return false;

    float sx       = (float)pb_back_buffer_width() / (float)videoSettings.pixWidth;
    float sy       = (float)pb_back_buffer_height() / (float)SCREEN_YSIZE;
    int32 clipX1   = currentScreen->clipBound_X1, clipX2 = currentScreen->clipBound_X2;
    int32 clipY1   = currentScreen->clipBound_Y1, clipY2 = currentScreen->clipBound_Y2;
    int32 lineSize = clipX2 - clipX1;
    float dim      = PBDim();
    float invW = 1.0f / (65536.0f * floorW), invH = 1.0f / (65536.0f * floorH);

    for (int32 cy = clipY1; cy < clipY2; ++cy) {
        ScanlineInfo *sl = &scanlines[cy];
        int32 bank       = gfxLineBuffer[cy] & (PALETTE_BANK_COUNT - 1);
        float u0 = (float)sl->position.x * invW, v0 = (float)sl->position.y * invH;
        float u1 = ((float)sl->position.x + (float)lineSize * sl->deform.x) * invW;
        float v1 = ((float)sl->position.y + (float)lineSize * sl->deform.y) * invH;

        float px[4] = { clipX1 * sx, clipX2 * sx, clipX1 * sx, clipX2 * sx };
        float py[4] = { cy * sy, cy * sy, (cy + 1) * sy, (cy + 1) * sy };
        float cu[4] = { u0, u1, u0, u1 };
        float cv[4] = { v0, v1, v0, v1 };
        EmitTexQuadUV(floorTexPhys, floorW, floorH, bank | 0x100, XGU_FACTOR_SRC_ALPHA, XGU_FACTOR_ONE_MINUS_SRC_ALPHA, dim, 0xFF, px, py, cu, cv);
    }
    return true;
}

// DrawDeformedSprite (water/heat-haze): the same per-scanline affine as the Mode-7 floor, but the
// texture is a sprite surface (POT, WRAP-tiled — the software path masks with `& (w-1)`/`& (h-1)`).
// Each scanline is a full-width 1px strip; UV interpolates from position stepped by deform per
// pixel. Reproduces DrawDeformedSprite's per-pixel walk. Returns false (software fallback) for a
// non-POT surface or an ink the GPU blend map doesn't cover.
bool DrawDeformedSpriteToGPU(int32 sheetID, int32 inkEffect, int32 alpha)
{
    if (!PBSpriteOffloadOK())
        return false;
    XguBlendFactor sf, df;
    float a;
    if (!SprInkToBlend(inkEffect, alpha, &sf, &df, &a))
        return false;
    PBSurfTex *t = GetSurfaceTexture(sheetID);
    if (!t)
        return false;
    if (t->texW != t->w || t->texH != t->h) // non-POT surface -> WRAP would sample padding
        return false;

    float sx       = (float)pb_back_buffer_width() / (float)videoSettings.pixWidth;
    float sy       = (float)pb_back_buffer_height() / (float)SCREEN_YSIZE;
    int32 clipY1   = currentScreen->clipBound_Y1, clipY2 = currentScreen->clipBound_Y2;
    int32 w        = currentScreen->size.x; // strip spans the full screen width from x=0
    float dim      = PBDim();
    uint8 a8       = (uint8)(a * 255.0f);
    float invW = 1.0f / (65536.0f * t->texW), invH = 1.0f / (65536.0f * t->texH);

    for (int32 cy = clipY1; cy < clipY2; ++cy) {
        ScanlineInfo *sl = &scanlines[cy];
        int32 bank       = gfxLineBuffer[cy] & (PALETTE_BANK_COUNT - 1);
        float u0 = (float)sl->position.x * invW, v0 = (float)sl->position.y * invH;
        float u1 = ((float)sl->position.x + (float)w * sl->deform.x) * invW;
        float v1 = ((float)sl->position.y + (float)w * sl->deform.y) * invH;
        float px[4] = { 0.0f, w * sx, 0.0f, w * sx };
        float py[4] = { cy * sy, cy * sy, (cy + 1) * sy, (cy + 1) * sy };
        float cu[4] = { u0, u1, u0, u1 };
        float cv[4] = { v0, v1, v0, v1 };
        EmitTexQuadUV(t->phys, t->texW, t->texH, bank | 0x100, sf, df, dim, a8, px, py, cu, cv);
    }
    validDraw = true;
    return true;
}

// --- parallax background strips (S6.7) ---------------------------------------
// A high-parallax HScroll layer has a different scroll on (almost) every scanline — hundreds of
// constant-X bands, too many for the per-tile band path (it bailed those to software). Render
// them like the Mode-7 floor instead: compose the whole layer tilemap into one POT I8 texture
// (WRAP-addressed + swizzled, cached per scene+layer) and draw one 1px strip per scanline whose
// UV is that scanline's scroll (position.x, position.y) — exactly the per-scanline sample the
// software does, at ~one quad per scanline. Layers too large to compose fall back to software
// (the wide foreground playfield stays on the cheap band path, which handles it as one band).
#define BG_TEX_MAX   (2048)
#define BG_TEX_CACHE (4)
struct BgLayerTex {
    uint8 *data = nullptr, *phys = nullptr;
    int32 w = 0, h = 0;
    int32 key = -1;
};
BgLayerTex bgTexCache[BG_TEX_CACHE];
int32 bgTexNext = 0;

// Compose (or fetch cached) the layer's tilemap as a swizzled I8 texture. Null = too big/failed.
BgLayerTex *GetComposedLayerTex(TileLayer *layer)
{
    int32 key = ((int32)sceneInfo.activeCategory << 20) | (((int32)sceneInfo.listPos & 0xFFFF) << 4) | (int32)(layer - tileLayers);
    for (int32 i = 0; i < BG_TEX_CACHE; ++i)
        if (bgTexCache[i].data && bgTexCache[i].key == key)
            return &bgTexCache[i];

    int32 w = TILE_SIZE << layer->widthShift, h = TILE_SIZE << layer->heightShift;
    if (w > BG_TEX_MAX || h > BG_TEX_MAX)
        return nullptr; // too big to compose -> software band/scanline path

    BgLayerTex *slot = &bgTexCache[bgTexNext];
    bgTexNext        = (bgTexNext + 1) % BG_TEX_CACHE;
    if (slot->data && (slot->w != w || slot->h != h)) {
        MmFreeContiguousMemory(slot->data);
        slot->data = nullptr;
    }
    if (!slot->data) {
        slot->data = (uint8 *)MmAllocateContiguousMemoryEx((SIZE_T)w * h, 0, 0xFFFFFFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
        if (!slot->data)
            return nullptr;
        slot->phys = (uint8 *)MmGetPhysicalAddress(slot->data);
    }
    slot->w   = w;
    slot->h   = h;
    slot->key = key;

    uint8 *tmp = (uint8 *)malloc((size_t)w * h);
    if (!tmp) {
        slot->key = -1; // leave the buffer for reuse, but mark uncomposed
        return nullptr;
    }
    int32 tilesX = 1 << layer->widthShift, tilesY = 1 << layer->heightShift;
    for (int32 ty = 0; ty < tilesY; ++ty) {
        for (int32 tx = 0; tx < tilesX; ++tx) {
            uint16 entry = layer->layout[tx + (ty << layer->widthShift)];
            uint8 *dst   = &tmp[(ty * TILE_SIZE) * w + tx * TILE_SIZE];
            if (entry >= 0xFFFF) { // empty tile -> transparent (index 0), like the software skip
                for (int32 row = 0; row < TILE_SIZE; ++row) memset(&dst[row * w], 0, TILE_SIZE);
            }
            else {
                uint8 *src = &tilesetPixels[(entry & 0xFFF) * TILE_DATASIZE]; // low 12 bits pick the pre-flipped copy
                for (int32 row = 0; row < TILE_SIZE; ++row) memcpy(&dst[row * w], &src[row * TILE_SIZE], TILE_SIZE);
            }
        }
    }
    swizzle_rect(tmp, w, h, slot->data, w, 1);
    free(tmp);
    return slot;
}

// One HScroll layer as per-scanline strips (arbitrary per-scanline parallax). Each strip samples
// a single source row (v constant) starting at the scanline's horizontal scroll (u linear), WRAP
// repeating the tilemap — matching the software per-scanline walk. Returns false (software) if the
// layer can't be composed.
bool DrawLayerHScrollStripGPU(TileLayer *layer)
{
    BgLayerTex *lt = GetComposedLayerTex(layer);
    if (!lt)
        return false;
    float sx     = (float)pb_back_buffer_width() / (float)videoSettings.pixWidth;
    float sy     = (float)pb_back_buffer_height() / (float)SCREEN_YSIZE;
    int32 clipY1 = currentScreen->clipBound_Y1, clipY2 = currentScreen->clipBound_Y2;
    int32 width  = currentScreen->size.x; // HScroll draws the full screen width from x=0
    float dim    = PBDim();
    float invW = 1.0f / (65536.0f * lt->w), invH = 1.0f / (65536.0f * lt->h);
    curClipW = -1; // full-screen (strips are bounded by their own Y already)
    for (int32 cy = clipY1; cy < clipY2; ++cy) {
        ScanlineInfo *sl = &scanlines[cy];
        int32 bank       = gfxLineBuffer[cy] & (PALETTE_BANK_COUNT - 1);
        float u0 = (float)sl->position.x * invW;                          // screen x=0 -> source position.x
        float u1 = ((float)sl->position.x + (float)(width << 16)) * invW; // +width source pixels
        float v  = (float)sl->position.y * invH;                          // single source row per scanline
        float px[4] = { 0.0f, width * sx, 0.0f, width * sx };
        float py[4] = { cy * sy, cy * sy, (cy + 1) * sy, (cy + 1) * sy };
        float cu[4] = { u0, u1, u0, u1 };
        float cv[4] = { v, v, v, v };
        EmitTexQuadUV(lt->phys, lt->w, lt->h, bank | 0x100, XGU_FACTOR_SRC_ALPHA, XGU_FACTOR_ONE_MINUS_SRC_ALPHA, dim, 0xFF, px, py, cu, cv);
    }
    return true;
}

// Ensure the FMV/image RGB565 texture holds a POT container of at least w x h.
bool EnsureImageTex(int32 w, int32 h)
{
    int32 tw = (int32)npot2pot((uint32)w), th = (int32)npot2pot((uint32)h);
    if (imgTexData && (imgTexW != tw || imgTexH != th)) {
        MmFreeContiguousMemory(imgTexData);
        imgTexData = nullptr;
    }
    if (!imgTexData) {
        imgTexData = (uint8 *)MmAllocateContiguousMemoryEx((SIZE_T)tw * th * 2, 0, 0xFFFFFFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
        if (!imgTexData)
            return false;
        imgTexPhys = (uint8 *)MmGetPhysicalAddress(imgTexData);
    }
    imgTexW = tw;
    imgTexH = th;
    imgW    = w;
    imgH    = h;
    return true;
}

// YUV planes -> RGB565 into the image texture (BT.601, ported from the SDL3 device).
// Large videos (Mania.ogv is 1024x512) downsample to <=512 wide (RAM + 4x cheaper).
void PBConvertYUVToImage(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU, int32 strideV,
                         int32 chromaShiftX, int32 chromaShiftY)
{
    static uint8 clampTable[864];
    static bool clampReady = false;
    if (!clampReady) {
        for (int32 i = 0; i < 864; ++i) {
            int32 v       = i - 288;
            clampTable[i] = v < 0 ? 0 : (v > 255 ? 255 : (uint8)v);
        }
        clampReady = true;
    }
    const uint8 *clamp = &clampTable[288];

    int32 downShift = 0;
    while ((width >> downShift) > 512) downShift++;
    const int32 texWidth = width >> downShift, texHeight = height >> downShift;
    if (!EnsureImageTex(texWidth, texHeight))
        return;

    int32 pitch16 = imgTexW; // RGB565 texels per row of the POT container
    for (int32 y = 0; y < texHeight; ++y) {
        const int32 srcY  = y << downShift;
        const uint8 *yRow = yPlane + srcY * strideY;
        const uint8 *uRow = uPlane + (srcY >> chromaShiftY) * strideU;
        const uint8 *vRow = vPlane + (srcY >> chromaShiftY) * strideV;
        uint16 *dst       = (uint16 *)imgTexData + y * pitch16;
        for (int32 x = 0; x < texWidth; ++x) {
            const int32 srcX = x << downShift;
            int32 c          = ((int32)yRow[srcX] - 16) * 298;
            int32 d          = (int32)uRow[srcX >> chromaShiftX] - 128;
            int32 e          = (int32)vRow[srcX >> chromaShiftX] - 128;
            uint16 r         = clamp[(c + 409 * e + 128) >> 8];
            uint16 g         = clamp[(c - 100 * d - 208 * e + 128) >> 8];
            uint16 b         = clamp[(c + 516 * d + 128) >> 8];
            dst[x]           = (uint16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
}

// Draw the accumulated GPU 2D batches over the presented framebuffer, in draw order
// (called in FlipScreen). A batch with texPhys != NULL is an I8 paletted sprite; texPhys
// == NULL is an untextured colored poly (Scene3D faces / 2D primitives).
void FlushSpriteBatches()
{
    if (!sprBatchCount)
        return;
    int32 lastTextured = -1; // -1 = unknown, forces the first combiner set
    int32 bw = pb_back_buffer_width(), bh = pb_back_buffer_height();
    int32 lcX = -2, lcY = -2, lcW = -2, lcH = -2; // last-applied scissor (forces first set)
    for (int32 i = 0; i < sprBatchCount; ++i) {
        SprBatch *b       = &sprBatches[i];
        int32 isTextured  = b->texPhys != nullptr;

        // Per-batch scissor (tile-layer strips clip to their band; clipW<0 = full screen).
        if (b->clipX != lcX || b->clipY != lcY || b->clipW != lcW || b->clipH != lcH) {
            int32 sx = b->clipW < 0 ? 0 : b->clipX, sy = b->clipW < 0 ? 0 : b->clipY;
            int32 sw = b->clipW < 0 ? bw : b->clipW, sh = b->clipW < 0 ? bh : b->clipH;
            p = pb_begin();
            p = xgu_set_scissor_rect(p, false, sx, sy, sw, sh);
            pb_end(p);
            lcX = b->clipX; lcY = b->clipY; lcW = b->clipW; lcH = b->clipH;
        }

        p = pb_begin();
        // FillScreen fade batches use a per-channel constant blend (src*C + dst*(1-C),
        // C = the R/G/B fade alphas); push the constant before the func.
        if (b->sfactor == XGU_FACTOR_CONSTANT_COLOR) {
            pb_push1(p, NV097_SET_BLEND_COLOR, b->blendColor);
            p += 2;
        }
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
            // bank low 3 bits = palette bank; bit 8 = WRAP address (Mode-7 floor repeat).
            int32 realBank = b->bank & 7;
            bool wrap      = (b->bank & 0x100) != 0;
            // Select the palette bank: byte offset bank*256*4 into the CLUT, passed >>6.
            p = xgu_set_texture_palette(p, 0, true, XGU_PALETTE_LENGTH_256, (void *)(((uint32_t)pbClutPhys + (uint32_t)realBank * 256u * 4u) >> 6));
            p = xgu_set_texture_filter(p, 0, 0, XGU_TEXTURE_CONVOLUTION_GAUSSIAN, XGU_TEXTURE_FILTER_NEAREST, XGU_TEXTURE_FILTER_NEAREST, false, false,
                                       false, false);
            XguTextureAddress addr = wrap ? XGU_WRAP : XGU_CLAMP_TO_EDGE;
            p = xgu_set_texture_address(p, 0, addr, wrap, addr, wrap, XGU_CLAMP_TO_EDGE, false, false);
        }
        pb_end(p);

        xgux_set_attrib_pointer(XGU_VERTEX_ARRAY, XGU_FLOAT, 2, sizeof(PBVertex), sprVerts[b->start].pos);
        xgux_set_attrib_pointer(XGU_COLOR_ARRAY, XGU_UNSIGNED_BYTE_OGL, 4, sizeof(PBVertex), sprVerts[b->start].color);
        if (isTextured)
            xgux_set_attrib_pointer(XGU_TEXCOORD0_ARRAY, XGU_FLOAT, 2, sizeof(PBVertex), sprVerts[b->start].tex);
        xgux_draw_arrays(XGU_TRIANGLES, 0, b->count);
    }
    // Restore full-screen scissor + the texture combiner for the next framebuffer present.
    p = pb_begin();
    p = xgu_set_scissor_rect(p, false, 0, 0, bw, bh);
    if (lastTextured == 0)
        texture_combiner_apply();
    pb_end(p);
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
        if (tileAtlasData) {
            MmFreeContiguousMemory(tileAtlasData);
            tileAtlasData     = nullptr;
            tileAtlasPhys     = nullptr;
            tileAtlasSceneKey = -1;
        }
        if (floorTexData) {
            MmFreeContiguousMemory(floorTexData);
            floorTexData = nullptr;
            floorTexPhys = nullptr;
            floorW = floorH = 0;
            floorKey        = -1;
        }
        for (int32 i = 0; i < BG_TEX_CACHE; ++i) {
            if (bgTexCache[i].data) {
                MmFreeContiguousMemory(bgTexCache[i].data);
                bgTexCache[i].data = nullptr;
                bgTexCache[i].phys = nullptr;
                bgTexCache[i].key  = -1;
            }
        }
        if (imgTexData) {
            MmFreeContiguousMemory(imgTexData);
            imgTexData = nullptr;
            imgTexPhys = nullptr;
            imgTexW = imgTexH = imgW = imgH = 0;
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

    // Image/FMV mode: when screenCount == 0 the game plays a video/image — present the
    // RGB565 image texture fullscreen instead of the framebuffer (and skip GPU 2D content).
    const bool imageMode  = (videoSettings.screenCount == 0 && imgTexData);
    uint8 *ptxPhys        = imageMode ? imgTexPhys : fbTex.phys;
    const int32 ptxW      = imageMode ? imgTexW : PB_FB_TEX_W;
    const int32 ptxH      = imageMode ? imgTexH : PB_FB_TEX_H;
    const int32 ptxPitch  = imageMode ? (imgTexW * 2) : fbTex.pitch;

    // UVs in TEXELS: the NV2A samples LINEAR textures (this RGB565 present texture) with
    // texel coordinates, not normalized [0,1] (swizzled textures use normalized — that's
    // why the I8 sprite path uses 0..1). The frame occupies the top-left of the container.
    const float u1 = imageMode ? (float)imgW : (float)screens[0].size.x;
    const float v1 = imageMode ? (float)imgH : (float)screens[0].size.y;

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
    p = xgu_set_texture_offset(p, 0, ptxPhys);
    p = xgu_set_texture_format(p, 0, 2, false, XGU_SOURCE_COLOR, 2, XGU_TEXTURE_FORMAT_R5G6B5, 1, __builtin_ctz(ptxW), __builtin_ctz(ptxH), 0);
    p = xgu_set_texture_control0(p, 0, true, 0, 0);
    p = xgu_set_texture_control1(p, 0, ptxPitch);
    p = xgu_set_texture_image_rect(p, 0, ptxW, ptxH);
    // Video scales up — use LINEAR for the image texture, NEAREST for the pixel-art fb.
    XguTexFilter pf = imageMode ? XGU_TEXTURE_FILTER_LINEAR : XGU_TEXTURE_FILTER_NEAREST;
    p = xgu_set_texture_filter(p, 0, 0, XGU_TEXTURE_CONVOLUTION_GAUSSIAN, pf, pf, false, false, false, false);
    p = xgu_set_texture_address(p, 0, XGU_CLAMP_TO_EDGE, false, XGU_CLAMP_TO_EDGE, false, XGU_CLAMP_TO_EDGE, false, false);
    pb_end(p);

    xgux_set_attrib_pointer(XGU_VERTEX_ARRAY, XGU_FLOAT, 2, sizeof(PBVertex), presentVerts->pos);
    xgux_set_attrib_pointer(XGU_COLOR_ARRAY, XGU_UNSIGNED_BYTE_OGL, 4, sizeof(PBVertex), presentVerts->color);
    xgux_set_attrib_pointer(XGU_TEXCOORD0_ARRAY, XGU_FLOAT, 2, sizeof(PBVertex), presentVerts->tex);
    xgux_draw_arrays(XGU_TRIANGLES, 0, 6);

    // Stage 2: sync the 8-bank CLUT from the (possibly cycled) palette, then draw the
    // accumulated GPU sprite quads over the framebuffer background, in draw order. (Video/
    // image mode has no GPU 2D content — just present the frame.)
    if (!imageMode) {
        PBSyncCLUT();
        FlushSpriteBatches();
    }

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

// FMV / image on pbkit (Stage 6): convert the frame into the RGB565 image texture; it's
// presented as a fullscreen quad in FlipScreen when screenCount == 0.
void RenderDevice::SetupImageTexture(int32 width, int32 height, uint8 *imagePixels)
{
    if (!EnsureImageTex(width, height) || !imagePixels)
        return;
    uint32 *src = (uint32 *)imagePixels; // ARGB8888
    for (int32 y = 0; y < height; ++y) {
        uint16 *dst = (uint16 *)imgTexData + y * imgTexW;
        for (int32 x = 0; x < width; ++x) {
            uint32 c = src[y * width + x];
            uint32 r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
            dst[x]   = (uint16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
}
void RenderDevice::SetupVideoTexture_YUV420(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 sy, int32 su, int32 sv)
{
    PBConvertYUVToImage(width, height, yPlane, uPlane, vPlane, sy, su, sv, 1, 1);
}
void RenderDevice::SetupVideoTexture_YUV422(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 sy, int32 su, int32 sv)
{
    PBConvertYUVToImage(width, height, yPlane, uPlane, vPlane, sy, su, sv, 1, 0);
}
void RenderDevice::SetupVideoTexture_YUV444(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 sy, int32 su, int32 sv)
{
    PBConvertYUVToImage(width, height, yPlane, uPlane, vPlane, sy, su, sv, 0, 0);
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

// --- 2D primitives as untextured colored polys (Stage 3) ---------------------
// DrawRectangle: x,y,width,height are already clipped screen pixels; color is 0xRRGGBB.
bool RenderDevice::DrawRectangleGPU(int32 x, int32 y, int32 width, int32 height, uint32 color, int32 alpha, int32 inkEffect)
{
    if (!PBSpriteOffloadOK() || width <= 0 || height <= 0)
        return false;
    XguBlendFactor sf, df;
    float a;
    if (!SprInkToBlend(inkEffect, alpha, &sf, &df, &a))
        return false;
    // 16.16 quad corners TL,TR,BR,BL (EmitColoredPoly fans 0-1-2 / 0-2-3).
    Vector2 v[4]   = { { x << 16, y << 16 }, { (x + width) << 16, y << 16 }, { (x + width) << 16, (y + height) << 16 }, { x << 16, (y + height) << 16 } };
    uint32 rgb     = color & 0xFFFFFF;
    uint32 cols[4] = { rgb, rgb, rgb, rgb };
    if (EmitColoredPoly(v, cols, 4, (uint8)(a * 255.0f), sf, df)) {
        validDraw = true;
        return true;
    }
    return false;
}

// FillScreen: a fullscreen per-channel alpha fade of `color` over everything drawn so far
// — Mania's zone transition fade (Zone.c: RSDK.FillScreen(fadeColor, timer, timer-128,
// timer-256)). The software version blends the whole framebuffer; on the GPU the fill was
// landing in the (background) framebuffer *under* the GPU sprite/tile stream, so the fade
// never appeared over gameplay. Emit it as one fullscreen quad in the batch stream (so it
// composites in draw order, over prior GPU content) with a per-channel CONSTANT_COLOR blend:
// result_c = color_c*(a_c/255) + dst_c*(1 - a_c/255) — exactly the software LERP, the three
// alphas pushed via NV097_SET_BLEND_COLOR at flush.
bool RenderDevice::DrawFillScreenGPU(uint32 color, int32 alphaR, int32 alphaG, int32 alphaB)
{
    if (!PBSpriteOffloadOK())
        return false;
    alphaR = alphaR < 0 ? 0 : (alphaR > 0xFF ? 0xFF : alphaR);
    alphaG = alphaG < 0 ? 0 : (alphaG > 0xFF ? 0xFF : alphaG);
    alphaB = alphaB < 0 ? 0 : (alphaB > 0xFF ? 0xFF : alphaB);
    if (!(alphaR + alphaG + alphaB))
        return true; // nothing to blend, but handled (don't also run the software fill)
    if (!sprVerts || sprVertCount + 6 > MAX_SPR_VERTS || sprBatchCount >= MAX_SPR_BATCHES)
        return false;

    // Fullscreen quad in back-buffer pixels — bypasses the current clip, like software
    // FillScreen which writes the entire framebuffer. dim modulates the fade color so the
    // screensaver dim applies consistently with the rest of the GPU frame.
    const float bw = (float)pb_back_buffer_width(), bh = (float)pb_back_buffer_height();
    const float dim = PBDim();
    const uint8 cr = (uint8)(((color >> 16) & 0xFF) * dim);
    const uint8 cg = (uint8)(((color >> 8) & 0xFF) * dim);
    const uint8 cb = (uint8)((color & 0xFF) * dim);
    const float px[4]    = { 0.0f, bw, 0.0f, bw };
    const float py[4]    = { 0.0f, 0.0f, bh, bh };
    const int32 order[6] = { 0, 1, 2, 1, 3, 2 };
    int32 start          = sprVertCount;
    for (int32 i = 0; i < 6; ++i) {
        int32 c     = order[i];
        PBVertex *v = &sprVerts[sprVertCount++];
        v->pos[0]   = px[c];
        v->pos[1]   = py[c];
        v->color[0] = cr;
        v->color[1] = cg;
        v->color[2] = cb;
        v->color[3] = 0xFF;
        v->tex[0]   = 0.0f;
        v->tex[1]   = 0.0f;
    }
    // Untextured, full-screen clip (clipW<0), per-channel constant blend. Always its own
    // batch (never coalesced — the blend constant is unique to this fill).
    uint32 blendColor = 0xFF000000u | ((uint32)alphaR << 16) | ((uint32)alphaG << 8) | (uint32)alphaB;
    sprBatches[sprBatchCount++] = { start,        6,  nullptr, 0, 0, 0, XGU_FACTOR_CONSTANT_COLOR, XGU_FACTOR_ONE_MINUS_CONSTANT_COLOR,
                                    0,            0,  -1,      -1, blendColor };
    validDraw                   = true;
    return true;
}

// DrawLine: a 1px-logical line as a thin quad between the (already screen-space) endpoints.
bool RenderDevice::DrawLineGPU(int32 x1, int32 y1, int32 x2, int32 y2, uint32 color, int32 alpha, int32 inkEffect)
{
    if (!PBSpriteOffloadOK())
        return false;
    XguBlendFactor sf, df;
    float a;
    if (!SprInkToBlend(inkEffect, alpha, &sf, &df, &a))
        return false;
    float dx = (float)(x2 - x1), dy = (float)(y2 - y1);
    float len = __builtin_sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) { // degenerate (a point) — a 1px dot
        dx  = 1.0f;
        dy  = 0.0f;
        len = 1.0f;
    }
    float nx = -dy / len * 0.5f, ny = dx / len * 0.5f; // 0.5px perpendicular half-width
    Vector2 v[4]   = { { (int32)((x1 + nx) * 65536.0f), (int32)((y1 + ny) * 65536.0f) },
                       { (int32)((x2 + nx) * 65536.0f), (int32)((y2 + ny) * 65536.0f) },
                       { (int32)((x2 - nx) * 65536.0f), (int32)((y2 - ny) * 65536.0f) },
                       { (int32)((x1 - nx) * 65536.0f), (int32)((y1 - ny) * 65536.0f) } };
    uint32 rgb     = color & 0xFFFFFF;
    uint32 cols[4] = { rgb, rgb, rgb, rgb };
    if (EmitColoredPoly(v, cols, 4, (uint8)(a * 255.0f), sf, df)) {
        validDraw = true;
        return true;
    }
    return false;
}

// DrawCircle: filled circle as a triangle fan (center + perimeter). Perimeter points from the
// engine's Sin256/Cos256 tables (scale >>8); segment count scales with radius.
bool RenderDevice::DrawCircleGPU(int32 x, int32 y, int32 radius, uint32 color, int32 alpha, int32 inkEffect)
{
    if (!PBSpriteOffloadOK() || radius <= 0)
        return false;
    XguBlendFactor sf, df;
    float a;
    if (!SprInkToBlend(inkEffect, alpha, &sf, &df, &a))
        return false;
    int32 N = radius >> 1;
    N       = N < 12 ? 12 : (N > 64 ? 64 : N);
    Vector2 v[66];
    uint32 cols[66];
    uint32 rgb = color & 0xFFFFFF;
    v[0].x     = x << 16;
    v[0].y     = y << 16;
    cols[0]    = rgb;
    for (int32 i = 0; i <= N; ++i) {
        int32 ang  = (i * 256) / N;
        v[i + 1].x = (x + (Cos256(ang) * radius >> 8)) << 16;
        v[i + 1].y = (y + (Sin256(ang) * radius >> 8)) << 16;
        cols[i + 1] = rgb;
    }
    if (EmitColoredPoly(v, cols, N + 2, (uint8)(a * 255.0f), sf, df)) {
        validDraw = true;
        return true;
    }
    return false;
}

// DrawCircleOutline: an annulus (ring) as a strip of quads between inner and outer radius.
bool RenderDevice::DrawCircleOutlineGPU(int32 x, int32 y, int32 innerRadius, int32 outerRadius, uint32 color, int32 alpha, int32 inkEffect)
{
    if (!PBSpriteOffloadOK() || outerRadius <= 0 || innerRadius >= outerRadius)
        return false;
    XguBlendFactor sf, df;
    float a;
    if (!SprInkToBlend(inkEffect, alpha, &sf, &df, &a))
        return false;
    if (innerRadius < 0)
        innerRadius = 0;
    int32 N = outerRadius >> 1;
    N       = N < 12 ? 12 : (N > 64 ? 64 : N);
    uint32 rgb = color & 0xFFFFFF;
    uint8 a8   = (uint8)(a * 255.0f);
    int32 pox = x + (Cos256(0) * outerRadius >> 8), poy = y + (Sin256(0) * outerRadius >> 8);
    int32 pix = x + (Cos256(0) * innerRadius >> 8), piy = y + (Sin256(0) * innerRadius >> 8);
    bool any = false;
    for (int32 i = 1; i <= N; ++i) {
        int32 ang = (i * 256) / N;
        int32 nox = x + (Cos256(ang) * outerRadius >> 8), noy = y + (Sin256(ang) * outerRadius >> 8);
        int32 nix = x + (Cos256(ang) * innerRadius >> 8), niy = y + (Sin256(ang) * innerRadius >> 8);
        Vector2 v[4]   = { { pox << 16, poy << 16 }, { nox << 16, noy << 16 }, { nix << 16, niy << 16 }, { pix << 16, piy << 16 } };
        uint32 cols[4] = { rgb, rgb, rgb, rgb };
        if (EmitColoredPoly(v, cols, 4, a8, sf, df))
            any = true;
        pox = nox;
        poy = noy;
        pix = nix;
        piy = niy;
    }
    if (any) {
        validDraw = true;
        return true;
    }
    return false;
}

bool RenderDevice::DrawFaceGPU(Vector2 *vertices, int32 vertCount, int32 r, int32 g, int32 b, int32 alpha, int32 inkEffect)
{
    if (!PBSpriteOffloadOK() || vertCount < 3)
        return false;
    if (vertCount > 4)
        vertCount = 4;
    XguBlendFactor sf, df;
    float a;
    if (!SprInkToBlend(inkEffect, alpha, &sf, &df, &a))
        return false;
    uint32 rgb     = ((uint32)(r & 0xFF) << 16) | ((uint32)(g & 0xFF) << 8) | (uint32)(b & 0xFF);
    uint32 cols[4] = { rgb, rgb, rgb, rgb };
    if (EmitColoredPoly(vertices, cols, vertCount, (uint8)(a * 255.0f), sf, df)) {
        validDraw = true;
        return true;
    }
    return false;
}

bool RenderDevice::DrawBlendedFaceGPU(Vector2 *vertices, uint32 *colors, int32 vertCount, int32 alpha, int32 inkEffect)
{
    if (!PBSpriteOffloadOK() || vertCount < 3)
        return false;
    if (vertCount > 4)
        vertCount = 4;
    XguBlendFactor sf, df;
    float a;
    if (!SprInkToBlend(inkEffect, alpha, &sf, &df, &a))
        return false;
    if (EmitColoredPoly(vertices, colors, vertCount, (uint8)(a * 255.0f), sf, df)) {
        validDraw = true;
        return true;
    }
    return false;
}

void RenderDevice::UpdateAniTileGPU(int32 tileIndex, int32 cnt) { UpdateAniTileAtlas(tileIndex, cnt); }

bool RenderDevice::DrawDeformedSpriteGPU(int32 sheetID, int32 inkEffect, int32 alpha) { return DrawDeformedSpriteToGPU(sheetID, inkEffect, alpha); }

bool RenderDevice::DrawLayerGPU(RSDK::TileLayer *layer)
{
#ifdef PBKIT_NO_GPU_SPRITES
    return false;
#endif
    if (!PBSpriteOffloadOK())
        return false;
    // A custom scanline callback (rotozoom/Mode-7 effects) fills scanlines[] with arbitrary
    // non-linear per-scanline positions our banded/linear tile path can't represent — software.
    //
    // The special stage's *background* tile layers additionally garble on the GPU atlas path
    // (only the tile layers — sprites are correct — pointing at a stale/mismatched tileset
    // atlas for that scene, not the band logic). Keep the whole special stage on software until
    // the atlas issue is debugged during the framebuffer-retire step (S6.7).
    if (layer->scanlineCallback || PBInSpecialStage())
        return false;
    // Rebuild the tileset atlas when the scene changes (cheap key check per call).
    int32 key = ((int32)sceneInfo.activeCategory << 16) | ((int32)sceneInfo.listPos & 0xFFFF);
    if (key != tileAtlasSceneKey) {
        BuildTilesetAtlas();
        tileAtlasSceneKey = key;
    }
    switch (layer->type) {
        case LAYER_HSCROLL: return DrawLayerHScrollGPU(layer);
        case LAYER_VSCROLL: return DrawLayerVScrollGPU(layer);
        case LAYER_BASIC: return DrawLayerBasicGPU(layer);
        default: return false; // Rotozoom -> its own DrawLayerRotozoomGPU path
    }
}

bool RenderDevice::DrawLayerRotozoomGPU(TileLayer *layer)
{
#ifdef PBKIT_NO_GPU_SPRITES
    return false;
#endif
    // NOTE: unlike the HScroll tile path, this DOES run for the special stage (its Mode-7
    // floor is the whole point) — the special stage is ENGINESTATE_REGULAR.
    if (!PBSpriteOffloadOK())
        return false;
    // Compose the floor texture when the (scene + layer slot) changes; bail if too big.
    int32 key = ((int32)sceneInfo.activeCategory << 20) | (((int32)sceneInfo.listPos & 0xFFFF) << 4) | (int32)(layer - tileLayers);
    if (key != floorKey) {
        floorKey = key;
        if (!BuildFloorTexture(layer)) {
            floorW = 0; // mark unavailable -> software for this layer
            return false;
        }
    }
    return RotozoomLayerToGPU(layer);
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
