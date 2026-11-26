#include "ssvm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#include <linux/magic.h>

struct wr {
    int fd;
    uint64_t base;
};

static int wput(struct wr *w, uint64_t off, const void *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t r = pwrite(w->fd, (const char *)buf + done, len - done,
                           (off_t)(w->base + off + done));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return -1;
        done += (size_t)r;
    }
    return 0;
}

static void p16(unsigned char *b, uint16_t v)
{
    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)(v >> 8);
}

static void p32(unsigned char *b, uint32_t v)
{
    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)((v >> 8) & 0xff);
    b[2] = (unsigned char)((v >> 16) & 0xff);
    b[3] = (unsigned char)((v >> 24) & 0xff);
}

static void p64(unsigned char *b, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        b[i] = (unsigned char)((v >> (8 * i)) & 0xff);
}

static void b16(unsigned char *b, uint16_t v)
{
    b[0] = (unsigned char)(v >> 8);
    b[1] = (unsigned char)(v & 0xff);
}

static void b32(unsigned char *b, uint32_t v)
{
    b[0] = (unsigned char)((v >> 24) & 0xff);
    b[1] = (unsigned char)((v >> 16) & 0xff);
    b[2] = (unsigned char)((v >> 8) & 0xff);
    b[3] = (unsigned char)(v & 0xff);
}

static void b64(unsigned char *b, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        b[7 - i] = (unsigned char)((v >> (8 * i)) & 0xff);
}

static uint32_t crc32_run(uint32_t crc, const unsigned char *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

static void raw_uuid(unsigned char out[16])
{
    int fd = open("/dev/urandom", O_RDONLY);
    ssize_t got = -1;

    if (fd >= 0) {
        got = read(fd, out, 16);
        close(fd);
    }
    if (got != 16) {
        struct timespec ts;
        uint64_t x;
        clock_gettime(CLOCK_REALTIME, &ts);
        x = (uint64_t)ts.tv_sec * 1000000000ull ^ (uint64_t)ts.tv_nsec
            ^ ((uint64_t)getpid() << 32);
        for (int i = 0; i < 16; i++) {
            out[i] = (unsigned char)(x & 0xff);
            x = x * 6364136223846793005ull + 1442695040888963407ull;
        }
    }
}

int ssvm_valid_fstype(const char *s)
{
    if (!s)
        return 0;
    return strcmp(s, "ext2") == 0 || strcmp(s, "ext3") == 0 ||
           strcmp(s, "ext4") == 0 || strcmp(s, "xfs") == 0 ||
           strcmp(s, "f2fs") == 0;
}

uint64_t ssvm_fs_min_size(const char *fstype)
{
    if (fstype && (strcmp(fstype, "xfs") == 0 || strcmp(fstype, "f2fs") == 0))
        return 64ull * 1024ull * 1024ull;
    return SSVM_SIZE_MIN;
}

const char *ssvm_host_fstype(const char *dir)
{
    struct statfs st;

    if (!dir || statfs(dir, &st) != 0)
        return NULL;
    switch ((unsigned long)st.f_type) {
    case EXT4_SUPER_MAGIC:
        return "ext4";
    case F2FS_SUPER_MAGIC:
        return "f2fs";
    case XFS_SUPER_MAGIC:
        return "xfs";
    default:
        return NULL;
    }
}

#define EXT_BS    4096u
#define EXT_BPG   32768u
#define EXT_IPG   2048u
#define EXT_ISZ   256u
#define EXT_ITB   (EXT_IPG * EXT_ISZ / EXT_BS)

struct extg {
    uint32_t blocks, groups, bgdtb;
    uint32_t jstart, jblocks, ind, root, lf;
    uint32_t free_b;
    int level;
};

static int ext_has_super(uint32_t g)
{
    uint32_t p;

    if (g == 0 || g == 1)
        return 1;
    for (p = 3; p <= g; p *= 3)
        if (p == g)
            return 1;
    for (p = 5; p <= g; p *= 5)
        if (p == g)
            return 1;
    for (p = 7; p <= g; p *= 7)
        if (p == g)
            return 1;
    return 0;
}

static uint32_t ext_gsize(const struct extg *g, uint32_t gi)
{
    uint64_t start = (uint64_t)gi * EXT_BPG;

    if (start + EXT_BPG > g->blocks)
        return (uint32_t)(g->blocks - start);
    return EXT_BPG;
}

static void ext_glayout(const struct extg *g, uint32_t gi,
                        uint32_t *bm, uint32_t *ibm, uint32_t *it)
{
    uint64_t start = (uint64_t)gi * EXT_BPG;
    uint32_t meta = ext_has_super(gi) ? 1u + g->bgdtb : 0u;

    *bm = (uint32_t)start + meta;
    *ibm = *bm + 1;
    *it = *bm + 2;
}

static uint32_t ext_gused(const struct extg *g, uint32_t gi)
{
    uint32_t meta = ext_has_super(gi) ? 1u + g->bgdtb : 0u;
    uint32_t used = meta + 2u + EXT_ITB;

    if (gi == 0) {
        used += 2;
        if (g->jblocks)
            used += g->jblocks + 1;
    }
    return used;
}

static void ext_dirent(unsigned char *b, size_t off, uint32_t ino,
                       uint16_t rec, const char *name, uint8_t nl)
{
    p32(b + off, ino);
    p16(b + off + 4, rec);
    b[off + 6] = nl;
    b[off + 7] = 2;
    memcpy(b + off + 8, name, nl);
}

static int ext_write_inode(struct wr *w, uint32_t it,
                           uint32_t ino, uint16_t mode, uint32_t size,
                           uint16_t links, uint32_t blocks512,
                           const uint32_t *bmap)
{
    unsigned char b[EXT_ISZ];
    uint32_t now = (uint32_t)time(NULL);

    memset(b, 0, sizeof b);
    p16(b + 0, mode);
    p16(b + 2, 0);
    p32(b + 4, size);
    p32(b + 8, now);
    p32(b + 12, now);
    p32(b + 16, now);
    p32(b + 20, 0);
    p16(b + 24, 0);
    p16(b + 26, links);
    p32(b + 28, blocks512);
    p32(b + 32, 0);
    if (bmap)
        for (int i = 0; i < 15; i++)
            p32(b + 40 + 4 * i, bmap[i]);
    return wput(w, (uint64_t)it * EXT_BS + (uint64_t)(ino - 1) * EXT_ISZ,
                b, sizeof b);
}

static int mkfs_ext(struct wr *w, int level, const char *label,
                    uint64_t datalen)
{
    struct extg g;
    unsigned char *sb, *gdt, *blk;
    unsigned char uuid[16];
    uint32_t i, now = (uint32_t)time(NULL);
    uint64_t fpos;
    int rc = -1;

    memset(&g, 0, sizeof g);
    g.level = level;
    g.blocks = (uint32_t)(datalen / EXT_BS);
    if (g.blocks > 0xFFFFFF00u)
        g.blocks = 0xFFFFFF00u;
    if (g.blocks < 1024u) {
        errno = EINVAL;
        return -1;
    }
    g.groups = (g.blocks + EXT_BPG - 1) / EXT_BPG;
    g.bgdtb = (g.groups * 32u + EXT_BS - 1) / EXT_BS;
    if (level >= 3) {
        uint64_t j = datalen / EXT_BS / 32;
        if (j < 1024)
            j = 1024;
        if (j > 1036)
            j = 1036;
        g.jblocks = (uint32_t)j;
    }

    {
        uint32_t bm, ibm, it;
        ext_glayout(&g, 0, &bm, &ibm, &it);
        g.jstart = it + EXT_ITB;
        g.ind = g.jstart + g.jblocks;
        g.root = g.ind + (g.jblocks ? 1u : 0u);
        g.lf = g.root + 1;
    }

    g.free_b = 0;
    for (i = 0; i < g.groups; i++) {
        uint32_t used = ext_gused(&g, i);
        if (used > ext_gsize(&g, i)) {
            errno = ENOSPC;
            return -1;
        }
        g.free_b += ext_gsize(&g, i) - used;
    }

    sb = calloc(1, 1024);
    gdt = calloc(1, (size_t)g.bgdtb * EXT_BS);
    blk = calloc(1, EXT_BS);
    if (!sb || !gdt || !blk)
        goto out;
    raw_uuid(uuid);

    p32(sb + 0, g.groups * EXT_IPG);
    p32(sb + 4, g.blocks);
    p32(sb + 8, g.blocks / 20);
    p32(sb + 12, g.free_b);
    p32(sb + 16, g.groups * EXT_IPG - 11);
    p32(sb + 20, 0);
    p32(sb + 24, 2);
    p32(sb + 28, 2);
    p32(sb + 32, EXT_BPG);
    p32(sb + 36, EXT_BPG);
    p32(sb + 40, EXT_IPG);
    p32(sb + 48, now);
    p16(sb + 54, 0xFFFF);
    p16(sb + 56, 0xEF53);
    p16(sb + 58, 1);
    p16(sb + 60, 1);
    p32(sb + 64, now);
    p32(sb + 72, 0);
    p32(sb + 76, 1);
    p32(sb + 84, 11);
    p16(sb + 88, EXT_ISZ);
    p32(sb + 96, level >= 3 ? 0x6u : 0x2u);
    if (level == 4)
        p32(sb + 96, 0x46u);
    p32(sb + 100, 0x3u);
    memcpy(sb + 104, uuid, 16);
    snprintf((char *)sb + 120, 17, "%s", label);
    p32(sb + 224, level >= 3 ? 8u : 0u);
    p16(sb + 254, 32);
    p32(sb + 264, now);

    if (wput(w, 1024, sb, 1024) != 0)
        goto out;

    for (i = 0; i < g.groups; i++) {
        uint32_t bm, ibm, it, used, freeb;
        unsigned char *e = gdt + (size_t)i * 32;

        ext_glayout(&g, i, &bm, &ibm, &it);
        used = ext_gused(&g, i);
        freeb = ext_gsize(&g, i) - used;
        p32(e + 0, bm);
        p32(e + 4, ibm);
        p32(e + 8, it);
        p16(e + 12, (uint16_t)freeb);
        p16(e + 14, (uint16_t)(i == 0 ? EXT_IPG - 11 : EXT_IPG));
        p16(e + 16, (uint16_t)(i == 0 ? 2 : 0));
    }
    if (wput(w, EXT_BS, gdt, (size_t)g.bgdtb * EXT_BS) != 0)
        goto out;

    for (i = 0; i < g.groups; i++) {
        uint32_t bm, ibm, it, used, k, lim;

        ext_glayout(&g, i, &bm, &ibm, &it);
        used = ext_gused(&g, i);

        memset(blk, 0, EXT_BS);
        lim = used < EXT_BS * 8 ? used : EXT_BS * 8;
        for (k = 0; k < lim; k++)
            blk[k >> 3] |= (unsigned char)(1u << (k & 7));
        if (wput(w, (uint64_t)bm * EXT_BS, blk, EXT_BS) != 0)
            goto out;

        memset(blk, 0, EXT_BS);
        if (i == 0) {
            for (k = 0; k < 11; k++)
                blk[k >> 3] |= (unsigned char)(1u << (k & 7));
        }
        if (wput(w, (uint64_t)ibm * EXT_BS, blk, EXT_BS) != 0)
            goto out;

        if (i != 0 && ext_has_super(i)) {
            p16(sb + 90, (uint16_t)i);
            fpos = (uint64_t)i * EXT_BPG * EXT_BS;
            if (wput(w, fpos + 1024, sb, 1024) != 0)
                goto out;
            if (wput(w, fpos + EXT_BS, gdt, (size_t)g.bgdtb * EXT_BS) != 0)
                goto out;
        }
    }
    p16(sb + 90, 0);

    {
        uint32_t bm, ibm, it;
        uint32_t rmap[15];

        ext_glayout(&g, 0, &bm, &ibm, &it);

        memset(rmap, 0, sizeof rmap);
        rmap[0] = g.root;
        if (ext_write_inode(w, it, 2, 0x41ED, EXT_BS, 3, 8, rmap) != 0)
            goto out;

        memset(rmap, 0, sizeof rmap);
        rmap[0] = g.lf;
        if (ext_write_inode(w, it, 11, 0x41C0, EXT_BS, 2, 8, rmap) != 0)
            goto out;

        if (g.jblocks) {
            unsigned char jsb[128];

            memset(rmap, 0, sizeof rmap);
            for (i = 0; i < 12 && i < g.jblocks; i++)
                rmap[i] = g.jstart + i;
            if (g.jblocks > 12)
                rmap[12] = g.ind;
            if (ext_write_inode(w, it, 8, 0x8180,
                                g.jblocks * EXT_BS, 1,
                                (g.jblocks + 1u) * 8u, rmap) != 0)
                goto out;

            memset(jsb, 0, sizeof jsb);
            b32(jsb + 0, 0xc03b3998u);
            b32(jsb + 4, 4);
            b32(jsb + 8, 0);
            b32(jsb + 12, EXT_BS);
            b32(jsb + 16, g.jblocks);
            b32(jsb + 20, 1);
            b32(jsb + 24, 1);
            b32(jsb + 28, 0);
            b32(jsb + 32, 0);
            b32(jsb + 36, 0);
            memcpy(jsb + 48, uuid, 16);
            b32(jsb + 64, 1);
            if (wput(w, (uint64_t)g.jstart * EXT_BS, jsb, sizeof jsb) != 0)
                goto out;

            if (g.jblocks > 12) {
                uint32_t n = g.jblocks - 12;

                memset(blk, 0, EXT_BS);
                for (i = 0; i < n; i++)
                    p32(blk + 4 * i, g.jstart + 12 + i);
                if (wput(w, (uint64_t)g.ind * EXT_BS, blk, EXT_BS) != 0)
                    goto out;
            }
        }

        memset(blk, 0, EXT_BS);
        ext_dirent(blk, 0, 2, 12, ".", 1);
        ext_dirent(blk, 12, 2, 12, "..", 2);
        ext_dirent(blk, 24, 11, EXT_BS - 24, "lost+found", 10);
        if (wput(w, (uint64_t)g.root * EXT_BS, blk, EXT_BS) != 0)
            goto out;

        memset(blk, 0, EXT_BS);
        ext_dirent(blk, 0, 11, 12, ".", 1);
        ext_dirent(blk, 12, 2, EXT_BS - 12, "..", 2);
        if (wput(w, (uint64_t)g.lf * EXT_BS, blk, EXT_BS) != 0)
            goto out;
    }

    rc = 0;

out:
    free(sb);
    free(gdt);
    free(blk);
    return rc;
}

struct xfsg {
    uint64_t dblocks;
    uint32_t agcount, agsize, agblklog;
    uint32_t logstart, logblocks, chunk;
    uint64_t freeblks;
    unsigned char uuid[16];
};

static int xfs_write_dinode(struct wr *w, uint32_t agblock, uint32_t idx,
                            uint16_t mode, uint8_t format, uint32_t nlink,
                            uint64_t size, const unsigned char *fork,
                            size_t forklen)
{
    unsigned char b[256];
    uint32_t now = (uint32_t)time(NULL);

    memset(b, 0, sizeof b);
    b16(b + 0, 0x494e);
    b16(b + 2, mode);
    b[4] = 2;
    b[5] = format;
    b16(b + 6, 0);
    b32(b + 8, 0);
    b32(b + 12, 0);
    b32(b + 16, nlink);
    b32(b + 32, now);
    b32(b + 36, 0);
    b32(b + 40, now);
    b32(b + 44, 0);
    b32(b + 48, now);
    b32(b + 52, 0);
    b64(b + 56, size);
    b64(b + 64, 0);
    b32(b + 76, 0);
    b[81] = 0;
    b16(b + 82, 2);
    b32(b + 96, 0xFFFFFFFFu);
    if (fork && forklen)
        memcpy(b + 100, fork, forklen);
    return wput(w, (uint64_t)agblock * 4096ull + (uint64_t)idx * 256ull,
                b, sizeof b);
}

static int mkfs_xfs(struct wr *w, const char *label, uint64_t datalen)
{
    struct xfsg x;
    unsigned char *b;
    uint32_t *agfree;
    uint64_t fdb = 0;
    uint32_t ag;
    int rc = -1;

    memset(&x, 0, sizeof x);
    x.dblocks = datalen / 4096;
    if (x.dblocks < 16384) {
        errno = EINVAL;
        return -1;
    }
    x.agcount = 1;
    while (x.dblocks / x.agcount > 0x7FFFFFFFull)
        x.agcount++;
    x.agsize = (uint32_t)(x.dblocks / x.agcount);
    x.dblocks = (uint64_t)x.agsize * x.agcount;
    x.agblklog = 0;
    while (((uint64_t)1 << x.agblklog) < x.agsize)
        x.agblklog++;
    x.logblocks = (uint32_t)(x.dblocks / 4);
    if (x.logblocks > 2560)
        x.logblocks = 2560;
    x.logstart = 8;
    x.chunk = x.logstart + x.logblocks;
    if (x.chunk & 1)
        x.chunk++;
    raw_uuid(x.uuid);

    agfree = calloc(x.agcount, sizeof *agfree);
    b = calloc(1, 4096);
    if (!agfree || !b)
        goto out;

    for (ag = 0; ag < x.agcount; ag++) {
        uint64_t agbase = (uint64_t)ag * x.agsize;
        uint32_t agblocks = (ag == x.agcount - 1)
                                ? (uint32_t)(x.dblocks - agbase)
                                : x.agsize;
        uint32_t freestart = (ag == 0) ? x.chunk + 4 : 4;

        agfree[ag] = agblocks - freestart;
        fdb += agfree[ag] + (ag == 0 ? 4u : 0u);
    }

    for (ag = 0; ag < x.agcount; ag++) {
        uint64_t agbase = (uint64_t)ag * x.agsize;
        uint32_t agblocks = (ag == x.agcount - 1)
                                ? (uint32_t)(x.dblocks - agbase)
                                : x.agsize;
        uint32_t freestart = (ag == 0) ? x.chunk + 4 : 4;
        uint32_t freeblks = agfree[ag];
        uint32_t i;

        memset(b, 0, 512);
        b32(b + 0, 0x58465342u);
        b32(b + 4, 4096);
        b64(b + 8, x.dblocks);
        b64(b + 16, 0);
        b64(b + 24, 0);
        memcpy(b + 32, x.uuid, 16);
        b64(b + 48, x.logstart);
        b64(b + 56, (uint64_t)x.chunk * 16u);
        b64(b + 64, (uint64_t)x.chunk * 16u + 1u);
        b64(b + 72, (uint64_t)x.chunk * 16u + 2u);
        b32(b + 80, 1);
        b32(b + 84, agblocks);
        b32(b + 88, x.agcount);
        b32(b + 92, 0);
        b32(b + 96, x.logblocks);
        b16(b + 100, 0xB4A4);
        b16(b + 102, 512);
        b16(b + 104, 256);
        b16(b + 106, 16);
        snprintf((char *)b + 108, 13, "%s", label);
        b[120] = 12;
        b[121] = 9;
        b[122] = 8;
        b[123] = 4;
        b[124] = (unsigned char)x.agblklog;
        b[126] = 0;
        b[127] = 25;
        b64(b + 128, 64);
        b64(b + 136, 61);
        b64(b + 144, fdb);
        b64(b + 152, 0);
        b64(b + 160, 0);
        b64(b + 168, 0);
        b16(b + 176, 0);
        b[178] = 0;
        b[179] = 0;
        b32(b + 180, 2);
        b32(b + 184, 0);
        b32(b + 188, 0);
        b[192] = 0;
        b[193] = 0;
        b16(b + 194, 0);
        b32(b + 196, 1);
        b32(b + 200, 0x0000028A);
        b32(b + 204, 0x0000028A);
        if (wput(w, agbase * 4096ull, b, 512) != 0)
            goto out;

        memset(b, 0, 512);
        b32(b + 0, 0x58414746u);
        b32(b + 4, 1);
        b32(b + 8, ag);
        b32(b + 12, agblocks);
        b32(b + 16, 1);
        b32(b + 20, 2);
        b32(b + 24, 0xFFFFFFFFu);
        b32(b + 28, 1);
        b32(b + 32, 1);
        b32(b + 36, 0);
        b32(b + 40, ag == 0 ? 1 : 0);
        b32(b + 44, ag == 0 ? 4 : 0);
        b32(b + 48, ag == 0 ? 4 : 0);
        b32(b + 52, freeblks);
        b32(b + 56, freeblks);
        b32(b + 60, 0);
        if (wput(w, agbase * 4096ull + 512, b, 512) != 0)
            goto out;

        memset(b, 0, 512);
        b32(b + 0, 0x58414749u);
        b32(b + 4, 1);
        b32(b + 8, ag);
        b32(b + 12, agblocks);
        b32(b + 16, ag == 0 ? 64 : 0);
        b32(b + 20, 3);
        b32(b + 24, 1);
        b32(b + 28, ag == 0 ? 61 : 0);
        b32(b + 32, ag == 0 ? (uint32_t)x.chunk * 16u : 0xFFFFFFFFu);
        b32(b + 36, 0xFFFFFFFFu);
        for (i = 0; i < 64; i++)
            b32(b + 40 + 4 * i, 0xFFFFFFFFu);
        if (wput(w, agbase * 4096ull + 1024, b, 512) != 0)
            goto out;

        memset(b, 0xFF, 512);
        if (ag == 0) {
            b32(b + 4, 4);
            b32(b + 8, 5);
            b32(b + 12, 6);
            b32(b + 16, 7);
        }
        if (wput(w, agbase * 4096ull + 1536, b, 512) != 0)
            goto out;

        memset(b, 0, 4096);
        b32(b + 0, 0x41425442u);
        b16(b + 4, 0);
        b16(b + 6, 1);
        b32(b + 8, 0xFFFFFFFFu);
        b32(b + 12, 0xFFFFFFFFu);
        b32(b + 16, freestart);
        b32(b + 20, freeblks);
        if (wput(w, agbase * 4096ull + 4096, b, 4096) != 0)
            goto out;

        memset(b, 0, 4096);
        b32(b + 0, 0x41425443u);
        b16(b + 4, 0);
        b16(b + 6, 1);
        b32(b + 8, 0xFFFFFFFFu);
        b32(b + 12, 0xFFFFFFFFu);
        b32(b + 16, freestart);
        b32(b + 20, freeblks);
        if (wput(w, agbase * 4096ull + 8192, b, 4096) != 0)
            goto out;

        memset(b, 0, 4096);
        b32(b + 0, 0x49414254u);
        b16(b + 4, 0);
        b16(b + 6, ag == 0 ? 1 : 0);
        b32(b + 8, 0xFFFFFFFFu);
        b32(b + 12, 0xFFFFFFFFu);
        if (ag == 0) {
            b32(b + 16, (uint32_t)x.chunk * 16u);
            b32(b + 20, 61);
            b64(b + 24, 0xFFFFFFFFFFFFFFF8ull);
        }
        if (wput(w, agbase * 4096ull + 12288, b, 4096) != 0)
            goto out;
    }

    {
        unsigned char fork[6];
        uint32_t ri = x.chunk * 16u;
        uint32_t k;

        fork[0] = 0;
        fork[1] = 0;
        fork[2] = (unsigned char)((ri >> 24) & 0xff);
        fork[3] = (unsigned char)((ri >> 16) & 0xff);
        fork[4] = (unsigned char)((ri >> 8) & 0xff);
        fork[5] = (unsigned char)(ri & 0xff);
        if (xfs_write_dinode(w, x.chunk, 0, 0x41ED, 1, 2, 6, fork, 6) != 0)
            goto out;
        if (xfs_write_dinode(w, x.chunk, 1, 0x8180, 2, 1, 0, NULL, 0) != 0)
            goto out;
        if (xfs_write_dinode(w, x.chunk, 2, 0x8180, 2, 1, 0, NULL, 0) != 0)
            goto out;
        for (k = 3; k < 64; k++) {
            unsigned char fb[256];

            memset(fb, 0, sizeof fb);
            b16(fb + 0, 0x494eu);
            fb[4] = 2;
            b32(fb + 96, 0xFFFFFFFFu);
            if (wput(w, (uint64_t)x.chunk * 4096ull + (uint64_t)k * 256ull,
                      fb, sizeof fb) != 0)
                goto out;
        }
    }

    rc = 0;

out:
    free(agfree);
    free(b);
    return rc;
}

struct f2g {
    uint32_t blocks, seg_count;
    uint32_t sit_half, nat_half, cp_payload;
    uint32_t sit_ba, nat_ba, main_ba;
    uint32_t main_segs, zones;
    uint32_t cseg[6];
    uint32_t reserved, overprov, free_segs, user_blocks;
    unsigned char uuid[16];
};

#define F2_MAGIC       0xF2F52010u
#define F2_SEGSZ       512u
#define F2_SIT_EPB     55u
#define F2_NAT_EPB     455u
#define F2_SUM_JOURNAL 507u

static int f2fs_write_sb(struct wr *w, const struct f2g *g,
                         const char *label, uint64_t datalen)
{
    unsigned char *b = calloc(1, 4096);
    int rc = -1;

    if (!b)
        return -1;
    p32(b + 1024 + 0, F2_MAGIC);
    p16(b + 1024 + 4, 1);
    p16(b + 1024 + 6, 0);
    p32(b + 1024 + 8, 9);
    p32(b + 1024 + 12, 3);
    p32(b + 1024 + 16, 12);
    p32(b + 1024 + 20, 9);
    p32(b + 1024 + 24, 1);
    p32(b + 1024 + 28, 1);
    p32(b + 1024 + 32, 0);
    p64(b + 1024 + 36, datalen / 4096);
    p32(b + 1024 + 44, g->zones);
    p32(b + 1024 + 48, g->seg_count);
    p32(b + 1024 + 52, 2);
    p32(b + 1024 + 56, g->sit_half * 2);
    p32(b + 1024 + 60, g->nat_half * 2);
    p32(b + 1024 + 64, g->seg_count - g->main_segs - 2 - g->sit_half * 2
                       - g->nat_half * 2);
    p32(b + 1024 + 68, g->main_segs);
    p32(b + 1024 + 72, 512);
    p32(b + 1024 + 76, 512);
    p32(b + 1024 + 80, g->sit_ba);
    p32(b + 1024 + 84, g->nat_ba);
    p32(b + 1024 + 88, g->nat_ba + g->nat_half * 2 * F2_SEGSZ);
    p32(b + 1024 + 92, g->main_ba);
    p32(b + 1024 + 96, 3);
    p32(b + 1024 + 100, 1);
    p32(b + 1024 + 104, 2);
    memcpy(b + 1024 + 108, g->uuid, 16);
    for (size_t i = 0; label[i] && i < 63; i++)
        p16(b + 1024 + 124 + 2 * i, (uint16_t)(unsigned char)label[i]);

    if (wput(w, 0, b, 4096) != 0)
        goto out;
    if (wput(w, 4096, b, 4096) != 0)
        goto out;
    rc = 0;

out:
    free(b);
    return rc;
}

static void f2fs_fill_cp(unsigned char *cp, const struct f2g *g,
                         uint64_t ver)
{
    memset(cp, 0, 4096);
    p64(cp + 0, ver);
    p64(cp + 8, g->user_blocks);
    p64(cp + 16, 2);
    p32(cp + 24, g->reserved);
    p32(cp + 28, g->overprov);
    p32(cp + 32, g->free_segs);
    p32(cp + 36, g->cseg[3]);
    p32(cp + 40, g->cseg[4]);
    p32(cp + 44, g->cseg[5]);
    for (int i = 3; i < 8; i++)
        p32(cp + 36 + 4 * i, 0xFFFFFFFFu);
    p16(cp + 68, 1);
    p32(cp + 84, g->cseg[0]);
    p32(cp + 88, g->cseg[1]);
    p32(cp + 92, g->cseg[2]);
    for (int i = 3; i < 8; i++)
        p32(cp + 84 + 4 * i, 0xFFFFFFFFu);
    p16(cp + 116, 1);
    p32(cp + 132, 0x5u);
    p32(cp + 136, 6 + g->cp_payload);
    p32(cp + 140, 1 + g->cp_payload);
    p32(cp + 144, 1);
    p32(cp + 148, 1);
    p32(cp + 152, 4);
    p32(cp + 156, g->sit_half * 64);
    p32(cp + 160, g->nat_half * 64);
    p32(cp + 164, 4092);
    p32(cp + 4092, crc32_run(F2_MAGIC, cp, 4092));
}

static int mkfs_f2fs(struct wr *w, const char *label, uint64_t datalen)
{
    struct f2g g;
    unsigned char *b;
    unsigned char *cp;
    uint64_t cp_ver;
    uint32_t v, blocks_for_sit, blocks_for_nat, ssa_segs, meta_segs;
    int rc = -1;

    memset(&g, 0, sizeof g);
    g.blocks = (uint32_t)(datalen / 4096);
    if (g.blocks < 16384) {
        errno = EINVAL;
        return -1;
    }
    g.seg_count = (g.blocks - 512) / F2_SEGSZ;

    g.sit_ba = 512 + 2 * F2_SEGSZ;
    blocks_for_sit = (g.seg_count + F2_SIT_EPB - 1) / F2_SIT_EPB;
    g.sit_half = (blocks_for_sit + F2_SEGSZ - 1) / F2_SEGSZ;
    g.nat_ba = g.sit_ba + g.sit_half * 2 * F2_SEGSZ;

    v = (g.seg_count - (2 + g.sit_half * 2)) * F2_SEGSZ;
    blocks_for_nat = (v + F2_NAT_EPB - 1) / F2_NAT_EPB;
    g.nat_half = (blocks_for_nat + F2_SEGSZ - 1) / F2_SEGSZ;
    {
        uint32_t sit_bitmap_size = g.sit_half * 64;
        uint32_t max_nat, max_nat_segs;

        if (sit_bitmap_size > 3836) {
            max_nat = 3900;
            g.cp_payload = (sit_bitmap_size + 4095) / 4096;
        } else {
            max_nat = 3900 - sit_bitmap_size;
            g.cp_payload = 0;
        }
        max_nat_segs = (max_nat * 8) >> 9;
        if (g.nat_half > max_nat_segs)
            g.nat_half = max_nat_segs;
    }

    v = (g.seg_count - (2 + g.sit_half * 2 + g.nat_half * 2)) * F2_SEGSZ;
    ssa_segs = ((v / F2_SEGSZ) + 1 + F2_SEGSZ - 1) / F2_SEGSZ;
    meta_segs = 2 + g.sit_half * 2 + g.nat_half * 2 + ssa_segs;
    g.main_ba = 512 + meta_segs * F2_SEGSZ;
    g.main_segs = g.seg_count - meta_segs;
    g.zones = g.main_segs;

    if (g.zones <= 6) {
        errno = EINVAL;
        return -1;
    }

    g.cseg[3] = g.zones - 1;
    g.cseg[4] = g.zones - 2;
    g.cseg[5] = g.zones - 3;
    g.cseg[0] = g.zones - 4;
    g.cseg[2] = 0;
    g.cseg[1] = 1;

    g.reserved = 18;
    g.overprov = (g.main_segs - g.reserved) * 20 / 100 + g.reserved;
    g.free_segs = g.main_segs - 6;
    g.user_blocks = (uint64_t)(g.free_segs + 6 - g.overprov) * F2_SEGSZ;
    raw_uuid(g.uuid);
    cp_ver = ((uint64_t)time(NULL) & 0x7FFFFFFFull) << 1 | 1ull;

    b = calloc(1, 4096);
    cp = calloc(1, 4096);
    if (!b || !cp)
        goto out;

    if (f2fs_write_sb(w, &g, label, datalen) != 0)
        goto out;

    f2fs_fill_cp(cp, &g, cp_ver);

    if (wput(w, (uint64_t)512 * 4096, cp, 4096) != 0)
        goto out;

    {
        uint32_t root_addr = g.main_ba + g.cseg[3] * F2_SEGSZ;
        uint64_t cpb = (uint64_t)(512 + 1 + g.cp_payload) * 4096;

        memset(b, 0, 4096);
        p16(b + 0, 1);
        p32(b + 2, 3);
        b[6] = 0;
        p32(b + 7, 3);
        p32(b + 11, root_addr);

        p16(b + F2_SUM_JOURNAL, 6);
        {
            uint32_t offs = F2_SUM_JOURNAL + 2;
            const uint32_t segs[6] = { g.cseg[3], g.cseg[4], g.cseg[5],
                                       g.cseg[0], g.cseg[1], g.cseg[2] };
            const uint32_t types[6] = { 3, 4, 5, 0, 1, 2 };
            for (int i = 0; i < 6; i++) {
                p32(b + offs, segs[i]);
                p16(b + offs + 4, (uint16_t)((types[i] << 10)
                                             | ((i == 0 || i == 3) ? 1 : 0)));
                if (i == 0 || i == 3)
                    b[offs + 6] = 1;
                offs += 78;
            }
        }
        p32(b + 2 * F2_SUM_JOURNAL, 3);
        b[2 * F2_SUM_JOURNAL + 4] = 0;
        p16(b + 2 * F2_SUM_JOURNAL + 5, 0);
        if (wput(w, cpb, b, 4096) != 0)
            goto out;

        for (int t = 0; t < 3; t++) {
            memset(b, 0, 4096);
            if (t == 0) {
                p32(b + 0, 3);
                p16(b + 4, 0);
            }
            b[4091] = 1;
            if (wput(w, cpb + 4096 + (uint64_t)t * 4096, b, 4096) != 0)
                goto out;
        }

        if (wput(w, cpb + 4 * 4096, cp, 4096) != 0)
            goto out;

        f2fs_fill_cp(cp, &g, 0);
        if (wput(w, (uint64_t)(512 + F2_SEGSZ) * 4096, cp, 4096) != 0)
            goto out;
        if (wput(w, (uint64_t)(512 + F2_SEGSZ + 5 + g.cp_payload) * 4096,
                 cp, 4096) != 0)
            goto out;

        memset(b, 0, 4096);
        p16(b + 0, 0x41ED);
        b[3] = 0;
        p32(b + 12, 2);
        p64(b + 16, 4096);
        p64(b + 24, 2);
        p64(b + 32, (uint64_t)time(NULL));
        p64(b + 40, (uint64_t)time(NULL));
        p64(b + 48, (uint64_t)time(NULL));
        p32(b + 72, 1);
        b[347] = 0;
        p32(b + 360, g.main_ba + g.cseg[0] * F2_SEGSZ);
        p32(b + 4072, 3);
        p32(b + 4076, 3);
        p32(b + 4080, 0);
        p64(b + 4084, 1);
        p32(b + 4092, root_addr + 1);
        if (wput(w, (uint64_t)root_addr * 4096, b, 4096) != 0)
            goto out;

        memset(b, 0, 4096);
        b[0] = 3;
        p32(b + 30, 0);
        p32(b + 34, 3);
        p16(b + 38, 1);
        b[40] = 2;
        p32(b + 41, 0);
        p32(b + 45, 3);
        p16(b + 49, 2);
        b[51] = 2;
        b[2384] = '.';
        b[2392] = '.';
        b[2393] = '.';
        if (wput(w, (uint64_t)(g.main_ba + g.cseg[0] * F2_SEGSZ) * 4096,
                 b, 4096) != 0)
            goto out;
    }

    rc = 0;

out:
    free(b);
    free(cp);
    return rc;
}

int ssvm_mkfs(const char *file, const char *fstype, const char *label,
              uint64_t datalen)
{
    struct wr w;
    int rc;

    if (getenv(SSVM_NO_MKFS_ENV)) {
        fprintf(stderr, "W: %s is set — .sbv created WITHOUT a "
                        "filesystem (test mode)\n", SSVM_NO_MKFS_ENV);
        return 0;
    }
    if (!fstype || !*fstype)
        fstype = SSVM_FSTYPE_DEFAULT;
    if (!ssvm_valid_fstype(fstype)) {
        errno = EINVAL;
        return -1;
    }

    w.fd = open(file, O_RDWR | O_CLOEXEC);
    if (w.fd < 0)
        return -1;
    w.base = SBV_HDR_SIZE;

    if (strcmp(fstype, "ext2") == 0)
        rc = mkfs_ext(&w, 2, label, datalen);
    else if (strcmp(fstype, "ext3") == 0)
        rc = mkfs_ext(&w, 3, label, datalen);
    else if (strcmp(fstype, "ext4") == 0)
        rc = mkfs_ext(&w, 4, label, datalen);
    else if (strcmp(fstype, "xfs") == 0)
        rc = mkfs_xfs(&w, label, datalen);
    else
        rc = mkfs_f2fs(&w, label, datalen);

    if (rc == 0 && fsync(w.fd) != 0)
        rc = -1;
    close(w.fd);
    return rc;
}
