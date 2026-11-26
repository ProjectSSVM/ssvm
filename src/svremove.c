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
"Usage: svremove [-f] SUBVOL                 removes the subvolume\n"
"       svremove [-f] SUBVOL/NAME.sbvs       removes a snapshot\n"
"       svremove [-f] SNAPSHOT               removes a snapshot by name\n"
"       svremove -t TAG SUBVOL               removes only the tag\n"
"                                                   (SSVM " SSVM_VERSION ")\n"
"\n"
"  SUBVOL      subvolume path (vg/[thin-pool/]lv/name) or just its\n"
"              name, when unique among the registered subvolumes\n"
"  SNAPSHOT    snapshot name (with or without the .sbvs suffix),\n"
"              unique among all the registered snapshots — a bare name\n"
"              that matches a subvolume always resolves to the subvolume\n"
"  -f          skips the confirmation prompt\n"
"  -t TAG      removes only tag TAG from the subvolume (data untouched)\n"
"  -h, --help  this help\n"
"\n"
"Subvolumes and snapshots can only be removed while INACTIVE.\n"
"\n"
"Exit codes: 0 ok · 1 invalid usage · 2 not found · 3 state conflict\n"
"            · 4 system error\n",
    out);
}

static int is_snapshot_path(const char *s)
{
    size_t l = strlen(s);
    char buf[SSVM_PATHSTR_MAX + SSVM_SNAPNAME_MAX + 8];
    char *save = NULL, *last = NULL;
    int n = 0;

    if (l < 6 || l >= sizeof buf)
        return 0;
    if (strcmp(s + l - 5, ".sbvs") != 0)
        return 0;
    strcpy(buf, s);
    for (char *tok = strtok_r(buf, "/", &save); tok;
         tok = strtok_r(NULL, "/", &save)) {
        last = tok;
        n++;
    }
    if ((n < 3 || n > 5) || !last)
        return 0;
    {
        size_t ll = strlen(last);
        char base[SSVM_SNAPNAME_MAX];
        if (ll <= 5 || ll - 5 >= sizeof base)
            return 0;
        memcpy(base, last, ll - 5);
        base[ll - 5] = '\0';
        return ssvm_valid_name(base);
    }
}

static void split_snapshot_path(const char *s, char *subpath, size_t n)
{
    size_t l = strlen(s);
    size_t cut = l;

    while (cut > 0 && s[cut - 1] != '/')
        cut--;
    snprintf(subpath, n, "%.*s", (int)(cut - 1), s);
}

static int remove_snapshot(struct ssvm_index *ix, struct ssvm_subvol *sv,
                           int si, const char *display, int force, int lockfd)
{
    char dir[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 8];
    char file[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + SSVM_NAME_MAX + 16];
    char mp[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 8];
    char snapfile[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + SSVM_SNAPNAME_MAX + 8];
    char src[128], used[32];
    const char *snapname = sv->snaps[si].name;

    ssvm_sbv_dir(sv, dir, sizeof dir);
    ssvm_sbv_file(sv, file, sizeof file);
    ssvm_mp_of(sv, mp, sizeof mp);
    if (ssvm_find_mount(mp, src, sizeof src)) {
        ssvm_index_unlock(lockfd);
        ssvm_die(EXIT_CONFLICT, "%s is active — deactivate first "
                 "(svchange -an)", sv->path);
    }
    if (ssvm_path_join(snapfile, sizeof snapfile, dir, snapname) != 0) {
        ssvm_index_unlock(lockfd);
        ssvm_die(EXIT_SYSTEM, "path too long");
    }

    ssvm_format_size(ssvm_used_bytes(snapfile), used, sizeof used);
    if (!force && !ssvm_confirm(display))
        ssvm_die(EXIT_USAGE, "aborted");

    ssvm_chattr_i(dir, 0);
    ssvm_chattr_i(snapfile, 0);
    if (unlink(snapfile) != 0) {
        ssvm_chattr_i(dir, 1);
        ssvm_index_unlock(lockfd);
        ssvm_sysdie(EXIT_SYSTEM, "cannot remove %s", snapfile);
    }
    ssvm_chattr_i(dir, 1);

    for (int i = si; i + 1 < sv->nsnap; i++)
        sv->snaps[i] = sv->snaps[i + 1];
    sv->nsnap--;

    if (strcmp(sv->pendrb, snapname) == 0)
        sv->pendrb[0] = '\0';
    if (ssvm_index_save(ix) != 0) {
        ssvm_index_unlock(lockfd);
        return EXIT_SYSTEM;
    }
    ssvm_index_unlock(lockfd);
    printf("snapshot removed: %s (%s on disk)\n", display, used);
    return 0;
}

int main(int argc, char **argv)
{
    int opt, force = 0, lockfd;
    const char *tagrm = NULL;
    char path[SSVM_PATHSTR_MAX + SSVM_SNAPNAME_MAX + 8];
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
    char sz[32];

    opterr = 0;
    while ((opt = getopt_long(argc, argv, "ft:h", lo, NULL)) != -1) {
        switch (opt) {
        case 'f':
            force = 1;
            break;
        case 't':
            if (!ssvm_valid_tag(optarg))
                ssvm_die(EXIT_USAGE, "invalid tag: %s", optarg);
            tagrm = optarg;
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
    snprintf(path, sizeof path, "%s", argv[optind]);

    ssvm_require_root();

    lockfd = ssvm_index_lock();
    if (ssvm_index_load(&ix) != 0) {
        ssvm_index_unlock(lockfd);
        return EXIT_SYSTEM;
    }

    if (tagrm) {
        int found = -1;
        int nm;

        if (is_snapshot_path(path))
            ssvm_die(EXIT_USAGE, "-t expects a subvolume, not a snapshot");
        sv = ssvm_index_resolve(&ix, path, &nm);
        if (!sv) {
            ssvm_index_unlock(lockfd);
            if (nm > 1)
                ssvm_die(EXIT_USAGE, "ambiguous name: %s — use the full path",
                         path);
            ssvm_die(EXIT_NOENT, "no such subvolume: %s", path);
        }
        snprintf(path, sizeof path, "%s", sv->path);
        for (int i = 0; i < sv->ntags; i++)
            if (strcmp(sv->tags[i], tagrm) == 0)
                found = i;
        if (found < 0) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_NOENT, "tag does not exist on %s: %s", path, tagrm);
        }
        for (int i = found; i + 1 < sv->ntags; i++)
            strcpy(sv->tags[i], sv->tags[i + 1]);
        sv->ntags--;
        if (ssvm_index_save(&ix) != 0) {
            ssvm_index_unlock(lockfd);
            return EXIT_SYSTEM;
        }
        ssvm_index_unlock(lockfd);
        printf("tag removed: %s from %s (data untouched)\n", tagrm, path);
        if (sv->ntags > 0) {
            printf("remaining tags:");
            for (int i = 0; i < sv->ntags; i++)
                printf(" %s", sv->tags[i]);
            printf("\n");
        } else {
            printf("remaining tags: (none)\n");
        }
        ssvm_index_free(&ix);
        return 0;
    }

    if (is_snapshot_path(path)) {
        char subpath[SSVM_PATHSTR_MAX];
        char snapname[SSVM_SNAPNAME_MAX];
        int si = -1;

        split_snapshot_path(path, subpath, sizeof subpath);
        sv = ssvm_index_find(&ix, subpath);
        if (!sv) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_NOENT, "no such subvolume: %s", subpath);
        }
        {
            const char *base = strrchr(path, '/') + 1;
            snprintf(snapname, sizeof snapname, "%s", base);
        }
        for (int i = 0; i < sv->nsnap; i++)
            if (strcmp(sv->snaps[i].name, snapname) == 0)
                si = i;
        if (si < 0) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_NOENT, "snapshot not found: %s", path);
        }
        {
            int rc = remove_snapshot(&ix, sv, si, path, force, lockfd);
            ssvm_index_free(&ix);
            return rc;
        }
    }

    {
        char vg[SSVM_PATHSTR_MAX], tp[SSVM_PATHSTR_MAX], lv[SSVM_PATHSTR_MAX];

        if (strchr(path, '/')) {
            if (ssvm_parse_lvpath(path, vg, tp, lv) != 0)
                ssvm_die(EXIT_USAGE, "invalid path: %s (vg/[tp/]lv/name, "
                         "vg/[tp/]lv/name/SNAP.sbvs or -t TAG SUBVOL)", path);
        }
    }
    {
        int nm = 0;

        sv = ssvm_index_resolve(&ix, path, &nm);
        if (!sv && nm == 0) {
            int si = -1, snm = 0;
            struct ssvm_subvol *ow = ssvm_snap_find(&ix, path, &si, &snm);

            if (ow && snm == 1) {
                char disp[SSVM_PATHSTR_MAX + SSVM_SNAPNAME_MAX + 8];
                snprintf(disp, sizeof disp, "%s/%s", ow->path,
                         ow->snaps[si].name);
                {
                    int rc = remove_snapshot(&ix, ow, si, disp, force,
                                             lockfd);
                    ssvm_index_free(&ix);
                    return rc;
                }
            }
            if (ow && snm > 1) {
                ssvm_index_unlock(lockfd);
                ssvm_die(EXIT_USAGE, "ambiguous snapshot name: %s — use the "
                         "full path vg/[thin-pool/]lv/name/SNAP.sbvs", path);
            }
            ssvm_index_unlock(lockfd);
            if (!ssvm_valid_name(path))
                ssvm_die(EXIT_USAGE, "invalid name: %s", path);
            ssvm_die(EXIT_NOENT, "no such subvolume or snapshot: %s", path);
        }
        if (!sv) {
            ssvm_index_unlock(lockfd);
            ssvm_die(EXIT_USAGE, "ambiguous name: %s — use the full path",
                     path);
        }
        snprintf(path, sizeof path, "%s", sv->path);
    }

    ssvm_sbv_dir(sv, dir, sizeof dir);
    ssvm_sbv_file(sv, file, sizeof file);
    ssvm_mp_of(sv, mp, sizeof mp);
    if (ssvm_find_mount(mp, src, sizeof src)) {
        ssvm_index_unlock(lockfd);
        ssvm_die(EXIT_CONFLICT, "%s is active — deactivate first "
                 "(svchange -an)", path);
    }

    ssvm_format_size(sv->size, sz, sizeof sz);
    {
        char what[SSVM_PATHSTR_MAX + SSVM_SNAPNAME_MAX + 160];
        snprintf(what, sizeof what,
                 "%s (%s, %d snapshot(s), %d tag(s))", path, sz,
                 sv->nsnap, sv->ntags);
        if (!force && !ssvm_confirm(what))
            ssvm_die(EXIT_USAGE, "aborted");
    }

    ssvm_chattr_i(dir, 0);
    ssvm_chattr_i(file, 0);
    for (int s = 0; s < sv->nsnap; s++) {
        char f[SSVM_MPT_MAX + SSVM_SNAPNAME_MAX + 8];
        ssvm_path_join(f, sizeof f, dir, sv->snaps[s].name);
        ssvm_chattr_i(f, 0);
    }
    ssvm_chattr_i(mp, 0);

    if (ssvm_rm_rf(dir) != 0) {
        ssvm_index_unlock(lockfd);
        ssvm_die(EXIT_SYSTEM, "could not fully remove %s", dir);
    }

    {
        char parent[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 8];
        const char *root = ssvm_dir();

        snprintf(parent, sizeof parent, "%s", dir);
        for (;;) {
            char *slash = strrchr(parent, '/');
            if (!slash)
                break;
            *slash = '\0';
            if (parent[0] == '\0' || strcmp(parent, root) == 0)
                break;
            if (rmdir(parent) != 0)
                break;
        }
    }

    if (sv->mountpoint[0]) {
        if (rmdir(mp) != 0)
            ssvm_warn("mountpoint %s kept (not empty)", mp);
    }

    {
        size_t idx = (size_t)(sv - ix.v);
        free(ix.v[idx].snaps);
        for (size_t i = idx; i + 1 < ix.n; i++)
            ix.v[i] = ix.v[i + 1];
        ix.n--;
    }
    if (ssvm_index_save(&ix) != 0) {
        ssvm_index_unlock(lockfd);
        return EXIT_SYSTEM;
    }
    ssvm_index_unlock(lockfd);
    printf("removed: %s (%s)\n", path, dir);
    ssvm_index_free(&ix);
    return 0;
}
