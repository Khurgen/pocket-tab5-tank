/* persist_port_sim.c — sim implementation of the progression ports:
 * ~/.cache/pocket-tank/tank.sav + wall clock. */
#include "progression.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* POCKET_TANK_SAVE overrides the save path (selftests use a scratch file) */
static const char *path(void) {
    static char p[512];
    if (getenv("POCKET_TANK_SAVE")) return getenv("POCKET_TANK_SAVE");
    snprintf(p, sizeof p, "%s/.cache/pocket-tank/tank.sav", getenv("HOME") ? getenv("HOME") : ".");
    return p;
}
bool persist_port_load(void *buf, size_t len) {
    FILE *f = fopen(path(), "rb"); if (!f) return false;
    size_t n = fread(buf, 1, len, f); fclose(f); return n == len;
}
bool persist_port_save(const void *buf, size_t len) {
    char dir[512]; snprintf(dir, sizeof dir, "%s/.cache/pocket-tank", getenv("HOME") ? getenv("HOME") : ".");
    char cmd[600]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir); (void)system(cmd);
    FILE *f = fopen(path(), "wb"); if (!f) return false;
    size_t n = fwrite(buf, 1, len, f); fclose(f); return n == len;
}
int64_t clock_port_now_unix(void) { return (int64_t)time(NULL); }
