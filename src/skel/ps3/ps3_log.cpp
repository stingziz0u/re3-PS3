// ps3_log.cpp -- log file in USRDIR: thread-safe, rotated at boot (the
// previous run stays as re3-ps3.old.log), size-capped, every line flushed so
// a hang or a crash still leaves the last line on disk.

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <sys/mutex.h>

#include "ps3_platform.h"

extern "C" FILE *__real_fopen(const char *path, const char *mode);

#define PS3_LOG_MAX_BYTES (4 * 1024 * 1024)

static FILE *log_file;
static long log_bytes;
static int log_capped;
static sys_mutex_t log_mutex;
static int log_mutex_ok;

static void log_lock(void)   { if (log_mutex_ok) sysMutexLock(log_mutex, 0); }
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
// The game thread leaves the last 64 steps here (no I/O, a few stores each).
// When it dies, the main thread is still alive and dumps them to the log.

#define CRUMB_COUNT 64

struct Crumb {
	const char *tag;
	int val;
	unsigned frame;
};
static Crumb crumbs[CRUMB_COUNT];
static volatile unsigned crumbHead;
unsigned PS3_crumbFrame;

static char scriptName[9];
static volatile int scriptCommand = -1;
static volatile unsigned scriptIp;
static volatile unsigned scriptCount;

extern "C" void
PS3_Crumb(const char *tag, int val)
{
	Crumb *c = &crumbs[crumbHead % CRUMB_COUNT];
	c->tag = tag;
	c->val = val;
	c->frame = PS3_crumbFrame;
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

extern "C" void
PS3_DumpCrumbs(void)
{
	unsigned head = crumbHead;
	unsigned n = head < CRUMB_COUNT ? head : CRUMB_COUNT;
	char name[9];

	memcpy(name, scriptName, 8);
	name[8] = '\0';
	PS3_Logf("[crash] last steps of the game thread (oldest first, frame: step value):");
	for (unsigned i = head - n; i != head; i++) {
		Crumb *c = &crumbs[i % CRUMB_COUNT];
		PS3_Logf("[crash]   %u: %s %d", c->frame, c->tag ? c->tag : "?", c->val);
	}
	PS3_Logf("[crash] last script command: script \"%s\" opcode 0x%04x at ip %u (%u commands run)",
	         name, scriptCommand, scriptIp, scriptCount);
}
