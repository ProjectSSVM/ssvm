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
"Usage: svchange -ay [-m PATH] SUBVOL       activate\n"
"       svchange -an SUBVOL                  deactivate     (SSVM "
    SSVM_VERSION ")\n"
"\n"
"  SUBVOL      subvolume path (vg/[thin-pool/]lv/name) or just its\n"
"              name, when unique among the registered subvolumes\n"
"  -ay          activate: attaches a loop device to the .sbv (data region\n"
"               after the 1 MiB header) and mounts it at the mountpoint\n"
"  -an          deactivate: unmounts, releases the loop and seals with +i\n"
"  -m PATH      with -ay: sets and persists a custom mountpoint\n"
"  -h, --help   this help\n"
"\n"
"Snapshots are taken with svcreate -s and merged (rolled back) with\n"
"svmerge.\n"
"\n"
"Exit codes: 0 ok · 1 invalid usage · 2 not found · 3 state conflict\n"
"            · 4 system error\n",
    out);
}

int main(int argc, char **argv)
{
    int opt, action = 0, nm;
    const char *mpnew = NULL;
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

    opterr = 0;
    while ((opt = getopt_long(argc, argv, "a:m:h", lo, NULL)) != -1) {
        switch (opt) {
        case 'a':
            if (strcmp(optarg, "y") != 0 && strcmp(optarg, "n") != 0)
                ssvm_die(EXIT_USAGE, "-a expects 'y' or 'n' (use -ay or -an)");
            if (action)
                ssvm_die(EXIT_USAGE, "use only one action per invocation");
            action = optarg[0];
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
    if (mpnew && !ssvm_valid_mountpoint(mpnew))
        ssvm_die(EXIT_USAGE, "invalid mountpoint: %s", mpnew);

    snprintf(path, sizeof path, "%s", argv[optind]);
    if (strchr(path, '/')) {
        char vg[SSVM_PATHSTR_MAX], tp[SSVM_PATHSTR_MAX], lv[SSVM_PATHSTR_MAX];
        if (ssvm_parse_lvpath(path, vg, tp, lv) != 0)
            ssvm_die(EXIT_USAGE, "invalid path: %s "
                     "(vg/[tp/]lv/name or a plain unique name)", path);
    } else if (!ssvm_valid_name(path)) {
        ssvm_die(EXIT_USAGE, "invalid name: %s", path);
    }

    ssvm_require_root();

    lockfd = ssvm_index_lock();
    if (ssvm_index_load(&ix) != 0) {
        ssvm_index_unlock(lockfd);
        return EXIT_SYSTEM;
    }
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

    default:
        usage(stderr);
        ssvm_index_unlock(lockfd);
        return EXIT_USAGE;
    }

    ssvm_index_free(&ix);
    return 0;
}
