/*
 * libglfix - supply the two glPixelStorei pnames renderd asks for and this
 * Mesa does not have.
 *
 * renderd raises GL_INVALID_ENUM about 25000 times a minute, all of it from
 * glPixelStorei, and all of it from exactly two pnames:
 *
 *   GL_PACK_INVERT_MESA          (0x8758, GL_MESA_pack_invert)
 *   GL_UNPACK_CLIENT_STORAGE_APPLE (0x85B2, GL_APPLE_client_storage)
 *
 * Both are extensions the Radeon DRI driver had and software Mesa 5.0.1 over
 * indirect GLX does not. Their fates are different:
 *
 *   - CLIENT_STORAGE_APPLE is a *hint* - "keep using my memory for this
 *     texture rather than copying it". Ignoring it is already the specified
 *     behaviour, so swallowing it silently is exactly right.
 *
 *   - PACK_INVERT_MESA is *semantics*. With it set, glReadPixels writes rows
 *     top-to-bottom instead of GL's usual bottom-to-top. renderd sets it
 *     around every readback because a video frame is top-down. Rejected, the
 *     readback comes back bottom-up and the frame handed to the Thunderstorm
 *     is vertically flipped - silently, because renderd never checks. So this
 *     one is emulated rather than swallowed: reverse the row order after the
 *     real glReadPixels returns.
 *
 * LD_PRELOAD ahead of libGL. See notes/gl-invalid-enumerant.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <GL/gl.h>

#ifndef GL_PACK_INVERT_MESA
#define GL_PACK_INVERT_MESA            0x8758
#endif
#ifndef GL_UNPACK_CLIENT_STORAGE_APPLE
#define GL_UNPACK_CLIENT_STORAGE_APPLE 0x85B2
#endif
#ifndef GL_YCBCR_MESA
#define GL_YCBCR_MESA                  0x8757
#endif
#ifndef GL_UNSIGNED_SHORT_8_8_MESA
#define GL_UNSIGNED_SHORT_8_8_MESA     0x85BA  /* == GL_UNSIGNED_SHORT_8_8_APPLE */
#define GL_UNSIGNED_SHORT_8_8_REV_MESA 0x85BB
#endif

static void *real_lib;
static int   verbose;
static FILE *logf;

/*
 * Log to our own file. istard does not give renderd a stderr we can find -
 * it is neither the startx log nor syslog - so relying on it wastes runs.
 */
static FILE *glfix_log(void)
{
	if (!logf) {
		const char *path = getenv("GLFIX_LOG");
		logf = path ? fopen(path, "w") : stderr;
		if (!logf) logf = stderr;
	}
	return logf;
}

static void *resolve(const char *name)
{
	if (!real_lib) {
		const char *p = getenv("GLFIX_REAL");
		if (!p) p = "/usr/X11R6/lib/libGL.so.1";
		real_lib = dlopen(p, RTLD_LAZY);
		verbose = getenv("GLFIX_VERBOSE") != NULL || getenv("GLFIX_LOG") != NULL;
		if (!real_lib) {
			fprintf(stderr, "glfix: cannot dlopen %s: %s\n", p, dlerror());
			return 0;
		}
	}
	return dlsym(real_lib, name);
}

/*
 * Only GL_PACK_INVERT_MESA is tracked here, because only we implement it.
 *
 * The row stride is deliberately NOT tracked: it is asked of GL at readback
 * time instead. Shadowing it looked cheaper - one fewer round trip per frame -
 * and was wrong, because glPixelStore is not the only thing that can change
 * pack state (glPopClientAttrib restores it wholesale), and a stale row length
 * makes the flip stride wrong. That is not a cosmetic error: the flip would
 * then read and write past the end of whatever buffer GL filled. It showed up
 * as a frame full of noise, and it is exactly the kind of bug worth paying two
 * glGetIntegerv calls a frame to make impossible.
 */
static GLint pack_invert;
/*
 * GLFIX_SKIP_READ: do not actually read the framebuffer back. renderd pulls
 * 720x480x4 - 1.38 MB - out of the X server every frame through indirect GLX,
 * which is a synchronous round trip and a copy of that much data across the
 * socket. This removes it and leaves whatever was in the buffer, so the
 * difference is the whole readback. Measurement only: the card then shows a
 * frozen picture.
 */
static int skip_read = -1;


static int intercept(GLenum pname, GLint param)
{
	switch (pname) {
	case GL_PACK_INVERT_MESA:
		pack_invert = (param != 0);
		return 1;                 /* swallowed: emulated in glReadPixels */
	case GL_UNPACK_CLIENT_STORAGE_APPLE:
		return 1;                 /* swallowed: a hint, ignoring it is legal */
	}
	return 0;
}

void glPixelStorei(GLenum pname, GLint param)
{
	static void (*real)(GLenum, GLint);

	if (intercept(pname, param))
		return;
	if (!real) real = (void (*)(GLenum, GLint))resolve("glPixelStorei");
	if (real) real(pname, param);
}

void glPixelStoref(GLenum pname, GLfloat param)
{
	static void (*real)(GLenum, GLfloat);

	if (intercept(pname, (GLint)param))
		return;
	if (!real) real = (void (*)(GLenum, GLfloat))resolve("glPixelStoref");
	if (real) real(pname, param);
}

/* Bytes per pixel, for the combinations that can actually reach us. */
static int pixel_bytes(GLenum format, GLenum type)
{
	int comps;

	switch (type) {
	case GL_UNSIGNED_INT_8_8_8_8:
	case GL_UNSIGNED_INT_8_8_8_8_REV:
	case GL_UNSIGNED_INT_10_10_10_2:
	case GL_UNSIGNED_INT_2_10_10_10_REV:
		return 4;
	case GL_UNSIGNED_SHORT_5_6_5:
	case GL_UNSIGNED_SHORT_5_6_5_REV:
	case GL_UNSIGNED_SHORT_4_4_4_4:
	case GL_UNSIGNED_SHORT_4_4_4_4_REV:
	case GL_UNSIGNED_SHORT_5_5_5_1:
	case GL_UNSIGNED_SHORT_1_5_5_5_REV:
		return 2;
	}

	switch (format) {
	case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA:
	case GL_LUMINANCE: case GL_STENCIL_INDEX: case GL_DEPTH_COMPONENT:
		comps = 1; break;
	case GL_LUMINANCE_ALPHA:
		comps = 2; break;
	case GL_RGB: case GL_BGR:
		comps = 3; break;
	case GL_RGBA: case GL_BGRA:
		comps = 4; break;
	default:
		return 0;               /* unknown: do not guess, do not flip */
	}

	switch (type) {
	case GL_BYTE: case GL_UNSIGNED_BYTE:   return comps;
	case GL_SHORT: case GL_UNSIGNED_SHORT: return comps * 2;
	case GL_INT: case GL_UNSIGNED_INT:
	case GL_FLOAT:                         return comps * 4;
	}
	return 0;
}

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid *pixels)
{
	static void (*real)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *);
	static unsigned char *rowbuf;
	static long rowbuf_size;
	long row_pixels, stride, bpp;
	unsigned char *p = pixels;
	GLsizei i;

	if (!real)
		real = (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *))
		       resolve("glReadPixels");
	if (!real)
		return;
	if (skip_read < 0)
		skip_read = getenv("GLFIX_SKIP_READ") != NULL;
	if (!skip_read)
		real(x, y, width, height, format, type, pixels);

	if (!pack_invert || !pixels || height < 2)
		return;
	/* GLFIX_NOINVERT: swallow the pname but skip the emulation, to separate
	 * "the errors were costing us" from "the flip is correct". */
	{
		static int off = -1;
		if (off < 0) off = getenv("GLFIX_NOINVERT") != NULL;
		if (off) return;
	}

	bpp = pixel_bytes(format, type);
	if (bpp <= 0) {
		static int warned;
		if (!warned++)
			fprintf(stderr, "glfix: cannot invert format 0x%x type 0x%x\n",
			        (unsigned)format, (unsigned)type);
		return;
	}

	/* Ask GL what it actually just used. Authoritative, unlike a shadow copy. */
	{
		static void (*real_get)(GLenum, GLint *);
		GLint row_length = 0, alignment = 4;

		if (!real_get)
			real_get = (void (*)(GLenum, GLint *))resolve("glGetIntegerv");
		if (!real_get)
			return;
		real_get(GL_PACK_ROW_LENGTH, &row_length);
		real_get(GL_PACK_ALIGNMENT, &alignment);

		row_pixels = row_length > 0 ? row_length : width;
		if (row_pixels < width)
			row_pixels = width;
		stride = row_pixels * bpp;
		if (alignment > 1)
			stride = (stride + alignment - 1) & ~(long)(alignment - 1);
	}

	if (stride > rowbuf_size) {
		unsigned char *n = realloc(rowbuf, stride);
		if (!n) return;
		rowbuf = n;
		rowbuf_size = stride;
	}

	/* Reverse the row order in place: that is all GL_PACK_INVERT_MESA does. */
	for (i = 0; i < height / 2; i++) {
		unsigned char *a = p + (long)i * stride;
		unsigned char *b = p + (long)(height - 1 - i) * stride;
		memcpy(rowbuf, a, stride);
		memcpy(a, b, stride);
		memcpy(b, rowbuf, stride);
	}
	if (verbose) {
		static long n;
		if (n < 4 || (n % 900) == 0) {
			FILE *f = glfix_log();
			fprintf(f, "glfix: readback-invert %dx%d stride %ld  (call %ld)\n",
			        (int)width, (int)height, stride, n);
			fflush(f);
		}
		n++;
	}
}


/* ------------------------------------------------------------- YCbCr 4:2:2
 *
 * renderd uploads the incoming video as a 4:2:2 texture:
 *
 *   glPixelStorei(GL_UNPACK_ROW_LENGTH, 2048)
 *   glTexImage2D(..., GL_YCBCR_MESA, GL_YCBCR_MESA, GL_UNSIGNED_SHORT_8_8_APPLE, p)
 *
 * which is GL_MESA_ycbcr_texture. XFree86 4.3's software Mesa accepts it and
 * then segfaults the X server, intermittently, and only on this path - the
 * NullFrameSource configuration never triggers it because it has no video
 * layer to texture. With indirect GLX the client packs the image and the
 * server unpacks it, and a 4:2:2 format at a 2048 row length is exactly the
 * sort of thing a 2003 client and server disagree about the byte count for; a
 * server that expects more than it was sent reads off the end, which is a
 * crash whose timing depends on heap layout.
 *
 * So do the colour conversion here and hand the server plain GL_RGB, which it
 * has no trouble with. See notes/gl-ycbcr-and-the-x-crash.md.
 *
 * Byte order: GL_UNSIGNED_SHORT_8_8_APPLE is 'yuvs', Y0 Cb Y1 Cr in memory;
 * the _REV form is '2vuy', Cb Y0 Cr Y1. ThunderstormHost.h describes the same
 * data as "Cb-Y0-Cr-Y1" packed in an unsigned long, but it says that from the
 * card's big-endian point of view, which is the same bytes. GLFIX_YCBCR_REV
 * forces the other interpretation if a picture ever comes out with luma and
 * chroma swapped.
 */
static int ycbcr_disabled = -1;
static int grey = -1;

/*
 * Two measurement switches, for separating the cost of converting the video
 * texture from the cost of uploading it. GL_LUMINANCE was tried for this and
 * was useless - it is *slower* than GL_RGB in this Mesa despite being a third
 * of the bytes - so instead remove the work outright and see what comes back:
 *
 *   GLFIX_SKIP_CONVERT  convert once, then reuse the buffer. Upload unchanged,
 *                       so the difference is the client-side conversion.
 *   GLFIX_SKIP_TEX      after the first two uploads, do not upload at all. The
 *                       video layer freezes, the geometry work is untouched,
 *                       and the difference is the whole upload - an upper
 *                       bound on anything an optimisation here could recover.
 */
static int skip_convert = -1;
static int skip_tex = -1;


static int clamp255(int v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* BT.601 studio swing. */
static void ycbcr_to_rgb(int y, int cb, int cr, unsigned char *out)
{
	int c = y - 16, d = cb - 128, e = cr - 128;

	out[0] = clamp255((298 * c + 409 * e + 128) >> 8);
	out[1] = clamp255((298 * c - 100 * d - 208 * e + 128) >> 8);
	out[2] = clamp255((298 * c + 516 * d + 128) >> 8);
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels)
{
	static void (*real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
	                    GLenum, GLenum, const GLvoid *);
	static void (*real_store)(GLenum, GLint);
	static void (*real_get)(GLenum, GLint *);
	static unsigned char *rgb;
	static long rgb_size;
	static int converted_once;
	static long uploads;
	const unsigned char *src = pixels;
	GLint row_length = 0, alignment = 4, skip_rows = 0, skip_pixels = 0;
	long src_stride, need;
	int rev, x, y;

	if (!real)
		real = (void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
		                 GLenum, GLenum, const GLvoid *))resolve("glTexImage2D");
	if (!real)
		return;

	if (ycbcr_disabled < 0)
		ycbcr_disabled = getenv("GLFIX_NO_YCBCR") != NULL;

	if (format != GL_YCBCR_MESA || !pixels || ycbcr_disabled ||
	    width <= 0 || height <= 0 || (width & 1)) {
		real(target, level, internalformat, width, height, border,
		     format, type, pixels);
		return;
	}

	if (!real_store)
		real_store = (void (*)(GLenum, GLint))resolve("glPixelStorei");
	if (!real_get)
		real_get = (void (*)(GLenum, GLint *))resolve("glGetIntegerv");
	if (!real_store || !real_get) {
		real(target, level, internalformat, width, height, border,
		     format, type, pixels);
		return;
	}

	/* Ask GL, rather than shadowing - same lesson as the readback flip. */
	real_get(GL_UNPACK_ROW_LENGTH, &row_length);
	real_get(GL_UNPACK_ALIGNMENT, &alignment);
	real_get(GL_UNPACK_SKIP_ROWS, &skip_rows);
	real_get(GL_UNPACK_SKIP_PIXELS, &skip_pixels);

	src_stride = (row_length > 0 ? row_length : width) * 2;
	if (alignment > 1)
		src_stride = (src_stride + alignment - 1) & ~(long)(alignment - 1);
	src += (long)skip_rows * src_stride + (long)skip_pixels * 2;

	need = (long)width * height * 3;   /* 3 covers the grey case too */
	if (need > rgb_size) {
		unsigned char *n = realloc(rgb, need);
		if (!n) {
			real(target, level, internalformat, width, height, border,
			     format, type, pixels);
			return;
		}
		rgb = n;
		rgb_size = need;
	}

	rev = (type == GL_UNSIGNED_SHORT_8_8_REV_MESA) ^
	      (getenv("GLFIX_YCBCR_REV") != NULL);

	/*
	 * GLFIX_YCBCR_GREY: upload luma only, as GL_LUMINANCE. One byte per
	 * pixel against three, so it is a third of the wire and server cost of
	 * the RGB path and half of the 4:2:2 the application asked for. Purely
	 * a measurement - the video comes out grey - for answering how much of
	 * the driven scene's frame time is the per-frame video texture upload.
	 */
	if (grey < 0)
		grey = getenv("GLFIX_YCBCR_GREY") != NULL;
	if (skip_convert < 0)
		skip_convert = getenv("GLFIX_SKIP_CONVERT") != NULL;
	if (skip_tex < 0)
		skip_tex = getenv("GLFIX_SKIP_TEX") != NULL;

	if (skip_convert && converted_once)
		goto upload;

	for (y = 0; y < height; y++) {
		const unsigned char *sr = src + (long)y * src_stride;
		unsigned char *dr = rgb + (long)y * width * 3;

		for (x = 0; x < width; x += 2) {
			int y0, y1, cb, cr;

			if (rev) {                 /* '2vuy': Cb Y0 Cr Y1 */
				cb = sr[0]; y0 = sr[1]; cr = sr[2]; y1 = sr[3];
			} else {                   /* 'yuvs': Y0 Cb Y1 Cr */
				y0 = sr[0]; cb = sr[1]; y1 = sr[2]; cr = sr[3];
			}
			if (grey) {
				dr[0] = clamp255((298 * (y0 - 16) + 128) >> 8);
				dr[1] = clamp255((298 * (y1 - 16) + 128) >> 8);
				dr += 2;
			} else {
				ycbcr_to_rgb(y0, cb, cr, dr);
				ycbcr_to_rgb(y1, cb, cr, dr + 3);
				dr += 6;
			}
			sr += 4;
		}
	}

	converted_once = 1;

upload:
	if (skip_tex && uploads >= 2) {
		uploads++;
		return;
	}
	uploads++;

	/* Our buffer is tightly packed; the app's unpack state does not apply. */
	real_store(GL_UNPACK_ROW_LENGTH, 0);
	real_store(GL_UNPACK_ALIGNMENT, 1);
	real_store(GL_UNPACK_SKIP_ROWS, 0);
	real_store(GL_UNPACK_SKIP_PIXELS, 0);

	real(target, level, grey ? GL_LUMINANCE : GL_RGB, width, height, border,
	     grey ? GL_LUMINANCE : GL_RGB, GL_UNSIGNED_BYTE, rgb);

	real_store(GL_UNPACK_ROW_LENGTH, row_length);
	real_store(GL_UNPACK_ALIGNMENT, alignment);
	real_store(GL_UNPACK_SKIP_ROWS, skip_rows);
	real_store(GL_UNPACK_SKIP_PIXELS, skip_pixels);

	if (verbose) {
		static long n;
		/* The first few are what matter - we want the geometry, not a trace. */
		if (n < 8 || (n % 900) == 0) {
			FILE *f = glfix_log();
			fprintf(f, "glfix: ycbcr->rgb %dx%d row_length %d align %d "
			        "skip %d,%d src_stride %ld  (call %ld)\n",
			        (int)width, (int)height, (int)row_length, (int)alignment,
			        (int)skip_pixels, (int)skip_rows, src_stride, n);
			fflush(f);
		}
		n++;
	}
}

/* ------------------------------------------------------------- ablations
 *
 * Three switches that make the application ask software Mesa for less work,
 * so that the gap between renderd and this bench's own glxgears baseline can
 * be attributed rather than guessed at. None of them is a fix: each one
 * changes what is drawn, and two of them visibly.
 *
 * The question they exist to answer is whether the gap is one expensive path
 * or a broad limit, because that decides whether accelerating this is a
 * matter of fixing a path or of building a GPU (notes/todo-by-effort.md #4).
 *
 *   GLFIX_NEAREST        force GL_NEAREST on every texture filter. Bilinear
 *                        minification through software Mesa touches four
 *                        texels and interpolates per fragment; nearest
 *                        touches one. If the scene is texture-filter bound
 *                        this is where it shows.
 *   GLFIX_REPLACE        force GL_REPLACE as the texture environment. The
 *                        app uses GL_MODULATE, which multiplies the texel by
 *                        the fragment colour; REPLACE skips that per
 *                        fragment. Colours come out wrong, which is fine.
 *   GLFIX_VIEWPORT_DIV=n force the viewport to 1/n in each axis, so roughly
 *                        1/n^2 of the fragments. The readback in
 *                        glReadPixels is deliberately left at full size, so
 *                        this isolates fragment cost from transport cost -
 *                        GLFIX_SKIP_READ is the other half of that pair.
 */

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
	static void (*real)(GLenum, GLenum, GLint);
	static int nearest = -1;

	if (nearest < 0)
		nearest = getenv("GLFIX_NEAREST") != NULL;
	if (nearest && (pname == GL_TEXTURE_MIN_FILTER ||
			pname == GL_TEXTURE_MAG_FILTER))
		param = GL_NEAREST;
	if (!real) real = (void (*)(GLenum, GLenum, GLint))resolve("glTexParameteri");
	if (real) real(target, pname, param);
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
	static void (*real)(GLenum, GLenum, GLfloat);
	static int nearest = -1;

	if (nearest < 0)
		nearest = getenv("GLFIX_NEAREST") != NULL;
	if (nearest && (pname == GL_TEXTURE_MIN_FILTER ||
			pname == GL_TEXTURE_MAG_FILTER))
		param = (GLfloat)GL_NEAREST;
	if (!real) real = (void (*)(GLenum, GLenum, GLfloat))resolve("glTexParameterf");
	if (real) real(target, pname, param);
}

void glTexEnvi(GLenum target, GLenum pname, GLint param)
{
	static void (*real)(GLenum, GLenum, GLint);
	static int replace = -1;

	if (replace < 0)
		replace = getenv("GLFIX_REPLACE") != NULL;
	if (replace && pname == GL_TEXTURE_ENV_MODE)
		param = GL_REPLACE;
	if (!real) real = (void (*)(GLenum, GLenum, GLint))resolve("glTexEnvi");
	if (real) real(target, pname, param);
}

void glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{
	static void (*real)(GLenum, GLenum, GLfloat);
	static int replace = -1;

	if (replace < 0)
		replace = getenv("GLFIX_REPLACE") != NULL;
	if (replace && pname == GL_TEXTURE_ENV_MODE)
		param = (GLfloat)GL_REPLACE;
	if (!real) real = (void (*)(GLenum, GLenum, GLfloat))resolve("glTexEnvf");
	if (real) real(target, pname, param);
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	static void (*real)(GLint, GLint, GLsizei, GLsizei);
	static int div = -1;

	if (div < 0) {
		const char *p = getenv("GLFIX_VIEWPORT_DIV");

		div = p ? atoi(p) : 1;
		if (div < 1)
			div = 1;
	}
	if (div > 1) {
		width /= div;
		height /= div;
		if (width < 1) width = 1;
		if (height < 1) height = 1;
	}
	if (!real) real = (void (*)(GLint, GLint, GLsizei, GLsizei))resolve("glViewport");
	if (real) real(x, y, width, height);
}
