#include "ssvm.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <linux/fs.h>
#include <linux/loop.h>

void ssvm_die(int code, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "E: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(code);
}

void ssvm_sysdie(int code, const char *fmt, ...)
{
    int e = errno;
    va_list ap;
    fprintf(stderr, "E: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ": %s (errno %d)\n", strerror(e), e);
    exit(code);
}

void ssvm_warn(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "W: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " (%s)\n", strerror(errno));
}

const char *ssvm_dir(void)
{
    const char *d = getenv(SSVM_DIR_ENV);
    if (d && *d)
        return d;
    return SSVM_DIR_DEFAULT;
}

void ssvm_require_root(void)
{
    if (getenv(SSVM_DIR_ENV))
        return;
    if (geteuid() != 0)
        ssvm_die(EXIT_SYSTEM, "this operation requires root (run with sudo)");
}

static int join_lens(size_t total, size_t n)
{
    if (total + 1 > n) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int ssvm_path_join(char *out, size_t n, const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);

    if (join_lens(la + lb + 1, n) != 0)
        return -1;
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb + 1);
    return 0;
}

int ssvm_path_join3(char *out, size_t n, const char *a, const char *b,
                    const char *c)
{
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);

    if (join_lens(la + lb + lc + 2, n) != 0)
        return -1;
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb);
    out[la + 1 + lb] = '/';
    memcpy(out + la + lb + 2, c, lc + 1);
    return 0;
}

int ssvm_strcpy(char *dst, size_t n, const char *src)
{
    size_t l = strlen(src);

    if (join_lens(l, n) != 0)
        return -1;
    memcpy(dst, src, l + 1);
    return 0;
}

int ssvm_valid_name(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len > SSVM_NAME_MAX)
        return 0;
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0)
        return 0;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.')
            return 0;
    return 1;
}

int ssvm_valid_tag(const char *s)
{
    return ssvm_valid_name(s) && strlen(s) < SSVM_TAG_MAX;
}

int ssvm_parse_lvpath(const char *s, char *vg, char *tp, char *lv)
{
    char buf[SSVM_PATHSTR_MAX];
    char *parts[5], *save = NULL;
    int n = 0;

    if (strlen(s) >= sizeof buf)
        return -1;
    strcpy(buf, s);

    for (char *tok = strtok_r(buf, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (n >= 4)
            return -1;
        parts[n++] = tok;
    }
    if (n < 2 || n > 4)
        return -1;
    for (int i = 0; i < n; i++)
        if (!ssvm_valid_name(parts[i]))
            return -1;

    strcpy(vg, parts[0]);
    if (n >= 3) {
        strcpy(tp, parts[1]);
        strcpy(lv, parts[2]);
    } else {
        tp[0] = '\0';
        strcpy(lv, parts[1]);
    }
    return 0;
}

int ssvm_valid_mountpoint(const char *mp)
{
    size_t len = strlen(mp);

    if (len < 1 || len >= SSVM_MPT_MAX)
        return 0;
    if (mp[0] != '/')
        return 0;
    if (strchr(mp, '\\') || strchr(mp, '\n') || strchr(mp, '\t'))
        return 0;

    int comps = 0;
    char *first = NULL;
    char buf[SSVM_MPT_MAX];
    strcpy(buf, mp);
    char *save = NULL;
    for (char *tok = strtok_r(buf, "/", &save); tok;
         tok = strtok_r(NULL, "/", &save)) {
        if (!first)
            first = tok;
        comps++;
        if (strcmp(tok, "..") == 0)
            return 0;
    }
    if (comps < 1)
        return 0;

    if (len > 1 && mp[len - 1] == '/')
        return 0;

    if (comps == 1) {
        static const char *sysdirs[] = {
            "bin", "boot", "dev", "etc", "lib", "lib32", "lib64",
            "libx32", "proc", "root", "run", "sbin", "sys", "usr",
            "var", NULL
        };
        for (int i = 0; sysdirs[i]; i++)
            if (strcmp(first, sysdirs[i]) == 0)
                return 0;
    }
    return 1;
}

int ssvm_mp_covers(const char *mp, const char *path)
{
    size_t ml = strlen(mp), pl = strlen(path);

    if (ml == 0 || ml > pl)
        return 0;
    if (memcmp(mp, path, ml) != 0)
        return 0;
    return ml == pl || path[ml] == '/';
}

static int read_first_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    size_t l;

    if (!f)
        return -1;
    if (!fgets(buf, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    l = strlen(buf);
    while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
        buf[--l] = '\0';
    return 0;
}

static void unescape_dashes(char *dst, size_t n, const char *src, size_t len)
{
    size_t i = 0, o = 0;

    while (i < len && o + 1 < n) {
        if (src[i] == '-' && i + 1 < len && src[i + 1] == '-') {
            dst[o++] = '-';
            i += 2;
        } else {
            dst[o++] = src[i++];
        }
    }
    dst[o] = '\0';
}

static int lvm_pool_of(const char *dir, const char *vg, const char *lv,
                       char *pool, size_t n)
{
    char path[SSVM_MPT_MAX + SSVM_NAME_MAX + 8];
    char line[1024];
    FILE *f;
    int depth = 0, in_lvs = 0, in_target = 0, found = 0, ispool = 0;

    pool[0] = '\0';
    if (snprintf(path, sizeof path, "%s/%s", dir, vg) >= (int)sizeof path)
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        size_t l;

        while (*s == ' ' || *s == '\t')
            s++;
        l = strlen(s);
        while (l > 0 && (s[l - 1] == '\n' || s[l - 1] == '\r' ||
                         s[l - 1] == ' ' || s[l - 1] == '\t'))
            s[--l] = '\0';
        if (!l || *s == '#')
            continue;
        if (s[l - 1] == '{') {
            s[l - 1] = '\0';
            l--;
            while (l > 0 && (s[l - 1] == ' ' || s[l - 1] == '\t'))
                s[--l] = '\0';
            depth++;
            if (depth == 2 && strcmp(s, "logical_volumes") == 0)
                in_lvs = 1;
            else if (depth == 3 && in_lvs && strcmp(s, lv) == 0)
                in_target = 1;
            continue;
        }
        if (strcmp(s, "}") == 0) {
            if (depth == 3)
                in_target = 0;
            if (depth == 2)
                in_lvs = 0;
            if (depth > 0)
                depth--;
            continue;
        }
        if (in_target && depth >= 3) {
            char v[SSVM_NAME_MAX];
            if (sscanf(s, "pool = \"%63[^\"]\"", v) == 1 ||
                sscanf(s, "thin_pool = \"%63[^\"]\"", v) == 1) {
                snprintf(pool, n, "%s", v);
                found = 1;
            } else if (sscanf(s, "type = \"%63[^\"]\"", v) == 1 &&
                       strcmp(v, "thin-pool") == 0) {
                ispool = 1;
            }
        }
    }
    fclose(f);
    if (ispool) {
        pool[0] = '\0';
        return -1;
    }
    return found ? 0 : -1;
}

int ssvm_lvm_detect(const char *refdir, char *vg, char *tp, char *lv)
{
    char dev[32];
    char uuid[128];
    char name[256];
    char p[SSVM_MPT_MAX + 128];
    const char *env;

    env = getenv(SSVM_LVM_DEV_ENV);
    if (env && *env) {
        snprintf(dev, sizeof dev, "%s", env);
    } else {
        char cur[SSVM_MPT_MAX + 8];
        struct stat st;

        snprintf(cur, sizeof cur, "%s", refdir ? refdir : "/");
        for (;;) {
            if (stat(cur, &st) == 0)
                break;
            {
                char *slash = strrchr(cur, '/');
                if (!slash || slash == cur) {
                    if (stat("/", &st) != 0)
                        return -1;
                    snprintf(cur, sizeof cur, "/");
                    break;
                }
                *slash = '\0';
            }
        }
        snprintf(dev, sizeof dev, "%u:%u", major(st.st_dev), minor(st.st_dev));
    }

    env = getenv(SSVM_LVM_SYSFS_ENV);
    if (!env || !*env)
        env = "/sys";
    if (snprintf(p, sizeof p, "%s/dev/block/%s/dm/uuid", env, dev) >=
        (int)sizeof p)
        return -1;
    if (read_first_line(p, uuid, sizeof uuid) != 0)
        return -1;
    if (strncmp(uuid, "LVM-", 4) != 0)
        return -1;
    if (snprintf(p, sizeof p, "%s/dev/block/%s/dm/name", env, dev) >=
        (int)sizeof p)
        return -1;
    if (read_first_line(p, name, sizeof name) != 0 || !name[0])
        return -1;

    {
        size_t i = 0;

        while (name[i]) {
            if (name[i] == '-') {
                if (name[i + 1] == '-') {
                    i += 2;
                    continue;
                }
                break;
            }
            i++;
        }
        if (!name[i] || i == 0 || !name[i + 1])
            return -1;
        unescape_dashes(vg, SSVM_NAME_MAX, name, i);
        unescape_dashes(lv, SSVM_NAME_MAX, name + i + 1, strlen(name + i + 1));
    }
    if (!ssvm_valid_name(vg) || !ssvm_valid_name(lv))
        return -1;

    tp[0] = '\0';
    env = getenv(SSVM_LVM_BACKUP_ENV);
    if (!env || !*env)
        env = "/etc/lvm/backup";
    {
        char pool[SSVM_NAME_MAX];

        if (lvm_pool_of(env, vg, lv, pool, sizeof pool) == 0 &&
            pool[0] && strcmp(pool, lv) != 0 && ssvm_valid_name(pool))
            snprintf(tp, SSVM_NAME_MAX, "%s", pool);
    }
    return 0;
}

int ssvm_parse_size(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;
    unsigned long long mult = 1;

    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || s[0] == '-' || s[0] == '+')
        return -1;

    switch (*end) {
    case '\0':
        break;
    case 'k': case 'K': mult = 1ULL << 10; end++; break;
    case 'm': case 'M': mult = 1ULL << 20; end++; break;
    case 'g': case 'G': mult = 1ULL << 30; end++; break;
    case 't': case 'T': mult = 1ULL << 40; end++; break;
    case 'p': case 'P': mult = 1ULL << 50; end++; break;
    default:
        return -1;
    }

    if (end[0] == 'B' && end[1] == '\0')
        end += 1;
    else if (end[0] == 'i' && end[1] == 'B' && end[2] == '\0')
        end += 2;
    if (*end != '\0')
        return -1;

    if (v > UINT64_MAX / mult)
        return -1;
    *out = (uint64_t)v * mult;
    return 0;
}

void ssvm_format_size(uint64_t bytes, char *buf, size_t n)
{
    static const char *unit[] = { "B", "K", "M", "G", "T", "P", "E" };
    double v = (double)bytes;
    int i = 0;

    while (v >= 1024.0 && i < 6) {
        v /= 1024.0;
        i++;
    }
    if (i == 0)
        snprintf(buf, n, "%llu B", (unsigned long long)bytes);
    else if (v >= 100.0)
        snprintf(buf, n, "%.0f%s", v, unit[i]);
    else if (v >= 10.0)
        snprintf(buf, n, "%.1f%s", v, unit[i]);
    else
        snprintf(buf, n, "%.2f%s", v, unit[i]);
}

int64_t ssvm_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

void ssvm_fmt_time(int64_t t, char *buf, size_t n)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
}

void ssvm_uuid(char out[SSVM_UUID_LEN])
{
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    ssize_t got = -1;

    if (fd >= 0) {
        got = read(fd, b, sizeof b);
        close(fd);
    }
    if (got != (ssize_t)sizeof b) {

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t x = (uint64_t)ts.tv_sec * 1000000000ull ^ (uint64_t)ts.tv_nsec
                     ^ ((uint64_t)getpid() << 32);
        for (size_t i = 0; i < sizeof b; i++) {
            b[i] = (unsigned char)(x & 0xff);
            x = x * 6364136223846793005ull + 1442695040888963407ull;
        }
    }
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);

    snprintf(out, SSVM_UUID_LEN,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void xml_escape(FILE *f, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&':  fputs("&amp;",  f); break;
        case '<':  fputs("&lt;",   f); break;
        case '>':  fputs("&gt;",   f); break;
        case '"':  fputs("&quot;", f); break;
        case '\'': fputs("&apos;", f); break;
        default:   fputc((unsigned char)*s, f);
        }
    }
}

static int xml_unescape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) {
        if (o + 1 >= n)
            return -1;
        if (in[i] == '&') {
            if (strncmp(in + i, "&amp;", 5) == 0)  { out[o++] = '&';  i += 4; }
            else if (strncmp(in + i, "&lt;", 4) == 0)   { out[o++] = '<';  i += 3; }
            else if (strncmp(in + i, "&gt;", 4) == 0)   { out[o++] = '>';  i += 3; }
            else if (strncmp(in + i, "&quot;", 6) == 0) { out[o++] = '"';  i += 5; }
            else if (strncmp(in + i, "&apos;", 6) == 0) { out[o++] = '\''; i += 5; }
            else return -1;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
    return 0;
}

int ssvm_index_save(const struct ssvm_index *ix)
{
    char path[SSVM_MPT_MAX + 64], tmp[SSVM_MPT_MAX + 80];
    FILE *f;

    if (ssvm_path_join(path, sizeof path, ssvm_dir(), SSVM_INDEX_NAME))
        ssvm_die(EXIT_SYSTEM, "index path too long");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    f = fopen(tmp, "w");
    if (!f)
        ssvm_sysdie(EXIT_SYSTEM, "cannot create %s", tmp);

    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", f);
    fputs("<ssvm version=\"1\">\n", f);

    for (size_t i = 0; i < ix->n; i++) {
        const struct ssvm_subvol *sv = &ix->v[i];
        fprintf(f, "  <subvolume path=\"%s\" uuid=\"%s\" size=\"%llu\" "
                   "fs=\"%s\" "
                   "mountpoint=\"", sv->path, sv->uuid,
                (unsigned long long)sv->size,
                sv->fstype[0] ? sv->fstype : SSVM_FSTYPE_DEFAULT);
        xml_escape(f, sv->mountpoint);
        fprintf(f, "\" created=\"%lld\"", (long long)sv->created);
        if (sv->pendrb[0]) {
            fputs(" pendrb=\"", f);
            xml_escape(f, sv->pendrb);
            fputc('"', f);
        }
        fputs(">\n", f);
        for (int t = 0; t < sv->ntags; t++) {
            fprintf(f, "    <tag name=\"%s\"/>\n", sv->tags[t]);
        }
        for (int s = 0; s < sv->nsnap; s++) {
            const struct ssvm_snap *sn = &sv->snaps[s];
            fprintf(f, "    <snapshot name=\"%s\" uuid=\"%s\" parent=\"%s\" "
                       "size=\"%llu\" created=\"%lld\"/>\n",
                    sn->name, sn->uuid, sn->parent,
                    (unsigned long long)sn->size, (long long)sn->created);
        }
        fputs("  </subvolume>\n", f);
    }
    fputs("</ssvm>\n", f);

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        ssvm_sysdie(EXIT_SYSTEM, "error writing %s", tmp);
    }
    fclose(f);

    ssvm_chattr_i(path, 0);
    if (rename(tmp, path) != 0)
        ssvm_sysdie(EXIT_SYSTEM, "error renaming %s to %s", tmp, path);
    ssvm_fsync_dir(ssvm_dir());
    return 0;
}

struct xattr {
    char name[32];
    char value[SSVM_MPT_MAX];
};

struct xtag {
    char name[32];
    struct xattr attrs[12];
    int nattrs;
    int closing;
    int selfclose;
};

static const char *x_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

static const char *x_next(const char *p, struct xtag *t)
{
    const char *q;

    p = x_skip_ws(p);
    if (*p == '\0')
        return NULL;
    if (*p != '<')
        return NULL;
    p++;

    if (strncmp(p, "!--", 3) == 0) {
        q = strstr(p + 3, "-->");
        if (!q)
            return NULL;
        return x_next(q + 3, t);
    }
    if (strncmp(p, "?", 1) == 0) {
        q = strstr(p, "?>");
        if (!q)
            return NULL;
        return x_next(q + 2, t);
    }

    memset(t, 0, sizeof *t);
    if (*p == '/') {
        t->closing = 1;
        p++;
    }

    q = p;
    while (isalnum((unsigned char)*q) || *q == '_' || *q == '-')
        q++;
    if (q == p || (size_t)(q - p) >= sizeof t->name)
        return NULL;
    memcpy(t->name, p, (size_t)(q - p));
    p = q;

    for (;;) {
        p = x_skip_ws(p);
        if (*p == '\0')
            return NULL;
        if (*p == '>') {
            p++;
            break;
        }
        if (*p == '/' && p[1] == '>') {
            t->selfclose = 1;
            p += 2;
            break;
        }
        if (t->nattrs >= 12)
            return NULL;

        q = p;
        while (isalnum((unsigned char)*q) || *q == '_' || *q == '-')
            q++;
        if (q == p || (size_t)(q - p) >= sizeof t->attrs[0].name)
            return NULL;
        memcpy(t->attrs[t->nattrs].name, p, (size_t)(q - p));
        p = x_skip_ws(q);
        if (*p != '=')
            return NULL;
        p = x_skip_ws(p + 1);
        if (*p != '"')
            return NULL;
        p++;

        q = p;
        while (*q && *q != '"')
            q++;
        if (*q != '"')
            return NULL;
        {
            char raw[SSVM_MPT_MAX * 6];
            size_t len = (size_t)(q - p);
            if (len >= sizeof raw)
                return NULL;
            memcpy(raw, p, len);
            raw[len] = '\0';
            if (xml_unescape(raw, t->attrs[t->nattrs].value,
                             sizeof t->attrs[0].value) != 0)
                return NULL;
        }
        t->nattrs++;
        p = q + 1;
    }
    return p;
}

static const char *x_attr(const struct xtag *t, const char *name)
{
    for (int i = 0; i < t->nattrs; i++)
        if (strcmp(t->attrs[i].name, name) == 0)
            return t->attrs[i].value;
    return NULL;
}

static int x_attr_u64(const struct xtag *t, const char *name, uint64_t *out)
{
    const char *v = x_attr(t, name);
    char *end;
    if (!v || !*v)
        return -1;
    errno = 0;
    *out = strtoull(v, &end, 10);
    if (errno != 0 || *end != '\0')
        return -1;
    return 0;
}

static int x_attr_i64(const struct xtag *t, const char *name, int64_t *out)
{
    uint64_t u;
    if (x_attr_u64(t, name, &u) != 0)
        return -1;
    if (u > (uint64_t)INT64_MAX)
        return -1;
    *out = (int64_t)u;
    return 0;
}

static int x_attr_str(const struct xtag *t, const char *name,
                      char *out, size_t n)
{
    const char *v = x_attr(t, name);
    if (!v)
        return -1;
    if (strlen(v) >= n)
        return -1;
    strcpy(out, v);
    return 0;
}

int ssvm_index_load(struct ssvm_index *ix)
{
    char path[SSVM_MPT_MAX + 64];
    FILE *f;
    char *buf;
    long sz;
    const char *p;
    struct xtag t;

    memset(ix, 0, sizeof *ix);
    if (ssvm_path_join(path, sizeof path, ssvm_dir(), SSVM_INDEX_NAME) != 0)
        ssvm_die(EXIT_SYSTEM, "index path too long");

    f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT)
            return 0;
        ssvm_sysdie(EXIT_SYSTEM, "cannot open %s", path);
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz < 0 || sz > 32 * 1024 * 1024) {
        fclose(f);
        ssvm_die(EXIT_SYSTEM, "%s: invalid size", path);
    }
    rewind(f);
    buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        ssvm_die(EXIT_SYSTEM, "out of memory");
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        ssvm_die(EXIT_SYSTEM, "error reading %s", path);
    }
    fclose(f);
    buf[sz] = '\0';

    p = x_next(buf, &t);
    if (!p || t.closing || strcmp(t.name, "ssvm") != 0 || t.selfclose)
        goto corrupt;

    while ((p = x_next(p, &t)) != NULL) {
        if (t.closing && strcmp(t.name, "ssvm") == 0) {
            if (*x_skip_ws(p) != '\0')
                goto corrupt;
            free(buf);
            return 0;
        }
        if (t.closing || strcmp(t.name, "subvolume") != 0)
            goto corrupt;

        {
            struct ssvm_subvol sv;
            memset(&sv, 0, sizeof sv);
            if (x_attr_str(&t, "path", sv.path, sizeof sv.path) != 0 ||
                x_attr_str(&t, "uuid", sv.uuid, sizeof sv.uuid) != 0 ||
                x_attr_u64(&t, "size", &sv.size) != 0 ||
                x_attr_i64(&t, "created", &sv.created) != 0)
                goto corrupt;
            {
                const char *fs = x_attr(&t, "fs");
                if (fs && *fs) {
                    if (!ssvm_valid_fstype(fs) || strlen(fs) >= sizeof sv.fstype)
                        goto corrupt;
                    strcpy(sv.fstype, fs);
                } else {
                    strcpy(sv.fstype, SSVM_FSTYPE_DEFAULT);
                }
            }
            {
                const char *mp = x_attr(&t, "mountpoint");
                if (mp && *mp) {
                    if (strlen(mp) >= sizeof sv.mountpoint)
                        goto corrupt;
                    strcpy(sv.mountpoint, mp);
                }
            }
            {
                const char *rb = x_attr(&t, "pendrb");
                if (rb && *rb) {
                    if (strlen(rb) >= sizeof sv.pendrb)
                        goto corrupt;
                    strcpy(sv.pendrb, rb);
                }
            }
            if (ssvm_index_find(ix, sv.path) != NULL)
                goto corrupt;
            if (ix->n == ix->cap) {
                size_t nc = ix->cap ? ix->cap * 2 : 8;
                struct ssvm_subvol *nv = realloc(ix->v, nc * sizeof *nv);
                if (!nv)
                    ssvm_die(EXIT_SYSTEM, "out of memory");
                ix->v = nv;
                ix->cap = nc;
            }
            ix->v[ix->n] = sv;
            ix->n++;

            struct ssvm_subvol *cur = &ix->v[ix->n - 1];

            if (t.selfclose)
                continue;

            while ((p = x_next(p, &t)) != NULL) {
                if (t.closing && strcmp(t.name, "subvolume") == 0)
                    break;
                if (t.closing)
                    goto corrupt;
                if (strcmp(t.name, "tag") == 0 && t.selfclose) {
                    char tag[SSVM_TAG_MAX];
                    if (x_attr_str(&t, "name", tag, sizeof tag) != 0 ||
                        cur->ntags >= SSVM_MAX_TAGS)
                        goto corrupt;
                    strcpy(cur->tags[cur->ntags++], tag);
                } else if (strcmp(t.name, "snapshot") == 0 && t.selfclose) {
                    if (cur->nsnap >= 4096)
                        goto corrupt;
                    struct ssvm_snap *sn = realloc(cur->snaps,
                        (size_t)(cur->nsnap + 1) * sizeof *sn);
                    if (!sn)
                        ssvm_die(EXIT_SYSTEM, "out of memory");
                    cur->snaps = sn;
                    struct ssvm_snap *ns = &cur->snaps[cur->nsnap];
                    memset(ns, 0, sizeof *ns);
                    if (x_attr_str(&t, "name", ns->name, sizeof ns->name) != 0 ||
                        x_attr_str(&t, "uuid", ns->uuid, sizeof ns->uuid) != 0 ||
                        x_attr_str(&t, "parent", ns->parent, sizeof ns->parent) != 0 ||
                        x_attr_u64(&t, "size", &ns->size) != 0 ||
                        x_attr_i64(&t, "created", &ns->created) != 0)
                        goto corrupt;
                    cur->nsnap++;
                } else {
                    goto corrupt;
                }
            }
            if (!p)
                goto corrupt;
        }
    }
    goto corrupt;

corrupt:
    free(buf);
    ssvm_index_free(ix);
    ssvm_die(EXIT_SYSTEM, "%s is corrupted (remove the leftovers with "
             "svremove or delete the file)", path);
}

void ssvm_index_free(struct ssvm_index *ix)
{
    for (size_t i = 0; i < ix->n; i++)
        free(ix->v[i].snaps);
    free(ix->v);
    memset(ix, 0, sizeof *ix);
}

struct ssvm_subvol *ssvm_index_find(struct ssvm_index *ix, const char *path)
{
    for (size_t i = 0; i < ix->n; i++)
        if (strcmp(ix->v[i].path, path) == 0)
            return &ix->v[i];
    return NULL;
}

struct ssvm_subvol *ssvm_index_resolve(struct ssvm_index *ix, const char *ref,
                                       int *nmatch)
{
    struct ssvm_subvol *found = NULL;
    int c = 0;

    *nmatch = 0;
    if (strchr(ref, '/')) {
        *nmatch = 1;
        return ssvm_index_find(ix, ref);
    }
    for (size_t i = 0; i < ix->n; i++) {
        const char *last = strrchr(ix->v[i].path, '/');
        last = last ? last + 1 : ix->v[i].path;
        if (strcmp(last, ref) == 0) {
            found = &ix->v[i];
            c++;
        }
    }
    *nmatch = c;
    return c == 1 ? found : NULL;
}

static void snap_strip_suffix(char *s)
{
    size_t l = strlen(s);

    if (l > 5 && strcmp(s + l - 5, ".sbvs") == 0)
        s[l - 5] = '\0';
}

struct ssvm_subvol *ssvm_snap_find(struct ssvm_index *ix, const char *ref,
                                   int *sidx, int *nmatch)
{
    struct ssvm_subvol *found = NULL;
    char want[SSVM_SNAPNAME_MAX];
    size_t l = strlen(ref);
    int c = 0;

    if (sidx)
        *sidx = -1;
    *nmatch = 0;
    if (l == 0 || l >= sizeof want)
        return NULL;
    memcpy(want, ref, l + 1);
    snap_strip_suffix(want);
    if (!want[0])
        return NULL;

    for (size_t i = 0; i < ix->n; i++) {
        for (int s = 0; s < ix->v[i].nsnap; s++) {
            char nm[SSVM_SNAPNAME_MAX];
            size_t sl = strlen(ix->v[i].snaps[s].name);

            if (sl == 0 || sl >= sizeof nm)
                continue;
            memcpy(nm, ix->v[i].snaps[s].name, sl + 1);
            snap_strip_suffix(nm);
            if (strcmp(nm, want) == 0) {
                if (!found) {
                    found = &ix->v[i];
                    if (sidx)
                        *sidx = s;
                }
                c++;
            }
        }
    }
    *nmatch = c;
    return found;
}

int ssvm_index_lock(void)
{
    char path[SSVM_MPT_MAX + 64];
    int fd;

    if (ssvm_ensure_dir(ssvm_dir(), 0755) != 0)
        ssvm_sysdie(EXIT_SYSTEM, "cannot create SSVM root directory %s "
                    "(check permissions, or set %s)", ssvm_dir(),
                    SSVM_DIR_ENV);

    if (ssvm_path_join(path, sizeof path, ssvm_dir(), SSVM_LOCK_NAME) != 0)
        ssvm_die(EXIT_SYSTEM, "lock file path too long");

    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0 && (errno == EACCES || errno == EPERM)) {
        ssvm_chattr_i(path, 0);
        ssvm_chattr_i(ssvm_dir(), 0);
        fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    }
    if (fd < 0)
        ssvm_sysdie(EXIT_SYSTEM, "cannot open lock file %s "
                    "(immutable? try: chattr -i %s)", path, ssvm_dir());

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr, "W: waiting for the index lock %s "
                    "(another ssvm command is running)...\n", path);
            if (flock(fd, LOCK_EX) != 0) {
                close(fd);
                ssvm_sysdie(EXIT_SYSTEM, "cannot lock %s", path);
            }
        } else {
            close(fd);
            ssvm_sysdie(EXIT_SYSTEM, "cannot lock %s", path);
        }
    }
    return fd;
}

void ssvm_index_unlock(int fd)
{
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

void ssvm_sbv_dir(const struct ssvm_subvol *sv, char *out, size_t n)
{
    if (ssvm_path_join(out, n, ssvm_dir(), sv->path) != 0)
        ssvm_die(EXIT_SYSTEM, "path too long: %s", sv->path);
}

void ssvm_sbv_file(const struct ssvm_subvol *sv, char *out, size_t n)
{
    char dir[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 8];
    char base[SSVM_PATHSTR_MAX + 8];
    const char *lc = strrchr(sv->path, '/');

    snprintf(base, sizeof base, "%s.sbv", lc ? lc + 1 : sv->path);
    ssvm_sbv_dir(sv, dir, sizeof dir);
    if (ssvm_path_join(out, n, dir, base) != 0)
        ssvm_die(EXIT_SYSTEM, "path too long: %s", sv->path);
}

void ssvm_default_mp(const struct ssvm_subvol *sv, char *out, size_t n)
{
    char dir[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 8];

    ssvm_sbv_dir(sv, dir, sizeof dir);
    if (ssvm_path_join(out, n, dir, "mnt") != 0)
        ssvm_die(EXIT_SYSTEM, "path too long: %s", sv->path);
}

void ssvm_mp_of(const struct ssvm_subvol *sv, char *out, size_t n)
{
    if (sv->mountpoint[0])
        snprintf(out, n, "%s", sv->mountpoint);
    else
        ssvm_default_mp(sv, out, n);
}

#define OFF_MAGIC     0
#define OFF_VERSION   8
#define OFF_HDRSIZE   12
#define OFF_CHUNKLOG2 16
#define OFF_FLAGS     20
#define OFF_VSIZE     24
#define OFF_CREATED   32
#define OFF_SNAPCNT   40
#define OFF_UUID      48
#define OFF_PARENT    85
#define OFF_LABEL     122
#define OFF_FSTYPE    372
#define HDR_TOTAL     512

static void wr32(unsigned char *b, uint32_t v)
{
    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)((v >> 8) & 0xff);
    b[2] = (unsigned char)((v >> 16) & 0xff);
    b[3] = (unsigned char)((v >> 24) & 0xff);
}

static void wr64(unsigned char *b, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        b[i] = (unsigned char)((v >> (8 * i)) & 0xff);
}

static uint32_t rd32(const unsigned char *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t rd64(const unsigned char *b)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | b[i];
    return v;
}

static void wr_str(unsigned char *b, size_t off, size_t max, const char *s)
{
    size_t l = 0;

    memset(b + off, 0, max);
    if (s) {
        l = strlen(s);
        if (l >= max)
            l = max - 1;
        memcpy(b + off, s, l);
    }
}

static void rd_str(const unsigned char *b, size_t off, size_t max,
                   char *out, size_t outn)
{
    size_t l = 0;

    while (l + 1 < max && b[off + l])
        l++;
    if (l + 1 > outn)
        l = outn - 1;
    memcpy(out, b + off, l);
    out[l] = '\0';
}

void ssvm_sbv_init_hdr(struct sbv_hdr *h, uint64_t vsize, const char *label,
                       const char *fstype)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, SBV_MAGIC, 8);
    h->version = SBV_VERSION;
    h->hdr_size = SBV_HDR_SIZE;
    h->chunk_log2 = 22;
    h->flags = SBV_FLAG_SEALED;
    h->vsize = vsize;
    h->created = (uint64_t)ssvm_now();
    h->snapcount = 0;
    ssvm_uuid(h->uuid);
    h->parent[0] = '\0';
    snprintf(h->label, sizeof h->label, "%s", label);
    snprintf(h->fstype, sizeof h->fstype, "%s",
             (fstype && *fstype) ? fstype : SSVM_FSTYPE_DEFAULT);
}

int ssvm_sbv_write_hdr(const char *file, const struct sbv_hdr *h)
{
    unsigned char b[HDR_TOTAL];
    int fd = open(file, O_WRONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;
    memset(b, 0, sizeof b);
    memcpy(b + OFF_MAGIC, SBV_MAGIC, 8);
    wr32(b + OFF_VERSION, h->version);
    wr32(b + OFF_HDRSIZE, h->hdr_size);
    wr32(b + OFF_CHUNKLOG2, h->chunk_log2);
    wr32(b + OFF_FLAGS, h->flags);
    wr64(b + OFF_VSIZE, h->vsize);
    wr64(b + OFF_CREATED, h->created);
    wr64(b + OFF_SNAPCNT, h->snapcount);
    wr_str(b, OFF_UUID, 37, h->uuid);
    wr_str(b, OFF_PARENT, 37, h->parent);
    wr_str(b, OFF_LABEL, 128, h->label);
    wr_str(b, OFF_FSTYPE, SSVM_FSTYPE_MAX,
           h->fstype[0] ? h->fstype : SSVM_FSTYPE_DEFAULT);

    ssize_t w = pwrite(fd, b, sizeof b, 0);
    int rc = (w == (ssize_t)sizeof b) ? 0 : -1;
    if (rc == 0 && fsync(fd) != 0)
        rc = -1;
    close(fd);
    return rc;
}

int ssvm_sbv_read_hdr(const char *file, struct sbv_hdr *h)
{
    unsigned char b[HDR_TOTAL];
    int fd = open(file, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;
    ssize_t r = pread(fd, b, sizeof b, 0);
    close(fd);
    if (r != (ssize_t)sizeof b)
        return -1;

    memset(h, 0, sizeof *h);
    memcpy(h->magic, b + OFF_MAGIC, 8);
    h->magic[8] = '\0';
    if (strcmp(h->magic, SBV_MAGIC) != 0)
        return -1;
    h->version = rd32(b + OFF_VERSION);
    h->hdr_size = rd32(b + OFF_HDRSIZE);
    h->chunk_log2 = rd32(b + OFF_CHUNKLOG2);
    h->flags = rd32(b + OFF_FLAGS);
    h->vsize = rd64(b + OFF_VSIZE);
    h->created = rd64(b + OFF_CREATED);
    h->snapcount = rd64(b + OFF_SNAPCNT);
    rd_str(b, OFF_UUID, 37, h->uuid, sizeof h->uuid);
    rd_str(b, OFF_PARENT, 37, h->parent, sizeof h->parent);
    rd_str(b, OFF_LABEL, 128, h->label, sizeof h->label);
    rd_str(b, OFF_FSTYPE, SSVM_FSTYPE_MAX, h->fstype, sizeof h->fstype);
    if (!h->fstype[0] || !ssvm_valid_fstype(h->fstype))
        snprintf(h->fstype, sizeof h->fstype, "%s", SSVM_FSTYPE_DEFAULT);
    return 0;
}

int ssvm_chattr_i(const char *path, int on)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    long flags;

    if (fd < 0)
        return -1;
    if (ioctl(fd, FS_IOC_GETFLAGS, &flags) < 0) {
        close(fd);
        return -1;
    }
    if (on)
        flags |= FS_IMMUTABLE_FL;
    else
        flags &= ~(long)FS_IMMUTABLE_FL;
    if (ioctl(fd, FS_IOC_SETFLAGS, &flags) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void unescape_mnt(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < n; i++) {
        if (in[i] == '\\' && in[i + 1] && in[i + 2] && in[i + 3]) {
            char oct[4] = { in[i + 1], in[i + 2], in[i + 3], '\0' };
            char *end;
            long v = strtol(oct, &end, 8);
            if (end != oct && *end == '\0' && v >= 0 && v < 256) {
                out[o++] = (char)v;
                i += 3;
                continue;
            }
        }
        out[o++] = in[i];
    }
    out[o] = '\0';
}

int ssvm_find_mount(const char *mp, char *src, size_t n)
{
    const char *mf = getenv(SSVM_MOUNTS_ENV);
    FILE *f = fopen((mf && *mf) ? mf : "/proc/mounts", "r");
    char line[4096];

    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char s[1024], m[2048], fst[64];
        char mdec[2048];
        int got;

        got = sscanf(line, "%1023s %2047s %63s", s, m, fst);
        if (got < 3)
            continue;
        unescape_mnt(m, mdec, sizeof mdec);
        if (strcmp(mdec, mp) == 0 && strncmp(s, "/dev/loop", 9) == 0) {
            fclose(f);
            snprintf(src, n, "%s", s);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

int ssvm_is_active(const struct ssvm_subvol *sv, char *src, size_t n)
{
    char mp[SSVM_MPT_MAX + 8];
    ssvm_mp_of(sv, mp, sizeof mp);
    return ssvm_find_mount(mp, src ? src : (char *)"", src ? n : 0);
}

int ssvm_loop_attach(const char *file, uint64_t offset, uint64_t sizelimit,
                     char *name, size_t n)
{
    char dev[64];
    int lfd = -1, ffd, idx = -1;
    int lc = open("/dev/loop-control", O_RDWR | O_CLOEXEC);

    if (lc >= 0) {
        idx = ioctl(lc, LOOP_CTL_GET_FREE);
        close(lc);
    }
    if (idx >= 0) {
        snprintf(dev, sizeof dev, "/dev/loop%d", idx);
        lfd = open(dev, O_RDWR | O_CLOEXEC);
    }
    if (lfd < 0) {

        for (int i = 0; i < 64; i++) {
            char cand[64];
            struct loop_info64 st;
            snprintf(cand, sizeof cand, "/dev/loop%d", i);
            lfd = open(cand, O_RDWR | O_CLOEXEC);
            if (lfd < 0)
                continue;
            if (ioctl(lfd, LOOP_GET_STATUS64, &st) < 0 && errno == ENXIO) {
                snprintf(dev, sizeof dev, "%s", cand);
                break;
            }
            close(lfd);
            lfd = -1;
        }
    }
    if (lfd < 0) {
        errno = ENODEV;
        return -1;
    }

    ffd = open(file, O_RDWR | O_CLOEXEC);
    if (ffd < 0) {
        close(lfd);
        return -1;
    }

#ifdef LOOP_CONFIGURE
    {
        struct loop_config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.fd = ffd;
        cfg.block_size = 4096;
        cfg.info.lo_offset = (off_t)offset;
        cfg.info.lo_sizelimit = (off_t)sizelimit;
        cfg.info.lo_flags = LO_FLAGS_AUTOCLEAR;
        if (ioctl(lfd, LOOP_CONFIGURE, &cfg) == 0)
            goto attached;

    }
#endif
    {
        struct loop_info64 li;
        if (ioctl(lfd, LOOP_SET_FD, (void *)(intptr_t)ffd) < 0) {
            close(ffd);
            close(lfd);
            return -1;
        }
        memset(&li, 0, sizeof li);
        li.lo_offset = (off_t)offset;
        li.lo_sizelimit = (off_t)sizelimit;
        li.lo_flags = LO_FLAGS_AUTOCLEAR;
        if (ioctl(lfd, LOOP_SET_STATUS64, &li) < 0) {
            ioctl(lfd, LOOP_CLR_FD, 0);
            close(ffd);
            close(lfd);
            return -1;
        }
    }

attached:
    close(ffd);
    snprintf(name, n, "%s", dev);
    return lfd;
}

int ssvm_loop_detach(const char *loopdev)
{
    int fd = open(loopdev, O_RDWR | O_CLOEXEC);
    int rc;

    if (fd < 0)
        return -1;
    rc = ioctl(fd, LOOP_CLR_FD, 0);
    close(fd);
    return rc;
}

int ssvm_do_mount(const char *src, const char *mp, const char *fstype)
{
    return mount(src, mp, (fstype && *fstype) ? fstype : SSVM_FSTYPE_DEFAULT,
                 0, NULL);
}

int ssvm_do_umount(const char *mp)
{
    if (umount2(mp, 0) == 0)
        return 0;
    if (errno == EAGAIN)
        return umount2(mp, MNT_DETACH);
    return -1;
}

int ssvm_ensure_dir(const char *path, mode_t mode)
{
    char buf[SSVM_MPT_MAX + 8];
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof buf)
        return -1;
    strcpy(buf, path);

    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, mode) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(buf, mode) != 0 && errno != EEXIST)
        return -1;

    struct stat st;
    if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

int ssvm_sparse_copy(const char *src, const char *dst, uint64_t *copied)
{
    int in = -1, out = -1, rc = -1;
    off_t size, data = 0;
    unsigned char *buf = NULL;
    uint64_t total = 0;

    in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0)
        goto out;

    size = lseek(in, 0, SEEK_END);
    if (size < 0)
        goto out;
    if (ftruncate(out, size) != 0)
        goto out;

    buf = malloc(SBV_CHUNK_SIZE);
    if (!buf)
        goto out;

    {
        int fullcopy = 0;
        while (data < size) {
            off_t d, h, pos;
            d = lseek(in, data, SEEK_DATA);
            if (d < 0) {
                if (errno == ENXIO)
                    break;
                if (errno == EINVAL || errno == ENOTSUP || errno == EOPNOTSUPP) {

                    fullcopy = 1;
                    d = 0;
                    h = size;
                    if (lseek(in, 0, SEEK_SET) < 0)
                        goto out;
                } else {
                    goto out;
                }
            } else {
                h = lseek(in, d, SEEK_HOLE);
                if (h < 0)
                    h = size;
            }

            pos = d;
            while (pos < h) {
                size_t want = (size_t)((h - pos) > (off_t)SBV_CHUNK_SIZE
                                           ? SBV_CHUNK_SIZE : (h - pos));
                ssize_t r = pread(in, buf, want, pos);
                ssize_t w;
                if (r <= 0)
                    goto out;
                w = pwrite(out, buf, (size_t)r, pos);
                if (w != r)
                    goto out;
                pos += r;
                total += (uint64_t)r;
            }
            if (fullcopy)
                break;
            data = h;
        }
    }
    if (fsync(out) != 0)
        goto out;
    rc = 0;

out:
    free(buf);
    if (out >= 0)
        close(out);
    if (in >= 0)
        close(in);
    if (rc != 0)
        unlink(dst);
    if (copied)
        *copied = total;
    return rc;
}

uint64_t ssvm_free_bytes(const char *dir)
{
    struct statvfs st;
    if (statvfs(dir, &st) != 0)
        return 0;
    return (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
}

uint64_t ssvm_used_bytes(const char *file)
{
    struct stat st;
    if (stat(file, &st) != 0)
        return 0;
    return (uint64_t)st.st_blocks * 512ull;
}

int ssvm_confirm(const char *what)
{
    char line[128];

    if (!isatty(0)) {
        fprintf(stderr, "E: confirmation required, but stdin is not a "
                        "terminal (use -f to force)\n");
        exit(EXIT_USAGE);
    }
    printf("Remove %s? [y/N] ", what);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin))
        return 0;
    if (line[0] == 'y' || line[0] == 'Y')
        return 1;
    return 0;
}

int ssvm_fsync_dir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int rc;

    if (fd < 0)
        return -1;
    rc = fsync(fd);
    close(fd);
    return rc;
}

int ssvm_rm_rf(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);

    DIR *d = opendir(path);
    if (!d)
        return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[SSVM_MPT_MAX + SSVM_SNAPNAME_MAX + 8];
        if (snprintf(child, sizeof child, "%s/%s", path, e->d_name)
                >= (int)sizeof child)
            continue;
        if (ssvm_rm_rf(child) != 0)
            rc = -1;
    }
    closedir(d);
    if (rmdir(path) != 0)
        rc = -1;
    return rc;
}
