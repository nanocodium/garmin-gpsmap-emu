/*
 * Garmin GPSMAP 7x08: OpenGL ES 1.1 API interception.  See garmin_gl.h.
 *
 * Trampoline written over each hooked entry point (Thumb-2, 18 bytes):
 *     movw  r12, #lo(GARMIN_GL_MMIO_BASE)
 *     movt  r12, #hi(GARMIN_GL_MMIO_BASE)
 *     str   r12, [r12, #slot*4]     ; trap: QEMU runs the call
 *     ldr   r0,  [r12, #0xffc]      ; result
 *     bx    lr
 * r0-r3 and the stack still hold the AAPCS arguments when the store
 * traps, so the handler reads them straight from the CPU state.  The
 * original function body is not preserved: the host owns the call.
 *
 * Execution model: the vCPU thread (holding the BQL) decodes the call,
 * copies whatever guest memory it references (client arrays, texels,
 * parameter vectors) into a request and hands it to a render thread that
 * owns a Mesa OpenGL compatibility context (surfaceless EGL, FBO of the
 * panel size).  GLES 1.1 is a subset of fixed-function desktop GL, so
 * most calls forward 1:1; fixed-point variants are converted to float,
 * client arrays are converted to GL_FLOAT (desktop GL lacks GL_FIXED and
 * GL_BYTE vertices), and VBOs are kept as host-side byte caches so that
 * the same conversion path serves both.  eglSwapBuffers reads the FBO
 * back, packs RGB565 and writes it into the guest's scan-out buffer,
 * which the DISPC model displays.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "qemu/thread.h"
#include "qemu/rcu.h"
#include "system/runstate.h"
#include <math.h>
#include "system/address-spaces.h"
#include "system/memory.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "garmin_gl.h"

/*
 * Host rendering needs libepoxy, which QEMU announces as CONFIG_OPENGL when
 * it was configured with --enable-opengl.  Renderer and QEMU's own GL display
 * paths are independent here (this file never touches the UI), so a tree
 * where CONFIG_OPENGL cannot be enabled can still build the renderer by
 * linking epoxy for this file and configuring
 * --extra-cflags=-DCONFIG_GARMIN_GL_EPOXY.
 */
#if defined(CONFIG_OPENGL) || defined(CONFIG_GARMIN_GL_EPOXY)
#define GARMIN_GL_HOST_RENDER 1
#endif

#ifdef GARMIN_GL_HOST_RENDER
#include <epoxy/gl.h>
#ifdef _WIN32
#include <epoxy/wgl.h>
#else
#include <epoxy/egl.h>
#endif
#endif

bool garmin_gl_log;                   /* GARMIN_GL_LOG=1: log every call */

/*
 * GARMIN_GL_FBO=1 enables the GL_OES_framebuffer_object and EGL surface
 * entry points (the ops marked opt_in below).  Off by default, because
 * implementing them is a regression as things stand: the GUI does not need
 * them - its compositor software-renders its layers into buffers in guest
 * memory, which the renderer picks up at swap time - and with them the page
 * header comes out as uninitialised texture content instead of the bitmap.
 * Left in and switchable because the driver does call them, so whatever
 * they are for is worth finding out.
 */
static bool garmin_gl_fbo;

/* GARMIN_GL_DRAWTEX=1: trace glDrawTexOES blits and crop-rectangle sets. */
static bool garmin_gl_drawtex_log;

/*
 * GARMIN_GL_RESYNC=1 re-reads a texture's guest buffer at swap time even for
 * textures the firmware did not upload in this frame, re-uploading it when
 * the bytes there have changed.
 *
 * Off by default, because it is a guess about memory the firmware owns and
 * the guess is wrong after the GUI tears its windows down and rebuilds them
 * (switching the unit into Store Demonstration does exactly that): the
 * layer's buffer has been freed and reused by then, so what gets uploaded is
 * unrelated memory, and the page background fills with bands of noise.
 *
 * The case this was meant to cover - the compositor drawing into a layer
 * after the GL calls that reference it - is handled without guessing by the
 * pass over this frame's own queued uploads in sync_large_textures(), which
 * is always active.
 */
static bool garmin_gl_resync;

/*
 * Base of the synthetic EGLSurface handles handed back to the firmware: any
 * non-NULL value it will only ever pass back to us.
 */
#define EGL_FAKE_SURFACE  0x5eeb0000u

/* ------------------------------------------------------------------ */
/* operation table                                                       */
/* ------------------------------------------------------------------ */

enum GlOp {
    OP_UNKNOWN = 0,
    /* scalar pass-through (argument types from the signature string) */
    OP_BLENDFUNC, OP_DEPTHFUNC, OP_CULLFACE, OP_FRONTFACE, OP_MATRIXMODE,
    OP_ENABLE, OP_DISABLE, OP_BINDTEXTURE, OP_BLENDFUNCSEP, OP_POINTSIZE,
    OP_COLOR4F, OP_DEPTHRANGE, OP_SCALE, OP_TRANSLATE, OP_ROTATE,
    OP_FRUSTUM, OP_ORTHO, OP_TEXPARAMF, OP_TEXPARAMI, OP_TEXENVF, OP_TEXENVI,
    OP_LINEWIDTH, OP_CLEARDEPTH, OP_DEPTHMASK, OP_COLORMASK, OP_SHADEMODEL,
    OP_LOGICOP, OP_CLEAR, OP_CLEARCOLOR, OP_SCISSOR, OP_VIEWPORT,
    OP_LOADIDENTITY, OP_PUSHMATRIX, OP_POPMATRIX, OP_ACTIVETEXTURE,
    OP_ALPHAFUNC, OP_CLEARSTENCIL, OP_STENCILFUNC, OP_STENCILOP,
    OP_STENCILMASK, OP_POLYGONOFFSET, OP_SAMPLECOVERAGE, OP_HINT,
    OP_LIGHTF, OP_LIGHTMODELF, OP_MATERIALF, OP_FOGF, OP_POINTPARAMF,
    OP_NORMAL3F, OP_MULTITEXCOORD4F, OP_FLUSH, OP_FINISH, OP_GETERROR,
    OP_CULLFACE_END_,
    /* vector parameters: 4 floats copied from the guest */
    OP_LIGHTFV, OP_LIGHTMODELFV, OP_MATERIALFV, OP_FOGFV, OP_TEXENVFV,
    OP_TEXPARAMFV, OP_POINTPARAMFV, OP_CLIPPLANEF,
    OP_LOADMATRIX, OP_MULTMATRIX,
    /* state tracked on the vCPU side (no host call) */
    OP_VERTEXPTR, OP_NORMALPTR, OP_COLORPTR, OP_TEXCOORDPTR, OP_POINTSIZEPTR,
    OP_ENABLECLIENT, OP_DISABLECLIENT, OP_CLIENTACTIVETEX,
    OP_BINDBUFFER, OP_GENBUFFERS, OP_DELETEBUFFERS, OP_BUFFERDATA,
    OP_BUFFERSUBDATA, OP_PIXELSTORE, OP_GETSTRING,
    /* mixed */
    OP_DRAWARRAYS, OP_DRAWELEMENTS, OP_TEXIMAGE2D, OP_TEXSUBIMAGE2D,
    OP_COMPRESSEDTEXIMAGE2D, OP_COMPRESSEDTEXSUBIMAGE2D, OP_READPIXELS,
    OP_GENTEXTURES, OP_DELETETEXTURES, OP_ISTEXTURE, OP_ISENABLED,
    OP_GETFLOATV, OP_GETINTEGERV, OP_GETBOOLEANV, OP_GETFIXEDV,
    OP_GETLIGHTFV, OP_GETMATERIALFV, OP_GETTEXENVFV, OP_GETTEXPARAMFV,
    OP_GETCLIPPLANEF, OP_GETBUFFERPARAM, OP_GETPOINTERV,
    OP_COPYTEXIMAGE2D, OP_COPYTEXSUBIMAGE2D,
    OP_DRAWTEX, OP_SWAPBUFFERS, OP_NOP, OP_EGL_TRUE,
    /* GL_OES_framebuffer_object: the GUI compositor renders into textures */
    OP_BINDFRAMEBUFFER, OP_BINDRENDERBUFFER, OP_GENFRAMEBUFFERS,
    OP_GENRENDERBUFFERS, OP_DELETEFRAMEBUFFERS, OP_DELETERENDERBUFFERS,
    OP_RENDERBUFFERSTORAGE, OP_FRAMEBUFFERRENDERBUFFER,
    OP_FRAMEBUFFERTEXTURE2D, OP_CHECKFRAMEBUFFER, OP_GENERATEMIPMAP,
    OP_EGLIMAGE_TEX2D,
    /* EGL surface management */
    OP_EGL_SURFACE, OP_EGL_CURSURFACE, OP_EGL_QUERYSURFACE, OP_EGL_NO_IMAGE,
};

typedef struct GlOpDef {
    const char *name;
    enum GlOp op;
    const char *sig;    /* i=int, f=float, x=fixed->float, p=pointer */
    bool opt_in;        /* only used when GARMIN_GL_FBO=1 (see below) */
} GlOpDef;

static const GlOpDef opdefs[] = {
    { "glBlendFunc", OP_BLENDFUNC, "ii" },
    { "glDepthFunc", OP_DEPTHFUNC, "i" },
    { "glCullFace", OP_CULLFACE, "i" },
    { "glFrontFace", OP_FRONTFACE, "i" },
    { "glMatrixMode", OP_MATRIXMODE, "i" },
    { "glEnable", OP_ENABLE, "i" },
    { "glDisable", OP_DISABLE, "i" },
    { "glBindTexture", OP_BINDTEXTURE, "ii" },
    { "glBlendFuncSeparate", OP_BLENDFUNCSEP, "iiii" },
    { "glBlendFuncSeparateOES", OP_BLENDFUNCSEP, "iiii" },
    { "glPointSize", OP_POINTSIZE, "f" },
    { "glPointSizex", OP_POINTSIZE, "x" },
    { "glColor4f", OP_COLOR4F, "ffff" },
    { "glColor4x", OP_COLOR4F, "xxxx" },
    { "glColor4ub", OP_COLOR4F, "bbbb" },
    { "glDepthRangef", OP_DEPTHRANGE, "ff" },
    { "glDepthRangex", OP_DEPTHRANGE, "xx" },
    { "glScalef", OP_SCALE, "fff" },
    { "glScalex", OP_SCALE, "xxx" },
    { "glTranslatef", OP_TRANSLATE, "fff" },
    { "glTranslatex", OP_TRANSLATE, "xxx" },
    { "glRotatef", OP_ROTATE, "ffff" },
    { "glRotatex", OP_ROTATE, "xxxx" },
    { "glFrustumf", OP_FRUSTUM, "ffffff" },
    { "glFrustumx", OP_FRUSTUM, "xxxxxx" },
    { "glOrthof", OP_ORTHO, "ffffff" },
    { "glOrthox", OP_ORTHO, "xxxxxx" },
    { "glTexParameterf", OP_TEXPARAMF, "iif" },
    { "glTexParameterx", OP_TEXPARAMF, "iix" },
    { "glTexParameteri", OP_TEXPARAMI, "iii" },
    { "glTexEnvf", OP_TEXENVF, "iif" },
    { "glTexEnvx", OP_TEXENVF, "iix" },
    { "glTexEnvi", OP_TEXENVI, "iii" },
    { "glLineWidth", OP_LINEWIDTH, "f" },
    { "glLineWidthx", OP_LINEWIDTH, "x" },
    { "glClearDepthf", OP_CLEARDEPTH, "f" },
    { "glClearDepthx", OP_CLEARDEPTH, "x" },
    { "glClearDepth", OP_CLEARDEPTH, "f" },
    { "glDepthMask", OP_DEPTHMASK, "i" },
    { "glColorMask", OP_COLORMASK, "iiii" },
    { "glShadeModel", OP_SHADEMODEL, "i" },
    { "glLogicOp", OP_LOGICOP, "i" },
    { "glClear", OP_CLEAR, "i" },
    { "glClearColor", OP_CLEARCOLOR, "ffff" },
    { "glClearColorx", OP_CLEARCOLOR, "xxxx" },
    { "glScissor", OP_SCISSOR, "iiii" },
    { "glViewport", OP_VIEWPORT, "iiii" },
    { "glLoadIdentity", OP_LOADIDENTITY, "" },
    { "glPushMatrix", OP_PUSHMATRIX, "" },
    { "glPopMatrix", OP_POPMATRIX, "" },
    { "glActiveTexture", OP_ACTIVETEXTURE, "i" },
    { "glAlphaFunc", OP_ALPHAFUNC, "if" },
    { "glAlphaFuncx", OP_ALPHAFUNC, "ix" },
    { "glClearStencil", OP_CLEARSTENCIL, "i" },
    { "glStencilFunc", OP_STENCILFUNC, "iii" },
    { "glStencilOp", OP_STENCILOP, "iii" },
    { "glStencilMask", OP_STENCILMASK, "i" },
    { "glPolygonOffset", OP_POLYGONOFFSET, "ff" },
    { "glPolygonOffsetx", OP_POLYGONOFFSET, "xx" },
    { "glSampleCoverage", OP_SAMPLECOVERAGE, "fi" },
    { "glSampleCoveragex", OP_SAMPLECOVERAGE, "xi" },
    { "glHint", OP_HINT, "ii" },
    { "glLightf", OP_LIGHTF, "iif" },
    { "glLightx", OP_LIGHTF, "iix" },
    { "glLightModelf", OP_LIGHTMODELF, "if" },
    { "glLightModelx", OP_LIGHTMODELF, "ix" },
    { "glMaterialf", OP_MATERIALF, "iif" },
    { "glMaterialx", OP_MATERIALF, "iix" },
    { "glFogf", OP_FOGF, "if" },
    { "glFogx", OP_FOGF, "ix" },
    { "glPointParameterf", OP_POINTPARAMF, "if" },
    { "glPointParameterx", OP_POINTPARAMF, "ix" },
    { "glNormal3f", OP_NORMAL3F, "fff" },
    { "glNormal3x", OP_NORMAL3F, "xxx" },
    { "glMultiTexCoord4f", OP_MULTITEXCOORD4F, "iffff" },
    { "glMultiTexCoord4x", OP_MULTITEXCOORD4F, "ixxxx" },
    { "glFlush", OP_FLUSH, "" },
    { "glFinish", OP_FINISH, "" },
    { "glGetError", OP_GETERROR, "" },
    { "glLightfv", OP_LIGHTFV, "iip" },
    { "glLightxv", OP_LIGHTFV, "iiP" },
    { "glLightModelfv", OP_LIGHTMODELFV, "ip" },
    { "glLightModelxv", OP_LIGHTMODELFV, "iP" },
    { "glMaterialfv", OP_MATERIALFV, "iip" },
    { "glMaterialxv", OP_MATERIALFV, "iiP" },
    { "glFogfv", OP_FOGFV, "ip" },
    { "glFogxv", OP_FOGFV, "iP" },
    { "glTexEnvfv", OP_TEXENVFV, "iip" },
    { "glTexEnvxv", OP_TEXENVFV, "iiP" },
    { "glTexEnviv", OP_TEXENVFV, "iiI" },
    { "glTexParameterfv", OP_TEXPARAMFV, "iip" },
    { "glTexParameterxv", OP_TEXPARAMFV, "iiP" },
    { "glTexParameteriv", OP_TEXPARAMFV, "iiI" },
    { "glPointParameterfv", OP_POINTPARAMFV, "ip" },
    { "glPointParameterxv", OP_POINTPARAMFV, "iP" },
    { "glClipPlanef", OP_CLIPPLANEF, "ip" },
    { "glClipPlanex", OP_CLIPPLANEF, "iP" },
    { "glLoadMatrixf", OP_LOADMATRIX, "p" },
    { "glLoadMatrixx", OP_LOADMATRIX, "P" },
    { "glMultMatrixf", OP_MULTMATRIX, "p" },
    { "glMultMatrixx", OP_MULTMATRIX, "P" },
    { "glVertexPointer", OP_VERTEXPTR, "iiip" },
    { "glNormalPointer", OP_NORMALPTR, "iip" },
    { "glColorPointer", OP_COLORPTR, "iiip" },
    { "glTexCoordPointer", OP_TEXCOORDPTR, "iiip" },
    { "glPointSizePointerOES", OP_POINTSIZEPTR, "iip" },
    { "glEnableClientState", OP_ENABLECLIENT, "i" },
    { "glDisableClientState", OP_DISABLECLIENT, "i" },
    { "glClientActiveTexture", OP_CLIENTACTIVETEX, "i" },
    { "glBindBuffer", OP_BINDBUFFER, "ii" },
    { "glGenBuffers", OP_GENBUFFERS, "ip" },
    { "glDeleteBuffers", OP_DELETEBUFFERS, "ip" },
    { "glBufferData", OP_BUFFERDATA, "iipi" },
    { "glBufferSubData", OP_BUFFERSUBDATA, "iiip" },
    { "glPixelStorei", OP_PIXELSTORE, "ii" },
    { "glGetString", OP_GETSTRING, "i" },
    { "glDrawArrays", OP_DRAWARRAYS, "iii" },
    { "glDrawElements", OP_DRAWELEMENTS, "iiip" },
    { "glTexImage2D", OP_TEXIMAGE2D, "iiiiiiiip" },
    { "glTexSubImage2D", OP_TEXSUBIMAGE2D, "iiiiiiiip" },
    { "glCompressedTexImage2D", OP_COMPRESSEDTEXIMAGE2D, "iiiiiiip" },
    { "glCompressedTexSubImage2D", OP_COMPRESSEDTEXSUBIMAGE2D, "iiiiiiiip" },
    { "glCopyTexImage2D", OP_COPYTEXIMAGE2D, "iiiiiiii" },
    { "glCopyTexSubImage2D", OP_COPYTEXSUBIMAGE2D, "iiiiiiii" },
    { "glReadPixels", OP_READPIXELS, "iiiiiip" },
    { "glGenTextures", OP_GENTEXTURES, "ip" },
    { "glDeleteTextures", OP_DELETETEXTURES, "ip" },
    { "glIsTexture", OP_ISTEXTURE, "i" },
    { "glIsEnabled", OP_ISENABLED, "i" },
    { "glGetFloatv", OP_GETFLOATV, "ip" },
    { "glGetIntegerv", OP_GETINTEGERV, "ip" },
    { "glGetBooleanv", OP_GETBOOLEANV, "ip" },
    { "glGetFixedv", OP_GETFIXEDV, "ip" },
    { "glGetLightfv", OP_GETLIGHTFV, "iip" },
    { "glGetLightxv", OP_GETLIGHTFV, "iiP" },
    { "glGetMaterialfv", OP_GETMATERIALFV, "iip" },
    { "glGetMaterialxv", OP_GETMATERIALFV, "iiP" },
    { "glGetTexEnvfv", OP_GETTEXENVFV, "iip" },
    { "glGetTexEnviv", OP_GETTEXENVFV, "iiI" },
    { "glGetTexEnvxv", OP_GETTEXENVFV, "iiP" },
    { "glGetTexParameterfv", OP_GETTEXPARAMFV, "iip" },
    { "glGetTexParameteriv", OP_GETTEXPARAMFV, "iiI" },
    { "glGetTexParameterxv", OP_GETTEXPARAMFV, "iiP" },
    { "glGetClipPlanef", OP_GETCLIPPLANEF, "ip" },
    { "glGetClipPlanex", OP_GETCLIPPLANEF, "iP" },
    { "glGetBufferParameteriv", OP_GETBUFFERPARAM, "iip" },
    { "glGetPointerv", OP_GETPOINTERV, "ip" },
    { "glDrawTexfOES", OP_DRAWTEX, "fffff" },
    { "glDrawTexxOES", OP_DRAWTEX, "xxxxx" },
    { "glDrawTexiOES", OP_DRAWTEX, "IIIII" },
    { "glDrawTexsOES", OP_DRAWTEX, "IIIII" },
    { "eglSwapBuffers", OP_SWAPBUFFERS, "ii" },
    { "eglWaitGL", OP_EGL_TRUE, "" },
    { "eglWaitNative", OP_EGL_TRUE, "i" },
    /*
     * GL_OES_framebuffer_object.  Desktop GL's ARB_framebuffer_object has the
     * same semantics, so these forward directly; the one translation needed is
     * framebuffer 0, which means "the EGL window surface" to the firmware and
     * is the renderer's read-back FBO here (fb_target()).
     */
    { "glBindFramebufferOES", OP_BINDFRAMEBUFFER, "ii" , true },
    { "glBindRenderbufferOES", OP_BINDRENDERBUFFER, "ii" , true },
    { "glGenFramebuffersOES", OP_GENFRAMEBUFFERS, "ip" , true },
    { "glGenRenderbuffersOES", OP_GENRENDERBUFFERS, "ip" , true },
    { "glDeleteFramebuffersOES", OP_DELETEFRAMEBUFFERS, "ip" , true },
    { "glDeleteRenderbuffersOES", OP_DELETERENDERBUFFERS, "ip" , true },
    { "glRenderbufferStorageOES", OP_RENDERBUFFERSTORAGE, "iiii" , true },
    { "glFramebufferRenderbufferOES", OP_FRAMEBUFFERRENDERBUFFER, "iiii" , true },
    { "glFramebufferTexture2DOES", OP_FRAMEBUFFERTEXTURE2D, "iiiii" , true },
    { "glCheckFramebufferStatusOES", OP_CHECKFRAMEBUFFER, "i" , true },
    { "glGenerateMipmapOES", OP_GENERATEMIPMAP, "i" , true },
    { "glEGLImageTargetTexture2DOES", OP_EGLIMAGE_TEX2D, "ii" , true },
    /*
     * EGL.  One host context and one FBO serve every surface the driver
     * makes, so surfaces are synthetic handles and making one current is a
     * no-op that must still report success - these used to fall through to
     * the "logged only" path, which returns 0 = EGL_FALSE and fails the
     * driver's error checks.
     */
    { "eglCreatePbufferSurface", OP_EGL_SURFACE, "iip" , true },
    { "eglCreatePixmapSurface", OP_EGL_SURFACE, "iiip" , true },
    { "eglDestroySurface", OP_EGL_TRUE, "ii" , true },
    { "eglQuerySurface", OP_EGL_QUERYSURFACE, "iiip" , true },
    { "eglMakeCurrent", OP_EGL_TRUE, "iiii" , true },
    { "eglGetCurrentSurface", OP_EGL_CURSURFACE, "i" , true },
    { "eglSwapInterval", OP_EGL_TRUE, "ii" , true },
    { "eglCreateImageKHR", OP_EGL_NO_IMAGE, "iiiip" , true },
    { "eglDestroyImageKHR", OP_EGL_TRUE, "ii" , true },
    { NULL }
};

typedef struct GlHook {
    char name[48];
    uint32_t addr;
    int nargs;
    const GlOpDef *def;
    bool disabled;      /* has an implementation, switched off */
    uint32_t calls;
} GlHook;

static GlHook hooks[GARMIN_GL_MAX_HOOKS];
static int nhooks;

typedef struct CodePatch {
    uint32_t addr;
    size_t len;
    uint8_t bytes[32];
} CodePatch;

static CodePatch patches[32];
static int npatches;
static uint32_t gl_result;
static uint32_t gl_width = 1024, gl_height = 600;
static uint32_t gl_scanout = 0xbed46000;

void garmin_gl_set_display(uint32_t w, uint32_t h)
{
    gl_width = w;
    gl_height = h;
}

void garmin_gl_set_scanout(uint32_t paddr)
{
    gl_scanout = paddr;
}

int garmin_gl_load_table(const char *path, Error **errp)
{
    garmin_gl_fbo = getenv("GARMIN_GL_FBO") &&
                    getenv("GARMIN_GL_FBO")[0] == '1';
    garmin_gl_drawtex_log = getenv("GARMIN_GL_DRAWTEX") &&
                            getenv("GARMIN_GL_DRAWTEX")[0] == '1';
    garmin_gl_resync = getenv("GARMIN_GL_RESYNC") &&
                       getenv("GARMIN_GL_RESYNC")[0] == '1';
    FILE *f = fopen(path, "r");
    char line[256];

    if (!f) {
        error_setg_errno(errp, errno, "gl-hooks: cannot open %s", path);
        return -1;
    }
    nhooks = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[48];
        unsigned addr;
        int nargs;
        GlHook *h;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        if (!strncmp(line, "patch ", 6)) {
            /* patch <addr> <hexbytes>: raw code patch applied with the hooks */
            char hex[128];

            if (sscanf(line + 6, "%x %127s", &addr, hex) == 2 &&
                npatches < ARRAY_SIZE(patches)) {
                CodePatch *p = &patches[npatches];
                size_t l = strlen(hex) / 2;

                p->addr = addr & ~1u;
                p->len = MIN(l, sizeof(p->bytes));
                for (size_t i = 0; i < p->len; i++) {
                    unsigned b;

                    sscanf(hex + 2 * i, "%2x", &b);
                    p->bytes[i] = b;
                }
                npatches++;
            }
            continue;
        }
        if (sscanf(line, "%47s %x %d", name, &addr, &nargs) != 3) {
            continue;
        }
        if (nhooks == GARMIN_GL_MAX_HOOKS) {
            break;
        }
        h = &hooks[nhooks++];
        pstrcpy(h->name, sizeof(h->name), name);
        h->addr = addr & ~1u;
        h->nargs = nargs;
        h->def = NULL;
        h->disabled = false;
        for (const GlOpDef *d = opdefs; d->name; d++) {
            if (!strcmp(d->name, name)) {
                if (d->opt_in && !garmin_gl_fbo) {
                    h->disabled = true;         /* stays logged-only */
                    break;
                }
                h->def = d;
                break;
            }
        }
        if (h->disabled) {
            warn_report("gl-hooks: %s is logged only "
                        "(implemented, off by default: GARMIN_GL_FBO=1)",
                        name);
        } else if (!h->def) {
            warn_report("gl-hooks: %s is logged only (no host implementation)",
                        name);
        }
    }
    fclose(f);
    return nhooks;
}

/* ---- trampoline encoding ------------------------------------------ */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8;
}

static void enc_mov_imm16(uint8_t *p, bool top, int rd, uint16_t imm)
{
    uint16_t imm4 = imm >> 12, i = (imm >> 11) & 1, imm3 = (imm >> 8) & 7,
             imm8 = imm & 0xff;
    put16(p, (top ? 0xf2c0 : 0xf240) | (i << 10) | imm4);
    put16(p + 2, (imm3 << 12) | (rd << 8) | imm8);
}

static int enc_trampoline(uint8_t *p, int slot)
{
    enc_mov_imm16(p, false, 12, GARMIN_GL_MMIO_BASE & 0xffff);
    enc_mov_imm16(p + 4, true, 12, GARMIN_GL_MMIO_BASE >> 16);
    put16(p + 8, 0xf8cc);                       /* str.w r12, [r12, #imm12] */
    put16(p + 10, 0xc000 | (slot * 4));
    put16(p + 12, 0xf8dc);                      /* ldr.w r0, [r12, #0xffc] */
    put16(p + 14, 0x0000 | GARMIN_GL_RESULT_OFF);
    put16(p + 16, 0x4770);                      /* bx lr */
    return 18;
}

void garmin_gl_reset(void *opaque)
{
    for (int i = 0; i < nhooks; i++) {
        uint8_t code[18];
        int n = enc_trampoline(code, i);

        address_space_write(&address_space_memory, hooks[i].addr,
                            MEMTXATTRS_UNSPECIFIED, code, n);
        hooks[i].calls = 0;
    }
    for (int i = 0; i < npatches; i++) {
        address_space_write(&address_space_memory, patches[i].addr,
                            MEMTXATTRS_UNSPECIFIED, patches[i].bytes,
                            patches[i].len);
    }
    if (nhooks) {
        qemu_log("garmin_gl: %d entry points hooked, %d code patches\n",
                 nhooks, npatches);
    }
}

/* ------------------------------------------------------------------ */
/* guest memory helpers (vCPU thread only)                               */
/* ------------------------------------------------------------------ */

static uint32_t guest_ld32(CPUState *cs, uint32_t va)
{
    uint32_t v = 0;

    cpu_memory_rw_debug(cs, va, &v, 4, false);
    return v;
}

static bool guest_read(CPUState *cs, uint32_t va, void *buf, size_t len)
{
    if (!va || !len) {
        return false;
    }
    return cpu_memory_rw_debug(cs, va, buf, len, false) == 0;
}

static void guest_write(CPUState *cs, uint32_t va, const void *buf, size_t len)
{
    if (va && len) {
        cpu_memory_rw_debug(cs, va, (void *)buf, len, true);
    }
}

static const char *task_name(CPUState *cs)
{
    static char name[17];
    uint32_t tcb = guest_ld32(cs, 0xA4AACAB8);

    name[0] = 0;
    if (tcb >= 0x80000000 && tcb != 0xffffffff) {
        cpu_memory_rw_debug(cs, tcb + 0x7c, name, 16, false);
        name[16] = 0;
    }
    return name;
}

static inline float fx2f(uint32_t x)
{
    return (float)(int32_t)x / 65536.0f;
}

static inline uint32_t f2fx(float f)
{
    return (uint32_t)(int32_t)(f * 65536.0f);
}

static inline float bits2f(uint32_t b)
{
    float f;

    memcpy(&f, &b, 4);
    return f;
}

static inline uint32_t f2bits(float f)
{
    uint32_t b;

    memcpy(&b, &f, 4);
    return b;
}

/* ------------------------------------------------------------------ */
/* client-side state (vCPU thread)                                       */
/* ------------------------------------------------------------------ */

enum { ARR_VERTEX, ARR_NORMAL, ARR_COLOR, ARR_TEX0, ARR_TEX1, ARR_TEX2,
       ARR_TEX3, ARR_POINTSIZE, ARR_COUNT };

typedef struct ClientArray {
    bool enabled;
    int size, type, stride;
    uint32_t ptr;                 /* guest pointer or VBO offset */
    uint32_t buffer;              /* VBO bound when the pointer was set */
} ClientArray;

typedef struct GlBuffer {
    uint32_t id;
    uint8_t *data;
    size_t size;
} GlBuffer;

static ClientArray arrays[ARR_COUNT];
static int client_tex_unit;
static uint32_t bound_array_buf, bound_elem_buf;
static GHashTable *buffers;       /* id -> GlBuffer */
static uint32_t next_buffer_id = 1;
static int unpack_alignment = 4;

/*
 * The PowerVR driver uploads texel data lazily (at first use), and the
 * firmware relies on that: it calls glTexImage2D on a buffer it fills
 * afterwards.  We remember every client-memory texture source and, before
 * each draw, re-read the bound texture; if the bytes changed it is
 * re-uploaded.
 */
typedef struct TexSrc {
    uint32_t ptr;
    int w, h;
    uint32_t fmt, type;
    size_t len;
    uint64_t hash;
    bool dirty_check;                 /* large: re-read at swap time */
    bool synced_frame;                /* uploaded by a queued call this frame */
} TexSrc;

static GHashTable *texsrcs;       /* texture id -> TexSrc */
/*
 * GL_TEXTURE_CROP_RECT_OES per texture.  Desktop GL has no such texture
 * parameter, so forwarding it only produced GL_INVALID_ENUM and the
 * rectangle was lost; glDrawTexOES then stretched the whole texture over
 * the target rectangle, which is why the screen-aligned chrome strips (page
 * header, softkey bar) came out as unrelated texels while everything drawn
 * with ordinary quads was correct.  Kept here and applied in OP_DRAWTEX.
 */
static GHashTable *crops;         /* texture id -> int32_t[4] */
/*
 * Textures that have been attached to a framebuffer.  Their content is
 * produced by the GPU, so the swap-time re-read below must leave them alone:
 * guest memory holds nothing for them, and re-uploading it blanked every
 * bitmap layer (text survived because glyph textures are copied at call
 * time) - the GUI flickered between complete and text-only frames.
 */
static GHashTable *gpu_textures;
/*
 * The texture the per-texture state below is recorded against.  Strictly a
 * binding is per texture unit, but tracking it that way made the GUI worse,
 * not better: the compositor uploads its layers with an active unit we have
 * seen no glBindTexture for, so the per-unit binding reads back as 0 and the
 * layer stops being re-read from guest memory (wide bands of stale texels
 * across the page background).  "Last glBindTexture" matches what this
 * driver actually does.
 */
static int server_tex_unit;             /* GL_ACTIVE_TEXTURE, for logging */
static uint32_t bound_texture;

/* The crop rectangle of a texture; all zero when it never set one. */
static const int32_t *crop_of(uint32_t tex)
{
    static const int32_t none[4];
    const int32_t *c = tex ? g_hash_table_lookup(crops,
                                                 GUINT_TO_POINTER(tex)) : NULL;

    return c ? c : none;
}

static uint64_t hash_bytes(const uint8_t *p, size_t len)
{
    uint64_t h = 1469598103934665603ull;

    for (size_t i = 0; i < len; i += 4) {
        h = (h ^ *(const uint32_t *)(p + i)) * 1099511628211ull;
    }
    return h;
}

static GlBuffer *buffer_lookup(uint32_t id)
{
    return id ? g_hash_table_lookup(buffers, GUINT_TO_POINTER(id)) : NULL;
}

/* ------------------------------------------------------------------ */
/* request passed to the render thread                                   */
/* ------------------------------------------------------------------ */

typedef struct ArrayBlob {
    bool enabled;
    int size;                     /* components */
    size_t off;                   /* float offset into req.in */
} ArrayBlob;

typedef struct GlReq {
    enum GlOp op;
    uint32_t a[16];               /* decoded args: ints or float bits */
    uint8_t *in;
    size_t in_len, in_cap;
    uint8_t *out;
    size_t out_len;
    uint32_t result;
    /* draws */
    ArrayBlob arr[ARR_COUNT];
    int nverts;
    size_t idx_off;               /* byte offset of indices in `in` */
    int idx_type, idx_count;
} GlReq;

static GlReq req;
static GlReq *r_cur;                  /* request the render thread executes */
static QemuThread rthread;
static QemuMutex rlock;
static QemuCond rcond_req, rcond_done;
static bool r_pending, r_started, r_ok;

static size_t req_push(GlReq *r, const void *p, size_t len)
{
    size_t off = ROUND_UP(r->in_len, 16);

    if (off + len > r->in_cap) {
        r->in_cap = MAX(r->in_cap * 2, off + len + 4096);
        r->in = g_realloc(r->in, r->in_cap);
    }
    if (p) {
        memcpy(r->in + off, p, len);
    } else {
        memset(r->in + off, 0, len);
    }
    r->in_len = off + len;
    return off;
}

static void *req_out(GlReq *r, size_t len)
{
    g_free(r->out);
    r->out = g_malloc0(len);
    r->out_len = len;
    return r->out;
}

/* ------------------------------------------------------------------ */
/* render thread                                                          */
/* ------------------------------------------------------------------ */

#ifdef GARMIN_GL_HOST_RENDER

static GLuint fbo, fbo_color, fbo_depth;

/*
 * The renderer needs a current desktop-GL *compatibility* context (the
 * GLES1 fixed-function pipeline is forwarded 1:1) with framebuffer objects,
 * but no window: everything is drawn into an FBO and read back.  How that
 * context is obtained is the only host-specific part of this file.
 */
#ifdef _WIN32

/*
 * WGL: Windows has no surfaceless context, so create a hidden window and
 * make its DC current.  A legacy wglCreateContext context is a full
 * compatibility context (GL 4.6 on the stock AMD/NVIDIA/Intel drivers),
 * which is exactly what is needed; dropping Mesa's opengl32.dll next to
 * the binary gives a software (llvmpipe) fallback.
 */
static HWND   wgl_wnd;
static HDC    wgl_dc;
static HGLRC  wgl_ctx;

static bool render_ctx_init(void)
{
    static const PIXELFORMATDESCRIPTOR pfd = {
        .nSize = sizeof(PIXELFORMATDESCRIPTOR),
        .nVersion = 1,
        .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL,
        .iPixelType = PFD_TYPE_RGBA,
        .cColorBits = 32,
        .cDepthBits = 24,
        .cStencilBits = 8,
        .iLayerType = PFD_MAIN_PLANE,
    };
    WNDCLASSEXA wc = {
        .cbSize = sizeof(WNDCLASSEXA),
        .lpfnWndProc = DefWindowProcA,
        .hInstance = GetModuleHandle(NULL),
        .lpszClassName = "garmin_gl_hidden",
    };
    int fmt;

    if (!RegisterClassExA(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error_report("garmin_gl: RegisterClass failed (%lu)",
                     (unsigned long)GetLastError());
        return false;
    }
    wgl_wnd = CreateWindowExA(0, wc.lpszClassName, "garmin_gl",
                              WS_OVERLAPPEDWINDOW, 0, 0, 16, 16,
                              NULL, NULL, wc.hInstance, NULL);
    if (!wgl_wnd) {
        error_report("garmin_gl: CreateWindow failed (%lu)",
                     (unsigned long)GetLastError());
        return false;
    }
    wgl_dc = GetDC(wgl_wnd);
    fmt = wgl_dc ? ChoosePixelFormat(wgl_dc, &pfd) : 0;
    if (!fmt || !SetPixelFormat(wgl_dc, fmt, &pfd)) {
        error_report("garmin_gl: no usable pixel format (%lu)",
                     (unsigned long)GetLastError());
        return false;
    }
    wgl_ctx = wglCreateContext(wgl_dc);
    if (!wgl_ctx || !wglMakeCurrent(wgl_dc, wgl_ctx)) {
        error_report("garmin_gl: cannot create/make current GL context (%lu)",
                     (unsigned long)GetLastError());
        return false;
    }
    return true;
}

#else /* !_WIN32 */

static EGLDisplay egl_dpy;
static EGLContext egl_ctx;
static EGLSurface egl_surf;

static bool render_ctx_init(void)
{
    EGLint major, minor, n;
    EGLConfig cfg;
    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    static const EGLint pb_attr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };

    egl_dpy = EGL_NO_DISPLAY;
    if (epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        egl_dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                           EGL_DEFAULT_DISPLAY, NULL);
    }
    if (egl_dpy == EGL_NO_DISPLAY) {
        egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (egl_dpy == EGL_NO_DISPLAY || !eglInitialize(egl_dpy, &major, &minor)) {
        error_report("garmin_gl: no EGL display (0x%x)", eglGetError());
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        error_report("garmin_gl: EGL has no desktop OpenGL");
        return false;
    }
    if (!eglChooseConfig(egl_dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        error_report("garmin_gl: no EGL config");
        return false;
    }
    egl_surf = eglCreatePbufferSurface(egl_dpy, cfg, pb_attr);
    egl_ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, NULL);
    if (egl_ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx)) {
        error_report("garmin_gl: cannot create/make current GL context (0x%x)",
                     eglGetError());
        return false;
    }
    return true;
}

#endif /* !_WIN32 */

static bool render_init(void)
{
    if (!render_ctx_init()) {
        return false;
    }
    qemu_log("garmin_gl: host GL %s / %s / %s\n", glGetString(GL_VENDOR),
             glGetString(GL_RENDERER), glGetString(GL_VERSION));
    if (!epoxy_is_desktop_gl() || epoxy_gl_version() < 20) {
        error_report("garmin_gl: need desktop OpenGL 2.0 or later");
        return false;
    }

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &fbo_color);
    glBindRenderbuffer(GL_RENDERBUFFER, fbo_color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, gl_width, gl_height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, fbo_color);
    glGenRenderbuffers(1, &fbo_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, fbo_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, gl_width, gl_height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, fbo_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        error_report("garmin_gl: FBO incomplete");
        return false;
    }
    glViewport(0, 0, gl_width, gl_height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return true;
}

/*
 * Framebuffer 0 is the window surface for the firmware; for us it is the FBO
 * whose pixels eglSwapBuffers reads back into the DISPC scan-out buffer.
 * Host FBO names are handed to the guest unchanged, and the host allocator
 * already knows about `fbo`, so no other name can collide with it.
 */
static GLuint fb_target(GLuint fb)
{
    return fb ? fb : fbo;
}

/* The one GLES renderbuffer format desktop GL spells differently */
static GLenum rb_format(GLenum fmt)
{
    if (fmt == 0x8d62) {                          /* GL_RGB565_OES */
        return epoxy_gl_version() >= 41 ? GL_RGB565 : GL_RGB5;
    }
    return fmt;              /* RGBA4, RGB5_A1, DEPTH_COMPONENT16, STENCIL8 */
}

#define I(n)  ((GLint)r->a[n])
#define U(n)  ((GLuint)r->a[n])
#define F(n)  (bits2f(r->a[n]))
#define FV(off) ((const GLfloat *)(r->in + (off)))

static void render_draw_setup(GlReq *r)
{
    static const GLenum names[] = {
        GL_VERTEX_ARRAY, GL_NORMAL_ARRAY, GL_COLOR_ARRAY,
        GL_TEXTURE_COORD_ARRAY, GL_TEXTURE_COORD_ARRAY,
        GL_TEXTURE_COORD_ARRAY, GL_TEXTURE_COORD_ARRAY
    };

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    for (int i = 0; i < ARR_POINTSIZE; i++) {
        ArrayBlob *b = &r->arr[i];

        if (i >= ARR_TEX0) {
            glClientActiveTexture(GL_TEXTURE0 + (i - ARR_TEX0));
        }
        if (!b->enabled) {
            glDisableClientState(names[i]);
            continue;
        }
        glEnableClientState(names[i]);
        switch (i) {
        case ARR_VERTEX:
            glVertexPointer(b->size, GL_FLOAT, 0, FV(b->off));
            break;
        case ARR_NORMAL:
            glNormalPointer(GL_FLOAT, 0, FV(b->off));
            break;
        case ARR_COLOR:
            glColorPointer(b->size, GL_FLOAT, 0, FV(b->off));
            break;
        default:
            glTexCoordPointer(b->size, GL_FLOAT, 0, FV(b->off));
            break;
        }
    }
    glClientActiveTexture(GL_TEXTURE0);
}

static void render_exec(GlReq *r)
{
    r->result = 0;
    switch (r->op) {
    case OP_BLENDFUNC:     glBlendFunc(U(0), U(1)); break;
    case OP_DEPTHFUNC:     glDepthFunc(U(0)); break;
    case OP_CULLFACE:      glCullFace(U(0)); break;
    case OP_FRONTFACE:     glFrontFace(U(0)); break;
    case OP_MATRIXMODE:    glMatrixMode(U(0)); break;
    case OP_ENABLE:        glEnable(U(0)); break;
    case OP_DISABLE:       glDisable(U(0)); break;
    case OP_BINDTEXTURE:   glBindTexture(U(0), U(1)); break;
    case OP_BLENDFUNCSEP:  glBlendFuncSeparate(U(0), U(1), U(2), U(3)); break;
    case OP_POINTSIZE:     glPointSize(F(0)); break;
    case OP_COLOR4F:       glColor4f(F(0), F(1), F(2), F(3)); break;
    case OP_DEPTHRANGE:    glDepthRange(F(0), F(1)); break;
    case OP_SCALE:         glScalef(F(0), F(1), F(2)); break;
    case OP_TRANSLATE:     glTranslatef(F(0), F(1), F(2)); break;
    case OP_ROTATE:        glRotatef(F(0), F(1), F(2), F(3)); break;
    case OP_FRUSTUM:       glFrustum(F(0), F(1), F(2), F(3), F(4), F(5)); break;
    case OP_ORTHO:         glOrtho(F(0), F(1), F(2), F(3), F(4), F(5)); break;
    case OP_TEXPARAMF:     glTexParameterf(U(0), U(1), F(2)); break;
    case OP_TEXPARAMI:     glTexParameteri(U(0), U(1), I(2)); break;
    case OP_TEXENVF:       glTexEnvf(U(0), U(1), F(2)); break;
    case OP_TEXENVI:       glTexEnvi(U(0), U(1), I(2)); break;
    case OP_LINEWIDTH:     glLineWidth(F(0)); break;
    case OP_CLEARDEPTH:    glClearDepth(F(0)); break;
    case OP_DEPTHMASK:     glDepthMask(U(0)); break;
    case OP_COLORMASK:     glColorMask(U(0), U(1), U(2), U(3)); break;
    case OP_SHADEMODEL:    glShadeModel(U(0)); break;
    case OP_LOGICOP:       glLogicOp(U(0)); break;
    case OP_CLEAR:         glClear(U(0)); break;
    case OP_CLEARCOLOR:    glClearColor(F(0), F(1), F(2), F(3)); break;
    case OP_SCISSOR:       glScissor(I(0), I(1), I(2), I(3)); break;
    case OP_VIEWPORT:      glViewport(I(0), I(1), I(2), I(3)); break;
    case OP_LOADIDENTITY:  glLoadIdentity(); break;
    case OP_PUSHMATRIX:    glPushMatrix(); break;
    case OP_POPMATRIX:     glPopMatrix(); break;
    case OP_ACTIVETEXTURE: glActiveTexture(U(0)); break;
    case OP_ALPHAFUNC:     glAlphaFunc(U(0), F(1)); break;
    case OP_CLEARSTENCIL:  glClearStencil(I(0)); break;
    case OP_STENCILFUNC:   glStencilFunc(U(0), I(1), U(2)); break;
    case OP_STENCILOP:     glStencilOp(U(0), U(1), U(2)); break;
    case OP_STENCILMASK:   glStencilMask(U(0)); break;
    case OP_POLYGONOFFSET: glPolygonOffset(F(0), F(1)); break;
    case OP_SAMPLECOVERAGE: glSampleCoverage(F(0), U(1)); break;
    case OP_HINT:          glHint(U(0), U(1)); break;
    case OP_LIGHTF:        glLightf(U(0), U(1), F(2)); break;
    case OP_LIGHTMODELF:   glLightModelf(U(0), F(1)); break;
    case OP_MATERIALF:     glMaterialf(U(0), U(1), F(2)); break;
    case OP_FOGF:          glFogf(U(0), F(1)); break;
    case OP_POINTPARAMF:   glPointParameterf(U(0), F(1)); break;
    case OP_NORMAL3F:      glNormal3f(F(0), F(1), F(2)); break;
    case OP_MULTITEXCOORD4F: glMultiTexCoord4f(U(0), F(1), F(2), F(3), F(4)); break;
    case OP_FLUSH:         glFlush(); break;
    case OP_FINISH:        glFinish(); break;
    case OP_GETERROR:      r->result = glGetError(); return;
    case OP_LIGHTFV:       glLightfv(U(0), U(1), FV(0)); break;
    case OP_LIGHTMODELFV:  glLightModelfv(U(0), FV(0)); break;
    case OP_MATERIALFV:    glMaterialfv(U(0), U(1), FV(0)); break;
    case OP_FOGFV:         glFogfv(U(0), FV(0)); break;
    case OP_TEXENVFV:      glTexEnvfv(U(0), U(1), FV(0)); break;
    case OP_TEXPARAMFV:    glTexParameterfv(U(0), U(1), FV(0)); break;
    case OP_POINTPARAMFV:  glPointParameterfv(U(0), FV(0)); break;
    case OP_CLIPPLANEF: {
        GLdouble eq[4] = { FV(0)[0], FV(0)[1], FV(0)[2], FV(0)[3] };
        glClipPlane(U(0), eq);
        break;
    }
    case OP_LOADMATRIX:    glLoadMatrixf(FV(0)); break;
    case OP_MULTMATRIX:    glMultMatrixf(FV(0)); break;
    case OP_DRAWARRAYS:
        render_draw_setup(r);
        glDrawArrays(U(0), 0, r->nverts);
        break;
    case OP_DRAWELEMENTS:
        render_draw_setup(r);
        glDrawElements(U(0), r->idx_count, r->idx_type, r->in + r->idx_off);
        break;
    case OP_TEXIMAGE2D:
        glPixelStorei(GL_UNPACK_ALIGNMENT, I(9));
        glTexImage2D(U(0), I(1), I(2), I(3), I(4), I(5), U(6), U(7),
                     r->in_len ? r->in : NULL);
        break;
    case OP_TEXSUBIMAGE2D:
        glPixelStorei(GL_UNPACK_ALIGNMENT, I(9));
        glTexSubImage2D(U(0), I(1), I(2), I(3), I(4), I(5), U(6), U(7), r->in);
        break;
    case OP_COMPRESSEDTEXIMAGE2D:
        /* a[2] was rewritten to an uncompressed format by the decoder */
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(U(0), I(1), I(2), I(3), I(4), 0, U(2), GL_UNSIGNED_BYTE,
                     r->in);
        break;
    case OP_COMPRESSEDTEXSUBIMAGE2D:
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(U(0), I(1), I(2), I(3), I(4), I(5), U(6),
                        GL_UNSIGNED_BYTE, r->in);
        break;
    case OP_COPYTEXIMAGE2D:
        glCopyTexImage2D(U(0), I(1), U(2), I(3), I(4), I(5), I(6), I(7));
        break;
    case OP_COPYTEXSUBIMAGE2D:
        glCopyTexSubImage2D(U(0), I(1), I(2), I(3), I(4), I(5), I(6), I(7));
        break;
    case OP_READPIXELS:
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(I(0), I(1), I(2), I(3), U(4), U(5), r->out);
        break;
    case OP_GENTEXTURES:
        glGenTextures(I(0), (GLuint *)r->out);
        break;
    case OP_DELETETEXTURES:
        glDeleteTextures(I(0), (const GLuint *)r->in);
        break;
    case OP_ISTEXTURE:     r->result = glIsTexture(U(0)); return;
    case OP_ISENABLED:     r->result = glIsEnabled(U(0)); return;
    case OP_GETFLOATV: case OP_GETINTEGERV: case OP_GETBOOLEANV:
    case OP_GETFIXEDV:
        glGetFloatv(U(0), (GLfloat *)r->out);
        break;
    case OP_GETLIGHTFV:    glGetLightfv(U(0), U(1), (GLfloat *)r->out); break;
    case OP_GETMATERIALFV: glGetMaterialfv(U(0), U(1), (GLfloat *)r->out); break;
    case OP_GETTEXENVFV:   glGetTexEnvfv(U(0), U(1), (GLfloat *)r->out); break;
    case OP_GETTEXPARAMFV: glGetTexParameterfv(U(0), U(1), (GLfloat *)r->out); break;
    case OP_GETCLIPPLANEF: {
        GLdouble eq[4];
        GLfloat *o = (GLfloat *)r->out;

        glGetClipPlane(U(0), eq);
        for (int i = 0; i < 4; i++) {
            o[i] = eq[i];
        }
        break;
    }
    case OP_DRAWTEX: {
        /*
         * GL_OES_draw_texture: a screen-aligned quad at (x,y,z) of size
         * (w,h) showing the part of the bound texture that its crop
         * rectangle (Ucr,Vcr,Wcr,Hcr) selects, per the extension:
         *     s = (Ucr + Wcr * (xs - x) / w) / tw
         *     t = (Vcr + Hcr * (ys - y) / h) / th
         * A zero rectangle means the firmware never set one; fall back to
         * the whole texture, which is what this used to always do.
         */
        GLint vp[4], tw = 0, th = 0;
        float x = F(0), y = F(1), z = F(2), w = F(3), h = F(4);
        float cu = F(5), cv = F(6), cw = F(7), ch = F(8);
        float s0, t0, s1, t1;
        float v[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
        float t[8];

        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
        if (tw <= 0 || th <= 0) {
            tw = th = 1;
        }
        if (cw == 0 && ch == 0) {
            cu = 0; cv = 0; cw = tw; ch = th;
        }
        s0 = cu / tw; t0 = cv / th;
        s1 = (cu + cw) / tw; t1 = (cv + ch) / th;
        if (garmin_gl_drawtex_log) {
            qemu_log("garmin_gl: drawtex tex=%u unit=%u dest=%g,%g %gx%g "
                     "crop=%g,%g %gx%g texsize=%dx%d st=%g,%g..%g,%g\n",
                     (unsigned)r->a[9], (unsigned)r->a[10], x, y, w, h,
                     cu, cv, cw, ch, tw, th, s0, t0, s1, t1);
        }
        t[0] = s0; t[1] = t0;
        t[2] = s1; t[3] = t0;
        t[4] = s1; t[5] = t1;
        t[6] = s0; t[7] = t1;

        glGetIntegerv(GL_VIEWPORT, vp);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(vp[0], vp[0] + vp[2], vp[1], vp[1] + vp[3], -1, 1);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glTranslatef(0, 0, -z);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(2, GL_FLOAT, 0, v);
        glTexCoordPointer(2, GL_FLOAT, 0, t);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION); glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        break;
    }
    /* ---- GL_OES_framebuffer_object ---- */
    case OP_BINDFRAMEBUFFER:
        glBindFramebuffer(GL_FRAMEBUFFER, fb_target(U(1)));
        break;
    case OP_BINDRENDERBUFFER:
        glBindRenderbuffer(GL_RENDERBUFFER, U(1));
        break;
    case OP_RENDERBUFFERSTORAGE:
        glRenderbufferStorage(GL_RENDERBUFFER, rb_format(U(1)), I(2), I(3));
        break;
    case OP_FRAMEBUFFERRENDERBUFFER:
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, U(1), GL_RENDERBUFFER, U(3));
        break;
    case OP_FRAMEBUFFERTEXTURE2D:
        glFramebufferTexture2D(GL_FRAMEBUFFER, U(1), U(2), U(3), I(4));
        break;
    case OP_GENERATEMIPMAP:
        glGenerateMipmap(U(0));
        break;
    case OP_GENFRAMEBUFFERS:
        glGenFramebuffers(I(0), (GLuint *)r->out);
        break;
    case OP_GENRENDERBUFFERS:
        glGenRenderbuffers(I(0), (GLuint *)r->out);
        break;
    case OP_DELETEFRAMEBUFFERS: {
        GLint bound = 0;

        glDeleteFramebuffers(I(0), (const GLuint *)r->in);
        /*
         * Deleting the bound framebuffer rebinds 0, which here is the hidden
         * window/pbuffer rather than the buffer that gets presented, so
         * everything drawn afterwards would vanish.
         */
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
        if (bound == 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        }
        break;
    }
    case OP_DELETERENDERBUFFERS:
        glDeleteRenderbuffers(I(0), (const GLuint *)r->in);
        break;
    case OP_CHECKFRAMEBUFFER:
        r->result = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        return;
    case OP_EGLIMAGE_TEX2D:
        /*
         * An EGLImage wraps a native buffer whose layout we do not know.
         * eglCreateImageKHR therefore reports EGL_NO_IMAGE_KHR so the driver
         * uploads texels instead (a path the renderer implements), and this
         * call should not be reachable.
         */
        warn_report_once("garmin_gl: glEGLImageTargetTexture2DOES ignored "
                         "(no host EGLImage)");
        break;
    case OP_SWAPBUFFERS: {
        GLint bound = 0;

        /* Present the window surface, whatever the firmware left bound. */
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFinish();
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, gl_width, gl_height, GL_RGBA, GL_UNSIGNED_BYTE,
                     r->out);
        if (bound > 0 && (GLuint)bound != fbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, bound);
        }
        r->result = 1;
        return;
    }
    default:
        return;
    }
    r->result = 0;
}

static void *render_thread(void *arg)
{
    rcu_register_thread();
    qemu_mutex_lock(&rlock);
    r_ok = render_init();
    r_started = true;
    qemu_cond_signal(&rcond_done);
    while (true) {
        while (!r_pending) {
            qemu_cond_wait(&rcond_req, &rlock);
        }
        if (r_ok) {
            render_exec(r_cur);
        }
        r_pending = false;
        qemu_cond_signal(&rcond_done);
    }
    return NULL;
}

#else /* !GARMIN_GL_HOST_RENDER */

static void *render_thread(void *arg)
{
    qemu_mutex_lock(&rlock);
    r_ok = false;
    r_started = true;
    qemu_cond_signal(&rcond_done);
    while (true) {
        while (!r_pending) {
            qemu_cond_wait(&rcond_req, &rlock);
        }
        r_pending = false;
        qemu_cond_signal(&rcond_done);
    }
    return NULL;
}

#endif

/* run one request synchronously on the render thread (rlock held) */
static void render_call_req(GlReq *r)
{
    r_cur = r;
    r_pending = true;
    qemu_cond_signal(&rcond_req);
    while (r_pending) {
        qemu_cond_wait(&rcond_done, &rlock);
    }
}

static void render_call(void)
{
    render_call_req(&req);
}

/*
 * Frame queue.  The PowerVR driver only consumes texture memory when the
 * frame is kicked (eglSwapBuffers), and the firmware relies on it: the
 * compositor draws its software-rendered layer into a texture *after*
 * calling glTexImage2D/glDrawArrays for it.  Reading texels at call time
 * gave stale or half-written layers.  So state and draw calls are queued
 * and executed at swap, when large client textures are (re)read from guest
 * memory; queries flush the queue first.
 */
static GQueue frame_q = G_QUEUE_INIT;
static uint32_t bound_at_swap;

static GlReq *req_clone(const GlReq *r)
{
    GlReq *c = g_memdup2(r, sizeof(*r));

    c->in = r->in_len ? g_memdup2(r->in, r->in_len) : NULL;
    c->in_cap = r->in_len;
    c->out = NULL;
    c->out_len = 0;
    return c;
}

static void req_free(GlReq *r)
{
    g_free(r->in);
    g_free(r->out);
    g_free(r);
}

static void frame_flush(void)
{
    GlReq *e;

    while ((e = g_queue_pop_head(&frame_q))) {
        render_call_req(e);
        req_free(e);
    }
}

static bool op_is_query(enum GlOp op)
{
    switch (op) {
    case OP_GETERROR: case OP_ISTEXTURE: case OP_ISENABLED: case OP_READPIXELS:
    case OP_GENTEXTURES: case OP_GETFLOATV: case OP_GETINTEGERV:
    case OP_GETBOOLEANV: case OP_GETFIXEDV: case OP_GETLIGHTFV:
    case OP_GETMATERIALFV: case OP_GETTEXENVFV: case OP_GETTEXPARAMFV:
    case OP_GETCLIPPLANEF: case OP_FINISH: case OP_FLUSH:
    case OP_GENFRAMEBUFFERS: case OP_GENRENDERBUFFERS:
    case OP_CHECKFRAMEBUFFER:
        return true;
    default:
        return false;
    }
}

static void render_start(void)
{
    static bool started;

    if (started) {
        return;
    }
    started = true;
    qemu_mutex_init(&rlock);
    qemu_cond_init(&rcond_req);
    qemu_cond_init(&rcond_done);
    buffers = g_hash_table_new(NULL, NULL);
    texsrcs = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    crops = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    gpu_textures = g_hash_table_new(NULL, NULL);
    garmin_gl_log = getenv("GARMIN_GL_LOG") && getenv("GARMIN_GL_LOG")[0] == '1';
    qemu_mutex_lock(&rlock);
    qemu_thread_create(&rthread, "garmin-gl", render_thread, NULL,
                       QEMU_THREAD_JOINABLE);
    while (!r_started) {
        qemu_cond_wait(&rcond_done, &rlock);
    }
    qemu_mutex_unlock(&rlock);
    if (!r_ok) {
        warn_report("garmin_gl: host renderer unavailable, GL calls are no-ops");
    }
}

/* ------------------------------------------------------------------ */
/* vCPU side: decode, marshal, complete                                  */
/* ------------------------------------------------------------------ */

static int type_size(int type)
{
    switch (type) {
    case 0x1400: case 0x1401: return 1;         /* BYTE / UNSIGNED_BYTE */
    case 0x1402: case 0x1403: return 2;         /* SHORT / UNSIGNED_SHORT */
    default:                  return 4;         /* FIXED / FLOAT / INT */
    }
}

/* convert one component to float; `norm` for colors/normals */
static float comp_to_float(const uint8_t *p, int type, bool norm)
{
    switch (type) {
    case 0x1400: return norm ? (int8_t)*p / 127.0f : (int8_t)*p;
    case 0x1401: return norm ? *p / 255.0f : *p;
    case 0x1402: { int16_t v = p[0] | (p[1] << 8);
                   return norm ? v / 32767.0f : v; }
    case 0x1403: { uint16_t v = p[0] | (p[1] << 8);
                   return norm ? v / 65535.0f : v; }
    case 0x140c: return fx2f(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
    case 0x1406: return bits2f(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
    default:     return 0;
    }
}

/* Copy `count` vertices of one array (starting at `first`) into req as floats */
static bool marshal_array(CPUState *cs, GlReq *r, int idx, int first, int count)
{
    ClientArray *a = &arrays[idx];
    ArrayBlob *b = &r->arr[idx];
    int tsz = type_size(a->type);
    int stride = a->stride ? a->stride : a->size * tsz;
    bool norm = idx == ARR_COLOR || idx == ARR_NORMAL;
    size_t need = (size_t)stride * (first + count);
    g_autofree uint8_t *raw = NULL;
    const uint8_t *src;
    float *dst;

    b->enabled = a->enabled;
    if (!a->enabled || count <= 0) {
        return true;
    }
    if (a->buffer) {
        GlBuffer *buf = buffer_lookup(a->buffer);

        if (!buf || a->ptr + need > buf->size + stride) {
            return false;
        }
        src = buf->data + a->ptr;
    } else {
        raw = g_malloc(need);
        if (!guest_read(cs, a->ptr, raw, need)) {
            return false;
        }
        src = raw;
    }
    b->size = a->size;
    b->off = req_push(r, NULL, (size_t)count * a->size * 4);
    dst = (float *)(r->in + b->off);
    for (int v = 0; v < count; v++) {
        const uint8_t *p = src + (size_t)(first + v) * stride;

        for (int c = 0; c < a->size; c++) {
            dst[v * a->size + c] = comp_to_float(p + c * tsz, a->type, norm);
        }
    }
    return true;
}

static bool marshal_draw(CPUState *cs, GlReq *r, int first, int count)
{
    static const char *names[] = { "v", "n", "c", "t0", "t1", "t2", "t3" };

    r->nverts = count;
    for (int i = 0; i < ARR_POINTSIZE; i++) {
        if (!marshal_array(cs, r, i, first, count)) {
            return false;
        }
    }
    /* small draws (UI quads) are logged with their converted arrays */
    if (garmin_gl_log && count <= 6) {
        for (int i = 0; i < ARR_POINTSIZE; i++) {
            ArrayBlob *b = &r->arr[i];
            const float *f = (const float *)(r->in + b->off);
            char buf[400];
            int len = 0;

            if (!b->enabled) {
                continue;
            }
            for (int k = 0; k < count * b->size && len < 360; k++) {
                len += snprintf(buf + len, sizeof(buf) - len, "%s%g",
                                k && k % b->size == 0 ? " | " : k ? "," : "", f[k]);
            }
            qemu_log("gl:   %s[%d]: %s\n", names[i], b->size, buf);
        }
    }
    return true;
}

static int texel_bytes(uint32_t format, uint32_t type)
{
    switch (type) {
    case 0x8033: case 0x8034: case 0x8363:      /* 4444 / 5551 / 565 */
        return 2;
    default:
        break;
    }
    switch (format) {
    case 0x1907: return 3;                      /* GL_RGB */
    case 0x1908: return 4;                      /* GL_RGBA */
    case 0x190a: return 2;                      /* GL_LUMINANCE_ALPHA */
    case 0x80e1: return 4;                      /* GL_BGRA */
    default:     return 1;                      /* ALPHA / LUMINANCE */
    }
}

static size_t image_bytes(int w, int h, uint32_t format, uint32_t type)
{
    size_t row = (size_t)w * texel_bytes(format, type);

    row = ROUND_UP(row, unpack_alignment);
    return row * h;
}

/*
 * Compressed formats the host lacks are decoded to RGBA8 here.
 * Supported: GL_PALETTE*_OES (0x8b90..0x8b99).  PVRTC/ETC1 produce a
 * flat grey placeholder so the UI stays readable.
 */
static bool decode_compressed(GlReq *r, uint32_t fmt, int w, int h, int level,
                              const uint8_t *data, size_t len, uint32_t *outfmt)
{
    size_t npix = (size_t)w * h;
    uint8_t *dst;

    *outfmt = 0x1908;                           /* GL_RGBA */
    if (fmt >= 0x8b90 && fmt <= 0x8b99) {
        static const int pal_bpp[] = { 3, 4, 2, 2, 2, 3, 4, 2, 2, 2 };
        static const int pal_bits[] = { 4, 4, 4, 4, 4, 8, 8, 8, 8, 8 };
        static const uint32_t pal_type[] = { 0x1401, 0x1401, 0x8363, 0x8033,
                                             0x8034, 0x1401, 0x1401, 0x8363,
                                             0x8033, 0x8034 };
        int k = fmt - 0x8b90;
        int entries = 1 << pal_bits[k];
        size_t pal_len = (size_t)entries * pal_bpp[k];
        const uint8_t *pal = data, *idx = data + pal_len;

        if (len < pal_len) {
            return false;
        }
        /* only the base level of a mip chain (level <= 0 encodes the count) */
        dst = r->in + req_push(r, NULL, npix * 4);
        for (size_t i = 0; i < npix; i++) {
            int e;
            const uint8_t *c;
            uint8_t rgba[4] = { 0, 0, 0, 255 };

            if (pal_bits[k] == 4) {
                e = (idx[i / 2] >> ((i & 1) ? 0 : 4)) & 0xf;
            } else {
                e = idx[i];
            }
            c = pal + e * pal_bpp[k];
            switch (pal_type[k]) {
            case 0x1401:
                rgba[0] = c[0]; rgba[1] = c[1]; rgba[2] = c[2];
                if (pal_bpp[k] == 4) {
                    rgba[3] = c[3];
                }
                break;
            case 0x8363: {
                uint16_t p = c[0] | (c[1] << 8);
                rgba[0] = (p >> 11) << 3; rgba[1] = ((p >> 5) & 0x3f) << 2;
                rgba[2] = (p & 0x1f) << 3;
                break;
            }
            case 0x8033: {
                uint16_t p = c[0] | (c[1] << 8);
                rgba[0] = (p >> 12) * 17; rgba[1] = ((p >> 8) & 0xf) * 17;
                rgba[2] = ((p >> 4) & 0xf) * 17; rgba[3] = (p & 0xf) * 17;
                break;
            }
            case 0x8034: {
                uint16_t p = c[0] | (c[1] << 8);
                rgba[0] = (p >> 11) << 3; rgba[1] = ((p >> 6) & 0x1f) << 3;
                rgba[2] = ((p >> 1) & 0x1f) << 3; rgba[3] = (p & 1) ? 255 : 0;
                break;
            }
            }
            memcpy(dst + i * 4, rgba, 4);
        }
        return true;
    }
    qemu_log("garmin_gl: compressed texture format %#x not supported (%dx%d)\n",
             fmt, w, h);
    dst = r->in + req_push(r, NULL, npix * 4);
    for (size_t i = 0; i < npix; i++) {
        dst[i * 4] = dst[i * 4 + 1] = dst[i * 4 + 2] = 0x80;
        dst[i * 4 + 3] = 0xff;
    }
    return true;
}

/* number of values glGet*(pname) returns */
static int get_count(uint32_t pname)
{
    switch (pname) {
    case 0x0ba6: case 0x0ba7: case 0x0ba8:      /* matrices */
        return 16;
    case 0x0ba2: case 0x0c10: case 0x0c22: case 0x0c23: case 0x0b00:
    case 0x0b53: case 0x0b66: case 0x0b02:      /* viewport, scissor, ... */
        return 4;
    case 0x0d3a: case 0x0b70: case 0x846d: case 0x846e: case 0x0b12:
    case 0x0b22:
        return 2;
    case 0x0b74: case 0x0d32: case 0x0d34: case 0x0d3b: case 0x0d3d:
    case 0x8b9c: case 0x8b9d:
        return 4;
    default:
        return 1;
    }
}

static int array_index(uint32_t cap)
{
    switch (cap) {
    case 0x8074: return ARR_VERTEX;
    case 0x8075: return ARR_NORMAL;
    case 0x8076: return ARR_COLOR;
    case 0x8078: return ARR_TEX0 + client_tex_unit;
    case 0x8b9c: return ARR_POINTSIZE;
    default:     return -1;
    }
}

static void set_pointer(int idx, int size, int type, int stride, uint32_t ptr)
{
    ClientArray *a = &arrays[idx];

    a->size = size;
    a->type = type;
    a->stride = stride;
    a->ptr = ptr;
    a->buffer = bound_array_buf;
}

static void write_getv(CPUState *cs, GlReq *r, uint32_t pname, uint32_t dstva,
                       int n, char kind)
{
    const float *f = (const float *)r->out;
    uint32_t tmp[16];

    for (int i = 0; i < n; i++) {
        switch (kind) {
        case 'f': tmp[i] = f2bits(f[i]); break;
        case 'x': tmp[i] = f2fx(f[i]); break;
        case 'b': tmp[i] = f[i] != 0; break;
        default:  tmp[i] = (int32_t)lrintf(f[i]); break;
        }
    }
    if (kind == 'b') {
        uint8_t b[16];

        for (int i = 0; i < n; i++) {
            b[i] = tmp[i];
        }
        guest_write(cs, dstva, b, n);
    } else {
        guest_write(cs, dstva, tmp, n * 4);
    }
}

/* fills r->a from the raw register/stack args according to the signature;
 * pointer params ('p', 'P', 'I') stay raw guest addresses */
static void decode_args(GlReq *r, const GlOpDef *d, const uint32_t *raw, int n)
{
    for (int i = 0; i < n && i < 16; i++) {
        char c = d->sig[i] ? d->sig[i] : 'i';

        switch (c) {
        case 'x': r->a[i] = f2bits(fx2f(raw[i])); break;
        case 'b': r->a[i] = f2bits((raw[i] & 0xff) / 255.0f); break;
        case 'I': r->a[i] = f2bits((float)(int32_t)raw[i]); break;
        default:  r->a[i] = raw[i]; break;
        }
    }
}

/* copy a parameter vector (4 entries) from the guest, converting to float */
static void marshal_vec4(CPUState *cs, GlReq *r, uint32_t va, char kind)
{
    uint32_t v[4] = { 0 };
    float f[4];

    guest_read(cs, va, v, 16);
    for (int i = 0; i < 4; i++) {
        f[i] = kind == 'x' ? fx2f(v[i]) : kind == 'I' ? (float)(int32_t)v[i]
                                                       : bits2f(v[i]);
    }
    req_push(r, f, 16);
}

static void marshal_mat16(CPUState *cs, GlReq *r, uint32_t va, bool fixed)
{
    uint32_t v[16] = { 0 };
    float f[16];

    guest_read(cs, va, v, 64);
    for (int i = 0; i < 16; i++) {
        f[i] = fixed ? fx2f(v[i]) : bits2f(v[i]);
    }
    req_push(r, f, 64);
}

/* Debug aid: with GARMIN_GL_DUMP=path, textures are written next to it as
 * path.texN.ppm (RGB, alpha dropped) so uploads can be inspected. */
static void dump_texture(const uint8_t *px, int w, int h, uint32_t fmt,
                         uint32_t type)
{
    static int n;
    const char *dump = getenv("GARMIN_GL_DUMP");
    char *name;
    FILE *f;
    int bpp = texel_bytes(fmt, type);
    size_t row = ROUND_UP((size_t)w * bpp, unpack_alignment);

    if (!dump || n >= 40 || w <= 0 || h <= 0) {
        return;
    }
    name = g_strdup_printf("%s.tex%d_%dx%d_%x_%x.ppm", dump, n++, w, h, fmt, type);
    f = fopen(name, "wb");
    g_free(name);
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t *p = px + y * row;

        for (int x = 0; x < w; x++, p += bpp) {
            uint8_t rgb[3];
            uint16_t v = p[0] | (p[1] << 8);

            switch (type) {
            case 0x8363:
                rgb[0] = (v >> 11) << 3; rgb[1] = ((v >> 5) & 0x3f) << 2;
                rgb[2] = (v & 0x1f) << 3;
                break;
            case 0x8034:
                rgb[0] = (v >> 11) << 3; rgb[1] = ((v >> 6) & 0x1f) << 3;
                rgb[2] = ((v >> 1) & 0x1f) << 3;
                break;
            case 0x8033:
                rgb[0] = (v >> 12) * 17; rgb[1] = ((v >> 8) & 0xf) * 17;
                rgb[2] = ((v >> 4) & 0xf) * 17;
                break;
            default:
                if (bpp >= 3) {
                    rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2];
                } else {
                    rgb[0] = rgb[1] = rgb[2] = p[0];
                }
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static void present(CPUState *cs, GlReq *r)
{
    size_t w = gl_width, h = gl_height;
    const uint8_t *rgba = r->out;
    g_autofree uint16_t *fb = g_malloc(w * h * 2);
    static int frame;
    const char *dump = getenv("GARMIN_GL_DUMP");

    if (r->out_len < w * h * 4) {
        return;
    }
    for (size_t y = 0; y < h; y++) {
        const uint8_t *src = rgba + (h - 1 - y) * w * 4;
        uint16_t *dst = fb + y * w;

        for (size_t x = 0; x < w; x++, src += 4) {
            dst[x] = ((src[0] >> 3) << 11) | ((src[1] >> 2) << 5) | (src[2] >> 3);
        }
    }
    address_space_write(&address_space_memory, gl_scanout,
                        MEMTXATTRS_UNSPECIFIED, fb, w * h * 2);
    frame++;
    if (dump) {
        FILE *f = fopen(dump, "wb");

        if (f) {
            fprintf(f, "P6\n%zu %zu\n255\n", w, h);
            for (size_t y = 0; y < h; y++) {
                const uint8_t *src = rgba + (h - 1 - y) * w * 4;

                for (size_t x = 0; x < w; x++) {
                    fwrite(src + x * 4, 1, 3, f);
                }
            }
            fclose(f);
        }
    }
    qemu_log("garmin_gl: frame %d presented to %08x\n", frame, gl_scanout);
}

static const char *kind_of_ptr(const GlOpDef *d, int i)
{
    static const char *k[] = { "f", "x", "I" };
    char c = d->sig[i];

    return c == 'P' ? k[1] : c == 'I' ? k[2] : k[0];
}


static GlReq *mk_bind_req(uint32_t id)
{
    GlReq *e = g_new0(GlReq, 1);

    e->op = OP_BINDTEXTURE;
    e->a[0] = 0x0de1;
    e->a[1] = id;
    return e;
}

/*
 * At eglSwapBuffers: bring every large client-memory texture to the state
 * the GPU would read.  Queued glTexImage2D calls of this frame get their
 * texels re-read now (a[10] holds the texture id, a[8] the guest pointer);
 * large textures uploaded in earlier frames are re-hashed and, if the
 * firmware drew into them since, re-uploaded before the frame replays.
 */
static void sync_large_textures(CPUState *cs)
{
    GHashTableIter it;
    gpointer key, val;
    GList *l;

    for (l = frame_q.head; l; l = l->next) {
        GlReq *e = l->data;
        TexSrc *t;

        if (e->op != OP_TEXIMAGE2D || !e->a[8] || !e->in_len) {
            continue;
        }
        t = g_hash_table_lookup(texsrcs, GUINT_TO_POINTER(e->a[10]));
        if (!t || !t->dirty_check || g_hash_table_contains(gpu_textures, GUINT_TO_POINTER(e->a[10]))) {
            continue;
        }
        guest_read(cs, e->a[8], e->in, e->in_len);
        t->hash = hash_bytes(e->in, e->in_len & ~3u);
        t->synced_frame = true;
    }
    g_hash_table_iter_init(&it, texsrcs);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        TexSrc *t = val;
        uint32_t id = GPOINTER_TO_UINT(key);
        GlReq *e;
        uint64_t h;

        if (!t->dirty_check || !garmin_gl_resync ||
            g_hash_table_contains(gpu_textures, GUINT_TO_POINTER(id))) {
            continue;
        }
        if (t->synced_frame) {
            t->synced_frame = false;
            continue;
        }
        e = g_new0(GlReq, 1);
        e->in = g_malloc(t->len);
        e->in_len = e->in_cap = t->len;
        if (!guest_read(cs, t->ptr, e->in, t->len)) {
            req_free(e);
            continue;
        }
        h = hash_bytes(e->in, t->len & ~3u);
        if (h == t->hash) {
            req_free(e);
            continue;
        }
        t->hash = h;
        e->op = OP_TEXIMAGE2D;
        e->a[0] = 0x0de1; e->a[1] = 0; e->a[2] = t->fmt; e->a[3] = t->w;
        e->a[4] = t->h; e->a[5] = 0; e->a[6] = t->fmt; e->a[7] = t->type;
        e->a[8] = t->ptr; e->a[9] = 4; e->a[10] = id;
        g_queue_push_head(&frame_q, mk_bind_req(bound_at_swap));
        g_queue_push_head(&frame_q, e);
        g_queue_push_head(&frame_q, mk_bind_req(id));
        if (garmin_gl_log || garmin_gl_drawtex_log) {
            qemu_log("garmin_gl: texture %u re-uploaded at swap "
                     "(%dx%d, layer changed)\n", id, t->w, t->h);
        }
    }
}

/*
 * Execute one intercepted call.  Returns the value for r0.
 */
static uint32_t gl_dispatch(CPUState *cs, GlHook *h, const uint32_t *raw)
{
    const GlOpDef *d = h->def;
    GlReq *r = &req;
    int n = h->nargs;

    if (!d) {
        return 0;
    }
    render_start();
    qemu_mutex_lock(&rlock);
    r->op = d->op;
    r->in_len = 0;
    memset(r->arr, 0, sizeof(r->arr));
    decode_args(r, d, raw, n);

    switch (d->op) {
    /* ---- tracked client state, no host call ---- */
    case OP_VERTEXPTR:
        set_pointer(ARR_VERTEX, raw[0], raw[1], raw[2], raw[3]);
        goto done;
    case OP_NORMALPTR:
        set_pointer(ARR_NORMAL, 3, raw[0], raw[1], raw[2]);
        goto done;
    case OP_COLORPTR:
        set_pointer(ARR_COLOR, raw[0], raw[1], raw[2], raw[3]);
        goto done;
    case OP_TEXCOORDPTR:
        set_pointer(ARR_TEX0 + client_tex_unit, raw[0], raw[1], raw[2], raw[3]);
        goto done;
    case OP_POINTSIZEPTR:
        set_pointer(ARR_POINTSIZE, 1, raw[0], raw[1], raw[2]);
        goto done;
    case OP_ENABLECLIENT:
    case OP_DISABLECLIENT: {
        int idx = array_index(raw[0]);

        if (idx >= 0) {
            arrays[idx].enabled = d->op == OP_ENABLECLIENT;
        }
        goto done;
    }
    case OP_CLIENTACTIVETEX:
        client_tex_unit = MIN(raw[0] - 0x84c0, 3u);
        goto done;
    case OP_BINDBUFFER:
        if (raw[0] == 0x8892) {
            bound_array_buf = raw[1];
        } else if (raw[0] == 0x8893) {
            bound_elem_buf = raw[1];
        }
        goto done;
    case OP_BINDTEXTURE:
        bound_texture = raw[1];
        break;
    case OP_GENBUFFERS: {
        uint32_t ids[64];
        int cnt = MIN(raw[0], 64u);

        for (int i = 0; i < cnt; i++) {
            GlBuffer *b = g_new0(GlBuffer, 1);

            b->id = next_buffer_id++;
            g_hash_table_insert(buffers, GUINT_TO_POINTER(b->id), b);
            ids[i] = b->id;
        }
        guest_write(cs, raw[1], ids, cnt * 4);
        goto done;
    }
    case OP_DELETEBUFFERS: {
        uint32_t ids[64];
        int cnt = MIN(raw[0], 64u);

        guest_read(cs, raw[1], ids, cnt * 4);
        for (int i = 0; i < cnt; i++) {
            GlBuffer *b = buffer_lookup(ids[i]);

            if (b) {
                g_hash_table_remove(buffers, GUINT_TO_POINTER(ids[i]));
                g_free(b->data);
                g_free(b);
            }
            if (ids[i] == bound_array_buf) {
                bound_array_buf = 0;
            }
            if (ids[i] == bound_elem_buf) {
                bound_elem_buf = 0;
            }
        }
        goto done;
    }
    case OP_BUFFERDATA: {
        GlBuffer *b = buffer_lookup(raw[0] == 0x8893 ? bound_elem_buf
                                                     : bound_array_buf);

        if (b) {
            g_free(b->data);
            b->size = raw[1];
            b->data = g_malloc0(b->size);
            if (raw[2]) {
                guest_read(cs, raw[2], b->data, b->size);
            }
        }
        goto done;
    }
    case OP_BUFFERSUBDATA: {
        GlBuffer *b = buffer_lookup(raw[0] == 0x8893 ? bound_elem_buf
                                                     : bound_array_buf);

        if (b && raw[1] + raw[2] <= b->size) {
            guest_read(cs, raw[3], b->data + raw[1], raw[2]);
        }
        goto done;
    }
    case OP_ACTIVETEXTURE:
        server_tex_unit = MIN(raw[0] - 0x84c0, 3u);
        break;
    case OP_PIXELSTORE:
        if (raw[0] == 0x0cf5) {                 /* GL_UNPACK_ALIGNMENT */
            unpack_alignment = raw[1] ? raw[1] : 4;
        }
        goto done;
    case OP_GETSTRING:
        /* strings live in the driver's own rodata (identity mapped) */
        switch (raw[0]) {
        case 0x1f00: r->result = 0x8031380c; break;   /* GL_VENDOR */
        case 0x1f02: r->result = 0x80313828; break;   /* GL_VERSION */
        case 0x1f01: r->result = 0x80313828; break;   /* GL_RENDERER */
        default:     r->result = 0x80313824; break;   /* "" (NUL) */
        }
        goto done;
    case OP_EGL_TRUE:
        r->result = 1;
        goto done;
    case OP_EGL_SURFACE: {
        static uint32_t surfaces;

        /* Synthetic handle: every surface aliases the one host FBO. */
        r->result = EGL_FAKE_SURFACE + (++surfaces);
        goto done;
    }
    case OP_EGL_CURSURFACE:
        r->result = EGL_FAKE_SURFACE;
        goto done;
    case OP_EGL_QUERYSURFACE: {
        uint32_t v;

        switch (raw[2]) {
        case 0x3057: v = gl_width; break;            /* EGL_WIDTH */
        case 0x3056: v = gl_height; break;           /* EGL_HEIGHT */
        case 0x3086: v = 0x3084; break;              /* RENDER_BUFFER = BACK */
        case 0x3093: v = 0x3095; break;              /* SWAP_BEHAVIOR = DESTROYED */
        case 0x3080: case 0x3081: v = 0x305c; break; /* TEXTURE_* = NO_TEXTURE */
        case 0x3090: case 0x3091: case 0x3092:       /* resolution / aspect */
            v = 0xffffffff; break;                   /* EGL_UNKNOWN */
        default: v = 0; break;
        }
        guest_write(cs, raw[3], &v, 4);
        r->result = 1;
        goto done;
    }
    case OP_EGL_NO_IMAGE:
        /*
         * EGL_NO_IMAGE_KHR.  Failing here is deliberate: the driver then
         * uploads the texture itself, which the renderer supports, instead of
         * relying on a zero-copy native buffer it cannot share with us.
         */
        r->result = 0;
        goto done;

    /* ---- calls needing guest data ---- */
    case OP_LIGHTFV: case OP_MATERIALFV: case OP_TEXENVFV: case OP_TEXPARAMFV:
        if (d->op == OP_TEXPARAMFV && raw[1] == 0x8b9d && bound_texture) {
            uint32_t words[4] = { 0 };
            int32_t *c = g_new0(int32_t, 4);
            bool fixed = d->sig[2] == 'P';

            guest_read(cs, raw[2], words, sizeof(words));
            for (int i = 0; i < 4; i++) {
                /* glTexParameterxv passes 16.16 fixed point */
                c[i] = fixed ? (int32_t)fx2f(words[i]) : (int32_t)words[i];
            }
            if (garmin_gl_drawtex_log) {
                qemu_log("garmin_gl: crop set tex=%u unit=%d rect=%d,%d %dx%d\n",
                         bound_texture, server_tex_unit,
                         c[0], c[1], c[2], c[3]);
            }
            g_hash_table_insert(crops, GUINT_TO_POINTER(bound_texture), c);
            goto done;
        }
        marshal_vec4(cs, r, raw[2], *kind_of_ptr(d, 2));
        break;
    case OP_LIGHTMODELFV: case OP_FOGFV: case OP_POINTPARAMFV: case OP_CLIPPLANEF:
        marshal_vec4(cs, r, raw[1], *kind_of_ptr(d, 1));
        break;
    case OP_LOADMATRIX: case OP_MULTMATRIX:
        marshal_mat16(cs, r, raw[0], d->sig[0] == 'P');
        break;
    case OP_DRAWARRAYS:

        if (!marshal_draw(cs, r, raw[1], raw[2])) {
            goto done;
        }
        break;
    case OP_DRAWTEX: {
        const int32_t *c = crop_of(bound_texture);

        for (int i = 0; i < 4; i++) {
            r->a[5 + i] = f2bits((float)c[i]);
        }
        r->a[9] = bound_texture;
        r->a[10] = server_tex_unit;
        break;
    }
    case OP_DRAWELEMENTS: {
        int count = raw[1], type = raw[2];


        int isz = type == 0x1403 ? 2 : 1;
        g_autofree uint8_t *idx = g_malloc((size_t)count * isz);
        int maxi = -1;

        if (count <= 0) {
            goto done;
        }
        if (bound_elem_buf) {
            GlBuffer *b = buffer_lookup(bound_elem_buf);

            if (!b || raw[3] + (size_t)count * isz > b->size) {
                goto done;
            }
            memcpy(idx, b->data + raw[3], (size_t)count * isz);
        } else if (!guest_read(cs, raw[3], idx, (size_t)count * isz)) {
            goto done;
        }
        for (int i = 0; i < count; i++) {
            int v = isz == 2 ? (idx[2 * i] | (idx[2 * i + 1] << 8)) : idx[i];

            maxi = MAX(maxi, v);
        }
        r->idx_type = type;
        r->idx_count = count;
        r->idx_off = req_push(r, idx, (size_t)count * isz);
        if (!marshal_draw(cs, r, 0, maxi + 1)) {
            goto done;
        }
        break;
    }
    case OP_TEXIMAGE2D:
    case OP_TEXSUBIMAGE2D: {
        size_t len = image_bytes(raw[3], raw[4], raw[6], raw[7]);

        r->a[9] = unpack_alignment;
        r->a[10] = bound_texture;
        if (raw[8] && len) {
            req_push(r, NULL, len);
            if (!guest_read(cs, raw[8], r->in, len)) {
                qemu_log("garmin_gl: texel read failed at %08x (%zu bytes)\n",
                         raw[8], len);
            }
            dump_texture(r->in, raw[3], raw[4], raw[6], raw[7]);
            if (d->op == OP_TEXIMAGE2D && raw[1] == 0 && bound_texture) {
                TexSrc *t = g_new0(TexSrc, 1);

                t->ptr = raw[8]; t->w = raw[3]; t->h = raw[4];
                t->fmt = raw[6]; t->type = raw[7]; t->len = len;
                t->hash = hash_bytes(r->in, len & ~3u);
                /* only large surfaces are drawn into after the call, and
                 * never ones the GPU renders into itself */
                t->dirty_check = (size_t)raw[3] * raw[4] >= 256 * 256 &&
                                 raw[6] != 0x1906 &&    /* never GL_ALPHA strips */
                                 !g_hash_table_contains(gpu_textures, GUINT_TO_POINTER(bound_texture));
                g_hash_table_insert(texsrcs, GUINT_TO_POINTER(bound_texture), t);
            }
        } else if (d->op == OP_TEXSUBIMAGE2D) {
            goto done;
        } else if (bound_texture) {
            g_hash_table_remove(texsrcs, GUINT_TO_POINTER(bound_texture));
        }
        break;
    }
    case OP_FRAMEBUFFERTEXTURE2D:
        /* The GPU takes over this texture's content from here on. */
        if (raw[3]) {
            TexSrc *t = g_hash_table_lookup(texsrcs, GUINT_TO_POINTER(raw[3]));

            g_hash_table_add(gpu_textures, GUINT_TO_POINTER(raw[3]));
            if (t) {
                t->dirty_check = false;
            }
        }
        break;
    case OP_COMPRESSEDTEXIMAGE2D:
    case OP_COMPRESSEDTEXSUBIMAGE2D: {
        bool sub = d->op == OP_COMPRESSEDTEXSUBIMAGE2D;
        uint32_t fmt = sub ? raw[6] : raw[2];
        int w = sub ? raw[4] : raw[3], hh = sub ? raw[5] : raw[4];
        size_t len = sub ? raw[7] : raw[6];
        uint32_t dva = sub ? raw[8] : raw[7];
        g_autofree uint8_t *data = g_malloc(len + 4);
        uint32_t outfmt;

        if (!guest_read(cs, dva, data, len) ||
            !decode_compressed(r, fmt, w, hh, raw[1], data, len, &outfmt)) {
            goto done;
        }
        if (sub) {
            r->a[6] = outfmt;
        } else {
            r->a[2] = outfmt;
        }
        break;
    }
    case OP_READPIXELS: {
        size_t len = (size_t)raw[2] * raw[3] * texel_bytes(raw[4], raw[5]);

        req_out(r, len);
        break;
    }
    case OP_GENTEXTURES: case OP_GENFRAMEBUFFERS: case OP_GENRENDERBUFFERS:
        req_out(r, MIN(raw[0], 256u) * 4);
        r->a[0] = MIN(raw[0], 256u);
        break;
    case OP_DELETEFRAMEBUFFERS: case OP_DELETERENDERBUFFERS: {
        int cnt = MIN(raw[0], 256u);

        req_push(r, NULL, cnt * 4);
        guest_read(cs, raw[1], r->in, cnt * 4);
        r->a[0] = cnt;
        break;
    }
    case OP_DELETETEXTURES: {
        int cnt = MIN(raw[0], 256u);

        req_push(r, NULL, cnt * 4);
        guest_read(cs, raw[1], r->in, cnt * 4);
        r->a[0] = cnt;
        for (int i = 0; i < cnt; i++) {         /* forget their client sources */
            g_hash_table_remove(texsrcs,
                                GUINT_TO_POINTER(((uint32_t *)r->in)[i]));
            g_hash_table_remove(gpu_textures,
                                GUINT_TO_POINTER(((uint32_t *)r->in)[i]));
            g_hash_table_remove(crops,
                                GUINT_TO_POINTER(((uint32_t *)r->in)[i]));
        }
        break;
    }
    case OP_GETFLOATV: case OP_GETINTEGERV: case OP_GETBOOLEANV:
    case OP_GETFIXEDV: case OP_GETLIGHTFV: case OP_GETMATERIALFV:
    case OP_GETTEXENVFV: case OP_GETTEXPARAMFV: case OP_GETCLIPPLANEF:
        req_out(r, 16 * 4);
        break;
    case OP_GETBUFFERPARAM: {
        GlBuffer *b = buffer_lookup(raw[0] == 0x8893 ? bound_elem_buf
                                                     : bound_array_buf);
        uint32_t v = 0;

        if (b) {
            v = raw[1] == 0x8764 ? b->size : raw[1] == 0x8765 ? 0x88e4 : 0;
        }
        guest_write(cs, raw[2], &v, 4);
        goto done;
    }
    case OP_GETPOINTERV: {
        static const uint32_t names[] = { 0x808e, 0x808f, 0x8090, 0x8092 };
        uint32_t v = 0;

        for (int i = 0; i < 4; i++) {
            if (raw[0] == names[i]) {
                v = arrays[i == 3 ? ARR_TEX0 + client_tex_unit : i].ptr;
            }
        }
        guest_write(cs, raw[1], &v, 4);
        goto done;
    }
    case OP_SWAPBUFFERS:
        req_out(r, (size_t)gl_width * gl_height * 4);
        break;
    default:
        break;
    }

    if (d->op == OP_SWAPBUFFERS) {
        sync_large_textures(cs);            /* texels as the GPU would see them */
        frame_flush();
        bound_at_swap = bound_texture;
    } else if (op_is_query(d->op)) {
        frame_flush();
    } else {
        g_queue_push_tail(&frame_q, req_clone(r));
        goto done;
    }
    render_call();

    /* ---- completion: write results back to the guest ---- */
    switch (d->op) {
    case OP_READPIXELS:
        guest_write(cs, raw[6], r->out, r->out_len);
        break;
    case OP_GENTEXTURES:
        for (uint32_t i = 0; i < r->a[0]; i++) {
            g_hash_table_remove(texsrcs, GUINT_TO_POINTER(((uint32_t *)r->out)[i]));
            g_hash_table_remove(gpu_textures,
                                GUINT_TO_POINTER(((uint32_t *)r->out)[i]));
            g_hash_table_remove(crops,
                                GUINT_TO_POINTER(((uint32_t *)r->out)[i]));
        }
        guest_write(cs, raw[1], r->out, r->a[0] * 4);
        break;
    case OP_GENFRAMEBUFFERS: case OP_GENRENDERBUFFERS:
        guest_write(cs, raw[1], r->out, r->a[0] * 4);
        break;
    case OP_GETFLOATV:
        write_getv(cs, r, raw[0], raw[1], get_count(raw[0]), 'f');
        break;
    case OP_GETINTEGERV: {
        int cnt = get_count(raw[0]);
        int32_t special = -1;

        switch (raw[0]) {
        case 0x8894: special = bound_array_buf; break;    /* ARRAY_BUFFER_BINDING */
        case 0x8895: special = bound_elem_buf; break;
        case 0x86a2: special = 10; break;                 /* NUM_COMPRESSED_TEXTURE_FORMATS */
        case 0x8b9a: special = 0x8363; break;             /* IMPL_COLOR_READ_TYPE */
        case 0x8b9b: special = 0x1907; break;             /* IMPL_COLOR_READ_FORMAT */
        case 0x84e2: special = 2; break;                  /* MAX_TEXTURE_UNITS */
        }
        if (raw[0] == 0x86a3) {                           /* COMPRESSED_TEXTURE_FORMATS */
            uint32_t fmts[10];

            for (int i = 0; i < 10; i++) {
                fmts[i] = 0x8b90 + i;
            }
            guest_write(cs, raw[1], fmts, sizeof(fmts));
        } else if (special >= 0) {
            guest_write(cs, raw[1], &special, 4);
        } else {
            write_getv(cs, r, raw[0], raw[1], cnt, 'i');
        }
        break;
    }
    case OP_GETBOOLEANV:
        write_getv(cs, r, raw[0], raw[1], get_count(raw[0]), 'b');
        break;
    case OP_GETFIXEDV:
        write_getv(cs, r, raw[0], raw[1], get_count(raw[0]), 'x');
        break;
    case OP_GETLIGHTFV: case OP_GETMATERIALFV: case OP_GETTEXENVFV:
    case OP_GETTEXPARAMFV:
        write_getv(cs, r, raw[1], raw[2], 4, d->sig[2] == 'P' ? 'x'
                                          : d->sig[2] == 'I' ? 'i' : 'f');
        break;
    case OP_GETCLIPPLANEF:
        write_getv(cs, r, raw[0], raw[1], 4, d->sig[1] == 'P' ? 'x' : 'f');
        break;
    case OP_SWAPBUFFERS:
        present(cs, r);
        break;
    default:
        break;
    }
done:
    qemu_mutex_unlock(&rlock);
    return r->result;
}

/* ---- the trap ------------------------------------------------------ */

static void gl_trap(int slot)
{
    CPUState *cs = current_cpu;
    CPUARMState *env = &ARM_CPU(cs)->env;
    GlHook *h;
    uint32_t args[16];
    int n;

    if (slot >= nhooks) {
        gl_result = 0;
        return;
    }
    h = &hooks[slot];
    h->calls++;
    n = MIN(h->nargs, 16);
    for (int i = 0; i < n; i++) {
        args[i] = i < 4 ? env->regs[i]
                        : guest_ld32(cs, env->regs[13] + 4 * (i - 4));
    }
    req.result = 0;
    gl_result = gl_dispatch(cs, h, args);

    if (garmin_gl_log) {
        char buf[320];
        int len = snprintf(buf, sizeof(buf), "gl: %s(", h->name);

        for (int i = 0; i < n && len < (int)sizeof(buf) - 16; i++) {
            char c = h->def ? h->def->sig[i] : 0;

            if (c == 'f') {
                len += snprintf(buf + len, sizeof(buf) - len, "%s%g",
                                i ? ", " : "", bits2f(args[i]));
            } else if (c == 'x') {
                len += snprintf(buf + len, sizeof(buf) - len, "%s%gx",
                                i ? ", " : "", fx2f(args[i]));
            } else {
                len += snprintf(buf + len, sizeof(buf) - len, "%s%#x",
                                i ? ", " : "", args[i]);
            }
        }
        snprintf(buf + len, sizeof(buf) - len, ") -> %#x [%s lr=%08x]",
                 gl_result, task_name(cs), env->regs[14]);
        qemu_log("%s\n", buf);
    }
}

static uint64_t gl_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    if (addr == GARMIN_GL_RESULT_OFF) {
        return gl_result;
    }
    return 0;
}

static void gl_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    if (addr < GARMIN_GL_RESULT_OFF) {
        gl_trap(addr / 4);
    }
}

static const MemoryRegionOps gl_mmio_ops = {
    .read = gl_mmio_read,
    .write = gl_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* Snapshots restore unpatched RAM: re-apply the trampolines whenever the
 * VM (re)starts running, e.g. after -loadvm. */
static void gl_vm_state(void *opaque, bool running, RunState state)
{
    if (running) {
        garmin_gl_reset(NULL);
    }
}

void garmin_gl_mmio_init(MemoryRegion *sysmem)
{
    MemoryRegion *mr = g_new0(MemoryRegion, 1);

    memory_region_init_io(mr, NULL, &gl_mmio_ops, NULL, "garmin.gl-hooks",
                          GARMIN_GL_MMIO_SIZE);
    memory_region_add_subregion_overlap(sysmem, GARMIN_GL_MMIO_BASE, mr, 10);
    qemu_add_vm_change_state_handler(gl_vm_state, NULL);
}
