About the ISO7816 driver API
----------------------------

.. highlight:: c


.. highlight:: c

The API exposed by the driver is quite simple and mainly exposes:

  * Initialization functions
  * Primitives to handle Vcc and RST lines (Vcc and RST are mandatory pins of ISO7816)
  * Primitives to read and write bytes on the I/O line
  * Primitives to set the current USART baudrate depending on an asked ETU and asked frequency
  * Primitives to handle time measurement (for polling and dealing with ISO781 timeouts)
  * Primitives to handle card insertion detection

.. note::
   The notions of ETU (Elementary Time Unit) and ISO7816 baudrate are defined in the ISO7816-3
   ISO standard. Please refer to it for definitions of these concepts


Initialization functions
""""""""""""""""""""""""

The initialization functions are the following: ::

  int platform_smartcard_early_init(drv7816_map_mode_t map_mode);
  int platform_smartcard_init(void);
  void platform_smartcard_reinit(void);

The early init function declares the USART to be used in SMARTCARD mode, and the other functions
(re)initialize all the necessary variables.

Vcc and RST handling
""""""""""""""""""""

Quite straightforward APIs handle the Vcc and RST (Reset) GPIOs: ::

  void platform_set_smartcard_vcc(uint8_t val);
  void platform_set_smartcard_rst(uint8_t val);
  

I/O line read and write primitives
""""""""""""""""""""""""""""""""""

The two main exposed functions to read and write bytes on the I/O line are: ::

  int platform_SC_getc(uint8_t *c,
                     uint32_t timeout __attribute__((unused)),
                     uint8_t reset __attribute__((unused)));

  int platform_SC_putc(uint8_t c,
                     uint32_t timeout __attribute__((unused)),
                     uint8_t reset);

The API is quite self-explanatory: ``platform_SC_getc`` writes the received character
in its argument ``uint8_t *c``, ``platform_SC_putc`` pushes the ``uint8_t c`` byte on the
I/O line. These two function are non-blocking: they return -1 in case of failure, and
0 in case of success (i.e. the byte has been properly received or sent).

.. note::
   The ``platform_SC_getc`` and ``platform_SC_putc`` have unused arguments. These arguments are
   here for API compatibility and future use

Another function is used for the bytes send/receive primitive: ::
  int platform_SC_set_direct_conv(void);
  int platform_SC_set_inverse_conv(void);

This function inverses the convention of the bits received on the line (see the ISO7816 notions of direct and
inverse conventions).

Finally, the following function: ::

  platform_SC_flush(void);

is used to flush the internal buffers of the ISO7816 driver. It must be called whenever a software reset or
automaton reinitialization is performed.

Handling ETU and frequency primitives
"""""""""""""""""""""""""""""""""""""

The following APU: ::

   int platform_SC_adapt_clocks(uint32_t *etu, uint32_t *frequency);

adapts the low-level USART baudrate and clocks according to the asked ETU in ``uint32_t \*etu`` and
the asked frequency in ``uint32_t \*frequency``. Since all the ETU and frequency are not attainable,
these arguments are updated with the chosen values according to a ``best fit`` algorithm.
  

Time measurement
""""""""""""""""

In order to handle ISO7816 timeouts, the driver exposes a time measurement with
microseconds precision: ::

  uint64_t platform_get_microseconds_ticks(void);

This is merely a wrapper to the ``sys_get_systick(&tick, PREC_MICRO)`` syscall.

Card insertion detection
"""""""""""""""""""""""""

A simple API is used to detect if a smart card is present or not: ::

  uint8_t platform_is_smartcard_inserted(void);

It returns 0 if no smart card is present, and non zero if a smart card
is present. This can be used in polling mode. If the user wants to
use asynchronous detection, a callback registration API is provided: ::

  void platform_smartcard_register_user_handler_action(void (*action)(void));

The user provides a ``void (*action)(void)`` handler that is called whenever the
GPIO handling the smart card detection changes its state.


.. note::
  LEDs toggling is also present in the driver (but not exposed in the API)
  for user interactions in order to show card presence and absence as well as card activity.

Finally, there is an API to be called by upper layers when a smart card is detected as lost: ::

  void platform_smartcard_lost(void)

this function helps the driver to reinitialize and flush elements, and
eventually notify other drivers. It should be called when the upper layer libraries indeed detects
a smart card loss.

