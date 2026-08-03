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

// --- 2D shader pipeline (foundation of the GX->GL translation layer) ---
static GLuint s_prog2d = 0;
static GLint  s_locPos = -1;    // attribute: vertex position (clip space)
static GLint  s_locColor = -1;  // uniform: solid color

extern void OSReport(const char* msg, ...);  // engine logger (goes to SD log)

static void GfxInitTex(void);  // defined later (textured pipeline)
static void GfxInit3DTex(void);  // defined later (depth-tested textured path)

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

static const char* kFrag2D =
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "void main() { gl_FragColor = uColor; }\n";

// HSF's first native draw path uses the same solid-color material fallback as
// the 2D path, but keeps clip-space Z so nearer triangles hide farther ones.
static GLuint s_prog3d = 0;
static GLint s_loc3dPos = -1;
static GLint s_loc3dColor = -1;
static GLint s_loc3dVertexColor = -1;

static const char* kVert3D =
    "attribute vec3 aPos;\n"
    "attribute vec4 aColor;\n"
    "varying vec4 vColor;\n"
    "void main() { vColor = aColor; gl_Position = vec4(aPos, 1.0); }\n";

static const char* kFrag3D =
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "varying vec4 vColor;\n"
    "void main() { gl_FragColor = uColor * vColor; }\n";

static GLuint s_prog3dTex = 0;
static GLint s_3dTexLocPos = -1, s_3dTexLocUV = -1;
static GLint s_3dTexLocColor = -1;
static GLint s_3dTexLocTint = -1, s_3dTexLocSampler = -1;

static const char* kVert3DTex =
    "attribute vec3 aPos;\n"
    "attribute vec2 aUV;\n"
    "attribute vec4 aColor;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vColor;\n"
    "void main() { vUV = aUV; vColor = aColor; gl_Position = vec4(aPos, 1.0); }\n";

static const char* kFrag3DTex =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vColor;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4 uTint;\n"
    "void main() { gl_FragColor = texture2D(uTex, vUV) * uTint * vColor; }\n";

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

static void GfxInit3D(void) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVert3D);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFrag3D);
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
    s_loc3dColor = glGetUniformLocation(s_prog3d, "uColor");
    s_loc3dVertexColor = glGetAttribLocation(s_prog3d, "aColor");
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
    GfxInit2D();
    GfxInitTex();
    GfxInit3D();
    GfxInit3DTex();
    return 1;
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
static GLint  s_texLocPos = -1, s_texLocUV = -1, s_texLocTint = -1, s_texLocSampler = -1;

static const char* kVertTex =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char* kFragTex =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4 uTint;\n"
    "void main() { gl_FragColor = texture2D(uTex, vUV) * uTint; }\n";

static void GfxInitTex(void) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertTex);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragTex);
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
    s_texLocTint = glGetUniformLocation(s_progTex, "uTint");
    s_texLocSampler = glGetUniformLocation(s_progTex, "uTex");
    OSReport("Gfx: textured pipeline ready\n");
}

static void GfxInit3DTex(void) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVert3DTex);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFrag3DTex);
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
    s_3dTexLocTint = glGetUniformLocation(s_prog3dTex, "uTint");
    s_3dTexLocSampler = glGetUniformLocation(s_prog3dTex, "uTex");
    OSReport("Gfx: depth-tested textured pipeline ready\n");
}

unsigned int GfxCreateTexture(int w, int h, const void* rgba) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

void GfxDeleteTexture(unsigned int tex) {
    GLuint t = tex;
    if (t) glDeleteTextures(1, &t);
}

void Gfx2D_DrawTexTris(const float* clipXY, const float* uv, int count,
                       unsigned int tex, float r, float g, float b, float a) {
    if (s_progTex == 0) return;
    glUseProgram(s_progTex);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glUniform1i(s_texLocSampler, 0);
    glUniform4f(s_texLocTint, r, g, b, a);
    glEnableVertexAttribArray(s_texLocPos);
    glVertexAttribPointer(s_texLocPos, 2, GL_FLOAT, GL_FALSE, 0, clipXY);
    glEnableVertexAttribArray(s_texLocUV);
    glVertexAttribPointer(s_texLocUV, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDisableVertexAttribArray(s_texLocPos);
    glDisableVertexAttribArray(s_texLocUV);
}

void Gfx3D_DrawTexTris(const float* clipXYZ, const float* uv,
                       const float* color, int count, unsigned int tex,
                       float r, float g, float b, float a) {
    if (s_prog3dTex == 0 || !clipXYZ || !uv || !color || count <= 0) return;
    glUseProgram(s_prog3dTex);
    GfxApply3DState();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glUniform1i(s_3dTexLocSampler, 0);
    glUniform4f(s_3dTexLocTint, r, g, b, a);
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

void Gfx2D_DrawSolidTris(const float* clipXY, int count,
                         float r, float g, float b, float a) {
    if (s_prog2d == 0 || !clipXY || count <= 0) return;
    glUseProgram(s_prog2d);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glUniform4f(s_locColor, r, g, b, a);
    glEnableVertexAttribArray(s_locPos);
    glVertexAttribPointer(s_locPos, 2, GL_FLOAT, GL_FALSE, 0, clipXY);
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDisableVertexAttribArray(s_locPos);
}

void Gfx3D_DrawSolidTris(const float* clipXYZ, const float* color, int count,
                         float r, float g, float b, float a) {
    if (s_prog3d == 0 || !clipXYZ || !color || count <= 0) return;
    glUseProgram(s_prog3d);
    GfxApply3DState();
    glUniform4f(s_loc3dColor, r, g, b, a);
    glEnableVertexAttribArray(s_loc3dPos);
    glVertexAttribPointer(s_loc3dPos, 3, GL_FLOAT, GL_FALSE, 0, clipXYZ);
    glEnableVertexAttribArray(s_loc3dVertexColor);
    glVertexAttribPointer(s_loc3dVertexColor, 4, GL_FLOAT, GL_FALSE, 0, color);
    glDrawArrays(GL_TRIANGLES, 0, count);
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
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f);
    glClearColor(s_clearR, s_clearG, s_clearB, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void GfxPresent(void)
{
    if (s_display == EGL_NO_DISPLAY) {
        return;
    }
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
