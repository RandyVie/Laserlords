#include "ll.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

FILE *ll_trace;
int   ll_verbose;

/* Lines are buffered so identical consecutive lines collapse into a repeat count. */
static char line[1024], prev[1024];
static size_t len;
static unsigned repeats;

static void emit_line(void)
{
    line[len] = 0;
    if (!strcmp(line, prev)) { repeats++; len = 0; return; }
    if (repeats) fprintf(ll_trace, "        (previous line repeated %u more times)\n", repeats);
    repeats = 0;
    fputs(line, ll_trace);
    memcpy(prev, line, len + 1);
    len = 0;
}

void ll_log(const char *fmt, ...)
{
    if (!ll_trace) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (char *p = buf; *p; p++) {
        if (len < sizeof line - 2) line[len++] = *p;
        if (*p == '\n') emit_line();
    }
}

void ll_flush(void)
{
    if (!ll_trace) return;
    if (len) { line[len++] = '\n'; emit_line(); }
    if (repeats) fprintf(ll_trace, "        (previous line repeated %u more times)\n", repeats);
    repeats = 0;
    fflush(ll_trace);
}

void ll_fatal(const char *fmt, ...)
{
    ll_flush();
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
