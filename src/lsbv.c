#include "ssvm.h"

#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

static void usage(FILE *out)
{
    fputs(
"Usage: lsbv [path] [-l] [--tag TAG]      (SSVM " SSVM_VERSION ")\n"
"\n"
"Lists SSVM subvolumes registered under /var/ssvm.\n"
"\n"
"  path       a subvolume path (vg/[thin-pool/]lv/name — shows the\n"
"             detail view), its plain name when unique, or any path\n"
"             prefix such as a vg, a vg/thin-pool or a vg/[tp/]lv\n"
"             (default: all)\n"
"  -l         long format (details for each subvolume)\n"
"  --tag TAG  shows only subvolumes carrying tag TAG\n"
"  -h, --help this help\n"
"\n"
"Exit codes: 0 ok · 1 invalid usage · 2 path not found\n",
    out);
}

static int path_matches(const char *path, const char *prefix,
                         const char *name)
{
    if (prefix) {
        size_t pl = strlen(prefix);

        if (strncmp(path, prefix, pl) != 0 || path[pl] != '/')
            return 0;
    }
    if (name) {
        const char *last = strrchr(path, '/');

        last = last ? last + 1 : path;
        if (strcmp(last, name) != 0)
            return 0;
    }
    return 1;
}

static int tagfiltered(const struct ssvm_subvol *sv, const char *tagfilter)
{
    if (!tagfilter)
        return 0;
    for (int t = 0; t < sv->ntags; t++)
        if (strcmp(sv->tags[t], tagfilter) == 0)
            return 0;
    return 1;
}

static void print_short(const struct ssvm_index *ix, const char *prefix,
                        const char *name, const char *tagfilter)
{
    char lastvg[SSVM_NAME_MAX + 1] = "";
    int anypend = 0;

    for (size_t i = 0; i < ix->n; i++) {
        const struct ssvm_subvol *sv = &ix->v[i];
        char vg[SSVM_PATHSTR_MAX], tp[SSVM_PATHSTR_MAX], lv[SSVM_PATHSTR_MAX];
        char sz[32], src[128];
        int active;

        if (!path_matches(sv->path, prefix, name))
            continue;
        if (ssvm_parse_lvpath(sv->path, vg, tp, lv) != 0)
            continue;
        if (tagfilter) {
            int hit = 0;
            for (int t = 0; t < sv->ntags; t++)
                if (strcmp(sv->tags[t], tagfilter) == 0)
                    hit = 1;
            if (!hit)
                continue;
        }

        if (strcmp(lastvg, vg) != 0) {
            printf("%s:\n", vg);
            ssvm_strcpy(lastvg, sizeof lastvg, vg);
        }

        active = ssvm_is_active(sv, src, sizeof src);
        ssvm_format_size(sv->size, sz, sizeof sz);
        {
            char est[24];
            const char *row = sv->path + strlen(vg) + 1;

            snprintf(est, sizeof est, "%s%s",
                     active ? "active" : "inactive",
                     sv->pendrb[0] ? "*" : "");
            if (sv->pendrb[0])
                anypend = 1;
            printf("  %-28s %6s  %-8s", row, sz, est);
        }
        if (sv->ntags > 0) {
            char tags[SSVM_TAG_MAX * SSVM_MAX_TAGS + 4];
            size_t o = 0;
            for (int t = 0; t < sv->ntags && o + 2 < sizeof tags; t++)
                o += (size_t)snprintf(tags + o, sizeof tags - o, "%s%s",
                                      t ? "," : "", sv->tags[t]);
            printf("  [%s]", tags);
        }
        printf("\n");
    }

    if (anypend)
        printf("\n(* pending rollback — the on-disk image is already "
               "restored; it applies on remount: svchange -an && "
               "svchange -ay)\n");
}

static void print_long(const struct ssvm_subvol *sv)
{
    char mp[SSVM_MPT_MAX + 8], file[SSVM_MPT_MAX + SSVM_NAME_MAX + 16];
    char dir[SSVM_MPT_MAX + SSVM_PATHSTR_MAX + 8];
    char src[128], sz[32], used[32], tbuf[32];
    struct sbv_hdr h;
    int active = ssvm_is_active(sv, src, sizeof src);

    ssvm_mp_of(sv, mp, sizeof mp);
    ssvm_sbv_dir(sv, dir, sizeof dir);
    ssvm_sbv_file(sv, file, sizeof file);

    printf("Path:             %s\n", sv->path);
    printf("UUID:             %s\n", sv->uuid);
    printf("Filesystem:       %s\n",
           sv->fstype[0] ? sv->fstype : SSVM_FSTYPE_DEFAULT);
    ssvm_fmt_time(sv->created, tbuf, sizeof tbuf);
    printf("Created:          %s\n", tbuf);
    printf("State:            %s", active ? "active" : "inactive");
    if (!active && ssvm_sbv_read_hdr(file, &h) == 0)
        printf(" (%s)", (h.flags & SBV_FLAG_SEALED) ? "sealed" : "unsealed");
    printf("\n");

    if (sv->pendrb[0]) {
        char f[SSVM_MPT_MAX + SSVM_SNAPNAME_MAX + 8];
        struct stat st;

        printf("Pending rollback: %s", sv->pendrb);
        if (ssvm_path_join(f, sizeof f, dir, sv->pendrb) != 0 ||
            lstat(f, &st) != 0)
            printf(" (snapshot consumed by the merge)");
        printf("\n");
        printf("  the on-disk image was already restored; %s\n",
               active
               ? "the current mount still runs on the old image (volatile) "
                 "— it applies on remount (svchange -an && svchange -ay)"
                 : "it becomes visible on the next activation (svchange -ay)");
    }

    printf("Mountpoint:       %s%s\n", mp,
           sv->mountpoint[0] ? " (custom)" : " (default)");
    if (active)
        printf("Mounted via:      %s\n", src);

    ssvm_format_size(sv->size, sz, sizeof sz);
    if (active) {
        struct statvfs st;
        if (statvfs(mp, &st) == 0)
            ssvm_format_size((uint64_t)(st.f_blocks - st.f_bfree) *
                             (uint64_t)st.f_frsize, used, sizeof used);
        else
            snprintf(used, sizeof used, "?");
    } else {
        ssvm_format_size(ssvm_used_bytes(file), used, sizeof used);
    }
    printf("Size:             %s virtual / %s used\n", sz, used);

    if (sv->ntags > 0) {
        printf("Tags:             ");
        for (int t = 0; t < sv->ntags; t++)
            printf("%s%s", t ? ", " : "", sv->tags[t]);
        printf("\n");
    } else {
        printf("Tags:             (none)\n");
    }

    printf("Snapshots:        %d\n", sv->nsnap);
    if (sv->nsnap > 0) {
        for (int s = 0; s < sv->nsnap; s++) {
            const struct ssvm_snap *sn = &sv->snaps[s];
            char f[SSVM_MPT_MAX + SSVM_SNAPNAME_MAX + 8];
            ssvm_path_join(f, sizeof f, dir, sn->name);
            ssvm_format_size(ssvm_used_bytes(f), used, sizeof used);
            ssvm_fmt_time(sn->created, tbuf, sizeof tbuf);
            printf("  %-40s %8s  %s\n", sn->name, used, tbuf);
        }
    }
}

int main(int argc, char **argv)
{
    int opt, longmode = 0, rc = EXIT_OK;
    const char *tagfilter = NULL, *patharg = NULL;
    struct ssvm_index ix;
    struct option lo[] = {
        { "tag",  required_argument, NULL, 'T' },
        { "help", no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    opterr = 0;
    while ((opt = getopt_long(argc, argv, "lh", lo, NULL)) != -1) {
        switch (opt) {
        case 'l':
            longmode = 1;
            break;
        case 'T':
            if (!ssvm_valid_tag(optarg))
                ssvm_die(EXIT_USAGE, "invalid tag: %s", optarg);
            tagfilter = optarg;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return EXIT_USAGE;
        }
    }
    if (optind < argc)
        patharg = argv[optind];
    if (optind + 1 < argc) {
        usage(stderr);
        return EXIT_USAGE;
    }

    if (ssvm_index_load(&ix) != 0)
        return EXIT_SYSTEM;

    if (patharg) {
        int nm;
        struct ssvm_subvol *sv = ssvm_index_resolve(&ix, patharg, &nm);

        if (sv) {
            if (tagfilter) {
                int hit = 0;
                for (int t = 0; t < sv->ntags; t++)
                    if (strcmp(sv->tags[t], tagfilter) == 0)
                        hit = 1;
                if (!hit)
                    goto done;
            }
            print_long(sv);
        } else if (nm > 1) {
            if (longmode) {
                for (size_t i = 0; i < ix.n; i++)
                    if (path_matches(ix.v[i].path, NULL, patharg) &&
                        !tagfiltered(&ix.v[i], tagfilter))
                        print_long(&ix.v[i]);
            } else {
                print_short(&ix, NULL, patharg, tagfilter);
            }
        } else {
            int found = 0;

            for (size_t i = 0; i < ix.n; i++)
                if (path_matches(ix.v[i].path, patharg, NULL))
                    found = 1;
            if (!found) {
                fprintf(stderr, "E: no such subvolume: %s\n", patharg);
                rc = EXIT_NOENT;
                goto done;
            }
            if (longmode) {
                for (size_t i = 0; i < ix.n; i++)
                    if (path_matches(ix.v[i].path, patharg, NULL) &&
                        !tagfiltered(&ix.v[i], tagfilter))
                        print_long(&ix.v[i]);
            } else {
                print_short(&ix, patharg, NULL, tagfilter);
            }
        }
    } else {
        if (ix.n > 0) {
            if (longmode) {
                for (size_t i = 0; i < ix.n; i++)
                    if (!tagfiltered(&ix.v[i], tagfilter))
                        print_long(&ix.v[i]);
            } else {
                print_short(&ix, NULL, NULL, tagfilter);
            }
        }
    }

done:
    ssvm_index_free(&ix);
    return rc;
}
