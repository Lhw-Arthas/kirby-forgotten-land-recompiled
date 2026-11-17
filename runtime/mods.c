#include "mods.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int find(const ModName *m, int n, const char *name) {
    for (int i = 0; i < n; i++) if (!strcmp(m[i], name)) return i;
    return -1;
}

static int by_name(const void *a, const void *b) { return strcmp(a, b); }

int mods_order(const char *dir, ModName **out) {
    ModName *found = NULL, *off = NULL;
    int nfound = 0, noff = 0;
    char p[1400];
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        struct stat sb;
        snprintf(p, sizeof p, "%s/%s/romfs", dir, e->d_name);
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(ModName) || stat(p, &sb) || !S_ISDIR(sb.st_mode)) continue;
        found = realloc(found, sizeof *found * (nfound + 1));
        snprintf(found[nfound++], sizeof *found, "%s", e->d_name);
    }
    if (d) closedir(d);
    if (nfound) qsort(found, nfound, sizeof *found, by_name);
    ModName *on = calloc(nfound + 1, sizeof *on);
    int n = 0;
    snprintf(p, sizeof p, "%s/mods.txt", dir);
    FILE *f = fopen(p, "r");
    char line[300];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        const char *name = line[0] == '-' ? line + 1 : line;
        if (find(found, nfound, name) < 0 || find(on, n, name) >= 0 || find(off, noff, name) >= 0) continue;
        if (line[0] == '-') {
            off = realloc(off, sizeof *off * (noff + 1));
            snprintf(off[noff++], sizeof *off, "%s", name);
        } else snprintf(on[n++], sizeof *on, "%s", name);
    }
    if (f) fclose(f);
    for (int i = 0; i < nfound; i++)
        if (find(on, n, found[i]) < 0 && find(off, noff, found[i]) < 0) snprintf(on[n++], sizeof *on, "%s", found[i]);
    free(found);
    free(off);
    *out = on;
    return n;
}
