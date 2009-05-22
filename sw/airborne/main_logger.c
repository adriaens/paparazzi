/*
 * $Id$
 *  
 * Copyright (C) 2009  Martin Mueller
 *
 * This file is part of paparazzi.
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA. 
 *
 */

/** \file main_logger.c
 *  \brief Logger application
 *
 *   This collects telemetry received through a serial port and writes that
 * to a (micro) SD card through the efsl library
 */

  /* XBee-message: ABCDxxxxxxxE
     A XBEE_START (0x7E)
     B LENGTH_MSB (D->D)
     C LENGTH_LSB
     D XBEE_PAYLOAD
       0 XBEE_TX16 (0x01) / XBEE_RX16 (0x81)
       1 FRAME_ID (0)     / SRC_ID_MSB
       2 DEST_ID_MSB      / SRC_ID_LSB
       3 DEST_ID_LSB      / XBEE_RSSI
       4 TX16_OPTIONS (0) / RX16_OPTIONS
       5 PPRZ_DATA
         0 SENDER_ID
         1 MSG_ID
         2 MSG_PAYLOAD
         . DATA (messages.xml)
     E XBEE_CHECKSUM (sum[A->D])
     
    ID is AC_ID for aircraft, 0x100 for ground station
  */

  /* PPRZ-message: ABCxxxxxxxDE
     A PPRZ_STX (0x99)
     B LENGTH (A->E)
     C PPRZ_DATA
       0 SENDER_ID
       1 MSG_ID
       2 MSG_PAYLOAD
       . DATA (messages.xml)
     D PPRZ_CHECKSUM_A (sum[B->C])
     E PPRZ_CHECKSUM_B (sum[ck_a])
  */

  /* LOG-message: ABCDEFGHxxxxxxxI
     A PPRZ_STX (0x99)
     B LENGTH (H->H)
     C SOURCE (0=uart0, 1=uart1, 2=i2c0, ...)
     D TIMESTAMP_LSB (100 microsec raster)
     E TIMESTAMP
     F TIMESTAMP
     G TIMESTAMP_MSB
     H PPRZ_DATA
       0 SENDER_ID
       1 MSG_ID
       2 MSG_PAYLOAD
       . DATA (messages.xml)
     I CHECKSUM (sum[B->H])
  */

#include "std.h"
#include "init_hw.h"
#include "sys_time.h"
#include "led.h"
#include "interrupt_hw.h"
#include "uart_hw.h"
#include "uart.h"

#include "efs.h"
#include "ls.h"
#include "mkfs.h"

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE (!FALSE)
#endif

/* BUTTON that stops logging (BUTTON = P0.7, INT1 = P0.14) */
#define STOP_KEY 14

/** Constants for the API protocol */
#define XBEE_START 0x7e
#define XBEE_TX16_ID 0x01
#define TX16_OPTIONS 0x00
#define NO_FRAME_ID 0
#define XBEE_RFDATA_OFFSET 5
#define XBEE_RX16_ID 0x81

/** Status of the API packet receiver automata */
#define XBEE_UNINIT 0
#define XBEE_GOT_START 1
#define XBEE_GOT_LENGTH_MSB 2
#define XBEE_GOT_LENGTH_LSB 3
#define XBEE_GOT_PAYLOAD 4
#define XBEE_PAYLOAD_LEN 256

/** Receiving pprz messages */
#define STX  0x99
#define UNINIT 0
#define GOT_STX 1
#define GOT_LENGTH 2
#define GOT_PAYLOAD 3
#define GOT_CRC1 4
#define PPRZ_PAYLOAD_LEN 256
#define PPRZ_DATA_OFFSET 2

/** logging messages **/
#define LOG_DATA_OFFSET 7
#define MSG_SIZE 256
/* logging frequency in Hz */
#define LOG_FREQ 10000
/* T0_CLK = PCLK / T0_PCLK_DIV (shall be 15MHz)
   frequency = T0_CLK / LOG_FREQ (10kHz = 100micro seconds) */
#define LOG_DIV ((PCLK / T0_PCLK_DIV) / LOG_FREQ)

static inline void main_init( void );
static inline void main_periodic_task( void );
int main_log(void);

void set_filename(unsigned int local, char* name);
unsigned char checksum(unsigned char start, unsigned char* data, int length);
unsigned int getclock(void);
void log_payload(int len, unsigned char source, unsigned int timestamp);
void log_xbee(unsigned char c);
void log_pprz(unsigned char c);

extern int main_mass_storage(void);

DirList list;
EmbeddedFileSystem efs;
EmbeddedFile filer;	
EmbeddedFile filew;

unsigned char xbeel_payload[XBEE_PAYLOAD_LEN];
unsigned char pprzl_payload[PPRZ_PAYLOAD_LEN];
volatile unsigned char xbeel_payload_len;
volatile unsigned char pprzl_payload_len;
unsigned char xbeel_error;
unsigned char pprzl_error;
unsigned char log_buffer[MSG_SIZE]  __attribute__ ((aligned));
static unsigned int xbeel_timestamp = 0;
static unsigned int pprzl_timestamp = 0;
unsigned int nb_messages = 0;
unsigned int nb_fail_write = 0;
int bytes = 0;
unsigned int clock_msb = 0;
unsigned int clock_lsb_last = 0;

void set_filename(unsigned int local, char* name)
{
    /* do not use sprintf or similar */
    int i;
    
    for (i=7; i>=0; i--) {
        name[i] = (local % 10) + '0';
        local /= 10;
    }
    name[8]='.';name[9]='t';name[10]='x';name[11]='t';name[12]=0;
}

unsigned char checksum(unsigned char start, unsigned char* data, int length)
{
    int i;
    unsigned char retval = start;
    for (i=0;i<length;i++) retval += data[i];

    return retval;
}

unsigned int getclock(void)
{
    uint64_t clock;
    uint32_t clock_lsb;

    clock_lsb = T0TC;

    if (clock_lsb < clock_lsb_last) clock_msb++;
    clock_lsb_last = clock_lsb;

    clock = (((uint64_t)clock_msb << 32) | (uint64_t)clock_lsb) / LOG_DIV;

    return(clock & 0xFFFFFFFF);
}

/** Parsing a frame data and copy the payload to the log buffer */
void log_payload(int len, unsigned char source, unsigned int timestamp)
{
  unsigned char chk;
  
  /* start delimiter */
  log_buffer[0] = STX;

  /* length is just payload */
  log_buffer[1] = len & 0xFF;

  /* source */
  log_buffer[2] = source;
    
  /* add a 32bit timestamp */
  log_buffer[3] = (timestamp) & 0xFF;       // LSB first
  log_buffer[4] = (timestamp >> 8)  & 0xFF;
  log_buffer[5] = (timestamp >> 16) & 0xFF;
  log_buffer[6] = (timestamp >> 24) & 0xFF;

  /* data is already written */

  /* calculate checksum over start+length+timestamp+data */
  log_buffer[LOG_DATA_OFFSET+len] = checksum(0, &log_buffer[1], LOG_DATA_OFFSET+len-1);

  /* write data, start+length+timestamp+data+checksum */
  chk = file_write(&filew, LOG_DATA_OFFSET+len+1, log_buffer);

  if (len != chk) 
  {
    nb_fail_write++;
  }
  
  bytes += chk;
  nb_messages++;
//  dl_parse_msg();
}

/** Parsing a XBee API frame */
void log_xbee(unsigned char c)
{
  static unsigned char xbeel_status = XBEE_UNINIT;
  static unsigned char cs, payload_idx, i;

  switch (xbeel_status) {
  case XBEE_UNINIT:
    if (c == XBEE_START)
    {
      xbeel_timestamp = getclock();
      xbeel_status++;
    }
    break;
  case XBEE_GOT_START:
    xbeel_payload_len = c<<8;
    xbeel_status++;
    break;
  case XBEE_GOT_LENGTH_MSB:
    xbeel_payload_len |= c;
    xbeel_status++;
    payload_idx = 0;
    cs=0;
    break;
  case XBEE_GOT_LENGTH_LSB:
    xbeel_payload[payload_idx] = c;
    cs += c;
    payload_idx++;
    if (payload_idx == xbeel_payload_len)
      xbeel_status++;
    break;
  case XBEE_GOT_PAYLOAD:
    if (c + cs != 0xff)
      goto error;
    if ((xbeel_payload[0] != XBEE_RX16_ID) &&
        (xbeel_payload[0] != XBEE_TX16_ID))
      goto error;
    /* copy the XBee message to the logger buffer */
    for (i = 0; i < xbeel_payload_len-XBEE_RFDATA_OFFSET; i++) {
      log_buffer[i+LOG_DATA_OFFSET] = xbeel_payload[i+XBEE_RFDATA_OFFSET];
    }
    log_payload(xbeel_payload_len-XBEE_RFDATA_OFFSET, 0, xbeel_timestamp);
    goto restart;
  }
  return;
 error:
  xbeel_error++;
 restart:
  xbeel_status = XBEE_UNINIT;
  return;
}

void log_pprz(unsigned char c)
{
  static unsigned char pprzl_status = UNINIT;
  static unsigned char _ck_a, _ck_b, payload_idx, i;

  switch (pprzl_status) {
  case UNINIT:
    if (c == STX)
      pprzl_timestamp = getclock();
      pprzl_status++;
    break;
  case GOT_STX:
    pprzl_payload_len = c-4; /* Counting STX, LENGTH and CRC1 and CRC2 */
    _ck_a = _ck_b = c;
    pprzl_status++;
    payload_idx = 0;
    break;
  case GOT_LENGTH:
    pprzl_payload[payload_idx] = c;
    _ck_a += c; _ck_b += _ck_a;
    payload_idx++;
    if (payload_idx == pprzl_payload_len)
      pprzl_status++;
    break;
  case GOT_PAYLOAD:
    if (c != _ck_a)
      goto error;
    pprzl_status++;
    break;
  case GOT_CRC1:
    if (c != _ck_b)
      goto error;
    /* copy the pprz message to the logger buffer */
    for (i = 0; i < pprzl_payload_len; i++) {
      log_buffer[i+LOG_DATA_OFFSET] = pprzl_payload[i];
    }
    log_payload(pprzl_payload_len, 0, pprzl_timestamp);
    goto restart;
  }
  return;
 error:
  pprzl_error++;
 restart:
  pprzl_status = UNINIT;
  return;
}

int main_log(void)
{
    unsigned int count;
    unsigned char name[13];
    unsigned char inc;               

	if(efs_init(&efs, 0) != 0) {
		return(-1);
	}

    /* find an unused file number the dumb way */
    for (count = 1; count < 0xFFFFFFF; count++)
    {
        set_filename(count, name);
        if(file_fopen(&filer, &efs.myFs, name,'r')!=0) break;
        file_fclose(&filer);
    }

    if (file_fopen(&filew, &efs.myFs, name, 'w' ) != 0) 
    {
		return(-1);
    }

    /* write to SD until key is pressed */
    LED_ON(3);
    while ((IO0PIN & _BV(STOP_KEY))>>STOP_KEY)
    {
        if (Uart1ChAvailable())
        {   
			LED_TOGGLE(2);
			inc = Uart1Getch();
            log_pprz(inc);
//            log_xbee(inc);
//            file_write(&filew, 1, &inc);
        }
    }
    LED_OFF(2);
    LED_OFF(3);

    file_fclose( &filew );
    fs_umount( &efs.myFs ) ;
 
main_mass_storage();
   
	return(0);
}

int main( void ) {
  main_init();

  main_log();

  while(1) {
    if (sys_time_periodic())
      main_periodic_task();
  }
  return 0;
}

static inline void main_init( void ) {
  hw_init();
  sys_time_init();
  led_init();
#ifdef USE_UART1
    Uart1Init();
#endif
  int_enable();
}

static inline void main_periodic_task( void ) {
  LED_TOGGLE(1);
}
