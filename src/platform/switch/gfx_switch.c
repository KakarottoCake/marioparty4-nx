#ifdef __SWITCH__

#include "gfx_switch.h"

#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>

// EGL objects for the default Switch window.
static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;

// Current clear color, normalized to 0..1 for GL.
static float s_clearR = 0.0f;
static float s_clearG = 0.0f;
static float s_clearB = 0.0f;
static int s_surfaceWidth = 1280;
static int s_surfaceHeight = 720;
static int s_colorUpdate = 1;
static int s_alphaUpdate = 1;
static int s_dbgScX, s_dbgScY, s_dbgScW, s_dbgScH;
static int s_dbgScRaw[4];
static unsigned int s_dbgScEmpty = 0;
static unsigned int s_dbgScCalls = 0;
static int s_dbgScSeen = 0;

// Per-frame draw statistics, reported periodically so the log shows whether
// geometry is actually reaching GL. A macro that mentions its own name is not
// expanded again, so this instruments every call site without renaming them.
static unsigned int s_drawCalls = 0;
static unsigned int s_drawVerts = 0;
#define glDrawArrays(mode, first, count) \
    (s_drawCalls++, s_drawVerts += (unsigned int)(count), \
     glDrawArrays((mode), (first), (count)))

static GfxTevState s_tevState;

typedef struct GfxTevUniforms {
    GLint numStages;
    GLint colorIn[4][4];
    GLint alphaIn[4][4];
    GLint colorOp[4], colorBias[4], colorScale[4], colorClamp[4], colorOut[4];
    GLint alphaOp[4], alphaBias[4], alphaScale[4], alphaClamp[4], alphaOut[4];
    GLint texMap[4], channel[4], kColorSel[4], kAlphaSel[4];
    GLint regs;
    GLint kColors;
    GLint hasTexture;
    GLint tint;
    GLint sampler;
    GLint alphaComp0;
    GLint alphaRef0;
    GLint alphaCompareOp;
    GLint alphaComp1;
    GLint alphaRef1;
} GfxTevUniforms;

// Small GX-compatible state cache for the native 3D path.  These defaults are
// the normal GameCube material settings used by the original renderer.
static int s_3dCullMode = 2;       // GX_CULL_BACK
static int s_3dDepthEnable = 1;
static int s_3dDepthFunc = 3;      // GX_LEQUAL
static int s_3dDepthWrite = 1;
static int s_3dBlendMode = 1;      // GX_BM_BLEND
static int s_3dBlendSrc = 4;       // GX_BL_SRCALPHA
static int s_3dBlendDst = 5;       // GX_BL_INVSRCALPHA
static int s_3dBlendOp = 5;        // GX_LO_NOOP
static int s_3dAlphaComp0 = 6;     // GX_GEQUAL
static float s_3dAlphaRef0 = 1.0f / 255.0f;
static int s_3dAlphaOp = 0;        // GX_AOP_AND
static int s_3dAlphaComp1 = 6;     // GX_GEQUAL
static float s_3dAlphaRef1 = 1.0f / 255.0f;

// --- 2D shader pipeline (foundation of the GX->GL translation layer) ---
static GLuint s_prog2d = 0;
static GLint  s_locPos = -1;    // attribute: vertex position (clip space)
static GLint  s_locColor = -1;  // uniform: solid color
static GLuint s_prog2dTev = 0;
static GLint s_2dTevLocPos = -1, s_2dTevLocColor = -1;
static GfxTevUniforms s_2dTevUniforms;

extern void OSReport(const char* msg, ...);  // engine logger (goes to SD log)

static void GfxInitTex(void);  // defined later (textured pipeline)
static void GfxInit3DTex(void);  // defined later (depth-tested textured path)
static void GfxInit2DTev(void);

void GfxSetTevState(const GfxTevState* state) {
    if (!state) return;
    s_tevState = *state;
    if (s_tevState.numStages < 1) s_tevState.numStages = 1;
    if (s_tevState.numStages > GFX_TEV_MAX_STAGES) {
        s_tevState.numStages = GFX_TEV_MAX_STAGES;
    }
}

static GLenum GfxCompareFunc(int func) {
    switch (func) {
        case 0: return GL_NEVER;
        case 1: return GL_LESS;
        case 2: return GL_EQUAL;
        case 3: return GL_LEQUAL;
        case 4: return GL_GREATER;
        case 5: return GL_NOTEQUAL;
        case 6: return GL_GEQUAL;
        default: return GL_ALWAYS;
    }
}

static GLenum GfxBlendFactor(int factor, int destination) {
    switch (factor) {
        case 0: return GL_ZERO;
        case 1: return GL_ONE;
        case 2: return destination ? GL_DST_COLOR : GL_SRC_COLOR;
        case 3: return destination ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
        case 4: return GL_SRC_ALPHA;
        case 5: return GL_ONE_MINUS_SRC_ALPHA;
        case 6: return GL_DST_ALPHA;
        case 7: return GL_ONE_MINUS_DST_ALPHA;
        default: return GL_ONE;
    }
}

static void GfxApply3DState(void) {
    if (s_3dCullMode == 0) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        if (s_3dCullMode == 1) {
            glCullFace(GL_FRONT);
        } else if (s_3dCullMode == 3) {
            glCullFace(GL_FRONT_AND_BACK);
        } else {
            glCullFace(GL_BACK);
        }
    }

    if (s_3dDepthEnable) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GfxCompareFunc(s_3dDepthFunc));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(s_3dDepthWrite ? GL_TRUE : GL_FALSE);
    glColorMask(s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_alphaUpdate ? GL_TRUE : GL_FALSE);

    if (s_3dBlendMode == 0) {
        glDisable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
    } else {
        glEnable(GL_BLEND);
        glBlendFunc(GfxBlendFactor(s_3dBlendSrc, 0),
                    GfxBlendFactor(s_3dBlendDst, 1));
        glBlendEquation(s_3dBlendMode == 3 ? GL_FUNC_SUBTRACT : GL_FUNC_ADD);
    }
}

static const char* kVert2D =
    "attribute vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char* kVert2DTev =
    "attribute vec2 aPos;\n"
    "attribute vec4 aColor;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vColor;\n"
    "void main() { vUV = vec2(0.0); vColor = aColor; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char* kFrag2D =
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "void main() { gl_FragColor = uColor; }\n";

// HSF's first native draw path uses the same solid-color material fallback as
// the 2D path, but keeps clip-space Z so nearer triangles hide farther ones.
static GLuint s_prog3d = 0;
static GLint s_loc3dPos = -1;
static GLint s_loc3dVertexColor = -1;
static GLint s_loc3dTint = -1;
static GLuint s_dummyTexture = 0;

static const char* kVert3D =
    "attribute vec3 aPos;\n"
    "attribute vec4 aColor;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vColor;\n"
    "void main() { vUV = vec2(0.0); vColor = aColor; gl_Position = vec4(aPos, 1.0); }\n";

static const char* kFrag3DSolid =
    "precision mediump float;\n"
    "varying vec4 vColor;\n"
    "uniform vec4 uTint;\n"
    "void main() { gl_FragColor = vColor * uTint; }\n";

// GX TEV translated to a small fixed GLES2 shader.  This follows Aurora's
// state-driven approach while staying within the Switch port's C/GLES2
// constraints.  Texture maps beyond the selected draw texture are treated as
// white; the state still preserves their stage configuration for future use.
static const char* kFragTev =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vColor;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4 uTint;\n"
    "uniform int uHasTexture;\n"
    "uniform int uTevNumStages;\n"
    "uniform int uTevColorIn[16];\n"
    "uniform int uTevAlphaIn[16];\n"
    "uniform int uTevColorOp[4];\n"
    "uniform int uTevColorBias[4];\n"
    "uniform int uTevColorScale[4];\n"
    "uniform int uTevColorClamp[4];\n"
    "uniform int uTevColorOut[4];\n"
    "uniform int uTevAlphaOp[4];\n"
    "uniform int uTevAlphaBias[4];\n"
    "uniform int uTevAlphaScale[4];\n"
    "uniform int uTevAlphaClamp[4];\n"
    "uniform int uTevAlphaOut[4];\n"
    "uniform int uTevTexMap[4];\n"
    "uniform int uTevChannel[4];\n"
    "uniform int uTevKColorSel[4];\n"
    "uniform int uTevKAlphaSel[4];\n"
    "uniform vec4 uTevRegs[3];\n"
    "uniform vec4 uTevKColors[4];\n"
    "uniform int uAlphaComp0;\n"
    "uniform float uAlphaRef0;\n"
    "uniform int uAlphaOp;\n"
    "uniform int uAlphaComp1;\n"
    "uniform float uAlphaRef1;\n"
    "\n"
    "vec3 tevKColor(int sel) {\n"
    "  if (sel == 0) return vec3(1.0);\n"
    "  if (sel == 1) return vec3(7.0 / 8.0);\n"
    "  if (sel == 2) return vec3(6.0 / 8.0);\n"
    "  if (sel == 3) return vec3(5.0 / 8.0);\n"
    "  if (sel == 4) return vec3(4.0 / 8.0);\n"
    "  if (sel == 5) return vec3(3.0 / 8.0);\n"
    "  if (sel == 6) return vec3(2.0 / 8.0);\n"
    "  if (sel == 7) return vec3(1.0 / 8.0);\n"
    "  if (sel == 12) return uTevKColors[0].rgb;\n"
    "  if (sel == 13) return uTevKColors[1].rgb;\n"
    "  if (sel == 14) return uTevKColors[2].rgb;\n"
    "  if (sel == 15) return uTevKColors[3].rgb;\n"
    "  if (sel >= 16 && sel <= 19) return vec3(uTevKColors[sel - 16].r);\n"
    "  if (sel >= 20 && sel <= 23) return vec3(uTevKColors[sel - 20].g);\n"
    "  if (sel >= 24 && sel <= 27) return vec3(uTevKColors[sel - 24].b);\n"
    "  return vec3(uTevKColors[sel - 28].a);\n"
    "}\n"
    "float tevKAlpha(int sel) {\n"
    "  if (sel == 0) return 1.0;\n"
    "  if (sel == 1) return 7.0 / 8.0;\n"
    "  if (sel == 2) return 6.0 / 8.0;\n"
    "  if (sel == 3) return 5.0 / 8.0;\n"
    "  if (sel == 4) return 4.0 / 8.0;\n"
    "  if (sel == 5) return 3.0 / 8.0;\n"
    "  if (sel == 6) return 2.0 / 8.0;\n"
    "  if (sel == 7) return 1.0 / 8.0;\n"
    "  return uTevKColors[sel - 16].r;\n"
    "}\n"
    "vec3 tevColorArg(int arg, vec4 prev, vec4 r0, vec4 r1, vec4 r2,\n"
    "                 vec4 tex, vec4 ras, int texMap, int channel, int ksel) {\n"
    "  if (arg == 0) return prev.rgb;\n"
    "  if (arg == 1) return vec3(prev.a);\n"
    "  if (arg == 2) return r0.rgb;\n"
    "  if (arg == 3) return vec3(r0.a);\n"
    "  if (arg == 4) return r1.rgb;\n"
    "  if (arg == 5) return vec3(r1.a);\n"
    "  if (arg == 6) return r2.rgb;\n"
    "  if (arg == 7) return vec3(r2.a);\n"
    "  if (arg == 8) return texMap == 255 ? vec3(1.0) : tex.rgb;\n"
    "  if (arg == 9) return texMap == 255 ? vec3(1.0) : vec3(tex.a);\n"
    "  if (arg == 10) return (channel == 6 || channel == 255) ? vec3(0.0) : ras.rgb;\n"
    "  if (arg == 11) return (channel == 6 || channel == 255) ? vec3(0.0) : vec3(ras.a);\n"
    "  if (arg == 12) return vec3(1.0);\n"
    "  if (arg == 13) return vec3(0.5);\n"
    "  if (arg == 14) return tevKColor(ksel);\n"
    "  return vec3(0.0);\n"
    "}\n"
    "float tevAlphaArg(int arg, vec4 prev, vec4 r0, vec4 r1, vec4 r2,\n"
    "                  vec4 tex, vec4 ras, int texMap, int channel, int ksel) {\n"
    "  if (arg == 0) return prev.a;\n"
    "  if (arg == 1) return r0.a;\n"
    "  if (arg == 2) return r1.a;\n"
    "  if (arg == 3) return r2.a;\n"
    "  if (arg == 4) return texMap == 255 ? 1.0 : tex.a;\n"
    "  if (arg == 5) return (channel == 6 || channel == 255) ? 0.0 : ras.a;\n"
    "  if (arg == 6) return tevKAlpha(ksel);\n"
    "  return 0.0;\n"
    "}\n"
    "bool tevCompare(int op, float a, float b) {\n"
    "  if (op == 8 || op == 10 || op == 12 || op == 14) return a > b;\n"
    "  return a == b;\n"
    "}\n"
    "vec3 tevColorOp(int op, vec3 a, vec3 b, vec3 c, vec3 d) {\n"
    "  if (op == 1) return d - mix(a, b, c);\n"
    "  if (op >= 8) return tevCompare(op, a.r, b.r) ? c + d : d;\n"
    "  return mix(a, b, c) + d;\n"
    "}\n"
    "float tevAlphaOp(int op, float a, float b, float c, float d) {\n"
    "  if (op == 1) return d - mix(a, b, c);\n"
    "  if (op >= 8) return tevCompare(op, a, b) ? c + d : d;\n"
    "  return mix(a, b, c) + d;\n"
    "}\n"
    "vec3 tevColorScale(vec3 value, int bias, int scale, int clampValue) {\n"
    "  if (bias == 1) value += vec3(0.5);\n"
    "  else if (bias == 2) value -= vec3(0.5);\n"
    "  if (scale == 1) value *= 2.0;\n"
    "  else if (scale == 2) value *= 4.0;\n"
    "  else if (scale == 3) value *= 0.5;\n"
    "  return clampValue != 0 ? clamp(value, 0.0, 1.0) : clamp(value, -4.0, 4.0);\n"
    "}\n"
    "float tevAlphaScale(float value, int bias, int scale, int clampValue) {\n"
    "  if (bias == 1) value += 0.5;\n"
    "  else if (bias == 2) value -= 0.5;\n"
    "  if (scale == 1) value *= 2.0;\n"
    "  else if (scale == 2) value *= 4.0;\n"
    "  else if (scale == 3) value *= 0.5;\n"
    "  return clampValue != 0 ? clamp(value, 0.0, 1.0) : clamp(value, -4.0, 4.0);\n"
    "}\n"
    "bool alphaCompare(float value, float ref, int func) {\n"
    "  if (func == 0) return false;\n"
    "  if (func == 1) return value < ref;\n"
    "  if (func == 2) return value == ref;\n"
    "  if (func == 3) return value <= ref;\n"
    "  if (func == 4) return value > ref;\n"
    "  if (func == 5) return value != ref;\n"
    "  if (func == 6) return value >= ref;\n"
    "  return true;\n"
    "}\n"
    "void main() {\n"
    "  vec4 tex = uHasTexture != 0 ? texture2D(uTex, vUV) : vec4(1.0);\n"
    "  vec4 ras = vColor * uTint;\n"
    "  vec4 prev = vec4(0.0);\n"
    "  vec4 r0 = uTevRegs[0];\n"
    "  vec4 r1 = uTevRegs[1];\n"
    "  vec4 r2 = uTevRegs[2];\n"
    "  for (int stage = 0; stage < 4; stage++) {\n"
    "    if (stage >= uTevNumStages) break;\n"
    "    vec4 stageTex = uTevTexMap[stage] == 255 ? vec4(1.0) : tex;\n"
    "    vec3 ca = tevColorArg(uTevColorIn[stage * 4 + 0], prev, r0, r1, r2, stageTex, ras, uTevTexMap[stage], uTevChannel[stage], uTevKColorSel[stage]);\n"
    "    vec3 cb = tevColorArg(uTevColorIn[stage * 4 + 1], prev, r0, r1, r2, stageTex, ras, uTevTexMap[stage], uTevChannel[stage], uTevKColorSel[stage]);\n"
    "    vec3 cc = tevColorArg(uTevColorIn[stage * 4 + 2], prev, r0, r1, r2, stageTex, ras, uTevTexMap[stage], uTevChannel[stage], uTevKColorSel[stage]);\n"
    "    vec3 cd = tevColorArg(uTevColorIn[stage * 4 + 3], prev, r0, r1, r2, stageTex, ras, uTevTexMap[stage], uTevChannel[stage], uTevKColorSel[stage]);\n"
    "    vec3 color = tevColorScale(tevColorOp(uTevColorOp[stage], ca, cb, cc, cd), uTevColorBias[stage], uTevColorScale[stage], uTevColorClamp[stage]);\n"
    "    if (uTevColorOut[stage] == 1) r0.rgb = color;\n"
    "    else if (uTevColorOut[stage] == 2) r1.rgb = color;\n"
    "    else if (uTevColorOut[stage] == 3) r2.rgb = color;\n"
    "    else prev.rgb = color;\n"
    "    float aa = tevAlphaArg(uTevAlphaIn[stage * 4 + 0], prev, r0, r1, r2, stageTex, ras, uTevTexMap[stage], uTevChannel[stage], uTevKAlphaSel[stage]);\n"
    "    float ab = tevAlphaArg(uTevAlphaIn[stage * 4 + 1], prev, r0, r1, r2, stageTex, ras, uTevTexMap[stage], uTevChannel[stage], uTevKAlphaSel[stage]);\n"
    "    float ac = tevAlphaArg(uTevAlphaIn[stage * 4 + 2], prev, r0, r1, r2, stageTex, ras, uTevTexMap[stage], uTevChannel[stage], uTevKAlphaSel[stage]);\n"
    "    float ad = tevAlphaArg(uTevAlphaIn[stage * 4 + 3], prev, r0, r1, r2, stageTex, ras, uTevTexMap[stage], uTevChannel[stage], uTevKAlphaSel[stage]);\n"
    "    float alpha = tevAlphaScale(tevAlphaOp(uTevAlphaOp[stage], aa, ab, ac, ad), uTevAlphaBias[stage], uTevAlphaScale[stage], uTevAlphaClamp[stage]);\n"
    "    if (uTevAlphaOut[stage] == 1) r0.a = alpha;\n"
    "    else if (uTevAlphaOut[stage] == 2) r1.a = alpha;\n"
    "    else if (uTevAlphaOut[stage] == 3) r2.a = alpha;\n"
    "    else prev.a = alpha;\n"
    "  }\n"
    "  bool a0 = alphaCompare(prev.a, uAlphaRef0, uAlphaComp0);\n"
    "  bool a1 = alphaCompare(prev.a, uAlphaRef1, uAlphaComp1);\n"
    "  bool pass = (uAlphaOp == 1) ? (a0 || a1) :\n"
    "              ((uAlphaOp == 2) ? (a0 != a1) :\n"
    "              ((uAlphaOp == 3) ? (a0 == a1) : (a0 && a1)));\n"
    "  if (!pass) discard;\n"
    "  gl_FragColor = prev;\n"
    "}\n";

static GLuint s_prog3dTex = 0;
static GLint s_3dTexLocPos = -1, s_3dTexLocUV = -1;
static GLint s_3dTexLocColor = -1;
static GfxTevUniforms s_3dTexUniforms;

static const char* kVert3DTex =
    "attribute vec3 aPos;\n"
    "attribute vec2 aUV;\n"
    "attribute vec4 aColor;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vColor;\n"
    "void main() { vUV = aUV; vColor = aColor; gl_Position = vec4(aPos, 1.0); }\n";

static void GfxInitDummyTexture(void) {
    static const unsigned char white[4] = {255, 255, 255, 255};
    s_dummyTexture = GfxCreateTexture(1, 1, white, 0, 0);
}

static GLuint CompileShader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[512];
        glGetShaderInfoLog(sh, sizeof(info), NULL, info);
        OSReport("Gfx: shader compile failed: %s\n", info);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void GfxCacheTevUniforms(GLuint program, GfxTevUniforms* u) {
    char name[64];
    memset(u, 0xFF, sizeof(*u));
    u->numStages = glGetUniformLocation(program, "uTevNumStages");
    for (int stage = 0; stage < GFX_TEV_MAX_STAGES; stage++) {
        for (int arg = 0; arg < 4; arg++) {
            snprintf(name, sizeof(name), "uTevColorIn[%d]", stage * 4 + arg);
            u->colorIn[stage][arg] = glGetUniformLocation(program, name);
            snprintf(name, sizeof(name), "uTevAlphaIn[%d]", stage * 4 + arg);
            u->alphaIn[stage][arg] = glGetUniformLocation(program, name);
        }
        snprintf(name, sizeof(name), "uTevColorOp[%d]", stage);
        u->colorOp[stage] = glGetUniformLocation(program, name);
        snprintf(name, sizeof(name), "uTevColorBias[%d]", stage);
        u->colorBias[stage] = glGetUniformLocation(program, name);
        snprintf(name, sizeof(name), "uTevColorScale[%d]", stage);
        u->colorScale[stage] = glGetUniformLocation(program, name);
        snprintf(name, sizeof(name), "uTevColorClamp[%d]", stage);
        u->colorClamp[stage] = glGetUniformLocation(program, name);
        snprintf(name, sizeof(name), "uTevColorOut[%d]", stage);
        u->colorOut[stage] = glGetUniformLocation(program, name);
        snprintf(name, sizeof(name), "uTevAlphaOp[%d]", stage);
        u->alphaOp[stage] = glGetUniformLocation(program, name);
        snprintf(name, sizeof(name), "uTevAlphaBias[%d]", stage);
        u->alphaBias[stage] = glGetUniformLocation(program, name);
        snprintf(name, sizeof(name), "uTevAlphaScale[%d]", stage);
        u->alphaScale[stage] = glGetUniformLocation(program, name);
        snprintf(name, sizeof(name), "uTevAlphaClamp[%d]", stage);
        u->alphaClamp[stage] = glGetUniformLocation(program, name);
        snprintf(name, sizeof(name), "uTevAlphaOut[%d]", stage);
        u->alphaOut[stage] = glGetUniformLocation(program, name);
        snprintf(name, sizeof(name), "uTevTexMap[%d]", stage);
        u->texMap[stage] = glGetUniformLocation(program, name);
        snprintf(name, sizeof(name), "uTevChannel[%d]", stage);
        u->channel[stage] = glGetUniformLocation(program, name);
        snprintf(name, sizeof(name), "uTevKColorSel[%d]", stage);
        u->kColorSel[stage] = glGetUniformLocation(program, name);
        snprintf(name, sizeof(name), "uTevKAlphaSel[%d]", stage);
        u->kAlphaSel[stage] = glGetUniformLocation(program, name);
    }
    u->regs = glGetUniformLocation(program, "uTevRegs[0]");
    u->kColors = glGetUniformLocation(program, "uTevKColors[0]");
    u->hasTexture = glGetUniformLocation(program, "uHasTexture");
    u->tint = glGetUniformLocation(program, "uTint");
    u->sampler = glGetUniformLocation(program, "uTex");
    u->alphaComp0 = glGetUniformLocation(program, "uAlphaComp0");
    u->alphaRef0 = glGetUniformLocation(program, "uAlphaRef0");
    u->alphaCompareOp = glGetUniformLocation(program, "uAlphaOp");
    u->alphaComp1 = glGetUniformLocation(program, "uAlphaComp1");
    u->alphaRef1 = glGetUniformLocation(program, "uAlphaRef1");
}

static void GfxUploadTev(const GfxTevUniforms* u, int hasTexture,
                          int directSolid, float r, float g, float b, float a) {
    if (!u) return;
    glUniform1i(u->numStages, directSolid ? 1 : s_tevState.numStages);
    for (int stage = 0; stage < GFX_TEV_MAX_STAGES; stage++) {
        GfxTevStageState solidStage;
        const GfxTevStageState* s = &s_tevState.stages[stage];
        if (directSolid && stage == 0) {
            solidStage = *s;
            /* GX_CC_ZERO/RASC/ONE and GX_CA_RASA/ZERO.  This is the
             * fixed one-stage equivalent of outputting the material tint. */
            solidStage.colorIn[0] = 15;
            solidStage.colorIn[1] = 10;
            solidStage.colorIn[2] = 12;
            solidStage.colorIn[3] = 15;
            solidStage.alphaIn[0] = 5;
            solidStage.alphaIn[1] = 5;
            solidStage.alphaIn[2] = 7;
            solidStage.alphaIn[3] = 7;
            solidStage.colorOp = 0;
            solidStage.colorBias = 0;
            solidStage.colorScale = 0;
            solidStage.colorClamp = 1;
            solidStage.colorOut = 0;
            solidStage.alphaOp = 0;
            solidStage.alphaBias = 0;
            solidStage.alphaScale = 0;
            solidStage.alphaClamp = 1;
            solidStage.alphaOut = 0;
            solidStage.texMap = 255;
            /* GX_COLOR0A0: the shader uses this to expose raster color. */
            solidStage.channel = 0;
            s = &solidStage;
        }
        for (int arg = 0; arg < 4; arg++) {
            glUniform1i(u->colorIn[stage][arg], s->colorIn[arg]);
            glUniform1i(u->alphaIn[stage][arg], s->alphaIn[arg]);
        }
        glUniform1i(u->colorOp[stage], s->colorOp);
        glUniform1i(u->colorBias[stage], s->colorBias);
        glUniform1i(u->colorScale[stage], s->colorScale);
        glUniform1i(u->colorClamp[stage], s->colorClamp);
        glUniform1i(u->colorOut[stage], s->colorOut);
        glUniform1i(u->alphaOp[stage], s->alphaOp);
        glUniform1i(u->alphaBias[stage], s->alphaBias);
        glUniform1i(u->alphaScale[stage], s->alphaScale);
        glUniform1i(u->alphaClamp[stage], s->alphaClamp);
        glUniform1i(u->alphaOut[stage], s->alphaOut);
        glUniform1i(u->texMap[stage], s->texMap);
        glUniform1i(u->channel[stage], s->channel);
        glUniform1i(u->kColorSel[stage], s->kColorSel);
        glUniform1i(u->kAlphaSel[stage], s->kAlphaSel);
    }
    glUniform4fv(u->regs, 3, &s_tevState.regs[0][0]);
    glUniform4fv(u->kColors, 4, &s_tevState.kColors[0][0]);
    glUniform1i(u->hasTexture, hasTexture ? 1 : 0);
    glUniform4f(u->tint, r, g, b, a);
    glUniform1i(u->alphaComp0, s_3dAlphaComp0);
    glUniform1f(u->alphaRef0, s_3dAlphaRef0);
    glUniform1i(u->alphaCompareOp, s_3dAlphaOp);
    glUniform1i(u->alphaComp1, s_3dAlphaComp1);
    glUniform1f(u->alphaRef1, s_3dAlphaRef1);
}

static void GfxInit2D(void) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVert2D);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFrag2D);
    if (!vs || !fs) return;
    s_prog2d = glCreateProgram();
    glAttachShader(s_prog2d, vs);
    glAttachShader(s_prog2d, fs);
    glBindAttribLocation(s_prog2d, 0, "aPos");
    glLinkProgram(s_prog2d);
    GLint ok = 0;
    glGetProgramiv(s_prog2d, GL_LINK_STATUS, &ok);
    if (!ok) {
        char info[512];
        glGetProgramInfoLog(s_prog2d, sizeof(info), NULL, info);
        OSReport("Gfx: program link failed: %s\n", info);
        s_prog2d = 0;
        return;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    s_locPos = glGetAttribLocation(s_prog2d, "aPos");
    s_locColor = glGetUniformLocation(s_prog2d, "uColor");
    OSReport("Gfx: 2D shader pipeline ready\n");
}

static void GfxInit2DTev(void) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVert2DTev);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragTev);
    if (!vs || !fs) return;
    s_prog2dTev = glCreateProgram();
    glAttachShader(s_prog2dTev, vs);
    glAttachShader(s_prog2dTev, fs);
    glBindAttribLocation(s_prog2dTev, 0, "aPos");
    glBindAttribLocation(s_prog2dTev, 1, "aColor");
    glLinkProgram(s_prog2dTev);
    GLint ok = 0;
    glGetProgramiv(s_prog2dTev, GL_LINK_STATUS, &ok);
    if (!ok) {
        OSReport("Gfx: TEV 2D program link failed\n");
        s_prog2dTev = 0;
        glDeleteShader(vs);
        glDeleteShader(fs);
        return;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    s_2dTevLocPos = glGetAttribLocation(s_prog2dTev, "aPos");
    s_2dTevLocColor = glGetAttribLocation(s_prog2dTev, "aColor");
    GfxCacheTevUniforms(s_prog2dTev, &s_2dTevUniforms);
    OSReport("Gfx: TEV 2D pipeline ready\n");
}

static void GfxInit3D(void) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVert3D);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFrag3DSolid);
    if (!vs || !fs) return;
    s_prog3d = glCreateProgram();
    glAttachShader(s_prog3d, vs);
    glAttachShader(s_prog3d, fs);
    glBindAttribLocation(s_prog3d, 0, "aPos");
    glBindAttribLocation(s_prog3d, 1, "aColor");
    glLinkProgram(s_prog3d);
    GLint ok = 0;
    glGetProgramiv(s_prog3d, GL_LINK_STATUS, &ok);
    if (!ok) {
        OSReport("Gfx: 3D program link failed\n");
        s_prog3d = 0;
        glDeleteShader(vs);
        glDeleteShader(fs);
        return;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    s_loc3dPos = glGetAttribLocation(s_prog3d, "aPos");
    s_loc3dVertexColor = glGetAttribLocation(s_prog3d, "aColor");
    s_loc3dTint = glGetUniformLocation(s_prog3d, "uTint");
    OSReport("Gfx: solid 3D pipeline ready\n");
}

int GfxInit(void)
{
    // Grab the default window handle libnx sets up for us.
    s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (s_display == EGL_NO_DISPLAY) {
        return 0;
    }

    if (eglInitialize(s_display, NULL, NULL) == EGL_FALSE) {
        eglTerminate(s_display);
        s_display = EGL_NO_DISPLAY;
        return 0;
    }

    // We want the OpenGL ES API.
    if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
        eglTerminate(s_display);
        s_display = EGL_NO_DISPLAY;
        return 0;
    }

    // Ask for a basic RGBA8 + depth24 config.
    EGLConfig config;
    EGLint numConfigs = 0;
    static const EGLint configAttrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_NONE
    };
    if (eglChooseConfig(s_display, configAttrs, &config, 1, &numConfigs) == EGL_FALSE
        || numConfigs < 1) {
        eglTerminate(s_display);
        s_display = EGL_NO_DISPLAY;
        return 0;
    }

    // Create the window surface bound to the default nwindow.
    s_surface = eglCreateWindowSurface(s_display, config,
                                       (EGLNativeWindowType)nwindowGetDefault(), NULL);
    if (s_surface == EGL_NO_SURFACE) {
        eglTerminate(s_display);
        s_display = EGL_NO_DISPLAY;
        return 0;
    }

    static const EGLint contextAttrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, contextAttrs);
    if (s_context == EGL_NO_CONTEXT) {
        eglDestroySurface(s_display, s_surface);
        s_surface = EGL_NO_SURFACE;
        eglTerminate(s_display);
        s_display = EGL_NO_DISPLAY;
        return 0;
    }

    eglMakeCurrent(s_display, s_surface, s_surface, s_context);
    eglQuerySurface(s_display, s_surface, EGL_WIDTH, &s_surfaceWidth);
    eglQuerySurface(s_display, s_surface, EGL_HEIGHT, &s_surfaceHeight);
    if (s_surfaceWidth <= 0) s_surfaceWidth = 1280;
    if (s_surfaceHeight <= 0) s_surfaceHeight = 720;
    GfxInit2D();
    GfxInit2DTev();
    GfxInitTex();
    GfxInit3D();
    GfxInit3DTex();
    GfxInitDummyTexture();
    glViewport(0, 0, s_surfaceWidth, s_surfaceHeight);
    glDisable(GL_SCISSOR_TEST);
    return 1;
}

void GfxSetViewport(float left, float top, float width, float height,
                    float nearZ, float farZ) {
    (void)nearZ;
    (void)farZ;
    if (width <= 0.0f || height <= 0.0f) return;
    const float sx = (float)s_surfaceWidth / 640.0f;
    const float sy = (float)s_surfaceHeight / 480.0f;
    int x = (int)(left * sx + 0.5f);
    int w = (int)(width * sx + 0.5f);
    int h = (int)(height * sy + 0.5f);
    int yTop = (int)(top * sy + 0.5f);
    int y = s_surfaceHeight - yTop - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > s_surfaceWidth) w = s_surfaceWidth - x;
    if (y + h > s_surfaceHeight) h = s_surfaceHeight - y;
    if (w > 0 && h > 0) glViewport(x, y, w, h);
}

void GfxSetScissor(unsigned int left, unsigned int top,
                   unsigned int width, unsigned int height) {
    const float sx = (float)s_surfaceWidth / 640.0f;
    const float sy = (float)s_surfaceHeight / 480.0f;
    int x = (int)((float)left * sx + 0.5f);
    int w = (int)((float)width * sx + 0.5f);
    int h = (int)((float)height * sy + 0.5f);
    int y = s_surfaceHeight - (int)(((float)top + (float)height) * sy + 0.5f);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > s_surfaceWidth) w = s_surfaceWidth - x;
    if (y + h > s_surfaceHeight) h = s_surfaceHeight - y;
    s_dbgScCalls++;
    if (!s_dbgScSeen || (long)w * h < (long)s_dbgScW * s_dbgScH) {
        s_dbgScX = x; s_dbgScY = y; s_dbgScW = w; s_dbgScH = h;
        s_dbgScRaw[0] = (int)left; s_dbgScRaw[1] = (int)top;
        s_dbgScRaw[2] = (int)width; s_dbgScRaw[3] = (int)height;
        s_dbgScSeen = 1;
    }
    if (w <= 0 || h <= 0) {
        glScissor(0, 0, 0, 0);
        s_dbgScEmpty++;
    } else {
        glScissor(x, y, w, h);
    }
    glEnable(GL_SCISSOR_TEST);
}

// Convert a GameCube screen X (0..640, left->right) to GL clip X (-1..1).
static float ScreenToClipX(float x) { return (x / 320.0f) - 1.0f; }
// Convert a GameCube screen Y (0..480, top->bottom) to GL clip Y (1..-1).
static float ScreenToClipY(float y) { return 1.0f - (y / 240.0f); }

void Gfx2D_DrawQuad(float x0, float y0, float x1, float y1,
                    float r, float g, float b, float a) {
    if (s_prog2d == 0) return;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    float cx0 = ScreenToClipX(x0), cy0 = ScreenToClipY(y0);
    float cx1 = ScreenToClipX(x1), cy1 = ScreenToClipY(y1);
    const GLfloat verts[] = {
        cx0, cy0,  cx1, cy0,  cx0, cy1,   // triangle 1
        cx1, cy0,  cx1, cy1,  cx0, cy1,   // triangle 2
    };
    glUseProgram(s_prog2d);
    glUniform4f(s_locColor, r, g, b, a);
    glEnableVertexAttribArray(s_locPos);
    glVertexAttribPointer(s_locPos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(s_locPos);
}

// --- Textured 2D pipeline ---
static GLuint s_progTex = 0;
static GLint  s_texLocPos = -1, s_texLocUV = -1, s_texLocColor = -1;
static GfxTevUniforms s_texUniforms;

static const char* kVertTex =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "attribute vec4 aColor;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vColor;\n"
    "void main() { vUV = aUV; vColor = aColor; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static void GfxInitTex(void) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertTex);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragTev);
    if (!vs || !fs) return;
    s_progTex = glCreateProgram();
    glAttachShader(s_progTex, vs);
    glAttachShader(s_progTex, fs);
    glLinkProgram(s_progTex);
    GLint ok = 0;
    glGetProgramiv(s_progTex, GL_LINK_STATUS, &ok);
    if (!ok) { OSReport("Gfx: tex program link failed\n"); s_progTex = 0; return; }
    glDeleteShader(vs); glDeleteShader(fs);
    s_texLocPos = glGetAttribLocation(s_progTex, "aPos");
    s_texLocUV = glGetAttribLocation(s_progTex, "aUV");
    s_texLocColor = glGetAttribLocation(s_progTex, "aColor");
    GfxCacheTevUniforms(s_progTex, &s_texUniforms);
    OSReport("Gfx: textured pipeline ready\n");
}

static void GfxInit3DTex(void) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVert3DTex);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragTev);
    if (!vs || !fs) return;
    s_prog3dTex = glCreateProgram();
    glAttachShader(s_prog3dTex, vs);
    glAttachShader(s_prog3dTex, fs);
    glBindAttribLocation(s_prog3dTex, 0, "aPos");
    glBindAttribLocation(s_prog3dTex, 1, "aUV");
    glBindAttribLocation(s_prog3dTex, 2, "aColor");
    glLinkProgram(s_prog3dTex);
    GLint ok = 0;
    glGetProgramiv(s_prog3dTex, GL_LINK_STATUS, &ok);
    if (!ok) {
        OSReport("Gfx: 3D texture program link failed\n");
        s_prog3dTex = 0;
        glDeleteShader(vs);
        glDeleteShader(fs);
        return;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    s_3dTexLocPos = glGetAttribLocation(s_prog3dTex, "aPos");
    s_3dTexLocUV = glGetAttribLocation(s_prog3dTex, "aUV");
    s_3dTexLocColor = glGetAttribLocation(s_prog3dTex, "aColor");
    GfxCacheTevUniforms(s_prog3dTex, &s_3dTexUniforms);
    OSReport("Gfx: depth-tested textured pipeline ready\n");
}

unsigned int GfxCreateTexture(int w, int h, const void* rgba,
                              int wrapS, int wrapT) {
    GLuint tex = 0;
    GLint glWrapS = wrapS == 1 ? GL_REPEAT :
                    (wrapS == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE);
    GLint glWrapT = wrapT == 1 ? GL_REPEAT :
                    (wrapT == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrapT);
    return tex;
}

void GfxDeleteTexture(unsigned int tex) {
    GLuint t = tex;
    if (t) glDeleteTextures(1, &t);
}

void Gfx2D_DrawTexTris(const float* clipXY, const float* uv, int count,
                       const float* color, unsigned int tex,
                       float r, float g, float b, float a) {
    if (s_progTex == 0 || !clipXY || !uv || !color || count <= 0) return;
    glUseProgram(s_progTex);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_alphaUpdate ? GL_TRUE : GL_FALSE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glUniform1i(s_texUniforms.sampler, 0);
    GfxUploadTev(&s_texUniforms, tex != 0, 0, r, g, b, a);
    glEnableVertexAttribArray(s_texLocPos);
    glVertexAttribPointer(s_texLocPos, 2, GL_FLOAT, GL_FALSE, 0, clipXY);
    glEnableVertexAttribArray(s_texLocUV);
    glVertexAttribPointer(s_texLocUV, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glEnableVertexAttribArray(s_texLocColor);
    glVertexAttribPointer(s_texLocColor, 4, GL_FLOAT, GL_FALSE, 0, color);
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDisableVertexAttribArray(s_texLocPos);
    glDisableVertexAttribArray(s_texLocUV);
    glDisableVertexAttribArray(s_texLocColor);
}

static GLenum GfxPrimitiveMode(int primitive) {
    if (primitive == 1) return GL_LINE_STRIP;
    if (primitive == 2) return GL_POINTS;
    if (primitive == 0) return GL_LINES;
    return GL_TRIANGLES;
}

void Gfx2D_DrawTexGeometry(const float* clipXY, const float* uv,
                           const float* color, int count, int primitive,
                           unsigned int tex, float r, float g, float b, float a) {
    if (s_progTex == 0 || !clipXY || !uv || !color || count <= 0) return;
    glUseProgram(s_progTex);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_alphaUpdate ? GL_TRUE : GL_FALSE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glUniform1i(s_texUniforms.sampler, 0);
    GfxUploadTev(&s_texUniforms, tex != 0, 0, r, g, b, a);
    glEnableVertexAttribArray(s_texLocPos);
    glVertexAttribPointer(s_texLocPos, 2, GL_FLOAT, GL_FALSE, 0, clipXY);
    glEnableVertexAttribArray(s_texLocUV);
    glVertexAttribPointer(s_texLocUV, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glEnableVertexAttribArray(s_texLocColor);
    glVertexAttribPointer(s_texLocColor, 4, GL_FLOAT, GL_FALSE, 0, color);
    glDrawArrays(GfxPrimitiveMode(primitive), 0, count);
    glDisableVertexAttribArray(s_texLocPos);
    glDisableVertexAttribArray(s_texLocUV);
    glDisableVertexAttribArray(s_texLocColor);
}

void Gfx3D_DrawTexTris(const float* clipXYZ, const float* uv,
                       const float* color, int count, unsigned int tex,
                       float r, float g, float b, float a) {
    if (s_prog3dTex == 0 || !clipXYZ || !uv || !color || count <= 0) return;
    glUseProgram(s_prog3dTex);
    GfxApply3DState();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glUniform1i(s_3dTexUniforms.sampler, 0);
    GfxUploadTev(&s_3dTexUniforms, tex != 0, 0, r, g, b, a);
    glEnableVertexAttribArray(s_3dTexLocPos);
    glVertexAttribPointer(s_3dTexLocPos, 3, GL_FLOAT, GL_FALSE, 0, clipXYZ);
    glEnableVertexAttribArray(s_3dTexLocUV);
    glVertexAttribPointer(s_3dTexLocUV, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glEnableVertexAttribArray(s_3dTexLocColor);
    glVertexAttribPointer(s_3dTexLocColor, 4, GL_FLOAT, GL_FALSE, 0, color);
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDisableVertexAttribArray(s_3dTexLocPos);
    glDisableVertexAttribArray(s_3dTexLocUV);
    glDisableVertexAttribArray(s_3dTexLocColor);
}

void Gfx3D_DrawTexGeometry(const float* clipXYZ, const float* uv,
                           const float* color, int count, int primitive,
                           unsigned int tex, float r, float g, float b, float a) {
    if (s_prog3dTex == 0 || !clipXYZ || !uv || !color || count <= 0) return;
    glUseProgram(s_prog3dTex);
    GfxApply3DState();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glUniform1i(s_3dTexUniforms.sampler, 0);
    GfxUploadTev(&s_3dTexUniforms, tex != 0, 0, r, g, b, a);
    glEnableVertexAttribArray(s_3dTexLocPos);
    glVertexAttribPointer(s_3dTexLocPos, 3, GL_FLOAT, GL_FALSE, 0, clipXYZ);
    glEnableVertexAttribArray(s_3dTexLocUV);
    glVertexAttribPointer(s_3dTexLocUV, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glEnableVertexAttribArray(s_3dTexLocColor);
    glVertexAttribPointer(s_3dTexLocColor, 4, GL_FLOAT, GL_FALSE, 0, color);
    glDrawArrays(GfxPrimitiveMode(primitive), 0, count);
    glDisableVertexAttribArray(s_3dTexLocPos);
    glDisableVertexAttribArray(s_3dTexLocUV);
    glDisableVertexAttribArray(s_3dTexLocColor);
}

void Gfx2D_DrawSolidTris(const float* clipXY, const float* color, int count,
                         float r, float g, float b, float a) {
    if (s_prog2dTev == 0 || !clipXY || !color || count <= 0) return;
    glUseProgram(s_prog2dTev);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    if (s_3dBlendMode == 0) glDisable(GL_BLEND);
    else {
        glEnable(GL_BLEND);
        glBlendFunc(GfxBlendFactor(s_3dBlendSrc, 0),
                    GfxBlendFactor(s_3dBlendDst, 1));
    }
    glColorMask(s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_alphaUpdate ? GL_TRUE : GL_FALSE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_dummyTexture);
    glUniform1i(s_2dTevUniforms.sampler, 0);
    GfxUploadTev(&s_2dTevUniforms, 0, 0, r, g, b, a);
    glEnableVertexAttribArray(s_2dTevLocPos);
    glVertexAttribPointer(s_2dTevLocPos, 2, GL_FLOAT, GL_FALSE, 0, clipXY);
    glEnableVertexAttribArray(s_2dTevLocColor);
    glVertexAttribPointer(s_2dTevLocColor, 4, GL_FLOAT, GL_FALSE, 0, color);
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDisableVertexAttribArray(s_2dTevLocPos);
    glDisableVertexAttribArray(s_2dTevLocColor);
}

void Gfx2D_DrawSolidGeometry(const float* clipXY, const float* color,
                             int count, int primitive,
                             float r, float g, float b, float a) {
    if (s_prog2dTev == 0 || !clipXY || !color || count <= 0) return;
    glUseProgram(s_prog2dTev);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    if (s_3dBlendMode == 0) glDisable(GL_BLEND);
    else {
        glEnable(GL_BLEND);
        glBlendFunc(GfxBlendFactor(s_3dBlendSrc, 0),
                    GfxBlendFactor(s_3dBlendDst, 1));
    }
    glColorMask(s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_colorUpdate ? GL_TRUE : GL_FALSE,
                s_alphaUpdate ? GL_TRUE : GL_FALSE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_dummyTexture);
    glUniform1i(s_2dTevUniforms.sampler, 0);
    GfxUploadTev(&s_2dTevUniforms, 0, 0, r, g, b, a);
    glEnableVertexAttribArray(s_2dTevLocPos);
    glVertexAttribPointer(s_2dTevLocPos, 2, GL_FLOAT, GL_FALSE, 0, clipXY);
    glEnableVertexAttribArray(s_2dTevLocColor);
    glVertexAttribPointer(s_2dTevLocColor, 4, GL_FLOAT, GL_FALSE, 0, color);
    glDrawArrays(GfxPrimitiveMode(primitive), 0, count);
    glDisableVertexAttribArray(s_2dTevLocPos);
    glDisableVertexAttribArray(s_2dTevLocColor);
}

void Gfx3D_DrawSolidTris(const float* clipXYZ, const float* color, int count,
                         float r, float g, float b, float a) {
    if (s_prog3d == 0 || !clipXYZ || !color || count <= 0) return;
    glUseProgram(s_prog3d);
    GfxApply3DState();
    glUniform4f(s_loc3dTint, r, g, b, a);
    glEnableVertexAttribArray(s_loc3dPos);
    glVertexAttribPointer(s_loc3dPos, 3, GL_FLOAT, GL_FALSE, 0, clipXYZ);
    glEnableVertexAttribArray(s_loc3dVertexColor);
    glVertexAttribPointer(s_loc3dVertexColor, 4, GL_FLOAT, GL_FALSE, 0, color);
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDisableVertexAttribArray(s_loc3dPos);
    glDisableVertexAttribArray(s_loc3dVertexColor);
}

void Gfx3D_DrawSolidGeometry(const float* clipXYZ, const float* color,
                             int count, int primitive,
                             float r, float g, float b, float a) {
    if (s_prog3d == 0 || !clipXYZ || !color || count <= 0) return;
    glUseProgram(s_prog3d);
    GfxApply3DState();
    glUniform4f(s_loc3dTint, r, g, b, a);
    glEnableVertexAttribArray(s_loc3dPos);
    glVertexAttribPointer(s_loc3dPos, 3, GL_FLOAT, GL_FALSE, 0, clipXYZ);
    glEnableVertexAttribArray(s_loc3dVertexColor);
    glVertexAttribPointer(s_loc3dVertexColor, 4, GL_FLOAT, GL_FALSE, 0, color);
    glDrawArrays(GfxPrimitiveMode(primitive), 0, count);
    glDisableVertexAttribArray(s_loc3dPos);
    glDisableVertexAttribArray(s_loc3dVertexColor);
}

void Gfx3D_SetCullMode(int mode) {
    s_3dCullMode = mode;
}

void Gfx3D_SetDepthMode(int enable, int func, int update) {
    s_3dDepthEnable = enable ? 1 : 0;
    s_3dDepthFunc = func;
    s_3dDepthWrite = update ? 1 : 0;
}

void Gfx3D_SetBlendMode(int mode, int src, int dst, int op) {
    s_3dBlendMode = mode;
    s_3dBlendSrc = src;
    s_3dBlendDst = dst;
    s_3dBlendOp = op;
    (void)s_3dBlendOp;
}

void Gfx3D_SetAlphaCompare(int comp0, int ref0, int op,
                           int comp1, int ref1) {
    s_3dAlphaComp0 = comp0;
    s_3dAlphaRef0 = (float)ref0 / 255.0f;
    s_3dAlphaOp = op;
    s_3dAlphaComp1 = comp1;
    s_3dAlphaRef1 = (float)ref1 / 255.0f;
}

void Gfx3D_SetColorUpdate(int enable) {
    s_colorUpdate = enable ? 1 : 0;
}

void Gfx3D_SetAlphaUpdate(int enable) {
    s_alphaUpdate = enable ? 1 : 0;
}

void GfxDebugDrawTest(void) {
    // A magenta rectangle in the middle of the screen. If this shows, the
    // whole GL render path (context, shaders, present) is working.
    Gfx2D_DrawQuad(220.0f, 160.0f, 420.0f, 320.0f, 1.0f, 0.0f, 1.0f, 1.0f);
}

void GfxSetClearColor(unsigned char r, unsigned char g, unsigned char b)
{
    s_clearR = (float)r / 255.0f;
    s_clearG = (float)g / 255.0f;
    s_clearB = (float)b / 255.0f;
}

void GfxBeginFrame(void)
{
    if (s_display == EGL_NO_DISPLAY) {
        return;
    }
    glViewport(0, 0, s_surfaceWidth, s_surfaceHeight);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f);
    glClearColor(s_clearR, s_clearG, s_clearB, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void GfxPresent(void)
{
    static unsigned int frame = 0;

    if (s_display == EGL_NO_DISPLAY) {
        return;
    }
    if ((frame % 120) == 0) {
        extern void GXGLReportFrameStats(void);
        OSReport("Gfx: frame %u draws=%u verts=%u\n", frame, s_drawCalls,
                 s_drawVerts);
        OSReport("Gfx: scissor raw(%d,%d,%d,%d) gl(%d,%d,%d,%d) emptyCount=%u\n",
                 s_dbgScRaw[0], s_dbgScRaw[1], s_dbgScRaw[2], s_dbgScRaw[3],
                 s_dbgScX, s_dbgScY, s_dbgScW, s_dbgScH, s_dbgScEmpty);
        OSReport("Gfx: colorUpd=%d alphaUpd=%d blendMode=%d src=%d dst=%d depthEn=%d depthFunc=%d cull=%d surf=%dx%d\n",
                 s_colorUpdate, s_alphaUpdate, s_3dBlendMode, s_3dBlendSrc,
                 s_3dBlendDst, s_3dDepthEnable, s_3dDepthFunc, s_3dCullMode,
                 s_surfaceWidth, s_surfaceHeight);
        s_dbgScEmpty = 0;
        s_dbgScCalls = 0;
        s_dbgScSeen = 0;
        GXGLReportFrameStats();
    }
    frame++;
    s_drawCalls = 0;
    s_drawVerts = 0;
    eglSwapBuffers(s_display, s_surface);
}

void GfxExit(void)
{
    if (s_display == EGL_NO_DISPLAY) {
        return;
    }
    eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (s_context != EGL_NO_CONTEXT) {
        eglDestroyContext(s_display, s_context);
        s_context = EGL_NO_CONTEXT;
    }
    if (s_surface != EGL_NO_SURFACE) {
        eglDestroySurface(s_display, s_surface);
        s_surface = EGL_NO_SURFACE;
    }
    eglTerminate(s_display);
    s_display = EGL_NO_DISPLAY;
}

#endif // __SWITCH__
