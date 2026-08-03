#ifndef GFX_SWITCH_H
#define GFX_SWITCH_H

#ifdef __SWITCH__

// Rendering backend (EGL + OpenGL ES 2) for the Switch port.

#ifdef __cplusplus
extern "C" {
#endif

// Initialize EGL/GLES2 on the default window. Returns 1 on success, 0 on failure.
int GfxInit(void);

// Store the color used to clear the framebuffer next frame (0-255 per channel).
void GfxSetClearColor(unsigned char r, unsigned char g, unsigned char b);

// Clear the framebuffer to the current clear color. Call at frame start.
void GfxBeginFrame(void);

// Swap buffers / present the finished frame to the screen.
void GfxPresent(void);

// Draw a solid-color 2D quad in GameCube screen space (640x480, origin top-left).
// This is the foundation of the GX->GL 2D path (sprites/UI will build on it).
void Gfx2D_DrawQuad(float x0, float y0, float x1, float y1,
                    float r, float g, float b, float a);

// Temporary: draw a visible test quad so we can confirm rendering works on screen.
void GfxDebugDrawTest(void);

// --- Textured 2D path (used by the GX->GL sprite translation) ---
// Create/replace a GL texture from RGBA8888 pixels. Returns a GL texture id.
unsigned int GfxCreateTexture(int w, int h, const void* rgba);
void GfxDeleteTexture(unsigned int tex);

// Draw triangles given in GL clip space (x,y per vertex) with matching UVs,
// sampled from 'tex' and modulated by tint rgba, using alpha blending.
// 'count' is the number of vertices (must be a multiple of 3).
void Gfx2D_DrawTexTris(const float* clipXY, const float* uv, int count,
                       unsigned int tex, float r, float g, float b, float a);

// Textured triangles with full clip-space XYZ coordinates and depth testing.
void Gfx3D_DrawTexTris(const float* clipXYZ, const float* uv, int count,
                       unsigned int tex, float r, float g, float b, float a);

// Draw triangles in clip space without sampling a texture.  This is used by
// the first native HSF mesh path, where materials are currently solid-color.
void Gfx2D_DrawSolidTris(const float* clipXY, int count,
                         float r, float g, float b, float a);

// Draw solid triangles with full OpenGL clip-space XYZ coordinates and depth
// testing.  This is used by the native HSF mesh path.
void Gfx3D_DrawSolidTris(const float* clipXYZ, int count,
                         float r, float g, float b, float a);

// Tear down the GL context.
void GfxExit(void);

#ifdef __cplusplus
}
#endif

#endif // __SWITCH__
#endif // GFX_SWITCH_H
