// mods_order (runtime/mods.c) on a synthetic mods folder: no game files.
//   clang -std=gnu11 -O1 tools/test_mods.c runtime/mods.c -o build/test-mods.exe && build/test-mods.exe
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#endif

#include "../runtime/mods.h"

static void mod(const char *root, const char *name, bool with_romfs) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, name);
    mkdir(p, 0755);
    if (with_romfs) snprintf(p, sizeof p, "%s/%s/romfs", root, name), mkdir(p, 0755);
}

static void list(const char *root, const char *text) {
    char p[512];
    snprintf(p, sizeof p, "%s/mods.txt", root);
    FILE *f = fopen(p, "w");
    fputs(text, f);
    fclose(f);
}

static void expect(const char *root, const char *want) {
    ModName *m = NULL;
    int n = mods_order(root, &m);
    char got[1024] = "";
    for (int i = 0; i < n; i++) strcat(strcat(got, i ? "," : ""), m[i]);
    if (strcmp(got, want)) { fprintf(stderr, "mods_order: want \"%s\", got \"%s\"\n", want, got); exit(1); }
    free(m);
}

int main(void) {
    char root[256];
    snprintf(root, sizeof root, "build/test-mods-%u", (unsigned)rand());
    mkdir("build", 0755);
    mkdir(root, 0755);
    expect("build/no-such-folder", "");
    mod(root, "b60fps", true);
    mod(root, "atextures", true);
    mod(root, "notamod", false);  // (no romfs/ inside: not a mod)
    expect(root, "atextures,b60fps");  // no mods.txt: name order
    list(root, "b60fps\r\n");
    expect(root, "b60fps,atextures");  // listed first, the rest after in name order
    list(root, "-b60fps\natextures\n");
    expect(root, "atextures");  // switched off
    list(root, "gone\n-gone\natextures\natextures\nnotamod\n");
    expect(root, "atextures,b60fps");  // a mod that is not there, a repeat, and a folder that is not a mod: ignored
    puts("PASS: mods order from mods.txt, then name order; switched-off and non-mod folders left out");
    return 0;
}
