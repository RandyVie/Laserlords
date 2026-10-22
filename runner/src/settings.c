/* Persistent settings: key=value lines in <pref path>/settings.txt. */
#include "ll.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#define MAXSET 16
static char keys[MAXSET][32], vals[MAXSET][1024];
static int nset;
static char file[1024];

static void load(void)
{
    if (file[0]) return;
    char *pref = SDL_GetPrefPath("LaserLords", "LaserLords");
    snprintf(file, sizeof file, "%ssettings.txt", pref ? pref : "");
    SDL_free(pref);
    FILE *f = fopen(file, "r");
    if (!f) return;
    char line[1100];
    while (nset < MAXSET && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        line[strcspn(line, "\r")] = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        snprintf(keys[nset], sizeof keys[0], "%s", line);
        snprintf(vals[nset], sizeof vals[0], "%s", v);
        nset++;
    }
    fclose(f);
}

const char *settings_get(const char *key, const char *def)
{
    load();
    for (int i = 0; i < nset; i++) if (!strcmp(keys[i], key)) return vals[i];
    return def;
}

void settings_set(const char *key, const char *val)
{
    load();
    int i;
    for (i = 0; i < nset; i++) if (!strcmp(keys[i], key)) break;
    if (i == nset) { if (nset == MAXSET) return; nset++; }
    snprintf(keys[i], sizeof keys[0], "%s", key);
    snprintf(vals[i], sizeof vals[0], "%s", val);
    FILE *f = fopen(file, "w");
    if (!f) return;
    for (int k = 0; k < nset; k++) fprintf(f, "%s=%s\n", keys[k], vals[k]);
    fclose(f);
}
