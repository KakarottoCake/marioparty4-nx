#include <stdio.h>
#include <stdlib.h>
#include "types.h"

#ifdef __SWITCH__
#include <switch.h>
#include "controller.h"
#include "gfx_switch.h"
#include "dolphin/gx/GXStruct.h"
#include "game/memory.h"

// Background/clear color the engine last requested (via GXSetCopyClear).
static GXColor s_bgColor = {0, 0, 0, 0};

// Mock render targets and constants
void* GXNtsc480IntDf = NULL;
void* GXPal528IntDf = NULL;
// RenderMode is dereferenced by the engine (e.g. sprput.c RenderMode->field_rendering),
// so it must point at a real render-mode object, not be a scalar.
static GXRenderModeObj s_renderMode = {0};
GXRenderModeObj* RenderMode = &s_renderMode;
s32 minimumVcount = 0;
float minimumVcountf = 0.0f;
u32 __OSBusClock;
u32 __OSCoreClock;

// Stub engine variables
u8 GWPlayerCfg[4096] = {0};
// Stub engine subsystem calls
void HuSysInit(void* mode) {
    extern void HuMemInitAll(void);
    u64 tick_freq = armGetSystemTickFreq();
    if (tick_freq == 0) {
        tick_freq = 19200000;
    }
    __OSBusClock = (u32)(tick_freq * 4);
    __OSCoreClock = __OSBusClock * 3;
    HuMemInitAll();
}
void GWInit(void) {}
void pfInit(void) {}
void HuPerfInit(void) {}
void HuPerfCreate(const char* name, u8 r, u8 g, u8 b, u8 a) {}
void WipeInit(s32 mode) {}

s32 VIGetNextField(void) { return 0; }
u32 VIGetRetraceCount(void) { return 0; }
s32 HuSoftResetButtonCheck(void) { return 0; }

void HuPerfZero(void) {}
void HuPerfBegin(s32 id) {}
void HuPerfEnd(s32 id) {}

void HuSysBeforeRender(void) {
    // Start a fresh GL frame cleared to the engine's requested background color.
    GfxSetClearColor(s_bgColor.r, s_bgColor.g, s_bgColor.b);
    GfxBeginFrame();
}
void HuSysDoneRender(s32 retrace) {
    // Present the finished frame.
    GfxPresent();
}

// GX performance and pixel metrics mocks
void GXSetGPMetric(s32 gp0, s32 gp1) {}
void GXClearGPMetric(void) {}
void GXSetVCacheMetric(s32 metric) {}
void GXClearVCacheMetric(void) {}
void GXClearPixMetric(void) {}
void GXClearMemMetric(void) {}

void GXReadGPMetric(u32* gp0, u32* gp1) { if(gp0) *gp0 = 0; if(gp1) *gp1 = 0; }
void GXReadVCacheMetric(u32* vcheck, u32* vmiss, u32* vstall) {
    if(vcheck) *vcheck = 0; if(vmiss) *vmiss = 0; if(vstall) *vstall = 0;
}
void GXReadPixMetric(u32* a, u32* b, u32* c, u32* d, u32* e, u32* f) {
    if(a) *a = 0; if(b) *b = 0; if(c) *c = 0; if(d) *d = 0; if(e) *e = 0; if(f) *f = 0;
}
void GXReadMemMetric(u32* a, u32* b, u32* c, u32* d, u32* e, u32* f, u32* g, u32* h, u32* i, u32* j) {
    if(a) *a = 0; if(b) *b = 0; if(c) *c = 0; if(d) *d = 0; if(e) *e = 0; if(f) *f = 0;
    if(g) *g = 0; if(h) *h = 0; if(i) *i = 0; if(j) *j = 0;
}

void pfClsScr(void) {}
void MGSeqMain(void) {}
void WipeExecAlways(void) {}
void pfDrawFonts(void) {}
void msmMusFdoutEnd(void) {}

// Dolphin pad/OS/VI/SI library stubs
void PADSetSpec(u32 spec) {}
BOOL PADInit(void) { return TRUE; }
u32 PADRead(void* status) { return 0; }
BOOL PADClamp(void* status) { return TRUE; }
void PADControlMotor(s32 chan, u32 command) {}
BOOL PADReset(u32 mask) { return TRUE; }

void SISetSamplingRate(u32 rate) {}
u32 OSDisableInterrupts(void) { return 0; }
void OSRestoreInterrupts(u32 level) {}
void VISetPostRetraceCallback(void* cb) {}
void VIWaitForRetrace(void) {
    for (int i = 0; i < 4; i++) {
        u64 kDown = padGetButtonsDown(&g_Pads[i]);
        if (kDown & (HidNpadButton_Plus | HidNpadButton_Minus)) {
            GfxExit();
            romfsExit();
            exit(0);
        }
    }
}

void msmSysRegularProc(void) {}

// Internal 3D / Graphics engine stubs for hsfman.c
void Hu3DCameraMotionExec(void) {}
void Hu3DDrawPost(void) {}
void Hu3DMotionNext(void) {}
void Hu3DAnimExec(void) {}
void Hu3DMotionExec(void) {}
void Hu3DSubMotionExec(void) {}
void GXInvalidateVtxCache(void) {}
void GXWaitDrawDone(void) {}
void ClusterMotionExec(void) {}
void GXSetDrawDone(void) {}
void InitVtxParm(void) {}
void PPCSync(void) {}
void GXSetFog(s32 type, float start, float end, float near, float far, void* color) {}
void ShapeProc(void) {}
void ClusterProc(void) {}
void EnvelopeProc(void) {}

// Additional Dolphin GX/MTX library stubs (loose no-ops).
// NOTE: matrices, GXSetProjection/GXLoadPosMtxImm, GXBegin/GXPosition/GXTexCoord/
// GXEnd, GXInitTexObj*/GXLoadTexObj and GXSetChanMatColor now live in gx_gl.c.
void GXSetViewport(float left, float top, float width, float height, float nearZ, float farZ) {}
void GXSetScissor(u32 left, u32 top, u32 width, u32 height) {}
void GXClearVtxDesc(void) {}
void GXSetVtxDesc(u8 attr, u8 type) {}
void GXSetVtxAttrFmt(u8 vtxfmt, u32 attr, u32 type, u32 size, u8 frac) {}
void GXSetTevColor(s32 id, void* color) {}
void GXSetNumTexGens(u8 num) {}
void GXSetNumTevStages(u8 num) {}
void GXSetTevOrder(s32 stage, u8 texcoord, u32 texmap, u8 color) {}
void GXSetTevColorIn(s32 stage, u32 a, u32 b, u32 c, u32 d) {}
void GXSetTevColorOp(s32 stage, u8 op, u8 bias, u8 scale, u8 clamp, u32 out_reg) {}
void GXSetTevAlphaIn(s32 stage, u32 a, u32 b, u32 c, u32 d) {}
void GXSetTevAlphaOp(s32 stage, u8 op, u8 bias, u8 scale, u8 clamp, u32 out_reg) {}
void GXSetNumChans(u8 num) {}
void GXSetChanCtrl(s32 chan, u8 enable, u8 amb_src, u8 mat_src, u32 light_mask, u8 diff_fn, u8 attn_fn) {}
void GXSetChanAmbColor(s32 chan, GXColor c) {}
void GXSetZCompLoc(u8 before_tex) {}
void GXSetNumIndStages(u8 num) {}
void GXSetTevOp(s32 stage, s32 mode) {}
void GXSetTevDirect(s32 stage) {}
void GXSetIndTexCoordScale(s32 ind_stage, s32 scale_s, s32 scale_t) {}
void GXSetIndTexOrder(s32 ind_stage, s32 tex_coord, s32 tex_map) {}
void GXSetTevIndTile(s32 stage, s32 ind_stage, u16 w, u16 h, u16 tw, u16 th, s32 fmt, s32 mtx, s32 bias, s32 alpha) {}
void GXSetTexCoordGen2(s32 dst_coord, s32 func, s32 src, u32 mtx, u32 normalize, u32 pt_mtx) {}
void GXSetTexCoordScaleManually(s32 coord, u8 enable, u16 scale_s, u16 scale_t) {}
void GXInitTexObjLOD(void* obj, s32 minf, s32 magf, float minlod, float maxlod, float lodbias, u8 biasclamp, u8 edgelod, s32 maxaniso) {}
void GXSetViewportJitter(float left, float top, float width, float height, float nearZ, float farZ, u32 field) {}
void GXSetTexCopySrc(u32 x, u32 y, u32 w, u32 h) {}
void GXSetTexCopyDst(u32 w, u32 h, u32 fmt, u8 mip) {}
void GXSetCurrentMtx(u32 id) {}
void Hu3DDrawPreInit(void) {}

// Missing stubs from latest build check
void Hu3DAnimInit(void) {}
void Hu3DParManInit(void) {}
void GXSetCopyClear(GXColor color, u32 clear_z) { s_bgColor = color; }
void GXCopyTex(u32 dest_addr, u8 clear) {}

// Performance metrics counters
u32 totalPolyCnt = 0;
u32 totalPolyCnted = 0;
u32 totalMatCnt = 0;
u32 totalMatCnted = 0;
u32 totalTexCnt = 0;
u32 totalTexCnted = 0;
u32 totalTexCacheCnt = 0;
u32 totalTexCacheCnted = 0;

void Hu3DMotionInit(void) {}

// Missing stubs for bootDll
void HuAudSndGrpSetSet(s32 id) {}
void HuWinInit(void) {}
void HuWinMesMaxSizeBetGet(void* size, s32 mes) {}
s16 HuWinCreate(float x, float y, float w, float h, s16 type) { return 0; }
void HuWinMesSpeedSet(s16 win, s16 speed) {}
void HuWinBGTPLvlSet(s16 win, float lvl) {}
void HuWinPriSet(s16 win, s16 prio) {}
void HuWinAttrSet(s16 win, u32 attr) {}
void WipeCreate(s32 type, s32 mode, s16 delay) {}
s32 WipeStatGet(void) { return 0; }
s32 HuTHPEndCheck(void) { return 1; }
s32 HuTHPFrameGet(void) { return 0; }
void HuWinMesSet(s16 win, s32 mes) {}
void HuWinHomeClear(s16 win) {}
void HuWinKill(s16 win) {}
void HuTHPClose(void) {}
void WipeColorSet(u8 r, u8 g, u8 b) {}
void HuAudSStreamAllFadeOut(s32 fade) {}

// More bootDll stubs
s32 msmSeGetEntryID(s32 id) { return 0; }
s32 msmSeGetNumPlay(s32 id) { return 0; }
void HuAudSStreamPlay(s32 id) {}
void HuAudFXPlay(s32 id) {}
u32 OSGetTick(void) { return (u32)armGetSystemTick(); }
void CharInit(void) {}
void HuWindowInit(void) {}
void MGSeqInit(void) {}
s32 msmSysGetSampSize(s32 group) {
    (void)group;
    /* The Switch audio backend is not wired yet, but the boot code still
     * allocates and releases a staging buffer.  A zero-byte allocation
     * corrupts the original heap allocator's free-list. */
    return 32;
}
void msmSysLoadGroup(s32 group, void* buffer, s32 flag) {}
s16 HuTHPSprCreateVol(const char* path, s16 loop, float vol) { return 0; }

// Stubs for overlay loader, audio, progressive mode, and sprite
void DCFlushRangeNoSync(void* addr, u32 size) {}
s32 fadeStat = 0;
void espInit(void) {}
void HuAudSndCharGrpSet(s16 id) {}
void HuAudDllSndGrpSet(s16 id) {}
s32 _CheckFlag(u32 flag) { return 0; }
void omSysPauseEnable(s32 enable) {}
void MGSeqPracticeInit(void) {}
void CharModelKill(void) {}
void MGSeqKillAll(void) {}
void HuWinAllKill(void) {}
void HuAudFXListnerKill(void) {}

u32 OSGetResetCode(void) { return 0; }
u32 VIGetDTVStatus(void) { return 0; }
void OSSetProgressiveMode(u32 mode) {}
u32 OSGetProgressiveMode(void) { return 0; }
void* GXNtsc480Prog = NULL;
void VIConfigure(void* mode) {}
void VIFlush(void) {}

// HSF loading and the basic static draw path live in hsf_switch.c.
s32 Hu3DMotionModelCreate(void* model) { return 0; }
float Hu3DMotionMaxTimeGet(s32 id) { return 0.0f; }
void Hu3DMotionClusterSet(s32 model, s32 motion) {}
void Hu3DMotionShapeSet(s32 model, s32 motion) {}
void Hu3DAnimModelKill(s32 model) {}
void Hu3DMotionKill(s32 motion) {}
void Hu3DParManAllKill(void) {}
void Hu3DMotionAllKill(void) {}
void Hu3DAnimAllKill(void) {}

u8 Hu3DMotion[4096] = {0};

// Vector math functions
#include <math.h>
void PSVECSubtract(const void* a, const void* b, void* dest) {
    float* fa = (float*)a;
    float* fb = (float*)b;
    float* fd = (float*)dest;
    fd[0] = fa[0] - fb[0];
    fd[1] = fa[1] - fb[1];
    fd[2] = fa[2] - fb[2];
}

void PSVECNormalize(const void* src, void* dest) {
    float* fs = (float*)src;
    float* fd = (float*)dest;
    float len = sqrtf(fs[0]*fs[0] + fs[1]*fs[1] + fs[2]*fs[2]);
    if (len > 0.0f) {
        fd[0] = fs[0] / len;
        fd[1] = fs[1] / len;
        fd[2] = fs[2] / len;
    } else {
        fd[0] = fd[1] = fd[2] = 0.0f;
    }
}

// Print functions stubs
u8 fontcolor[4] = {255, 255, 255, 255};
void printWin(s16 win, float x, float y, const char* str, ...) {}
void print8(float x, float y, const char* str, ...) {}

u64 OSGetTime(void) {
    return armGetSystemTick();
}

// Cache and ARAM stubs
void DCInvalidateRange(void* addr, u32 size) {}
void DCFlushRange(void* addr, u32 size) {}
u32 HuARDirCheck(u32 dir) { return 0; }
void* HuAR_ARAMtoMRAMNum(u32 dir, s32 num) { return NULL; }
s32 HuARDMACheck(void) { return 0; }

#endif
