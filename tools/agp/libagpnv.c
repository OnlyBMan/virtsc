/*
 * libagpnv - give renderd the two GL entry points it needs, over /dev/agpgart.
 *
 * renderd gets its Thunderstorm frame buffers from glXAllocateMemoryNV() and
 * asks Mesa for their aperture offset with glXGetAGPOffsetMESA(). Both are
 * implemented in Mesa only on the DRI path (the Radeon driver's GART texture
 * heap), so on an indirect-GLX stack the first returns NULL and renderd panics
 * "Could not allocate agp memory".
 *
 * Nothing about that requires a GPU. The memory is used as a plain pixel
 * buffer - renderd renders with GL and glReadPixels()es into it - and the only
 * thing the card needs is an aperture-relative offset to be told about via
 * VIDIOC_S_AGP. /dev/agpgart can supply exactly that, and now does.
 *
 * So: LD_PRELOAD this ahead of libGL. Everything else still goes to the real
 * libGL; only these three symbols are replaced.
 *
 *   gcc -O2 -fPIC -shared -o /usr/local/lib/libagpnv.so libagpnv.c
 *   LD_PRELOAD=/usr/local/lib/libagpnv.so startx
 *
 * See notes/renderd-needs-dri.md.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/agpio.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>

typedef int GLsizei;
typedef float GLfloat;
typedef unsigned int GLuint;
typedef void GLvoid;

#define PAGE_SZ   4096
#define MAX_BLOCKS 64

struct block {
	void     *ptr;
	u_int32_t  offset;   /* byte offset into the aperture */
	u_int32_t  bytes;
	int       key;
};

static int          agp_fd = -1;
static u_int32_t     aper_base, aper_size;
static u_int32_t     next_offset;
static struct block blocks[MAX_BLOCKS];
static int          nblocks;
static int          verbose;

static void agpnv_log(const char *fmt, ...)
{
	va_list ap;
	if (!verbose)
		return;
	va_start(ap, fmt);
	fprintf(stderr, "agpnv: ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fflush(stderr);
}

static int agpnv_open(void)
{
	agp_info info;

	if (agp_fd >= 0)
		return 0;

	verbose = getenv("AGPNV_VERBOSE") != NULL;

	agp_fd = open("/dev/agpgart", O_RDWR);
	if (agp_fd < 0) {
		agpnv_log("open /dev/agpgart: %s\n", strerror(errno));
		return -1;
	}
	if (ioctl(agp_fd, AGPIOC_INFO, &info) < 0) {
		agpnv_log("AGPIOC_INFO: %s\n", strerror(errno));
		close(agp_fd);
		agp_fd = -1;
		return -1;
	}
	/*
	 * ACQUIRE may legitimately fail if something else holds the GART; that
	 * is fatal for us, since BIND needs it.
	 */
	if (ioctl(agp_fd, AGPIOC_ACQUIRE, 0) < 0) {
		agpnv_log("AGPIOC_ACQUIRE: %s\n", strerror(errno));
		close(agp_fd);
		agp_fd = -1;
		return -1;
	}

	aper_base = (u_int32_t)info.aper_base;
	aper_size = (u_int32_t)info.aper_size * 1024 * 1024;
	next_offset = 0;
	agpnv_log("aperture 0x%08x size %u MB\n", aper_base, (unsigned)info.aper_size);
	return 0;
}

void *glXAllocateMemoryNV(GLsizei size, GLfloat readfreq, GLfloat writefreq,
                          GLfloat priority)
{
	agp_allocate alloc;
	agp_bind bind;
	struct block *b;
	u_int32_t bytes;
	void *p;

	if (size <= 0 || nblocks >= MAX_BLOCKS)
		return NULL;
	if (agpnv_open() < 0)
		return NULL;

	bytes = ((u_int32_t)size + PAGE_SZ - 1) & ~(PAGE_SZ - 1);
	if (next_offset + bytes > aper_size) {
		agpnv_log("aperture full: want %u, have %u\n",
		          bytes, aper_size - next_offset);
		return NULL;
	}

	memset(&alloc, 0, sizeof alloc);
	alloc.type = 0;
	alloc.pg_count = bytes / PAGE_SZ;
	if (ioctl(agp_fd, AGPIOC_ALLOCATE, &alloc) < 0) {
		agpnv_log("AGPIOC_ALLOCATE %u pages: %s\n",
		          (unsigned)alloc.pg_count, strerror(errno));
		return NULL;
	}

	bind.key = alloc.key;
	bind.pg_start = next_offset / PAGE_SZ;
	if (ioctl(agp_fd, AGPIOC_BIND, &bind) < 0) {
		agpnv_log("AGPIOC_BIND: %s\n", strerror(errno));
		ioctl(agp_fd, AGPIOC_DEALLOCATE, &alloc.key);
		return NULL;
	}

	p = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, agp_fd,
	         (off_t)next_offset);
	if (p == MAP_FAILED) {
		agpnv_log("mmap at 0x%x: %s\n", next_offset, strerror(errno));
		return NULL;
	}

	b = &blocks[nblocks++];
	b->ptr = p;
	b->offset = next_offset;
	b->bytes = bytes;
	b->key = alloc.key;
	next_offset += bytes;

	agpnv_log("alloc %d -> %p, aperture offset 0x%08x (phys 0x%08x)\n",
	          size, p, b->offset, aper_base + b->offset);
	return p;
}

void glXFreeMemoryNV(GLvoid *pointer)
{
	int i;

	for (i = 0; i < nblocks; i++) {
		if (blocks[i].ptr == pointer) {
			/*
			 * Unmap but do not reclaim the aperture range: renderd
			 * allocates once at start-up, and a bump allocator that
			 * never frees cannot fragment or hand back a range the
			 * card still has a SET_AGP pointing into.
			 */
			munmap(blocks[i].ptr, blocks[i].bytes);
			blocks[i].ptr = NULL;
			agpnv_log("free %p\n", pointer);
			return;
		}
	}
}

GLuint glXGetAGPOffsetMESA(const GLvoid *pointer)
{
	int i;

	for (i = 0; i < nblocks; i++) {
		const char *base = (const char *)blocks[i].ptr;
		if (base && (const char *)pointer >= base &&
		    (const char *)pointer < base + blocks[i].bytes) {
			GLuint off = blocks[i].offset +
			             (GLuint)((const char *)pointer - base);
			agpnv_log("offset of %p = 0x%08x\n", pointer, off);
			return off;
		}
	}
	agpnv_log("offset of %p: NOT FOUND\n", pointer);
	return ~0u;
}
