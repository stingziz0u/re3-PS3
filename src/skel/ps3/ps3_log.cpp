// ps3_log.cpp -- log file in USRDIR: thread-safe, rotated at boot (the
// previous run stays as re3-ps3.old.log), size-capped, every line flushed so
// a hang or a crash still leaves the last line on disk.

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/thread.h>

#include <sys/mutex.h>

#include "ps3_platform.h"

#define PS3_STACK_WALK_MAX (8 * 1024 * 1024)	// the game thread has 4 MB

extern "C" FILE *__real_fopen(const char *path, const char *mode);

#define PS3_LOG_MAX_BYTES (4 * 1024 * 1024)

static FILE *log_file;
static long log_bytes;
static int log_capped;
static sys_mutex_t log_mutex;
static int log_mutex_ok;

// set by the exception handler: the stopped game thread may hold the mutex
static volatile int log_crashing;
static volatile int log_buffered;	// the watchdog flushes the log (see log_write)

static void log_lock(void)
{
	if (!log_mutex_ok)
		return;
	if (log_crashing)
		sysMutexLock(log_mutex, 300 * 1000);	// at most 0.3 s, then write anyway
	else
		sysMutexLock(log_mutex, 0);
}
static void log_unlock(void) { if (log_mutex_ok) sysMutexUnlock(log_mutex); }

extern "C" void
PS3_LogInit(void)
{
	sys_mutex_attr_t attr;

	if (log_file)
		return;

	sysMutexAttrInitialize(attr);
	attr.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
	log_mutex_ok = (sysMutexCreate(&log_mutex, &attr) == 0);

	remove(PS3_LOG_OLD_PATH);
	rename(PS3_LOG_PATH, PS3_LOG_OLD_PATH);
	log_file = __real_fopen(PS3_LOG_PATH, "w");
	if (log_file) {
		static char log_buf[64 * 1024];
		setvbuf(log_file, log_buf, _IOFBF, sizeof(log_buf));
	}
	log_bytes = 0;
	log_capped = 0;
}

extern "C" void
PS3_LogShutdown(void)
{
	log_lock();
	if (log_file) {
		fclose(log_file);
		log_file = NULL;
	}
	log_unlock();
}

// flushes the log through lv2's cache to the disk (a forced power-off loses
// what is only in the cache)
extern "C" void
PS3_LogSync(void)
{
	log_lock();
	if (log_file) {
		fflush(log_file);
		fsync(fileno(log_file));
	}
	log_unlock();
}

static void
log_write(const char *msg, int newline)
{
	size_t len;

	if (!log_file || log_capped)
		return;

	len = strlen(msg);
	if (log_bytes + (long)len > PS3_LOG_MAX_BYTES) {
		fputs("\n[log] size cap reached, logging stopped\n", log_file);
		fflush(log_file);
		log_capped = 1;
		return;
	}
	fwrite(msg, 1, len, log_file);
	log_bytes += (long)len;
	if (newline && (len == 0 || msg[len - 1] != '\n')) {
		fputc('\n', log_file);
		log_bytes++;
	}
	// Once the watchdog runs, lines stay in the buffer and it writes them
	// out every second: a write to the disk per line stalled the game
	// thread while the streaming read from the same disk (the hitches when
	// new areas load). Crash and hang dumps still go out right away.
	if (!log_buffered || log_crashing || strncmp(msg, "[crash]", 7) == 0 || strncmp(msg, "[hang]", 6) == 0)
		fflush(log_file);
}

extern "C" void
PS3_LogRaw(const char *msg)
{
	log_lock();
	log_write(msg, 0);
	log_unlock();
}

extern "C" void
PS3_Log(const char *msg)
{
	log_lock();
	log_write(msg, 1);
	log_unlock();
}

extern "C" void
PS3_Logf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	PS3_Log(buf);
}

// ---- stdout/stderr of the game and librw go to the log --------------------
// (linked with -Wl,--wrap=printf,--wrap=puts,... see ps3/Makefile)

extern "C" int __real_vfprintf(FILE *f, const char *fmt, va_list ap);
extern "C" int __real_fprintf(FILE *f, const char *fmt, ...);
extern "C" int __real_fputs(const char *s, FILE *f);

static int
log_vprintf(const char *fmt, va_list ap)
{
	char buf[1024];
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	PS3_LogRaw(buf);
	return n;
}

extern "C" int
__wrap_printf(const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = log_vprintf(fmt, ap);
	va_end(ap);
	return n;
}

extern "C" int
__wrap_vprintf(const char *fmt, va_list ap)
{
	return log_vprintf(fmt, ap);
}

extern "C" int
__wrap_puts(const char *s)
{
	PS3_Log(s);
	return 1;
}

extern "C" int
__wrap_putchar(int c)
{
	char buf[2] = { (char)c, 0 };
	PS3_LogRaw(buf);
	return c;
}

extern "C" int
__wrap_vfprintf(FILE *f, const char *fmt, va_list ap)
{
	if (f == stdout || f == stderr)
		return log_vprintf(fmt, ap);
	return __real_vfprintf(f, fmt, ap);
}

extern "C" int
__wrap_fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = __wrap_vfprintf(f, fmt, ap);
	va_end(ap);
	return n;
}

extern "C" int
__wrap_fputs(const char *s, FILE *f)
{
	if (f == stdout || f == stderr) {
		PS3_LogRaw(s);
		return 1;
	}
	return __real_fputs(s, f);
}

// ---- crash breadcrumbs ---------------------------------------------------
// The game thread leaves the last 256 steps here (no I/O, a few stores each).
// When it dies, the main thread is still alive and dumps them to the log.

#define CRUMB_COUNT 256

struct Crumb {
	const char *tag;
	int val;
	unsigned frame;
	unsigned where;		// return address: the code that left the crumb
};
static Crumb crumbs[CRUMB_COUNT];
// set by the game (Ped.cpp): prints the entity being processed at the crash
void (*PS3_crashDumpHook)(void);
void (*PS3_hangDumpHook)(void);	// more for the [hang] dump (sampman_ps3.cpp: the audio threads)
static volatile unsigned crumbHead;
unsigned PS3_crumbFrame;

static char scriptName[9];
static volatile int scriptCommand = -1;
static volatile unsigned scriptIp;
static volatile unsigned scriptCount;

extern "C" __attribute__((noinline)) void
PS3_Crumb(const char *tag, int val)
{
	Crumb *c = &crumbs[crumbHead % CRUMB_COUNT];
	c->tag = tag;
	c->val = val;
	c->frame = PS3_crumbFrame;
	c->where = (unsigned)(uintptr_t)__builtin_return_address(0);
	crumbHead++;
}

// as PS3_Crumb, but with the address given (a function's own caller)
extern "C" void
PS3_CrumbAt(const char *tag, int val, unsigned where)
{
	Crumb *c = &crumbs[crumbHead % CRUMB_COUNT];
	c->tag = tag;
	c->val = val;
	c->frame = PS3_crumbFrame;
	c->where = where;
	crumbHead++;
}

extern "C" void
PS3_ScriptCrumb(const char *name8, int command, unsigned ip)
{
	memcpy(scriptName, name8, 8);
	scriptCommand = command;
	scriptIp = ip;
	scriptCount++;
}

// an angle loop got inf/NaN/a huge angle (common.h PS3_AngleOk): it would
// never have ended
extern "C" __attribute__((noinline)) void
PS3_BadAngle(void)
{
	static int logged;
	unsigned where = (unsigned)(uintptr_t)__builtin_return_address(0);
	PS3_CrumbAt("bad angle in an angle loop", 0, where);
	if (logged < 16) {
		logged++;
		PS3_Logf("[angle] an angle loop got an infinite/NaN/huge angle (set to 0) @%08x", where);
	}
}

// a CPool had no free slot: the new object isn't made (templates.h)
extern "C" void
PS3_PoolFull(const char *name, int size)
{
	static int nBadHandle;
	if (size < 0) {
		// at most 16 lines, then one every 1024 (a caller that does it every frame)
		if (nBadHandle++ < 16 || (nBadHandle & 1023) == 0)
			PS3_Logf("[pool] %s: handle asked for something not in the pool (nil?) @%08x (%d times)", name ? name : "?",
			         (unsigned)(uintptr_t)__builtin_return_address(0), nBadHandle);
		return;
	} else
		PS3_Logf("[pool] %s pool full (%d): an object wasn't created", name ? name : "?", size);
	PS3_Crumb("pool full: size", size);
}

// ---- function trace --------------------------------------------------------
// The objects built with -finstrument-functions (FTRACE_DIRS in ps3/Makefile)
// call these on every function entry and exit (inlined ones too). The ring
// keeps the last FTRACE_COUNT (16384) events: the function (its .opd descriptor),
// bit 0 set on exit. ps3/tools/crashaddr.py turns the [ftrace]
// lines of a log into the call tree at the moment of the crash.

#define FTRACE_COUNT 16384	// power of 2
static unsigned ftrace[FTRACE_COUNT];
static unsigned ftraceHead;

extern "C" __attribute__((no_instrument_function)) void
__cyg_profile_func_enter(void *fn, void *site)
{
	(void)site;
	ftrace[ftraceHead++ & (FTRACE_COUNT-1)] = (unsigned)(uintptr_t)fn;
}

extern "C" __attribute__((no_instrument_function)) void
__cyg_profile_func_exit(void *fn, void *site)
{
	(void)site;
	ftrace[ftraceHead++ & (FTRACE_COUNT-1)] = (unsigned)(uintptr_t)fn | 1;
}

static void
PS3_DumpFtrace(void)
{
	unsigned head = ftraceHead;
	unsigned n = head < FTRACE_COUNT ? head : FTRACE_COUNT;
	char line[16 + 8 * 10];

	if (n == 0)
		return;
	PS3_Logf("[ftrace] %u events (oldest first; function, odd = exit): "
	         "python3 ps3/tools/crashaddr.py <elf> <log>", n);
	for (unsigned i = head - n; i != head; ) {
		int len = sprintf(line, "[ftrace]");
		for (int k = 0; k < 8 && i != head; k++, i++)
			len += sprintf(line + len, " %08x", ftrace[i & (FTRACE_COUNT-1)]);
		PS3_Log(line);
	}
}

extern "C" void
PS3_DumpCrumbs(void)
{
	unsigned head = crumbHead;
	unsigned n = head < CRUMB_COUNT ? head : CRUMB_COUNT;
	char name[9];

	PS3_DumpFtrace();

	memcpy(name, scriptName, 8);
	name[8] = '\0';
	PS3_Logf("[crash] last steps of the game thread (oldest first, frame: step value @ address):");
	for (unsigned i = head - n; i != head; i++) {
		Crumb *c = &crumbs[i % CRUMB_COUNT];
		PS3_Logf("[crash]   %u: %s %d @%08x", c->frame, c->tag ? c->tag : "?", c->val, c->where);
	}
	PS3_Logf("[crash] last script command: script \"%s\" opcode 0x%04x at ip %u (%u commands run)",
	         name, scriptCommand, scriptIp, scriptCount);
	if (PS3_crashDumpHook)
		PS3_crashDumpHook();
}

// ---- PPU exception handler -------------------------------------------------
// lv2's debug library (liblv2dbg) calls this on its own thread when a PPU
// thread faults; the faulting thread is stopped, so its registers and stack
// can still be read: the exact instruction (pc), the address it touched
// (DAR) and a backtrace through the stack frames, all as @xxxxxxxx for
// ps3/tools/crashaddr.py. Then the crumbs and the function trace, and exit.

#include <sys/dbg.h>
#include <sys/process.h>
#include <sysmodule/sysmodule.h>
#include <ppu-asm.h>

static void
PS3_ExceptionHandler(u64 cause, sys_ppu_thread_t tid, u64 dar)
{
	static sys_dbg_ppu_thread_context_t ctx;	// big: not on the handler's stack
	char line[256];
	s32 ret;

	log_crashing = 1;
	memset(&ctx, 0, sizeof(ctx));
	ret = sysDbgReadPPUThreadContext(tid, &ctx);
	PS3_Logf("[crash] PPU exception: cause 0x%llx, thread 0x%llx, address touched (DAR) 0x%08llx",
	         (unsigned long long)cause, (unsigned long long)tid, (unsigned long long)dar);
	if (ret != 0) {
		PS3_Logf("[crash] (registers unreadable: 0x%08x)", (unsigned)ret);
	} else {
		PS3_Logf("[crash] pc @%08x", ctx.pc);
		PS3_Logf("[crash] lr @%08x", ctx.lr);
		PS3_Logf("[crash] ctr %08x cr %08x xer %08x", ctx.ctr, ctx.cr, ctx.xer);
		for (int r = 0; r < 32; r += 4) {
			snprintf(line, sizeof(line), "[crash] r%-2d %016llx %016llx %016llx %016llx", r,
			         (unsigned long long)ctx.gpr[r], (unsigned long long)ctx.gpr[r+1],
			         (unsigned long long)ctx.gpr[r+2], (unsigned long long)ctx.gpr[r+3]);
			PS3_Log(line);
		}
		// PPC64 ELFv1 frames: 0(sp) = caller's frame, 16(frame) = return address
		// saved by its callee. Walk up while the chain looks like a stack.
		u64 sp = ctx.gpr[1], top = sp + PS3_STACK_WALK_MAX;
		for (int i = 0; i < 48; i++) {
			if (sp == 0 || (sp & 15) || sp >= top)
				break;
			u64 next = *(volatile u64 *)(uintptr_t)sp;
			if (next <= sp || next >= top)
				break;
			u64 ra = *(volatile u64 *)(uintptr_t)(next + 16);
			PS3_Logf("[crash] stack %2d @%08x", i, (unsigned)ra);
			sp = next;
		}
	}
	PS3_DumpCrumbs();
	PS3_Log("[crash] exiting");
	PS3_LogShutdown();
	sysProcessExit(1);
}

extern "C" void
PS3_InstallCrashHandler(void)
{
	s32 ret;

	ret = sysModuleLoad(SYSMODULE_LV2DBG);
	if (ret != 0) {
		PS3_Logf("[crash handler] not available (liblv2dbg: 0x%08x): crashes only leave the crumbs", (unsigned)ret);
		return;
	}
	sysDbgSetStacksizePPUExceptionHandler(64 * 1024);
	ret = sysDbgInitializePPUExceptionHandler(100);
	if (ret == 0)
		// the prx calls it through a 32-bit function descriptor (as sysutil callbacks)
		ret = sysDbgRegisterPPUExceptionHandler((dbg_exception_handler_t)__get_opd32(PS3_ExceptionHandler), 0);
	if (ret != 0)
		PS3_Logf("[crash handler] not available (0x%08x): crashes only leave the crumbs", (unsigned)ret);
	else
		PS3_Log("[crash handler] ready: a crash logs pc, registers and a backtrace");
}

// ---- hang watchdog ---------------------------------------------------------
// A freeze (an endless loop, a wait that never ends) leaves no crash dump:
// the game thread is still alive. This thread wakes up once a second; if the
// game has run frames and then none for 10 s, it writes the same dump as a
// crash ([hang] ...), once per freeze, and says when frames come back.
static volatile int watchdogQuit;
static sys_ppu_thread_t watchdogTid;

static void
PS3_WatchdogThread(void *arg)
{
	unsigned last = 0, still = 0;
	int reported = 0;
	(void)arg;
	int tick = 0;
	log_buffered = 1;
	while (!watchdogQuit) {
		usleep(1000000);
		// the console may have to be switched off by force (a freeze that
		// takes the system with it): the log's lines to the file every
		// second, onto the disk every 5 s
		if (++tick % 5 == 0)
			PS3_LogSync();
		else {
			log_lock();
			if (log_file)
				fflush(log_file);
			log_unlock();
		}
		unsigned f = PS3_crumbFrame;
		if (f == 0 || f != last) {
			if (reported)
				PS3_Logf("[hang] frames again after %u s (frame %u)", still, f);
			last = f;
			still = 0;
			reported = 0;
			continue;
		}
		if (++still >= 10 && !reported && !watchdogQuit) {
			reported = 1;
			PS3_Logf("[hang] no frame for %u s (stuck in frame %u); what the game thread was doing:", still, f);
			PS3_DumpCrumbs();
			if (PS3_hangDumpHook)
				PS3_hangDumpHook();
			PS3_LogSync();
		}
	}
	sysThreadExit(0);
}

void
PS3_StartWatchdog(void)
{
	if (sysThreadCreate(&watchdogTid, PS3_WatchdogThread, NULL, 900, 64 * 1024,
	                    THREAD_JOINABLE, (char *)"watchdog") != 0)
		watchdogTid = 0;
}

void
PS3_StopWatchdog(void)
{
	u64 ret;
	watchdogQuit = 1;
	if (watchdogTid)
		sysThreadJoin(watchdogTid, &ret);
	watchdogTid = 0;
}
