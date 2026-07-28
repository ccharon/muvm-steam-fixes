/*
 * muvm-dmabuf-fixup.so -- restores GPU acceleration for muvm on Apple Silicon.
 *
 * libkrun maps GPU blob resources into the guest's shared memory window as
 * dma-bufs via mmap(MAP_FIXED). Those VMAs are VM_PFNMAP. When the guest
 * writes to one, KVM resolves the page through hva_to_pfn_remapped(); if no
 * writable PTE is present it returns KVM_PFN_ERR_RO_FAULT, user_mem_abort()
 * fails with -EFAULT and KVM_RUN tears down the VM ("Failure during vcpu run:
 * Bad address (os error 14)"). KVM cannot fault in VM_PFNMAP memory itself --
 * the VMM has to, and libkrun does not.
 *
 * Workaround: touch every page of such a mapping once with a read-modify-write
 * of the same value. Contents are unchanged, but the PTE now exists as
 * writable before the guest reaches it.
 *
 * Deliberately narrow: MAP_FIXED and MAP_SHARED and PROT_READ|PROT_WRITE and
 * an fd pointing at "/dmabuf:". Writing to anything else -- notably the
 * loader's read-only MAP_FIXED mappings -- would fault.
 *
 * Build: gcc -shared -fPIC -O2 -o muvm-dmabuf-fixup.so muvm-dmabuf-fixup.c -ldl
 * Use:   LD_PRELOAD=/path/muvm-dmabuf-fixup.so muvm <command>
 * Debug: MUVM_FIXUP_LOG=/path/log.txt  (unset: silent)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

static FILE *lg;
static int   lg_tried;

static void logline(const char *fmt, ...) {
    va_list ap;
    if (!lg_tried) {
        const char *p = getenv("MUVM_FIXUP_LOG");
        lg_tried = 1;
        if (p && *p) { lg = fopen(p, "a"); if (lg) setvbuf(lg, NULL, _IOLBF, 0); }
    }
    if (!lg) return;
    va_start(ap, fmt);
    vfprintf(lg, fmt, ap);
    va_end(ap);
}

static int fd_is_dmabuf(int fd) {
    char p[64], tgt[256];
    ssize_t n;
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    n = readlink(p, tgt, sizeof tgt - 1);
    if (n <= 0) return 0;
    tgt[n] = 0;
    return strncmp(tgt, "/dmabuf:", 8) == 0;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    static void *(*real)(void *, size_t, int, int, int, off_t);
    void *r;

    if (!real) real = dlsym(RTLD_NEXT, "mmap");
    r = real(addr, len, prot, flags, fd, off);

    if (r != MAP_FAILED && fd >= 0 &&
        (flags & MAP_FIXED) && (flags & MAP_SHARED) &&
        (prot & PROT_WRITE) && (prot & PROT_READ) &&
        fd_is_dmabuf(fd)) {

        volatile unsigned char *q = (volatile unsigned char *)r;
        size_t ps = (size_t)sysconf(_SC_PAGESIZE), o;
        if (ps == (size_t)-1 || ps == 0) ps = 16384;

        for (o = 0; o < len; o += ps) {
            unsigned char v = q[o];
            q[o] = v;                 /* make the PTE writable, keep contents */
        }
        logline("[fixup] dmabuf fd=%d addr=%p len=%zu -> %zu pages touched\n",
                fd, r, len, (len + ps - 1) / ps);
    }
    return r;
}
