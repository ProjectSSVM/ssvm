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
"Usage: svchange -ay [-m PATH] vg/lv        activate\n"
"       svchange -an vg/lv                  deactivate\n"
"       svchange -s [-n NAME] vg/lv         snapshot\n"
"       svchange -r SNAPSHOT vg/lv          rollback     (SSVM "
    SSVM_VERSION ")\n"
"\n"
"  -ay          activate: attaches a loop device to the .sbv (data region\n"
"               after the 1 MiB header) and mounts it at the mountpoint\n"
"  -an          deactivate: unmounts, releases the loop and seals with +i\n"
"  -s           snapshot: sparse copy (allocated blocks only) of the\n"
"               current .sbv to NAME.sbvs — requires an INACTIVE subvolume\n"
"  -n NAME      snapshot name (default: lv--YYYYMMDD-HHMMSS)\n"
"  -r SNAP      rollback: replaces the .sbv image with the contents of\n"
"               snapshot SNAP (which is preserved). With the subvolume\n"
"               ACTIVE, the on-disk image is swapped immediately, but the\n"
"               current mount keeps running on the old image — volatile:\n"
"               new data is lost. The rollback only applies the restored\n"
"               image after a remount:\n"
"                 svchange -an vg/lv && svchange -ay vg/lv\n"
"               (lsbv shows \"pending rollback\" until that happens)\n"
"  -m PATH      with -ay: sets and persists a custom mountpoint\n"
"  -h, --help   this help\n"
"\n"
"Exit codes: 0 ok · 1 invalid usage · 2 not found · 3 state conflict\n"
"            · 4 system error\n",
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

static const struct ssvm_snap *find_snap(const struct ssvm_subvol *sv,
                                         const char *arg, const char *lv)
{
    char u[SSVM_SNAPNAME_MAX];
    char want[SSVM_SNAPNAME_MAX + SSVM_NAME_MAX + 4];
    size_t l = strlen(arg);

    if (l == 0 || l >= sizeof u)
        return NULL;
    memcpy(u, arg, l + 1);
    if (l > 5 && strcmp(u + l - 5, ".sbvs") == 0)
        u[l - 5] = '\0';

    if (snprintf(want, sizeof want, "%s--%s", lv, u) >= (int)sizeof want)
        return NULL;

    for (int i = 0; i < sv->nsnap; i++) {
        char s[SSVM_SNAPNAME_MAX];
        size_t sl = strlen(sv->snaps[i].name);

        if (sl >= sizeof s)
            continue;
        memcpy(s, sv->snaps[i].name, sl + 1);
        if (sl > 5 && strcmp(s + sl - 5, ".sbvs") == 0)
            s[sl - 5] = '\0';
        if (strcmp(s, u) == 0 || strcmp(s, want) == 0)
            return &sv->snaps[i];
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int opt, action = 0;
    const char *mpnew = NULL, *snapname = NULL, *rbarg = NULL;
    char path[SSVM_PATHSTR_MAX];
    struct option lo[] = {
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    struct ssvm_index ix;
    struct ssvm_subvol *sv;
    char dir[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 8];
    char file[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + SSVM_NAME_MAX + 16];
    char mp[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 8];
    char src[128];
    struct sbv_hdr hdr;
    int lockfd;

    ssvm_set_prog(argv[0]);
    opterr = 0;
    while ((opt = getopt_long(argc, argv, "a:sn:r:m:h", lo, NULL)) != -1) {
        switch (opt) {
        case 'a':
            if (strcmp(optarg, "y") != 0 && strcmp(optarg, "n") != 0)
                ssvm_die(EXIT_USAGE, "-a expects 'y' or 'n' (use -ay or -an)");
            if (action)
                ssvm_die(EXIT_USAGE, "use only one action per invocation");
            action = optarg[0];
            break;
        case 's':
            if (action)
                ssvm_die(EXIT_USAGE, "use only one action per invocation");
            action = 's';
            break;
        case 'r':
            if (action)
                ssvm_die(EXIT_USAGE, "use only one action per invocation");
            action = 'r';
            rbarg = optarg;
            break;
        case 'n':
            snapname = optarg;
            break;
        case 'm':
            mpnew = optarg;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return EXIT_USAGE;
        }
    }
    if (!action || optind + 1 != argc) {
        usage(stderr);
        return EXIT_USAGE;
    }
    if (action != 'y' && mpnew)
        ssvm_die(EXIT_USAGE, "-m is only used with -ay");
    if (action != 's' && snapname)
        ssvm_die(EXIT_USAGE, "-n is only used with -s");
    if (mpnew && !ssvm_valid_mountpoint(mpnew))
        ssvm_die(EXIT_USAGE, "invalid mountpoint: %s", mpnew);

    snprintf(path, sizeof path, "%s", argv[optind]);
    {
        char vg[SSVM_PATHSTR_MAX], tp[SSVM_PATHSTR_MAX], lv[SSVM_PATHSTR_MAX];
        if (ssvm_parse_lvpath(path, vg, tp, lv) != 0)
            ssvm_die(EXIT_USAGE, "invalid path: %s", path);
    }

    ssvm_require_root();

    lockfd = ssvm_index_lock();
    if (ssvm_index_load(&ix) != 0) {
        ssvm_index_unlock(lockfd);
        return EXIT_SYSTEM;
    }
    sv = ssvm_index_find(&ix, path);
    if (!sv) {
        ssvm_index_unlock(lockfd);
        ssvm_die(EXIT_NOENT, "no such subvolume: %s", path);
    }

    ssvm_sbv_dir(sv, dir, sizeof dir);
    ssvm_sbv_file(sv, file, sizeof file);

    switch (action) {

    case 'y': {
        int lfd;
        char loopdev[64];

        if (ssvm_is_active(sv, src, sizeof src)) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_CONFLICT, "%s is already active (mounted at %s)",
                     path, src);
        }

        if (mpnew)
            snprintf(mp, sizeof mp, "%s", mpnew);
        else
            ssvm_mp_of(sv, mp, sizeof mp);

        if (mpnew) {
            char defmp[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 8];
            ssvm_default_mp(sv, defmp, sizeof defmp);
            if (strcmp(mpnew, defmp) == 0)
                mpnew = NULL;
        }

        {
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
                if (&ix.v[i] == sv)
                    continue;
                ssvm_sbv_dir(&ix.v[i], od, sizeof od);
                if (ssvm_mp_covers(mp, od)) {
                    ssvm_index_unlock(lockfd);
                    ssvm_die(EXIT_CONFLICT, "mountpoint %s covers the data "
                             "directory of %s", mp, ix.v[i].path);
                }
            }
        }

        ssvm_chattr_i(file, 0);
        ssvm_chattr_i(dir, 0);
        for (int s = 0; s < sv->nsnap; s++) {
            char f[SSVM_MPT_MAX + SSVM_SNAPNAME_MAX + 8];
            ssvm_path_join(f, sizeof f, dir, sv->snaps[s].name);
            ssvm_chattr_i(f, 0);
        }
        ssvm_chattr_i(mp, 0);

        if (ssvm_ensure_dir(mp, 0755) != 0) {
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "cannot prepare %s", mp);
        }

        if (ssvm_sbv_read_hdr(file, &hdr) != 0) {
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "cannot read the header of %s", file);
        }
        if (sv->fstype[0] && hdr.fstype[0] &&
            strcmp(sv->fstype, hdr.fstype) != 0) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_SYSTEM, "filesystem mismatch: index says %s, "
                     "%s header says %s", sv->fstype, file, hdr.fstype);
        }

        lfd = ssvm_loop_attach(file, SBV_HDR_SIZE, sv->size,
                               loopdev, sizeof loopdev);
        if (lfd < 0) {
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "cannot attach a loop device to %s "
                        "(loop devices available? /dev/loop-control?)", file);
        }

        if (hdr.flags & SBV_FLAG_SEALED) {
            hdr.flags &= ~(uint32_t)SBV_FLAG_SEALED;
            ssvm_sbv_write_hdr(file, &hdr);
        }

        if (ssvm_do_mount(loopdev, mp,
                          sv->fstype[0] ? sv->fstype : SSVM_FSTYPE_DEFAULT) != 0) {
            int err = errno;
            close(lfd);
            ssvm_loop_detach(loopdev);
            errno = err;
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "mount of %s on %s failed", loopdev, mp);
        }
        close(lfd);

        {
            char rbname[SSVM_SNAPNAME_MAX];
            int had_rb = sv->pendrb[0] != '\0';

            snprintf(rbname, sizeof rbname, "%s", sv->pendrb);
            if (mpnew)
                snprintf(sv->mountpoint, sizeof sv->mountpoint, "%s", mpnew);
            if (had_rb)
                sv->pendrb[0] = '\0';
            if (mpnew || had_rb) {
                if (ssvm_index_save(&ix) != 0) {
                    ssvm_index_unlock(lockfd);
                    return EXIT_SYSTEM;
                }
            }
            ssvm_index_unlock(lockfd);
            printf("activated: %s\n  %s on %s (%s)\n", path, loopdev, mp,
                   sv->mountpoint[0] ? "custom mountpoint"
                                     : "default mountpoint");
            if (had_rb)
                printf("  rollback applied: image restored from %s now in "
                       "use (remount complete)\n", rbname);
        }
        break;
    }

    case 'n': {
        ssvm_mp_of(sv, mp, sizeof mp);

        if (!ssvm_find_mount(mp, src, sizeof src)) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_CONFLICT, "%s is already inactive", path);
        }
        if (ssvm_do_umount(mp) != 0) {
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "cannot unmount %s (files in use?)", mp);
        }
        ssvm_loop_detach(src);

        if (ssvm_sbv_read_hdr(file, &hdr) == 0) {
            hdr.flags |= SBV_FLAG_SEALED;
            ssvm_sbv_write_hdr(file, &hdr);
        }

        if (ssvm_chattr_i(file, 1) != 0)
            ssvm_warn("cannot seal %s (chattr +i)", file);
        if (ssvm_chattr_i(dir, 1) != 0)
            ssvm_warn("cannot seal %s (chattr +i)", dir);
        for (int s = 0; s < sv->nsnap; s++) {
            char f[SSVM_MPT_MAX + SSVM_SNAPNAME_MAX + 8];
            ssvm_path_join(f, sizeof f, dir, sv->snaps[s].name);
            if (ssvm_chattr_i(f, 1) != 0)
                ssvm_warn("cannot seal %s", f);
        }
        if (ssvm_chattr_i(mp, 1) != 0)
            ssvm_warn("cannot seal %s (chattr +i)", mp);

        ssvm_index_unlock(lockfd);
        printf("deactivated: %s\n  %s sealed (+i)\n", path, dir);
        if (sv->pendrb[0])
            printf("  pending rollback (%s): the restored image will be "
                   "mounted on the next activation\n", sv->pendrb);
        break;
    }

    case 's': {
        char name[SSVM_SNAPNAME_MAX + 32];
        char snapfile[SSVM_MPT_MAX + SSVM_SNAPNAME_MAX + 8];
        uint64_t copied = 0;
        char used[32];
        const char *lvbase = strrchr(path, '/') + 1;

        ssvm_mp_of(sv, mp, sizeof mp);
        if (ssvm_find_mount(mp, src, sizeof src)) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_CONFLICT, "%s is active — deactivate before "
                     "taking a snapshot (svchange -an)", path);
        }
        if (ssvm_sbv_read_hdr(file, &hdr) != 0) {
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "cannot read the header of %s", file);
        }

        {
            char base[SSVM_SNAPNAME_MAX];

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
            } else {
                snap_default_name(lvbase, base, sizeof base);
            }

            for (int suffix = 0;; suffix++) {
                struct stat st;
                if (suffix == 0)
                    snprintf(name, sizeof name, "%s.sbvs", base);
                else
                    snprintf(name, sizeof name, "%s-%d.sbvs", base, suffix + 1);
                if (ssvm_path_join(snapfile, sizeof snapfile, dir, name) != 0) {
                    ssvm_index_unlock(lockfd);
                    ssvm_die(EXIT_SYSTEM, "path too long");
                }
                if (lstat(snapfile, &st) != 0 && errno == ENOENT)
                    break;
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
            ssvm_sysdie(EXIT_SYSTEM, "failed copying %s to %s", file, snapfile);
        }

        {
            struct sbv_hdr sh = hdr;
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
                            sizeof sv->snaps[sv->nsnap].name, name) != 0) {
                unlink(snapfile);
                ssvm_chattr_i(dir, 1);
                ssvm_index_unlock(lockfd);
                ssvm_die(EXIT_SYSTEM, "snapshot name too long");
            }
            {
                struct sbv_hdr sh;
                if (ssvm_sbv_read_hdr(snapfile, &sh) == 0)
                    snprintf(sv->snaps[sv->nsnap].uuid,
                             sizeof sv->snaps[sv->nsnap].uuid, "%s", sh.uuid);
            }
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
        ssvm_format_size(copied, used, sizeof used);
        printf("snapshot created: %s\n  %s (%s copied — allocated blocks "
               "only)\n", path, name, used);
        break;
    }

    case 'r': {
        char snapfile[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + SSVM_SNAPNAME_MAX + 8];
        char tmpfile[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + SSVM_SNAPNAME_MAX + 8];
        const struct ssvm_snap *sn;
        struct sbv_hdr sh, nh;
        struct stat st;
        uint64_t copied = 0;
        char used[32];
        const char *lvbase = strrchr(path, '/') + 1;
        int active;

        sn = find_snap(sv, rbarg, lvbase);
        if (!sn) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_NOENT, "snapshot not found in %s: %s "
                     "(lsbv -l %s lists the snapshots)", path, rbarg, path);
        }
        if (ssvm_path_join(snapfile, sizeof snapfile, dir, sn->name) != 0 ||
            ssvm_path_join(tmpfile, sizeof tmpfile, dir, ".rollback.tmp") != 0) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_SYSTEM, "path too long");
        }
        if (lstat(snapfile, &st) != 0) {
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "snapshot file not found: %s", snapfile);
        }
        if (ssvm_sbv_read_hdr(snapfile, &sh) != 0) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_SYSTEM, "corrupted snapshot (invalid header): %s",
                     snapfile);
        }

        ssvm_mp_of(sv, mp, sizeof mp);
        active = ssvm_is_active(sv, src, sizeof src);

        if (active) {

            size_t dl = strlen(dir), ml = strlen(mp);

            if (strcmp(mp, dir) == 0 ||
                (dl > ml && strncmp(dir, mp, ml) == 0 && dir[ml] == '/')) {
                ssvm_index_unlock(lockfd);
                ssvm_die(EXIT_CONFLICT, "mountpoint %s covers the data "
                         "directory — deactivate before the rollback "
                         "(or reactivate with a -m outside of it)", mp);
            }
        } else {
            ssvm_chattr_i(dir, 0);
            ssvm_chattr_i(file, 0);
        }

        unlink(tmpfile);

        if (ssvm_sparse_copy(snapfile, tmpfile, &copied) != 0) {
            unlink(tmpfile);
            if (!active) {
                ssvm_chattr_i(file, 1);
                ssvm_chattr_i(dir, 1);
            }
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "failed copying %s to %s", snapfile,
                        tmpfile);
        }

        nh = sh;
        snprintf(nh.uuid, sizeof nh.uuid, "%s",
                 sv->uuid[0] ? sv->uuid : sh.uuid);
        nh.parent[0] = '\0';
        nh.snapcount = (uint64_t)sv->nsnap;
        if (active)
            nh.flags &= ~(uint32_t)SBV_FLAG_SEALED;
        else
            nh.flags |= SBV_FLAG_SEALED;
        if (ssvm_sbv_write_hdr(tmpfile, &nh) != 0) {
            unlink(tmpfile);
            if (!active) {
                ssvm_chattr_i(file, 1);
                ssvm_chattr_i(dir, 1);
            }
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "error writing the header of %s", tmpfile);
        }

        if (rename(tmpfile, file) != 0) {
            unlink(tmpfile);
            if (!active) {
                ssvm_chattr_i(file, 1);
                ssvm_chattr_i(dir, 1);
            }
            ssvm_index_unlock(lockfd);
            ssvm_sysdie(EXIT_SYSTEM, "failed replacing %s", file);
        }
        ssvm_fsync_dir(dir);

        if (!active) {
            if (ssvm_chattr_i(file, 1) != 0)
                ssvm_warn("cannot seal %s (chattr +i)", file);
            if (ssvm_chattr_i(dir, 1) != 0)
                ssvm_warn("cannot seal %s (chattr +i)", dir);
            for (int s = 0; s < sv->nsnap; s++) {
                char f[SSVM_MPT_MAX + SSVM_SNAPNAME_MAX + 8];
                ssvm_path_join(f, sizeof f, dir, sv->snaps[s].name);
                if (ssvm_chattr_i(f, 1) != 0)
                    ssvm_warn("cannot seal %s", f);
            }
        }

        snprintf(sv->pendrb, sizeof sv->pendrb, "%s", sn->name);
        if (ssvm_index_save(&ix) != 0) {
            ssvm_index_unlock(lockfd);
            return EXIT_SYSTEM;
        }
        ssvm_index_unlock(lockfd);
        ssvm_format_size(copied, used, sizeof used);

        if (active) {
            printf("rollback registered: %s <- %s (%s copied)\n", path,
                   sn->name, used);
            fflush(stdout);
            fprintf(stderr, "%s: WARNING: %s is MOUNTED — the on-disk image "
                    "has been replaced, but the current mount keeps\n"
                    "  running on the old image, which is now VOLATILE: "
                    "data written from now on is NOT persisted and will "
                    "be lost.\n"
                    "  The rollback applies the restored image only on "
                    "remount:\n"
                    "    svchange -an %s && svchange -ay %s\n",
                    ssvm_prog(), path, path, path);
        } else {
            printf("rollback applied: %s <- %s (%s copied)\n", path,
                   sn->name, used);
            printf("  restored content becomes visible on activation "
                   "(svchange -ay %s)\n", path);
        }
        break;
    }

    default:
        usage(stderr);
        ssvm_index_unlock(lockfd);
        return EXIT_USAGE;
    }

    ssvm_index_free(&ix);
    return 0;
}
