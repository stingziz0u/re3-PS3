// ps3_cdstream.cpp -- CdStream (the .img streaming reads) for the PS3.
//
// Patch 68: a reader thread. CdStreamRead used to do the read right away
// with lv2 calls, the game thread waiting for the disk: 8-18 ms in the
// frames that started a read (patch 67's [loadreq] lines, Ren's drive
// around the city: every one of the 80 was CdStreamRead). Now it queues the
// read and returns; the thread reads while the game goes on, and the
// channel says READING until it's done, as the PC's CdStream. CdStreamSync
// waits for it (loading screens, LoadAllRequestedModels). The mods' files
// (USRDIR/mods) are read as before. USRDIR/nocdthread.txt: synchronous, as
// before.

#include "common.h"
#include "crossplatform.h"

#include <string.h>
#include <sys/file.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#include <sys/systime.h>

#include "CdStream.h"
#include "rwcore.h"

#include "ps3_platform.h"

#ifdef FLUSHABLE_STREAMING
bool flushStream[MAX_CDCHANNELS];
#endif

struct CdChannel
{
	volatile int32 nStatus;
	// patch 68: the read queued for the reader thread
	volatile bool busy;	// queued or being read (the buffer is the thread's)
	void *buffer;
	int32 img;
	uint32 sector, sectors;
	uint32 seq;		// order of the requests (the oldest first)
};

static char gCdImageNames[MAX_CDIMAGES+1][64];
static s32 gImgFd[MAX_CDIMAGES];
static bool gImgOpen[MAX_CDIMAGES];
static int32 gNumImages;
static int32 gNumChannels;
static CdChannel *gChannels;
static int32 lastPosnRead;

// stats for the log
uint32 gPS3CdReads, gPS3CdErrors;
uint64 gPS3CdBytes;

// ---- patch 68: the reader thread ----
static bool gCdThreadOn;
static sys_ppu_thread_t gCdThread;
static sys_mutex_t gCdMutex;
static sys_cond_t gCdWork, gCdDone;
static volatile bool gCdQuit;
static uint32 gCdSeq;
uint64 gPS3CdThreadUs;	// time the thread spent reading (instead of the game thread)
uint32 gPS3CdSyncWaits;	// CdStreamSync calls that had to wait for it

// one read with lv2 calls; the channel's status after it
static int32
ReadSectors(int32 img, void *buffer, uint32 sector, uint32 sectors)
{
	u64 pos = 0, nread = 0;
	u64 bytes = (u64)sectors * CDSTREAM_SECTOR_SIZE;
	s32 ret = sysLv2FsLSeek64(gImgFd[img], (u64)sector * CDSTREAM_SECTOR_SIZE, 0, &pos);
	if (ret == 0)
		ret = sysLv2FsRead(gImgFd[img], buffer, bytes, &nread);
	gPS3CdReads++;
	gPS3CdBytes += nread;
	// a short read at the end of the image is fine (the last file)
	if (ret != 0 || nread == 0) {
		gPS3CdErrors++;
		if (gPS3CdErrors <= 20)
			PS3_Logf("[cd] read error 0x%08x: image %d sector %u, %u sectors, got %llu bytes",
			         (unsigned)ret, img, sector, sectors, (unsigned long long)nread);
		return STREAM_ERROR;
	}
	return STREAM_NONE;
}

static void
CdThreadMain(void *arg)
{
	(void)arg;
	sysMutexLock(gCdMutex, 0);
	while (!gCdQuit) {
		// the oldest queued read
		CdChannel *ch = nil;
		for (int32 i = 0; i < gNumChannels; i++)
			if (gChannels[i].busy && (ch == nil || (int32)(gChannels[i].seq - ch->seq) < 0))
				ch = &gChannels[i];
		if (ch == nil) {
			sysCondWait(gCdWork, 0);
			continue;
		}
		sysMutexUnlock(gCdMutex);
		u64 t0 = sysGetSystemTime();
		int32 status = ReadSectors(ch->img, ch->buffer, ch->sector, ch->sectors);
		gPS3CdThreadUs += sysGetSystemTime() - t0;
		sysMutexLock(gCdMutex, 0);
		ch->nStatus = status;
		ch->busy = false;
		sysCondBroadcast(gCdDone);
	}
	sysMutexUnlock(gCdMutex);
	sysThreadExit(0);
}

static void
CdThreadStart(void)
{
	FILE *f = fopen(PS3_USRDIR "/nocdthread.txt", "rb");
	if (f) {
		fclose(f);
		PS3_Logf("[cd] reads on the game thread (nocdthread.txt)");
		return;
	}
	sys_mutex_attr_t ma;
	memset(&ma, 0, sizeof(ma));
	ma.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
	ma.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
	ma.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
	ma.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	strncpy(ma.name, "cdread", sizeof(ma.name)-1);
	if (sysMutexCreate(&gCdMutex, &ma) != 0) {
		PS3_Logf("[cd] reader thread: no mutex, reads on the game thread");
		return;
	}
	sys_cond_attr_t ca;
	sysCondAttrInitialize(ca);
	if (sysCondCreate(&gCdWork, gCdMutex, &ca) != 0 || sysCondCreate(&gCdDone, gCdMutex, &ca) != 0) {
		PS3_Logf("[cd] reader thread: no condition variables, reads on the game thread");
		return;
	}
	gCdQuit = false;
	// below the game thread (1000): it mostly waits for the disk, and when
	// it has work the PPU's other hardware thread takes it
	if (sysThreadCreate(&gCdThread, CdThreadMain, NULL, 1050, 64*1024, THREAD_JOINABLE, (char*)"cdread") != 0) {
		PS3_Logf("[cd] reader thread not created, reads on the game thread");
		return;
	}
	gCdThreadOn = true;
	PS3_Logf("[cd] reader thread on (nocdthread.txt turns it off)");
}

static void
CdThreadStop(void)
{
	if (!gCdThreadOn)
		return;
	sysMutexLock(gCdMutex, 0);
	gCdQuit = true;
	sysCondSignal(gCdWork);
	sysMutexUnlock(gCdMutex);
	u64 rv;
	sysThreadJoin(gCdThread, &rv);
	sysCondDestroy(gCdWork);
	sysCondDestroy(gCdDone);
	sysMutexDestroy(gCdMutex);
	gCdThreadOn = false;
}

void
CdStreamInitThread(void)
{
}

void
CdStreamInit(int32 numChannels)
{
	gNumImages = 0;
	gNumChannels = numChannels;
	gChannels = (CdChannel*)calloc(numChannels, sizeof(CdChannel));
	CdThreadStart();
}

uint32
GetGTA3ImgSize(void)
{
	char abs[1024];
	long long size;
	if (!PS3_ResolvePath(gCdImageNames[0], abs, sizeof(abs)) || !PS3_FileSize(abs, &size))
		return 0;
	return (uint32)size;
}

void
CdStreamShutdown(void)
{
	CdThreadStop();
	CdStreamRemoveImages();
	free(gChannels);
	gChannels = nil;
}

int32
CdStreamRead(int32 channel, void *buffer, uint32 offset, uint32 size)
{
	ASSERT(channel < gNumChannels);
	lastPosnRead = size + offset;

	int32 img = _GET_INDEX(offset);
	CdChannel *ch = &gChannels[channel];
	if (img == PS3_MODS_IMAGE) {
		// USRDIR/mods files that replace .img entries (ps3_mods.cpp)
		gPS3CdReads++;
		if (PS3_ModsRead(_GET_OFFSET(offset), buffer, size)) {
			gPS3CdBytes += (uint64)size * CDSTREAM_SECTOR_SIZE;
			ch->nStatus = STREAM_NONE;
		} else {
			gPS3CdErrors++;
			ch->nStatus = STREAM_ERROR;
		}
		return STREAM_SUCCESS;
	}
	if (img >= MAX_CDIMAGES || !gImgOpen[img]) {
		ch->nStatus = STREAM_ERROR;
		gPS3CdErrors++;
		return STREAM_SUCCESS;
	}

	if (gCdThreadOn) {
		// patch 68: queued for the reader thread
		sysMutexLock(gCdMutex, 0);
		if (ch->busy) {
			// still reading the last one: as the PC's CdStream, not taken
			sysMutexUnlock(gCdMutex);
			return STREAM_NONE;
		}
		ch->buffer = buffer;
		ch->img = img;
		ch->sector = _GET_OFFSET(offset);
		ch->sectors = size;
		ch->seq = gCdSeq++;
		ch->nStatus = STREAM_NONE;
		ch->busy = true;
		sysCondSignal(gCdWork);
		sysMutexUnlock(gCdMutex);
		return STREAM_SUCCESS;
	}
	ch->nStatus = ReadSectors(img, buffer, _GET_OFFSET(offset), size);
	return STREAM_SUCCESS;
}

int32
CdStreamGetStatus(int32 channel)
{
	CdChannel *ch = &gChannels[channel];
	if (ch->busy)
		return STREAM_READING;
	__asm__ volatile("sync" ::: "memory");	// the thread's writes to the buffer before ours
	int32 status = ch->nStatus;
	ch->nStatus = STREAM_NONE;
	return status;
}

int32
CdStreamGetLastPosn(void)
{
	return lastPosnRead;
}

int32
CdStreamSync(int32 channel)
{
#ifdef FLUSHABLE_STREAMING
	flushStream[channel] = false;
#endif
	if (gCdThreadOn && gChannels[channel].busy) {
		gPS3CdSyncWaits++;
		sysMutexLock(gCdMutex, 0);
		while (gChannels[channel].busy)
			sysCondWait(gCdDone, 0);
		sysMutexUnlock(gCdMutex);
	}
	__asm__ volatile("sync" ::: "memory");
	return gChannels[channel].nStatus;
}

void
AddToQueue(Queue *queue, int32 item)
{
	queue->items[queue->tail] = item;
	queue->tail = (queue->tail + 1) % queue->size;
}

int32
GetFirstInQueue(Queue *queue)
{
	if (queue->head == queue->tail)
		return -1;
	return queue->items[queue->head];
}

void
RemoveFirstInQueue(Queue *queue)
{
	if (queue->head == queue->tail)
		return;
	queue->head = (queue->head + 1) % queue->size;
}

bool
CdStreamAddImage(char const *path)
{
	char abs[1024];
	s32 fd;

	ASSERT(gNumImages < MAX_CDIMAGES);
	if (gNumImages >= PS3_MODS_IMAGE) {
		// the last index is USRDIR/mods' (ps3_mods.cpp)
		PS3_Logf("[cd] image %s not added: %d images at most", path, PS3_MODS_IMAGE);
		return false;
	}
	if (!PS3_ResolvePath(path, abs, sizeof(abs)) ||
	    sysLv2FsOpen(abs, SYS_O_RDONLY, &fd, 0, NULL, 0) != 0) {
		PS3_Logf("[cd] can't open image %s (%s)", path, abs);
		return false;
	}
	gImgFd[gNumImages] = fd;
	gImgOpen[gNumImages] = true;
	snprintf(gCdImageNames[gNumImages], sizeof(gCdImageNames[0]), "%s", path);
	PS3_Logf("[cd] image %d: %s", gNumImages, abs);
	gNumImages++;
	return true;
}

char*
CdStreamGetImageName(int32 cd)
{
	ASSERT(cd < MAX_CDIMAGES);
	if (gImgOpen[cd])
		return gCdImageNames[cd];
	return nil;
}

void
CdStreamRemoveImages(void)
{
	// patch 68: no read of the thread left on a file being closed
	for (int32 c = 0; c < gNumChannels; c++)
		CdStreamSync(c);
	for (int32 i = 0; i < gNumImages; i++) {
		if (gImgOpen[i])
			sysLv2FsClose(gImgFd[i]);
		gImgOpen[i] = false;
	}
	gNumImages = 0;
}

int32
CdStreamGetNumImages(void)
{
	return gNumImages;
}
