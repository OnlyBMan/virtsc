/*
 * is1gl_ring.h - the guest's copy of the is1gl ring ABI.
 *
 * Kept in step with hw/misc/is1gl.c in the QEMU tree BY HAND until Phase 2,
 * when tools/is1gl/protocol.py becomes the single source and generates both
 * sides. Until then, a change here is a change there.
 *
 * See notes/gl-acceleration-plan.md and notes/is1gl-phase0.md.
 */
#ifndef IS1GL_RING_H
#define IS1GL_RING_H

/* MMIO register file, reached by mmap()ing /dev/mem at the device's page. */
#define IS1GL_MMIO_BASE     0xfed10000
#define IS1GL_MAGIC         0x49533147   /* 'IS1G' */

#define IS1GL_REG_MAGIC     0x00
#define IS1GL_REG_VERSION   0x04
#define IS1GL_REG_RING_LO   0x08
#define IS1GL_REG_RING_HI   0x0c
#define IS1GL_REG_RING_SZ   0x10   /* writing this arms the ring */
#define IS1GL_REG_DOORBELL  0x14
#define IS1GL_REG_DONE_SEQ  0x18   /* debug only - poll the header instead */
#define IS1GL_REG_BELLS     0x1c
#define IS1GL_REG_STATUS    0x20
/*
 * The AGP aperture, so the host can map it once instead of chasing
 * individual readback destinations. renderd cycles through 28 frame buffers
 * 2 MB apart, which is what broke a fixed-size cache of them.
 */
#define IS1GL_REG_APER_LO   0x24
#define IS1GL_REG_APER_HI   0x28
#define IS1GL_REG_APER_SZ   0x2c
#define IS1GL_REG_CAPS      0x30   /* optional operations; 0 on older hosts */
#define IS1GL_CAP_QT_PNG    0x01

/*
 * The ring: a 4 KiB header followed by `size` bytes of records.
 *
 *      u32 opcode; u32 length; payload...
 *
 * `length` is the whole record including the 8-byte head, rounded up to 8.
 * A record never straddles the end of the data area - write IS1GL_OP_WRAP
 * and restart at 0 instead.
 */
#define IS1GL_RING_MAGIC    0x49533152   /* 'IS1R' */
#define IS1GL_RING_HDR      4096

#define RH_MAGIC      0x00
#define RH_VERSION    0x04
#define RH_SIZE       0x08
#define RH_HEAD       0x10   /* guest writes */
#define RH_TAIL       0x14   /* host writes  */
#define RH_DONE_SEQ   0x18   /* host writes - this is what we poll */
#define RH_GUEST_SEQ  0x1c   /* guest writes */
#define RH_ERRORS     0x20   /* host writes  */

#define IS1GL_OP_NOP        0
#define IS1GL_OP_WRAP       1
#define IS1GL_OP_FENCE      2    /* payload: u32 seq */

#endif
