/*
 * STM32 system glue. printf is already routed to USART1 via
 * syscalls.c, so I_Error/I_Print just fall through to stdio.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

void I_Init(void) { }

void I_Quit(void)
{
    printf("\n[doom] I_Quit called\n");
    while (1) { __asm volatile ("wfi"); }
}

void I_Error(const char *fmt, ...)
{
    va_list ap;
    printf("\n[doom] I_Error: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    while (1) { __asm volatile ("wfi"); }
}

/* Pico SDK panic() stand-in. Engine code calls this in DOOM_TINY paths
 * (e.g. z_zone.c on OOM). Same effect as I_Error: print and halt. */
void panic(const char *fmt, ...) __attribute__((noreturn));
void panic(const char *fmt, ...)
{
    va_list ap;
    printf("\n[doom] panic: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    while (1) { __asm volatile ("wfi"); }
}

typedef void (*atexit_func_t)(void);
void I_AtExit(atexit_func_t func, int run_if_error)
{
    /* Stub: we never exit cleanly. */
    (void)func; (void)run_if_error;
}

void I_BindVariables(void) { }
unsigned int I_GetMemorySize(void) { return 768u * 1024u; }
void I_GetMemoryConfig(unsigned int *zonemem) {
    if (zonemem) *zonemem = 256u * 1024u;
}

/* Backing store for the Doom zone allocator. The engine asks I_ZoneBase
 * for a contiguous chunk and runs its own block-list allocator inside
 * it. 256 KB matches the budget rp2040-doom uses on the Pico (which
 * has 264 KB total SRAM); we have 768 KB so this leaves >500 KB for
 * stack, .bss, and the framebuffer. Aligned to 8 bytes for the
 * memzone_t/memblock_t headers. */
#define DOOM_ZONE_BYTES (256u * 1024u)
static __attribute__((aligned(8))) uint8_t doom_zone[DOOM_ZONE_BYTES];

uint8_t *I_ZoneBase(int *size)
{
    if (size) *size = (int)DOOM_ZONE_BYTES;
    return doom_zone;
}

/* Networking is fully disabled (NO_USE_NET=1), but a few engine sites
 * still write to net_client_connected. We shadow doom/src/net_client.h
 * to keep this as a real variable; storage lives here so any TU that
 * includes the shadow header gets a single definition. */
#include <stdbool.h>
bool net_client_connected = false;
bool drone = false;

/* Pico SDK panic_unsupported() stand-in. Same effect as panic(). */
void panic_unsupported(void) __attribute__((noreturn));
void panic_unsupported(void)
{
    printf("\n[doom] panic_unsupported (an engine path we did not stub)\n");
    while (1) { __asm volatile ("wfi"); }
}
void I_PrintBanner(const char *msg) { printf("%s\n", msg); }
void I_PrintDivider(void) { printf("---------------------------------------\n"); }
void I_PrintStartupBanner(const char *gamedescription) {
    printf("[doom] %s\n", gamedescription);
}

void *I_Realloc(void *ptr, size_t size) { return realloc(ptr, size); }

const char *I_ConsoleStdout(void) { return "uart"; }
