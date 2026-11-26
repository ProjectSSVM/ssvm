#include "ssvm.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(FILE *out)
{
    fputs(
"Usage: svcreate [-L SIZE] [-f FSTYPE] [-m MOUNTPOINT] [--tag TAG]... NAME\n"
"       svcreate -s [-n SNAPNAME] SUBVOL                 (SSVM "
    SSVM_VERSION ")\n"
"\n"
"Creates a subvolume named NAME: a sparse .sbv image in a custom format\n"
"(1 MiB header + filesystem data region) registered in index.xml.\n"
"With -s it takes a snapshot of an existing subvolume instead.\n"
"\n"
"  NAME        plain subvolume name — the vg/[thin-pool/]lv prefix of\n"
"              its path is detected automatically from the real LVM\n"
"              layout where the mountpoint lives (or where the ssvm\n"
"              directory lives, when -m is not given)\n"
"  -L SIZE     virtual size (e.g. 512M, 20G, 1T). Without -L, uses all\n"
"              the free space available at creation time (thin\n"
"              provisioning)\n"
"  -f FSTYPE   filesystem in the data region: ext2, ext3, ext4, xfs or\n"
"              f2fs. Default: the filesystem of the host — the one\n"
"              detected on the ssvm directory — or ext4 when it is not\n"
"              one of the supported types (minimum size: 16M for ext*,\n"
"              64M for xfs and f2fs)\n"
"  -m PATH     custom mountpoint: any absolute path, even a single\n"
"              component such as /a (system directories like /etc are\n"
"              refused). LVM detection follows this path. Default: the\n"
"              mnt/ subdirectory inside the subvolume\n"
"  --tag TAG   attaches a tag (repeatable)\n"
"  -s          snapshot: sparse copy (allocated blocks only) of the\n"
"              current .sbv of SUBVOL to SNAPNAME.sbvs — requires an\n"
"              INACTIVE subvolume (consistent image)\n"
"  -n SNAPNAME snapshot name (default: NAME--YYYYMMDD-HHMMSS), unique\n"
"              among all the registered snapshots\n"
"  SUBVOL      the snapshot origin: subvolume path\n"
"              (vg/[thin-pool/]lv/name) or just its name, when unique\n"
"  -h, --help  this help\n"
"\n"
"LVM is mandatory when creating a subvolume: when no LVM logical volume\n"
"backs the reference path, svcreate fails with\n"
"  E: LVM Usage is Mandatory on the mountpoint.\n"
"\n"
"Exit codes: 0 ok · 1 invalid usage · 2 not found · 3 already exists or\n"
"state conflict · 4 system error\n",
    out);
}

static void snap_default_name(const char *lv, char *out, size_t n)
{
    char ts[32];
    time_t t = (time_t)ssvm_now();
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
    snprintf(out, n, "%s--%s", lv, ts);
}

int main(int argc, char **argv)
{
    int opt, lockfd, ntags = 0, fd, nm;
    int snapmode = 0, have_size = 0, fs_from_host = 0;
    uint64_t size = 0;
    const char *mp = NULL;
    const char *fstype = NULL;
    const char *snapname = NULL;
    char tags[SSVM_MAX_TAGS][SSVM_TAG_MAX];
    char path[SSVM_PATHSTR_MAX];
    char prefix[SSVM_PATHSTR_MAX];
    char name[SSVM_NAME_MAX + 1];
    struct option lo[] = {
        { "tag",  required_argument, NULL, 'T' },
        { "fs",   required_argument, NULL, 'f' },
        { "help", no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    char vg[SSVM_NAME_MAX], tp[SSVM_NAME_MAX], lv[SSVM_NAME_MAX];
    const char *lvmref;
    struct ssvm_index ix;
    struct ssvm_subvol *sv;
    struct sbv_hdr hdr;
    char dir[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 8];
    char file[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + SSVM_NAME_MAX + 16];
    char sz[32];

    opterr = 0;
    while ((opt = getopt_long(argc, argv, "L:m:f:sn:h", lo, NULL)) != -1) {
        switch (opt) {
        case 'L':
            if (ssvm_parse_size(optarg, &size) != 0)
                ssvm_die(EXIT_USAGE, "invalid size: %s "
                         "(use 512M, 20G, 1T...)", optarg);
            have_size = 1;
            break;
        case 's':
            snapmode = 1;
            break;
        case 'n':
            snapname = optarg;
            break;
        case 'm':
            mp = optarg;
            break;
        case 'f':
            if (!ssvm_valid_fstype(optarg))
                ssvm_die(EXIT_USAGE, "invalid filesystem type: %s "
                         "(use ext2, ext3, ext4, xfs or f2fs)", optarg);
            fstype = optarg;
            break;
        case 'T':
            if (!ssvm_valid_tag(optarg))
                ssvm_die(EXIT_USAGE, "invalid tag: %s", optarg);
            if (ntags >= SSVM_MAX_TAGS)
                ssvm_die(EXIT_USAGE, "tag limit of %d reached", SSVM_MAX_TAGS);
            for (int i = 0; i < ntags; i++)
                if (strcmp(tags[i], optarg) == 0)
                    ssvm_die(EXIT_USAGE, "duplicate tag: %s", optarg);
            strcpy(tags[ntags++], optarg);
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return EXIT_USAGE;
        }
    }
    if (optind + 1 != argc) {
        usage(stderr);
        return EXIT_USAGE;
    }

    if (snapmode) {
        if (have_size)
            ssvm_die(EXIT_USAGE, "-L is not used with -s");
        if (fstype)
            ssvm_die(EXIT_USAGE, "-f is not used with -s");
        if (mp)
            ssvm_die(EXIT_USAGE, "-m is not used with -s");
        if (ntags > 0)
            ssvm_die(EXIT_USAGE, "--tag is not used with -s");
        snprintf(path, sizeof path, "%s", argv[optind]);
        if (strchr(path, '/')) {
            char svg[SSVM_PATHSTR_MAX], stp[SSVM_PATHSTR_MAX];
            char slv[SSVM_PATHSTR_MAX];
            if (ssvm_parse_lvpath(path, svg, stp, slv) != 0)
                ssvm_die(EXIT_USAGE, "invalid path: %s "
                         "(vg/[tp/]lv/name or a plain unique name)", path);
        } else if (!ssvm_valid_name(path)) {
            ssvm_die(EXIT_USAGE, "invalid name: %s", path);
        }
    } else {
        if (snapname)
            ssvm_die(EXIT_USAGE, "-n is only used with -s");
        if (strchr(argv[optind], '/'))
            ssvm_die(EXIT_USAGE, "pass a plain NAME — the vg/[thin-pool/]lv "
                     "prefix is auto-detected from the mountpoint: %s",
                     argv[optind]);
        if (!ssvm_valid_name(argv[optind]))
            ssvm_die(EXIT_USAGE, "invalid name: %s", argv[optind]);
        snprintf(name, sizeof name, "%s", argv[optind]);
        if (mp && !ssvm_valid_mountpoint(mp))
            ssvm_die(EXIT_USAGE, "invalid mountpoint: %s "
                     "(absolute path, no '..', not a system directory)",
                     mp ? mp : "(null)");
    }

    ssvm_require_root();

    lockfd = ssvm_index_lock();
    if (ssvm_index_load(&ix) != 0) {
        ssvm_index_unlock(lockfd);
        return EXIT_SYSTEM;
    }

    if (snapmode) {
        char svmp[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 8];
        char snapfile[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + SSVM_SNAPNAME_MAX + 8];
        char src[128];
        char base[SSVM_SNAPNAME_MAX];
        char sname[SSVM_SNAPNAME_MAX + 32];
        const char *lvbase;
        uint64_t copied = 0;
        struct stat st;
        struct sbv_hdr sh;

        sv = ssvm_index_resolve(&ix, path, &nm);
        if (!sv) {
            ssvm_index_unlock(lockfd);
            if (nm > 1)
                ssvm_die(EXIT_USAGE, "ambiguous name: %s — use the full path",
                         path);
            ssvm_die(EXIT_NOENT, "no such subvolume: %s", path);
        }
        snprintf(path, sizeof path, "%s", sv->path);

        ssvm_sbv_dir(sv, dir, sizeof dir);
        ssvm_sbv_file(sv, file, sizeof file);
        ssvm_mp_of(sv, svmp, sizeof svmp);
        if (ssvm_find_mount(svmp, src, sizeof src)) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_CONFLICT, "%s is active — deactivate before "
                     "taking a snapshot (svchange -an)", path);
        }
        if (ssvm_sbv_read_hdr(file, &hdr) != 0) {
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "cannot read the header of %s", file);
        }
        lvbase = strrchr(path, '/') + 1;

        if (snapname) {
            size_t l = strlen(snapname);

            if (l >= sizeof base)
                ssvm_die(EXIT_USAGE, "snapshot name too long");
            if (l > 5 && strcmp(snapname + l - 5, ".sbvs") == 0)
                l -= 5;
            memcpy(base, snapname, l);
            base[l] = '\0';
            if (!ssvm_valid_name(base))
                ssvm_die(EXIT_USAGE, "invalid snapshot name: %s", base);
            {
                struct ssvm_subvol *ow;
                int osi, onm;

                ow = ssvm_snap_find(&ix, base, &osi, &onm);
                if (ow) {
                    ssvm_index_unlock(lockfd);
                    ssvm_die(EXIT_CONFLICT, "snapshot name already in use: "
                             "%s (on %s)", base, ow->path);
                }
            }
            snprintf(sname, sizeof sname, "%s.sbvs", base);
            if (ssvm_path_join(snapfile, sizeof snapfile, dir, sname) != 0) {
                ssvm_index_unlock(lockfd);
                ssvm_die(EXIT_SYSTEM, "path too long");
            }
            if (lstat(snapfile, &st) == 0) {
                ssvm_index_unlock(lockfd);
                ssvm_die(EXIT_CONFLICT, "snapshot file already exists: %s",
                         snapfile);
            }
            if (errno != ENOENT) {
                ssvm_index_unlock(lockfd);
                ssvm_sysdie(EXIT_SYSTEM, "error accessing %s", snapfile);
            }
        } else {
            snap_default_name(lvbase, base, sizeof base);
            for (int suffix = 0;; suffix++) {
                struct ssvm_subvol *ow;
                int osi, onm;

                if (suffix == 0)
                    snprintf(sname, sizeof sname, "%s.sbvs", base);
                else
                    snprintf(sname, sizeof sname, "%s-%d.sbvs",
                             base, suffix + 1);
                ow = ssvm_snap_find(&ix, sname, &osi, &onm);
                if (!ow) {
                    if (ssvm_path_join(snapfile, sizeof snapfile, dir,
                                       sname) != 0) {
                        ssvm_index_unlock(lockfd);
                        ssvm_die(EXIT_SYSTEM, "path too long");
                    }
                    if (lstat(snapfile, &st) != 0 && errno == ENOENT)
                        break;
                }
                if (suffix >= 99) {
                    ssvm_index_unlock(lockfd);
                    ssvm_die(EXIT_CONFLICT, "too many snapshots named %s "
                             "already exist", base);
                }
            }
        }

        ssvm_chattr_i(dir, 0);

        if (ssvm_sparse_copy(file, snapfile, &copied) != 0) {
            ssvm_chattr_i(dir, 1);
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "failed copying %s to %s", file,
                        snapfile);
        }

        sh = hdr;
        ssvm_uuid(sh.uuid);
        snprintf(sh.parent, sizeof sh.parent, "%s", hdr.uuid);
        sh.created = (uint64_t)ssvm_now();
        sh.flags |= SBV_FLAG_SEALED;
        if (ssvm_sbv_write_hdr(snapfile, &sh) != 0) {
            unlink(snapfile);
            ssvm_chattr_i(dir, 1);
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "error writing the header of %s",
                        snapfile);
        }

        hdr.snapcount++;
        ssvm_sbv_write_hdr(file, &hdr);

        ssvm_chattr_i(snapfile, 1);
        ssvm_chattr_i(dir, 1);

        {
            struct ssvm_snap *sn = realloc(sv->snaps,
                (size_t)(sv->nsnap + 1) * sizeof *sn);
            if (!sn) {
                unlink(snapfile);
                ssvm_chattr_i(dir, 1);
                ssvm_index_unlock(lockfd);
                ssvm_die(EXIT_SYSTEM, "out of memory");
            }
            sv->snaps = sn;
            memset(&sv->snaps[sv->nsnap], 0, sizeof *sv->snaps);
            if (ssvm_strcpy(sv->snaps[sv->nsnap].name,
                            sizeof sv->snaps[sv->nsnap].name, sname) != 0) {
                unlink(snapfile);
                ssvm_chattr_i(dir, 1);
                ssvm_index_unlock(lockfd);
                ssvm_die(EXIT_SYSTEM, "snapshot name too long");
            }
            snprintf(sv->snaps[sv->nsnap].uuid,
                     sizeof sv->snaps[sv->nsnap].uuid, "%s", sh.uuid);
            snprintf(sv->snaps[sv->nsnap].parent,
                     sizeof sv->snaps[sv->nsnap].parent, "%s", hdr.uuid);
            sv->snaps[sv->nsnap].size = sv->size;
            sv->snaps[sv->nsnap].created = ssvm_now();
            sv->nsnap++;
        }

        if (ssvm_index_save(&ix) != 0) {
            ssvm_index_unlock(lockfd);
            return EXIT_SYSTEM;
        }
        ssvm_index_unlock(lockfd);

        ssvm_format_size(copied, sz, sizeof sz);
        printf("snapshot created: %s\n  %s (%s copied — allocated blocks "
               "only)\n", path, sname, sz);
        printf("merge with: svmerge %.*s\n",
               (int)(strlen(sname) - 5), sname);
        ssvm_index_free(&ix);
        return 0;
    }

    lvmref = mp ? mp : ssvm_dir();
    if (ssvm_lvm_detect(lvmref, vg, tp, lv) != 0) {
        ssvm_index_unlock(lockfd);
        ssvm_die(EXIT_SYSTEM, "LVM Usage is Mandatory on the mountpoint.");
    }
    snprintf(prefix, sizeof prefix, "%s%s%s/%s",
             vg, tp[0] ? "/" : "", tp[0] ? tp : "", lv);
    if (ssvm_path_join(path, sizeof path, prefix, name) != 0) {
        ssvm_index_unlock(lockfd);
        ssvm_die(EXIT_SYSTEM, "path too long");
    }
    if (ssvm_index_find(&ix, path)) {
        ssvm_index_unlock(lockfd);
        ssvm_die(EXIT_CONFLICT, "subvolume already exists: %s", path);
    }

    if (!fstype) {
        fstype = ssvm_host_fstype(ssvm_dir());
        if (fstype)
            fs_from_host = 1;
        else
            fstype = SSVM_FSTYPE_DEFAULT;
    }

    if (!have_size) {
        char root[SSVM_MPT_MAX + 8];
        snprintf(root, sizeof root, "%s", ssvm_dir());
        size = ssvm_free_bytes(root);
        size -= size % SBV_CHUNK_SIZE;
        if (size > SSVM_SIZE_MAX)
            size = SSVM_SIZE_MAX - (SSVM_SIZE_MAX % SBV_CHUNK_SIZE);
        if (size < SSVM_SIZE_MIN)
            ssvm_die(EXIT_SYSTEM, "not enough free space for the 16M minimum");
    } else {
        uint64_t rounded = size + SBV_CHUNK_SIZE - 1;
        rounded -= rounded % SBV_CHUNK_SIZE;
        if (rounded != size) {
            ssvm_format_size(rounded, sz, sizeof sz);
            fprintf(stderr, "W: size rounded up to %s (multiple of 4M)\n", sz);
        }
        size = rounded;
    }
    if (size < SSVM_SIZE_MIN || size > SSVM_SIZE_MAX)
        ssvm_die(EXIT_USAGE, "size out of range (16M to 16T)");
    if (size < ssvm_fs_min_size(fstype)) {
        char min[32];
        ssvm_format_size(ssvm_fs_min_size(fstype), min, sizeof min);
        ssvm_die(EXIT_USAGE, "size below the %s minimum of %s "
                 "(use -f ext4 for a smaller minimum)", fstype, min);
    }

    if (ssvm_path_join(dir, sizeof dir, ssvm_dir(), path) != 0) {
        ssvm_index_unlock(lockfd);
        ssvm_die(EXIT_SYSTEM, "path too long");
    }
    {
        struct stat st;
        if (lstat(dir, &st) == 0) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_CONFLICT, "directory already exists: %s", dir);
        }
        if (errno != ENOENT) {
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "error accessing %s", dir);
        }
    }

    if (mp) {
        char root[SSVM_MPT_MAX + 8];
        char od[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 8];
        snprintf(root, sizeof root, "%s", ssvm_dir());
        if (ssvm_mp_covers(mp, root)) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_CONFLICT, "mountpoint %s covers the ssvm data "
                     "directory %s", mp, root);
        }
        if (ssvm_mp_covers(mp, dir)) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_CONFLICT, "mountpoint %s covers the data "
                     "directory of %s", mp, dir);
        }
        for (size_t i = 0; i < ix.n; i++) {
            ssvm_sbv_dir(&ix.v[i], od, sizeof od);
            if (ssvm_mp_covers(mp, od)) {
                ssvm_index_unlock(lockfd);
                ssvm_die(EXIT_CONFLICT, "mountpoint %s covers the data "
                         "directory of %s", mp, ix.v[i].path);
            }
        }
    }

    {
        char parent[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 8];
        if (ssvm_path_join(parent, sizeof parent, ssvm_dir(), prefix) != 0) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_SYSTEM, "path too long");
        }
        if (ssvm_ensure_dir(parent, 0755) != 0) {
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "cannot create %s", parent);
        }
        if (mkdir(dir, 0700) != 0) {
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "cannot create %s", dir);
        }
        {
            char mntdir[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 16];
            if (ssvm_path_join(mntdir, sizeof mntdir, dir, "mnt") != 0) {
                ssvm_index_unlock(lockfd);
                ssvm_die(EXIT_SYSTEM, "path too long");
            }
            if (mkdir(mntdir, 0755) != 0 && errno != EEXIST) {
                ssvm_index_unlock(lockfd);
                ssvm_sysdie(EXIT_SYSTEM, "cannot create %s", mntdir);
            }
        }
    }

    {
        char base[SSVM_PATHSTR_MAX + 8];
        snprintf(base, sizeof base, "%s.sbv", name);
        if (ssvm_path_join(file, sizeof file, dir, base) != 0) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_SYSTEM, "path too long");
        }
    }
    fd = open(file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        ssvm_index_unlock(lockfd);
        ssvm_sysdie(EXIT_SYSTEM, "cannot create %s", file);
    }
    if (ftruncate(fd, (off_t)SBV_HDR_SIZE + (off_t)size) != 0) {
        unlink(file);
        close(fd);
        ssvm_index_unlock(lockfd);
        ssvm_sysdie(EXIT_SYSTEM, "cannot (sparsely) allocate %s", file);
    }
    close(fd);

    ssvm_sbv_init_hdr(&hdr, size, path, fstype);
    if (ssvm_sbv_write_hdr(file, &hdr) != 0) {
        unlink(file);
        ssvm_index_unlock(lockfd);
        ssvm_sysdie(EXIT_SYSTEM, "error writing the header of %s", file);
    }

    if (ssvm_mkfs(file, fstype, name, size) != 0) {
        ssvm_rm_rf(dir);
        ssvm_index_unlock(lockfd);
        ssvm_sysdie(EXIT_SYSTEM, "failed to format %s as %s", file, fstype);
    }

    {
        char mntdir[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 16];
        if (ssvm_path_join(mntdir, sizeof mntdir, dir, "mnt") == 0)
            ssvm_chattr_i(mntdir, 1);
    }
    if (ssvm_chattr_i(file, 1) != 0)
        ssvm_warn("cannot seal %s (chattr +i)", file);
    if (ssvm_chattr_i(dir, 1) != 0)
        ssvm_warn("cannot seal %s (chattr +i)", dir);

    if (ix.n == ix.cap) {
        size_t nc = ix.cap ? ix.cap * 2 : 8;
        struct ssvm_subvol *nv = realloc(ix.v, nc * sizeof *nv);
        if (!nv) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_SYSTEM, "out of memory");
        }
        ix.v = nv;
        ix.cap = nc;
    }
    sv = &ix.v[ix.n];
    memset(sv, 0, sizeof *sv);
    snprintf(sv->path, sizeof sv->path, "%s", path);
    snprintf(sv->uuid, sizeof sv->uuid, "%s", hdr.uuid);
    sv->size = size;
    snprintf(sv->fstype, sizeof sv->fstype, "%s", fstype);
    sv->created = (int64_t)hdr.created;
    if (mp) {
        char defmp[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 16];
        ssvm_path_join3(defmp, sizeof defmp, ssvm_dir(), path, "mnt");
        if (strcmp(mp, defmp) != 0)
            snprintf(sv->mountpoint, sizeof sv->mountpoint, "%s", mp);
    }
    sv->ntags = ntags;
    for (int i = 0; i < ntags; i++)
        memcpy(sv->tags[i], tags[i], SSVM_TAG_MAX);
    ix.n++;

    if (ssvm_index_save(&ix) != 0) {
        ssvm_index_unlock(lockfd);
        return EXIT_SYSTEM;
    }
    ssvm_index_unlock(lockfd);
    ssvm_index_free(&ix);

    ssvm_format_size(size, sz, sizeof sz);
    printf("created: %s (%s) [%s]%s\n", path, sz, fstype,
           have_size ? "" : " [thin: all free space]");
    printf("lvm: %s (detected on %s)\n", prefix, lvmref);
    if (fs_from_host)
        printf("filesystem: %s (host default)\n", fstype);
    {
        char mpout[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 16];
        ssvm_path_join3(mpout, sizeof mpout, ssvm_dir(), path, "mnt");
        printf("mountpoint: %s%s\n", mp ? mp : mpout,
               mp ? " (custom)" : "");
        printf("file: %s\n", file);
        if (ntags > 0) {
            printf("tags:");
            for (int i = 0; i < ntags; i++)
                printf(" %s", tags[i]);
            printf("\n");
        }
        printf("activate with: svchange -ay %s\n", path);
    }
    return 0;
}
