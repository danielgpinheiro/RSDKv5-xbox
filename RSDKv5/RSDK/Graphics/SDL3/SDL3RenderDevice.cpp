
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

// ============================================================================
// 3D GPU offload (special stages)
//
// Draw3DScene appends its projected faces here as GPU triangles instead of
// software-filling the RGB565 framebuffer. At present time we composite three
// layers in the correct order: the framebuffer as it stood at the first 3D face
// (background 2D) -> the 3D triangles (nxdk_xgu QueueGeometry) -> the 2D drawn
// after the 3D (foreground: billboards/HUD), keyed transparent by diffing the
// final framebuffer against that background snapshot. Occlusion among the 3D
// polygons is identical to the CPU path (same face sort, replayed on the GPU);
// only 3D-vs-2D-billboard depth is approximate. See the plan for the tradeoffs.
// ============================================================================
bool RenderDevice::gpu3DEnabled = true;

namespace {
struct Batch3D {
    int32 start;
    int32 count;
    SDL_BlendMode blend;
    SDL_Texture *tex; // NULL = untextured 3D face; else a baked sprite-frame texture
};

// Fixed, boot-allocated staging for the special-stage GPU layer. A std::vector
// here reallocated as geometry grew around a curve and hit bad_alloc on the tight
// 64MB heap (the G1-only crash); a fixed buffer allocated once at boot — when RAM
// is plentiful — removes that. Overflow drops faces rather than growing/crashing.
#define MAX_3D_VERTS   (24576)
#define MAX_3D_BATCHES (1024)
SDL_Vertex *scene3DVerts = NULL; // RenderDevice::Init allocates MAX_3D_VERTS
int32 scene3DVertCount   = 0;
Batch3D scene3DBatches[MAX_3D_BATCHES];
int32 scene3DBatchCount  = 0;
bool has3DThisFrame        = false;
// True once a real 3D FACE (Draw3DScene solid geometry) has been emitted this frame —
// distinct from has3DThisFrame (any GPU element, incl. sprites). Used to tell genuine
// 3D gameplay from the special-stage 2D UI screens (results/SpecialClear, which draw
// only sprites): the approximate bg->GPU->fg composite scrambles interleaved CPU
// rectangles vs GPU sprites on those screens, so sprites there stay on the CPU path.
bool had3DFacesThisFrame   = false;
bool prevFrameHad3DFaces   = false; // had3DFacesThisFrame from the previous presented frame
uint16 *bg3DBuffer         = NULL; // framebuffer snapshot at the first 3D face (screen 0)
int32 bg3DPitch            = 0;
int32 bg3DHeight           = 0;
SDL_Texture *fg3DTexture   = NULL; // ARGB1555: post-3D 2D, alpha-keyed against bg3DBuffer

inline SDL_BlendMode Ink3DToBlend(int32 inkEffect)
{
    switch (inkEffect) {
        case INK_ALPHA:
        case INK_BLEND: return SDL_BLENDMODE_BLEND;
        case INK_ADD: return SDL_BLENDMODE_ADD;
        default: return SDL_BLENDMODE_NONE; // NONE/SUB/TINT/MASKED -> opaque
    }
}

inline uint32 RGB565to888(uint16 c)
{
    uint32 r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
    uint32 r8 = (r5 << 3) | (r5 >> 2), g8 = (g6 << 2) | (g6 >> 4), b8 = (b5 << 3) | (b5 >> 2);
    return (r8 << 16) | (g8 << 8) | b8;
}

// Snapshot the current framebuffer as the 3D background (called on the first 3D
// face of the frame — everything drawn before the 3D is "behind" it).
void Snapshot3DBackground()
{
    int32 pitch = screens[0].pitch;
    int32 h     = screens[0].size.y;
    if (!bg3DBuffer || bg3DPitch != pitch || bg3DHeight != h) {
        free(bg3DBuffer);
        bg3DBuffer  = (uint16 *)malloc((size_t)pitch * h * sizeof(uint16));
        bg3DPitch   = pitch;
        bg3DHeight  = h;
    }
    if (bg3DBuffer)
        memcpy(bg3DBuffer, screens[0].frameBuffer, (size_t)pitch * h * sizeof(uint16));
}

inline void Add3DVertex(float x, float y, uint32 rgb, float a) // untextured (3D face)
{
    SDL_Vertex *v  = &scene3DVerts[scene3DVertCount++];
    v->position.x  = x;
    v->position.y  = y;
    v->color.r     = ((rgb >> 16) & 0xFF) / 255.0f;
    v->color.g     = ((rgb >> 8) & 0xFF) / 255.0f;
    v->color.b     = (rgb & 0xFF) / 255.0f;
    v->color.a     = a;
    v->tex_coord.x = 0.0f;
    v->tex_coord.y = 0.0f;
}

inline void Add3DVertexUV(float x, float y, float u, float v, float a) // textured sprite (white * a)
{
    SDL_Vertex *vtx  = &scene3DVerts[scene3DVertCount++];
    vtx->position.x  = x;
    vtx->position.y  = y;
    vtx->color.r     = 1.0f;
    vtx->color.g     = 1.0f;
    vtx->color.b     = 1.0f;
    vtx->color.a     = a;
    vtx->tex_coord.x = u;
    vtx->tex_coord.y = v;
}

// Begin the special-stage GPU layer for this frame: snapshot the framebuffer as the
// background (everything drawn before the first GPU element is "behind" it) and
// reset the staging buffers. Triggered by the first 3D face OR sprite quad.
inline void BeginGPUFrame()
{
    if (!has3DThisFrame) {
        has3DThisFrame    = true;
        scene3DVertCount  = 0;
        scene3DBatchCount = 0;
        Snapshot3DBackground();
    }
}

// Fan-triangulate a face (verts already in 16.16 framebuffer/logical pixels) into
// the fixed staging buffer, coalescing consecutive faces that share a blend mode.
void Add3DFaceInternal(Vector2 *vertices, uint32 *colors, int32 vertCount, float a, SDL_BlendMode blend)
{
    if (vertCount < 3 || !scene3DVerts)
        return; // points/lines aren't filled; no staging buffer -> CPU path

    BeginGPUFrame();

    int32 needed = (vertCount - 2) * 3;
    if (scene3DVertCount + needed > MAX_3D_VERTS)
        return; // out of staging room this frame — drop the face (no realloc, no crash)

    // Can this face extend the previous batch (same blend, untextured, contiguous)?
    bool coalesce = scene3DBatchCount > 0 && scene3DBatches[scene3DBatchCount - 1].blend == blend
                    && scene3DBatches[scene3DBatchCount - 1].tex == NULL
                    && scene3DBatches[scene3DBatchCount - 1].start + scene3DBatches[scene3DBatchCount - 1].count == scene3DVertCount;
    if (!coalesce && scene3DBatchCount >= MAX_3D_BATCHES)
        return; // no room for a new batch

    int32 start = scene3DVertCount;
    for (int32 t = 1; t + 1 < vertCount; ++t) {
        Add3DVertex(vertices[0].x / 65536.0f, vertices[0].y / 65536.0f, colors[0], a);
        Add3DVertex(vertices[t].x / 65536.0f, vertices[t].y / 65536.0f, colors[t], a);
        Add3DVertex(vertices[t + 1].x / 65536.0f, vertices[t + 1].y / 65536.0f, colors[t + 1], a);
    }
    int32 count = scene3DVertCount - start;

    if (coalesce)
        scene3DBatches[scene3DBatchCount - 1].count += count;
    else
        scene3DBatches[scene3DBatchCount++] = { start, count, blend, NULL };

    had3DFacesThisFrame = true; // genuine 3D geometry this frame (not just sprites)
}

// --- baked sprite-frame texture cache (special-stage billboards) -------------
// Each scaled special-stage sprite frame is baked once into a small ARGB8888
// texture (palette applied, index 0 transparent) with a 2px transparent border so
// the reused DrawSpriteRotozoom posX/posY corners (which carry a +/-2px margin)
// map onto UV [0,1] exactly. Drawn as textured GPU quads instead of the software
// rotozoom fill — that fill is the special stage's ~40ms bottleneck.
// All baked frames live in ONE atlas texture so the (interleaved) sprite quads
// coalesce into ~1 draw batch instead of one texture-bind per frame — the per-batch
// binds were the special stage's added present cost.
#define MAX_BAKED_SPRITES (256)
#define SPR_BORDER        (2)
#define ATLAS_W           (512)
#define ATLAS_H           (256)
#define SPR_MAX_DIM       (160) // max padded frame dimension we can bake
struct BakedSprite {
    int32 sheetID, sprX, sprY, w, h;
    float u0, v0, u1, v1; // normalized sub-rect in the atlas
};
BakedSprite bakedSprites[MAX_BAKED_SPRITES];
int32 bakedSpriteCount       = 0;
uint32 bakedPaletteSum       = 0;     // re-bake when the palette changes (fades/cycles)
bool paletteCheckedThisFrame = false; // the checksum runs ONCE per frame, not per sprite
SDL_Texture *spriteAtlas     = NULL;
int32 atlasPackX = 0, atlasPackY = 0, atlasRowH = 0; // shelf packer

void ClearBakedSprites() // reset the packer; the atlas texture is reused
{
    bakedSpriteCount = 0;
    atlasPackX       = 0;
    atlasPackY       = 0;
    atlasRowH        = 0;
}

uint32 PaletteChecksum()
{
    uint32 s        = 0;
    const uint16 *p = &fullPalette[0][0];
    for (int32 i = 0; i < PALETTE_BANK_COUNT * PALETTE_BANK_SIZE; ++i) s += (uint32)(i + 1) * p[i];
    return s;
}

// Bake one frame into the shared atlas (palette applied, 2px transparent border).
// Returns the new BakedSprite index, or -1 (too big / atlas full).
int32 BakeSpriteIntoAtlas(int32 sheetID, int32 sprX, int32 sprY, int32 w, int32 h, uint8 bank)
{
    GFXSurface *surface = &gfxSurface[sheetID];
    if (!surface->pixels || w <= 0 || h <= 0)
        return -1;
    int32 tw = w + 2 * SPR_BORDER, th = h + 2 * SPR_BORDER;
    if (tw > SPR_MAX_DIM || th > SPR_MAX_DIM || tw > ATLAS_W || bakedSpriteCount >= MAX_BAKED_SPRITES)
        return -1;

    if (!spriteAtlas) {
        // STREAMING (not STATIC): STATIC textures are swizzled, and sub-rect updates
        // to a swizzled texture didn't populate — sprites came out invisible. A
        // streaming texture is linear; the xgu LockTexture returns a direct pointer
        // into its persistent buffer, so sub-rect writes accumulate correctly.
        spriteAtlas = SDL_CreateTexture(RenderDevice::renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ATLAS_W, ATLAS_H);
        if (!spriteAtlas)
            return -1;
        SDL_SetTextureScaleMode(spriteAtlas, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(spriteAtlas, SDL_BLENDMODE_BLEND);
    }

    if (atlasPackX + tw > ATLAS_W) { // next shelf
        atlasPackX = 0;
        atlasPackY += atlasRowH;
        atlasRowH = 0;
    }
    if (atlasPackY + th > ATLAS_H)
        return -1; // atlas full -> CPU fallback
    int32 px = atlasPackX, py = atlasPackY;
    atlasPackX += tw;
    if (th > atlasRowH)
        atlasRowH = th;

    SDL_Rect region = { px, py, tw, th };
    void *pix   = NULL;
    int32 pitch = 0;
    if (!SDL_LockTexture(spriteAtlas, &region, &pix, &pitch))
        return -1;
    uint16 *palette = fullPalette[bank & (PALETTE_BANK_COUNT - 1)];
    uint8 *pixels   = surface->pixels;
    int32 lineShift = surface->lineSize; // row stride = 1 << lineSize
    uint32 *dst     = (uint32 *)pix;
    int32 dstStride = pitch / (int32)sizeof(uint32);
    for (int32 ty = 0; ty < th; ++ty) {
        for (int32 tx = 0; tx < tw; ++tx) {
            uint32 argb = 0; // transparent: the border and any index-0 pixels
            int32 sx = tx - SPR_BORDER, sy = ty - SPR_BORDER;
            if (sx >= 0 && sx < w && sy >= 0 && sy < h) {
                uint8 index = pixels[((sprY + sy) << lineShift) + (sprX + sx)];
                if (index)
                    argb = 0xFF000000u | RGB565to888(palette[index]);
            }
            dst[tx] = argb;
        }
        dst += dstStride;
    }
    SDL_UnlockTexture(spriteAtlas);

    BakedSprite *b = &bakedSprites[bakedSpriteCount];
    b->sheetID = sheetID;
    b->sprX    = sprX;
    b->sprY    = sprY;
    b->w       = w;
    b->h       = h;
    b->u0      = (float)px / ATLAS_W;
    b->v0      = (float)py / ATLAS_H;
    b->u1      = (float)(px + tw) / ATLAS_W;
    b->v1      = (float)(py + th) / ATLAS_H;
    return bakedSpriteCount++;
}

BakedSprite *GetBakedSprite(int32 sheetID, int32 sprX, int32 sprY, int32 w, int32 h, uint8 bank)
{
    // Full-palette checksum ONCE per frame (on the first baked sprite) — reset in
    // FlipScreen. Safe to clear here: no batch this frame uses the atlas regions yet.
    if (!paletteCheckedThisFrame) {
        paletteCheckedThisFrame = true;
        uint32 sum              = PaletteChecksum();
        if (sum != bakedPaletteSum) {
            ClearBakedSprites();
            bakedPaletteSum = sum;
        }
    }
    for (int32 i = 0; i < bakedSpriteCount; ++i) {
        BakedSprite *b = &bakedSprites[i];
        if (b->sheetID == sheetID && b->sprX == sprX && b->sprY == sprY && b->w == w && b->h == h)
            return b;
    }
    int32 idx = BakeSpriteIntoAtlas(sheetID, sprX, sprY, w, h, bank);
    return idx >= 0 ? &bakedSprites[idx] : NULL;
}

// Is the current scene the UFO special stage? Matched purely on the scene *category*
// "Special Stage" (set via SetScene("Special Stage", ...)). We deliberately do NOT
// fall back to the scene folder: the Blue Spheres bonus stage lives in the same
// "Special" data folder but is a light 2D checkerboard that runs fine on the CPU —
// offloading it only adds GPU-composite present cost. Its category is "Blue Spheres"
// (no "Special" substring), so the category check correctly excludes it. Cached per
// frame (reset in FlipScreen) so the string search runs once, not per sprite.
int8 inSpecialCache = -1;
bool InSpecialStage()
{
    if (inSpecialCache < 0) {
        inSpecialCache = 0;
        if (sceneInfo.listCategory && strstr(sceneInfo.listCategory[sceneInfo.activeCategory].name, "Special"))
            inSpecialCache = 1;
    }
    return inSpecialCache == 1;
}

// Map an ink effect to a GPU blend + vertex alpha; false = unsupported (CPU path).
inline bool InkToSpriteBlend(int32 inkEffect, int32 alpha, SDL_BlendMode *blend, float *a)
{
    switch (inkEffect) {
        case INK_NONE: *blend = SDL_BLENDMODE_NONE; *a = 1.0f; return true;
        case INK_BLEND: *blend = SDL_BLENDMODE_BLEND; *a = 0.5f; return true;
        case INK_ALPHA: *blend = SDL_BLENDMODE_BLEND; *a = (alpha > 0xFF ? 0xFF : (alpha < 0 ? 0 : alpha)) / 255.0f; return true;
        case INK_ADD: *blend = SDL_BLENDMODE_ADD; *a = (alpha > 0xFF ? 0xFF : (alpha < 0 ? 0 : alpha)) / 255.0f; return true;
        default: return false;
    }
}

// Emit one atlas-textured quad (corners 0=TL,1=TR,2=BL,3=BR ; UVs u0,v0..u1,v1)
// into the shared sprite batch. Returns true if emitted (or intentionally dropped
// on a full staging buffer — caller must not also CPU-draw), false if unavailable.
bool EmitAtlasQuad(float px0, float py0, float px1, float py1, float px2, float py2, float px3, float py3, float u0, float v0, float u1, float v1,
                   float a, SDL_BlendMode blend)
{
    if (!scene3DVerts)
        return false;
    BeginGPUFrame();
    if (scene3DVertCount + 6 > MAX_3D_VERTS)
        return false; // staging full -> CPU draws it (visible, minor order glitch)

    bool coalesce = scene3DBatchCount > 0 && scene3DBatches[scene3DBatchCount - 1].blend == blend
                    && scene3DBatches[scene3DBatchCount - 1].tex == spriteAtlas
                    && scene3DBatches[scene3DBatchCount - 1].start + scene3DBatches[scene3DBatchCount - 1].count == scene3DVertCount;
    if (!coalesce && scene3DBatchCount >= MAX_3D_BATCHES)
        return false;

    int32 start = scene3DVertCount;
    Add3DVertexUV(px0, py0, u0, v0, a);
    Add3DVertexUV(px1, py1, u1, v0, a);
    Add3DVertexUV(px2, py2, u0, v1, a);
    Add3DVertexUV(px1, py1, u1, v0, a);
    Add3DVertexUV(px3, py3, u1, v1, a);
    Add3DVertexUV(px2, py2, u0, v1, a);
    int32 count = scene3DVertCount - start;

    if (coalesce)
        scene3DBatches[scene3DBatchCount - 1].count += count;
    else
        scene3DBatches[scene3DBatchCount++] = { start, count, blend, spriteAtlas };
    return true;
}
} // namespace

bool RenderDevice::Use3DOffload()
{
    // Single-screen only (the special stage is screen 0); splitscreen keeps the CPU
    // path. scene3DVerts NULL (boot allocation failed) also forces the CPU fallback.
    // Only during live gameplay: when paused/frozen the pause menu draws its overlay
    // and labels on the CPU, and mixing those into the bg->GPU->fg composite scrambles
    // their draw order (labels landed in the GPU layer, behind the CPU overlay). Falling
    // back to the pure software path for those states keeps menus correct (and the frozen
    // scene has no perf cost worth offloading).
    return gpu3DEnabled && scene3DVerts && videoSettings.screenCount == 1 && currentScreen == &screens[0]
           && sceneInfo.state == ENGINESTATE_REGULAR;
}

void RenderDevice::Add3DFace(Vector2 *vertices, int32 vertCount, int32 r, int32 g, int32 b, int32 alpha, int32 inkEffect)
{
    uint32 rgb       = ((uint32)(r & 0xFF) << 16) | ((uint32)(g & 0xFF) << 8) | (uint32)(b & 0xFF);
    uint32 colors[4] = { rgb, rgb, rgb, rgb };
    SDL_BlendMode bl = Ink3DToBlend(inkEffect);
    float a          = (bl == SDL_BLENDMODE_NONE) ? 1.0f : (alpha > 0xFF ? 0xFF : (alpha < 0 ? 0 : alpha)) / 255.0f;
    if (vertCount > 4)
        vertCount = 4;
    Add3DFaceInternal(vertices, colors, vertCount, a, bl);
}

void RenderDevice::Add3DBlendedFace(Vector2 *vertices, uint32 *colors, int32 vertCount, int32 alpha, int32 inkEffect)
{
    SDL_BlendMode bl = Ink3DToBlend(inkEffect);
    float a          = (bl == SDL_BLENDMODE_NONE) ? 1.0f : (alpha > 0xFF ? 0xFF : (alpha < 0 ? 0 : alpha)) / 255.0f;
    if (vertCount > 4)
        vertCount = 4;
    Add3DFaceInternal(vertices, colors, vertCount, a, bl);
}

// Called by DrawSpriteRotozoom after it has computed the 4 transformed corners
// (posX/posY, screen-relative pixels = logical coords). In a UFO special stage, a
// scaled billboard is baked once and drawn as a textured GPU quad here instead of
// software-rasterized. Returns true if handled on the GPU; false -> CPU fallback.
bool RenderDevice::DrawSpriteGPU(int32 *posX, int32 *posY, int32 sprX, int32 sprY, int32 width, int32 height, int32 sheetID, int32 inkEffect,
                                 int32 alpha)
{
    // prevFrameHad3DFaces: only offload sprites during genuine 3D gameplay, so the
    // special-stage results/UI screens (sprites only, no 3D) render fully on the CPU
    // and keep their correct draw order (see had3DFacesThisFrame note).
    if (!Use3DOffload() || !InSpecialStage() || !prevFrameHad3DFaces)
        return false;
    SDL_BlendMode blend;
    float a;
    if (!InkToSpriteBlend(inkEffect, alpha, &blend, &a))
        return false;

    // Palette bank at the sprite's top scanline (per-line palette assumed uniform).
    int32 topY = posY[0];
    for (int32 i = 1; i < 4; ++i)
        if (posY[i] < topY)
            topY = posY[i];
    topY = topY < 0 ? 0 : (topY >= SCREEN_YSIZE ? SCREEN_YSIZE - 1 : topY);

    BakedSprite *bs = GetBakedSprite(sheetID, sprX, sprY, width, height, gfxLineBuffer[topY]);
    if (!bs)
        return false; // bake failed / atlas full -> CPU

    // Reuse the software rasterizer's exact 4 corners; the frame's full padded
    // sub-rect (u0..v1, border included) maps onto them 1:1.
    return EmitAtlasQuad((float)posX[0], (float)posY[0], (float)posX[1], (float)posY[1], (float)posX[2], (float)posY[2], (float)posX[3], (float)posY[3],
                         bs->u0, bs->v0, bs->u1, bs->v1, a, blend);
}

// Unscaled sprites (DrawSpriteFlipped: rings, HUD, effects). Axis-aligned quad from
// the sprite's screen rect; UVs are the frame's INNER sub-rect (excluding the 2px
// bake border), flipped per direction. Shares the atlas -> same coalesced batch.
bool RenderDevice::DrawSpriteFlippedGPU(int32 x, int32 y, int32 width, int32 height, int32 sprX, int32 sprY, int32 direction, int32 sheetID,
                                        int32 inkEffect, int32 alpha)
{
    if (!Use3DOffload() || !InSpecialStage() || !prevFrameHad3DFaces || width <= 0 || height <= 0)
        return false;
    SDL_BlendMode blend;
    float a;
    if (!InkToSpriteBlend(inkEffect, alpha, &blend, &a))
        return false;

    int32 topY      = y < 0 ? 0 : (y >= SCREEN_YSIZE ? SCREEN_YSIZE - 1 : y);
    BakedSprite *bs = GetBakedSprite(sheetID, sprX, sprY, width, height, gfxLineBuffer[topY]);
    if (!bs)
        return false;

    float bu = (float)SPR_BORDER / ATLAS_W, bv = (float)SPR_BORDER / ATLAS_H;
    float u0 = bs->u0 + bu, u1 = bs->u1 - bu, v0 = bs->v0 + bv, v1 = bs->v1 - bv;
    if (direction & FLIP_X) {
        float t = u0;
        u0      = u1;
        u1      = t;
    }
    if (direction & FLIP_Y) {
        float t = v0;
        v0      = v1;
        v1      = t;
    }

    float fx0 = (float)x, fy0 = (float)y, fx1 = (float)(x + width), fy1 = (float)(y + height);
    return EmitAtlasQuad(fx0, fy0, fx1, fy0, fx0, fy1, fx1, fy1, u0, v0, u1, v1, a, blend);
}

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

    // Allocate the special-stage GPU staging buffer once, at boot, while RAM is
    // plentiful (a mid-stage allocation would fail on the tight heap). NULL is
    // handled everywhere as "GPU 3D unavailable -> CPU path".
    if (!scene3DVerts)
        scene3DVerts = (SDL_Vertex *)malloc(MAX_3D_VERTS * sizeof(SDL_Vertex));

    InitInputDevices();
    return true;
}

void RenderDevice::CopyFrameBuffer()
{
    int32 pitch    = 0;
    uint16 *pixels = NULL;

#if RETRO_PLATFORM == RETRO_XBOX
    // 3D frame: build the background (pre-3D snapshot) and foreground (post-3D 2D,
    // alpha-keyed by diffing against the snapshot) layers for the composite present.
    if (has3DThisFrame && bg3DBuffer) {
        int32 w = screens[0].size.x, h = screens[0].size.y, fbPitch = screens[0].pitch;

        void *bgPix = NULL;
        if (screenTexture[0] && SDL_LockTexture(screenTexture[0], NULL, &bgPix, &pitch)) {
            uint16 *dst = (uint16 *)bgPix;
            uint16 *src = bg3DBuffer;
            for (int32 y = 0; y < h; ++y) {
                memcpy(dst, src, w * sizeof(uint16));
                src += fbPitch;
                dst += pitch / sizeof(uint16);
            }
            SDL_UnlockTexture(screenTexture[0]);
        }

        if (!fg3DTexture) {
            fg3DTexture =
                SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, (int)textureSize.x, (int)textureSize.y);
            if (fg3DTexture) {
                SDL_SetTextureScaleMode(fg3DTexture, SDL_SCALEMODE_NEAREST);
                SDL_SetTextureBlendMode(fg3DTexture, SDL_BLENDMODE_BLEND);
            }
        }
        void *fgPix = NULL;
        if (fg3DTexture && SDL_LockTexture(fg3DTexture, NULL, &fgPix, &pitch)) {
            uint32 *dst     = (uint32 *)fgPix;
            uint16 *fb      = screens[0].frameBuffer;
            uint16 *bg      = bg3DBuffer;
            int32 dstStride = pitch / (int32)sizeof(uint32);
            for (int32 y = 0; y < h; ++y) {
                for (int32 x = 0; x < w; ++x) dst[x] = (fb[x] != bg[x]) ? (0xFF000000u | RGB565to888(fb[x])) : 0u;
                fb += fbPitch;
                bg += fbPitch;
                dst += dstStride;
            }
            SDL_UnlockTexture(fg3DTexture);
        }
        return;
    }
#endif

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
    paletteCheckedThisFrame = false; // re-check the palette once next frame
    inSpecialCache          = -1; // re-detect the special stage next frame
    // Roll the 3D-face history forward: next frame's sprite offload gate keys off
    // whether THIS frame drew real 3D geometry (i.e. we are in 3D gameplay).
    prevFrameHad3DFaces = had3DFacesThisFrame;
    had3DFacesThisFrame = false;
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

#if RETRO_PLATFORM == RETRO_XBOX
    // 3D composite present: background 2D -> GPU 3D triangles -> foreground 2D.
    // Coordinates are in the logical space (pixWidth x SCREEN_YSIZE) that the 3D
    // vertices were projected into, so all three layers align.
    if (has3DThisFrame) {
        SDL_FRect dst3D = { 0.0f, 0.0f, (float)videoSettings.pixWidth, (float)SCREEN_YSIZE };
        SDL_FRect src3D = { 0.0f, 0.0f, (float)screens[0].size.x, (float)screens[0].size.y };

        if (screenTexture[0])
            SDL_RenderTexture(renderer, screenTexture[0], &src3D, &dst3D);

        for (int32 i = 0; i < scene3DBatchCount; ++i) {
            SDL_SetRenderDrawBlendMode(renderer, scene3DBatches[i].blend);
            SDL_RenderGeometry(renderer, scene3DBatches[i].tex, &scene3DVerts[scene3DBatches[i].start], scene3DBatches[i].count, NULL, 0);
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        if (fg3DTexture)
            SDL_RenderTexture(renderer, fg3DTexture, &src3D, &dst3D);

        if (dimAmount < 1.0f) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF - (uint8)(dimAmount * 0xFF));
            SDL_RenderFillRect(renderer, NULL);
        }

        SDL_RenderPresent(renderer);

        has3DThisFrame    = false;
        scene3DVertCount  = 0;
        scene3DBatchCount = 0;
        return;
    }
#endif

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

#if RETRO_PLATFORM == RETRO_XBOX
    if (fg3DTexture)
        SDL_DestroyTexture(fg3DTexture);
    fg3DTexture       = NULL;
    ClearBakedSprites();
    if (spriteAtlas)
        SDL_DestroyTexture(spriteAtlas);
    spriteAtlas       = NULL;
    bakedPaletteSum   = 0;
    has3DThisFrame    = false;
    scene3DVertCount  = 0;
    scene3DBatchCount = 0;
    if (!isRefresh) {
        free(bg3DBuffer);
        bg3DBuffer = NULL;
        bg3DPitch  = 0;
        bg3DHeight = 0;
        free(scene3DVerts);
        scene3DVerts = NULL;
    }
#endif

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
void RenderDevice::SetFPSTarget(int32 fps)
{
    if (fps < 1)
        fps = 1;
    targetFreq = SDL_GetPerformanceFrequency() / fps;
}

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
