#ifndef FTDI_XVC_CORE_H
#define FTDI_XVC_CORE_H

#include <stdint.h>
#include <ftdi.h>

void ftdi_xvc_init(unsigned int verbosity);

void ftdi_xvc_close_device(void);

struct ftdi_context *ftdi_xvc_get_context(void);

int ftdi_xvc_open_device(int vendor, int product, const char *serial, enum ftdi_interface iface);

int ftdi_xvc_init_mpsse(unsigned int freq_hz);

uint32_t ftdi_xvc_set_tck_period(uint32_t period_ns);

int ftdi_xvc_shift_command(unsigned int len,
			   unsigned char *buffer,
			   unsigned char *result);

#endif // FTDI_XVC_CORE_H