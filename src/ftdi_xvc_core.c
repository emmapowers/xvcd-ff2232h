// Basic XVC core for an FTDI device in MPSSE mode, using
// the libftdi library under Linux. I've tested this under Linux:
// I guess it could work under Windows as well, but since XVC
// is network based, I never saw the point.

// Thanks to tmbinc for the original xvcd implementation:
// this code however is (I'm pretty sure) a total rewrite of the
// physical layer code.

// Author: P.S. Allison (allison.122@osu.edu)
// This code is in the public domain (CC0):
// see https://wiki.creativecommons.org/CC0

// Significantly rewritten by Wojciech M. Zabolotny (wzab@ise.pw.edu.pl)
// removed all assumptions how Xilinx tools use XVC, because Vivado
// didn't work with the original version

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <ftdi.h>

static unsigned int ftdi_verbosity;
static struct ftdi_context ftdi;
static enum ftdi_interface ftdi_iface = INTERFACE_A;

#define DEBUGCOND(lvl) (lvl<=ftdi_verbosity)
#define DEBUG(lvl,...) if (lvl<=ftdi_verbosity) printf(__VA_ARGS__)
#define DEBUGPRINTF(...) printf(__VA_ARGS__)

// Base clock for FT2232H in MPSSE mode is 60 MHz
#define FTDI_BASE_CLOCK_HZ 60000000

/** \brief Calculate TCK divisor for a given frequency in Hz. */
static unsigned int calc_tck_divisor(unsigned int freq_hz) {
  if (freq_hz >= FTDI_BASE_CLOCK_HZ / 2)
    return 0;
  return (FTDI_BASE_CLOCK_HZ / (2 * freq_hz)) - 1;
}

/** \brief Read bytes from the FTDI device, possibly in multiple chunks. */
int ftdi_xvc_read_bytes(unsigned int len, unsigned char *buf) {
  int bytes_read = 0;
  int to_read = len;
  while (bytes_read < to_read) {
    int result = ftdi_read_data(&ftdi, buf + bytes_read, to_read - bytes_read);
    if (result < 0) {
      fprintf(stderr, "xvcd: %s : read error: %s\n", __FUNCTION__, ftdi_get_error_string(&ftdi));
      return -1;
    }
    bytes_read += result;
  }
  return bytes_read;
}


/** \brief Close the FTDI device. */
void ftdi_xvc_close_device() {
  ftdi_usb_reset(&ftdi);
  ftdi_usb_close(&ftdi);
  ftdi_deinit(&ftdi);
}

/** \brief Initialize the FTDI library. */
void ftdi_xvc_init(unsigned int verbosity) 
{
  ftdi_init(&ftdi);
  ftdi_verbosity = verbosity;
}

/** \brief Open the FTDI device.
 *  \param vendor USB vendor ID
 *  \param product USB product ID
 *  \param serial USB serial number string (NULL for any)
 *  \param iface FTDI interface (INTERFACE_A, INTERFACE_B, etc.)
 */
int ftdi_xvc_open_device(int vendor, int product, const char *serial, enum ftdi_interface iface)
{
  ftdi_iface = iface;
  // ftdi_set_interface must be called before ftdi_usb_open
  ftdi_set_interface(&ftdi, ftdi_iface);
  if (ftdi_usb_open_desc(&ftdi, vendor, product, NULL, serial) < 0)
    {
      fprintf(stderr, "xvcd: %s : can't open device: %s\n", __FUNCTION__, ftdi_get_error_string(&ftdi));
      return -1;
    }
  ftdi_usb_reset(&ftdi);
  ftdi_set_latency_timer(&ftdi, 1);
  return 0;
}

/** \brief Fetch the FTDI context, in case someone else wants to muck with the device before we do. */
struct ftdi_context *ftdi_xvc_get_context() 
{
  return &ftdi;
}

/** \brief Set TCK period dynamically.
 *  \param period_ns Requested period in nanoseconds
 *  \return Actual achieved period in nanoseconds, or 0 on error
 */
uint32_t ftdi_xvc_set_tck_period(uint32_t period_ns) {
  // Convert period to frequency: freq = 1e9 / period_ns
  unsigned int freq_hz = (period_ns > 0) ? (1000000000U / period_ns) : FTDI_BASE_CLOCK_HZ;
  unsigned int divisor = calc_tck_divisor(freq_hz);

  // Clamp divisor to valid range (0-65535)
  if (divisor > 65535)
    divisor = 65535;

  unsigned char buf[3] = {
    TCK_DIVISOR, divisor & 0xff, (divisor >> 8) & 0xff
  };

  if (ftdi_write_data(&ftdi, buf, 3) != 3) {
    fprintf(stderr, "xvcd: %s : failed to set TCK divisor\n", __FUNCTION__);
    return 0;
  }

  // Calculate actual achieved period: period = 1e9 / actual_freq
  // actual_freq = FTDI_BASE_CLOCK_HZ / (2 * (divisor + 1))
  unsigned int actual_freq = FTDI_BASE_CLOCK_HZ / (2 * (divisor + 1));
  return 1000000000U / actual_freq;
}

/** \brief Initialize the MPSSE engine on the FTDI device.
 *  \param freq_hz Initial TCK frequency in Hz
 */
int ftdi_xvc_init_mpsse(unsigned int freq_hz) {
  int res;
  unsigned char byte;
  unsigned int divisor = calc_tck_divisor(freq_hz);

  unsigned char buf[7] = {
    SET_BITS_LOW, 0x08, 0x0B,  // Set TMS high, TCK/TDI/TMS as outputs.
    TCK_DIVISOR, divisor & 0xff, (divisor >> 8) & 0xff,
    SEND_IMMEDIATE
  };
  ftdi_set_bitmode(&ftdi, 0x0B, BITMODE_BITBANG);
  ftdi_set_bitmode(&ftdi, 0x0B, BITMODE_MPSSE);
  // Flush any pending data from the device
  while ((res = ftdi_read_data(&ftdi, &byte, 1)) > 0)
    ;
  if (ftdi_write_data(&ftdi, buf, 7) != 7)
    {
      fprintf(stderr, "xvcd: %s : FTDI initialization failed.\n", __FUNCTION__);
      return -1;
    }
  return 0;
}
typedef struct
{
  char oper; //0-byte shift, 1-bit shift, 2-TMS bit shift
  int len; //length of the operation
} data_desc;

#define MAX_DATA 4096
/* WZab - simplified command for shifting */
/** \brief Handle a 'shift:' command sent via XVC. */
int ftdi_xvc_shift_command(unsigned int len,
			   unsigned char *buffer,
			   unsigned char *result) 
{
  int i;
  int nr_bytes;
  int cur_byte_pos=0;
  int bit_pos = 0;
  int left;
  nr_bytes = (len+7)/8;
  left = len;
  unsigned char ftdi_cmd[MAX_DATA];
  unsigned char ftdi_res[MAX_DATA];
  data_desc ftdi_desc[MAX_DATA];
  //Prepare the result buffer
  memset(result,0,nr_bytes);
  while(left) { //loop until there are data to handle
    int rd_len = 0;
    int nr_opers = 0;
    int wr_ptr = 0;
    int desc_pos = 0;
    int in_cmd_building = 0;
    int last_len = 0;
    int cur_len = 0;
    while( ( desc_pos < (MAX_DATA-8)) && // We may generate up to 9 descriptors in a single iteration
	   ( wr_ptr < (MAX_DATA-25)) && // We may generate up to 24 command bytes in a single iteration
	   (left > 0)
	   ) { //loop until we may add a command to the current set
      if((left>7) && (buffer[cur_byte_pos] == 0)) { //No TMS and at least 1 byte to transmit
	if(in_cmd_building == 0) {
	  in_cmd_building = 1;
	  ftdi_cmd[wr_ptr++]=MPSSE_DO_WRITE|MPSSE_DO_READ|MPSSE_LSB|MPSSE_WRITE_NEG;
	  last_len=wr_ptr; //Save pointer position, to write the length
	  wr_ptr += 2; //reserve space for length
	  cur_len = -1; //will be increased when the byte is added
	}
	ftdi_cmd[wr_ptr++] = buffer[cur_byte_pos+nr_bytes];
	rd_len += 1; //It generates one byte for reading
	cur_len += 1;
	cur_byte_pos++;
	left -= 8;
      } else {
	//It is no standard shift, so if we created a standard shift command before,
	//we must complete it
	if(in_cmd_building) { //Complete the last command
	  ftdi_cmd[last_len] = cur_len & 0xff;
	  ftdi_cmd[last_len+1]= (cur_len >> 8) & 0xff;
	  in_cmd_building = 0;
	  //Add the descriptor to the read descriptors
	  ftdi_desc[desc_pos].oper = 0;
	  ftdi_desc[desc_pos++].len = cur_len;
	}
	if ((buffer[cur_byte_pos] == 0) && (left<=7)) { //No TMS, bit shift of last bits
	  ftdi_cmd[wr_ptr++] = MPSSE_DO_WRITE|MPSSE_DO_READ|MPSSE_LSB|MPSSE_BITMODE|MPSSE_WRITE_NEG;
	  ftdi_cmd[wr_ptr++] = left-1;
	  ftdi_cmd[wr_ptr++] = buffer[cur_byte_pos+nr_bytes];
	  rd_len += 1;
	  cur_byte_pos++;
	  //Add the descriptor to the read descriptors
	  ftdi_desc[desc_pos].oper = 1;
	  ftdi_desc[desc_pos++].len = left-1;                
	  left = 0;
	} else if (buffer[cur_byte_pos] != 0) { //TMS shift, convert it into a set of TMS shifts
	  int i;
	  for(i=0;i<8;i++) {
	    if(left==0) break; //This could be the last byte!
	    ftdi_cmd[wr_ptr++] = MPSSE_WRITE_TMS|MPSSE_DO_READ|MPSSE_LSB|MPSSE_BITMODE|MPSSE_WRITE_NEG;
	    ftdi_cmd[wr_ptr++] = 0; //One bit - 0!
	    ftdi_cmd[wr_ptr++] = 
	      ((buffer[cur_byte_pos] & (1<<i)) ? 0x01 : 0x00) |
	      ((buffer[cur_byte_pos+nr_bytes] & (1<<i)) ? 0x80 : 0x00);
	    left--;
	    rd_len += 1;
	    //Add the descriptor to the read descriptors
	    ftdi_desc[desc_pos].oper = 2;
	    ftdi_desc[desc_pos++].len = 0;                
	  }
	  cur_byte_pos++;
	}
      } // else
    } //while
      // We must complete the last command if it has not been completed yet
      //The code below should be the same, as in the loop!
    if(in_cmd_building) { //Complete the last command
      ftdi_cmd[last_len] = cur_len & 0xff;
      ftdi_cmd[last_len+1]= (cur_len >> 8) & 0xff;
      in_cmd_building = 0;
      //Add the descriptor to the read descriptors
      ftdi_desc[desc_pos].oper = 0;
      ftdi_desc[desc_pos++].len = cur_len;
    }
    //Send the created command list
    if (ftdi_write_data(&ftdi, ftdi_cmd, wr_ptr) != wr_ptr) {
      return 1;
    }
    //Read the response
    if (ftdi_xvc_read_bytes(rd_len, ftdi_res) < 0) {
      return 1;
    }
    //Unpack the response basing on the read descriptors
    //Please note, that the responses do not always come as full bits!
    int rd_byte_pos = 0;
    for(i=0;i<desc_pos;i++) {
      int j;
      //Reading depends on the type of command
      switch(ftdi_desc[i].oper) {
      case 0: //Standard shift
	for(j=0;j<=ftdi_desc[i].len;j++) {
	  int bnr;
	  for(bnr=0;bnr<8;bnr++) {
	    result[bit_pos/8] |= (ftdi_res[rd_byte_pos] & (1<<bnr)) ? (1<<(bit_pos & 7)) : 0;
	    bit_pos++;
	  }
	  rd_byte_pos++;
	}
	break;
      case 1://Bit shift
	{
	  int bnr; //Please note, that the received bits are shifted from the MSB!
	  for(bnr=7-ftdi_desc[i].len;bnr<8;bnr++) {
	    result[bit_pos/8] |= (ftdi_res[rd_byte_pos] & (1<<bnr)) ? (1<<(bit_pos & 7)) : 0;
	    bit_pos++;
	  }
	  rd_byte_pos++;
	}
	break;
      case 2://TMS shift
	result[bit_pos/8] |= (ftdi_res[rd_byte_pos] & 0x80) ? (1<<(bit_pos & 7)) : 0;
	bit_pos++;
	rd_byte_pos++;
	break;
      }
    }
  }
  return 0;
}
