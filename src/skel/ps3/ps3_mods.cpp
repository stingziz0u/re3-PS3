// ps3_mods.cpp -- USRDIR/mods, the way a PC mod loader does it: a file there
// (in any subfolder, any letter case) replaces the game's file of the same
// name, whether that one is a loose file (models/hud.txd, data/handling.cfg,
// main.scm...) or an entry of a .img (weapons.txd, infernus.dff...).
//
//  - Loose files: the fopen wrapper of ps3_fs.cpp asks PS3_ModsOpenPath for
//    every file opened for reading inside a game folder of USRDIR (not the
//    USRDIR root, with the ini, the settings and the logs, and not userfiles/,
//    mods/ or mp3/).
//  - .img entries: CStreaming::LoadCdDirectory asks PS3_ModsImgEntry for every
//    entry of every .dir. A replaced entry points into a virtual image (index
//    PS3_MODS_IMAGE) where every mod file has its own sectors; CdStreamRead
//    reads those from the file itself (PS3_ModsRead).
//
// Nothing is copied or converted: the files are read as they are. The list
// of mod files is made once, at the first lookup.

#include "common.h"
#include "crossplatform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/file.h>

#include "CdStream.h"
#include "ps3_platform.h"

#define MODS_DIR PS3_USRDIR "/mods"
#define MODS_MAX_DEPTH 6

struct ModFile {
	char *name;		// lowercase base name, the key
	char *path;		// absolute
	uint64 size;
	uint32 start;		// first sector in the virtual image (if used there)
	uint32 sectors;
	bool inImage;
	bool logged;
};

static ModFile *g_mods;
static int g_numMods, g_capMods;
static int *g_table;		// index+1 into g_mods, 0 = empty
static int g_tableSize;		// power of 2
static bool g_scanned;
static uint32 g_nextSector = 1;	// sector 0 left unused
static int g_imgEntries;

// image entries in sector order, for PS3_ModsRead
static int *g_imgOrder;
static int g_numImg, g_capImg;

static unsigned
hashLower(const char *s)
{
	unsigned h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (unsigned char)tolower((unsigned char)*s)) * 16777619u;
	return h;
}

static int
findMod(const char *name)
{
	if (g_table == nil)
		return -1;
	int j = hashLower(name) & (g_tableSize - 1);
	while (g_table[j]) {
		int i = g_table[j] - 1;
		if (strcasecmp(g_mods[i].name, name) == 0)
			return i;
		j = (j + 1) & (g_tableSize - 1);
	}
	return -1;
}

static void
addMod(const char *path, const char *name, uint64 size)
{
	if (g_numMods == g_capMods) {
		int ncap = g_capMods ? g_capMods * 2 : 64;
		ModFile *n = (ModFile*)realloc(g_mods, ncap * sizeof(ModFile));
		if (n == nil)
			return;
		g_mods = n;
		g_capMods = ncap;
	}
	ModFile *m = &g_mods[g_numMods++];
	memset(m, 0, sizeof(*m));
	m->name = strdup(name);
	for (char *p = m->name; *p; p++)
		*p = tolower((unsigned char)*p);
	m->path = strdup(path);
	m->size = size;
}

static void
scanDir(const char *dir, int depth)
{
	s32 fd;
	sysFSDirent ent;
	u64 nread;
	if (depth > MODS_MAX_DEPTH || sysLv2FsOpenDir(dir, &fd) != 0)
		return;
	for (;;) {
		nread = 0;
		if (sysLv2FsReadDir(fd, &ent, &nread) != 0 || nread == 0)
			break;
		if (ent.d_name[0] == '.')
			continue;	// ".", ".." and hidden files
		char path[1024];
		if (snprintf(path, sizeof(path), "%s/%s", dir, ent.d_name) >= (int)sizeof(path))
			continue;
		sysFSStat st;
		if (sysLv2FsStat(path, &st) != 0)
			continue;
		if ((st.st_mode & 0170000) == 0040000)
			scanDir(path, depth + 1);
		else if (strcasecmp(ent.d_name, "readme.txt") != 0)
			addMod(path, ent.d_name, (uint64)st.st_size);
	}
	sysLv2FsCloseDir(fd);
}

static int
cmpPath(const void *a, const void *b)
{
	return strcasecmp(((const ModFile*)a)->path, ((const ModFile*)b)->path);
}

static void
scanMods(void)
{
	if (g_scanned)
		return;
	g_scanned = true;
	scanDir(MODS_DIR, 0);
	if (g_numMods == 0) {
		PS3_Log("[mods] USRDIR/mods: no files");
		return;
	}
	// the same name twice: the first path in alphabetical order wins
	qsort(g_mods, g_numMods, sizeof(ModFile), cmpPath);
	g_tableSize = 16;
	while (g_tableSize < g_numMods * 2)
		g_tableSize *= 2;
	g_table = (int*)calloc(g_tableSize, sizeof(int));
	int unique = 0;
	for (int i = 0; g_table && i < g_numMods; i++) {
		int prev = findMod(g_mods[i].name);
		if (prev >= 0) {
			PS3_Logf("[mods] %s twice: %s is used, %s is not", g_mods[i].name,
			         g_mods[prev].path + strlen(MODS_DIR) + 1, g_mods[i].path + strlen(MODS_DIR) + 1);
			continue;
		}
		int j = hashLower(g_mods[i].name) & (g_tableSize - 1);
		while (g_table[j])
			j = (j + 1) & (g_tableSize - 1);
		g_table[j] = i + 1;
		unique++;
	}
	PS3_Logf("[mods] USRDIR/mods: %d files", unique);
}

// ---- loose files -----------------------------------------------------------

// abspath: the real path the game opens for reading. Returns the mod file
// to open instead, or nil.
extern "C" const char*
PS3_ModsOpenPath(const char *abspath)
{
	static const char usrdir[] = PS3_USRDIR "/";
	if (strncmp(abspath, usrdir, sizeof(usrdir) - 1) != 0)
		return nil;
	const char *rel = abspath + sizeof(usrdir) - 1;
	const char *slash = strchr(rel, '/');
	if (slash == nil)
		return nil;	// USRDIR itself: ini, settings, logs, VERSION.TXT
	int top = (int)(slash - rel);
	if ((top == 9 && strncasecmp(rel, "userfiles", 9) == 0) ||
	    (top == 4 && strncasecmp(rel, "mods", 4) == 0) ||
	    (top == 3 && strncasecmp(rel, "mp3", 3) == 0))
		return nil;

	scanMods();
	if (g_numMods == 0)
		return nil;
	const char *base = strrchr(abspath, '/') + 1;
	int i = findMod(base);
	if (i < 0)
		return nil;
	if (!g_mods[i].logged) {
		g_mods[i].logged = true;
		PS3_Logf("[mods] %s <- mods/%s", rel, g_mods[i].path + strlen(MODS_DIR) + 1);
	}
	return g_mods[i].path;
}

// ---- .img entries ----------------------------------------------------------

// name: the entry's name with its extension. On a mod file, offset and size
// (sectors) become the file's place in the virtual image, whose index is
// PS3_MODS_IMAGE.
bool
PS3_ModsImgEntry(const char *name, uint32 *offset, uint32 *size)
{
	scanMods();
	if (g_numMods == 0)
		return false;
	int i = findMod(name);
	if (i < 0)
		return false;
	ModFile *m = &g_mods[i];
	if (!m->inImage) {
		uint64 sectors = (m->size + CDSTREAM_SECTOR_SIZE - 1) / CDSTREAM_SECTOR_SIZE;
		if (sectors == 0 || g_nextSector + sectors > 0xFFFFFF) {
			PS3_Logf("[mods] %s not used: %s", name, sectors ? "the virtual image is full" : "empty file");
			return false;
		}
		if (g_numImg == g_capImg) {
			int ncap = g_capImg ? g_capImg * 2 : 64;
			int *n = (int*)realloc(g_imgOrder, ncap * sizeof(int));
			if (n == nil)
				return false;
			g_imgOrder = n;
			g_capImg = ncap;
		}
		m->inImage = true;
		m->start = g_nextSector;
		m->sectors = (uint32)sectors;
		g_nextSector += m->sectors;
		g_imgOrder[g_numImg++] = i;
		g_imgEntries++;
		PS3_Logf("[mods] %s (img, %u KB) <- mods/%s", name, (unsigned)((m->size + 1023) / 1024),
		         m->path + strlen(MODS_DIR) + 1);
	}
	*offset = m->start;
	*size = m->sectors;
	return true;
}

// A few mod files stay open (a texture pack's TXDs are read again and again).
#define MODS_OPEN_FDS 8
static struct { int mod; s32 fd; unsigned lastUse; } g_open[MODS_OPEN_FDS];
static unsigned g_openUse;

static s32
modFd(int mod)
{
	int slot = 0;
	for (int i = 0; i < MODS_OPEN_FDS; i++) {
		if (g_open[i].lastUse && g_open[i].mod == mod) {
			g_open[i].lastUse = ++g_openUse;
			return g_open[i].fd;
		}
		if (g_open[i].lastUse < g_open[slot].lastUse)
			slot = i;
	}
	if (g_open[slot].lastUse)
		sysLv2FsClose(g_open[slot].fd);
	g_open[slot].lastUse = 0;
	s32 fd;
	if (sysLv2FsOpen(g_mods[mod].path, SYS_O_RDONLY, &fd, 0, NULL, 0) != 0)
		return -1;
	g_open[slot].mod = mod;
	g_open[slot].fd = fd;
	g_open[slot].lastUse = ++g_openUse;
	return fd;
}

// CdStreamRead on the virtual image. Returns false on an error.
bool
PS3_ModsRead(uint32 sector, void *buffer, uint32 sectors)
{
	// the entry holding that sector (g_imgOrder is in sector order)
	int lo = 0, hi = g_numImg - 1, found = -1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		ModFile *m = &g_mods[g_imgOrder[mid]];
		if (sector < m->start)
			hi = mid - 1;
		else if (sector >= m->start + m->sectors)
			lo = mid + 1;
		else {
			found = g_imgOrder[mid];
			break;
		}
	}
	uint64 want = (uint64)sectors * CDSTREAM_SECTOR_SIZE;
	if (found < 0) {
		PS3_Logf("[mods] read of sector %u: no mod file there", sector);
		return false;
	}
	ModFile *m = &g_mods[found];
	uint64 pos = (uint64)(sector - m->start) * CDSTREAM_SECTOR_SIZE;
	uint64 have = pos < m->size ? m->size - pos : 0;
	uint64 len = want < have ? want : have;
	s32 fd = modFd(found);
	u64 got = 0, at = 0;
	if (fd < 0 || sysLv2FsLSeek64(fd, pos, 0, &at) != 0 ||
	    (len && sysLv2FsRead(fd, buffer, len, &got) != 0) || got != len) {
		PS3_Logf("[mods] can't read %s (%llu bytes at %llu)", m->path,
		         (unsigned long long)len, (unsigned long long)pos);
		return false;
	}
	if (len < want)
		memset((uint8*)buffer + len, 0, want - len);	// the last sector's tail
	return true;
}
