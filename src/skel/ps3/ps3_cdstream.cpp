// ps3_cdstream.cpp -- CdStream (the .img streaming reads) for the PS3.
//
// Synchronous: CdStreamRead does the read right away with lv2 calls and the
// channel is done when it returns. The streaming code handles that like an
// instant read. (A reader thread can come later if loading stutters.)

#include "common.h"
#include "crossplatform.h"

#include <sys/file.h>

#include "CdStream.h"
#include "rwcore.h"

#include "ps3_platform.h"

#ifdef FLUSHABLE_STREAMING
bool flushStream[MAX_CDCHANNELS];
#endif

struct CdChannel
{
	int32 nStatus;
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
	if (img >= MAX_CDIMAGES || !gImgOpen[img]) {
		ch->nStatus = STREAM_ERROR;
		gPS3CdErrors++;
		return STREAM_SUCCESS;
	}

	u64 pos = 0, nread = 0;
	u64 bytes = (u64)size * CDSTREAM_SECTOR_SIZE;
	s32 ret = sysLv2FsLSeek64(gImgFd[img], (u64)_GET_OFFSET(offset) * CDSTREAM_SECTOR_SIZE, 0, &pos);
	if (ret == 0)
		ret = sysLv2FsRead(gImgFd[img], buffer, bytes, &nread);
	gPS3CdReads++;
	gPS3CdBytes += nread;
	// a short read at the end of the image is fine (the last file)
	if (ret != 0 || nread == 0) {
		ch->nStatus = STREAM_ERROR;
		gPS3CdErrors++;
		if (gPS3CdErrors <= 20)
			PS3_Logf("[cd] read error 0x%08x: image %d sector %u, %u sectors, got %llu bytes",
			         (unsigned)ret, img, _GET_OFFSET(offset), size, (unsigned long long)nread);
	} else
		ch->nStatus = STREAM_NONE;
	return STREAM_SUCCESS;
}

int32
CdStreamGetStatus(int32 channel)
{
	CdChannel *ch = &gChannels[channel];
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
