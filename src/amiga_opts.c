/* The Amiga features: see amiga_opts.h. */
#include "amiga_opts.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

AmigaOpts amiga;
const char *amiga_disk = "Game Amiga";

static const struct { const char *name; size_t off; } NAMES[] = {
    { "intro",      offsetof(AmigaOpts, intro) },
    { "menu",       offsetof(AmigaOpts, menu) },
    { "dashboards", offsetof(AmigaOpts, dashboards) },
    { "music",      offsetof(AmigaOpts, music) },
    { "projection", offsetof(AmigaOpts, projection) },
    { "sky",        offsetof(AmigaOpts, sky) },
    { "cliffs",     offsetof(AmigaOpts, cliffs) },
    { "traffic",    offsetof(AmigaOpts, traffic) },
    { "cops",       offsetof(AmigaOpts, cops) },
    { "wide-roads", offsetof(AmigaOpts, wide_roads) },
    { "signs",      offsetof(AmigaOpts, signs) },
};
#define NNAMES (sizeof NAMES / sizeof NAMES[0])

bool amiga_parse(const char *list)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *item = strtok(buf, ","); item; item = strtok(NULL, ",")) {
        size_t i = 0;
        if (!strcmp(item, "all")) {
            for (i = 0; i < NNAMES; i++) *(bool *)((char *)&amiga + NAMES[i].off) = true;
            continue;
        }
        while (i < NNAMES && strcmp(NAMES[i].name, item)) i++;
        if (i == NNAMES) return false;
        *(bool *)((char *)&amiga + NAMES[i].off) = true;
    }
    return true;
}

bool amiga_any(void)
{
    for (size_t i = 0; i < NNAMES; i++)
        if (*(const bool *)((const char *)&amiga + NAMES[i].off)) return true;
    return false;
}
