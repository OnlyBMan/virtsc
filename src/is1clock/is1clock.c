/*
 * is1clock - keep the guest clock in step with the host.
 *
 * FreeBSD 4.8 measures the TSC's rate once at boot, and in a VM that
 * measurement can come out a few percent high. The guest clock then runs
 * slow for the whole boot, and renderd, which counts frames against time(),
 * eventually dies with "Panic: Time drifted too much".
 *
 * qemu-is1's is1-clock device reports the host's time. This reads it every
 * 30 seconds and, whenever the guest clock is a second or more out, steps
 * the guest clock to match and logs how far out it was. A one-second step
 * moves renderd's frame count by 30 frames, well inside the 120 it allows,
 * and even a guest clock 2.3% slow only gets ~0.7 s further out between
 * checks.
 *
 * With -f it first fixes the cause as well: the device also reports the TSC
 * rate QEMU gives the guest, and that replaces the kernel's boot-time
 * measurement in machdep.tsc_freq. The guest clock then keeps time on its
 * own, and the stepping is only a safety net.
 *
 * Build on the guest:  gcc -O2 -o /usr/local/sbin/is1clock is1clock.c
 * Run as root:         is1clock [-d] [-f] [-n] [-i seconds] [-t seconds]
 */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>

#define IS1CLOCK_PORT   0x530
#define IS1CLOCK_MAGIC  0x43315349  /* "IS1C" */

#define REG_MAGIC   0x0
#define REG_SEC_LO  0x4
#define REG_SEC_HI  0x8
#define REG_NSEC    0xc
#define REG_TSC_LO  0x10
#define REG_TSC_HI  0x14

/* A read that takes longer than this was interrupted; try again. */
#define MAX_READ_US 2000
#define READ_TRIES  5

static int daemonized;

static __inline u_int
inl(u_short port)
{
    u_int v;

    __asm __volatile("inl %1, %0" : "=a" (v) : "Nd" (port));
    return v;
}

static __inline void
outl(u_short port, u_int v)
{
    __asm __volatile("outl %0, %1" : : "a" (v), "Nd" (port));
}

/* Print to the console (until daemonized) and to syslog. */
static void
report(int prio, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!daemonized)
        fprintf(prio <= LOG_WARNING ? stderr : stdout, "is1clock: %s\n", buf);
    syslog(prio, "%s", buf);
}

static double
tv2d(const struct timeval *tv)
{
    return tv->tv_sec + tv->tv_usec / 1e6;
}

/*
 * Return host time minus guest time, in seconds.
 *
 * The guest clock is read on both sides of the device read and the midpoint
 * used, so the result does not depend on how long the port accesses took -
 * unless something preempted us in between, which the time limit catches.
 */
static int
offset(double *off)
{
    struct timeval g0, g1;
    u_int lo, hi, ns;
    long us;
    int i;

    for (i = 0; i < READ_TRIES; i++) {
        gettimeofday(&g0, NULL);
        outl(IS1CLOCK_PORT + REG_MAGIC, 0);
        lo = inl(IS1CLOCK_PORT + REG_SEC_LO);
        hi = inl(IS1CLOCK_PORT + REG_SEC_HI);
        ns = inl(IS1CLOCK_PORT + REG_NSEC);
        gettimeofday(&g1, NULL);

        us = (g1.tv_sec - g0.tv_sec) * 1000000L + (g1.tv_usec - g0.tv_usec);
        if (us >= 0 && us <= MAX_READ_US) {
            /* time_t is 32 bits here; hi is zero until 2106. */
            *off = ((double)hi * 4294967296.0 + lo + ns / 1e9) -
                (tv2d(&g0) + tv2d(&g1)) / 2;
            return 0;
        }
    }
    return -1;
}

/*
 * Replace the kernel's boot-time measurement of the TSC rate with the rate
 * QEMU reports. That measurement is why the guest clock runs slow in the
 * first place; with the right rate it keeps time, and the stepping below
 * only has to deal with the boot offset and pauses.
 */
static void
set_tsc(int dry_run)
{
    u_int lo, hi, old;
    size_t len = sizeof(old);

    outl(IS1CLOCK_PORT + REG_MAGIC, 0);
    lo = inl(IS1CLOCK_PORT + REG_TSC_LO);
    hi = inl(IS1CLOCK_PORT + REG_TSC_HI);

    if (lo == 0 && hi == 0) {
        report(LOG_WARNING, "the device does not know the TSC rate; "
            "leaving machdep.tsc_freq alone");
        return;
    }
    if (hi != 0) {
        report(LOG_ERR, "the TSC runs at %.0f Hz, more than "
            "machdep.tsc_freq can hold; leaving it alone",
            hi * 4294967296.0 + lo);
        return;
    }
    if (sysctlbyname("machdep.tsc_freq", &old, &len, NULL, 0) != 0 ||
        old == 0) {
        report(LOG_WARNING, "no TSC timecounter; nothing to set");
        return;
    }
    if (lo == old) {
        report(LOG_NOTICE, "machdep.tsc_freq is already %u Hz", old);
        return;
    }
    /*
     * The kernel takes this as an unsigned int. sysctl(8) parses it as a
     * signed one and clamps anything above 2147483647, which is why this is
     * set here rather than with "sysctl -w".
     */
    if (!dry_run && sysctlbyname("machdep.tsc_freq", NULL, NULL, &lo,
        sizeof(lo)) != 0) {
        report(LOG_ERR, "setting machdep.tsc_freq to %u Hz failed", lo);
        return;
    }
    report(LOG_NOTICE, "machdep.tsc_freq %u -> %u Hz (%+.3f%%)%s", old, lo,
        100.0 * ((double)lo - old) / old, dry_run ? " (dry run)" : "");
}

/* Move the guest clock by off seconds. */
static int
step(double off)
{
    struct timeval now;
    double t;

    gettimeofday(&now, NULL);
    t = tv2d(&now) + off;
    now.tv_sec = (time_t)t;
    now.tv_usec = (long)((t - now.tv_sec) * 1e6);
    return settimeofday(&now, NULL);
}

static void
usage(void)
{
    fprintf(stderr, "usage: is1clock [-d] [-f] [-n] [-i seconds] "
        "[-t seconds]\n"
        "  -d  run in the background, checking every interval\n"
        "  -f  first set machdep.tsc_freq to the TSC rate QEMU reports\n"
        "  -n  report the offset only; never change the clock\n"
        "  -i  seconds between checks with -d (default 30)\n"
        "  -t  step the clock once it is this many seconds out "
        "(default 1)\n");
    exit(2);
}

int
main(int argc, char **argv)
{
    double off, threshold = 1.0, since;
    struct timeval last, now;
    int ch, daemon_mode = 0, dry_run = 0, fix_tsc = 0, interval = 30, fd;
    int synced = 0;

    while ((ch = getopt(argc, argv, "dfni:t:")) != -1) {
        switch (ch) {
        case 'd':
            daemon_mode = 1;
            break;
        case 'f':
            fix_tsc = 1;
            break;
        case 'n':
            dry_run = 1;
            break;
        case 'i':
            interval = atoi(optarg);
            if (interval < 1)
                usage();
            break;
        case 't':
            threshold = atof(optarg);
            if (threshold <= 0)
                usage();
            break;
        default:
            usage();
        }
    }
    openlog("is1clock", LOG_PID, LOG_DAEMON);

    /* Opening /dev/io grants this process IOPL 3 for the port access. */
    if ((fd = open("/dev/io", O_RDWR)) < 0) {
        perror("is1clock: /dev/io (must run as root)");
        return 1;
    }
    if (inl(IS1CLOCK_PORT + REG_MAGIC) != IS1CLOCK_MAGIC) {
        report(LOG_ERR, "no is1-clock device at port 0x%x; add "
            "-device is1-clock to the QEMU command line", IS1CLOCK_PORT);
        return 1;
    }

    if (fix_tsc)
        set_tsc(dry_run);

    if (daemon_mode) {
        /* Keep /dev/io open: IOPL belongs to this process, and survives. */
        if (daemon(0, 0) != 0) {
            perror("is1clock: daemon");
            return 1;
        }
        daemonized = 1;
    }

    gettimeofday(&last, NULL);
    for (;;) {
        if (offset(&off) != 0) {
            report(LOG_WARNING, "could not get a clean reading; will retry");
        } else if (off >= threshold || off <= -threshold) {
            gettimeofday(&now, NULL);
            since = tv2d(&now) - tv2d(&last);
            if (dry_run) {
                report(LOG_NOTICE, "guest clock is %.3f s %s the host",
                    off > 0 ? off : -off, off > 0 ? "behind" : "ahead of");
            } else if (step(off) != 0) {
                report(LOG_ERR, "settimeofday failed; guest clock is %.3f s "
                    "%s the host", off > 0 ? off : -off,
                    off > 0 ? "behind" : "ahead of");
            } else if (!synced) {
                /* Mostly the RTC's one-second resolution at boot. */
                report(LOG_NOTICE, "guest clock was %.3f s %s the host; "
                    "stepped", off > 0 ? off : -off,
                    off > 0 ? "behind" : "ahead of");
            } else {
                /*
                 * The rate tells a slow boot calibration from a one-off.
                 * since is guest time; since + off is the host time that
                 * really passed.
                 */
                report(LOG_NOTICE, "guest clock was %.3f s %s the host; "
                    "stepped (%.0f s since last sync, guest clock %+.3f%%)",
                    off > 0 ? off : -off, off > 0 ? "behind" : "ahead of",
                    since + off, -100.0 * off / (since + off));
            }
            if (!dry_run) {
                synced = 1;
                gettimeofday(&last, NULL);
            }
        } else if (!daemon_mode) {
            report(LOG_NOTICE, "guest clock is within %.3f s of the host "
                "(%+.3f s)", threshold, off);
        }
        if (!daemon_mode)
            return 0;
        sleep(interval);
    }
}
