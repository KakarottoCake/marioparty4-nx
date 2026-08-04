// GX -> OpenGL translation layer (compiled with TARGET_PC so the GX immediate
// mode + texture calls become real functions instead of GameCube FIFO writes).
// Scope: enough of GX to draw 2D sprites and the current static HSF mesh slice.
#ifdef __SWITCH__

#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <math.h>
#include <string.h>
#include "gfx_switch.h"

extern void OSReport(const char* msg, ...);

// ---------------------------------------------------------------------------
// Matrix state + math (Mtx = f32[3][4], Mtx44 = f32[4][4], row-major)
// ---------------------------------------------------------------------------
static float s_proj[4][4];   // current projection (GXSetProjection)
static float s_pos[3][4];    // current position/modelview matrix (GXLoadPosMtxImm)

void PSMTXIdentity(Mtx m) {
    memset(m, 0, sizeof(float) * 12);
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

void PSMTXCopy(const Mtx src, Mtx dst) { memcpy(dst, src, sizeof(float) * 12); }

void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) {
    Mtx r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            float s = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
            if (j == 3) s += a[i][3];  // b's implicit 4th row is (0,0,0,1)
            r[i][j] = s;
        }
    }
    memcpy(ab, r, sizeof(r));
}

void PSMTXScale(Mtx m, float x, float y, float z) {
    PSMTXIdentity(m);
    m[0][0] = x; m[1][1] = y; m[2][2] = z;
}

void PSMTXTrans(Mtx m, float x, float y, float z) {
    PSMTXIdentity(m);
    m[0][3] = x; m[1][3] = y; m[2][3] = z;
}

void PSMTXRotAxisRad(Mtx m, const Vec* axis, float rad) {
    float x = axis->x, y = axis->y, z = axis->z;
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0.0f) { x /= len; y /= len; z /= len; }
    float c = cosf(rad), s = sinf(rad), t = 1.0f - c;
    m[0][0] = t*x*x + c;   m[0][1] = t*x*y - s*z; m[0][2] = t*x*z + s*y; m[0][3] = 0;
    m[1][0] = t*x*y + s*z; m[1][1] = t*y*y + c;   m[1][2] = t*y*z - s*x; m[1][3] = 0;
    m[2][0] = t*x*z - s*y; m[2][1] = t*y*z + s*x; m[2][2] = t*z*z + c;   m[2][3] = 0;
}

void mtxRot(Mtx m, float x, float y, float z) {
    Vec axis;
    Mtx turn;
    const float radians = 0.017453292519943295f;
    PSMTXIdentity(m);
    if (x != 0.0f) {
        axis.x = 1.0f; axis.y = 0.0f; axis.z = 0.0f;
        PSMTXRotAxisRad(turn, &axis, x * radians);
        PSMTXConcat(turn, m, m);
    }
    if (y != 0.0f) {
        axis.x = 0.0f; axis.y = 1.0f; axis.z = 0.0f;
        PSMTXRotAxisRad(turn, &axis, y * radians);
        PSMTXConcat(turn, m, m);
    }
    if (z != 0.0f) {
        axis.x = 0.0f; axis.y = 0.0f; axis.z = 1.0f;
        PSMTXRotAxisRad(turn, &axis, z * radians);
        PSMTXConcat(turn, m, m);
    }
}

void mtxRotCat(Mtx m, float x, float y, float z) {
    Mtx turn;
    mtxRot(turn, x, y, z);
    PSMTXConcat(turn, m, m);
}

void mtxScaleCat(Mtx m, float x, float y, float z) {
    m[0][0] *= x; m[1][0] *= x; m[2][0] *= x;
    m[0][1] *= y; m[1][1] *= y; m[2][1] *= y;
    m[0][2] *= z; m[1][2] *= z; m[2][2] *= z;
}

void C_MTXLookAt(Mtx m, const Point3d* camPos, const Vec* camUp,
                 const Point3d* target) {
    float fx = target->x - camPos->x;
    float fy = target->y - camPos->y;
    float fz = target->z - camPos->z;
    float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    float rx, ry, rz;
    float rl;
    float ux, uy, uz;

    if (fl < 0.000001f) {
        fx = 0.0f; fy = 0.0f; fz = -1.0f;
        fl = 1.0f;
    }
    fx /= fl; fy /= fl; fz /= fl;
    /* right = forward x up.  Using up x forward instead negates the camera's
     * X axis, which mirrors the whole scene and reverses triangle winding so
     * backface culling then discards the wrong faces. */
    rx = fy * camUp->z - fz * camUp->y;
    ry = fz * camUp->x - fx * camUp->z;
    rz = fx * camUp->y - fy * camUp->x;
    rl = sqrtf(rx * rx + ry * ry + rz * rz);
    if (rl < 0.000001f) {
        rx = 1.0f; ry = 0.0f; rz = 0.0f;
        rl = 1.0f;
    }
    rx /= rl; ry /= rl; rz /= rl;
    /* up = right x forward */
    ux = ry * fz - rz * fy;
    uy = rz * fx - rx * fz;
    uz = rx * fy - ry * fx;
    m[0][0] = rx; m[0][1] = ry; m[0][2] = rz;
    m[1][0] = ux; m[1][1] = uy; m[1][2] = uz;
    m[2][0] = -fx; m[2][1] = -fy; m[2][2] = -fz;
    m[0][3] = -(rx * camPos->x + ry * camPos->y + rz * camPos->z);
    m[1][3] = -(ux * camPos->x + uy * camPos->y + uz * camPos->z);
    m[2][3] = fx * camPos->x + fy * camPos->y + fz * camPos->z;
}

void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 nearZ, f32 farZ) {
    float cot;
    float radians = fovY * 0.017453292519943295f;
    memset(m, 0, sizeof(Mtx44));
    if (aspect == 0.0f) aspect = 4.0f / 3.0f;
    if (nearZ <= 0.0f) nearZ = 0.1f;
    if (farZ <= nearZ) farZ = nearZ + 1.0f;
    cot = 1.0f / tanf(radians * 0.5f);
    m[0][0] = cot / aspect;
    m[1][1] = cot;
    m[2][2] = farZ / (nearZ - farZ);
    m[2][3] = (nearZ * farZ) / (nearZ - farZ);
    m[3][2] = -1.0f;
}

u32 PSMTXInvXpose(const Mtx src, Mtx dst) {
    float a = src[0][0], b = src[0][1], c = src[0][2];
    float d = src[1][0], e = src[1][1], f = src[1][2];
    float g = src[2][0], h = src[2][1], i = src[2][2];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabsf(det) < 0.000001f) {
        PSMTXIdentity(dst);
        return 0;
    }
    {
        float inv = 1.0f / det;
        dst[0][0] = (e * i - f * h) * inv;
        dst[0][1] = (d * h - b * i) * inv;
        dst[0][2] = (b * f - d * e) * inv;
        dst[1][0] = (g * f - d * i) * inv;
        dst[1][1] = (a * i - c * g) * inv;
        dst[1][2] = (c * d - a * f) * inv;
        dst[2][0] = (d * h - e * g) * inv;
        dst[2][1] = (b * g - a * h) * inv;
        dst[2][2] = (a * e - b * d) * inv;
        dst[0][3] = dst[1][3] = dst[2][3] = 0.0f;
    }
    return 1;
}

// Concatenate a translation onto m (result = Trans(x,y,z) * m): just offset col 3.
void mtxTransCat(Mtx m, float x, float y, float z) {
    m[0][3] += x; m[1][3] += y; m[2][3] += z;
}

void C_MTXOrtho(Mtx44 m, float t, float b, float l, float r, float n, float f) {
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = 2.0f / (r - l);   m[0][3] = -(r + l) / (r - l);
    m[1][1] = 2.0f / (t - b);   m[1][3] = -(t + b) / (t - b);
    m[2][2] = -1.0f / (f - n);  m[2][3] = -n / (f - n);
    m[3][3] = 1.0f;
}

void GXSetProjection(const void* mtx, GXProjectionType type) {
    (void)type;
    memcpy(s_proj, mtx, sizeof(float) * 16);
}

void GXLoadPosMtxImm(const void* mtx, u32 id) {
    (void)id;
    memcpy(s_pos, mtx, sizeof(float) * 12);
}

// ---------------------------------------------------------------------------
// Texture state + GameCube texture decoding
// ---------------------------------------------------------------------------
typedef struct {
    const void* data;
    const void* palette;
    int w;
    int h;
    int fmt;
    int palette_fmt;
    int palette_entries;
    u32 tlut;
    int wrap_s;
    int wrap_t;
} MyTexObj;

typedef struct {
    const void* data;
    int fmt;
    int entries;
} MyTlutObj;

/* GXTexObj/GXTlutObj are opaque GameCube-sized structs.  Keep the larger
 * Switch-side state out of those caller-owned buffers; some callers allocate
 * them as 32-byte/12-byte stack objects. */
#define TEXOBJ_STATE_MAX 1024
#define TLUTOBJ_STATE_MAX 64
static struct {
    const GXTexObj* key;
    MyTexObj state;
} s_texObjStates[TEXOBJ_STATE_MAX];
static int s_texObjStateN = 0;
static struct {
    const GXTlutObj* key;
    MyTlutObj state;
} s_tlutObjStates[TLUTOBJ_STATE_MAX];
static int s_tlutObjStateN = 0;

// Cache decoded GL textures by image and palette.  The same image pointer can
// be used with different TLUT slots by the original renderer.
#define TEXCACHE_MAX 512
static struct {
    const void* key;
    const void* palette;
    int fmt;
    int palette_fmt;
    int wrap_s;
    int wrap_t;
    unsigned int tex;
} s_texCache[TEXCACHE_MAX];
static int s_texCacheN = 0;

#define TLUT_MAX 16
static MyTlutObj s_tluts[TLUT_MAX];

static unsigned int s_curTex = 0;   // compatibility alias for GX_TEXMAP0
static unsigned int s_texMaps[8];   // Aurora-style per-map texture bindings
static float s_tint[4] = {1, 1, 1, 1};
static GXBool s_useMaterialTint = TRUE;

/*
 * Small CPU-side representation of the GX lighting state.  GLES2 still
 * receives the final color as a vertex attribute, but keeping the decoded
 * light/channel state here makes the old GX calls useful instead of silently
 * dropping all normals and lights.
 */
#define SWITCH_GX_LIGHT_MAX 8
#define SWITCH_GX_LIGHT_OBJECT_MAX 64
typedef struct SwitchGXLightRaw_s {
    u32 reserved[3];
    u32 color;
    float a[3];
    float k[3];
    float lpos[3];
    float ldir[3];
} SwitchGXLightRaw;

typedef struct SwitchGXChannelState_s {
    GXBool enable;
    GXColorSrc ambSrc;
    GXColorSrc matSrc;
    u32 lightMask;
    GXDiffuseFn diffFn;
    GXAttnFn attnFn;
} SwitchGXChannelState;

static SwitchGXLightRaw s_lights[SWITCH_GX_LIGHT_MAX];
static GXLightObj* s_lightObjectKeys[SWITCH_GX_LIGHT_OBJECT_MAX];
static int s_lightObjectCount;
static SwitchGXChannelState s_chanState[2];
static GXColor s_chanAmb[2] = {{0, 0, 0, 255}, {0, 0, 0, 255}};
static GXColor s_chanMat[2] = {{255, 255, 255, 255}, {255, 255, 255, 255}};
static u8 s_numChans;

// GX TEV state.  The original hardware stores this in BP registers; Aurora
// keeps an equivalent decoded state and generates shader code from it.  The
// Switch path keeps the decoded values in a compact GLES2-friendly snapshot.
static GfxTevState s_tevState;
static BOOL s_tevStateReady = FALSE;

static void GXGLInitTevState(void) {
    int i;
    memset(&s_tevState, 0, sizeof(s_tevState));
    s_tevState.numStages = 1;
    for (i = 0; i < GFX_TEV_MAX_STAGES; i++) {
        GfxTevStageState* stage = &s_tevState.stages[i];
        stage->colorIn[0] = GX_CC_ZERO;
        stage->colorIn[1] = GX_CC_ZERO;
        stage->colorIn[2] = GX_CC_ZERO;
        stage->colorIn[3] = GX_CC_ZERO;
        stage->alphaIn[0] = GX_CA_ZERO;
        stage->alphaIn[1] = GX_CA_ZERO;
        stage->alphaIn[2] = GX_CA_ZERO;
        stage->alphaIn[3] = GX_CA_ZERO;
        stage->colorOp = GX_TEV_ADD;
        stage->colorBias = GX_TB_ZERO;
        stage->colorScale = GX_CS_SCALE_1;
        stage->colorClamp = GX_TRUE;
        stage->colorOut = GX_TEVPREV;
        stage->alphaOp = GX_TEV_ADD;
        stage->alphaBias = GX_TB_ZERO;
        stage->alphaScale = GX_CS_SCALE_1;
        stage->alphaClamp = GX_TRUE;
        stage->alphaOut = GX_TEVPREV;
        stage->texMap = GX_TEXMAP_NULL;
        stage->channel = GX_COLOR_NULL;
        stage->kColorSel = GX_TEV_KCSEL_1;
        stage->kAlphaSel = GX_TEV_KASEL_1;
    }
    // Match GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE), the common startup mode.
    s_tevState.stages[0].colorIn[0] = GX_CC_ZERO;
    s_tevState.stages[0].colorIn[1] = GX_CC_TEXC;
    s_tevState.stages[0].colorIn[2] = GX_CC_RASC;
    s_tevState.stages[0].alphaIn[0] = GX_CA_ZERO;
    s_tevState.stages[0].alphaIn[1] = GX_CA_TEXA;
    s_tevState.stages[0].alphaIn[2] = GX_CA_RASA;
    s_tevState.stages[0].texMap = GX_TEXMAP0;
    s_tevState.stages[0].channel = GX_COLOR0A0;
    s_tevStateReady = TRUE;
}

static void GXGLEnsureTevState(void) {
    if (!s_tevStateReady) GXGLInitTevState();
}

static void GXGLPushTevState(void) {
    GXGLEnsureTevState();
    GfxSetTevState(&s_tevState);
}

static MyTexObj* GetTexObjState(const GXTexObj* key) {
    int i;
    if (!key) return NULL;
    for (i = 0; i < s_texObjStateN; i++) {
        if (s_texObjStates[i].key == key) return &s_texObjStates[i].state;
    }
    if (s_texObjStateN >= TEXOBJ_STATE_MAX) return NULL;
    s_texObjStates[s_texObjStateN].key = key;
    memset(&s_texObjStates[s_texObjStateN].state, 0,
           sizeof(s_texObjStates[s_texObjStateN].state));
    return &s_texObjStates[s_texObjStateN++].state;
}

static MyTlutObj* GetTlutObjState(const GXTlutObj* key) {
    int i;
    if (!key) return NULL;
    for (i = 0; i < s_tlutObjStateN; i++) {
        if (s_tlutObjStates[i].key == key) return &s_tlutObjStates[i].state;
    }
    if (s_tlutObjStateN >= TLUTOBJ_STATE_MAX) return NULL;
    s_tlutObjStates[s_tlutObjStateN].key = key;
    memset(&s_tlutObjStates[s_tlutObjStateN].state, 0,
           sizeof(s_tlutObjStates[s_tlutObjStateN].state));
    return &s_tlutObjStates[s_tlutObjStateN++].state;
}

static const MyTlutObj* FindTlutObjState(const GXTlutObj* key) {
    int i;
    if (!key) return NULL;
    for (i = 0; i < s_tlutObjStateN; i++) {
        if (s_tlutObjStates[i].key == key) return &s_tlutObjStates[i].state;
    }
    return NULL;
}

static inline void PutRGBA(unsigned char* p, int idx, int r, int g, int b, int a) {
    p[idx*4+0] = (unsigned char)r; p[idx*4+1] = (unsigned char)g;
    p[idx*4+2] = (unsigned char)b; p[idx*4+3] = (unsigned char)a;
}

static int Expand4(int value) { return value * 17; }

static void DecodeRGB565(unsigned value, int* r, int* g, int* b, int* a) {
    *r = ((value >> 11) & 0x1F) * 255 / 31;
    *g = ((value >> 5) & 0x3F) * 255 / 63;
    *b = (value & 0x1F) * 255 / 31;
    *a = 255;
}

static void DecodeRGB5A3(unsigned value, int* r, int* g, int* b, int* a) {
    if (value & 0x8000) {
        *r = ((value >> 10) & 0x1F) * 255 / 31;
        *g = ((value >> 5) & 0x1F) * 255 / 31;
        *b = (value & 0x1F) * 255 / 31;
        *a = 255;
    } else {
        *a = ((value >> 12) & 0x7) * 255 / 7;
        *r = ((value >> 8) & 0xF) * 255 / 15;
        *g = ((value >> 4) & 0xF) * 255 / 15;
        *b = (value & 0xF) * 255 / 15;
    }
}

static void DecodePaletteColor(const MyTexObj* obj, int index,
                               int* r, int* g, int* b, int* a) {
    const unsigned char* p;
    unsigned value;
    if (!obj->palette || index < 0 || index >= obj->palette_entries) {
        *r = *g = *b = 255;
        *a = 0;
        return;
    }
    p = (const unsigned char*)obj->palette + index * 2;
    value = ((unsigned)p[0] << 8) | p[1];
    switch (obj->palette_fmt) {
        case GX_TL_IA8:
            *r = *g = *b = p[0];
            *a = p[1];
            break;
        case GX_TL_RGB565:
            DecodeRGB565(value, r, g, b, a);
            break;
        case GX_TL_RGB5A3:
        default:
            DecodeRGB5A3(value, r, g, b, a);
            break;
    }
}

// Decode a GameCube (tiled, big-endian) texture into a linear RGBA8 buffer.
static void DecodeGCTexture(int fmt, const unsigned char* src, int w, int h,
                            const MyTexObj* obj, unsigned char* out) {
    // Default: opaque white (so unsupported formats still show a shape).
    for (int i = 0; i < w * h; i++) PutRGBA(out, i, 255, 255, 255, 255);
    if (!src) return;

    if (fmt == GX_TF_RGBA8) {
        // 4x4 tiles, 64 bytes each: 32 bytes AR then 32 bytes GB.
        int o = 0;
        for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 4) {
            for (int k = 0; k < 16; k++) {         // AR plane
                int px = tx + (k % 4), py = ty + (k / 4);
                int a = src[o++], r = src[o++];
                if (px < w && py < h) { out[(py*w+px)*4+3] = a; out[(py*w+px)*4+0] = r; }
            }
            for (int k = 0; k < 16; k++) {         // GB plane
                int px = tx + (k % 4), py = ty + (k / 4);
                int g = src[o++], b = src[o++];
                if (px < w && py < h) { out[(py*w+px)*4+1] = g; out[(py*w+px)*4+2] = b; }
            }
        }
    } else if (fmt == GX_TF_RGB5A3 || fmt == GX_TF_RGB565) {
        int o = 0;
        for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 4)
        for (int k = 0; k < 16; k++) {
            int px = tx + (k % 4), py = ty + (k / 4);
            unsigned v = (src[o] << 8) | src[o + 1];  // big-endian u16
            o += 2;
            int r, g, b, a;
            if (fmt == GX_TF_RGB565) DecodeRGB565(v, &r, &g, &b, &a);
            else DecodeRGB5A3(v, &r, &g, &b, &a);
            if (px < w && py < h) PutRGBA(out, py * w + px, r, g, b, a);
        }
    } else if (fmt == GX_TF_I4 || fmt == GX_TF_C4) {
        // I4/C4: 8x8 tiles, two texels per byte.
        int o = 0;
        for (int ty = 0; ty < h; ty += 8)
        for (int tx = 0; tx < w; tx += 8) {
            for (int row = 0; row < 8; row++) {
                for (int col = 0; col < 4; col++) {
                    unsigned value = src[o++];
                    int indexes[2] = {(int)(value >> 4), (int)(value & 0xF)};
                    for (int n = 0; n < 2; n++) {
                        int px = tx + col * 2 + n;
                        int py = ty + row;
                        int r, g, b, a;
                        if (fmt == GX_TF_C4) {
                            DecodePaletteColor(obj, indexes[n], &r, &g, &b, &a);
                        } else {
                            r = g = b = Expand4(indexes[n]);
                            a = 255;
                        }
                        if (px < w && py < h) PutRGBA(out, py * w + px, r, g, b, a);
                    }
                }
            }
        }
    } else if (fmt == GX_TF_I8 || fmt == GX_TF_IA4 || fmt == GX_TF_A8 || fmt == GX_TF_C8) {
        // I8/IA4/A8/C8: 8x4 tiles, one texel per byte.
        int o = 0;
        for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 8)
        for (int row = 0; row < 4; row++)
        for (int col = 0; col < 8; col++) {
            unsigned value = src[o++];
            int px = tx + col;
            int py = ty + row;
            int r, g, b, a;
            if (fmt == GX_TF_C8) {
                DecodePaletteColor(obj, (int)value, &r, &g, &b, &a);
            } else if (fmt == GX_TF_I8) {
                r = g = b = (int)value;
                a = 255;
            } else if (fmt == GX_TF_IA4) {
                r = g = b = Expand4((int)(value >> 4));
                a = Expand4((int)(value & 0xF));
            } else {
                r = g = b = 255;
                a = (int)value;
            }
            if (px < w && py < h) PutRGBA(out, py * w + px, r, g, b, a);
        }
    } else if (fmt == GX_TF_IA8) {
        // IA8: 4x4 tiles, intensity byte followed by alpha byte.
        int o = 0;
        for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 4)
        for (int k = 0; k < 16; k++) {
            int px = tx + (k % 4), py = ty + (k / 4);
            int intensity = src[o++];
            int alpha = src[o++];
            if (px < w && py < h) PutRGBA(out, py * w + px,
                                           intensity, intensity, intensity, alpha);
        }
    } else if (fmt == GX_TF_CMPR) {
        // CMPR is four DXT1 4x4 blocks inside each 8x8 tile.
        int o = 0;
        for (int ty = 0; ty < h; ty += 8)
        for (int tx = 0; tx < w; tx += 8)
        for (int block = 0; block < 4; block++) {
            int bx = tx + (block & 1) * 4;
            int by = ty + (block >> 1) * 4;
            unsigned c0 = ((unsigned)src[o] << 8) | src[o + 1];
            unsigned c1 = ((unsigned)src[o + 2] << 8) | src[o + 3];
            unsigned bits = ((unsigned)src[o + 4] << 24) |
                            ((unsigned)src[o + 5] << 16) |
                            ((unsigned)src[o + 6] << 8) | src[o + 7];
            int r0, g0, b0, a0, r1, g1, b1, a1;
            int colors[4][4];
            DecodeRGB565(c0, &r0, &g0, &b0, &a0);
            DecodeRGB565(c1, &r1, &g1, &b1, &a1);
            colors[0][0] = r0; colors[0][1] = g0; colors[0][2] = b0; colors[0][3] = 255;
            colors[1][0] = r1; colors[1][1] = g1; colors[1][2] = b1; colors[1][3] = 255;
            if (c0 > c1) {
                colors[2][0] = (2*r0 + r1) / 3; colors[2][1] = (2*g0 + g1) / 3;
                colors[2][2] = (2*b0 + b1) / 3; colors[2][3] = 255;
                colors[3][0] = (r0 + 2*r1) / 3; colors[3][1] = (g0 + 2*g1) / 3;
                colors[3][2] = (b0 + 2*b1) / 3; colors[3][3] = 255;
            } else {
                colors[2][0] = (r0 + r1) / 2; colors[2][1] = (g0 + g1) / 2;
                colors[2][2] = (b0 + b1) / 2; colors[2][3] = 255;
                colors[3][0] = colors[3][1] = colors[3][2] = 0; colors[3][3] = 0;
            }
            o += 8;
            for (int row = 0; row < 4; row++)
            for (int col = 0; col < 4; col++) {
                int index = (bits >> (30 - (row * 4 + col) * 2)) & 3;
                int px = bx + col, py = by + row;
                if (px < w && py < h)
                    PutRGBA(out, py * w + px, colors[index][0], colors[index][1],
                            colors[index][2], colors[index][3]);
            }
        }
    }
}

unsigned int g_texHit, g_texMiss, g_texUpload, g_texProbe;

static unsigned int GetOrCreateTexture(const MyTexObj* obj) {
    if (!obj || !obj->data) return 0;
    for (int i = 0; i < s_texCacheN; i++)
        if (s_texCache[i].key == obj->data &&
            s_texCache[i].palette == obj->palette &&
            s_texCache[i].fmt == obj->fmt &&
            s_texCache[i].palette_fmt == obj->palette_fmt &&
            s_texCache[i].wrap_s == obj->wrap_s &&
            s_texCache[i].wrap_t == obj->wrap_t) {
            g_texHit++; g_texProbe += (unsigned int)i;
            return s_texCache[i].tex;
        }
    g_texMiss++;

    int w = obj->w, h = obj->h;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return 0;
    static unsigned char buf[1024 * 1024 * 4];
    DecodeGCTexture(obj->fmt, (const unsigned char*)obj->data, w, h, obj, buf);
    unsigned int tex = GfxCreateTexture(w, h, buf,
                                        obj->wrap_s, obj->wrap_t);
    g_texUpload++;
    if (s_texCacheN < TEXCACHE_MAX) {
        s_texCache[s_texCacheN].key = obj->data;
        s_texCache[s_texCacheN].palette = obj->palette;
        s_texCache[s_texCacheN].fmt = obj->fmt;
        s_texCache[s_texCacheN].palette_fmt = obj->palette_fmt;
        s_texCache[s_texCacheN].wrap_s = obj->wrap_s;
        s_texCache[s_texCacheN].wrap_t = obj->wrap_t;
        s_texCache[s_texCacheN].tex = tex;
        s_texCacheN++;
    }
    return tex;
}

void GXInitTexObj(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                  GXTexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t, u8 mip) {
    (void)mip;
    MyTexObj* t = GetTexObjState(obj);
    if (!t) return;
    t->data = image_ptr;
    t->palette = NULL;
    t->w = width;
    t->h = height;
    t->fmt = (int)format;
    t->palette_fmt = GX_TL_RGB5A3;
    t->palette_entries = 0;
    t->tlut = TLUT_MAX;
    t->wrap_s = (int)wrap_s;
    t->wrap_t = (int)wrap_t;
}

void GXInitTexObjCI(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                    GXCITexFmt format, GXTexWrapMode s, GXTexWrapMode t, u8 mip, u32 tlut) {
    (void)mip;
    MyTexObj* to = GetTexObjState(obj);
    if (!to) return;
    to->data = image_ptr;
    to->w = width;
    to->h = height;
    to->fmt = (int)format;
    to->tlut = tlut;
    to->wrap_s = (int)s;
    to->wrap_t = (int)t;
    if (tlut < TLUT_MAX) {
        to->palette = s_tluts[tlut].data;
        to->palette_fmt = s_tluts[tlut].fmt;
        to->palette_entries = s_tluts[tlut].entries;
    } else {
        to->palette = NULL;
        to->palette_fmt = GX_TL_RGB5A3;
        to->palette_entries = 0;
    }
}

void GXInitTlutObj(GXTlutObj* obj, void* data, GXTlutFmt fmt, u16 entries) {
    MyTlutObj* tlut = GetTlutObjState(obj);
    if (!tlut) return;
    tlut->data = data;
    tlut->fmt = (int)fmt;
    tlut->entries = entries;
}

void GXLoadTlut(GXTlutObj* obj, u32 tlut_name) {
    const MyTlutObj* tlut = FindTlutObjState(obj);
    if (!obj || tlut_name >= TLUT_MAX) return;
    if (!tlut) return;
    s_tluts[tlut_name] = *tlut;
}

void GXLoadTexObj(GXTexObj* obj, GXTexMapID id) {
    MyTexObj* state;
    if (id >= GX_MAX_TEXMAP) return;
    state = GetTexObjState(obj);
    if (!state) return;
    if (state->tlut < TLUT_MAX) {
        state->palette = s_tluts[state->tlut].data;
        state->palette_fmt = s_tluts[state->tlut].fmt;
        state->palette_entries = s_tluts[state->tlut].entries;
    }
    s_texMaps[id] = GetOrCreateTexture(state);
    if (id == GX_TEXMAP0) s_curTex = s_texMaps[id];
}

void GXInvalidateTexAll(void) {
    memset(s_texMaps, 0, sizeof(s_texMaps));
    s_curTex = 0;
}

// ---------------------------------------------------------------------------
// Immediate-mode vertex capture
// ---------------------------------------------------------------------------
#define MAXV 64
static float s_vClip[MAXV][3];
static float s_vUV[MAXV][2];
static float s_vColor[MAXV][4];
static float s_vLighting[MAXV][4];
static float s_curVertexColor[4] = {1, 1, 1, 1};
static float s_curNormal[3] = {0, 0, 1};
static int s_vCount = 0;
static int s_prim = 0;
static BOOL s_3dMode = FALSE;

static void SwitchNormalize3(float *x, float *y, float *z) {
    float length = sqrtf((*x) * (*x) + (*y) * (*y) + (*z) * (*z));
    if (length > 0.000001f) {
        *x /= length;
        *y /= length;
        *z /= length;
    } else {
        *x = 0.0f;
        *y = 0.0f;
        *z = 1.0f;
    }
}

static int SwitchLightBitIndex(u32 light) {
    int i;
    for (i = 0; i < SWITCH_GX_LIGHT_MAX; i++) {
        if (light == (1U << i)) return i;
    }
    return -1;
}

static float SwitchLightColor(u32 packed, int shift) {
    return (float)((packed >> shift) & 0xFFU) / 255.0f;
}

static void SwitchComputeLighting(float x, float y, float z, float out[4]) {
    const SwitchGXChannelState *channel = &s_chanState[0];
    float nx = s_pos[0][0] * s_curNormal[0] +
               s_pos[0][1] * s_curNormal[1] +
               s_pos[0][2] * s_curNormal[2];
    float ny = s_pos[1][0] * s_curNormal[0] +
               s_pos[1][1] * s_curNormal[1] +
               s_pos[1][2] * s_curNormal[2];
    float nz = s_pos[2][0] * s_curNormal[0] +
               s_pos[2][1] * s_curNormal[1] +
               s_pos[2][2] * s_curNormal[2];
    float r;
    float g;
    float b;
    int i;

    out[0] = 1.0f;
    out[1] = 1.0f;
    out[2] = 1.0f;
    out[3] = 1.0f;
    if (!channel->enable) return;

    SwitchNormalize3(&nx, &ny, &nz);
    r = channel->ambSrc == GX_SRC_REG ? s_chanAmb[0].r / 255.0f : 1.0f;
    g = channel->ambSrc == GX_SRC_REG ? s_chanAmb[0].g / 255.0f : 1.0f;
    b = channel->ambSrc == GX_SRC_REG ? s_chanAmb[0].b / 255.0f : 1.0f;

    for (i = 0; i < SWITCH_GX_LIGHT_MAX; i++) {
        const SwitchGXLightRaw *light;
        float lx;
        float ly;
        float lz;
        float distance;
        float ndotl;
        float attenuation = 1.0f;
        float spot = 1.0f;
        float denom;
        float contribution;
        BOOL directional;

        if ((channel->lightMask & (1U << i)) == 0) continue;
        light = &s_lights[i];
        directional = fabsf(light->lpos[0]) < 0.0001f &&
                      fabsf(light->lpos[1]) < 0.0001f &&
                      fabsf(light->lpos[2]) < 0.0001f &&
                      (fabsf(light->ldir[0]) > 0.0001f ||
                       fabsf(light->ldir[1]) > 0.0001f ||
                       fabsf(light->ldir[2]) > 0.0001f);
        if (directional) {
            /* GXInitLightDir stores the negated input direction. */
            lx = -light->ldir[0];
            ly = -light->ldir[1];
            lz = -light->ldir[2];
            distance = 1.0f;
        } else {
            lx = light->lpos[0] - x;
            ly = light->lpos[1] - y;
            lz = light->lpos[2] - z;
            distance = sqrtf(lx * lx + ly * ly + lz * lz);
            if (distance < 0.0001f) continue;
            lx /= distance;
            ly /= distance;
            lz /= distance;
        }
        SwitchNormalize3(&lx, &ly, &lz);
        ndotl = nx * lx + ny * ly + nz * lz;
        if (channel->diffFn == GX_DF_SIGN) {
            ndotl = ndotl * 2.0f - 1.0f;
        } else {
            ndotl = ndotl < 0.0f ? 0.0f : ndotl;
        }

        denom = light->k[0] + light->k[1] * distance +
                light->k[2] * distance * distance;
        if (denom > 0.0001f) attenuation = 1.0f / denom;
        if (channel->attnFn == GX_AF_SPOT) {
            float spotDot = -(nx * light->ldir[0] +
                              ny * light->ldir[1] +
                              nz * light->ldir[2]);
            spot = light->a[0] + light->a[1] * spotDot +
                   light->a[2] * spotDot * spotDot;
            if (spot < 0.0f) spot = 0.0f;
        }
        contribution = ndotl * attenuation * spot;
        if (contribution <= 0.0f) continue;
        r += SwitchLightColor(light->color, 24) * contribution;
        g += SwitchLightColor(light->color, 16) * contribution;
        b += SwitchLightColor(light->color, 8) * contribution;
    }
    out[0] = r > 1.0f ? 1.0f : (r < 0.0f ? 0.0f : r);
    out[1] = g > 1.0f ? 1.0f : (g < 0.0f ? 0.0f : g);
    out[2] = b > 1.0f ? 1.0f : (b < 0.0f ? 0.0f : b);
}

static void SwitchApplyVertexColor(int index, float r, float g, float b,
                                    float a) {
    s_curVertexColor[0] = r;
    s_curVertexColor[1] = g;
    s_curVertexColor[2] = b;
    s_curVertexColor[3] = a;
    s_vColor[index][0] = r * s_vLighting[index][0];
    s_vColor[index][1] = g * s_vLighting[index][1];
    s_vColor[index][2] = b * s_vLighting[index][2];
    s_vColor[index][3] = a * s_vLighting[index][3];
}

void GXSet3DMode(u8 enable) {
    s_3dMode = enable ? TRUE : FALSE;
    if (enable) {
        // HSF's native path supplies material state directly.  Do not let a
        // previous sprite's multi-stage TEV setup leak into the mesh pass.
        GXGLInitTevState();
        s_useMaterialTint = TRUE;
    }
}

// Translate the small set of GX raster states used by HSF materials into the
// GLES2 backend.  The original GX FIFO is not present on Switch, so these
// calls update the backend state consumed by Gfx3D_Draw*.
void GXSetCullMode(GXCullMode mode) {
    Gfx3D_SetCullMode((int)mode);
}

void GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable) {
    Gfx3D_SetDepthMode(compare_enable ? 1 : 0, (int)func,
                       update_enable ? 1 : 0);
}

void GXSetBlendMode(GXBlendMode type, GXBlendFactor src_factor,
                    GXBlendFactor dst_factor, GXLogicOp op) {
    Gfx3D_SetBlendMode((int)type, (int)src_factor, (int)dst_factor,
                       (int)op);
}

void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op,
                       GXCompare comp1, u8 ref1) {
    Gfx3D_SetAlphaCompare((int)comp0, (int)ref0, (int)op,
                          (int)comp1, (int)ref1);
}

void GXBegin(GXPrimitive type, GXVtxFmt fmt, u16 nverts) {
    (void)fmt; (void)nverts;
    GXGLEnsureTevState();
    s_prim = type;
    s_vCount = 0;
    s_curVertexColor[0] = 1.0f;
    s_curVertexColor[1] = 1.0f;
    s_curVertexColor[2] = 1.0f;
    s_curVertexColor[3] = 1.0f;
}

// Per-frame bounds of transformed geometry, in normalized device coordinates.
// Anything visible must land inside -1..1 on x and y, so these numbers say
// immediately whether the camera/projection math is putting geometry on screen.
static float s_dbgTint[4];
static int s_dbgStages = 0;
static unsigned int s_dbgTexDraws = 0;
static unsigned int s_dbgNoTexDraws = 0;
static unsigned int s_dbgEmptyDraws = 0;

void GXGLReportFrameStats(void) {
    OSReport("GXGL: texDraws=%u solidDraws=%u tint[%.2f %.2f %.2f %.2f] stages=%d\n",
             s_dbgTexDraws, s_dbgNoTexDraws, s_dbgTint[0], s_dbgTint[1],
             s_dbgTint[2], s_dbgTint[3], s_dbgStages);
    {
        extern unsigned int g_hsfTexCalls, g_hsfTexReflect, g_hsfTexNoAttr,
                            g_hsfTexNoBmp, g_hsfTexLoaded;
        OSReport("HSFtex: calls=%u reflect=%u noAttr=%u noBmp=%u loaded=%u tevMap0=%d\n",
                 g_hsfTexCalls, g_hsfTexReflect, g_hsfTexNoAttr, g_hsfTexNoBmp,
                 g_hsfTexLoaded, s_tevState.stages[0].texMap);
        g_hsfTexCalls = g_hsfTexReflect = g_hsfTexNoAttr = 0;
        g_hsfTexNoBmp = g_hsfTexLoaded = 0;
    }
    OSReport("TexCache: hit=%u miss=%u upload=%u avgProbe=%u entries=%d\n",
             g_texHit, g_texMiss, g_texUpload,
             g_texHit ? g_texProbe / g_texHit : 0, s_texCacheN);
    g_texHit = g_texMiss = g_texUpload = g_texProbe = 0;
    s_dbgTexDraws = 0;
    s_dbgNoTexDraws = 0;
    s_dbgEmptyDraws = 0;
}

void GXPosition3f32(f32 x, f32 y, f32 z) {
    if (s_vCount >= MAXV) return;
    // world = posMtx * (x,y,z,1)
    float ox = s_pos[0][0]*x + s_pos[0][1]*y + s_pos[0][2]*z + s_pos[0][3];
    float oy = s_pos[1][0]*x + s_pos[1][1]*y + s_pos[1][2]*z + s_pos[1][3];
    float oz = s_pos[2][0]*x + s_pos[2][1]*y + s_pos[2][2]*z + s_pos[2][3];
    // clip = proj * world
    float cx = s_proj[0][0]*ox + s_proj[0][1]*oy + s_proj[0][2]*oz + s_proj[0][3];
    float cy = s_proj[1][0]*ox + s_proj[1][1]*oy + s_proj[1][2]*oz + s_proj[1][3];
    float cz = s_proj[2][0]*ox + s_proj[2][1]*oy + s_proj[2][2]*oz + s_proj[2][3];
    float cw = s_proj[3][0]*ox + s_proj[3][1]*oy + s_proj[3][2]*oz + s_proj[3][3];
    if (cw == 0.0f) cw = 1.0f;
    s_vClip[s_vCount][0] = cx / cw;
    s_vClip[s_vCount][1] = cy / cw;
    s_vClip[s_vCount][2] = cz / cw;
    s_vUV[s_vCount][0] = 0.0f;
    s_vUV[s_vCount][1] = 0.0f;
    SwitchComputeLighting(ox, oy, oz, s_vLighting[s_vCount]);
    s_vColor[s_vCount][0] = s_curVertexColor[0] * s_vLighting[s_vCount][0];
    s_vColor[s_vCount][1] = s_curVertexColor[1] * s_vLighting[s_vCount][1];
    s_vColor[s_vCount][2] = s_curVertexColor[2] * s_vLighting[s_vCount][2];
    s_vColor[s_vCount][3] = s_curVertexColor[3] * s_vLighting[s_vCount][3];
    s_vCount++;
}

void GXPosition2f32(f32 x, f32 y) { GXPosition3f32(x, y, 0.0f); }
void GXPosition2u16(u16 x, u16 y) { GXPosition3f32((f32)x, (f32)y, 0.0f); }
void GXPosition2s16(s16 x, s16 y) { GXPosition3f32((f32)x, (f32)y, 0.0f); }
void GXPosition2u8(u8 x, u8 y) { GXPosition3f32((f32)x, (f32)y, 0.0f); }
void GXPosition2s8(s8 x, s8 y) { GXPosition3f32((f32)x, (f32)y, 0.0f); }
void GXPosition3u16(u16 x, u16 y, u16 z) { GXPosition3f32((f32)x, (f32)y, (f32)z); }
void GXPosition3s16(s16 x, s16 y, s16 z) { GXPosition3f32((f32)x, (f32)y, (f32)z); }
void GXPosition3u8(u8 x, u8 y, u8 z) { GXPosition3f32((f32)x, (f32)y, (f32)z); }
void GXPosition3s8(s8 x, s8 y, s8 z) { GXPosition3f32((f32)x, (f32)y, (f32)z); }

void GXNormal3f32(f32 x, f32 y, f32 z) {
    s_curNormal[0] = x;
    s_curNormal[1] = y;
    s_curNormal[2] = z;
}

void GXNormal3s16(s16 x, s16 y, s16 z) {
    GXNormal3f32((f32)x / 256.0f, (f32)y / 256.0f, (f32)z / 256.0f);
}

void GXNormal3s8(s8 x, s8 y, s8 z) {
    GXNormal3f32((f32)x, (f32)y, (f32)z);
}

void GXNormal1x16(u16 index) { (void)index; }
void GXNormal1x8(u8 index) { (void)index; }

void GXColor4u8(u8 r, u8 g, u8 b, u8 a) {
    if (s_vCount == 0 || s_vCount > MAXV) return;
    SwitchApplyVertexColor(s_vCount - 1, r / 255.0f, g / 255.0f,
                           b / 255.0f, a / 255.0f);
}

void GXColor3u8(u8 r, u8 g, u8 b) { GXColor4u8(r, g, b, 255); }
void GXColor1u32(u32 color) {
    GXColor4u8((u8)(color >> 24), (u8)(color >> 16),
               (u8)(color >> 8), (u8)color);
}
void GXColor1u16(u16 color) { (void)color; }
void GXColor1x16(u16 index) { (void)index; }
void GXColor1x8(u8 index) { (void)index; }

void GXTexCoord2f32(f32 s, f32 t) {
    if (s_vCount == 0 || s_vCount > MAXV) return;
    s_vUV[s_vCount - 1][0] = s;
    s_vUV[s_vCount - 1][1] = t;
}

void GXTexCoord2u16(u16 s, u16 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord2s16(s16 s, s16 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord2u8(u8 s, u8 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord2s8(s8 s, s8 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord1f32(f32 s, f32 t) { GXTexCoord2f32(s, t); }
void GXTexCoord1u16(u16 s, u16 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord1s16(s16 s, s16 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord1u8(u8 s, u8 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord1s8(s8 s, s8 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord1x16(u16 index) { (void)index; }
void GXTexCoord1x8(u8 index) { (void)index; }

void GXEnd(void) {
    int minimum = (s_prim == GX_POINTS) ? 1 :
                  ((s_prim == GX_LINES || s_prim == GX_LINESTRIP) ? 2 : 3);
    if (s_vCount < minimum) { s_vCount = 0; return; }
    // Expand quads (0,1,2,3) into two triangles (0,1,2)(0,2,3).
    float clipXY[MAXV * 3 * 2];
    float clipXYZ[MAXV * 3 * 3];
    float uv[MAXV * 3 * 2];
    float color[MAXV * 3 * 4];

    if (s_prim == GX_LINES || s_prim == GX_LINESTRIP || s_prim == GX_POINTS) {
        for (int i = 0; i < s_vCount; i++) {
            clipXY[i * 2 + 0] = s_vClip[i][0];
            clipXY[i * 2 + 1] = s_vClip[i][1];
            clipXYZ[i * 3 + 0] = s_vClip[i][0];
            clipXYZ[i * 3 + 1] = s_vClip[i][1];
            clipXYZ[i * 3 + 2] = s_vClip[i][2];
            uv[i * 2 + 0] = s_vUV[i][0];
            uv[i * 2 + 1] = s_vUV[i][1];
            color[i * 4 + 0] = s_vColor[i][0];
            color[i * 4 + 1] = s_vColor[i][1];
            color[i * 4 + 2] = s_vColor[i][2];
            color[i * 4 + 3] = s_vColor[i][3];
        }
        GXGLPushTevState();
        s_dbgTint[0] = s_tint[0];
        s_dbgTint[1] = s_tint[1];
        s_dbgTint[2] = s_tint[2];
        s_dbgTint[3] = s_tint[3];
        s_dbgStages = s_tevState.numStages;
        float drawTint[4] = {
            s_useMaterialTint ? s_tint[0] : 1.0f,
            s_useMaterialTint ? s_tint[1] : 1.0f,
            s_useMaterialTint ? s_tint[2] : 1.0f,
            s_useMaterialTint ? s_tint[3] : 1.0f
        };
        unsigned int drawTex = 0;
        int map = s_tevState.stages[0].texMap;
        if (map >= GX_TEXMAP0 && map <= GX_TEXMAP7) drawTex = s_texMaps[map];
        if (drawTex) s_dbgTexDraws++; else s_dbgNoTexDraws++;
        int primitive = s_prim == GX_LINESTRIP ? 1 :
                        (s_prim == GX_POINTS ? 2 : 0);
        if (drawTex) {
            if (s_3dMode) {
                Gfx3D_DrawTexGeometry(clipXYZ, uv, color, s_vCount,
                                      primitive, drawTex, drawTint[0],
                                      drawTint[1], drawTint[2], drawTint[3]);
            } else {
                Gfx2D_DrawTexGeometry(clipXY, uv, color, s_vCount,
                                      primitive, drawTex, drawTint[0],
                                      drawTint[1], drawTint[2], drawTint[3]);
            }
        } else if (s_3dMode) {
            Gfx3D_DrawSolidGeometry(clipXYZ, color, s_vCount, primitive,
                                    drawTint[0], drawTint[1], drawTint[2],
                                    drawTint[3]);
        } else {
            Gfx2D_DrawSolidGeometry(clipXY, color, s_vCount, primitive,
                                    drawTint[0], drawTint[1], drawTint[2],
                                    drawTint[3]);
        }
        s_vCount = 0;
        return;
    }

    int n = 0;
    if (s_prim == GX_QUADS) {
        for (int q = 0; q + 3 < s_vCount; q += 4) {
            int idx[6] = { q, q+1, q+2, q, q+2, q+3 };
            for (int j = 0; j < 6; j++) {
                clipXY[n*2+0] = s_vClip[idx[j]][0]; clipXY[n*2+1] = s_vClip[idx[j]][1];
                clipXYZ[n*3+0] = s_vClip[idx[j]][0];
                clipXYZ[n*3+1] = s_vClip[idx[j]][1];
                clipXYZ[n*3+2] = s_vClip[idx[j]][2];
                uv[n*2+0] = s_vUV[idx[j]][0];     uv[n*2+1] = s_vUV[idx[j]][1];
                color[n*4+0] = s_vColor[idx[j]][0];
                color[n*4+1] = s_vColor[idx[j]][1];
                color[n*4+2] = s_vColor[idx[j]][2];
                color[n*4+3] = s_vColor[idx[j]][3];
                n++;
            }
        }
    } else if (s_prim == GX_TRIANGLES) {
        for (int q = 0; q + 2 < s_vCount; q += 3) {
            for (int k = 0; k < 3; k++) {
                clipXY[n*2+0] = s_vClip[q+k][0]; clipXY[n*2+1] = s_vClip[q+k][1];
                clipXYZ[n*3+0] = s_vClip[q+k][0];
                clipXYZ[n*3+1] = s_vClip[q+k][1];
                clipXYZ[n*3+2] = s_vClip[q+k][2];
                uv[n*2+0] = s_vUV[q+k][0];     uv[n*2+1] = s_vUV[q+k][1];
                color[n*4+0] = s_vColor[q+k][0];
                color[n*4+1] = s_vColor[q+k][1];
                color[n*4+2] = s_vColor[q+k][2];
                color[n*4+3] = s_vColor[q+k][3];
                n++;
            }
        }
    } else if (s_prim == GX_TRIANGLESTRIP) {
        for (int j = 0; j + 2 < s_vCount; j++) {
            int idx[3];
            if (j & 1) {
                idx[0] = j + 1; idx[1] = j; idx[2] = j + 2;
            } else {
                idx[0] = j; idx[1] = j + 1; idx[2] = j + 2;
            }
            for (int k = 0; k < 3; k++) {
                clipXY[n*2+0] = s_vClip[idx[k]][0]; clipXY[n*2+1] = s_vClip[idx[k]][1];
                clipXYZ[n*3+0] = s_vClip[idx[k]][0];
                clipXYZ[n*3+1] = s_vClip[idx[k]][1];
                clipXYZ[n*3+2] = s_vClip[idx[k]][2];
                uv[n*2+0] = s_vUV[idx[k]][0];     uv[n*2+1] = s_vUV[idx[k]][1];
                color[n*4+0] = s_vColor[idx[k]][0];
                color[n*4+1] = s_vColor[idx[k]][1];
                color[n*4+2] = s_vColor[idx[k]][2];
                color[n*4+3] = s_vColor[idx[k]][3];
                n++;
            }
        }
    } else {  // treat other unsupported primitives as a fan from vertex 0
        for (int j = 1; j + 1 < s_vCount; j++) {
            int idx[3] = { 0, j, j+1 };
            for (int k = 0; k < 3; k++) {
                clipXY[n*2+0] = s_vClip[idx[k]][0]; clipXY[n*2+1] = s_vClip[idx[k]][1];
                clipXYZ[n*3+0] = s_vClip[idx[k]][0];
                clipXYZ[n*3+1] = s_vClip[idx[k]][1];
                clipXYZ[n*3+2] = s_vClip[idx[k]][2];
                uv[n*2+0] = s_vUV[idx[k]][0];     uv[n*2+1] = s_vUV[idx[k]][1];
                color[n*4+0] = s_vColor[idx[k]][0];
                color[n*4+1] = s_vColor[idx[k]][1];
                color[n*4+2] = s_vColor[idx[k]][2];
                color[n*4+3] = s_vColor[idx[k]][3];
                n++;
            }
        }
    }
    GXGLPushTevState();
    float drawTint[4] = {
        s_useMaterialTint ? s_tint[0] : 1.0f,
        s_useMaterialTint ? s_tint[1] : 1.0f,
        s_useMaterialTint ? s_tint[2] : 1.0f,
        s_useMaterialTint ? s_tint[3] : 1.0f
    };
    unsigned int drawTex = 0;
    if (s_tevState.numStages > 0) {
        int map = s_tevState.stages[0].texMap;
        if (map >= GX_TEXMAP0 && map <= GX_TEXMAP7) {
            drawTex = s_texMaps[map];
        }
    }
    s_dbgTint[0] = drawTint[0];
    s_dbgTint[1] = drawTint[1];
    s_dbgTint[2] = drawTint[2];
    s_dbgTint[3] = drawTint[3];
    s_dbgStages = s_tevState.numStages;
    if (drawTex) s_dbgTexDraws++; else s_dbgNoTexDraws++;
    if (n == 0) s_dbgEmptyDraws++;
    if (drawTex) {
        if (s_3dMode) {
            Gfx3D_DrawTexTris(clipXYZ, uv, color, n, drawTex,
                              drawTint[0], drawTint[1], drawTint[2], drawTint[3]);
        } else {
            Gfx2D_DrawTexTris(clipXY, uv, n, color, drawTex,
                              drawTint[0], drawTint[1], drawTint[2], drawTint[3]);
        }
    } else {
        if (s_3dMode) {
            Gfx3D_DrawSolidTris(clipXYZ, color, n,
                                drawTint[0], drawTint[1], drawTint[2], drawTint[3]);
        } else {
            Gfx2D_DrawSolidTris(clipXY, color, n,
                                drawTint[0], drawTint[1], drawTint[2], drawTint[3]);
        }
    }
    s_vCount = 0;
}

static u8 s_numTexGens = 0;
static GXAttrType s_vtxDesc[GX_VA_MAX_ATTR];
static GXCompCnt s_vtxCnt[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];
static GXCompType s_vtxType[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];
static u8 s_vtxFrac[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];
static GXTexGenType s_texGenFunc[GX_MAX_TEXCOORD];
static GXTexGenSrc s_texGenSrc[GX_MAX_TEXCOORD];

void GXSetViewport(f32 left, f32 top, f32 width, f32 height,
                   f32 nearZ, f32 farZ) {
    GfxSetViewport(left, top, width, height, nearZ, farZ);
}

void GXSetViewportJitter(f32 left, f32 top, f32 width, f32 height,
                         f32 nearZ, f32 farZ, u32 field) {
    (void)field;
    GfxSetViewport(left, top, width, height, nearZ, farZ);
}

void GXSetScissor(u32 left, u32 top, u32 width, u32 height) {
    GfxSetScissor(left, top, width, height);
}

void GXClearVtxDesc(void) {
    memset(s_vtxDesc, 0, sizeof(s_vtxDesc));
}

void GXSetVtxDesc(GXAttr attr, GXAttrType type) {
    if (attr < GX_VA_MAX_ATTR) s_vtxDesc[attr] = type;
}

void GXSetVtxDescv(GXVtxDescList* list) {
    if (!list) return;
    while (list->attr != GX_VA_NULL) {
        GXSetVtxDesc(list->attr, list->type);
        list++;
    }
}

void GXSetVtxAttrFmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt,
                     GXCompType type, u8 frac) {
    if (fmt >= GX_MAX_VTXFMT || attr >= GX_VA_MAX_ATTR) return;
    s_vtxCnt[fmt][attr] = cnt;
    s_vtxType[fmt][attr] = type;
    s_vtxFrac[fmt][attr] = frac;
}

void GXSetNumTexGens(u8 num) {
    s_numTexGens = num > GX_MAX_TEXCOORD ? GX_MAX_TEXCOORD : num;
}

void GXSetTexCoordGen2(GXTexCoordID coord, GXTexGenType func,
                       GXTexGenSrc src, u32 mtx, GXBool normalize,
                       u32 postMtx) {
    (void)mtx;
    (void)normalize;
    (void)postMtx;
    if (coord >= GX_MAX_TEXCOORD) return;
    s_texGenFunc[coord] = func;
    s_texGenSrc[coord] = src;
}

void GXSetLineWidth(u8 width, GXTexOffset texOffsets) {
    (void)width;
    (void)texOffsets;
}

void GXSetPointSize(u8 pointSize, GXTexOffset texOffsets) {
    (void)pointSize;
    (void)texOffsets;
}

void GXEnableTexOffsets(GXTexCoordID coord, GXBool lineEnable,
                        GXBool pointEnable) {
    (void)coord;
    (void)lineEnable;
    (void)pointEnable;
}

void GXSetTevColor(GXTevRegID id, GXColor color) {
    GXGLEnsureTevState();
    if (id >= GX_TEVREG0 && id <= GX_TEVREG2) {
        float* dst = s_tevState.regs[id - GX_TEVREG0];
        dst[0] = color.r / 255.0f;
        dst[1] = color.g / 255.0f;
        dst[2] = color.b / 255.0f;
        dst[3] = color.a / 255.0f;
    }
}

void GXSetTevColorS10(GXTevRegID id, GXColorS10 color) {
    GXGLEnsureTevState();
    if (id >= GX_TEVREG0 && id <= GX_TEVREG2) {
        float* dst = s_tevState.regs[id - GX_TEVREG0];
        dst[0] = color.r / 1023.0f;
        dst[1] = color.g / 1023.0f;
        dst[2] = color.b / 1023.0f;
        dst[3] = color.a / 1023.0f;
    }
}

void GXSetTevKColor(GXTevKColorID id, GXColor color) {
    GXGLEnsureTevState();
    if (id < GX_MAX_KCOLOR) {
        s_tevState.kColors[id][0] = color.r / 255.0f;
        s_tevState.kColors[id][1] = color.g / 255.0f;
        s_tevState.kColors[id][2] = color.b / 255.0f;
        s_tevState.kColors[id][3] = color.a / 255.0f;
    }
}

void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) {
    GXGLEnsureTevState();
    if (stage < GFX_TEV_MAX_STAGES) s_tevState.stages[stage].kColorSel = sel;
}

void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) {
    GXGLEnsureTevState();
    if (stage < GFX_TEV_MAX_STAGES) s_tevState.stages[stage].kAlphaSel = sel;
}

void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel rasSel,
                      GXTevSwapSel texSel) {
    (void)stage;
    (void)rasSel;
    (void)texSel;
}

void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red,
                           GXTevColorChan green, GXTevColorChan blue,
                           GXTevColorChan alpha) {
    (void)table;
    (void)red;
    (void)green;
    (void)blue;
    (void)alpha;
}

void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b,
                     GXTevColorArg c, GXTevColorArg d) {
    GXGLEnsureTevState();
    if (stage >= GFX_TEV_MAX_STAGES) return;
    s_tevState.stages[stage].colorIn[0] = a;
    s_tevState.stages[stage].colorIn[1] = b;
    s_tevState.stages[stage].colorIn[2] = c;
    s_tevState.stages[stage].colorIn[3] = d;
}

void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b,
                     GXTevAlphaArg c, GXTevAlphaArg d) {
    GXGLEnsureTevState();
    if (stage >= GFX_TEV_MAX_STAGES) return;
    s_tevState.stages[stage].alphaIn[0] = a;
    s_tevState.stages[stage].alphaIn[1] = b;
    s_tevState.stages[stage].alphaIn[2] = c;
    s_tevState.stages[stage].alphaIn[3] = d;
}

void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                     GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    GXGLEnsureTevState();
    if (stage >= GFX_TEV_MAX_STAGES) return;
    s_tevState.stages[stage].colorOp = op;
    s_tevState.stages[stage].colorBias = bias;
    s_tevState.stages[stage].colorScale = scale;
    s_tevState.stages[stage].colorClamp = clamp ? 1 : 0;
    s_tevState.stages[stage].colorOut = outReg;
}

void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                     GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    GXGLEnsureTevState();
    if (stage >= GFX_TEV_MAX_STAGES) return;
    s_tevState.stages[stage].alphaOp = op;
    s_tevState.stages[stage].alphaBias = bias;
    s_tevState.stages[stage].alphaScale = scale;
    s_tevState.stages[stage].alphaClamp = clamp ? 1 : 0;
    s_tevState.stages[stage].alphaOut = outReg;
}

void GXSetTevOp(GXTevStageID stage, GXTevMode mode) {
    GXTevColorArg carg = stage == GX_TEVSTAGE0 ? GX_CC_RASC : GX_CC_CPREV;
    GXTevAlphaArg aarg = stage == GX_TEVSTAGE0 ? GX_CA_RASA : GX_CA_APREV;
    switch (mode) {
        case GX_MODULATE:
            GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_TEXC, carg, GX_CC_ZERO);
            GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO);
            break;
        case GX_DECAL:
            GXSetTevColorIn(stage, carg, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
            GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg);
            break;
        case GX_BLEND:
            GXSetTevColorIn(stage, carg, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
            GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO);
            break;
        case GX_REPLACE:
            GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
            GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
            break;
        case GX_PASSCLR:
        default:
            GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, carg);
            GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg);
            break;
    }
    GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                    GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                    GX_TRUE, GX_TEVPREV);
}

void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord,
                   GXTexMapID map, GXChannelID color) {
    GXGLEnsureTevState();
    if (stage >= GFX_TEV_MAX_STAGES) return;
    s_tevState.stages[stage].texMap = map;
    s_tevState.stages[stage].channel = color;
    (void)coord;
}

void GXSetNumTevStages(u8 num) {
    GXGLEnsureTevState();
    s_tevState.numStages = num == 0 ? 1 : num;
}

void GXSetTevDirect(GXTevStageID stage) {
    (void)stage;
}

void GXSetNumIndStages(u8 num) {
    (void)num;
}

void GXSetIndTexCoordScale(GXIndTexStageID stage, GXIndTexScale s,
                           GXIndTexScale t) {
    (void)stage;
    (void)s;
    (void)t;
}

void GXSetIndTexOrder(GXIndTexStageID stage, GXTexCoordID coord,
                      GXTexMapID map) {
    (void)stage;
    (void)coord;
    (void)map;
}

void GXSetTevIndTile(GXTevStageID stage, GXIndTexStageID indStage,
                     u16 w, u16 h, u16 tw, u16 th, GXIndTexFormat fmt,
                     GXIndTexMtxID mtx, GXIndTexBiasSel bias,
                     GXIndTexAlphaSel alpha) {
    (void)stage;
    (void)indStage;
    (void)w;
    (void)h;
    (void)tw;
    (void)th;
    (void)fmt;
    (void)mtx;
    (void)bias;
    (void)alpha;
}

void GXSetTexCoordScaleManually(GXTexCoordID coord, u8 enable,
                                u16 scaleS, u16 scaleT) {
    (void)coord;
    (void)enable;
    (void)scaleS;
    (void)scaleT;
}

void GXSetNumChans(u8 num) {
    s_numChans = num > 2 ? 2 : num;
}

void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc ambSrc,
                   GXColorSrc matSrc, u32 lightMask, GXDiffuseFn diffFn,
                   GXAttnFn attnFn) {
    int index;
    if (chan == GX_COLOR0 || chan == GX_ALPHA0 || chan == GX_COLOR0A0) {
        index = 0;
        s_useMaterialTint = matSrc == GX_SRC_REG ? TRUE : FALSE;
    } else if (chan == GX_COLOR1 || chan == GX_ALPHA1 || chan == GX_COLOR1A1) {
        index = 1;
    } else {
        return;
    }
    s_chanState[index].enable = enable;
    s_chanState[index].ambSrc = ambSrc;
    s_chanState[index].matSrc = matSrc;
    s_chanState[index].lightMask = lightMask;
    s_chanState[index].diffFn = diffFn;
    s_chanState[index].attnFn = attnFn;
}

void GXSetChanAmbColor(GXChannelID chan, GXColor color) {
    if (chan == GX_COLOR1 || chan == GX_ALPHA1 || chan == GX_COLOR1A1) {
        s_chanAmb[1] = color;
    } else if (chan == GX_COLOR0 || chan == GX_ALPHA0 ||
               chan == GX_COLOR0A0) {
        s_chanAmb[0] = color;
    }
}

void GXSetZCompLoc(GXBool beforeTex) {
    (void)beforeTex;
}

void GXSetColorUpdate(GXBool enable) {
    Gfx3D_SetColorUpdate(enable ? 1 : 0);
}

void GXSetAlphaUpdate(GXBool enable) {
    Gfx3D_SetAlphaUpdate(enable ? 1 : 0);
}

// Capture the material color as our draw tint (texture is modulated by it).
void GXSetChanMatColor(GXChannelID id, GXColor c) {
    int index;
    if (id == GX_COLOR1 || id == GX_ALPHA1 || id == GX_COLOR1A1) {
        index = 1;
    } else {
        index = 0;
    }
    s_chanMat[index] = c;
    if (index == 0) {
        s_tint[0] = c.r / 255.0f; s_tint[1] = c.g / 255.0f;
        s_tint[2] = c.b / 255.0f; s_tint[3] = c.a / 255.0f;
    }
}

/* GXLight.c writes these fields into the GameCube FIFO.  The Switch build
 * keeps the same 64-byte object layout, then stores a decoded copy when the
 * light is loaded. */
static SwitchGXLightRaw *SwitchLightObject(GXLightObj *object) {
    int i;
    if (!object) return NULL;
    for (i = 0; i < s_lightObjectCount; i++) {
        if (s_lightObjectKeys[i] == object) return (SwitchGXLightRaw *)object;
    }
    if (s_lightObjectCount < SWITCH_GX_LIGHT_OBJECT_MAX) {
        s_lightObjectKeys[s_lightObjectCount++] = object;
        memset(object, 0, sizeof(*object));
        ((SwitchGXLightRaw *)object)->color = 0xFFFFFFFFU;
        ((SwitchGXLightRaw *)object)->a[0] = 1.0f;
        ((SwitchGXLightRaw *)object)->k[0] = 1.0f;
    }
    return (SwitchGXLightRaw *)object;
}

void GXInitLightAttn(GXLightObj *object, f32 a0, f32 a1, f32 a2,
                     f32 k0, f32 k1, f32 k2) {
    SwitchGXLightRaw *light = SwitchLightObject(object);
    if (!light) return;
    light->a[0] = a0; light->a[1] = a1; light->a[2] = a2;
    light->k[0] = k0; light->k[1] = k1; light->k[2] = k2;
}

void GXInitLightAttnA(GXLightObj *object, f32 a0, f32 a1, f32 a2) {
    SwitchGXLightRaw *light = SwitchLightObject(object);
    if (!light) return;
    light->a[0] = a0; light->a[1] = a1; light->a[2] = a2;
}

void GXInitLightAttnK(GXLightObj *object, f32 k0, f32 k1, f32 k2) {
    SwitchGXLightRaw *light = SwitchLightObject(object);
    if (!light) return;
    light->k[0] = k0; light->k[1] = k1; light->k[2] = k2;
}

void GXInitLightPos(GXLightObj *object, f32 x, f32 y, f32 z) {
    SwitchGXLightRaw *light = SwitchLightObject(object);
    if (!light) return;
    light->lpos[0] = x; light->lpos[1] = y; light->lpos[2] = z;
    light->ldir[0] = light->ldir[1] = light->ldir[2] = 0.0f;
}

void GXInitLightDir(GXLightObj *object, f32 nx, f32 ny, f32 nz) {
    SwitchGXLightRaw *light = SwitchLightObject(object);
    if (!light) return;
    light->ldir[0] = -nx; light->ldir[1] = -ny; light->ldir[2] = -nz;
    light->lpos[0] = light->lpos[1] = light->lpos[2] = 0.0f;
}

void GXInitLightColor(GXLightObj *object, GXColor color) {
    SwitchGXLightRaw *light = SwitchLightObject(object);
    if (!light) return;
    light->color = ((u32)color.r << 24) | ((u32)color.g << 16) |
                   ((u32)color.b << 8) | color.a;
}

void GXInitLightSpot(GXLightObj *object, f32 cutoff, GXSpotFn spotFunc) {
    SwitchGXLightRaw *light = SwitchLightObject(object);
    float c;
    float d;
    if (!light) return;
    if (cutoff <= 0.0f || cutoff > 90.0f) spotFunc = GX_SP_OFF;
    c = cosf((3.1415926535f * cutoff) / 180.0f);
    switch (spotFunc) {
        case GX_SP_FLAT:
            light->a[0] = -1000.0f * c; light->a[1] = 1000.0f; light->a[2] = 0.0f;
            break;
        case GX_SP_COS:
            light->a[0] = -c / (1.0f - c); light->a[1] = 1.0f / (1.0f - c); light->a[2] = 0.0f;
            break;
        case GX_SP_COS2:
            light->a[0] = 0.0f; light->a[1] = -c / (1.0f - c); light->a[2] = 1.0f / (1.0f - c);
            break;
        case GX_SP_SHARP:
            d = (1.0f - c) * (1.0f - c);
            light->a[0] = c * (c - 2.0f) / d; light->a[1] = 2.0f / d; light->a[2] = -1.0f / d;
            break;
        case GX_SP_RING1:
            d = (1.0f - c) * (1.0f - c);
            light->a[0] = -4.0f * c / d; light->a[1] = 4.0f * (1.0f + c) / d; light->a[2] = -4.0f / d;
            break;
        case GX_SP_RING2:
            d = (1.0f - c) * (1.0f - c);
            light->a[0] = 1.0f - 2.0f * c * c / d; light->a[1] = 4.0f * c / d; light->a[2] = -2.0f / d;
            break;
        case GX_SP_OFF:
        default:
            light->a[0] = 1.0f; light->a[1] = 0.0f; light->a[2] = 0.0f;
            break;
    }
}

void GXInitLightDistAttn(GXLightObj *object, f32 refDistance,
                         f32 refBrightness, GXDistAttnFn distFunc) {
    SwitchGXLightRaw *light = SwitchLightObject(object);
    if (!light) return;
    if (refDistance < 0.0f || refBrightness <= 0.0f ||
        refBrightness >= 1.0f) {
        distFunc = GX_DA_OFF;
    }
    switch (distFunc) {
        case GX_DA_GENTLE:
            light->k[0] = 1.0f;
            light->k[1] = (1.0f - refBrightness) /
                          (refBrightness * refDistance);
            light->k[2] = 0.0f;
            break;
        case GX_DA_MEDIUM:
            light->k[0] = 1.0f;
            light->k[1] = 0.5f * (1.0f - refBrightness) /
                          (refBrightness * refDistance);
            light->k[2] = 0.5f * (1.0f - refBrightness) /
                          (refBrightness * refDistance * refDistance);
            break;
        case GX_DA_STEEP:
            light->k[0] = 1.0f; light->k[1] = 0.0f;
            light->k[2] = (1.0f - refBrightness) /
                          (refBrightness * refDistance * refDistance);
            break;
        case GX_DA_OFF:
        default:
            light->k[0] = 1.0f; light->k[1] = 0.0f; light->k[2] = 0.0f;
            break;
    }
}

void GXLoadLightObjImm(GXLightObj *object, GXLightID lightId) {
    int index = SwitchLightBitIndex((u32)lightId);
    SwitchGXLightRaw *source = SwitchLightObject(object);
    if (!source || index < 0) return;
    memcpy(&s_lights[index], source, sizeof(s_lights[index]));
}

// Remaining GX state functions are kept as loose no-ops in sys_switch.c.

#endif // __SWITCH__
