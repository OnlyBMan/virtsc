#!/usr/bin/env python3
"""Generate a logging libGL shim for the IntelliStar guest.

Reads the *guest's own* GL/GLX headers (Mesa 5.0.1 / XFree86 4.3) and emits a C
file implementing every declared entry point as:

    count it  ->  optionally record the enum arguments  ->  forward to the real libGL

Generating from the guest headers rather than a modern copy matters: it is the
authoritative list of what this libGL actually exports, and it gives us exact
signatures, so the wrappers are ABI-identical to what the app already links against.

Usage:
    gen_glshim.py gl.h.guest glx.h.guest > glshim.c
"""
import re
import sys

# ---------------------------------------------------------------- header parsing

def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def split_params(s):
    """Split a parameter list on top-level commas."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [p.strip() for p in out]


def parse_param(p, idx):
    """Return (decl, name) for one parameter, inventing a name if unnamed."""
    p = p.strip()
    if p in ("void", ""):
        return None
    # move a trailing array suffix out of the way: "GLdouble m[16]"
    arr = ""
    m = re.search(r"(\[[^\]]*\])\s*$", p)
    if m:
        arr = m.group(1)
        p = p[: m.start()].strip()
    m = re.match(r"^(.*?)([A-Za-z_]\w*)$", p)
    if m and m.group(1).strip() not in ("", "const", "unsigned", "signed"):
        base, name = m.group(1).strip(), m.group(2)
    else:
        base, name = p, "a%d" % idx
    return ("%s %s%s" % (base, name, arr), name)


def parse_decls(text, patterns):
    text = strip_comments(text)
    # Drop preprocessor lines BEFORE collapsing whitespace. Otherwise the return-type
    # pattern happily runs through the Windows "#define GLAPI __declspec(dllexport)"
    # block and swallows it into the first declaration.
    text = "\n".join(l for l in text.split("\n") if not l.lstrip().startswith("#"))
    text = re.sub(r"\s+", " ", text)
    funcs = []
    seen = set()
    for pat in patterns:
        for m in re.finditer(pat, text):
            ret, name, params = m.group("ret"), m.group("name"), m.group("params")
            if name in seen:
                continue
            seen.add(name)
            plist = []
            for i, p in enumerate(split_params(params)):
                pp = parse_param(p, i)
                if pp:
                    plist.append(pp)
            funcs.append({"ret": ret.strip(), "name": name, "params": plist})
    return funcs


# Return types are restricted to plain identifiers/pointers so they cannot run away.
GL_PAT = (r"GLAPI\s+(?P<ret>(?:const\s+)?[A-Za-z_]\w*(?:\s*\*)*)\s+GLAPIENTRY\s+"
          r"(?P<name>gl\w+)\s*\((?P<params>[^)]*)\)\s*;")
GLX_PAT = (r"extern\s+(?P<ret>(?:const\s+)?[A-Za-z_]\w*(?:\s*\*)*)\s+"
           r"(?P<name>glX\w+)\s*\((?P<params>[^)]*)\)\s*;")


def parse_enums(text):
    """value -> [names]. GL reuses values, so keep every name and let the caller
    pick: the first for general display, the _BIT one for mask decoding."""
    vals = {}
    for m in re.finditer(r"^#define\s+(GL_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\s*$",
                         text, flags=re.M):
        name, v = m.group(1), m.group(2)
        try:
            iv = int(v, 0)
        except ValueError:
            continue
        vals.setdefault(iv, []).append(name)
    return vals


# ------------------------------------------------- which arguments are worth noting
#
# Recorded by PARAMETER INDEX, not by name: the header's parameter names are not
# guaranteed and indices are. These are the calls whose enum arguments decide the
# virgl-vs-custom-protocol question -- texenv modes, lighting, fog, blending,
# texture formats. See notes/virtio-gpu-feasibility.md.

NOTE_ARGS = {
    "glEnable": [0], "glDisable": [0], "glIsEnabled": [0],
    "glEnableClientState": [0], "glDisableClientState": [0],
    "glBegin": [0],
    "glShadeModel": [0], "glMatrixMode": [0], "glFrontFace": [0], "glCullFace": [0],
    "glBlendFunc": [0, 1], "glBlendEquation": [0],
    "glDepthFunc": [0], "glAlphaFunc": [0],
    "glStencilFunc": [0], "glStencilOp": [0, 1, 2],
    "glPolygonMode": [0, 1],
    "glTexEnvi": [0, 1, 2], "glTexEnvf": [0, 1, 2],
    "glTexEnviv": [0, 1], "glTexEnvfv": [0, 1],
    "glTexParameteri": [0, 1, 2], "glTexParameterf": [0, 1, 2],
    "glTexParameteriv": [0, 1], "glTexParameterfv": [0, 1],
    "glTexImage1D": [2, 5, 6],
    "glTexImage2D": [2, 6, 7],
    "glTexSubImage2D": [6, 7],
    "glCopyTexImage2D": [2],
    "glBindTexture": [0],
    "glPixelStorei": [0, 1], "glPixelStoref": [0, 1],
    "glHint": [0, 1],
    "glFogi": [0, 1], "glFogf": [0, 1], "glFogiv": [0], "glFogfv": [0],
    "glLightModeli": [0, 1], "glLightModelf": [0, 1],
    "glLightModeliv": [0], "glLightModelfv": [0],
    "glLighti": [0, 1], "glLightf": [0, 1],
    "glLightiv": [0, 1], "glLightfv": [0, 1],
    "glMateriali": [0, 1], "glMaterialf": [0, 1],
    "glMaterialiv": [0, 1], "glMaterialfv": [0, 1],
    "glColorMaterial": [0, 1],
    "glDrawArrays": [0], "glDrawElements": [0, 2],
    "glVertexPointer": [1], "glTexCoordPointer": [1], "glColorPointer": [1],
    "glNormalPointer": [0], "glInterleavedArrays": [0],
    "glClear": [0], "glPushAttrib": [0],
    "glReadPixels": [4, 5], "glDrawPixels": [2, 3],
    "glGetString": [0],
    # State QUERIES matter as much as state setting: every glGet is a round trip for a
    # forwarding libGL, and renderd makes ~400 per frame. Recording the pname tells us
    # exactly which state a client-side cache must track.
    "glGetIntegerv": [0], "glGetFloatv": [0], "glGetDoublev": [0],
    "glGetBooleanv": [0], "glGetTexParameteriv": [0, 1], "glGetTexParameterfv": [0, 1],
    "glNewList": [1],
    "glActiveTextureARB": [0], "glClientActiveTextureARB": [0],
}

INT_LIKE = re.compile(r"\b(GLenum|GLint|GLuint|GLbitfield|GLsizei|GLboolean|GLshort|"
                      r"GLushort|GLbyte|GLubyte|GLfloat|GLdouble|GLclampf|GLclampd|int|"
                      r"unsigned int|long)\b")


def notable(fn):
    """Parameter indices to record, filtered to ones that actually exist and are scalar."""
    want = NOTE_ARGS.get(fn["name"])
    if not want:
        return []
    out = []
    for i in want:
        if i >= len(fn["params"]):
            continue
        decl = fn["params"][i][0]
        if "*" in decl or "[" in decl:
            continue
        if not INT_LIKE.search(decl):
            continue
        out.append(i)
    return out


# ------------------------------------------------------------------ code emission

PROLOGUE = r"""/* GENERATED by tools/glshim/gen_glshim.py -- do not edit by hand.
 *
 * Logging libGL shim for the IntelliStar guest (FreeBSD 4.8 / Mesa 5.0.1).
 *
 *   LD_PRELOAD=/usr/local/lib/libglshim.so <app>
 *
 * Counts every GL/GLX entry point and records the distinct enum-argument tuples
 * of the state-setting calls, then writes a report at exit or on SIGUSR1.
 *
 * Design constraints, see notes/threading-constraint.md:
 *   - NO I/O on the hot path. The guest is libc_r (N:1 threads); a blocking write
 *     inside a GL call would stall every thread in the process, and would add a
 *     scheduling yield point where the real driver had none. Everything is
 *     accumulated in memory and written once, at dump time.
 *   - No malloc on the hot path either: fixed-size tables.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/time.h>
#include <unistd.h>
#include <GL/gl.h>
#include <GL/glx.h>

static void *real_lib = 0;
static int   use_next = 0;   /* GLSHIM_CHAIN=1: resolve with RTLD_NEXT */
static int   shim_ready = 0;
static unsigned long dump_every = 0; /* GLSHIM_DUMP_EVERY swaps; 0 = only at exit */
static unsigned long missing_calls = 0;
static struct timeval t_first;

static void shim_dump(void);

static void shim_init(void)
{
    const char *path = getenv("GLSHIM_REAL");
    /*
     * GLSHIM_CHAIN=1 resolves through RTLD_NEXT instead of dlopen()ing libGL
     * directly, so the shim can sit in front of the other LD_PRELOAD shims
     * rather than around them. Without it, counting the application's calls
     * and letting libglfix do its YCbCr conversion are mutually exclusive:
     * dlopen("libGL") jumps straight past every preloaded library, and this
     * rtld's dlsym(handle, ...) does NOT search the handle's DT_NEEDED, so
     * pointing GLSHIM_REAL at libglfix.so finds only the six symbols
     * libglfix itself defines. Verified on the guest, both ways.
     *
     * Order it as:
     *   LD_PRELOAD=libglshim.so:libagpnv.so:libglfix.so GLSHIM_CHAIN=1
     */
    use_next = getenv("GLSHIM_CHAIN") != 0;
    if (!path) path = "/usr/X11R6/lib/libGL.so.1";
    if (!use_next) {
        real_lib = dlopen(path, RTLD_LAZY);
        if (!real_lib)
            fprintf(stderr, "glshim: cannot dlopen %s: %s\n", path, dlerror());
    }
    gettimeofday(&t_first, 0);
    {
        const char *n = getenv("GLSHIM_DUMP_EVERY");
        if (n) dump_every = strtoul(n, 0, 0);
    }
    atexit(shim_dump);
    shim_ready = 1;
}

static void *shim_resolve(const char *name)
{
    void *p;
    if (!shim_ready) shim_init();
    if (use_next) return dlsym(RTLD_NEXT, name);
    if (!real_lib) return 0;
    p = dlsym(real_lib, name);
    return p;
}

static void shim_missing(const char *name)
{
    static int warned = 0;
    missing_calls++;
    if (warned < 20) {
        fprintf(stderr, "glshim: %s not present in real libGL\n", name);
        warned++;
    }
}

"""

EPILOGUE = r"""
/* ------------------------------------------------------------------ reporting */

static const char *enum_name(long v)
{
    int lo = 0, hi = N_ENUMS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (enum_tab[mid].val == v) return enum_tab[mid].name;
        if (enum_tab[mid].val < v) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* glClear/glPushAttrib take a bitfield, not an enum; decode bit by bit. */
static int is_mask_fn(const char *name)
{
    return strcmp(name, "glClear") == 0 || strcmp(name, "glPushAttrib") == 0;
}

static void fmt_mask(char *buf, size_t n, long v)
{
    size_t used = 0;
    int b;
    buf[0] = 0;
    for (b = 0; b < 32; b++) {
        long bit = 1L << b;
        const char *e;
        if (!(v & bit)) continue;
        e = 0;
        { int k; for (k = 0; k < N_BITS; k++) if (bit_tab[k].val == bit) { e = bit_tab[k].name; break; } }
        if (used) used += snprintf(buf + used, n - used, "|");
        if (e) used += snprintf(buf + used, n - used, "%s", e);
        else   used += snprintf(buf + used, n - used, "0x%lx", bit);
        if (used >= n) return;
    }
    if (!used) snprintf(buf, n, "0");
}

static void fmt_arg(char *buf, size_t n, long v)
{
    const char *e = enum_name(v);
    /* Always show the number too: GL enum values are ambiguous (0x4000 is both
     * GL_LIGHT0 and GL_COLOR_BUFFER_BIT), so the name alone can mislead. */
    if (e) snprintf(buf, n, "%s(0x%lx)", e, v);
    else   snprintf(buf, n, "0x%lx", v);
}

static void shim_dump(void)
{
    const char *out = getenv("GLSHIM_LOG");
    FILE *f;
    int i, j;
    unsigned long total = 0;
    struct timeval now;
    double secs;

    if (!out) out = "/tmp/glshim.log";

    /* The shim is preloaded into EVERY process in the app's tree -- python, sh,
     * helpers -- most of which never touch GL. Without this they would all dump
     * and clobber each other's reports. Only processes that actually made GL
     * calls write anything. */
    for (i = 0; i < N_FUNCS; i++) total += call_count[i];
    if (total == 0) return;

    /* One file per process, for the same reason. */
    {
        char path[1024];
        snprintf(path, sizeof path, "%s.%d", out, (int)getpid());
        f = fopen(path, "w");
    }
    if (!f) return;

    gettimeofday(&now, 0);
    secs = (now.tv_sec - t_first.tv_sec) + (now.tv_usec - t_first.tv_usec) / 1e6;

    fprintf(f, "# glshim report\n");
    fprintf(f, "# pid %d\n", (int)getpid());
    fprintf(f, "# elapsed_seconds %.3f\n", secs);
    fprintf(f, "# total_gl_calls %lu\n", total);
    fprintf(f, "# entry_points_declared %d\n", N_FUNCS);
    {
        int used = 0;
        for (i = 0; i < N_FUNCS; i++) if (call_count[i]) used++;
        fprintf(f, "# entry_points_used %d\n", used);
    }
    fprintf(f, "# missing_from_real_libGL %lu\n", missing_calls);
    if (swapbuffers_count && secs > 0.0)
        fprintf(f, "# swapbuffers %lu  (%.2f per second)\n",
                swapbuffers_count, swapbuffers_count / secs);

    {
        unsigned long n = hist_pos < HIST_N ? hist_pos : HIST_N;
        unsigned long k;

        fprintf(f, "\n[last_calls]  most recent last, %lu of %lu total\n",
                n, hist_pos);
        for (k = 0; k < n; k++) {
            unsigned long idx = (hist_pos - n + k) % HIST_N;
            fprintf(f, "  %s\n", fn_name[hist[idx]]);
        }
    }

    if (check_errors > 0) {
        fprintf(f, "\n[gl_errors]  total %lu\n", errors_total);
        if (!errors_total)
            fprintf(f, "  (none)\n");
        for (i = 0; i < N_FUNCS; i++)
            if (error_count[i])
                fprintf(f, "  %-36s %8lu  first=%s(0x%lx)\n",
                        fn_name[i], error_count[i],
                        enum_name((long)first_error[i]), first_error[i]);
    }

    fprintf(f, "\n[calls]\n");
    for (i = 0; i < N_FUNCS; i++)
        if (call_count[i])
            fprintf(f, "%-36s %lu\n", fn_name[i], call_count[i]);

    fprintf(f, "\n[unused]\n");
    for (i = 0; i < N_FUNCS; i++)
        if (!call_count[i])
            fprintf(f, "%s\n", fn_name[i]);

    fprintf(f, "\n[state]\n");
    for (i = 0; i < NOTE_SLOTS; i++) {
        char b[3][64];
        if (!notes[i].n) continue;
        for (j = 0; j < 3; j++) {
            if (j == 0 && is_mask_fn(fn_name[notes[i].fn]))
                fmt_mask(b[j], sizeof b[j], notes[i].a[j]);
            else
                fmt_arg(b[j], sizeof b[j], notes[i].a[j]);
        }
        if (notes[i].na == 1)
            fprintf(f, "%s(%s) %lu\n", fn_name[notes[i].fn], b[0], notes[i].n);
        else if (notes[i].na == 2)
            fprintf(f, "%s(%s, %s) %lu\n", fn_name[notes[i].fn], b[0], b[1], notes[i].n);
        else
            fprintf(f, "%s(%s, %s, %s) %lu\n", fn_name[notes[i].fn],
                    b[0], b[1], b[2], notes[i].n);
    }
    fclose(f);
}

static void shim_sig(int s)
{
    (void)s;
    shim_dump();
}

/* Constructor: FreeBSD 4.8's rtld honours this for preloaded objects. */
static void shim_ctor(void) __attribute__((constructor));
static void shim_ctor(void)
{
    if (!shim_ready) shim_init();
    signal(SIGUSR1, shim_sig);
}
"""


ERROR_TRAP = r"""
/* ------------------------------------------------------------- error trap
 *
 * With GLSHIM_CHECK=1 every wrapper calls the real glGetError() after
 * forwarding, so an error is attributed to the call that actually raised it
 * rather than to whichever glGetError() the application happens to reach next.
 *
 * Two rules this has to respect:
 *   - glGetError() is illegal between glBegin() and glEnd(), so checking is
 *     suppressed in that window. glEnd() itself is checked after it closes it.
 *   - it consumes the error, so the application's own glGetError() will return
 *     GL_NO_ERROR and stop reporting. That is the point, but it does mean a
 *     checked run behaves differently from an unchecked one.
 *
 * It is also slow: on indirect GLX every glGetError() is a synchronous round
 * trip to the X server. Use it to find a bug, not to measure anything.
 */
/*
 * A ring of the most recent calls. Counters say what was called; when the X
 * server dies mid-frame what you need is the order. renderd exits through its
 * X I/O error handler, so atexit() still runs and the ring still gets dumped.
 */
#define HIST_N 256
static unsigned short hist[HIST_N];
static unsigned long hist_pos;

static int check_errors = -1;       /* -1 = not yet read from the environment */
static int in_begin = 0;
static unsigned long error_count[N_FUNCS];
static unsigned long first_error[N_FUNCS];
static unsigned long errors_total = 0;

static void shim_check(int idx)
{
    static GLenum (*p_glGetError)(void);
    GLenum e;

    if (check_errors < 0) {
        const char *v = getenv("GLSHIM_CHECK");
        check_errors = (v && *v && *v != '0') ? 1 : 0;
    }
    if (!check_errors || in_begin) return;
    if (!p_glGetError) {
        p_glGetError = (GLenum (*)(void))shim_resolve("glGetError");
        if (!p_glGetError) { check_errors = 0; return; }
    }
    e = p_glGetError();
    if (e != GL_NO_ERROR) {
        if (!error_count[idx]) first_error[idx] = e;
        error_count[idx]++;
        errors_total++;
    }
}"""


def emit(funcs, enums):
    names = [f["name"] for f in funcs]
    idx = {n: i for i, n in enumerate(names)}

    o = []
    o.append(PROLOGUE)

    o.append("\n#define N_FUNCS %d\n" % len(funcs))
    o.append("static unsigned long call_count[N_FUNCS];\n")
    o.append("static unsigned long swapbuffers_count;\n")
    o.append("static const char *fn_name[N_FUNCS] = {\n")
    for n in names:
        o.append('    "%s",\n' % n)
    o.append("};\n")
    o.append(ERROR_TRAP)

    # enum table, sorted for binary search
    items = sorted((v, ns[0]) for v, ns in enums.items())
    o.append("\ntypedef struct { long val; const char *name; } enum_ent;\n")
    o.append("#define N_ENUMS %d\n" % len(items))
    o.append("static const enum_ent enum_tab[N_ENUMS] = {\n")
    for v, n in items:
        o.append('    { %dL, "%s" },\n' % (v, n))
    o.append("};\n")

    # Bitfield names are a SEPARATE table. GL reuses enum values across contexts --
    # 0x4000 is both GL_COLOR_BUFFER_BIT and GL_LIGHT0, 0x0100 is both
    # GL_DEPTH_BUFFER_BIT and GL_ACCUM -- so a single value->name map decodes masks
    # wrongly. Only names ending in _BIT are valid mask components.
    bitmap = {}
    for v, ns in enums.items():
        if bin(v).count("1") != 1:
            continue
        for n in ns:
            if n.endswith("_BIT"):
                bitmap.setdefault(v, n)
    bits = sorted(bitmap.items())
    o.append("\ntypedef struct { long val; const char *name; } bit_ent;\n")
    o.append("#define N_BITS %d\n" % len(bits))
    o.append("static const bit_ent bit_tab[N_BITS] = {\n")
    for v, n in bits:
        o.append('    { %dL, "%s" },\n' % (v, n))
    o.append("};\n")

    # distinct-state table: open addressing, no malloc on the hot path
    o.append(r"""
#define NOTE_SLOTS 8192
typedef struct { int fn; int na; long a[3]; unsigned long n; } note_t;
static note_t notes[NOTE_SLOTS];

static void note(int fn, int na, long a0, long a1, long a2)
{
    unsigned long h = (unsigned long)fn * 2654435761UL
                    ^ (unsigned long)a0 * 40503UL
                    ^ (unsigned long)a1 * 2246822519UL
                    ^ (unsigned long)a2 * 3266489917UL;
    int i = (int)(h & (NOTE_SLOTS - 1));
    int probes = 0;
    while (probes < NOTE_SLOTS) {
        if (notes[i].n == 0) {
            notes[i].fn = fn; notes[i].na = na;
            notes[i].a[0] = a0; notes[i].a[1] = a1; notes[i].a[2] = a2;
            notes[i].n = 1;
            return;
        }
        if (notes[i].fn == fn && notes[i].a[0] == a0 &&
            notes[i].a[1] == a1 && notes[i].a[2] == a2) {
            notes[i].n++;
            return;
        }
        i = (i + 1) & (NOTE_SLOTS - 1);
        probes++;
    }
}
""")

    for f in funcs:
        name, ret = f["name"], f["ret"]
        params = f["params"]
        decl = ", ".join(p[0] for p in params) if params else "void"
        args = ", ".join(p[1] for p in params)
        ptypes = ", ".join(re.sub(r"\s+[A-Za-z_]\w*(\[[^\]]*\])?$", r"\1", p[0])
                           for p in params) if params else "void"
        isvoid = (ret.replace(" ", "") == "void")
        i = idx[name]

        o.append("\ntypedef %s (*PFN_%s)(%s);\n" % (ret, name, ptypes))
        o.append("static PFN_%s real_%s;\n" % (name, name))
        o.append("%s %s(%s)\n{\n" % (ret, name, decl))
        o.append("    call_count[%d]++;\n" % i)
        o.append("    hist[hist_pos++ %% HIST_N] = %d;\n" % i)
        if name == "glXSwapBuffers":
            o.append("    swapbuffers_count++;\n")
            # Dumping on a swap count beats dumping on a signal: SIGUSR1 is
            # istard's own, and renderd dies if we borrow it.
            o.append("    if (dump_every && swapbuffers_count % dump_every == 0)\n")
            o.append("        shim_dump();\n")
        na = notable(f)
        if na:
            vals = ["(long)%s" % params[k][1] for k in na]
            while len(vals) < 3:
                vals.append("0")
            o.append("    note(%d, %d, %s, %s, %s);\n" % (i, len(na), vals[0], vals[1], vals[2]))
        o.append("    if (!real_%s) real_%s = (PFN_%s)shim_resolve(\"%s\");\n"
                 % (name, name, name, name))
        # glBegin opens a window in which glGetError() is illegal; glEnd closes
        # it and is itself checked. glGetError must never check itself.
        if name == "glBegin":
            o.append("    in_begin = 1;\n")
        checked = name not in ("glGetError", "glBegin")
        if isvoid:
            o.append("    if (!real_%s) { shim_missing(\"%s\"); return; }\n" % (name, name))
            o.append("    real_%s(%s);\n" % (name, args))
            if name == "glEnd":
                o.append("    in_begin = 0;\n")
            if checked:
                o.append("    shim_check(%d);\n" % i)
        else:
            o.append("    if (!real_%s) { shim_missing(\"%s\"); return (%s)0; }\n"
                     % (name, name, ret))
            if checked:
                o.append("    {\n        %s shim_r = real_%s(%s);\n" % (ret, name, args))
                o.append("        shim_check(%d);\n        return shim_r;\n    }\n" % i)
            else:
                o.append("    return real_%s(%s);\n" % (name, args))
        o.append("}\n")

    o.append(EPILOGUE)
    return "".join(o)


def main():
    gl_src = open(sys.argv[1]).read()
    glx_src = open(sys.argv[2]).read()
    funcs = parse_decls(gl_src, [GL_PAT]) + parse_decls(glx_src, [GLX_PAT])
    enums = parse_enums(gl_src)
    sys.stderr.write("parsed %d entry points, %d enums\n" % (len(funcs), len(enums)))
    noted = sum(1 for f in funcs if notable(f))
    sys.stderr.write("state-recording wrappers: %d\n" % noted)
    sys.stdout.write(emit(funcs, enums))


if __name__ == "__main__":
    main()
