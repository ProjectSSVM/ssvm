#ifndef SSVM_H
#define SSVM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#define SSVM_VERSION "1.2"

#define SSVM_DIR_ENV     "SSVM_DIR"
#define SSVM_NO_MKFS_ENV "SSVM_NO_MKFS"
#define SSVM_MOUNTS_ENV  "SSVM_MOUNTS"
#define SSVM_DIR_DEFAULT "/var/ssvm"
#define SSVM_INDEX_NAME  "index.xml"
#define SSVM_LOCK_NAME   ".index.lock"

#define SBV_MAGIC        "SSBVCOW1"
#define SBV_VERSION      1u
#define SBV_HDR_SIZE     (1024u * 1024u)
#define SBV_CHUNK_SIZE   (4u * 1024u * 1024u)
#define SBV_FLAG_SEALED  0x1u

#define SSVM_PATHSTR_MAX 256
#define SSVM_NAME_MAX    64
#define SSVM_TAG_MAX     64
#define SSVM_MAX_TAGS    32
#define SSVM_SNAPNAME_MAX 96
#define SSVM_MPT_MAX     512
#define SSVM_UUID_LEN    37
#define SSVM_FSTYPE_MAX  8
#define SSVM_FSTYPE_DEFAULT "ext4"

#define SSVM_SIZE_MIN    (16u * 1024u * 1024u)
#define SSVM_SIZE_MAX    (16384ull * 1024ull * 1024ull * 1024ull)

#define EXIT_OK      0
#define EXIT_USAGE   1
#define EXIT_NOENT   2
#define EXIT_CONFLICT 3
#define EXIT_SYSTEM  4

struct ssvm_snap {
    char     name[SSVM_SNAPNAME_MAX];
    char     uuid[SSVM_UUID_LEN];
    char     parent[SSVM_UUID_LEN];
    uint64_t size;
    int64_t  created;
};

struct ssvm_subvol {
    char     path[SSVM_PATHSTR_MAX];
    char     uuid[SSVM_UUID_LEN];
    uint64_t size;
    char     fstype[SSVM_FSTYPE_MAX];
    char     mountpoint[SSVM_MPT_MAX];
    char     pendrb[SSVM_SNAPNAME_MAX];

    int64_t  created;
    int      ntags;
    char     tags[SSVM_MAX_TAGS][SSVM_TAG_MAX];
    int      nsnap;
    struct ssvm_snap *snaps;
};

struct ssvm_index {
    struct ssvm_subvol *v;
    size_t n, cap;
};

struct sbv_hdr {
    char     magic[9];
    uint32_t version;
    uint32_t hdr_size;
    uint32_t chunk_log2;
    uint32_t flags;
    uint64_t vsize;
    uint64_t created;
    uint64_t snapcount;
    char     uuid[SSVM_UUID_LEN];
    char     parent[SSVM_UUID_LEN];
    char     label[SSVM_PATHSTR_MAX + 8];
    char     fstype[SSVM_FSTYPE_MAX];
};

void ssvm_die(int code, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 2, 3)));
void ssvm_sysdie(int code, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 2, 3)));
void ssvm_warn(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void ssvm_set_prog(const char *argv0);
const char *ssvm_prog(void);

const char *ssvm_dir(void);
void        ssvm_require_root(void);
int         ssvm_path_join(char *out, size_t n, const char *a, const char *b);
int         ssvm_path_join3(char *out, size_t n, const char *a, const char *b,
                            const char *c);
int         ssvm_strcpy(char *dst, size_t n, const char *src);

int   ssvm_valid_name(const char *s);
int   ssvm_valid_tag(const char *s);
int   ssvm_valid_fstype(const char *s);
uint64_t ssvm_fs_min_size(const char *fstype);
const char *ssvm_host_fstype(const char *dir);
int   ssvm_parse_lvpath(const char *s, char *vg, char *tp, char *lv);
int   ssvm_valid_mountpoint(const char *mp);
int   ssvm_mp_covers(const char *mp, const char *path);
int   ssvm_parse_size(const char *s, uint64_t *out);
void  ssvm_format_size(uint64_t bytes, char *buf, size_t n);

int64_t ssvm_now(void);
void    ssvm_fmt_time(int64_t t, char *buf, size_t n);
void    ssvm_uuid(char out[SSVM_UUID_LEN]);

int                 ssvm_index_load(struct ssvm_index *ix);
void                ssvm_index_free(struct ssvm_index *ix);
int                 ssvm_index_save(const struct ssvm_index *ix);
struct ssvm_subvol *ssvm_index_find(struct ssvm_index *ix, const char *path);
int                 ssvm_index_lock(void);
void                ssvm_index_unlock(int fd);

void ssvm_sbv_dir(const struct ssvm_subvol *sv, char *out, size_t n);
void ssvm_sbv_file(const struct ssvm_subvol *sv, char *out, size_t n);
void ssvm_default_mp(const struct ssvm_subvol *sv, char *out, size_t n);
void ssvm_mp_of(const struct ssvm_subvol *sv, char *out, size_t n);

int  ssvm_sbv_read_hdr(const char *file, struct sbv_hdr *h);
int  ssvm_sbv_write_hdr(const char *file, const struct sbv_hdr *h);
void ssvm_sbv_init_hdr(struct sbv_hdr *h, uint64_t vsize, const char *label,
                       const char *fstype);

int  ssvm_chattr_i(const char *path, int on);

int  ssvm_find_mount(const char *mp, char *src, size_t n);
int  ssvm_is_active(const struct ssvm_subvol *sv, char *src, size_t n);

int  ssvm_loop_attach(const char *file, uint64_t offset, uint64_t sizelimit,
                      char *name, size_t n);
int  ssvm_loop_detach(const char *loopdev);
int  ssvm_do_mount(const char *src, const char *mp, const char *fstype);
int  ssvm_do_umount(const char *mp);

int      ssvm_ensure_dir(const char *path, mode_t mode);
int      ssvm_sparse_copy(const char *src, const char *dst, uint64_t *copied);
uint64_t ssvm_free_bytes(const char *dir);
uint64_t ssvm_used_bytes(const char *file);
int      ssvm_mkfs(const char *file, const char *fstype, const char *label,
                   uint64_t datalen);
int      ssvm_confirm(const char *what);
int      ssvm_rm_rf(const char *path);
int      ssvm_fsync_dir(const char *path);

#endif
