# PowerVR OpenGL ES-CM 1.1 / EGL entry points in the GPSMAP 7x08 main firmware

Image: `fw/gpsmap7x08_main_0x80050000.bin` (link base 0x80050000, Thumb-2). All addresses are Thumb function starts (call with bit 0 set). `len` is the exact function length from the linker's ARM EHABI unwind table (entries `{0, func, len, opcodes}` at 0x83300000..0x83550000).

## How the addresses were found

* Every GLES1 API function begins with `bl 0x80986ec8` (GetCurrentContext: the thread-id stub 0x805e63ee returns the constant 0x539, which is looked up in the TLS table at 0xa1fe7e58) and returns silently when it yields NULL. 153 functions call it; that set is the complete public GL API present in the image. Errors are raised through `SetError` 0x80986efe (`r0=ctx, r1=GLenum`, stored at `ctx+0x868` only if no error is pending).
* Garmin's wrapper `modules/msl/gfx/gfx_3d.c` is compiled as `GFX_glXxx(gfx_ctx *ctx, ...)`. Each wrapper logs through 0x806a043a with an inline `adr` format string (`"glTexImage2D(%d, %d, ...)"`), tests `ctx->pipeline(+0x40)->type(+0x14) == 3` and tail-calls a small shim that drops `ctx` and jumps (`b.w`) into the driver. The wrappers were located through the relocated `.data` assert records (file 0x843xxxxx = runtime 0x9fb0xxxx, delta 0x1b80af58) that point at the `..\msl\gfx\gfx_3d.c: <line>` strings; the shim chain gives the driver function. The inline format string, not the GFX_ name table at 0x8213085f (its records are shifted by one entry), is the authoritative name.
* Extension entry points come from the driver's GetProcAddress (0x80688bb0: strcmp chain over the names at 0x82094c0f..0x82095029, each match returns `movw/movt` of the function). EGL KHR/NOK extensions come from the `{name, fn}` table at 0x81319f04.
* EGL entry points all start with `bl 0x805e5846` (IMGeglGetTLS with callback 0x80303fa5) and were labelled by their EGL enums/error codes. They are reached only through `b.w` veneers (0x800534d0.., 0x802398ce.., 0x80312262.., 0x8034af66, 0x80929f76), which is why plain BL-caller searches find nothing.
* `glGetError` does **not** exist in the image: no code other than SetError touches `ctx+0x868` (dead-stripped; Garmin never calls it). Likewise absent: glActiveTexture, glClientActiveTexture, glHint, glPixelStorei, glStencil*, glClipPlanef, glFog*, glLightModel*, glNormal3f, glMultiTexCoord4f, glSampleCoverage, glPolygonOffset, glClearStencil, glColor4ub, glIsTexture/glIsBuffer, glGetBufferParameteriv, glGetMaterial*, glGetFixedv/glGetLightxv and the fixed-point matrix/colour variants (glClearColorx, glColor4x, glDepthRangex, glLineWidthx, glLoadMatrixx, glMultMatrixx, glRotatex, glScalex, glTranslatex, glClearDepthx). Garmin's shim table (0x80debbb4..0x80debc7a) maps its own x-variants to `bx lr` no-ops.

## OpenGL ES 1.1 core entry points

| name | address | len | prototype | confidence | evidence |
|---|---|---|---|---|---|
| glAlphaFunc | 0x80332046 | 0x72 | `void glAlphaFunc(GLenum func, GLclampf ref)` | high | func-0x200<=7 else GL_INVALID_ENUM; clamps ref; called by Garmin glTexEnvf shim 0x80312c90 with GL_NEVER/GL_NOTEQUAL |
| glBindBuffer | 0x80272510 | 0x152 | `void glBindBuffer(GLenum target, GLuint buffer)` | high | GFX_glBindBuffer wrapper 0x8098b66c -> shim 0x8023b350; checks 0x8892/0x8893 |
| glBindTexture | 0x802b0140 | 0x52 | `void glBindTexture(GLenum target, GLuint texture)` | high | GFX_glBindTexture 0x809930d4 -> shim 0x802724a0; checks 0xDE1/0x8513/0x8C0D |
| glBlendFunc | 0x803d02c0 | 0x1e | `void glBlendFunc(GLenum sfactor, GLenum dfactor)` | high | GFX_glBlendFunc 0x803adbc4 -> shim 0x80405a7c; calls internal BlendFuncSeparate 0x8033eda0(ctx,s,d,s,d) |
| glBlendFuncSeparateOES | 0x803320b8 | 0x22 | `void glBlendFuncSeparateOES(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha)` | high | GetProcAddress table + Garmin GFX_glBlendFuncSeparate shim 0x80312af0 (falls back to glBlendFunc if GL_OES_blend_func_separate is missing) |
| glBufferData | 0x80272662 | 0x158 | `void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)` | high | GFX_glBufferData 0x8095598c -> shim 0x8023b358; checks 0x8892, usage 0x88E4 |
| glBufferSubData | 0x80dec244 | 0xaa | `void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)` | medium | checks 0x8892, GL_INVALID_VALUE/OPERATION; 4-arg buffer function next to the buffer helpers |
| glClear | 0x803320da | 0xc6 | `void glClear(GLbitfield mask)` | high | GFX_glClear 0x809651bc -> shim 0x80312bdc; GL_INVALID_FRAMEBUFFER_OPERATION 0x506 check on 0x8CD5 |
| glClearColor | 0x803321a0 | 0x64 | `void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)` | high | GFX_glClearColor 0x80965204 -> shim 0x80312be2; 4x clamp via 0x809696c2 |
| glClearDepthf | 0x80dec314 | 0x1e | `void glClearDepthf(GLclampf depth)` | high | GFX_glClearDepth 0x80685728 -> shim 0x80debbb6 |
| glColor4f | 0x802e4250 | 0x60 | `void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)` | high | GFX_glColor4f 0x8096c704 -> shim 0x80312bf0; also called directly by Garmin EGL bring-up 0x801d10da with 1,1,1,1 |
| glColorMask | 0x80dec332 | 0x4e | `void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)` | high | GFX_glColorMask 0x80685a18 -> shim 0x80debbc0 |
| glColorPointer | 0x805d2304 | 0xe2 | `void glColorPointer(GLint size, GLenum type, GLsizei stride, const void *pointer)` | high | GFX_glColorPointer 0x80934d80 -> shim 0x805bda8e; type checks 0x1401/0x1406/0x140C |
| glCompressedTexImage2D | 0x80dec380 | 0x670 | `void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data)` | high | internalformat range check 0x8B90..0x8C03 (GL_PALETTE*, GL_COMPRESSED_RGB*_PVRTC) at 0x80dec39a |
| glCompressedTexSubImage2D | 0x80dec9f0 | 0x170 | `void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data)` | medium | 9 args; format (stack slot 2) checked against 0x8C00..0x8C03 at 0x80deca08 |
| glCopyTexImage2D | 0x80decb60 | 0x638 | `void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border)` | medium | 8 args; internalformat accepted in 0x1906..0x190A or sized 0x803B..0x8058 |
| glCopyTexSubImage2D | 0x80ded198 | 0xc40 | `void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)` | medium | 8 args; validates the existing texture level format (0x8B8F/0x8B90../0x8C00../0x8D64 ETC1, 0x1906..) |
| glCullFace | 0x802727ba | 0x46 | `void glCullFace(GLenum mode)` | high | GFX_glCullFace 0x80997ce4 -> shim 0x802af4da; mode-0x404 in {0,1,4}; stores ctx+0x634; Garmin bring-up calls with GL_BACK |
| glDeleteBuffers | 0x803134c2 | 0xe0 | `void glDeleteBuffers(GLsizei n, const GLuint *buffers)` | high | GFX_glDeleteBuffers 0x808641cc -> Garmin shim 0x802e3efe (defers unless on the GL task) -> 0x803134c2 |
| glDeleteTextures | 0x803135a2 | 0x164 | `void glDeleteTextures(GLsizei n, const GLuint *textures)` | high | GFX_glDeleteTextures 0x80974398 -> Garmin shim 0x80312bfe -> 0x803135a2 |
| glDepthFunc | 0x8023b58a | 0x48 | `void glDepthFunc(GLenum func)` | high | GFX_glDepthFunc 0x80172a4c -> 0x801faa16; func-0x200<=7; Garmin bring-up calls with GL_LESS |
| glDepthMask | 0x8023b5d2 | 0x38 | `void glDepthMask(GLboolean flag)` | high | GFX_glDepthMask 0x809a790c -> shim 0x8023b366; Garmin bring-up calls with 1 |
| glDepthRangef | 0x80dedfaa | 0x9a | `void glDepthRangef(GLclampf zNear, GLclampf zFar)` | high | GFX_glDepthRangef 0x80685a88 -> shim 0x80debbce; clamps both |
| glDisable | 0x80852a88 | 0x352 | `void glDisable(GLenum cap)` | high | GFX_glDisable 0x809a7874 -> shim 0x8023b36c; Garmin bring-up disables GL_BLEND/GL_DITHER/GL_DEPTH_TEST/GL_TEXTURE_2D/GL_LIGHTING |
| glDisableClientState | 0x802b0194 | 0x8e | `void glDisableClientState(GLenum array)` | high | GFX_glDisableClientState 0x8099e7d8 -> shim 0x802724a8; 0x8074..0x8078, 0x8B9C, 0x8844, 0x86AC |
| glDrawArrays | 0x80332204 | 0x2a8 | `void glDrawArrays(GLenum mode, GLint first, GLsizei count)` | high | GFX_glDrawArrays 0x8096cc18 -> shim 0x80312c72; GL_INVALID_VALUE/ENUM, FBO-complete check 0x8CD5 |
| glDrawElements | 0x805d23e8 | 0x4a6 | `void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)` | high | GFX_glDrawElements 0x80923af0 -> shim 0x805bda9c; type 0x1401/0x1403 |
| glEnable | 0x80272800 | 0x358 | `void glEnable(GLenum cap)` | high | GFX_glEnable 0x809a78c0 -> shim 0x8023b372; caps 0xB10,0xB20,0xB90,0xBA1,0xBC0... |
| glEnableClientState | 0x802b0224 | 0x8e | `void glEnableClientState(GLenum array)` | high | GFX_glEnableClientState 0x8099e6b4 -> shim 0x802724ae |
| glFinish | 0x80313706 | 0xb6 | `void glFinish(void)` | high | kicks the TA (0x809092b8(ctx,1)) and waits for the render (0x8097bab8 with wait flags) |
| glFlush | 0x802e42b0 | 0x36 | `void glFlush(void)` | high | kicks the TA when flush mode ctx+0xF30==2, then 0x8097bab8 without waiting |
| glFrontFace | 0x80583334 | 0x3c | `void glFrontFace(GLenum mode)` | high | GFX_glFrontFace 0x80934d34 -> shim 0x805a8a3c; 0x900/0x901; Garmin bring-up uses GL_CCW |
| glFrustumf | 0x802e42e6 | 0x100 | `void glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)` | high | GFX_glFrustumf 0x80997d30 -> shim 0x802af4e0; GL_INVALID_VALUE; matrix helper 0x8097cc98 |
| glFrustumx | 0x80dee65c | 0x140 | `void glFrustumx(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f)` | high | GFX_glFrustumx 0x80685b54 -> shim 0x80debbd8 |
| glGenBuffers | 0x80272b58 | 0x3c | `void glGenBuffers(GLsizei n, GLuint *buffers)` | high | GFX_glGenBuffers 0x80954880 -> shim 0x8023b378; n<0 -> GL_INVALID_VALUE |
| glGenTextures | 0x802b02b2 | 0x3c | `void glGenTextures(GLsizei n, GLuint *textures)` | high | GFX_glGenTextures 0x8099beb8 -> shim 0x802724b4 |
| glGetBooleanv | 0x805bdb2c | 0x1c | `void glGetBooleanv(GLenum pname, GLboolean *params)` | high | GFX_glGetBooleanv 0x8072a518 -> shim 0x805a8a42; common glGet 0x80977b24(ctx,pname,params,type=3) |
| glGetFloatv | 0x802b02ee | 0x1c | `void glGetFloatv(GLenum pname, GLfloat *params)` | high | GFX_glGetFloatv 0x80950a04 -> shim 0x802724bc; glGet type=0 |
| glGetIntegerv | 0x802b030a | 0x1c | `void glGetIntegerv(GLenum pname, GLint *params)` | high | GFX_glGetIntegerv 0x809509be / 0x802af430 (GL_MAX_TEXTURE_SIZE query) -> shim 0x802af506; glGet type=2 |
| glGetLightfv | 0x805bdb48 | 0x1e | `void glGetLightfv(GLenum light, GLenum pname, GLfloat *params)` | medium | GFX_glGetLightfv 0x8071a090 -> shim 0x805a8a4a -> 0x805bdb48 -> 0x805cc2d8(ctx,light,pname,params) |
| glGetPointerv | 0x805bdb68 | 0x7c | `void glGetPointerv(GLenum pname, void **params)` | high | pnames 0x808E..0x8092, 0x86AC, 0x8849, 0x898C read from the client-array block ctx+0x9D8 |
| glGetString | 0x803137bc | 0x80 | `const GLubyte *glGetString(GLenum name)` | high | returns 'Imagination Technologies' / 'OpenGL ES-CM 1.1' / version / extensions for 0x1F00..0x1F03; name==0x6500 returns the GLES1 API table 0x8200ecb8 (used by the EGL loader 0x8029b2ca) |
| glGetTexEnv{fv,iv,xv} | 0x805bdbe4 | 0x1e | `void glGetTexEnv*v(GLenum env, GLenum pname, T *params)` | low | tail-calls the GetTexEnv common 0x80910ca0; siblings 0x80deebfc, 0x80deec4a (f/i/x assignment undetermined) |
| glGetTexParameter{fv,iv,xv} | 0x805bdc02 | 0x22 | `void glGetTexParameter*v(GLenum target, GLenum pname, T *params)` | low | calls the GetTexParameter common 0x80910fb8; siblings 0x80deed56, 0x80deed78 |
| glIsEnabled | 0x80deee88 | 0x18 | `GLboolean glIsEnabled(GLenum cap)` | medium | tail-calls 0x80300954 cap dispatch (0x4000 clip planes, 0x8074.., 0x8512); returns 0 without a context |
| glLightf | 0x80deef4a | 0x36 | `void glLightf(GLenum light, GLenum pname, GLfloat param)` | high | pname-0x1205<=4 (scalar light params) then Light common 0x80911120 |
| glLightfv | 0x805bdc24 | 0x1e | `void glLightfv(GLenum light, GLenum pname, const GLfloat *params)` | high | GFX_glLightfv 0x808c0c20 -> shim 0x805a8a70; tail-calls 0x80911120 |
| glLightx | 0x80deef80 | 0x4c | `void glLightx(GLenum light, GLenum pname, GLfixed param)` | medium | fixed->float conversion then 0x80911120; imm 0x1200/0x1205 |
| glLightxv | 0x80deefcc | 0x70 | `void glLightxv(GLenum light, GLenum pname, const GLfixed *params)` | medium | vector fixed->float then 0x80911120 |
| glLineWidth | 0x80370bde | 0x8c | `void glLineWidth(GLfloat width)` | high | GFX_glLineWidth 0x80949820 -> shim 0x8034cbda |
| glLoadIdentity | 0x80272b94 | 0x16 | `void glLoadIdentity(void)` | high | GFX_glLoadIdentity 0x809a79a8 -> shim 0x8023b380; jumps through ctx+0x88C (per matrix mode) |
| glLoadMatrixf | 0x805bdc42 | 0xa0 | `void glLoadMatrixf(const GLfloat *m)` | high | GFX_glLoadMatrixf 0x80560144 -> shim 0x805a8a7a; switches on matrix mode ctx+0x860 (0x1700/0x1701/0x1702/0x8840) |
| glLogicOp | 0x80def074 | 0x3c | `void glLogicOp(GLenum opcode)` | high | GFX_glLogicOp 0x80685f48 -> shim 0x80debbfe; opcode-0x1500 range check |
| glMaterialfv | 0x80def126 | 0x1e | `void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params)` | medium | tail-calls the Material common 0x80980984 (0x408 FRONT_AND_BACK, 0x1200..0x1202, 0x1600..0x1602) |
| glMaterialf/x (scalar) | 0x80def144 | 0x7c | `void glMaterialf(GLenum face, GLenum pname, GLfloat param)` | low | scalar variant feeding 0x80980984; float vs fixed undetermined |
| glMatrixMode | 0x80272baa | 0xc0 | `void glMatrixMode(GLenum mode)` | high | GFX_glMatrixMode 0x809a7958 -> shim 0x8023b384; 0x1700/0x1701/0x1702/0x8840; stores ctx+0x860 and the matrix fn ptrs ctx+0x884.. |
| glMultMatrixf | 0x80def29a | 0x5e | `void glMultMatrixf(const GLfloat *m)` | high | GFX_glMultMatrixf 0x80685f94 -> shim 0x80debc04; matrix helper 0x8097cc98 |
| glNormalPointer | 0x805bdce4 | 0x100 | `void glNormalPointer(GLenum type, GLsizei stride, const void *pointer)` | high | GFX_glNormalPointer 0x8056018c -> shim 0x805a8a80; type-0x1400 table, stride<0 -> GL_INVALID_VALUE |
| glOrthof | 0x80272c6a | 0x4c | `void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)` | high | GFX_glOrthof 0x8095c0ac -> shim 0x8023b38a; moves the args to s0..s5 and jumps to 0x8029cc04 |
| glOrthox | 0x80defbe8 | 0x78 | `void glOrthox(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f)` | high | GFX_glOrthox 0x80686028 -> shim 0x80debc0c; converts 6 fixed (x/65536) then 0x8029cc04 |
| glPointParameterf | 0x80defc60 | 0x32 | `void glPointParameterf(GLenum pname, GLfloat param)` | medium | pname-0x8126<=2 then 0x8072ac64 |
| glPointParameterfv | 0x80defc92 | 0x1a | `void glPointParameterfv(GLenum pname, const GLfloat *params)` | medium | tail-calls 0x8072ac64 |
| glPointParameterx | 0x80defcac | 0x4c | `void glPointParameterx(GLenum pname, GLfixed param)` | low | 0x8126/0x8129 checks, conversion, 0x8072ac64 |
| glPointParameterxv | 0x80defcf8 | 0x5c | `void glPointParameterxv(GLenum pname, const GLfixed *params)` | low | 0x8129 check, conversion, 0x8072ac64 |
| glPointSize | 0x80defd54 | 0x1a | `void glPointSize(GLfloat size)` | high | GFX_glPointSize 0x806860a4 -> shim 0x80debc2c; vmov s0 then 0x808f8858 |
| glPointSizex | 0x80defe4c | 0x2c | `void glPointSizex(GLfixed size)` | high | fixed->float then 0x808f8858 |
| glPopMatrix | 0x802e43e6 | 0x16 | `void glPopMatrix(void)` | high | GFX_glPopMatrix 0x8099fd78 -> shim 0x802af50e; jumps through ctx+0x888 |
| glPushMatrix | 0x802e43fc | 0x16 | `void glPushMatrix(void)` | high | GFX_glPushMatrix 0x8099fd34 -> shim 0x802af512; jumps through ctx+0x884 |
| glReadPixels | 0x80370c6c | 0x248 | `void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)` | high | GFX_glReadPixels 0x8032ae74 -> shim 0x8034cbe0 (7 args); format 0x1907/0x1908, type 0x1401 |
| glRotatef | 0x802b0328 | 0x158 | `void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)` | high | GFX_glRotatef 0x8099e8a4 -> shim 0x802724c4; sin/cos 0x806eafa4/0x806eaf94 |
| glScalef | 0x802e4412 | 0x3a | `void glScalef(GLfloat x, GLfloat y, GLfloat z)` | high | GFX_glScalef 0x80979e50 -> shim 0x802af516 |
| glScissor | 0x80272cb6 | 0xc0 | `void glScissor(GLint x, GLint y, GLsizei width, GLsizei height)` | high | GFX_glScissor 0x8098b6c4 -> shim 0x8023b3b0; w/h<0 -> GL_INVALID_VALUE |
| glShadeModel | 0x802e444c | 0x4c | `void glShadeModel(GLenum mode)` | high | GFX_glShadeModel 0x8098ae3c -> shim 0x80312c7c; 0x1D00/0x1D01 |
| glTexCoordPointer | 0x803324ac | 0x118 | `void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *pointer)` | high | GFX_glTexCoordPointer 0x8096cba4 -> shim 0x80312c82 |
| glTexEnvf | 0x802e4498 | 0x86 | `void glTexEnvf(GLenum target, GLenum pname, GLfloat param)` | high | Garmin GFX_glTexEnvf shim 0x80312c90 tail; Garmin bring-up calls (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, 8448.0f = GL_MODULATE); TexEnv common 0x80265ef8 |
| glTexEnvfv | 0x80df01aa | 0x1e | `void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params)` | high | GFX_glTexEnvfv 0x806863c8 -> shim 0x80debc38; tail-calls 0x80265ef8 |
| glTexEnvx | 0x80df01c8 | 0x86 | `void glTexEnvx(GLenum target, GLenum pname, GLfixed param)` | high | GFX_glTexEnvx 0x8068642c -> shim 0x80debc42; 0x8570/0x8571/0x8862; fixed common 0x80a3dbec |
| glTexEnvxv | 0x80df024e | 0x1e | `void glTexEnvxv(GLenum target, GLenum pname, const GLfixed *params)` | medium | tail-calls the fixed TexEnv common 0x80a3dbec |
| glTexImage2D | 0x802b0480 | 0x33e | `void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)` | high | GFX_glTexImage2D 0x8099303c -> shim 0x802724d2 (9 args); formats 0x1906..0x190A, types 0x1401/0x8033/0x8034/0x8363 |
| glTexParameterf | 0x802b07be | 0x24 | `void glTexParameterf(GLenum target, GLenum pname, GLfloat param)` | high | GFX_glTexParameterf 0x8099312c -> shim 0x802724fc; TexParameter common 0x80986bbc(type=0,vec=0) |
| glTexParameterfv | 0x80df03e4 | 0x26 | `void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)` | high | 0x80986bbc(type=0,vec=1) |
| glTexParameteri | 0x80718444 | 0x26 | `void glTexParameteri(GLenum target, GLenum pname, GLint param)` | high | Garmin bring-up 0x801d10da calls (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); 0x80986bbc(type=1,vec=0) |
| glTexParameteriv | 0x80df0456 | 0x24 | `void glTexParameteriv(GLenum target, GLenum pname, const GLint *params)` | high | 0x80986bbc(type=1,vec=1) |
| glTexParameterx | 0x80df040a | 0x26 | `void glTexParameterx(GLenum target, GLenum pname, GLfixed param)` | high | 0x80986bbc(type=2,vec=0) |
| glTexParameterxv | 0x80df0430 | 0x26 | `void glTexParameterxv(GLenum target, GLenum pname, const GLfixed *params)` | high | 0x80986bbc(type=2,vec=1) |
| glTexSubImage2D | 0x80df047c | 0x748 | `void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels)` | high | GFX_glTexSubImage2D 0x8068655c -> shim 0x80debc50 (9 args) |
| glTranslatef | 0x802b07e2 | 0x3a | `void glTranslatef(GLfloat x, GLfloat y, GLfloat z)` | high | GFX_glTranslatef 0x8099fcb0 -> shim 0x80272506; matrix helper 0x8097cc98 |
| glVertexPointer | 0x803325c4 | 0xee | `void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *pointer)` | high | GFX_glVertexPointer 0x8098ae8c -> shim 0x80312cd4 |
| glViewport | 0x80272d76 | 0x128 | `void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)` | high | w|h<0 -> GL_INVALID_VALUE; clamps to the surface dims (ctx+0xEE4 -> +0x24/+0x28); stores ctx+0x680..0x68C |

## Extension entry points (from the driver's GetProcAddress table)

All high confidence (direct `{name -> movw/movt fn}` mapping in 0x80688bb0); prototypes per the OES/IMG/EXT extension specs. glDrawTex{f,i,s,x}OES take (x, y, z, width, height) = 5 args; the *v variants take one pointer.

| name | address | len | args |
|---|---|---|---|
| glBindFramebufferOES | 0x80debe28 | 0x1ae | 2 |
| glBindRenderbufferOES | 0x80debfd6 | 0xd2 | 2 |
| glBindVertexArrayOES | 0x80dec0a8 | 0xc4 | 1 |
| glBlendEquationOES | 0x80dec16c | 0x58 | 1 |
| glBlendEquationSeparateOES | 0x80dec1c4 | 0x80 | 2 |
| glCheckFramebufferStatusOES | 0x80dec2ee | 0x26 | 1 |
| glCurrentPaletteMatrixOES | 0x80deddd8 | 0x3a | 1 |
| glDeleteFramebuffersOES | 0x80dede12 | 0x90 | 2 |
| glDeleteRenderbuffersOES | 0x80dedea2 | 0x8a | 2 |
| glDeleteVertexArraysOES | 0x80dedf2c | 0x7e | 2 |
| glDrawTexfOES | 0x80dee044 | 0x44 | 5 |
| glDrawTexfvOES | 0x80dee088 | 0x1a | 1 |
| glDrawTexiOES | 0x80dee0a2 | 0x48 | 5 |
| glDrawTexivOES | 0x80dee0ea | 0x3e | 1 |
| glDrawTexsOES | 0x80dee128 | 0x48 | 5 |
| glDrawTexsvOES | 0x80dee170 | 0x52 | 1 |
| glDrawTexxOES | 0x80dee1c4 | 0x64 | 5 |
| glDrawTexxvOES | 0x80dee228 | 0x5c | 1 |
| glEGLImageTargetRenderbufferStorageOES | 0x80dee284 | 0x94 | 2 |
| glEGLImageTargetTexture2DOES | 0x80dee318 | 0x12c | 2 |
| glFramebufferRenderbufferOES | 0x80dee444 | 0xe2 | 4 |
| glFramebufferTexture2DOES | 0x80dee526 | 0x136 | 5 |
| glGenFramebuffersOES | 0x80dee79c | 0x3c | 2 |
| glGenRenderbuffersOES | 0x80dee7d8 | 0x3c | 2 |
| glGenVertexArraysOES | 0x80dee814 | 0x38 | 2 |
| glGenerateMipmapOES | 0x80dee84c | 0x136 | 1 |
| glGetBufferPointervOES | 0x80dee982 | 0x60 | 3 |
| glGetFramebufferAttachmentParameterivOES | 0x80deea00 | 0x102 | 4 |
| glGetRenderbufferParameterivOES | 0x80deeb5c | 0xa0 | 3 |
| glGetTexGenfvOES | 0x80deecb8 | 0x3a | 3 |
| glGetTexGenivOES | 0x80deecf2 | 0x32 | 3 |
| glGetTexGenxvOES | 0x80deed24 | 0x32 | 3 |
| glGetTexStreamDeviceAttributeivIMG | 0x80deed9a | 0xa2 | 3 |
| glGetTexStreamDeviceNameIMG | 0x80deee3c | 0x4c | 1 |
| glIsFramebufferOES | 0x80deeea0 | 0x3a | 1 |
| glIsRenderbufferOES | 0x80deeeda | 0x3a | 1 |
| glLoadPaletteFromModelViewMatrixOES | 0x80def03c | 0x38 | 0 |
| glMapBufferOES | 0x80def0b0 | 0x76 | 2 |
| glMatrixIndexPointerOES | 0x80def1c0 | 0xda | 4 |
| glMultiDrawArraysEXT | 0x80def2f8 | 0x2c2 | 4 |
| glMultiDrawElementsEXT | 0x80def5bc | 0x62c | 5 |
| glPointSizePointerOES | 0x80defd6e | 0xdc | 3 |
| glQueryMatrixxOES | 0x80defe78 | 0x84 | 2 |
| glRenderbufferStorageOES | 0x80defefc | 0x1f6 | 4 |
| glTexBindStreamIMG | 0x80df00f2 | 0xb8 | 2 |
| glTexGenfOES | 0x80df026c | 0x46 | 3 |
| glTexGenfvOES | 0x80df02b2 | 0x46 | 3 |
| glTexGeniOES | 0x80df02f8 | 0x3a | 3 |
| glTexGenivOES | 0x80df0332 | 0x3c | 3 |
| glTexGenxOES | 0x80df036e | 0x3a | 3 |
| glTexGenxvOES | 0x80df03a8 | 0x3c | 3 |
| glUnmapBufferOES | 0x80df0bc4 | 0x52 | 1 |
| glWeightPointerOES | 0x80df0c16 | 0xf4 | 4 |

## EGL entry points

| name | address | len | prototype | confidence | evidence |
|---|---|---|---|---|---|
| eglGetError | 0x806acf1e | 0x24 | `EGLint eglGetError(void)` | high | reads the per-API error from the thread data (IMGeglGetTLS 0x805e5846), resets it to EGL_SUCCESS 0x3000 |
| eglGetDisplay | 0x80054f28 | 0x25c | `EGLDisplay eglGetDisplay(EGLNativeDisplayType id)` | high | veneer 0x800534d4 called from Garmin init 0x80051a44 with r0=0 (EGL_DEFAULT_DISPLAY) right before eglInitialize |
| eglInitialize | 0x80055184 | 0x2a8 | `EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)` | high | veneer 0x800534d8; errors 0x3001/0x3003/0x3008; called 2nd in 0x80051a44 |
| eglChooseConfig | 0x80054ca0 | 0x288 | `EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config)` | high | veneer 0x800534d0; 0x80051a44 passes the attrib list 0x816b8438 (RGB565, depth 16, WINDOW_BIT), configs=0xa22f4ce0, size=1 |
| eglGetConfigs | 0x806ace94 | 0x8a | `EGLBoolean eglGetConfigs(EGLDisplay, EGLConfig*, EGLint, EGLint*)` | medium | errors 0x3001/0x3008/0x300C, small |
| eglGetConfigAttrib | 0x806acde8 | 0xac | `EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint attribute, EGLint *value)` | medium | attribute range starts at 0x3020, EGL_BAD_ATTRIBUTE 0x3004 |
| eglCreateWindowSurface | 0x8026040c | 0x39c | `EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list)` | high | veneer 0x802398d2; EGL_BAD_NATIVE_WINDOW 0x300B, EGL_RENDER_BUFFER 0x3086, EGL_VG_COLORSPACE 0x3087; Garmin 0x801d10da calls with win=NULL, attribs=NULL |
| eglCreatePbufferSurface | 0x806aaea8 | 0x57c | `EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint*)` | medium | EGL_MAX_PBUFFER_* 0x302A..0x302C |
| eglCreatePixmapSurface | 0x806ab574 | 0x394 | `EGLSurface eglCreatePixmapSurface(EGLDisplay, EGLConfig, EGLNativePixmapType, const EGLint*)` | medium | EGL_BAD_NATIVE_PIXMAP 0x300A, 0x3086..0x3088 |
| eglDestroySurface | 0x8032b228 | 0xe4 | `EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface)` | medium | veneer 0x80312266 used by the Garmin teardown 0x802cf008; EGL_BAD_SURFACE 0x300D |
| eglQuerySurface | 0x806ad4ae | 0x10e | `EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface, EGLint attribute, EGLint *value)` | medium | 0x3028 CONFIG_ID, 0x3084..0x3086, 0x3096 |
| eglSurfaceAttrib | 0x806aadf6 | 0xb2 | `EGLBoolean eglSurfaceAttrib(EGLDisplay, EGLSurface, EGLint attribute, EGLint value)` | low | 0x3096 EGL_MULTISAMPLE_RESOLVE, EGL_BAD_PARAMETER |
| eglBindAPI | 0x806a9574 | 0x38 | `EGLBoolean eglBindAPI(EGLenum api)` | high | accepts 0x30A0 EGL_OPENGL_ES_API, EGL_BAD_PARAMETER otherwise |
| eglQueryAPI | 0x806ad458 | 0x2c | `EGLenum eglQueryAPI(void)` | medium | returns 0x3038 EGL_NONE when unbound |
| eglQueryString | 0x806adb3c | 0xc0 | `const char *eglQueryString(EGLDisplay dpy, EGLint name)` | high | 0x3053 VENDOR, 0x3054 VERSION, 0x3055 EXTENSIONS, 0x308D CLIENT_APIS |
| eglCreateContext | 0x80260108 | 0x304 | `EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share, const EGLint *attrib_list)` | high | veneer 0x802398ce; loads the GLES1 interface via 0x8029b2ca (glGetString(0x6500)); Garmin 0x801d10da calls with share=0, attribs=0 |
| eglDestroyContext | 0x8032b19c | 0x8c | `EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx)` | medium | veneer 0x80312262 used by the Garmin teardown 0x802cf008; EGL_BAD_CONTEXT 0x3006 |
| eglMakeCurrent | 0x802ffe68 | 0x568 | `EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx)` | high | veneer 0x80929f76 called from 0x801d10da / 0x801d12d8 / 0x802cf008 / 0x8069754a; calls the GLES1 MakeCurrent 0x806897c8 through the API table -> TLS setter 0x8073ce7a |
| eglGetCurrentContext (or Display) | 0x806acf42 | 0x2a | `EGLContext eglGetCurrentContext(void)` | low | returns field +0x14 of the current API record |
| eglGetCurrentSurface | 0x806acf6c | 0x46 | `EGLSurface eglGetCurrentSurface(EGLint readdraw)` | medium | 0x3059 EGL_DRAW / 0x305A EGL_READ |
| eglSwapInterval | 0x806ae642 | 0x140 | `EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval)` | high | clamps to the config's 0x303B/0x303C MIN/MAX_SWAP_INTERVAL |
| eglSwapBuffers | 0x80357766 | 0x1f2 | `EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)` | high | veneer 0x8034af66 <- 0x8032b184 <- Garmin shim 0x80312ce2 <- GFX present wrapper 0x8096518e; calls the GLES1 FlushBuffers (API slot 6, 0x80688238) then the display-class swap callback [surface+0x24]->+0x1C; errors 0x300D/0x300B/0x300E |
| eglWaitGL | 0x802607a8 | 0x6e | `EGLBoolean eglWaitGL(void)` | medium | veneer 0x802398d6 called by Garmin 0x801d12d8; flushes via API slot 6 with wait=1; EGL_BAD_CURRENT_SURFACE 0x3007 |
| eglWaitNative | 0x806ae9fe | 0xec | `EGLBoolean eglWaitNative(EGLint engine)` | medium | 0x305B EGL_CORE_NATIVE_ENGINE |
| eglWaitClient | 0x806ae990 | 0x6e | `EGLBoolean eglWaitClient(void)` | low | same shape as eglWaitGL (0x3007/0x300E) |
| eglCopyBuffers | 0x806a9c24 | 0x1ce | `EGLBoolean eglCopyBuffers(EGLDisplay, EGLSurface, EGLNativePixmapType)` | low | EGL_BAD_NATIVE_PIXMAP 0x300A plus surface/context checks |
| eglTerminate | 0x806ae78e | 0x202 | `EGLBoolean eglTerminate(EGLDisplay dpy)` | low | large, only EGL_BAD_DISPLAY 0x3008 |
| eglReleaseThread | 0x806ae464 | 0x3e | `EGLBoolean eglReleaseThread(void)` | low | calls eglMakeCurrent 0x802ffe68 internally |
| eglCreateImageKHR | 0x806a9e40 | 0x35c | `EGLImageKHR eglCreateImageKHR(EGLDisplay, EGLContext, EGLenum target, EGLClientBuffer, const EGLint*)` | high | eglGetProcAddress table 0x81319f04 |
| eglDestroyImageKHR | 0x806ac9cc | 0x6e | `EGLBoolean eglDestroyImageKHR(EGLDisplay, EGLImageKHR)` | high | table 0x81319f0c |
| eglCreateSyncKHR | 0x806ac794 | 0x1b4 | `EGLSyncKHR eglCreateSyncKHR(EGLDisplay, EGLenum, const EGLint*)` | high | table 0x81319f14 |
| eglDestroySyncKHR | 0x806acad8 | 0x162 | `EGLBoolean eglDestroySyncKHR(EGLDisplay, EGLSyncKHR)` | high | table 0x81319f1c |
| eglClientWaitSyncKHR | 0x806a96e0 | 0x270 | `EGLint eglClientWaitSyncKHR(EGLDisplay, EGLSyncKHR, EGLint flags, EGLTimeKHR timeout)` | high | table 0x81319f24 (64-bit timeout = 2 slots) |
| eglGetSyncAttribKHR | 0x806ad2d2 | 0xc6 | `EGLBoolean eglGetSyncAttribKHR(EGLDisplay, EGLSyncKHR, EGLint, EGLint*)` | high | table 0x81319f2c |
| eglCreateSharedImageNOK | 0x806ac550 | 0x162 | `EGLNativeSharedImageTypeNOK eglCreateSharedImageNOK(EGLDisplay, EGLImageKHR, EGLint*)` | high | table 0x81319f34 |
| eglDestroySharedImageNOK | 0x806aca3a | 0x9e | `EGLBoolean eglDestroySharedImageNOK(EGLDisplay, EGLNativeSharedImageTypeNOK)` | high | table 0x81319f3c |
| eglQueryImageNOK | 0x806ad7f4 | 0x80 | `EGLBoolean eglQueryImageNOK(EGLDisplay, EGLImageKHR, EGLint, EGLint*)` | high | table 0x81319f44 |
| GLES1 GetProcAddress (eglGetProcAddress back end) | 0x80688bb0 | 0x71c | `void *GLESGetProcAddress(const char *name)` | high | strcmp chain over the names at 0x82094c0f..; API table slot 1 |

## Garmin-side glue, helpers and tables

| what | address | len | notes |
|---|---|---|---|
| GFX present (swap) wrapper | 0x8096518e | 0x2e | void GFX_present(gfx_ctx *ctx): ctx->pipeline->type==3 -> 0x80312ce2 -> 0x8032b184 -> eglSwapBuffers(dpy=*0xa22f4ce4, surf=*(0xa3cf0448+8)) |
| Garmin swap shim | 0x80312ce2 | 0x8 | reads ctx+0x40 (pipeline) +0x18, jumps to 0x8032b184 |
| eglSwapBuffers argument loader | 0x8032b184 | 0x18 | r0=*(0xa22f4ce4) display, r1=*(0xa3cf0448+8) surface; b.w veneer 0x8034af66 |
| Garmin EGL bring-up | 0x801d10da | 0x1fe | eglCreateContext, eglCreateWindowSurface(win=NULL), eglMakeCurrent, default GL state, eglMakeCurrent(NULL) |
| Garmin EGL display init | 0x80051a44 | 0xf0 | eglGetDisplay(0), eglInitialize, eglChooseConfig(attribs 0x816b8438) -> config at 0xa22f4ce0, display at 0xa22f4ce4 |
| Garmin EGL teardown | 0x802cf008 | 0x86 | eglMakeCurrent(NULL), eglDestroySurface, eglDestroyContext via the veneers 0x80312266 / 0x80312262 |
| Render loop end-of-frame | 0x802a6160 | 0x1ec8 | ... GFX_glClearColor, GFX_glDepthMask, GFX_glClear, GFX_glDepthMask, GFX_present (call sites 0x802a627a, 0x802a7c04) |
| GetCurrentContext (GLES1) | 0x80986ec8 | 0x36 | gles1_ctx *GetCurrentContext(void): thread id (0x805e63ee, constant 0x539) looked up in the table at 0xa1fe7e58 (count at 0xa1fe7f58) |
| SetError (GLES1) | 0x80986efe | 0xe | void SetError(gles1_ctx *ctx, GLenum err): stores err at ctx+0x868 if zero |
| SetCurrentContext (GLES1) | 0x8073ce7a | 0x5e | used by the GLES MakeCurrent callback 0x806897c8 (API table slot 4) |
| IMGeglGetTLS | 0x805e5846 | 0x34 | EGL thread-data getter (callback arg 0x80303fa5); every EGL entry point calls it first |
| GLES1 API table | 0x8200ecb8 | 0x0 | {version 3, GetProcAddress 0x80688bb0, CreateGC 0x80686d84, DestroyGC 0x80688160, MakeCurrent 0x806897c8, wait/uncurrent 0x80689904, FlushBuffers 0x80688238, 0x80686cd6, 0x806899d0, 0x806882a0, 0x8068998a}; returned by glGetString(0x6500) |
| GFX_gl* wrapper shim table | 0x80debbb4 | 0x2 | Garmin shims dropping the ctx argument; entries for glClearColorx, glClearDepthx, glColor4x, glDepthRangex, glGetFixedv, glLineWidthx, glLoadMatrixx, glMultMatrixx, glPointSizex, glRotatex, glScalex, glTexEnvxv, glTexParameterx, glTranslatex are `bx lr` no-ops |

## How frames are presented

1. `0x80051a44` (early graphics init): `eglGetDisplay(0)` -> `*0xa22f4ce4`, `eglInitialize`, `eglChooseConfig(dpy, attribs@0x816b8438, &config@0xa22f4ce0, 1, &n)`. The attribute list is `{EGL_RED_SIZE 5, EGL_GREEN_SIZE 6, EGL_BLUE_SIZE 5, EGL_DEPTH_SIZE 16, EGL_SURFACE_TYPE EGL_WINDOW_BIT, EGL_BUFFER_SIZE 16, EGL_NONE}`, i.e. RGB565 colour plus a 16-bit depth buffer.
2. `0x801d10da` (GL task bring-up): `eglCreateContext(dpy, cfg, 0, 0)` -> `*(0xa3cf0448+4)`, `eglCreateWindowSurface(dpy, cfg, NULL, NULL)` -> `*(0xa3cf0448+8)`, `eglMakeCurrent(dpy, s, s, c)`, default state (`glColor4f(1,1,1,1)`, `glCullFace(GL_BACK)`, `glDepthFunc(GL_LESS)`, `glDepthMask(1)`, `glFrontFace(GL_CCW)`, `glShadeModel(GL_SMOOTH)`, `glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE)`, `glTexParameteri` MAG_FILTER/WRAP_S/WRAP_T, `glDisable` BLEND/DITHER/DEPTH_TEST/TEXTURE_2D/LIGHTING), then `eglMakeCurrent(dpy,0,0,0)`. The native window is NULL, so the surface is the display-class system buffer: 1024x600 RGB565 (the two framebuffers 0xbed46000 / 0xbefc6000 seen on the DISPC GFX_BA0/BA1 flips).
3. Per frame the render loop (0x802a6160, call sites 0x802a627a / 0x802a7c04; also 0x80cf56fc) ends with `GFX_glClearColor`, `GFX_glDepthMask`, `GFX_glClear`, `GFX_glDepthMask`, then `GFX_present` 0x8096518e -> 0x80312ce2 -> 0x8032b184 -> `eglSwapBuffers(*0xa22f4ce4, *(0xa3cf0448+8))` 0x80357766. eglSwapBuffers calls the GLES1 FlushBuffers callback (API table slot 6 = 0x80688238, which kicks the TA/3D through the PVRSRV bridge 0x805e63f4 / SGXKickTA 0x8034114e) and then the display-class swap callback `[surface+0x24]->+0x1c` (PVRSRV SwapToDCBuffer -> kernel display class -> Garmin DSS driver flips GFX_BA on VSYNC). There is no Garmin-specific present path outside eglSwapBuffers.
4. Hooking strategy for QEMU: hook the driver GL functions listed below (they receive plain GL arguments in r0-r3/stack, AAPCS soft-float: GLfloat values travel in core registers as IEEE bit patterns) to mirror state and draws on the host, and hook `eglSwapBuffers` (or the FlushBuffers slot) to present the host-rendered frame into the framebuffer that the following DISPC flip displays. Returning immediately from `eglSwapBuffers` with r0=1 (EGL_TRUE) skips the GPU kick entirely; if the fake SGX microkernel path should keep running, hook after the call instead.

## GLES1 context structure (offsets observed)

* `+0x52c` texture-environment state pointer (glTexEnv internal 0x8029e828 indexes it)
* `+0x634` cull-face mode (glCullFace); `+0x680..0x68c` viewport x,y,w,h (glViewport)
* `+0x7c4`, `+0x944`, `+0x94c` matrix stack pointers used by glLoadMatrixf per matrix mode; `+0x860` current matrix mode (0x1700/0x1701/0x1702/0x8840)
* `+0x800..` material ambient/diffuse/specular (Material common 0x80980984)
* `+0x864` client active texture unit; `+0x868` pending GL error (SetError); `+0x86c` dirty-state bitmask (bit 0 set by glCullFace etc.)
* `+0x884` / `+0x888` / `+0x88c` function pointers for glPushMatrix / glPopMatrix / glLoadIdentity of the current matrix mode (matrix-mode specific bodies 0x8029ce80/0x8029cec2/0x8029cf00 push, 0x8029cd88 pop)
* `+0x9d8` -> client vertex-array block: `+0x20` enabled-array bits, `+0x24` vertex pointer, `+0x34` normal, `+0x44` colour, `+0x54 + unit*0x10` texcoord pointers, `+0x94` point-size, `+0xa4` weight, `+0xb4` matrix-index (glGetPointerv)
* `+0xd14` current framebuffer object (NULL = window); `+0xee4` render-surface params (`+0x24/+0x28` = width/height), `+0xee8` / `+0xef0` surface / render-target handles, `+0xf30` flush mode, `+0xfbc` sync objects (waited on by 0x80689904)

## Machine-readable hook list

Arguments = number of 32-bit register/stack argument slots (AAPCS soft-float ABI: GLfloat in core registers; the 64-bit EGLTimeKHR counts 2). Medium/low-confidence entries are included as comments.

```c
/* OpenGL ES 1.1 core */
HOOK("glAlphaFunc", 0x80332046, 2)
HOOK("glBindBuffer", 0x80272510, 2)
HOOK("glBindTexture", 0x802b0140, 2)
HOOK("glBlendFunc", 0x803d02c0, 2)
HOOK("glBlendFuncSeparateOES", 0x803320b8, 4)
HOOK("glBufferData", 0x80272662, 4)
/* medium: HOOK("glBufferSubData", 0x80dec244, 4) */
HOOK("glClear", 0x803320da, 1)
HOOK("glClearColor", 0x803321a0, 4)
HOOK("glClearDepthf", 0x80dec314, 1)
HOOK("glColor4f", 0x802e4250, 4)
HOOK("glColorMask", 0x80dec332, 4)
HOOK("glColorPointer", 0x805d2304, 4)
HOOK("glCompressedTexImage2D", 0x80dec380, 8)
/* medium: HOOK("glCompressedTexSubImage2D", 0x80dec9f0, 9) */
/* medium: HOOK("glCopyTexImage2D", 0x80decb60, 8) */
/* medium: HOOK("glCopyTexSubImage2D", 0x80ded198, 8) */
HOOK("glCullFace", 0x802727ba, 1)
HOOK("glDeleteBuffers", 0x803134c2, 2)
HOOK("glDeleteTextures", 0x803135a2, 2)
HOOK("glDepthFunc", 0x8023b58a, 1)
HOOK("glDepthMask", 0x8023b5d2, 1)
HOOK("glDepthRangef", 0x80dedfaa, 2)
HOOK("glDisable", 0x80852a88, 1)
HOOK("glDisableClientState", 0x802b0194, 1)
HOOK("glDrawArrays", 0x80332204, 3)
HOOK("glDrawElements", 0x805d23e8, 4)
HOOK("glEnable", 0x80272800, 1)
HOOK("glEnableClientState", 0x802b0224, 1)
HOOK("glFinish", 0x80313706, 0)
HOOK("glFlush", 0x802e42b0, 0)
HOOK("glFrontFace", 0x80583334, 1)
HOOK("glFrustumf", 0x802e42e6, 6)
HOOK("glFrustumx", 0x80dee65c, 6)
HOOK("glGenBuffers", 0x80272b58, 2)
HOOK("glGenTextures", 0x802b02b2, 2)
HOOK("glGetBooleanv", 0x805bdb2c, 2)
HOOK("glGetFloatv", 0x802b02ee, 2)
HOOK("glGetIntegerv", 0x802b030a, 2)
/* medium: HOOK("glGetLightfv", 0x805bdb48, 3) */
HOOK("glGetPointerv", 0x805bdb68, 2)
HOOK("glGetString", 0x803137bc, 1)
/* low: HOOK("glGetTexEnv{fv,iv,xv}", 0x805bdbe4, 3) */
/* low: HOOK("glGetTexParameter{fv,iv,xv}", 0x805bdc02, 3) */
/* medium: HOOK("glIsEnabled", 0x80deee88, 1) */
HOOK("glLightf", 0x80deef4a, 3)
HOOK("glLightfv", 0x805bdc24, 3)
/* medium: HOOK("glLightx", 0x80deef80, 3) */
/* medium: HOOK("glLightxv", 0x80deefcc, 3) */
HOOK("glLineWidth", 0x80370bde, 1)
HOOK("glLoadIdentity", 0x80272b94, 0)
HOOK("glLoadMatrixf", 0x805bdc42, 1)
HOOK("glLogicOp", 0x80def074, 1)
/* medium: HOOK("glMaterialfv", 0x80def126, 3) */
/* low: HOOK("glMaterialf/x (scalar)", 0x80def144, 3) */
HOOK("glMatrixMode", 0x80272baa, 1)
HOOK("glMultMatrixf", 0x80def29a, 1)
HOOK("glNormalPointer", 0x805bdce4, 3)
HOOK("glOrthof", 0x80272c6a, 6)
HOOK("glOrthox", 0x80defbe8, 6)
/* medium: HOOK("glPointParameterf", 0x80defc60, 2) */
/* medium: HOOK("glPointParameterfv", 0x80defc92, 2) */
/* low: HOOK("glPointParameterx", 0x80defcac, 2) */
/* low: HOOK("glPointParameterxv", 0x80defcf8, 2) */
HOOK("glPointSize", 0x80defd54, 1)
HOOK("glPointSizex", 0x80defe4c, 1)
HOOK("glPopMatrix", 0x802e43e6, 0)
HOOK("glPushMatrix", 0x802e43fc, 0)
HOOK("glReadPixels", 0x80370c6c, 7)
HOOK("glRotatef", 0x802b0328, 4)
HOOK("glScalef", 0x802e4412, 3)
HOOK("glScissor", 0x80272cb6, 4)
HOOK("glShadeModel", 0x802e444c, 1)
HOOK("glTexCoordPointer", 0x803324ac, 4)
HOOK("glTexEnvf", 0x802e4498, 3)
HOOK("glTexEnvfv", 0x80df01aa, 3)
HOOK("glTexEnvx", 0x80df01c8, 3)
/* medium: HOOK("glTexEnvxv", 0x80df024e, 3) */
HOOK("glTexImage2D", 0x802b0480, 9)
HOOK("glTexParameterf", 0x802b07be, 3)
HOOK("glTexParameterfv", 0x80df03e4, 3)
HOOK("glTexParameteri", 0x80718444, 3)
HOOK("glTexParameteriv", 0x80df0456, 3)
HOOK("glTexParameterx", 0x80df040a, 3)
HOOK("glTexParameterxv", 0x80df0430, 3)
HOOK("glTexSubImage2D", 0x80df047c, 9)
HOOK("glTranslatef", 0x802b07e2, 3)
HOOK("glVertexPointer", 0x803325c4, 4)
HOOK("glViewport", 0x80272d76, 4)
/* extensions (GetProcAddress table) */
HOOK("glBindFramebufferOES", 0x80debe28, 2)
HOOK("glBindRenderbufferOES", 0x80debfd6, 2)
HOOK("glBindVertexArrayOES", 0x80dec0a8, 1)
HOOK("glBlendEquationOES", 0x80dec16c, 1)
HOOK("glBlendEquationSeparateOES", 0x80dec1c4, 2)
HOOK("glCheckFramebufferStatusOES", 0x80dec2ee, 1)
HOOK("glCurrentPaletteMatrixOES", 0x80deddd8, 1)
HOOK("glDeleteFramebuffersOES", 0x80dede12, 2)
HOOK("glDeleteRenderbuffersOES", 0x80dedea2, 2)
HOOK("glDeleteVertexArraysOES", 0x80dedf2c, 2)
HOOK("glDrawTexfOES", 0x80dee044, 5)
HOOK("glDrawTexfvOES", 0x80dee088, 1)
HOOK("glDrawTexiOES", 0x80dee0a2, 5)
HOOK("glDrawTexivOES", 0x80dee0ea, 1)
HOOK("glDrawTexsOES", 0x80dee128, 5)
HOOK("glDrawTexsvOES", 0x80dee170, 1)
HOOK("glDrawTexxOES", 0x80dee1c4, 5)
HOOK("glDrawTexxvOES", 0x80dee228, 1)
HOOK("glEGLImageTargetRenderbufferStorageOES", 0x80dee284, 2)
HOOK("glEGLImageTargetTexture2DOES", 0x80dee318, 2)
HOOK("glFramebufferRenderbufferOES", 0x80dee444, 4)
HOOK("glFramebufferTexture2DOES", 0x80dee526, 5)
HOOK("glGenFramebuffersOES", 0x80dee79c, 2)
HOOK("glGenRenderbuffersOES", 0x80dee7d8, 2)
HOOK("glGenVertexArraysOES", 0x80dee814, 2)
HOOK("glGenerateMipmapOES", 0x80dee84c, 1)
HOOK("glGetBufferPointervOES", 0x80dee982, 3)
HOOK("glGetFramebufferAttachmentParameterivOES", 0x80deea00, 4)
HOOK("glGetRenderbufferParameterivOES", 0x80deeb5c, 3)
HOOK("glGetTexGenfvOES", 0x80deecb8, 3)
HOOK("glGetTexGenivOES", 0x80deecf2, 3)
HOOK("glGetTexGenxvOES", 0x80deed24, 3)
HOOK("glGetTexStreamDeviceAttributeivIMG", 0x80deed9a, 3)
HOOK("glGetTexStreamDeviceNameIMG", 0x80deee3c, 1)
HOOK("glIsFramebufferOES", 0x80deeea0, 1)
HOOK("glIsRenderbufferOES", 0x80deeeda, 1)
HOOK("glLoadPaletteFromModelViewMatrixOES", 0x80def03c, 0)
HOOK("glMapBufferOES", 0x80def0b0, 2)
HOOK("glMatrixIndexPointerOES", 0x80def1c0, 4)
HOOK("glMultiDrawArraysEXT", 0x80def2f8, 4)
HOOK("glMultiDrawElementsEXT", 0x80def5bc, 5)
HOOK("glPointSizePointerOES", 0x80defd6e, 3)
HOOK("glQueryMatrixxOES", 0x80defe78, 2)
HOOK("glRenderbufferStorageOES", 0x80defefc, 4)
HOOK("glTexBindStreamIMG", 0x80df00f2, 2)
HOOK("glTexGenfOES", 0x80df026c, 3)
HOOK("glTexGenfvOES", 0x80df02b2, 3)
HOOK("glTexGeniOES", 0x80df02f8, 3)
HOOK("glTexGenivOES", 0x80df0332, 3)
HOOK("glTexGenxOES", 0x80df036e, 3)
HOOK("glTexGenxvOES", 0x80df03a8, 3)
HOOK("glUnmapBufferOES", 0x80df0bc4, 1)
HOOK("glWeightPointerOES", 0x80df0c16, 4)
/* EGL */
HOOK("eglGetError", 0x806acf1e, 0)
HOOK("eglGetDisplay", 0x80054f28, 1)
HOOK("eglInitialize", 0x80055184, 3)
HOOK("eglChooseConfig", 0x80054ca0, 5)
/* medium: HOOK("eglGetConfigs", 0x806ace94, 4) */
/* medium: HOOK("eglGetConfigAttrib", 0x806acde8, 4) */
HOOK("eglCreateWindowSurface", 0x8026040c, 4)
/* medium: HOOK("eglCreatePbufferSurface", 0x806aaea8, 3) */
/* medium: HOOK("eglCreatePixmapSurface", 0x806ab574, 4) */
/* medium: HOOK("eglDestroySurface", 0x8032b228, 2) */
/* medium: HOOK("eglQuerySurface", 0x806ad4ae, 4) */
/* low: HOOK("eglSurfaceAttrib", 0x806aadf6, 4) */
HOOK("eglBindAPI", 0x806a9574, 1)
/* medium: HOOK("eglQueryAPI", 0x806ad458, 0) */
HOOK("eglQueryString", 0x806adb3c, 2)
HOOK("eglCreateContext", 0x80260108, 4)
/* medium: HOOK("eglDestroyContext", 0x8032b19c, 2) */
HOOK("eglMakeCurrent", 0x802ffe68, 4)
/* low: HOOK("eglGetCurrentContext (or Display)", 0x806acf42, 0) */
/* medium: HOOK("eglGetCurrentSurface", 0x806acf6c, 1) */
HOOK("eglSwapInterval", 0x806ae642, 2)
HOOK("eglSwapBuffers", 0x80357766, 2)
/* medium: HOOK("eglWaitGL", 0x802607a8, 0) */
/* medium: HOOK("eglWaitNative", 0x806ae9fe, 1) */
/* low: HOOK("eglWaitClient", 0x806ae990, 0) */
/* low: HOOK("eglCopyBuffers", 0x806a9c24, 3) */
/* low: HOOK("eglTerminate", 0x806ae78e, 1) */
/* low: HOOK("eglReleaseThread", 0x806ae464, 0) */
HOOK("eglCreateImageKHR", 0x806a9e40, 5)
HOOK("eglDestroyImageKHR", 0x806ac9cc, 2)
HOOK("eglCreateSyncKHR", 0x806ac794, 3)
HOOK("eglDestroySyncKHR", 0x806acad8, 2)
HOOK("eglClientWaitSyncKHR", 0x806a96e0, 5)
HOOK("eglGetSyncAttribKHR", 0x806ad2d2, 4)
HOOK("eglCreateSharedImageNOK", 0x806ac550, 3)
HOOK("eglDestroySharedImageNOK", 0x806aca3a, 2)
HOOK("eglQueryImageNOK", 0x806ad7f4, 4)
HOOK("GLES1 GetProcAddress (eglGetProcAddress back end)", 0x80688bb0, 1)
/* Garmin-side helpers (first argument is the Garmin gfx context) */
HOOK("GFX_present", 0x8096518e, 1)
HOOK("GLES1_GetCurrentContext", 0x80986ec8, 0)
HOOK("GLES1_SetError", 0x80986efe, 2)
HOOK("GLES1_FlushBuffers_cb", 0x80688238, 5)
```
