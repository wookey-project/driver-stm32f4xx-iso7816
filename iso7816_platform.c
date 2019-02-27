#include "api/syscall.h"
#include "api/libusart_regs.h"
#include "api/libusart.h"
#include "api/libdrviso7816.h"
#include "api/semaphore.h"


/* The target clock frequency is 3.5MHz for the ATR < max 5MHz.
 * The advantage of this frequency is that it is a perfect divisor of
 * our USARTS core drequencies:
 *   - For USART 1 and 6: 84 MHz / 3.5 MHz =  24
 *   - For USART 2, 3 and 4: 42 MHz / 3.5 MHz = 12
 * NB: you can change this default frequency to 4.2 MHz or 5.25 MHz which are
 * the next divisors when using our USART 2 core frequency. This should work with
 * most of the cards while being faster, but we choose to keep 3.5 MHz mainly as a
 * conservative choice.
 * NB2: this default frequency is a priori only used during the ATR, and a faster communication
 * could/should be established with the card after parsing the ATR and/or negotiating with the
 * smartcard.
 */

/* Smartcard USART configuration: we use USART 2:
 * We use the SMARTCARD mode when configuring the GPIOs.
 * The baudrate is set to 9408 bauds (see the explanations in the smartcard_init function).
 * The parameters are set to meet the requirements in the datasheet 24.3.11 section 'Smartcard':
 *      - LINEN bit in the USART_CR2 register cleared.
 *      - HDSEL and IREN bits in the USART_CR3 register cleared.
 *      - Moreover, the CLKEN bit may be set in order to provide a clock to the smartcard.
 * The Smartcard interface is designed to support asynchronous protocol Smartcards as
 * defined in the ISO 7816-3 standard. The USART should be configured as:
 *      - 8 bits plus parity: where M=1 and PCE=1 in the USART_CR1 register
 *      - 1.5 stop bits when transmitting and receiving: where STOP=11 in the USART_CR2 register.
 */
/* Smartcard uses USART 2, i.e. I/O is on PA2 and CLK is on PA4 */
#define SMARTCARD_USART         2

/* The USART we use for smartcard.
 * STM32F4 provides the I/O pin on the TX USART pin, and
 * the CLK pin on the dedicated USART CK pin.
 * The RST (reset) pin uses a dedicated GPIO.
 * The card detect pin uses a dedicated GPIO.
 * The VCC and VPP pins use dedicated GPIOs.
 */
cb_usart_getc_t platform_SC_usart_getc = NULL;
cb_usart_putc_t platform_SC_usart_putc = NULL;
static void platform_smartcard_irq(uint32_t status, uint32_t data);

static usart_config_t smartcard_usart_config = {
        .set_mask = USART_SET_ALL,
        .mode = SMARTCARD,
        .usart = SMARTCARD_USART,

        /* To be filled later depending on the clock configuration */
        .baudrate = 0,

        /* Word length = 9 bits (8 bits + parity) */
        .word_length = USART_CR1_M_9,

        /* 1 stop bit instead of 1.5 is necessary for some cards that use a very short delay between USART characters ... */
        .stop_bits = USART_CR2_STOP_1BIT,

        /* 8 bits plus parity  (parity even) */
        .parity = USART_CR1_PCE_EN | USART_CR1_PS_EVEN,

        /* Hardware flow control disabled, smartcard mode enabled, smartard
         * NACK enabled, HDSEL and IREN disabled, error interrupts enabled (for framing
         * error) */
        .hw_flow_control = USART_CR3_CTSE_CTS_DIS | USART_CR3_RTSE_RTS_DIS |
                           USART_CR3_SCEN_EN | USART_CR3_NACK_EN | USART_CR3_HDSEL_DIS |
                           USART_CR3_IREN_DIS | USART_CR3_EIE_EN,

        /* TX and RX are enabled, parity error interrupt enabled */
        .options_cr1 = USART_CR1_TE_EN | USART_CR1_RE_EN | USART_CR1_PEIE_EN |
                       USART_CR1_RXNEIE_EN | USART_CR1_TCIE_EN,

        /* LINEN disabled, USART clock enabled, CPOL low, CPHA 1st edge, last bit clock pulse enabled */
        .options_cr2 = USART_CR2_LINEN_DIS | USART_CR2_CLKEN_PIN_EN |
                       USART_CR2_CPOL_DIS | USART_CR2_CPHA_DIS | USART_CR2_LBCL_EN,

        /* To be filled later depending on the clock configuration */
        .guard_time_prescaler = 0,

        /* Send/Receive/Error IRQ callback */
        .callback_irq_handler = platform_smartcard_irq,

        /* Receive and send function pointers */
        .callback_usart_getc_ptr = &platform_SC_usart_getc,
        .callback_usart_putc_ptr = &platform_SC_usart_putc,
};


device_t dev;   /* Device configuration */
int      dev_desc = 0;  /* Descriptor transmitted by the kernel */

static uint8_t exti_butt_count = 0;

void exti_button_handler(uint8_t irq __attribute__((unused)),
                  uint32_t status __attribute__((unused)),
                  uint32_t data __attribute__((unused)))
{
 	exti_butt_count++;
}

/* Initialize the CONTACT pin */
static volatile uint8_t platform_SC_gpio_smartcard_contact_changed = 0;

static void (*volatile user_handler_action)(void) = NULL;
void platform_smartcard_register_user_handler_action(void (*action)(void))
{
	if(action == NULL){
		return;
	}
	user_handler_action = action;
	return;
}



void exti_handler(uint8_t irq __attribute__((unused)),
                  uint32_t status __attribute__((unused)),
                  uint32_t data __attribute__((unused)))
{
	platform_SC_gpio_smartcard_contact_changed = 1;
	if(user_handler_action != NULL){
		user_handler_action();
	}
	return;
}

uint8_t platform_early_gpio_init(void)
{
  e_syscall_ret ret;

  strncpy(dev.name, "smart_gpios", sizeof("smart_gpios"));
#if CONFIG_WOOKEY // Support for pin card led indicator
  dev.gpio_num = 5;
#else
  dev.gpio_num = 3;
#endif
  dev.map_mode = DEV_MAP_AUTO;

  // contact port
  dev.gpios[0].mask = GPIO_MASK_SET_EXTI | GPIO_MASK_SET_MODE | GPIO_MASK_SET_PUPD | GPIO_MASK_SET_TYPE | GPIO_MASK_SET_SPEED;
  dev.gpios[0].kref.port = GPIO_PE;
  dev.gpios[0].kref.pin = 2;
  dev.gpios[0].mode = GPIO_PIN_INPUT_MODE;
  dev.gpios[0].pupd = GPIO_NOPULL;
  dev.gpios[0].type = GPIO_PIN_OTYPER_OD;
  dev.gpios[0].speed = GPIO_PIN_VERY_HIGH_SPEED;
  dev.gpios[0].exti_trigger = GPIO_EXTI_TRIGGER_BOTH;
  dev.gpios[0].exti_handler = exti_handler;

  // RST port
  dev.gpios[1].mask = GPIO_MASK_SET_MODE | GPIO_MASK_SET_PUPD | GPIO_MASK_SET_TYPE | GPIO_MASK_SET_SPEED;
  dev.gpios[1].kref.port = GPIO_PE;
  dev.gpios[1].kref.pin = 3;
  dev.gpios[1].mode = GPIO_PIN_OUTPUT_MODE;
  dev.gpios[1].pupd = GPIO_PULLDOWN;
  dev.gpios[1].type = GPIO_PIN_OTYPER_PP;
  dev.gpios[1].speed = GPIO_PIN_VERY_HIGH_SPEED;

  // VCC port
  dev.gpios[2].mask = GPIO_MASK_SET_MODE | GPIO_MASK_SET_PUPD | GPIO_MASK_SET_TYPE | GPIO_MASK_SET_SPEED;
  dev.gpios[2].kref.port = GPIO_PD;
  dev.gpios[2].kref.pin = 7;
  dev.gpios[2].mode = GPIO_PIN_OUTPUT_MODE;
  dev.gpios[2].pupd = GPIO_PULLDOWN;
  dev.gpios[2].type = GPIO_PIN_OTYPER_PP;
  dev.gpios[2].speed = GPIO_PIN_VERY_HIGH_SPEED;

#if CONFIG_WOOKEY
  // led info
  dev.gpios[3].mask = GPIO_MASK_SET_MODE | GPIO_MASK_SET_PUPD | GPIO_MASK_SET_SPEED;
  dev.gpios[3].kref.port = GPIO_PC;
  dev.gpios[3].kref.pin = 4;
  dev.gpios[3].pupd = GPIO_NOPULL;
  dev.gpios[3].mode = GPIO_PIN_OUTPUT_MODE;
  dev.gpios[3].speed = GPIO_PIN_HIGH_SPEED;

  // DFU button
  dev.gpios[4].mask = GPIO_MASK_SET_EXTI | GPIO_MASK_SET_MODE | GPIO_MASK_SET_PUPD | GPIO_MASK_SET_TYPE | GPIO_MASK_SET_SPEED;
  dev.gpios[4].kref.port = GPIO_PE;
  dev.gpios[4].kref.pin = 4;
  dev.gpios[4].mode = GPIO_PIN_INPUT_MODE;
  dev.gpios[4].pupd = GPIO_NOPULL;
  dev.gpios[4].type = GPIO_PIN_OTYPER_PP;
  dev.gpios[4].speed = GPIO_PIN_LOW_SPEED;
  dev.gpios[4].exti_trigger = GPIO_EXTI_TRIGGER_BOTH;
  dev.gpios[4].exti_handler = exti_button_handler;
#endif


  ret = sys_init(INIT_DEVACCESS, &dev, &dev_desc);
  if (ret != 0) {
      log_printf("Error while declaring GPIO device: %d\n", ret);
  }
  return ret;
}

void platform_set_smartcard_rst(uint8_t val)
{
  e_syscall_ret ret;
  ret = sys_cfg(CFG_GPIO_SET, (uint8_t)(('E' - 'A')<< 4) + 3, val);
  if (ret != SYS_E_DONE) {
    log_printf("unable to set gpio RST pin value %x: %x\n", val, strerror(ret));
  }
}

void platform_set_smartcard_vcc(uint8_t val)
{
  e_syscall_ret ret;
  ret = sys_cfg(CFG_GPIO_SET, (uint8_t)(('D' - 'A') << 4) + 7, val);
  if (ret != SYS_E_DONE) {
    log_printf("unable to set gpio VCC pin with %x: %s\n", val, strerror(ret));
  }
}

static volatile bool map_voluntary;

static uint8_t platform_early_usart_init(drv7816_map_mode_t map_mode)
{
  uint8_t ret = 0;
  switch (map_mode) {
      case DRV7816_MAP_AUTO:
          map_voluntary = false;
          ret = usart_early_init(&smartcard_usart_config, USART_MAP_AUTO);
          break;
      case DRV7816_MAP_VOLUNTARY:
          map_voluntary = true;
          ret = usart_early_init(&smartcard_usart_config, USART_MAP_VOLUNTARY);
          break;
      default:
          printf("invalid map mode\n");
          ret = 1;
  }
  if (ret != 0) {
      log_printf("Error while early init of USART: %d\n", ret);
  }
  return ret;
}

int platform_smartcard_map(void)
{
    if (map_voluntary) {
        return usart_map();
    }
    return 0;
}

int platform_smartcard_unmap(void)
{
    if (map_voluntary) {
        return usart_unmap();
    }
    return 0;
}

static volatile uint8_t platform_SC_is_smartcard_inserted = 0;

/* The SMARTCARD_CONTACT pin is at state high (pullup to Vcc) when no card is
 * not present, and at state low (linked to GND) when the card is inserted.
 */
uint8_t platform_is_smartcard_inserted(void)
{
	/* NOTE: we only do this for WooKey because we do not have
	 * insertion switch on the discovery.
 	*/
#ifdef CONFIG_WOOKEY
        e_syscall_ret ret;
        uint8_t val = 0;
        uint64_t count = 0;
        uint64_t local_count;

        if(platform_SC_gpio_smartcard_contact_changed == 1){

            sys_get_systick(&count, PREC_MICRO);
            local_count = count;
            do {
                sys_get_systick(&local_count, PREC_MICRO);
            } while (((local_count - count) / 1000) < 100);

            ret = sys_cfg(CFG_GPIO_GET, (uint8_t)((('E' - 'A') << 4) + 2), &val);
            if (ret != SYS_E_DONE) {
                log_printf("Unable to read from GPIOE / pin 2, ret %s\n", strerror(ret));
                return 0;
            }
            if (!val) {
                /* toogle led on */
                sys_cfg(CFG_GPIO_SET, (uint8_t)((('C' - 'A') << 4) + 4), 1);
            } else {
                /* toogle led off */
                sys_cfg(CFG_GPIO_SET, (uint8_t)((('C' - 'A') << 4) + 4), 0);
            }
            platform_SC_gpio_smartcard_contact_changed = 0;
            platform_SC_is_smartcard_inserted = !val;
        }
        return platform_SC_is_smartcard_inserted;
#else
	return 1;
#endif
}

void platform_smartcard_lost(void)
{
    sys_cfg(CFG_GPIO_SET, (uint8_t)((('C' - 'A') << 4) + 4), 0);
}

/* Initialize the USART in smartcard mode as
 * described in the datasheet, as well as smartcard
 * associated GPIOs.
 */
static int platform_smartcard_clocks_init(usart_config_t *config, uint32_t *target_freq, uint8_t target_guard_time, uint32_t *etu)
{
        uint32_t usart_bus_clk, prescaler;
        unsigned int i;

        /* First, get the usart clock */
        usart_bus_clk = usart_get_bus_clock(config);

        /* Find the best suitable target frequency <= target frequency with regards to our USART core frequency
         * (i.e. as a divisor of our USART core frequency).
         */
        if(*target_freq > usart_bus_clk){
                /* The target frequency is > USART frequency: there is no need to try ... */
                goto err;
        }

        i = *target_freq;
        while(i != 0){
                if(((usart_bus_clk / i) * i) == usart_bus_clk){
                        break;
                }
                i--;
        }
        if(i == 0){
                goto err;
        }
        else{
                *target_freq = i;
        }

        log_printf("Rounding target freguency to %d\n", *target_freq);

        /* Then, compute the baudrate depending on the target frequency */
        /* Baudrate is the clock frequency divided by one ETU (372 ticks by default, possibly negotiated).
         * For example, a frequency of 3.5MHz gives 3.5MHz / 372 ~= 9408 bauds for a 372 ticks ETU. */
	if(*etu == 0){
		/* Avoid division by 0 */
		goto err;
	}
        config->baudrate = (*target_freq) / (*etu);

        /* Finally, adapt the CLK clock pin frequency to the target frequency using the prescaler.
         * Also, adapt the guard time (expressed in bauds).
         * Frequency is = (APB_clock / PRESCALER) = (42MHz / 12) = 3.5MHz. The value of the prescaler field is x2 (cf. datasheet).
         */
        prescaler = (usart_bus_clk / (*target_freq));
        config->guard_time_prescaler = ((prescaler / 2) << USART_GTPR_PSC_Pos) | (target_guard_time << USART_GTPR_GT_Pos);

        return 0;

err:

        return -1;
}

static volatile uint8_t platform_SC_pending_receive_byte = 0;
static volatile uint8_t platform_SC_pending_send_byte = 0;
static volatile uint8_t platform_SC_byte = 0;


int platform_smartcard_early_init(drv7816_map_mode_t map_mode)
{
  // TODO check the return values
	/* Initialize the GPIOs */
    memset((void*)&dev, 0, sizeof(device_t));
	uint8_t ret = 0;
	if ((ret = platform_early_gpio_init()) != SYS_E_DONE) {
		goto gpio_err;
	}
	if ((ret = platform_early_usart_init(map_mode)) != SYS_E_DONE) {
        	goto usart_err;
	}
	return 0;
gpio_err:
	return 1;
usart_err:
	return 2;
}

int platform_smartcard_init(void){
	/* Reinitialize global variables */
	platform_SC_pending_receive_byte = 0;
	platform_SC_pending_send_byte = 0;
	platform_SC_byte = 0;

	/* Initialize the USART in smartcard mode */
	log_printf("==> Enable USART%d in smartcard mode!\n", smartcard_usart_config.usart);
 	usart_init(&smartcard_usart_config);
	return 0;
}

void platform_smartcard_usart_reinit(void){
	usart_disable(&smartcard_usart_config);
	usart_enable(&smartcard_usart_config);
	log_printf("==> Reinit USART%d\n", smartcard_usart_config.usart);

	return;
}

/* Adapt clocks and guard time depending on what has been received */
int platform_SC_adapt_clocks(uint32_t *etu, uint32_t *frequency){
	uint32_t old_mask;
	usart_config_t *config = &smartcard_usart_config;

	if(config->mode != SMARTCARD){
		goto err;
	}
	old_mask = config->set_mask;
	/* Adapt the clocks configuration in our structure */
	if(platform_smartcard_clocks_init(config, frequency, 1, etu)){
		goto err;
	}
	config->set_mask = USART_SET_BAUDRATE | USART_SET_GUARD_TIME_PS;
	/* Adapt the configuration at the USART level */
	usart_init(&smartcard_usart_config);
	config->set_mask = old_mask;

	return 0;
err:
	return -1;
}

/*
 * Low level related functions: we handle the low level USAT/smartcard
 * bytes send and receive stuff here.
 */

/* The following buffer is a circular buffer holding the received bytes when
 * an asynchronous burst of ISRs happens (i.e. when sending/receiving many bytes
 * in a short time slice).
 */
static uint8_t received_SC_bytes[64];
volatile unsigned int received_SC_bytes_start = 0;
volatile unsigned int received_SC_bytes_end   = 0;
/* The mutex for handling the reception ring buffer between ISR and main thread */
static volatile uint32_t SC_mutex;

volatile unsigned int received = 0;

static void platform_smartcard_irq(uint32_t status __attribute__((unused)), uint32_t data){
	/* Dummy read variable */
	uint8_t dummy_usart_read = 0;
	/* Check if we have a parity error */
	if ((get_reg(&status, USART_SR_PE)) && (platform_SC_pending_send_byte != 0)) {
		/* Parity error, program a resend */
		platform_SC_pending_send_byte = 3;
		/* Dummy read of the DR register to ACK the interrupt */
		dummy_usart_read = data & 0xff;
		return;
	}

	/* Check if we have a framing error */
	if ((get_reg(&status, USART_SR_FE)) && (platform_SC_pending_send_byte != 0)) {
		/* Frame error, program a resend */
		platform_SC_pending_send_byte = 4;
		/* Dummy read of the DR register to ACK the interrupt */
		dummy_usart_read = data & 0xff;
		return;
	}

	/* We have sent our byte */
	if ((get_reg(&status, USART_SR_TC)) && (platform_SC_pending_send_byte != 0)) {
		/* Clear TC, not needed here (done in posthook) */
		/* Signal that the byte has been sent */
		platform_SC_pending_send_byte = 2;
		return;
	}

	/* We can actually read data */
	if (get_reg(&status, USART_SR_RXNE)){
		/* We are in our sending state, no need to treceive anything */
		if(platform_SC_pending_send_byte != 0){
			return;
		}
		/* Lock the mutex */
		if(!semaphore_trylock(&SC_mutex)){
			/* We should not be blocking when locking the mutex since we are in ISR mode!
			 * This means that we will miss bytes here ... But this is better than corrupting our
			 * reception ring buffer!
			 */
			return;
		}

		/* Check if we overflow */
		/* We have no more room to store bytes, just give up ... and
		 * drop the current byte
		 */
		if(((received_SC_bytes_end + 1) % sizeof(received_SC_bytes)) == received_SC_bytes_start){
			/* Unlock the mutex */
			while(!semaphore_release(&SC_mutex)){
				continue;
			}
			dummy_usart_read = data & 0xff;
			return;
		}
		if(received_SC_bytes_end >= sizeof(received_SC_bytes)){
			/* This check should be unnecessary due to the modulus computation
			 * performed ahead (which is the only update to received_SC_bytes_end),
			 * but better safe than sorry!
			 */
			/* Overflow, get out */
			return;
		}
		received_SC_bytes[received_SC_bytes_end] = data & 0xff;
		/* Wrap up our ring buffer */
		received_SC_bytes_end = (received_SC_bytes_end + 1) % sizeof(received_SC_bytes);
		platform_SC_pending_receive_byte = 1;

		/* Unlock the mutex */
		while(!semaphore_release(&SC_mutex)){
			continue;
		}

		return;
	}

	return;
}

/* Set the direct convention at low level */
int platform_SC_set_direct_conv(void){
	return 0;
}

/* Set the inverse convention at low level */
int platform_SC_set_inverse_conv(void){
	uint32_t old_mask;
	usart_config_t *config = &smartcard_usart_config;
	uint64_t t, start_tick, curr_tick;
	/* Dummy read variable */
	uint8_t dummy_usart_read = 0;

	/* Flush the pending received byte from the USART block */
	dummy_usart_read = (*r_CORTEX_M_USART_DR(SMARTCARD_USART)) & 0xff;
	/* ACK the pending parity errors */
	dummy_usart_read = get_reg(r_CORTEX_M_USART_SR(smartcard_usart_config.usart), USART_SR_PE);

	/* Reconfigure the usart with an ODD parity */
	if(config->mode != SMARTCARD){
		goto err;
	}
	old_mask = config->set_mask;
	/* Adapt the configuration at the USART level */
	config->set_mask = USART_SET_PARITY;
	config->parity = USART_CR1_PCE_EN | USART_CR1_PS_ODD,
	usart_init(&smartcard_usart_config);
	config->set_mask = old_mask;

	/* Get the pending byte again (with 9600 ETU at 372 timeout) to send the proper
	 * parity ACK to the card and continue to the next bytes ...
	 */
        t = ((uint64_t)9600 * 372 * 1000) / 3500000;
        start_tick = platform_get_microseconds_ticks();
        curr_tick = start_tick;
	while(platform_SC_getc((uint8_t*)&dummy_usart_read, 0, 0)){
		if((curr_tick - start_tick) > t){
			goto err;
		}
		curr_tick = platform_get_microseconds_ticks();
	}

	return 0;

err:
	return -1;
}

/* Low level flush of our receive/send state, in order
 * for the higher level to be sure that everything is clean
 */
void platform_SC_flush(void){
	/* Flushing the receive/send state is only a matter of cleaning
	 * our ring buffer!
	 */
	/* Lock the mutex */
	while(!semaphore_trylock(&SC_mutex)){
		continue;
	}
	platform_SC_pending_receive_byte = platform_SC_pending_send_byte = 0;
	received_SC_bytes_start = received_SC_bytes_end = 0;
	while(!semaphore_release(&SC_mutex)){
		continue;
	}
}

/* Low level char PUSH/POP functions */
/* Smartcard putc and getc handling errors:
 * The getc function is non blocking */
int platform_SC_getc(uint8_t *c,
                     uint32_t timeout __attribute__((unused)),
                     uint8_t reset __attribute__((unused))){
	if(c == NULL){
		return -1;
	}
	if(platform_SC_pending_receive_byte != 1){
		return -1;
	}
	/* Lock the mutex */
	while(!semaphore_trylock(&SC_mutex)){
		continue;
	}

	/* Read our ring buffer to check if something is ready */
	if(received_SC_bytes_start == received_SC_bytes_end){
		/* Ring buffer is empty */
		/* Unlock the mutex */
		while(!semaphore_release(&SC_mutex)){
			continue;
		}
		return -1;
	}
	/* Data is ready, go ahead ... */
	if(received_SC_bytes_start >= sizeof(received_SC_bytes)){
		/* This check should be unnecessary due to the modulus computation
		 * performed ahead (which is the only update to received_SC_bytes_start),
		 * but better safe than sorry!
		 */
		/* Overflow, get out */
		return -1;
	}
	*c = received_SC_bytes[received_SC_bytes_start];
	/* Wrap up our ring buffer */
	received_SC_bytes_start = (received_SC_bytes_start + 1) % sizeof(received_SC_bytes);
	/* Tell that there is no more byte when our ring buffer is empty */
	if(received_SC_bytes_start == received_SC_bytes_end){
		platform_SC_pending_receive_byte = 0;
	}
	/* Unlock the mutex */
	while(!semaphore_release(&SC_mutex)){
		continue;
	}

	return 0;
}

/* The putc function is non-blocking and checks
 * for errors. In the case of errors, try to send the byte again.
 */
int platform_SC_putc(uint8_t c,
                     uint32_t timeout __attribute__((unused)),
                     uint8_t reset){
	if(reset){
		platform_SC_pending_send_byte = 0;
		return 0;
	}
	if((platform_SC_pending_send_byte == 0) || (platform_SC_pending_send_byte >= 3)){
		platform_SC_pending_send_byte = 1;
		/* Push the byte on the line */
		(*r_CORTEX_M_USART_DR(SMARTCARD_USART)) = c;
		return -1;
	}
	if(platform_SC_pending_send_byte == 2){
		/* The byte has been sent */
		platform_SC_pending_send_byte = 0;
		return 0;
	}

	return -1;
}

/* Get ticks/time in microseconds */
uint64_t platform_get_microseconds_ticks(void){
	uint64_t tick = 0;
	sys_get_systick(&tick, PREC_MICRO);
	return tick;
}


void platform_SC_reinit_smartcard_contact(void){
	sys_cfg(CFG_GPIO_GET, (uint8_t)((('E' - 'A') << 4) + 2), (uint8_t*)&platform_SC_is_smartcard_inserted);
	platform_SC_is_smartcard_inserted = (~platform_SC_is_smartcard_inserted) & 0x1;
        if (platform_SC_is_smartcard_inserted) {
                /* toogle led on */
                sys_cfg(CFG_GPIO_SET, (uint8_t)((('C' - 'A') << 4) + 4), 1);
        } else {
                /* toogle led off */
                sys_cfg(CFG_GPIO_SET, (uint8_t)((('C' - 'A') << 4) + 4), 0);
        }
	platform_SC_gpio_smartcard_contact_changed = 0;
	return;
}
void platform_SC_reinit_iso7816(void){
	platform_SC_pending_receive_byte = 0;
	platform_SC_pending_send_byte = 0;
	platform_SC_byte = 0;
	received_SC_bytes_start = received_SC_bytes_end = 0;
	semaphore_init(1, &SC_mutex);
	return;
}

