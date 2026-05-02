#ifndef UART_H
#define UART_H

#include <stddef.h>

void uart_init(unsigned int baud);
void uart_putc(char c);
void uart_write(const char *s, size_t len);
void uart_puts(const char *s);

#endif /* UART_H */
