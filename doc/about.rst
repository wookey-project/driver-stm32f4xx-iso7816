About the ISO7816 driver
-------------------------

Principles
""""""""""

The ISO7816 driver implements the low level layer of the ISO7816-3
protocol. It configures the SMARTCARD mode of the STM32F4 MCUs
USARTs (see the datasheet of the MCU for more information about
this mode). The driver also implements the card detection GPIO
monitoring, and executes a registered callback when the smart
card is inserted or removed.
