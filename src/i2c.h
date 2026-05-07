#ifndef I2C_H
#define I2C_H

#include <stddef.h>
#include <stdint.h>

/*
 * Polled I2C1 master driver for the wolfDemo Doom port.
 *
 * Standard Mode (100 kHz) on PB6 (SCL) / PB9 (SDA), AF4. Single-master,
 * blocking, 7-bit addressing. All entry points return 0 on success and
 * -1 on bus timeout (NACK or stuck SCL); they will not hang the caller
 * even if the slave is unplugged mid-transfer.
 */

void i2c_init(void);

int  i2c_write(uint8_t addr7, const uint8_t *data, size_t len);
int  i2c_read (uint8_t addr7,       uint8_t *data, size_t len);

/* Combined transaction: START -> write `wr_len` bytes -> repeated START
 * -> read `rd_len` bytes -> STOP. Used to address a register pointer
 * inside an I2C device, then read N bytes back. */
int  i2c_write_read(uint8_t addr7,
                    const uint8_t *wr, size_t wr_len,
                          uint8_t *rd, size_t rd_len);

#endif /* I2C_H */
