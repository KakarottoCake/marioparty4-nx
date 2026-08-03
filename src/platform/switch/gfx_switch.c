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

// --- 2D shader pipeline (foundation of the GX->GL translation layer) ---
static GLuint s_prog2d = 0;
static GLint  s_locPos = -1;    // attribute: vertex position (clip space)
static GLint  s_locColor = -1;  // uniform: solid color

extern void OSReport(const char* msg, ...);  // engine logger (goes to SD log)

static void GfxInitTex(void);  // defined later (textured pipeline)

static const char* kVert2D =
    "attribute vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char* kFrag2D =
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "void main() { gl_FragColor = uColor; }\n";

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
    return 1;
}

// Convert a GameCube screen X (0..640, left->right) to GL clip X (-1..1).
static float ScreenToClipX(float x) { return (x / 320.0f) - 1.0f; }
// Convert a GameCube screen Y (0..480, top->bottom) to GL clip Y (1..-1).
static float ScreenToClipY(float y) { return 1.0f - (y / 240.0f); }

void Gfx2D_DrawQuad(float x0, float y0, float x1, float y1,
                    float r, float g, float b, float a) {
    if (s_prog2d == 0) return;
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

void Gfx2D_DrawSolidTris(const float* clipXY, int count,
                         float r, float g, float b, float a) {
    if (s_prog2d == 0 || !clipXY || count <= 0) return;
    glUseProgram(s_prog2d);
    glDisable(GL_BLEND);
    glUniform4f(s_locColor, r, g, b, a);
    glEnableVertexAttribArray(s_locPos);
    glVertexAttribPointer(s_locPos, 2, GL_FLOAT, GL_FALSE, 0, clipXY);
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDisableVertexAttribArray(s_locPos);
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
