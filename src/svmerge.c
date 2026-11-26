#include "ssvm.h"

#include <errno.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(FILE *out)
{
    fputs(
"Usage: svmerge SNAPSHOT                       merge = rollback\n"
"                                                   (SSVM "
    SSVM_VERSION ")\n"
"\n"
"Merges SNAPSHOT into its subvolume: the .sbv image is replaced with\n"
"the contents of the snapshot (a rollback) and the snapshot is consumed\n"
"(removed), like lvconvert --merge. The subvolume that receives the\n"
"rollback is discovered automatically from index.xml.\n"
"\n"
"  SNAPSHOT    snapshot name, unique among all the registered\n"
"              snapshots (an optional .sbvs suffix is accepted), or\n"
"              the explicit form vg/[thin-pool/]lv/name/SNAPSHOT[.sbvs]\n"
"\n"
"  INACTIVE subvolume: the image is swapped immediately; the restored\n"
"  content shows up on the next activation (svchange -ay).\n"
"\n"
"  ACTIVE subvolume: the on-disk image is also swapped immediately\n"
"  (atomic rename(); the loop device keeps the old inode alive), but\n"
"  nothing changes in the current mount — it keeps running on the old\n"
"  image, which is now VOLATILE: data written from that point on is\n"
"  NOT persisted and disappears when the merge applies. The merge\n"
"  only applies the restored image after a remount:\n"
"    svchange -an SUBVOL && svchange -ay SUBVOL\n"
"  (lsbv shows \"pending rollback\" until that happens)\n"
"\n"
"Exit codes: 0 ok · 1 invalid usage or ambiguous name · 2 not found\n"
"            · 3 state conflict · 4 system error\n",
    out);
}

static int find_snap_in(const struct ssvm_subvol *sv, const char *ref)
{
    char want[SSVM_SNAPNAME_MAX];
    size_t l = strlen(ref);

    if (l == 0 || l >= sizeof want)
        return -1;
    memcpy(want, ref, l + 1);
    if (l > 5 && strcmp(want + l - 5, ".sbvs") == 0)
        want[l - 5] = '\0';
    if (!want[0])
        return -1;

    for (int i = 0; i < sv->nsnap; i++) {
        char nm[SSVM_SNAPNAME_MAX];
        size_t sl = strlen(sv->snaps[i].name);

        if (sl == 0 || sl >= sizeof nm)
            continue;
        memcpy(nm, sv->snaps[i].name, sl + 1);
        if (sl > 5 && strcmp(nm + sl - 5, ".sbvs") == 0)
            nm[sl - 5] = '\0';
        if (strcmp(nm, want) == 0)
            return i;
    }
    return -1;
}

int main(int argc, char **argv)
{
    int opt, lockfd, si = -1, nm = 0;
    char arg[SSVM_PATHSTR_MAX + SSVM_SNAPNAME_MAX + 8];
    char subref[SSVM_PATHSTR_MAX + SSVM_SNAPNAME_MAX + 8];
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
    char snapname[SSVM_SNAPNAME_MAX];
    char snapfile[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + SSVM_SNAPNAME_MAX + 8];
    char tmpfile[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + SSVM_SNAPNAME_MAX + 8];
    struct sbv_hdr sh, nh;
    struct stat st;
    uint64_t copied = 0;
    char used[32];
    int active;

    opterr = 0;
    while ((opt = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
        switch (opt) {
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
    snprintf(arg, sizeof arg, "%s", argv[optind]);

    if (strchr(arg, '/')) {
        char *slash = strrchr(arg, '/');

        if (slash == arg || !slash[1])
            ssvm_die(EXIT_USAGE, "invalid snapshot reference: %s "
                     "(NAME or vg/[tp/]lv/name/NAME[.sbvs])", argv[optind]);
        *slash = '\0';
        snprintf(subref, sizeof subref, "%s", arg);
        if (ssvm_strcpy(snapname, sizeof snapname, slash + 1) != 0)
            ssvm_die(EXIT_USAGE, "snapshot name too long: %s", slash + 1);
        if (strchr(subref, '/')) {
            char vg[SSVM_PATHSTR_MAX], tp[SSVM_PATHSTR_MAX];
            char lv[SSVM_PATHSTR_MAX];
            if (ssvm_parse_lvpath(subref, vg, tp, lv) != 0)
                ssvm_die(EXIT_USAGE, "invalid path: %s "
                         "(vg/[tp/]lv/name or a plain unique name)", subref);
        } else if (!ssvm_valid_name(subref)) {
            ssvm_die(EXIT_USAGE, "invalid name: %s", subref);
        }
    } else {
        subref[0] = '\0';
        if (ssvm_strcpy(snapname, sizeof snapname, arg) != 0)
            ssvm_die(EXIT_USAGE, "snapshot name too long: %s", arg);
    }

    ssvm_require_root();

    lockfd = ssvm_index_lock();
    if (ssvm_index_load(&ix) != 0) {
        ssvm_index_unlock(lockfd);
        return EXIT_SYSTEM;
    }

    if (subref[0]) {
        sv = ssvm_index_resolve(&ix, subref, &nm);
        if (!sv) {
            ssvm_index_unlock(lockfd);
            if (nm > 1)
                ssvm_die(EXIT_USAGE, "ambiguous name: %s — use the full "
                         "path", subref);
            ssvm_die(EXIT_NOENT, "no such subvolume: %s", subref);
        }
        si = find_snap_in(sv, snapname);
        if (si < 0) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_NOENT, "snapshot not found in %s: %s "
                     "(lsbv -l %s lists the snapshots)", sv->path, snapname,
                     sv->path);
        }
    } else {
        sv = ssvm_snap_find(&ix, snapname, &si, &nm);
        if (!sv || nm != 1) {
            ssvm_index_unlock(lockfd);
            if (nm > 1)
                ssvm_die(EXIT_USAGE, "ambiguous snapshot name: %s — use "
                         "vg/[thin-pool/]lv/name/SNAP.sbvs", snapname);
            ssvm_die(EXIT_NOENT, "no such snapshot: %s (lsbv -l lists the "
                     "snapshots per subvolume)", snapname);
        }
    }
    snprintf(path, sizeof path, "%s", sv->path);
    snprintf(snapname, sizeof snapname, "%s", sv->snaps[si].name);

    ssvm_sbv_dir(sv, dir, sizeof dir);
    ssvm_sbv_file(sv, file, sizeof file);
    if (ssvm_path_join(snapfile, sizeof snapfile, dir, snapname) != 0 ||
        ssvm_path_join(tmpfile, sizeof tmpfile, dir, ".merge.tmp") != 0) {
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
                     "directory — deactivate before the merge "
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
    nh.snapcount = (uint64_t)(sv->nsnap - 1);
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

    ssvm_chattr_i(snapfile, 0);
    if (unlink(snapfile) != 0) {
        ssvm_chattr_i(snapfile, 1);
        if (!active) {
            if (ssvm_chattr_i(file, 1) != 0)
                ssvm_warn("cannot seal %s (chattr +i)", file);
            if (ssvm_chattr_i(dir, 1) != 0)
                ssvm_warn("cannot seal %s (chattr +i)", dir);
        }
        ssvm_index_unlock(lockfd);
        ssvm_sysdie(EXIT_SYSTEM, "cannot remove the merged snapshot %s",
                    snapfile);
    }

    for (int i = si; i + 1 < sv->nsnap; i++)
        sv->snaps[i] = sv->snaps[i + 1];
    sv->nsnap--;

    if (!active) {
        if (ssvm_chattr_i(file, 1) != 0)
            ssvm_warn("cannot seal %s (chattr +i)", file);
        if (ssvm_chattr_i(dir, 1) != 0)
            ssvm_warn("cannot seal %s (chattr +i)", dir);
    }

    snprintf(sv->pendrb, sizeof sv->pendrb, "%s", snapname);
    if (ssvm_index_save(&ix) != 0) {
        ssvm_index_unlock(lockfd);
        return EXIT_SYSTEM;
    }
    ssvm_index_unlock(lockfd);

    ssvm_format_size(copied, used, sizeof used);

    if (active) {
        printf("merge registered: %s <- %s (%s copied)\n", path, snapname,
               used);
        printf("  snapshot %s consumed by the merge\n", snapname);
        fflush(stdout);
        fprintf(stderr, "W: %s is MOUNTED — the on-disk image "
                "has been replaced, but the current mount keeps\n"
                "  running on the old image, which is now VOLATILE: "
                "data written from now on is NOT persisted and will "
                "be lost.\n"
                "  The merge applies the restored image only on "
                "remount:\n"
                "    svchange -an %s && svchange -ay %s\n",
                path, path, path);
    } else {
        printf("merged: %s <- %s (%s copied)\n", path, snapname, used);
        printf("  restored content becomes visible on activation "
               "(svchange -ay %s)\n", path);
        printf("  snapshot %s consumed by the merge\n", snapname);
    }

    ssvm_index_free(&ix);
    return 0;
}
