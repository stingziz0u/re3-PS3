// ps3_fs.cpp -- file system glue for re3 on the PS3.
//
// lv2 only takes absolute paths and has no working directory, and the PC
// data mixes the letter case of names (DATA\GTA3.DAT vs data/gta3.dat) on a
// case-sensitive file system. So:
//   - the working directory is emulated (PS3_chdir / PS3_getcwd, used by
//     CFileMgr::SetDir),
//   - casepath() turns any game path into the real absolute one,
//   - fopen/remove/rename/mkdir/stat/opendir/unlink are wrapped at link time
//     (-Wl,--wrap=...) so the relative paths of the game and librw work.

#include "common.h"
#include "crossplatform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/file.h>

#include "ps3_platform.h"

#define PS3_PATH_MAX 1024

static char g_cwd[PS3_PATH_MAX] = PS3_USRDIR;

// ---- path helpers ---------------------------------------------------------

static int
lv2Exists(const char *abspath, sysFSStat *st)
{
	sysFSStat tmp;
	return sysLv2FsStat(abspath, st ? st : &tmp) == 0;
}

extern "C" int
PS3_IsDir(const char *abspath)
{
	sysFSStat st;
	if (!lv2Exists(abspath, &st))
		return 0;
	return (st.st_mode & 0170000) == 0040000;
}

extern "C" int
PS3_FileSize(const char *abspath, long long *size)
{
	sysFSStat st;
	if (!lv2Exists(abspath, &st))
		return 0;
	*size = (long long)st.st_size;
	return 1;
}

// Absolute, '/'-separated, without "." / ".." / empty parts or trailing
// spaces in a part (the PC paths have those).
static int
normalizePath(const char *path, char *out, int outSize)
{
	char tmp[PS3_PATH_MAX * 2];
	const char *parts[128];
	int lens[128];
	int nparts = 0;

	if (path[0] == '/' || path[0] == '\\')
		snprintf(tmp, sizeof(tmp), "%s", path);
	else
		snprintf(tmp, sizeof(tmp), "%s/%s", g_cwd, path);

	for (char *p = tmp; *p; p++)
		if (*p == '\\')
			*p = '/';

	char *p = tmp;
	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		char *start = p;
		while (*p && *p != '/')
			p++;
		int len = (int)(p - start);
		while (len > 0 && start[len - 1] == ' ')
			len--;
		if (len == 0 || (len == 1 && start[0] == '.'))
			continue;
		if (len == 2 && start[0] == '.' && start[1] == '.') {
			if (nparts > 0)
				nparts--;
			continue;
		}
		if (nparts == 128)
			return 0;
		parts[nparts] = start;
		lens[nparts] = len;
		nparts++;
	}

	int o = 0;
	for (int i = 0; i < nparts; i++) {
		if (o + 1 + lens[i] + 1 > outSize)
			return 0;
		out[o++] = '/';
		memcpy(out + o, parts[i], lens[i]);
		o += lens[i];
	}
	if (o == 0) {
		if (outSize < 2)
			return 0;
		out[o++] = '/';
	}
	out[o] = '\0';
	return 1;
}

// Small cache: lowercase normalized path -> real path (only for paths
// that exist). Most opens repeat (gta3.dir, txds by name, the .ifp...).
#define RESOLVE_CACHE_SIZE 512
struct ResolveEntry {
	unsigned hash;
	char *key;
	char *real;
};
static ResolveEntry g_cache[RESOLVE_CACHE_SIZE];

static unsigned
hashLower(const char *s)
{
	unsigned h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (unsigned char)tolower((unsigned char)*s)) * 16777619u;
	return h;
}

static const char*
cacheFind(const char *norm, unsigned h)
{
	ResolveEntry *e = &g_cache[h % RESOLVE_CACHE_SIZE];
	if (e->key && e->hash == h && strcasecmp(e->key, norm) == 0)
		return e->real;
	return NULL;
}

static void
cacheStore(const char *norm, unsigned h, const char *real)
{
	ResolveEntry *e = &g_cache[h % RESOLVE_CACHE_SIZE];
	free(e->key);
	free(e->real);
	e->hash = h;
	e->key = strdup(norm);
	e->real = strdup(real);
}

// Looks for name (any case) in the directory dir. found gets the real name.
static int
findInDir(const char *dir, const char *name, int nameLen, char *found, int foundSize)
{
	s32 fd;
	sysFSDirent ent;
	u64 nread;
	int ok = 0;

	if (sysLv2FsOpenDir(dir[0] ? dir : "/", &fd) != 0)
		return 0;
	for (;;) {
		nread = 0;
		if (sysLv2FsReadDir(fd, &ent, &nread) != 0 || nread == 0)
			break;
		if ((int)strlen(ent.d_name) == nameLen && strncasecmp(ent.d_name, name, nameLen) == 0) {
			snprintf(found, foundSize, "%s", ent.d_name);
			ok = 1;
			break;
		}
	}
	sysLv2FsCloseDir(fd);
	return ok;
}

extern "C" int
PS3_ResolvePath(const char *path, char *out, int outSize)
{
	char norm[PS3_PATH_MAX];

	if (!normalizePath(path, norm, sizeof(norm)))
		return 0;

	// exact name: nothing to fix
	if (lv2Exists(norm, NULL)) {
		snprintf(out, outSize, "%s", norm);
		return 1;
	}

	unsigned h = hashLower(norm);
	const char *cached = cacheFind(norm, h);
	if (cached) {
		snprintf(out, outSize, "%s", cached);
		return 1;
	}

	// walk the parts, fixing the case of each one that exists
	char real[PS3_PATH_MAX];
	int rl = 0;
	bool lost = false;	// a part is missing: keep the rest as given
	const char *p = norm;
	real[0] = '\0';
	while (*p) {
		p++;	// '/'
		const char *start = p;
		while (*p && *p != '/')
			p++;
		int len = (int)(p - start);
		if (rl + 1 + len + 1 > (int)sizeof(real))
			return 0;

		if (!lost) {
			char name[256];
			// exact part first (cheap), then a directory scan
			memcpy(real + rl, "/", 1);
			memcpy(real + rl + 1, start, len);
			real[rl + 1 + len] = '\0';
			if (lv2Exists(real, NULL)) {
				rl += 1 + len;
				continue;
			}
			real[rl] = '\0';
			if (len < (int)sizeof(name) && findInDir(real, start, len, name, sizeof(name))) {
				rl += snprintf(real + rl, sizeof(real) - rl, "/%s", name);
				continue;
			}
			lost = true;
		}
		real[rl++] = '/';
		memcpy(real + rl, start, len);
		rl += len;
		real[rl] = '\0';
	}
	if (rl == 0) {
		real[rl++] = '/';
		real[rl] = '\0';
	}

	if (!lost)
		cacheStore(norm, h, real);
	snprintf(out, outSize, "%s", real);
	return 1;
}

extern "C" int
PS3_chdir(const char *path)
{
	char abs[PS3_PATH_MAX];
	if (!PS3_ResolvePath(path, abs, sizeof(abs)))
		return -1;
	snprintf(g_cwd, sizeof(g_cwd), "%s", abs);
	return 0;
}

extern "C" char*
PS3_getcwd(char *buf, int size)
{
	snprintf(buf, size, "%s", g_cwd);
	return buf;
}

// ---- re3's crossplatform functions ----------------------------------------

// Always returns the real absolute path (lv2 has no relative paths), to be
// freed by the caller.
char*
casepath(char const *path, bool checkPathFirst)
{
	char abs[PS3_PATH_MAX];
	(void)checkPathFirst;
	if (!PS3_ResolvePath(path, abs, sizeof(abs)))
		return nil;
	return strdup(abs);
}

extern "C" FILE *__real_fopen(const char *path, const char *mode);

FILE*
_fcaseopen(char const *filename, char const *mode)
{
	char abs[PS3_PATH_MAX];
	if (!PS3_ResolvePath(filename, abs, sizeof(abs)))
		return nil;
	return __real_fopen(abs, mode);
}

HANDLE
FindFirstFile(const char *pathname, WIN32_FIND_DATA *firstfile)
{
	char pathCopy[MAX_PATH];
	char abs[PS3_PATH_MAX];

	snprintf(pathCopy, sizeof(pathCopy), "%s", pathname);
	char *star = strchr(pathCopy, '*');
	const char *extension = "";
	if (star) {
		*star = '\0';
		extension = star + 1;
	}
	if (!PS3_ResolvePath(pathCopy, abs, sizeof(abs)))
		return NULL;

	snprintf(firstfile->folder, sizeof(firstfile->folder), "%s", abs);
	if (strlen(abs) >= sizeof(firstfile->folder))
		return NULL;	// folder[] is only 32 chars in re3
	snprintf(firstfile->extension, sizeof(firstfile->extension), "%s", extension);

	DIR *d = opendir(firstfile->folder);
	if (d == NULL)
		return NULL;
	if (!FindNextFile(d, firstfile)) {
		closedir(d);
		return NULL;
	}
	return d;
}

bool
FindNextFile(HANDLE d, WIN32_FIND_DATA *finddata)
{
	struct dirent *file;
	char path[PS3_PATH_MAX];
	int extensionLen = (int)strlen(finddata->extension);

	while ((file = readdir((DIR*)d)) != NULL) {
		int nameLen = (int)strlen(file->d_name);
		if (extensionLen && (nameLen < extensionLen ||
		    strncasecmp(&file->d_name[nameLen - extensionLen], finddata->extension, extensionLen) != 0))
			continue;
		snprintf(path, sizeof(path), "%s/%s", finddata->folder, file->d_name);
		sysFSStat st;
		if (!lv2Exists(path, &st) || (st.st_mode & 0170000) == 0040000)
			continue;	// regular files only
		snprintf(finddata->cFileName, sizeof(finddata->cFileName), "%s", file->d_name);
		finddata->ftLastWriteTime = st.st_mtime;
		return true;
	}
	return false;
}

void
GetDateFormat(int, int, SYSTEMTIME *in, int, char *out, int size)
{
	snprintf(out, size, "%02d/%02d/%04d", in->wMonth, in->wDay, in->wYear);
}

extern void tmToSystemTime(const tm *tm, SYSTEMTIME *out);

void
FileTimeToSystemTime(time_t *writeTime, SYSTEMTIME *out)
{
	tm *ptm = gmtime(writeTime);
	tmToSystemTime(ptm, out);
}

// ---- link-time wrappers: relative paths for libc calls --------------------

static const char*
resolveArg(const char *path, char *buf, int size)
{
	if (path == NULL)
		return path;
	if (path[0] == '/' && strchr(path, '\\') == NULL)
		return path;	// already a real absolute path
	if (!PS3_ResolvePath(path, buf, size))
		return path;
	return buf;
}

extern "C" {

int __real_remove(const char *path);
int __real_rename(const char *from, const char *to);
int __real_mkdir(const char *path, mode_t mode);
int __real_stat(const char *path, struct stat *st);
DIR *__real_opendir(const char *path);
int __real_unlink(const char *path);

FILE*
__wrap_fopen(const char *path, const char *mode)
{
	char buf[PS3_PATH_MAX];
	return __real_fopen(resolveArg(path, buf, sizeof(buf)), mode);
}

int
__wrap_remove(const char *path)
{
	char buf[PS3_PATH_MAX];
	return __real_remove(resolveArg(path, buf, sizeof(buf)));
}

int
__wrap_rename(const char *from, const char *to)
{
	char b1[PS3_PATH_MAX], b2[PS3_PATH_MAX];
	return __real_rename(resolveArg(from, b1, sizeof(b1)), resolveArg(to, b2, sizeof(b2)));
}

int
__wrap_mkdir(const char *path, mode_t mode)
{
	char buf[PS3_PATH_MAX];
	return __real_mkdir(resolveArg(path, buf, sizeof(buf)), mode);
}

int
__wrap_stat(const char *path, struct stat *st)
{
	char buf[PS3_PATH_MAX];
	return __real_stat(resolveArg(path, buf, sizeof(buf)), st);
}

DIR*
__wrap_opendir(const char *path)
{
	char buf[PS3_PATH_MAX];
	return __real_opendir(resolveArg(path, buf, sizeof(buf)));
}

int
__wrap_unlink(const char *path)
{
	char buf[PS3_PATH_MAX];
	return __real_unlink(resolveArg(path, buf, sizeof(buf)));
}

}
