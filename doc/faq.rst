ISO7816 driver FAQ
-------------------

Is the ISO7816 driver self-contained?
"""""""""""""""""""""""""""""""""""""

No, the ISO7816 driver only exposes primitives to send and
receive characters on an I/O line as defined by the ISO7816
standard. This driver also generates the associated clock.

Is using USART the only way to handle ISO7816?
""""""""""""""""""""""""""""""""""""""""""""""

No, the same primitives to send and receive bytes on an
ISO7816 half-duplex I/O line can be implemented in a
bitbanging flavor with GPIOs. However, for efficiency and
performance reasons we have chosen to use the native USART
acceleration to do this.

Why this driver does not make use of USART DMA?
"""""""""""""""""""""""""""""""""""""""""""""""

The ISO7816 is a synchronous protocol. The half-duplex nature
of the I/O line and the fact that the sender and the receiver
are either exclusively sending or receiving makes polling a simple yet
still efficient strategy. Moreover, the ISO7816 bus is rather
slow (the maximum clock frequency authorized by the standard is 20 MHz,
and real life smart cards usually support up to 10 MHz): polling is not
really a deal breaker, and using DMA would not drastically improve
performance (compared to faster buses).
