/***************************
* This is the ISO7816 platform implementation
* for SmartCard interaction based on USART & GPIO devices.
*
*/

#ifndef __SMARTCARD_ISO7816_PLATFORM_H__
#define __SMARTCARD_ISO7816_PLATFORM_H__

#include "libc/types.h"
/* Low level platform related functions */

/* We also use a GPIO for the RST of the smarcard */
#include "libc/syscall.h"
//#include "stm32f4xx_rcc.h"

#include "libc/regutils.h"
#include "autoconf.h"

#include "smartcard_print.h"

typedef enum {
    DRV7816_MAP_AUTO,
    DRV7816_MAP_VOLUNTARY
} drv7816_map_mode_t;

/* The SMARTCARD_CONTACT pin is at state high (pullup to Vcc) when no card is
 * not present, and at state low (linked to GND) when the card is inserted.
 */
uint8_t platform_is_smartcard_inserted(void);

void platform_set_smartcard_rst(uint8_t val);

void platform_set_smartcard_vcc(uint8_t val);



int platform_smartcard_early_init(drv7816_map_mode_t map_mode);

int platform_smartcard_init(void);

/* Adapt clocks depending on what has been received */
int platform_SC_adapt_clocks(uint32_t *etu, uint32_t *frequency);

/*
 * Low level related functions: we handle the low level USAT/smartcard
 * bytes send and receive stuff here.
 */

int platform_SC_set_direct_conv(void);

int platform_SC_set_inverse_conv(void);


/* Smartcard putc and getc handling errors, with timeout in milliseconds */
void platform_SC_flush(void);

int platform_SC_getc(uint8_t *c, uint32_t timeout, uint8_t reset);

int platform_SC_putc(uint8_t c, uint32_t timeout, uint8_t reset);

/* Get ticks/time in milliseconds */
uint64_t platform_get_microseconds_ticks(void);

void platform_SC_reinit_smartcard_contact(void);

void platform_SC_reinit_iso7816(void);

/* shut the smartcard LED in case of communication error */
void platform_smartcard_lost(void);

void platform_smartcard_usart_reinit(void);

void platform_smartcard_register_user_handler_action(void (*action)(void));

int platform_smartcard_map(void);

int platform_smartcard_unmap(void);

#endif /* __SMARTCARD_ISO7816_PLATFORM_H__ */
