#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/times.h>
#include <unistd.h>
#include "uart.h"

/*
 * Minimal newlib syscall stubs. printf goes to USART1; everything else
 * is a no-op so the engine's I_Error/log paths land somewhere visible.
 */

extern char _end;
static char *heap_end = &_end;

int _write(int fd, const char *buf, int len)
{
    (void)fd;
    uart_write(buf, (size_t)len);
    return len;
}

int _read(int fd, char *buf, int len)
{
    (void)fd; (void)buf; (void)len;
    return 0;
}

int _close(int fd)        { (void)fd; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; if (st) st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd)       { (void)fd; return 1; }
off_t _lseek(int fd, off_t off, int dir) { (void)fd; (void)off; (void)dir; return 0; }
int _getpid(void)         { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }

void *_sbrk(ptrdiff_t incr)
{
    char *prev = heap_end;
    heap_end += incr;
    return prev;
}
