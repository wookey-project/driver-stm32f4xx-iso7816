#include "api/print.h"
#include "api/syscall.h"
#include "api/syscall.h"
#include "smartcard_iso7816_platform.h"
#include "smartcard_iso7816.h"
#include "api/libsmartcard.h"
/* The ISO7816 master mode uses the hardware USART
 * smartcard mode.
 */
#include "api/libusart.h"
#include "api/libusart_regs.h"

//#define SMARTCARD_DEFAULT_CLK_FREQ      3500000
//#define SMARTCARD_DEFAULT_CLK_FREQ    4200000
#define SMARTCARD_DEFAULT_CLK_FREQ    5250000

#define SMARTCARD_DEFAULT_ETU        372


static volatile uint32_t SC_current_sc_frequency = SMARTCARD_DEFAULT_CLK_FREQ;
/* Get the current smartcard configured frequency */
static uint32_t SC_get_sc_clock_freq(void){
        return SC_current_sc_frequency;
}

static volatile uint32_t SC_current_sc_etu = SMARTCARD_DEFAULT_ETU;
/* Get the current smartcard configured ETU */
static uint32_t SC_get_sc_etu(void){
        return SC_current_sc_etu;
}

/* Fixed delay of a given number of clock cycles */
static void SC_delay_sc_clock_cycles(uint32_t sc_clock_cycles_timeout){
        unsigned long long t, start_tick, curr_tick;

    if(sc_clock_cycles_timeout == 0){
        return;
    }

        /* The timeout is in smartcard clock cycles, which can be converted to MCU clock time
         * using a simple conversion. The clock time is expressed in microseconds.
         */
        t = (sc_clock_cycles_timeout * 1000ULL) / SC_get_sc_clock_freq();
        start_tick = platform_get_ticks();
        /* Now wait */
        curr_tick = start_tick;
        while((curr_tick - start_tick) <= t){
                curr_tick = platform_get_ticks();
        }

        return;
}

/* Fixed delay of a given number of ETUs */
static void SC_delay_etu(uint32_t etu_timeout){

        SC_delay_sc_clock_cycles(etu_timeout * SC_get_sc_etu());

        return;
}

typedef enum {
    SC_TS_DIRECT_CONVENTION = 0,
    SC_TS_INVERSE_CONVENTION = 1,
} Sc_convention;
static volatile uint8_t SC_convention = SC_TS_DIRECT_CONVENTION;

static uint8_t SC_is_inverse_conv(void){
    if(SC_convention == SC_TS_DIRECT_CONVENTION){
        return 0;
    }
    else{
        return 1;
    }
}

static int SC_set_direct_conv(void){
    /* Set the inverse convention */
    SC_convention = SC_TS_DIRECT_CONVENTION;

    return platform_SC_set_direct_conv();
}

static int SC_set_inverse_conv(void){
    /* Set the inverse convention */
    SC_convention = SC_TS_INVERSE_CONVENTION;

    return platform_SC_set_inverse_conv();
}

static uint8_t SC_inverse_conv(uint8_t c){
    unsigned int i;
    uint8_t res = 0;

    for(i = 0; i < 8; i++){
        res |= (((~c) >> i) & 0x1) << (7-i);
    }

    return res;
}

/* Get a byte with a timeout. Timeout of 0 means forever. */
static int SC_getc_timeout(uint8_t *c, uint32_t etu_timeout){
        unsigned long long t, start_tick, curr_tick;

        /* The timeout is in ETU times, which can be converted to sys ticks
         * using the baud rate. The sys ticks are expressed in milliseconds.
         */
        t = (etu_timeout * SC_get_sc_etu() * 1000ULL) / SC_get_sc_clock_freq();
        start_tick = platform_get_ticks();

        while(platform_SC_getc(c, t, 0)){
                if(etu_timeout != 0){
                        curr_tick = platform_get_ticks();
                        /* Check the timeout and return if time is elapsed */
                        if((curr_tick - start_tick) > t){
                /* Reset our getc state machine in the lower layers ... */
                platform_SC_getc(c, t, 1);
                                return -1;
                        }
                }
        }

    if(SC_is_inverse_conv()){
        *c = SC_inverse_conv(*c);
    }

        return 0;
}



static int SC_putc_timeout(uint8_t c, uint32_t etu_timeout){
        unsigned long long t, start_tick, curr_tick;

    if(SC_is_inverse_conv()){
        c = SC_inverse_conv(c);
    }
    
    /* The timeout is in ETU times, which can be converted to sys ticks
         * using the baud rate. The sys ticks are expressed in milliseconds.
         */
        t = (etu_timeout * SC_get_sc_etu() * 1000ULL) / SC_get_sc_clock_freq();
        start_tick = platform_get_ticks();

        while(platform_SC_putc(c, t, 0)){
                if(etu_timeout != 0){
                        curr_tick = platform_get_ticks();
                        /* Check the timeout and return if time is elapsed */
                        if((curr_tick - start_tick) > t){
                /* Reset our putc state machine in the lower layers ... */
                platform_SC_putc(c, t, 1);
                                return -1;
                        }
                }
        }

    /* Wait for the character extra guard time after the stop bit */
    SC_delay_etu(CGT_character_guard_time);

    return 0;
}

static int SC_adapt_clocks(uint32_t etu, uint32_t frequency){
    uint32_t tmp_etu = etu;
    uint32_t tmp_frequency = frequency;
    if(platform_SC_adapt_clocks(&tmp_etu, &tmp_frequency)){
        return -1;
    }
    else{
        SC_current_sc_frequency = tmp_frequency;
        SC_current_sc_etu = tmp_etu;

        return 0;
    }
}

/* Get the ATR from the card */
int SC_get_ATR(SC_ATR *atr){
    unsigned int i;    
    uint8_t curr_mask, checksum, do_check_checksum = 0;
    
        if(atr == NULL){
                goto err;
        }

    for(i = 0; i < SETUP_LENGTH; i++){
        atr->t_mask[i] = 0;
    }
    atr->h_num = 0;
    /* Wait for the ATR reception with a timeout
     * of 20160 ETU (20160 bauds).
     */
    /******************************/
    if(SC_getc_timeout(&(atr->ts), ATR_ETU_TIMEOUT)){
        goto err_timeout;
    }
    /* First byte of the ATR has been received */
    if(atr->ts == 0x3b){
        if(SC_set_direct_conv()){
            goto err;
        }
    }
    else if(atr->ts == 0x03){
        if(SC_set_inverse_conv()){
            goto err;
        }
        atr->ts = SC_inverse_conv(atr->ts);
    }
    else{
        goto err;
    }
    /* Get the format byte T0 */
    if(SC_getc_timeout(&(atr->t0), WT_wait_time)){
        goto err;
    }
    checksum = atr->t0;
    /* Get the interface bytes */
    curr_mask = (atr->t0) >> 4;
    for(i = 0; i < 4; i++){
        if(curr_mask & 0x1){
                if(SC_getc_timeout(&(atr->ta[i]), WT_wait_time)){
                        goto err;
                   }
            atr->t_mask[0] |= (0x1 << i);
            checksum ^= atr->ta[i];
        }
        if(curr_mask & 0x2){
                if(SC_getc_timeout(&(atr->tb[i]), WT_wait_time)){
                        goto err;
                   }
            atr->t_mask[1] |= (0x1 << i);
            checksum ^= atr->tb[i];
        }
        if(curr_mask & 0x4){
                if(SC_getc_timeout(&(atr->tc[i]), WT_wait_time)){
                        goto err;
                   }
            atr->t_mask[2] |= (0x1 << i);
            checksum ^= atr->tc[i];
        }
        if(curr_mask & 0x8){
                if(SC_getc_timeout(&(atr->td[i]), WT_wait_time)){
                        goto err;
                   }
            atr->t_mask[3] |= (0x1 << i);
            checksum ^= atr->td[i];
            curr_mask = atr->td[i] >> 4;
            if((atr->td[i] & 0x0f) != 0){
                do_check_checksum = 1;
            }
        }
        else{
            break;
        }
    }    
    /* Get the historical bytes */
    atr->h_num = atr->t0 & 0x0f;
    for(i = 0; i < atr->h_num; i++){
            if(SC_getc_timeout(&(atr->h[i]), WT_wait_time)){
                    goto err;
        }
        checksum ^= atr->h[i];
    }
    atr->tck = 0;
    atr->tck_present = 0;
    /* The checksum TCK is present if and only if any of the TDi encode a protocol != 0 */
    if(do_check_checksum){
        atr->tck_present = 1;
        /* Get the checksum */
        if(SC_getc_timeout(&(atr->tck), WT_wait_time)){
            goto err;
        }
        /* Check the checksum */
        if(checksum != atr->tck){
            printf("Smartcard ATR checksum error ...\n");
            goto err;
        }
    }

    return 0;
err:
    printf("ATR error: atr->ts %x\n", atr->ts);
    return -1;
err_timeout:
    printf("timeout error\n");
    return -2;
}

/* Print the ATR on the console */
void SC_print_ATR(SC_ATR *atr)
{
    unsigned int i;

        if(atr == NULL){
                return;
        }

    printf("===== ATR ============\n");
    printf("TS = %x, T0 = %x\n", atr->ts, atr->t0);
    for(i = 0; i < 4; i++){
        if(atr->t_mask[0] & (0x1 << i)){
            printf("TA[%d] = %x\n", i, atr->ta[i]);
        }
    }
    for(i = 0; i < 4; i++){
        if(atr->t_mask[1] & (0x1 << i)){
            printf("TB[%d] = %x\n", i, atr->tb[i]);
        }
    }
    for(i = 0; i < 4; i++){
        if(atr->t_mask[2] & (0x1 << i)){
            printf("TC[%d] = %x\n", i, atr->tc[i]);
        }
    }
    for(i = 0; i < 4; i++){
        if(atr->t_mask[3] & (0x1 << i)){
            printf("TD[%d] = %x\n", i, atr->td[i]);
        }
    }
    for(i = 0; i < (atr->h_num & 0x0f); i++){
        printf("H[%d] = %x\n", i, atr->h[i]);
    }
    if(atr->tck_present){
        printf("TCK = %x\n", atr->tck);
    }

    return;
}

/* [RB] FIXME: for now, the PTS/PSS negotation is a work in
 * progress and has not been tested/debugged against real cards.
 */
/* PTS (Protocol Type Selection) negotiation.
 * Returns the protocol that has been negotiated.
 */
int SC_negotiate_PTS(SC_ATR *atr, uint8_t *T_protocol, uint8_t do_negotiate_pts, uint8_t do_change_baud_rate){
    uint8_t ta1, td1;
    uint32_t D_i_curr = 0, F_i_curr = 0, f_max_curr = 0, etu_curr = 0;
    uint8_t extra_guard_time = 1;
    uint8_t asked_ta1 = 0, asked_tc1 = 0;
    uint8_t pck;
    uint8_t c;

        if((atr == NULL) || (T_protocol == NULL)){
                goto err;
        }

    if(do_negotiate_pts){
        /* Check TA2 bit 8 to see if we cannot negotiate */
        if(atr->t_mask[0] & (0x1 << 1)){
            if(atr->ta[1] & (0x1 << 7)){
                /* No possible negotiation, get out with the current values */
                return 0;
            }
        }
    }

    /* Do we have TA1? If not, assume it is 0x11 */
    if(atr->t_mask[0] & 0x1){
        ta1 = atr->ta[0];
        asked_ta1 = 1;
    }
    else{
        ta1 = 0x11;
    }

    /* T=0 is the default protocol if nothing is specified by the card */
    *T_protocol = 0;
    /* Do we have TD1? If yes, this is the preferred protocol. Use it */
    if(atr->t_mask[3] & 0x1){
        td1 = atr->td[0];
        *T_protocol = td1 & 0x0f;
        if((*T_protocol != 0) && (*T_protocol != 1)){
            /* We do not support T=15 or protocols other than T=0/T=1 */
            printf("[Smartcard] Asking for unsupported protocol T=%d\n", *T_protocol);
            goto err;
        }
    }

    if(do_change_baud_rate){
        /* Get asked Di, Fi and fmax */
        D_i_curr = D_i[ta1 & 0x0f];
        F_i_curr = F_i[ta1 >> 4];
        f_max_curr = f_max[ta1 >> 4];

        /* If the card is asking for a forbidden value, return */
        if((D_i_curr == 0) || (F_i_curr == 0) || (f_max_curr == 0)){
            goto err;
        }
        /* Compute our new ETU */
        if((D_i_curr & 0xffff) == 0){
            /* Di is a fraction */
            etu_curr = F_i_curr * (D_i_curr >> 16);
        }
        else{
            /* Di is not a fraction */
            etu_curr = F_i_curr / D_i_curr;
        }
        /* Adapt our baudrate and clock frequency to what the card is asking,
         * as well as the guard time
         */
        if(atr->t_mask[2] & 0x1){
            /* We have a TC1 representing the prefered guard time */
            extra_guard_time = atr->tc[0];
            asked_tc1 = 1;
        }
        else{
            /* Keep the current value */
            extra_guard_time = 1;
        }
    }
    if(do_negotiate_pts){
        /* TODO: depending on the negitiation mode, we have to switch Fi/Di before or after
         * sending the PPS.
         */
        /****** Send the PTS to the card ******/
        pck = 0;
        /* Send PTSS */
        if(SC_putc_timeout(0xff, WT_wait_time)){
            goto err;
        }
        pck ^= 0xff;
        /* Send PTS0 telling that we will apply new Fi/Di (PTS1) and new guard time (PTS2) */
        if(SC_putc_timeout((asked_ta1 << 4) | (asked_tc1 << 5) | (*T_protocol), WT_wait_time)){
            goto err;
        }
        pck ^= (asked_ta1 << 4) | (asked_tc1 << 5);
        /* Send PTS1 if necessary */
        if(asked_ta1){
            if(SC_putc_timeout(atr->ta[0], WT_wait_time)){
                goto err;
            }
            pck ^= atr->ta[0];
        }
        /* Send PTS2 if necessary */
        if(asked_tc1){
            if(SC_putc_timeout(atr->tc[0], WT_wait_time)){
                goto err;
            }
            pck ^= atr->tc[0];
        }
        /* Send the checksum */
        if(SC_putc_timeout(pck, WT_wait_time)){
            goto err;
        }
        /* Now check that the card agrees to our PTS */
        /* Check for PSS = 0xff */
        if(SC_getc_timeout(&c, WT_wait_time)){
            goto err;
        }
        if(c != 0xff){
            goto err;
        }
        /* Check for PTS0 */
        if(SC_getc_timeout(&c, WT_wait_time)){
            goto err;
        }
        if(c != ((asked_ta1 << 4) | (asked_tc1 << 5) | (*T_protocol))){
            goto err;
        }
        /* Optionally check for PTS1 */
        if(asked_ta1){
            if(SC_getc_timeout(&c, WT_wait_time)){
                goto err;
            }
            if(c != atr->ta[0]){
                goto err;
            }
        }
        /* Optionally check for PTS2 */
        if(asked_tc1){
            if(SC_getc_timeout(&c, WT_wait_time)){
                goto err;
            }
            if(c != atr->tc[0]){
                goto err;
            }
        }
        /* Check for the PCK checksum */
        if(SC_getc_timeout(&c, WT_wait_time)){
            goto err;
        }
        if(c != pck){
            goto err;
        }
        printf("[Smartcard] PTS sent and confirmed by the card!\n");
    }
    if(do_change_baud_rate){
        /* Set the USART clocks to the new settings.
         */
        if(asked_ta1 || asked_tc1){
            if(SC_adapt_clocks(etu_curr, f_max_curr)){
                goto err;
            }
        }

        printf("[Smartcard] Switching to ETU = %d (Di = %d, Fi = %d), guard time = %d, frequency = %d, Protocol T=%d\n", etu_curr, D_i_curr, F_i_curr, extra_guard_time, f_max_curr, *T_protocol);
    }

    return 0;

err:
    return -1;
}

/* Get the response of an APDU from the card in T=0. 
 * The response is a "raw" one, and the response fragmentation is handled
 * in our upper T=0 functions layer.
 * pull_type == 0 => we wait for a procedure byte (ACK, NULL byte, SW1)
 * pull_type == 1 => we wait to get data + SW1/SW2 with no possible NULL bytes
 * pull_type == 2 => we wait to get data + SW1/SW2 with a possible NULL byte
 * 
 * wait_all_bytes == 1 => we wait for all the bytes from the card regardless of 
 * wait_all_bytes == 0 => we stop waiting when we have reached the Le bytes in the current APDU
 */
static int SC_pull_RESP_T0(SC_T0_APDU_cmd *apdu,
                           SC_T0_APDU_resp *resp,
                           unsigned char pull_type,
                           unsigned char wait_all_bytes __UNUSED)
{
        int ret;
        uint8_t c;
        uint8_t ack;
        uint8_t ack_one_byte;

        if((apdu == NULL) || (resp == NULL)){
                goto err;
        }

        ack = apdu->ins;
        ack_one_byte = apdu->ins ^ 0xff;
        if(pull_type != 0){
                goto GET_RESP_BYTES;
        }

    /* First, we wait for the data with a timeout of
     * around some ETUs.
     */
WAIT_AGAIN:
    if(SC_getc_timeout(&(resp->data[resp->le]), WT_wait_time)){
        goto err;
    }
    /* Check the received procedure byte */
    if(resp->data[resp->le] == 0x60){
        /* We have received a NULL byte, this is a lose way in T=0 of 
         * telling us to wait ...
         */
        goto WAIT_AGAIN;
    }
    if(resp->data[resp->le] == ack){
        /* This is an ACK, go and send all the data */
        return 1;
    }
    if(resp->data[resp->le] == ack_one_byte){
        /* This is an ACK to send one byte */
        return 2;
    }
    /* Then, we get the data and SW1/SW2 
     */
    resp->le++;
GET_RESP_BYTES:
    while(1){
        ret = SC_getc_timeout(&c, WT_wait_time);
        if(ret){
            /* Timeout reached without any byte, get out */
            goto END;
        }
        else{
            if(resp->le == (SHORT_APDU_LE_MAX + 2)){
                /* Overflow ... Return an error */
                goto err;
            }
            /* A byte has been received, continue */
            resp->data[resp->le++] = c;
        }
    }
END:
    if(resp->le < 2){
        /* We should have received at least two bytes (SW1 and SW2).
         * If this is not the case, this is an erroneous answer.
         */
        goto err;
    }
    /* Split the status bytes from the received data */
    resp->le -= 2;
    /* Sanity checks on sizes */
    if(resp->le > SHORT_APDU_LE_MAX){
        goto err;
    }
    resp->sw1 = resp->data[resp->le];
    resp->sw2 = resp->data[resp->le+1];

    return 0;

err:
    resp->le = 0;
    return -1;
}

#define SC_T0_WAIT_ALL_BYTES    0

/* Push TPDU in T=0 and pull the response from the card */
static int SC_push_pull_APDU_T0(SC_T0_APDU_cmd *apdu, SC_T0_APDU_resp *resp){
    unsigned int i;
    uint8_t procedure_byte __attribute__((unused));
    int ret;

        if((apdu == NULL) || (resp == NULL)){
                goto err;
        }
    /* Sanity checks on the lengths */
    if(apdu->le > SHORT_APDU_LE_MAX){
        /* Note: apdu->lc is on an uint8_t, so no need to check */
        /* Return an error: in T=0, 
         * extended APDUs are handled at the upper layer level with the
         * ENVELOPE command.
         */
        goto err;
    }
    /* Sanity check on Le and Lc: we should not have
     * a case 4 APDU here since its specific fragmentation
     * is handled in the upper layer ... See page 36 in ISO7816-3:2006.
     * As a result, we return an error here if we have a case 4 APDU.
     */
    if((apdu->send_le != 0) && (apdu->lc != 0)){
        printf("[Smartcard] T=0 case 4 APDU not fragmented ...\n");
        goto err;
    }

    /* Push the header */
    /* Send the CLA */
    if(SC_putc_timeout(apdu->cla, WT_wait_time)){
        goto err;
    }
    /* Send the INS */
    if(SC_putc_timeout(apdu->ins, WT_wait_time)){
        goto err;
    }
    /* Send P1 */
    if(SC_putc_timeout(apdu->p1, WT_wait_time)){
        goto err;
    }
    /* Send P2 */
    if(SC_putc_timeout(apdu->p2, WT_wait_time)){
        goto err;
    }
    /* Push P3 */
    if(apdu->lc != 0){
        /* Send Lc in one byte */
        if(SC_putc_timeout(apdu->lc, WT_wait_time)){
                   goto err;
                }
    }
    else{
                /* Case 1 APDU or case 2? */
                if(apdu->send_le){
                        /* We send a 0 as P3 in a case 1 APDU */
                        if(SC_putc_timeout((apdu->le) & 0xff, WT_wait_time)){
                                goto err;
                        }
                }
                else{
                        /* Send Le in one byte */
                        if(SC_putc_timeout((apdu->le) & 0xff, WT_wait_time)){
                                goto err;
                        }
                }
    }
    /* Get the procedure byte(s) and possibly the answer from the card */
    ret = SC_pull_RESP_T0(apdu, resp, 0, SC_T0_WAIT_ALL_BYTES);
    if(ret == -1){
        goto err;
    }
    else if(ret == 2){
        /* [RB] TODO: send data byte per byte if the card asks for it ... */
        printf("[Smartcard] T=0 byte per byte send is not implemented yet ...\n");
        goto err;
    }
    else if(ret == 1){
        /* We had an ACK to send all our data */
        if(apdu->lc != 0){
            /* Send the data */
            for(i = 0; i < apdu->lc; i++){
                if(SC_putc_timeout(apdu->data[i], WT_wait_time)){
                     goto err;
                    }
            }
        }
                if(apdu->le != 0){
                        /* Get our response from the card with NO possible WAIT byte since
                         * we wait data from the card.
                         */
                        ret = SC_pull_RESP_T0(apdu, resp, 1, SC_T0_WAIT_ALL_BYTES);
                }
                else{
                        /* Get our response from the card with a possible WAIT byte since
                         * we do not wait data from the card.
                         */
                        ret = SC_pull_RESP_T0(apdu, resp, 2, SC_T0_WAIT_ALL_BYTES);
                }
                if(ret != 0){
                        goto err;
                }
    }
    else if(ret == 0){
        /* This is an answer from the card */
        return 0;    
    }
    else{
        /* Unexpected case, this is an error */
        goto err;
    }

    return 0;
err:

    return -1;
}


/*** The following two functions help APDU fragmentation across multiple
 *   units (envelopes in T=0 and blocks in T=1).
 */
/* Prepare the buffer to send */
static unsigned int SC_APDU_get_encapsulated_apdu_size(SC_APDU_cmd *apdu){
    unsigned int apdu_size, apdu_lc_size, apdu_le_size;

        if(apdu == NULL){
                return 0;
        }
    /* Compute the APDU size */
        apdu_lc_size = ((apdu->lc <= SHORT_APDU_LC_MAX) ?  ((apdu->lc == 0) ? 0 : 1) : 3);
        if(apdu->send_le != 0){
                if(apdu->le <= SHORT_APDU_LE_MAX){
                        apdu_le_size = 1;
                }
                else{
                        if(apdu_lc_size != 0){
                                apdu_le_size = 2;
                        }
                        else{
                                apdu_le_size = 3;
                        }
                }
        }
        else{
                apdu_le_size = 0;
        }
        apdu_size = 4 + apdu_lc_size + apdu->lc + apdu_le_size;

    return apdu_size;
}

static uint8_t SC_APDU_prepare_buffer(SC_APDU_cmd *apdu, uint8_t *buffer, unsigned int i, uint8_t block_size, int *ret){
    unsigned int apdu_size, apdu_lc_size, apdu_le_size;
    unsigned int to_push, offset;
    unsigned int size = 0;
    *ret = 0;

    /* Sanity checks on the lengths */
    if(apdu->le > ((uint32_t)0x1 << 16)){
        /* Absolute limits for Lc and Le (Lc is uint16, no need to check) */
        *ret = -1;
        return 0;
    }
    if((apdu->lc > APDU_MAX_BUFF_LEN) || (apdu->le > APDU_MAX_BUFF_LEN)){
        *ret = -1;
        return 0;
    }

    /* Compute the APDU size */
    apdu_lc_size = ((apdu->lc <= SHORT_APDU_LC_MAX) ? ((apdu->lc == 0) ? 0 : 1) : 3);
    if(apdu->send_le){
        /* apdu->send_le = 1 means short APDU encoding except if the size exceeds 256.
         * apdu->send_le = 2 means extended APDU encoding for Le.
         */
        if((apdu->le <= SHORT_APDU_LE_MAX) && (apdu->send_le == 1)){
            apdu_le_size = 1;
        }
        else{
            if(apdu_lc_size != 0){
                apdu_le_size = 2;
            }
            else{
                apdu_le_size = 3;
            }
        }
    }
    else{
        apdu_le_size = 0;
    }
    apdu_size = 4 + apdu_lc_size + apdu->lc + apdu_le_size;
    /* Sanity checks */
    if(apdu_size < (i * block_size)){
        *ret = -1;
        return 0;
    }
    if(apdu_size > (4 + APDU_MAX_BUFF_LEN + (2 * 3))){
        *ret = -1;
        return 0;
    }

    /* Size to push */
    to_push = (apdu_size - (i * block_size)) > block_size ? block_size : (apdu_size - (i * block_size));

    /* Put as much data as we can in the buffer */
    offset = i * block_size; /* offset where we begin */
    size = 0;
    while(size < to_push){
        /* Do we have to push CLA, IN, P1, P2? */
        if(offset == 0){
            buffer[size++] = apdu->cla;
            offset++;
            continue;
        }
        if(offset == 1){
            buffer[size++] = apdu->ins;
            offset++;
            continue;
        }
        if(offset == 2){
            buffer[size++] = apdu->p1;
            offset++;
            continue;
        }
        if(offset == 3){
            buffer[size++] = apdu->p2;
            offset++;
            continue;
        }
        /* Handle Lc */
        if((offset >= 4) && (offset < (apdu_size - apdu_le_size))){
            if(apdu->lc != 0){
                if(apdu->lc <= SHORT_APDU_LC_MAX){
                    if(offset == 4){
                        buffer[size++] = apdu->lc;
                        offset++;
                        continue;
                    }
                    if(offset > 4){
                        buffer[size++] = apdu->data[offset-5];
                        offset++;
                        continue;
                    }
                }
                else{
                    if(offset == 4){
                        buffer[size++] = 0;
                        offset++;
                        continue;
                    }
                    if(offset == 5){
                        buffer[size++] = (apdu->lc >> 8) & 0xff;
                        offset++;
                        continue;
                    }
                    if(offset == 6){
                        buffer[size++] = apdu->lc & 0xff;
                        offset++;
                        continue;
                    }
                    if(offset > 6){
                        buffer[size++] = apdu->data[offset-7];
                        offset++;
                        continue;
                    }
                }
            }
        }
        /* Handle Le */
        if(apdu->send_le){
            if(offset >= (apdu_size - apdu_le_size)){
                if(apdu_le_size == 1){
                    if(offset == apdu_size-1){
                        buffer[size++] = apdu->le;
                        offset++;
                        continue;
                    }
                }
                if(apdu_le_size == 2){
                    if(offset == apdu_size-2){
                        buffer[size++] = (apdu->le >> 8) & 0xff;
                        offset++;
                        continue;
                    }
                    if(offset == apdu_size-1){
                        buffer[size++] = apdu->le & 0xff;
                        offset++;
                        continue;
                    }
                }
                if(apdu_le_size == 3){
                    if(offset == apdu_size-3){
                        buffer[size++] = 0x00;
                        offset++;
                        continue;
                    }
                    if(offset == apdu_size-2){
                        buffer[size++] = (apdu->le >> 8) & 0xff;
                        offset++;
                        continue;
                    }
                    if(offset == apdu_size-1){
                        buffer[size++] = apdu->le & 0xff;
                        offset++;
                        continue;
                    }
                }
            }
        }
    }

    return to_push;
}


/* This primitive sends an APDU in T=0 and handles the request/response fragmentation 
 * by using ENVELOPE and GET_RESPONSE instructions whenever necessary (extended APDUs,
 * case 4 APDUs, ...).
 */
static int SC_send_APDU_T0(SC_APDU_cmd *apdu, SC_APDU_resp *resp){
    SC_T0_APDU_cmd curr_apdu = { .cla = 0, .ins = 0, .p1 = 0, .p2 = 0, .lc = 0, .data = { 0 }, .le = 0, .send_le = 0 };
    SC_T0_APDU_resp curr_resp = { .data = { 0 }, .le = 0, .sw1 = 0, .sw2 = 0 };
    /* Special case 4 APDU split with a GET_RESPONSE */
    unsigned char case4_getresponse = 0;

        if((apdu == NULL) || (resp == NULL)){
                goto err;
        }

    if(apdu->lc > SHORT_APDU_LC_MAX){
        /* If we have to send an extended APDU, we have to use the ENVELOPE command 
         * and split it.
         */
        unsigned int encapsulated_apdu_len;
        unsigned int num_t0_apdus;
        unsigned int i; 
    
        /* Sanity checks on our lengths */
        if((apdu->lc > APDU_MAX_BUFF_LEN) || (apdu->le > APDU_MAX_BUFF_LEN)){
            goto err;
        }

        /* Get the number of T=0 APDUs we will have to send with ENVELOPE commands */
        encapsulated_apdu_len = SC_APDU_get_encapsulated_apdu_size(apdu);
        num_t0_apdus = (encapsulated_apdu_len / SHORT_APDU_LC_MAX) + 1;
        if((encapsulated_apdu_len % SHORT_APDU_LC_MAX == 0) && (encapsulated_apdu_len != 0)){
            num_t0_apdus--;
        }
        /* Send fragmented T=0 APDUs */
        for(i = 0; i < num_t0_apdus; i++){
            int ret;
            /* Fill in the buffer of our local T0 APDU */
            curr_apdu.lc = SC_APDU_prepare_buffer(apdu, curr_apdu.data, i, SHORT_APDU_LC_MAX, &ret);
            if((curr_apdu.lc == 0) || (ret != 0)){
                /* Error */
                goto err;
            }
            curr_apdu.cla = apdu->cla;
            curr_apdu.p1 = curr_apdu.p2 = 0;
            curr_apdu.ins = INS_ENVELOPE;
            curr_apdu.le = 0;
            curr_apdu.send_le = 0;
            curr_resp.sw1 = curr_resp.sw2 = curr_resp.le = 0;
            if(SC_push_pull_APDU_T0(&curr_apdu, &curr_resp)){
                goto err;
            }
            /* Check that the response is 9000 except for the last envelope */
            if(((curr_resp.sw1 != 0x90) && (curr_resp.sw2 != 0x00)) && (i != num_t0_apdus-1)){
                /* This is an error (either the card does not support the
                 * ENVELOPE instruction, or this is another error). Anyways,
                 * return the error as is to the upper layer.
                 */
                resp->le = curr_resp.le;
                resp->sw1 = curr_resp.sw1;
                resp->sw2 = curr_resp.sw2;
                memcpy(resp->data, curr_resp.data, curr_resp.le);
                return 0;
            }
        }
        /* From here, we continue to getting the answer from the card */
    }
    else{
        /* We have to send a short APDU. Copy the data in our working buffer. */
        curr_apdu.cla = apdu->cla;
        curr_apdu.ins = apdu->ins;
        curr_apdu.p1  = apdu->p1;
        curr_apdu.p2  = apdu->p2;
        curr_apdu.lc  = apdu->lc;
        if(apdu->send_le != 0){
            if(apdu->lc != 0){
                /* There is a special case for case 4 APDUs.
                  * See page 36 in ISO7816-3:2006: we have to send a GET_RESPONSE
                 * to send our Le and get the actual response from the card.
                 */
                curr_apdu.le = 0;
                curr_apdu.send_le = 0;
            }
            else{
                if(apdu->le > SHORT_APDU_LE_MAX){
                    /* If it is a case 2E.2 APDU, we send P3 = 0 as described in the ISO7816 standard */
                    curr_apdu.le = 0;
                    curr_apdu.send_le = 1;
                }
                else{
                    curr_apdu.le = apdu->le;
                    curr_apdu.send_le = 1;
                }
            }
        }
        memcpy(curr_apdu.data, apdu->data, apdu->lc);
        curr_resp.sw1 = curr_resp.sw2 = curr_resp.le = 0;
        if(SC_push_pull_APDU_T0(&curr_apdu, &curr_resp)){
            goto err;
        }
        /* Handle the case 4 APDU using the GET_RESPONSE method */
        if((apdu->send_le != 0) && (apdu->lc != 0)){
            /* See page 36 in  ISO7816-3:2006 for the different cases */
            if((curr_resp.sw1 == 0x90) && (curr_resp.sw2 == 0x00)){
                case4_getresponse = 1;
                curr_resp.sw2 = apdu->le;
            }
            if(curr_resp.sw1 == 0x61){
                case4_getresponse = 1;
                curr_resp.sw2 = (curr_resp.sw2 < apdu->le) ? curr_resp.sw2 : apdu->le;
            }
            /* Else: map TPDU response without any change */
        }
    }

    /* Get the response, possibly split across multiple responses */
    if(((curr_resp.sw1 == 0x61) && (apdu->send_le != 0) && (apdu->le > SHORT_APDU_LE_MAX)) || (case4_getresponse == 1)){
        resp->le = 0;
        /* Zeroize our case 4 state */
        case4_getresponse = 0;
        while(1){
            /* We have data to get with an ISO7816 GET_RESPONSE */
            curr_apdu.cla = apdu->cla;
            curr_apdu.ins = INS_GET_RESPONSE;
            curr_apdu.p1  = 0;
            curr_apdu.p2  = 0;
            curr_apdu.lc  = 0;
            curr_apdu.le  = curr_resp.sw2;
            curr_apdu.send_le = 1;
            curr_resp.sw1 = curr_resp.sw2 = curr_resp.le = 0;
            if(SC_push_pull_APDU_T0(&curr_apdu, &curr_resp)){
                goto err;
            }
            /* Copy the data from the response */
            resp->sw1 = curr_resp.sw1;
            resp->sw2 = curr_resp.sw2;
            if((curr_resp.sw1 == 0x61) || ((curr_resp.sw1 == 0x90) && (curr_resp.sw2 == 0x00))){
                /* We still agregate fragmented answers */
                if((resp->le + curr_resp.le) > APDU_MAX_BUFF_LEN){
                    /* We have an overflow, this is an error */
                    goto err;
                }
                memcpy(&(resp->data[resp->le]), curr_resp.data, curr_resp.le);
                resp->le += curr_resp.le;
                if((curr_resp.sw1 == 0x90) && (curr_resp.sw2 == 0x00)){
                    /* This is the last packet without error, get out! */
                    break;
                }
            }
            else{
                /* We have an error, copy the last response data */
                resp->le = curr_resp.le;
                memcpy(resp->data, curr_resp.data, curr_resp.le);
                break;
            }
        }
    }
    else{
        /* Response is not fragmented: copy it in our upper layer APDU response */
        resp->le = curr_resp.le;
        resp->sw1 = curr_resp.sw1;
        resp->sw2 = curr_resp.sw2;
        memcpy(resp->data, curr_resp.data, curr_resp.le);
    }

    return 0;
err:
    return -1;
}

/***************** T=1 case ********************************/
/* Compute the checksum (LRC) of a TPDU */
static uint8_t SC_TPDU_T1_lrc(SC_TPDU *tpdu){
    unsigned int i;
    uint8_t lrc = 0;

        if(tpdu == NULL){
                return 0;
        }

    lrc ^= tpdu->nad;
    lrc ^= tpdu->pcb;
    lrc ^= tpdu->len;
    for(i = 0; i < tpdu->len; i++){
        lrc ^= tpdu->data[i];
    }

    return lrc;
}


/* Compute the checksum (CRC-16) of a TPDU */
/* [RB] TODO: check the CRC-16 algorithm ... */
#define CRC_BLOCK(in, crc, poly) do {            \
    unsigned int j;                    \
    uint32_t data;                    \
    data = in;                    \
    for(j = 0; j < 8; j++){                \
        if((crc & 0x0001) ^ (data & 0x0001)){    \
            crc = (crc >> 1) ^ poly;    \
        }                    \
        else{                    \
            crc >>= 1;            \
        }                    \
        data >>= 1;                \
    }                        \
} while(0);

static uint16_t SC_TPDU_T1_crc(SC_TPDU *tpdu){
    unsigned int i;
    uint32_t poly = 0x8408; /* CCIT polynomial x16 + x12 + x5 + 1 */
    uint32_t crc  = 0xffff;

        if(tpdu == NULL){
                return 0;
        }
    
    CRC_BLOCK(tpdu->nad, crc, poly);
    CRC_BLOCK(tpdu->pcb, crc, poly);
    CRC_BLOCK(tpdu->len, crc, poly);
    for(i = 0; i < tpdu->len; i++){
        CRC_BLOCK(tpdu->data[i], crc, poly);
    }

    crc = ~crc;
    crc = (crc << 8) | (crc >> 8 & 0xff);

    return (uint16_t)crc;
}

/* Compute the checksum of a TPDU */
static void SC_TPDU_T1_checksum_compute(SC_TPDU *tpdu, SC_ATR *atr){

        if((tpdu == NULL) || (atr == NULL)){
                return;
        }

    /* The method used for the checksum depends on ATR byte (LRC or CRC). Default is LRC. 
     * TCi (i>2) contains this information.
     */
    tpdu->edc_type = EDC_TYPE_LRC;
    if((atr->t_mask[2] & (0x1 << 2)) && (atr->tc[2] & 0x1)){
        tpdu->edc_type = EDC_TYPE_CRC;
    }

    if(tpdu->edc_type == EDC_TYPE_LRC){
        /* LRC is the xor of all the bytes of the TPDU */
        tpdu->edc_lrc = SC_TPDU_T1_lrc(tpdu);
        return;        
    }
    else{
        /* CRC type */
        tpdu->edc_crc = SC_TPDU_T1_crc(tpdu);
    }
    
    return;
}

/* Check the checksum of a TPDU */
static int SC_TPDU_T1_checksum_check(SC_TPDU *tpdu){

        if(tpdu == NULL){
                return 0;
        }

    if(tpdu->edc_type == EDC_TYPE_LRC){
        /* LRC is the xor of all the bytes of the TPDU */
        if(tpdu->edc_lrc != SC_TPDU_T1_lrc(tpdu)){
            return 0;
        }
    }
    else{
        /* CRC type */
        if(tpdu->edc_crc != SC_TPDU_T1_crc(tpdu)){
            return 0;
        }
    }

    return 1;
}


/* Push a TPDU on the line */
static int SC_push_TPDU_T1(SC_TPDU *tpdu){
    unsigned int i;

        if(tpdu == NULL){
                goto err;
        }

    /* Sanity check on the length (254 bytes max as specified in the standard) */
    if(tpdu->len > TPDU_T1_DATA_MAXLEN){
        goto err;
    }
    /* Send the NAD */
    if(SC_putc_timeout(tpdu->nad, WT_wait_time)){
        goto err;
    }
    /* Send PCB */
    if(SC_putc_timeout(tpdu->pcb, WT_wait_time)){
        goto err;
    }
    /* Send the length */
    if(SC_putc_timeout(tpdu->len, WT_wait_time)){
        goto err;
    }
    /* Send the information field if it is present */
    if(tpdu->data != NULL){
        for(i = 0; i < tpdu->len; i++){
            if(SC_putc_timeout(tpdu->data[i], WT_wait_time)){
                goto err;
            }
        }
    }
    /* Send the epilogue */
    if(tpdu->edc_type == EDC_TYPE_LRC){
        if(SC_putc_timeout(tpdu->edc_lrc, WT_wait_time)){
            goto err;
        }
    }
    else if(tpdu->edc_type == EDC_TYPE_CRC){
        if(SC_putc_timeout((tpdu->edc_crc >> 8) & 0xff, WT_wait_time)){
            goto err;
        }
        if(SC_putc_timeout(tpdu->edc_crc & 0xff, WT_wait_time)){
            goto err;
        }
    }
    else{
        goto err;
    }

    return 0;
err:

    return -1;
}

/* Pull a TPDU from the line */
static int SC_pull_TPDU_T1(SC_TPDU *tpdu, uint32_t timeout){
    unsigned int i;

        if(tpdu == NULL){
                goto err;
        }

    /* Get the NAD */
    if(SC_getc_timeout(&(tpdu->nad), timeout)){
        goto err;
    }
    /* Get the PCB */
    if(SC_getc_timeout(&(tpdu->pcb), timeout)){
        goto err;
    }
    /* Get the length */
    if(SC_getc_timeout(&(tpdu->len), timeout)){
        goto err;
    }
    /* Sanity check on the length (254 bytes max as specified in the standard) */
    if(tpdu->len > TPDU_T1_DATA_MAXLEN){
        goto err;
    }
    /* Get the data */
    if(tpdu->data != NULL){
        for(i = 0; i < tpdu->len; i++){
            if(SC_getc_timeout(&(tpdu->data[i]), timeout)){
                goto err;
            }
        }
    }
    /* Get the checksum */
    if(tpdu->edc_type == EDC_TYPE_LRC){
        if(SC_getc_timeout(&(tpdu->edc_lrc), timeout)){
            goto err;
        }
    }
    else if(tpdu->edc_type == EDC_TYPE_CRC){
        uint8_t crc1, crc2;
        if(SC_getc_timeout(&crc1, timeout)){
            goto err;
        }
        if(SC_getc_timeout(&crc2, timeout)){
            goto err;
        }
        tpdu->edc_crc = (crc1 << 8) & crc2;
    }
    else{
        goto err;
    }

    return 0;
err:
    
    return -1;
}

/*** T=1 helpers for block types and error handling ***/
static inline int SC_TPDU_T1_is_IBLOCK(SC_TPDU *tpdu){
        if(tpdu == NULL){
                return -1;
        }
    return ((tpdu->pcb & PCB_IBLOCK_MSK) == PCB_IBLOCK);
}

static inline int SC_TPDU_T1_is_RBLOCK(SC_TPDU *tpdu){
        if(tpdu == NULL){
                return -1;
        }
    return ((tpdu->pcb & PCB_RBLOCK_MSK) == PCB_RBLOCK);
}

static inline int SC_TPDU_T1_is_SBLOCK(SC_TPDU *tpdu){
        if(tpdu == NULL){
                return -1;
        }
    return ((tpdu->pcb & PCB_SBLOCK_MSK) == PCB_SBLOCK);
}

static inline void SC_TPDU_T1_set_IBLOCK(SC_TPDU *tpdu){
        if(tpdu == NULL){
                return;
        }
    tpdu->pcb |= PCB_IBLOCK;
    return;
}

static inline void SC_TPDU_T1_set_RBLOCK(SC_TPDU *tpdu){
        if(tpdu == NULL){
                return;
        }
    tpdu->pcb |= PCB_RBLOCK;
    return;
}

static inline void SC_TPDU_T1_set_SBLOCK(SC_TPDU *tpdu){
        if(tpdu == NULL){
                return;
        }
    tpdu->pcb |= PCB_SBLOCK;
    return;
}

static uint8_t SC_TPDU_T1_get_sequence(SC_TPDU *tpdu){
        if(tpdu == NULL){
                return 0xff;
        }
    if(SC_TPDU_T1_is_IBLOCK(tpdu)){
        return ((tpdu->pcb & PCB_ISEQ_NUM_MASK) >> PCB_ISEQ_NUM_POS);
    }
    else if(SC_TPDU_T1_is_RBLOCK(tpdu)){
        return ((tpdu->pcb & PCB_RSEQ_NUM_MASK) >> PCB_RSEQ_NUM_POS);
    }
    else{
        /* Should not happen */
        return 0xff;
    }
}

static inline int SC_TPDU_T1_is_sequence_ok(SC_TPDU *tpdu, uint8_t sequence_number){
    return (SC_TPDU_T1_get_sequence(tpdu) == sequence_number);
}


static int SC_TPDU_T1_set_sequence(SC_TPDU *tpdu, uint8_t sequence_number){
        if(tpdu == NULL){
                return -1;
        }
    if(SC_TPDU_T1_is_IBLOCK(tpdu)){
        tpdu->pcb |= (sequence_number << PCB_ISEQ_NUM_POS);
        return 0;
    }
    else if(SC_TPDU_T1_is_RBLOCK(tpdu)){
        tpdu->pcb |= (sequence_number << PCB_RSEQ_NUM_POS);
        return 0;
    }
    else{
        return -1;
    }
}

static inline uint8_t SC_TPDU_T1_RBLOCk_get_error(SC_TPDU *tpdu){
        if(tpdu == NULL){
                return 0xff;
        }
    /* Return an error if this is not an RBLOCK */
    if(!SC_TPDU_T1_is_RBLOCK(tpdu)){
        return 0xff;
    }
    return (tpdu->pcb & PCB_ERR_MASK);
}

/* Send an error frame with the given error and the given frame sequence */
static int SC_TPDU_T1_send_error(uint8_t pcb_err, uint8_t sequence_number, SC_ATR *atr){
    SC_TPDU err_tpdu;

        if(atr == NULL){
                return -1;
        }

    err_tpdu.nad = 0;

    /* PCB is an RBLOCK */
    err_tpdu.pcb = 0;
    SC_TPDU_T1_set_RBLOCK(&err_tpdu);
    SC_TPDU_T1_set_sequence(&err_tpdu, sequence_number);
    /* Put the error field */
    err_tpdu.pcb |= pcb_err;
    /* No data */
    err_tpdu.len  = 0;
    err_tpdu.data = NULL;
    /* Compute the checksum */
    SC_TPDU_T1_checksum_compute(&err_tpdu, atr);

    /* Send the error on the line */
    SC_push_TPDU_T1(&err_tpdu);

    return 0;
}

/* Get the type of an SBLOCK */
static uint8_t SC_TPDU_T1_SBLOCK_get_type(SC_TPDU *tpdu){
        if(tpdu == NULL){
                return 0xff;
        }

    /* Return an error if this is not an SBLOCK */
    if(!SC_TPDU_T1_is_SBLOCK(tpdu)){
        return 0xff;
    }

    return (tpdu->pcb & SBLOCK_TYPE_MSK);
}

/* Get the waiting time asked by an SBLOCK */
static int SC_TPDU_T1_SBLOCK_get_waiting_time(SC_TPDU *tpdu, uint8_t *waiting_time){
        if((tpdu == NULL) || (waiting_time == NULL)){
                goto err;
        }

    *waiting_time = 0;
    /* Sanity check: is this an SBLOCK? */
    if(!SC_TPDU_T1_is_SBLOCK(tpdu)){
        goto err;
    }
    /* The waiting time should be encoded in a one byte data field 
     * as a multiple of the BWT (Block Waiting Time).
     */
    if((tpdu->len != 1) || (tpdu->data == NULL)){
        goto err;
    }
    *waiting_time = tpdu->data[0];

    return 0;
err:
    return -1;
}

/* Send an SBLOCK */
static int SC_TPDU_T1_send_sblock(uint8_t sblock_type, uint8_t *data, uint8_t size, SC_ATR *atr){
    SC_TPDU s_tpdu;

        if((data == NULL) || (atr == NULL)){
                return -1;
        }

    s_tpdu.nad = 0;

    /* PCB is an SBLOCK */
    s_tpdu.pcb = 0;
    SC_TPDU_T1_set_SBLOCK(&s_tpdu);
    /* Set the SBLOCK type */
    s_tpdu.pcb |= sblock_type;
    /* Is there data? No data */
    if(size >= SHORT_APDU_LC_MAX){
        /* Sanity check */
        return 0;
    }
    s_tpdu.len  = size;
    s_tpdu.data  = data;
    /* Compute the checksum */
    SC_TPDU_T1_checksum_compute(&s_tpdu, atr);

    /* Send the SBLOCK on the line */
    SC_push_TPDU_T1(&s_tpdu);

    return 0;
}

/* Send APDU in T=1 and get the response 
 * [RB] FIXME: for now, this a a very basic way of handling T=1. Many
 * error/corner cases are not implemented yet! However, this should work
 * for the basic interactions with cards we need.
 */
static volatile uint8_t last_send_sequence = 0;
static volatile uint8_t last_received_sequence = 0;
static int SC_send_APDU_T1(SC_APDU_cmd *apdu, SC_APDU_resp *resp, SC_ATR *atr){
    /* Create an IBLOCK in order to send our APDU */
    SC_TPDU tpdu_send;
    SC_TPDU tpdu_rcv;
    unsigned int i, num_iblocks, atr_ifsc, bwi, cwi, cwt __attribute__((unused)), bwt __attribute__((unused));
    /* Internal working buffers.
     * Note: we can work with only one buffer for both send and receive, but
     * this is cleaner to split those two for debug purposes. Additionally, the
     * buffers size is 254 bytes maximum imposed by the standard, which is reasonable
     * on our STM32F4 platform.
     */
    uint8_t buffer_send[TPDU_T1_DATA_MAXLEN];
    uint8_t buffer_recv[TPDU_T1_DATA_MAXLEN];
    unsigned int encapsulated_apdu_len;
    unsigned int received_size;

        if((apdu == NULL) || (resp == NULL) || (atr == NULL)){
                goto err;
        }

    /* Is the ATR telling us we can change the IFSC in TA3? */
    atr_ifsc = 32; /* Default is 32 bytes as specified by the standard */
    if(atr->t_mask[0] & (0x1 << 2)){
        /* Sanity check */
        if(atr->ta[2] <= 0xfe){
            atr_ifsc = atr->ta[2];
        }
    }

    /* Compute the length we will have to send */
    encapsulated_apdu_len = SC_APDU_get_encapsulated_apdu_size(apdu);

    /* How much IBLOCKS do we need? */
    num_iblocks = (encapsulated_apdu_len / atr_ifsc) + 1;
    if((encapsulated_apdu_len % atr_ifsc == 0) && (encapsulated_apdu_len != 0)){
        num_iblocks--;
    }

        /* Get the max waiting times from TB3, useful for our reception primitives */
        bwi = 4; /* Default value */
        cwi = 13; /* Default value */
        if(atr->t_mask[1] & (0x1 << 2)){
                cwi = atr->tb[2] & 0x0f;
                bwi = (atr->tb[2] >> 4) & 0x0f;
        }
        /* Update the CWT and BWT according to the dedicated formulas */
        /* CWT =  11 etu + 2**cwi etu */
        CWT_character_wait_time = (0x1 << cwi);
        /* BWT = 11 * etu + 2**bwi * 960 etu */
        BWT_block_wait_time = (0x1 << bwi) * 960;

    /* NAD is always zero in our case (no multi-slaves) */
    tpdu_send.nad = 0;
    /* Send all the IBLOCKS */
    for(i = 0; i < num_iblocks; i++){
        int ret;
        tpdu_send.pcb = 0;
        /* PCB is an IBLOCK */
        SC_TPDU_T1_set_IBLOCK(&tpdu_send);
        if(i != num_iblocks-1){
            /* Blocks are chained except for the last one */
            tpdu_send.pcb |= PCB_M_CHAIN;
        }
        /* Set the sequence number */
        SC_TPDU_T1_set_sequence(&tpdu_send, last_send_sequence);
        /* Flip the sequence number for the next block to send */
        last_send_sequence = (last_send_sequence + 1) % 2;
        /* Compute the length to send and prepare the buffer */
        tpdu_send.len = SC_APDU_prepare_buffer(apdu, buffer_send, i, atr_ifsc, &ret);
        if(ret){
            goto err;
        }

        /* Adapt the data pointer */
        tpdu_send.data = buffer_send;

        /* Compute the checksum */
        SC_TPDU_T1_checksum_compute(&tpdu_send, atr);
SEND_TPDU_AGAIN_CMD:
        /* Send the TPDU */
        if(SC_push_TPDU_T1(&tpdu_send)){
            goto err;
        }
RECEIVE_TPDU_AGAIN_CMD:
        /* Get the ACK from the card */
        tpdu_rcv.data = buffer_recv;
        tpdu_rcv.len = 0;
        tpdu_rcv.edc_type = tpdu_send.edc_type;
        if(!SC_pull_TPDU_T1(&tpdu_rcv, BWT_block_wait_time)){
            /* If the checksum of the received block is wrong, send an error R block and receive again */
            if(!SC_TPDU_T1_checksum_check(&tpdu_rcv)){
                /* Wait a bit and send a parity error */
                SC_delay_etu(BGT_block_guard_time); /* Wait for the standardized Block Guard Time (22 ETU by default) */
                SC_TPDU_T1_send_error(PCB_ERR_EDC, SC_TPDU_T1_get_sequence(&tpdu_rcv), atr);
                goto RECEIVE_TPDU_AGAIN_CMD;
            }
            /* If we have an error, send again */
            if(SC_TPDU_T1_is_RBLOCK(&tpdu_rcv) && (SC_TPDU_T1_RBLOCk_get_error(&tpdu_rcv) != PCB_ERR_NOERR)){
                /* Check the sequence number */
                if(SC_TPDU_T1_is_sequence_ok(&tpdu_rcv, SC_TPDU_T1_get_sequence(&tpdu_send))){
                    /* Genuine error, send again */
                    SC_delay_etu(BGT_block_guard_time); /* Wait for the standardized Block Guard Time (22 ETU by default) */
                    goto SEND_TPDU_AGAIN_CMD;
                }
                /* Unexpected error */
                printf("[Smartcard T=1] Unexpected case: received error block with bad sequence number ...\n");
                goto err;
            }
            /* Check that this is the ACK we are waiting for */
            if(i != num_iblocks - 1){
                /* This is not the last block, we should receive a R type block with a last transmitted I Block sequence + 1 */
                if(!SC_TPDU_T1_is_RBLOCK(&tpdu_rcv) || !SC_TPDU_T1_is_sequence_ok(&tpdu_rcv, (SC_TPDU_T1_get_sequence(&tpdu_send) + 1) % 2)){
                    /* This is not what we expect */
                    printf("[Smartcard T=1] Unexpected case: received other block than expected RBLOCK, or bad sequence number ...\n");
                    goto err;
                }
            }
            else{
                /* This is the last block, we should receive at least one I type block with a last I Block received sequence + 1 value
                 */
                if(!SC_TPDU_T1_is_IBLOCK(&tpdu_rcv) || !SC_TPDU_T1_is_sequence_ok(&tpdu_rcv, last_received_sequence)){
                    /* If this is something else, fallback to our error case ... */
                    if(SC_TPDU_T1_is_SBLOCK(&tpdu_rcv)){
                        /* If this is an SBLOCK we should not receive, this is an error ... */
                        if((SC_TPDU_T1_SBLOCK_get_type(&tpdu_rcv) == SBLOCK_RESYNC_REQ) || (SC_TPDU_T1_SBLOCK_get_type(&tpdu_rcv) == SBLOCK_WAITING_RESP)){
                            printf("[Smartcard T=1] Unexpected SBLOCK reveived from smartcard (SBLOCK_RESYNC_REQ or SBLOCK_WAITING_RESP)\n");
                            goto err;
                        }
                        /* If this is a Request Waiting Time extension, answer and go back to waiting our response ... */
                        if(SC_TPDU_T1_SBLOCK_get_type(&tpdu_rcv) == SBLOCK_WAITING_REQ){
                            /* Get the expected waiting time in number of BWT */
                            uint8_t bwt_factor;
                            if(SC_TPDU_T1_SBLOCK_get_waiting_time(&tpdu_rcv, &bwt_factor)){
                                goto err;
                            }
                            /* Acknowledge the waiting time */
                                                        SC_delay_etu(BGT_block_guard_time); /* Wait for the standardized Block Guard Time (22 ETU by default) */
                            SC_TPDU_T1_send_sblock(SBLOCK_WAITING_RESP, tpdu_rcv.data, tpdu_rcv.len, atr);
                            goto RECEIVE_TPDU_AGAIN_CMD;
                        }
                        /* Else, fallback to error since SBLOCKS are not fully implemented */
                        printf("[Smartcard T=1] S blocks automaton not fully implemented yet!\n");
                        goto err;
                    }
                    printf("[Smartcard T=1] Unexpected case: received other block than expected IBLOCK, or bad sequence number ...\n");
                    goto err;
                }
                /* Now get out and receive other necessary I blocks */
            }
        }
        else{
            /* Error pulling the response ... */
            printf("[Smartcard T=1] TPDU response reception error 1 ...\n");
            goto err;
        }    
    }
    /* If we are here, we have received at least one IBlock. We have to check 
     * if more blocks have to be received.
     */
    received_size = 0;
    while(1){
        /* Flip the last reception sequence counter */
        last_received_sequence = (last_received_sequence + 1) % 2;
        /* Copy the data batch in the response buffer */
        for(i = 0; i < tpdu_rcv.len; i++){
            resp->data[received_size++] = tpdu_rcv.data[i];
            /* Sanity check */
            if(received_size > (APDU_MAX_BUFF_LEN + 2)){
                /* We have an overflow, this should not happen ... */
                goto err;
            }

        }    
        /* More IBlocks are to be received */
        if((tpdu_rcv.pcb & PCB_M_MSK) == PCB_M_CHAIN){
            SC_delay_etu(BGT_block_guard_time); /* Wait for the standardized Block Guard Time (22 ETU by default) */
            /* Send an ACK with the received IBlock sequence + 1 */
            SC_TPDU_T1_send_error(PCB_ERR_NOERR, (SC_TPDU_T1_get_sequence(&tpdu_rcv)+1)%2, atr);
RECEIVE_TPDU_AGAIN_RESP:
            /* Receive the new block */
            if(SC_pull_TPDU_T1(&tpdu_rcv, BWT_block_wait_time)){
                printf("[Smartcard T=1] TPDU response reception error 2 ...\n");
                goto err;
            }
            /* If the checksum of the received block is wrong, send an error R block and receive again */
            if(!SC_TPDU_T1_checksum_check(&tpdu_rcv)){
                /* Wait a bit and send a parity error */
                SC_delay_etu(BGT_block_guard_time); /* Wait for the standardized Block Guard Time (22 ETU by default) */
                SC_TPDU_T1_send_error(PCB_ERR_EDC, SC_TPDU_T1_get_sequence(&tpdu_rcv), atr);
                goto RECEIVE_TPDU_AGAIN_RESP;
            }
            /* If this is not an IBlock, check if this is an SBLOCK and perform the appropriate action. 
             * [RB] TODO: handle the *full* resync automaton here instead of aborting ...
             */
            if(!SC_TPDU_T1_is_IBLOCK(&tpdu_rcv)){
                if(SC_TPDU_T1_is_SBLOCK(&tpdu_rcv)){
                    /* If this is an SBLOCK we should not receive, this is an error ... */
                    if((SC_TPDU_T1_SBLOCK_get_type(&tpdu_rcv) == SBLOCK_RESYNC_REQ) || (SC_TPDU_T1_SBLOCK_get_type(&tpdu_rcv) == SBLOCK_WAITING_RESP)){
                        printf("[Smartcard T=1] Unexpected SBLOCK received from smartcard (SBLOCK_RESYNC_REQ or SBLOCK_WAITING_RESP)\n");
                        goto err;
                    }
                    /* If this is a Request Waiting Time extension, answer and go back to waiting our response ... */
                    if(SC_TPDU_T1_SBLOCK_get_type(&tpdu_rcv) == SBLOCK_WAITING_REQ){
                        /* Get the expected waiting time in number of BWT */
                        uint8_t bwt_factor;
                        if(SC_TPDU_T1_SBLOCK_get_waiting_time(&tpdu_rcv, &bwt_factor)){
                            goto err;
                        }
                        /* Acknowledge the waiting time */
                                                SC_delay_etu(BGT_block_guard_time); /* Wait for the standardized Block Guard Time (22 ETU by default) */
                        SC_TPDU_T1_send_sblock(SBLOCK_WAITING_RESP, tpdu_rcv.data, tpdu_rcv.len, atr);
                        goto RECEIVE_TPDU_AGAIN_RESP;
                    }
                    /* Else, fallback to error since SBLOCKS are not fully implemented */
                    printf("[Smartcard T=1] S blocks automaton not fully implemented yet!\n");
                    goto err;
                }
                else{
                    printf("[Smartcard T=1] TPDU response reception error: expectd IBLOCK but got something else ...\n");
                    goto err;
                }
            }
        }
        else{
            /* We are finished, nothing more to get. Get out. */
            break;
        }
    }
    /* We have received everything, format the response */
    if(received_size < 2){
        /* We have received less than 2 bytes, this is an error ... */
        goto err;
    }
    resp->le = received_size - 2;
    /* Sanity checks on sizes */
    if((resp->le > ((uint32_t)0x1 << 16)) || (resp->le > APDU_MAX_BUFF_LEN)){
        goto err;
    }
    resp->sw1 = resp->data[resp->le];
    resp->sw2 = resp->data[resp->le+1];
    resp->data[resp->le] = resp->data[resp->le+1] = 0;

    return 0;
err:
    return -1;
}

/**************** Printing functions *************/
/* Print an APDU on the console */
void SC_print_APDU(SC_APDU_cmd *apdu)
{
    unsigned int i;

        if(apdu == NULL){
                return;
        }

    /* Sanity check on length */
    if(apdu->lc > APDU_MAX_BUFF_LEN){
        printf("[Smartcard] SC_print_APDU error: length %d overflow\n", apdu->lc);
        return;
    }
    printf("===== APDU ============\n");
    printf("CLA = %x, INS = %x, P1 = %x, P2 = %x", apdu->cla, apdu->ins, apdu->p1, apdu->p2);
    if(apdu->lc != 0){
        printf(", Lc = %x", apdu->lc);
        if(apdu->lc > 255){
            printf(" (extended)");
        }
    }
    else{
        printf(", No Lc");

    }
    if(apdu->send_le != 0){
        printf(", Le = %x", apdu->le);
        if((apdu->le > 256) || (apdu->send_le == 2)){
            printf(" (extended)");
        }
    }
    else{
        printf(", No Le");
    }
    printf("\n");
    for(i = 0; i < apdu->lc; i++){
        printf("%x ", apdu->data[i]);
        if (i % 12 == 0 && i != 0) {
              printf("\n");
            }
    }
    if(apdu->lc != 0){
        printf("\n");
    }

    return;
}


/* Print a RESP on the console */
void SC_print_RESP(SC_APDU_resp *resp)
{
    unsigned int i;

        if(resp == NULL){
                return;
        }

    /* Sanity check on length */
    if(resp->le > APDU_MAX_BUFF_LEN){
        printf("[Smartcard] SC_print_RESP error: length %d overflow\n", resp->le);
        return;
    }
    printf("===== RESP ============\n");
    printf("SW1 = %x, SW2 = %x, Le = %x\n", resp->sw1, resp->sw2, resp->le);
    for(i = 0; i < resp->le; i++){
        printf("%x ", resp->data[i]);
            if (i % 12 == 0 && i != 0) {
                  printf("\n");
            }
    }
    if(resp->le != 0){
        printf("\n");
    }

    return;
}

/* Print a T=1 TPDU on the console */
void SC_print_TPDU(SC_TPDU *tpdu)
{
    unsigned int i;

        if(tpdu == NULL){
                return;
        }

    printf("===== TPDU ============\n");
    printf("NAD = %x, PCB = %x, LEN = %x\n", tpdu->nad, tpdu->pcb, tpdu->len);
    if(tpdu->len > TPDU_T1_DATA_MAXLEN){
        printf("Len error: too big ...\n");
        return;
    }
    printf("TPDU TYPE = ");
    if(SC_TPDU_T1_is_IBLOCK(tpdu)){
        printf("I-Type\n");
    }
    else if(SC_TPDU_T1_is_RBLOCK(tpdu)){
        printf("R-Type\n");
    }
    else if(SC_TPDU_T1_is_SBLOCK(tpdu)){
        printf("S-Type\n");
    }
    else{
        printf("UNKNOWN\n");
    }
    printf("APDU: ");
    if(tpdu->data != NULL){
        for(i = 0; i < tpdu->len; i++){
            printf("%x ", tpdu->data[i]);
        }
    }
    if(tpdu->len != 0){
        printf("\n");
    }
    if(tpdu->edc_type == EDC_TYPE_LRC){
        printf("EDC (LRC) = %x\n", tpdu->edc_lrc);
    }
    else{
        printf("EDC (CRC) = %x\n", tpdu->edc_crc);
    }

    return;
}


/* Abstract Send APDU/Receive response  function */
int SC_send_APDU(SC_APDU_cmd *apdu, SC_APDU_resp *resp, SC_ATR *atr, uint8_t T_protocol){
        if((resp == NULL) || (atr == NULL)){
                goto err;
        }

    switch(T_protocol){
        case 0:
            return SC_send_APDU_T0(apdu, resp);
            break;
        case 1:
            return SC_send_APDU_T1(apdu, resp, atr);
            break;
        default:
            printf("[Smartcard] Unsupported asked protocol T=%d in SC_send_APDU\n", T_protocol);
            goto err;
    }

    return 0;
err:
    return -1;
}


/*** T=0/T=1 Finite State Machine until Idle command *************/

/* 
 *  Vcc ____|°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°
 * 
 *  CLK _______|XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
 *
 *  RST ________________________|°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°°
 *
 *  I/O _XXXXXXXXXXX|°°°°°°°°°°°°°°°°°°°°°°°°°°|_______|XXXXXXXXX
 *
 *
 */

static int SC_reinit_iso7816(void){
        CGT_character_guard_time = CGT_DEFAULT;
        WT_wait_time = WT_DEFAULT;
        BGT_block_guard_time = BGT_DEFAULT;
        CWT_character_wait_time = CWT_DEFAULT;
        BWT_block_wait_time = BWT_DEFAULT;

        SC_convention = SC_TS_DIRECT_CONVENTION;

        /* (Re)initialize our global variables */       
        last_send_sequence = last_received_sequence = 0;

        /* (Re)Initialize the block */
        if(platform_smartcard_init()){
                goto err;
        }

        platform_SC_reinit_iso7816();

        /* Reinitialize the clocks */
        SC_adapt_clocks(SMARTCARD_DEFAULT_ETU, SMARTCARD_DEFAULT_CLK_FREQ);

        return 0;

err:
        return -1;
}

int SC_fsm_early_init(void)
{
   uint8_t ret;
   if((ret = platform_smartcard_early_init())){
       return ret;
   }
   return 0;
 }

void SC_smartcard_lost(void)
{
  platform_smartcard_lost();
}


int SC_fsm_init(SC_ATR *atr, uint8_t *T_protocol, uint8_t do_negiotiate_pts, uint8_t do_change_baud_rate){
    int ret;

        unsigned int num_tries = 0;

        if((atr == NULL) || (T_protocol == NULL)){
                return -1;
        }

    SC_current_state = SC_READER_IDLE;
   /* (Re)Initialize the block */
   if(platform_smartcard_init()){
       return -1;
   }

   platform_SC_reinit_iso7816();

   /* Reinitialize the clocks */
   SC_adapt_clocks(SMARTCARD_DEFAULT_ETU, SMARTCARD_DEFAULT_CLK_FREQ);


    switch (SC_current_state) {
        case SC_READER_IDLE:{
SC_READER_IDLE_LABEL:
            platform_smartcard_lost();
            SC_current_state = SC_READER_IDLE;

            /* (Re)initialize our waiting times and other variables  */
            if(SC_reinit_iso7816()){
                return -1;
            }
            /* RST is set low */
            platform_set_smartcard_rst(0);
            /* Vcc is set low */
            platform_set_smartcard_vcc(0);
            printf("Waiting for card insertion\n");
            /* We are waiting for a card insertion to make the transition
             */
            while (!platform_is_smartcard_inserted()) {
                sys_yield();
            }
            printf("Card detected ... Go!\n");
            num_tries++;
            if(num_tries == 2000){
                return -1;
            }
            /* A card has been inserted, go! */
            goto SC_POWER_CARD_LABEL;
            break;
        }
        case SC_POWER_CARD:{
SC_POWER_CARD_LABEL:
            SC_current_state = SC_POWER_CARD;
            /***** Cold reset sequence *******/
            /* Lower the Vcc and RST pins */
            platform_set_smartcard_vcc(0);
            platform_set_smartcard_rst(0);
            /* Reinit the USART device, avoid noise on
               USART line due to card injection and clean any
               potential status state
            */
            platform_smartcard_init();
            /* Raise the Vcc pin */
            platform_set_smartcard_vcc(1);
            /* Wait for 40000 clock cycles to raise RST */
            SC_delay_sc_clock_cycles(SC_RST_TIMEOUT);
            /* Raise the RST pin */
            platform_set_smartcard_rst(1);
            /* Wait for the ATR */
            ret = SC_get_ATR(atr);
            if(ret){
                /* If we don't have an ATR, go back to reader idle state */
                printf("[DBG] Timeout or ATR error\n");
                goto SC_READER_IDLE_LABEL;
            }
            else{
                /* If we have an ATR, move on! */
                printf("[DBG] receiving ATR\n");
                goto SC_PROTOCOL_NEG_LABEL;
            }
            break;
        }
        case SC_PROTOCOL_NEG:{
SC_PROTOCOL_NEG_LABEL:
            SC_current_state = SC_PROTOCOL_NEG;
            /* Negotiate PTS */
            ret = SC_negotiate_PTS(atr, T_protocol, do_negiotiate_pts, do_change_baud_rate);
            if(ret){
                /* If negotiation has failed, go back to default state */
                goto SC_READER_IDLE_LABEL;
            }
            else{
                /* If we have successfully negotiated, we can send APDUs */
                SC_current_state = SC_IDLE_CMD;
                return 0;
            }
            break;
        }
        default:
            printf("Smartcard unhandled state %d in T=0 initialization FSM", SC_current_state);
            return -1;
    }

    return 0;
}


uint8_t SC_is_smartcard_inserted(void)
{
  return (platform_is_smartcard_inserted());
}
