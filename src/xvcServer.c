/* This work, "xvcServer.c", is a derivative of "xvcd.c" (https://github.com/tmbinc/xvcd)
 * by tmbinc, used under CC0 1.0 Universal (http://creativecommons.org/publicdomain/zero/1.0/).
 * "xvcServer.c" is licensed under CC0 1.0 Universal (http://creativecommons.org/publicdomain/zero/1.0/)
 * by Avnet and is used by Xilinx for XAPP1251.
 *
 *  Description : XAPP1251 Xilinx Virtual Cable Server for Linux
 *
 * Support for FT2232H has been added by Wojciech M. Zabolotny (wzab@ise.pw.edu.pl) basing on the
 * https://github.com/barawn/xvcd-anita project.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include <sys/mman.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <pthread.h>

#include "ftdi_xvc_core.h"

#define DEFAULT_VENDOR  0x0403
#define DEFAULT_PRODUCT 0x6010
#define DEFAULT_PORT    2542
#define DEFAULT_CLOCK   6000000  // 6 MHz
#define MAX_VECTOR_LEN  32768    // Maximum shift length in bits

static int verbose = 0;
static ftdi_xvc_ctx *xvc_ctx = NULL;

static void print_usage(const char *prog) {
  fprintf(stderr, "Usage: %s [options]\n", prog);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  -v            Verbose output\n");
  fprintf(stderr, "  -p <port>     TCP port (default: %d)\n", DEFAULT_PORT);
  fprintf(stderr, "  -V <vendor>   USB vendor ID in hex (default: 0x%04x)\n", DEFAULT_VENDOR);
  fprintf(stderr, "  -P <product>  USB product ID in hex (default: 0x%04x)\n", DEFAULT_PRODUCT);
  fprintf(stderr, "  -s <serial>   USB device serial number\n");
  fprintf(stderr, "  -c <clock>    TCK clock in MHz (default: 6)\n");
  fprintf(stderr, "  -i <iface>    FTDI interface: A, B, C, or D (default: A)\n");
}

static int sread(int fd, void *target, int len) {
  unsigned char *t = target;
  while (len) {
    int r = read(fd, t, len);
    if (r <= 0)
      return r;
    t += r;
    len -= r;
  }
  return 1;
}

static int handle_data(int fd) {
  const char xvcInfo[] = "xvcServer_v1.0:32768\n";

  do {
    int len, nr_bytes;
    char cmd[16];
    unsigned char blen[4];
    unsigned char buffer[32768], result[16384];
    memset(cmd, 0, 16);

    if (sread(fd, cmd, 2) != 1)
      return 1;

    if (memcmp(cmd, "ge", 2) == 0) {
      if (sread(fd, cmd, 6) != 1)
        return 1;
      memcpy(result, xvcInfo, strlen(xvcInfo));
      if (write(fd, result, strlen(xvcInfo)) != strlen(xvcInfo)) {
        perror("write");
        return 1;
      }
      if (verbose) {
        printf("%u : Received command: 'getinfo'\n", (int)time(NULL));
        printf("\t Replied with %s\n", xvcInfo);
      }
      break;
    } else if (memcmp(cmd, "se", 2) == 0) {
      if (sread(fd, cmd, 9) != 1)
        return 1;
      // Extract requested period (little-endian uint32 at cmd+5)
      uint32_t requested_period = (unsigned char)cmd[5] |
                                  ((unsigned char)cmd[6] << 8) |
                                  ((unsigned char)cmd[7] << 16) |
                                  ((unsigned char)cmd[8] << 24);
      // Set the TCK period and get actual achieved period
      uint32_t actual_period = ftdi_xvc_set_tck_period(xvc_ctx, requested_period);
      // Send back actual period (little-endian)
      result[0] = actual_period & 0xff;
      result[1] = (actual_period >> 8) & 0xff;
      result[2] = (actual_period >> 16) & 0xff;
      result[3] = (actual_period >> 24) & 0xff;
      if (write(fd, result, 4) != 4) {
        perror("write");
        return 1;
      }
      if (verbose) {
        printf("%u : Received command: 'settck'\n", (int)time(NULL));
        printf("\t Requested period: %u ns, actual: %u ns\n", requested_period, actual_period);
      }
      break;
    } else if (memcmp(cmd, "sh", 2) == 0) {
      if (sread(fd, cmd, 4) != 1)
        return 1;
      if (verbose) {
        printf("%u : Received command: 'shift'\n", (int)time(NULL));
      }
    } else {
      fprintf(stderr, "invalid cmd '%s'\n", cmd);
      return 1;
    }
    /* Here we go only during the shift command */
    if (sread(fd, blen, 4) != 1)
      return 1;
    len = blen[0] + 256 * (blen[1] + 256 * (blen[2] + 256 * blen[3]));
    if (len > MAX_VECTOR_LEN) {
      fprintf(stderr, "shift length %d exceeds maximum %d\n", len, MAX_VECTOR_LEN);
      return 1;
    }
    nr_bytes = (len + 7) / 8;
    if (sread(fd, buffer, nr_bytes * 2) != 1)
      return 1;
    if (ftdi_xvc_shift_command(xvc_ctx, len, buffer, result))
      return 1;
    if (write(fd, result, nr_bytes) != nr_bytes) {
      perror("write");
      return 1;
    }
  } while (1);
  return 0;
}

int main(int argc, char **argv) {
  int i;
  int s;
  int c;

  // Configurable options with defaults
  int port = DEFAULT_PORT;
  int vendor = DEFAULT_VENDOR;
  int product = DEFAULT_PRODUCT;
  const char *serial = NULL;
  unsigned int clock_hz = DEFAULT_CLOCK;
  enum ftdi_interface iface = INTERFACE_A;

  struct sockaddr_in address;

  opterr = 0;

  while ((c = getopt(argc, argv, "vp:V:P:s:c:i:h")) != -1) {
    switch (c) {
    case 'v':
      verbose = 1;
      break;
    case 'p':
      port = atoi(optarg);
      break;
    case 'V':
      vendor = strtol(optarg, NULL, 0);
      break;
    case 'P':
      product = strtol(optarg, NULL, 0);
      break;
    case 's':
      serial = optarg;
      break;
    case 'c':
      clock_hz = (unsigned int)(atof(optarg) * 1000000);
      break;
    case 'i':
      switch (optarg[0]) {
      case 'A': case 'a': iface = INTERFACE_A; break;
      case 'B': case 'b': iface = INTERFACE_B; break;
      case 'C': case 'c': iface = INTERFACE_C; break;
      case 'D': case 'd': iface = INTERFACE_D; break;
      default:
        fprintf(stderr, "Invalid interface: %s\n", optarg);
        return 1;
      }
      break;
    case 'h':
      print_usage(*argv);
      return 0;
    case '?':
      print_usage(*argv);
      return 1;
    }
  }

  xvc_ctx = ftdi_xvc_create(verbose);
  if (!xvc_ctx) {
    fprintf(stderr, "Failed to create XVC context\n");
    return 1;
  }

  if (ftdi_xvc_open_device(xvc_ctx, vendor, product, serial, iface) < 0) {
    ftdi_xvc_destroy(xvc_ctx);
    return 1;
  }

  if (ftdi_xvc_init_mpsse(xvc_ctx, clock_hz) < 0) {
    ftdi_xvc_close_device(xvc_ctx);
    ftdi_xvc_destroy(xvc_ctx);
    return 1;
  }

  s = socket(AF_INET, SOCK_STREAM, 0);

  if (s < 0) {
    perror("socket");
    ftdi_xvc_close_device(xvc_ctx);
    ftdi_xvc_destroy(xvc_ctx);
    return 1;
  }

  i = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &i, sizeof i);

  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);
  address.sin_family = AF_INET;

  if (bind(s, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("bind");
    close(s);
    ftdi_xvc_close_device(xvc_ctx);
    ftdi_xvc_destroy(xvc_ctx);
    return 1;
  }

  if (listen(s, 1) < 0) {
    perror("listen");
    close(s);
    ftdi_xvc_close_device(xvc_ctx);
    ftdi_xvc_destroy(xvc_ctx);
    return 1;
  }

  fd_set conn;
  int maxfd = 0;

  FD_ZERO(&conn);
  FD_SET(s, &conn);

  maxfd = s;

  while (1) {
    fd_set read_fds = conn, except_fds = conn;
    int fd;

    if (select(maxfd + 1, &read_fds, 0, &except_fds, 0) < 0) {
      perror("select");
      break;
    }

    for (fd = 0; fd <= maxfd; ++fd) {
      if (FD_ISSET(fd, &read_fds)) {
        if (fd == s) {
          int newfd;
          socklen_t nsize = sizeof(address);

          newfd = accept(s, (struct sockaddr *)&address, &nsize);

          if (newfd < 0) {
            perror("accept");
          } else {
            if (verbose)
              printf("connection accepted - fd %d\n", newfd);
            int flag = 1;
            int optResult = setsockopt(newfd, IPPROTO_TCP, TCP_NODELAY,
                                       (char *)&flag, sizeof(int));
            if (optResult < 0)
              perror("TCP_NODELAY error");
            if (newfd > maxfd) {
              maxfd = newfd;
            }
            FD_SET(newfd, &conn);
          }
        } else if (handle_data(fd)) {
          if (verbose)
            printf("connection closed - fd %d\n", fd);
          close(fd);
          FD_CLR(fd, &conn);
        }
      } else if (FD_ISSET(fd, &except_fds)) {
        if (verbose)
          printf("connection aborted - fd %d\n", fd);
        close(fd);
        FD_CLR(fd, &conn);
        if (fd == s)
          break;
      }
    }
  }

  close(s);
  ftdi_xvc_close_device(xvc_ctx);
  ftdi_xvc_destroy(xvc_ctx);
  return 0;
}
