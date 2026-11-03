// ps3_spu.cpp -- from res-PS3 (patch 50): an SPU program as a job server.
//
// One SPU thread group of one thread per program, started from an ELF in the
// EBOOT (Makefile: .incbin). The PPU writes a job's address to the thread's
// signal 1 and waits on the program's event queue; the program answers with an
// event (data0 = the job's sequence number, 24 bits) and waits for the next
// one. 0 ends it. The audio mixer (sampman_ps3.cpp) keeps its own
// copy of this; the particles (Particle.cpp) use this one.

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/spu.h>
#include <sys/event_queue.h>
#include <sys/systime.h>

#include "ps3_platform.h"

// the names of the SPU attributes are 32-bit pointers (ATTRIBUTE_PRXPTR)
static void
SetPrxPtr(void *field, size_t size, const char *p)
{
	if(size == 4){
		u32 ea = (u32)(uintptr_t)p;
		memcpy(field, &ea, 4);
	}else{
		u64 ea = (uintptr_t)p;
		memcpy(field, &ea, 8);
	}
}

static_assert(sizeof(((PS3SpuTask*)0)->image) == sizeof(sysSpuImage), "PS3SpuTask image");
static_assert(sizeof(sys_event_queue_t) <= sizeof(uint64_t), "PS3SpuTask queue");

bool
PS3_SpuTaskStart(PS3SpuTask *t, const void *elf, const char *name, uint8_t port)
{
	memset(t, 0, sizeof(*t));
	t->name = name;
	t->port = port;
	s32 r = sysSpuInitialize(6, 0);
	(void)r;	// done before (the audio mixer): fine
	r = sysSpuImageImport((sysSpuImage*)t->image, elf, 0);
	if(r != 0){
		PS3_Logf("[spu] %s: sysSpuImageImport failed (%08x)", name, (unsigned)r);
		return false;
	}
	sys_event_queue_attr_t qattr;
	memset(&qattr, 0, sizeof(qattr));
	qattr.attr_protocol = SYS_EVENT_QUEUE_FIFO;
	qattr.type = SYS_EVENT_QUEUE_PPU;
	strncpy(qattr.name, name, sizeof(qattr.name));
	sys_event_queue_t q;
	r = sysEventQueueCreate(&q, &qattr, SYS_EVENT_QUEUE_KEY_LOCAL, 8);
	if(r != 0){
		PS3_Logf("[spu] %s: sysEventQueueCreate failed (%08x)", name, (unsigned)r);
		sysSpuImageClose((sysSpuImage*)t->image);
		return false;
	}
	t->queue = q;
	sysSpuThreadGroupAttribute gattr;
	memset(&gattr, 0, sizeof(gattr));
	gattr.nsize = strlen(name) + 1;
	SetPrxPtr(&gattr.name, sizeof(gattr.name), name);
	sys_spu_group_t group;
	r = sysSpuThreadGroupCreate(&group, 1, 100, &gattr);
	if(r != 0){
		PS3_Logf("[spu] %s: sysSpuThreadGroupCreate failed (%08x)", name, (unsigned)r);
		sysEventQueueDestroy(q, 0);
		sysSpuImageClose((sysSpuImage*)t->image);
		return false;
	}
	t->group = group;
	sysSpuThreadAttribute tattr;
	memset(&tattr, 0, sizeof(tattr));
	SetPrxPtr(&tattr.name, sizeof(tattr.name), name);
	tattr.nsize = strlen(name) + 1;
	tattr.option = SPU_THREAD_ATTR_NONE;
	sysSpuThreadArgument targ;
	memset(&targ, 0, sizeof(targ));
	sys_spu_thread_t thread;
	r = sysSpuThreadInitialize(&thread, group, 0, (sysSpuImage*)t->image, &tattr, &targ);
	if(r == 0)
		r = sysSpuThreadSetConfiguration(thread, SPU_SIGNAL1_OVERWRITE | SPU_SIGNAL2_OVERWRITE);
	if(r == 0)
		r = sysSpuThreadConnectEvent(thread, q, SPU_THREAD_EVENT_USER, port);
	if(r == 0)
		r = sysSpuThreadGroupStart(group);
	if(r != 0){
		PS3_Logf("[spu] %s: SPU thread setup failed (%08x)", name, (unsigned)r);
		sysSpuThreadGroupDestroy(group);
		sysEventQueueDestroy(q, 0);
		sysSpuImageClose((sysSpuImage*)t->image);
		return false;
	}
	t->thread = thread;
	t->ready = true;
	PS3_Logf("[spu] %s on SPU thread %08x", name, (unsigned)thread);
	return true;
}

bool
PS3_SpuTaskKick(PS3SpuTask *t, uint32_t jobEa)
{
	if(!t->ready)
		return false;
	sysEventQueueDrain(t->queue);
	__asm__ volatile("sync" ::: "memory");
	return sysSpuThreadWriteSignal(t->thread, 0, jobEa) == 0;
}

bool
PS3_SpuTaskWait(PS3SpuTask *t, uint32_t seq, uint32_t timeoutUs, uint64_t *waitUs)
{
	u64 t0 = sysGetSystemTime();
	sys_event_t ev;
	s32 r = sysEventQueueReceive(t->queue, &ev, timeoutUs);
	if(waitUs)
		*waitUs += sysGetSystemTime() - t0;
	if(r != 0 || (ev.data_2 & 0xFFFFFF) != (seq & 0xFFFFFF)){
		t->ready = false;	// not joined: it may still be running
		PS3_Logf("[spu] %s didn't answer job %u (%08x): the PPU does it from now on", t->name, seq, (unsigned)r);
		return false;
	}
	__asm__ volatile("sync" ::: "memory");
	return true;
}

void
PS3_SpuTaskStop(PS3SpuTask *t)
{
	if(!t->ready)
		return;
	t->ready = false;
	u32 cause, status;
	sysSpuThreadWriteSignal(t->thread, 0, 0);
	sysSpuThreadGroupJoin(t->group, &cause, &status);
	sysSpuThreadDisconnectEvent(t->thread, SPU_THREAD_EVENT_USER, t->port);
	sysSpuThreadGroupDestroy(t->group);
	sysEventQueueDestroy(t->queue, 0);
	sysSpuImageClose((sysSpuImage*)t->image);
	PS3_Logf("[spu] %s stopped", t->name);
}
