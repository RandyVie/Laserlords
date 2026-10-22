/* Portability shims: the runner is written against POSIX; these map the few calls it uses onto
 * Windows (MinGW and MSVC). */
#ifndef LL_COMPAT_H
#define LL_COMPAT_H

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define ll_mkdir(p)          _mkdir(p)
#define ll_truncate(fp, n)   _chsize(_fileno(fp), (long)(n))
#define ll_access_ok(p)      (_access((p), 0) == 0)
static inline struct tm *ll_localtime(const time_t *t, struct tm *out) { return localtime_s(out, t) ? NULL : out; }
#ifdef _MSC_VER
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
/* minimal dirent on top of FindFirstFile */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
struct dirent { char d_name[MAX_PATH]; };
typedef struct { HANDLE h; WIN32_FIND_DATAA fd; bool first; struct dirent e; } DIR;
static inline DIR *opendir(const char *path)
{
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", path);
    DIR *d = calloc(1, sizeof *d);
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = true;
    return d;
}
static inline struct dirent *readdir(DIR *d)
{
    if (!d->first && !FindNextFileA(d->h, &d->fd)) return NULL;
    d->first = false;
    snprintf(d->e.d_name, sizeof d->e.d_name, "%s", d->fd.cFileName);
    return &d->e;
}
static inline int closedir(DIR *d) { FindClose(d->h); free(d); return 0; }
#else
#include <dirent.h>
#include <strings.h>
#endif
#else
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#define ll_mkdir(p)          mkdir((p), 0755)
#define ll_truncate(fp, n)   ftruncate(fileno(fp), (off_t)(n))
#define ll_access_ok(p)      (access((p), F_OK) == 0)
static inline struct tm *ll_localtime(const time_t *t, struct tm *out) { return localtime_r(t, out); }
#endif

#endif
