#ifndef __SMARTCARD_H__
#define __SMARTCARD_H__

#include "api/types.h"

/*********** ATR ******************************/
/*** ATR parts lengths */
#define SETUP_LENGTH       4 /* Setup length is 4 * 4 = 16 bytes max */
#define HIST_LENGTH        16

/* An ATR structure */
typedef struct 
{
        uint8_t ts;               /* Bit Convention */
        uint8_t t0;               /* High nibble = Number of setup byte, low nibble = Number of historical byte */
        uint8_t ta[SETUP_LENGTH];  /* Setup array */
        uint8_t tb[SETUP_LENGTH];  /* Setup array */
        uint8_t tc[SETUP_LENGTH];  /* Setup array */
        uint8_t td[SETUP_LENGTH];  /* Setup array */
        uint8_t h[HIST_LENGTH];   /* Historical array */
        uint8_t t_mask[4];        /* Presence masks of interface bytes */
        uint8_t h_num;            /* Number of historical bytes */
        uint8_t tck;              /* Checksum */
        uint8_t tck_present;      /* Checksum presence */
} SC_ATR;


/********** APDU (T=0 and T=1) ****************/
/*** Extended APDU max length:
 * Because we are on an embedded platform and don't want to use dynamic allocation here, 
 * we do not support the maximum 65k size of extended APDUs (this would kill our RAM). 
 * This should be OK since most of cards do not support such a size either. 
 * Working buffers of 512 should do the trick.
 */
#define APDU_MAX_BUFF_LEN      512

/* An APDU command (handling extended APDU) */
typedef struct
{
        uint8_t cla;  /* Command class */
        uint8_t ins;  /* Instruction */
        uint8_t p1;   /* Parameter 1 */
        uint8_t p2;   /* Parameter 2 */
        uint16_t lc;  /* Length of data field, Lc encoded on 16 bits since it is always < 65535 */
        uint8_t data[APDU_MAX_BUFF_LEN];  /* Data field */
        uint32_t le;   /* Expected return length, encoded on 32 bits since it is <= 65536 (so we must encode the last value) */
        uint8_t send_le;
} SC_APDU_cmd;

/* An APDU response */
typedef struct
{
        uint8_t data[APDU_MAX_BUFF_LEN + 2]; /* Data field + 2 bytes for temporaty SW1/SW2 storage */
        uint32_t le; /* Actual return length. It is on an uint32_t because we increment it when receiving (this avoids integer overflows). */
        uint8_t sw1; /* Status Word 1 */
        uint8_t sw2; /* Status Word 2 */
} SC_APDU_resp;

#define SHORT_APDU_LC_MAX	255
#define SHORT_APDU_LE_MAX	256

/* 'Low level' communication with the smartcard */
int SC_get_ATR(SC_ATR *atr);
void SC_print_ATR(SC_ATR *atr);
void SC_print_APDU(SC_APDU_cmd *apdu);
void SC_print_RESP(SC_APDU_resp *resp);
int SC_send_APDU(SC_APDU_cmd *apdu, SC_APDU_resp *resp, SC_ATR *atr, uint8_t T_protocol);

int SC_fsm_early_init(void);
int SC_fsm_init(SC_ATR *atr, uint8_t *T_protocol, uint8_t do_negotiate_pts, uint8_t do_change_baudrate);
void SC_smartcard_lost(void);
uint8_t SC_is_smartcard_inserted(void);

#endif /* __SMARTCARD_H__ */
