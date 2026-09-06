/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercises the non-blocking read path of the SANE backend.
 *
 * scanimage never calls sane_set_io_mode(TRUE), so this is the only way to
 * cover it.  It dlopens the backend directly - PANTUM_SO points at
 * libsane-pantum.so.1 - and reports how many reads came back empty.
 *
 * Needs a C compiler and the SANE headers in PATH/include:
 *   cc -std=c99 -Wall -Wextra -o nonblocking_test proto/nonblocking_test.c -ldl
 *   PANTUM_SO=<prefix>/lib/sane/libsane-pantum.so.1 ./nonblocking_test usb:003:004
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sane/sane.h>

typedef SANE_Status (*fn_init) (SANE_Int *, SANE_Auth_Callback);
typedef SANE_Status (*fn_open) (SANE_String_Const, SANE_Handle *);
typedef SANE_Status (*fn_io) (SANE_Handle, SANE_Bool);
typedef SANE_Status (*fn_fd) (SANE_Handle, SANE_Int *);
typedef SANE_Status (*fn_start) (SANE_Handle);
typedef SANE_Status (*fn_read) (SANE_Handle, SANE_Byte *, SANE_Int,
                                SANE_Int *);
typedef void (*fn_close) (SANE_Handle);
typedef void (*fn_exit) (void);

int
main (int argc, char **argv)
{
  const char *dev = argc > 1 ? argv[1] : "usb:003:004";
  void *lib = dlopen (getenv ("PANTUM_SO"), RTLD_NOW);
  fn_init f_init;
  fn_open f_open;
  fn_io f_io;
  fn_fd f_fd;
  fn_start f_start;
  fn_read f_read;
  fn_close f_close;
  fn_exit f_exit;
  SANE_Handle h;
  SANE_Status st;
  SANE_Int ver, fd = -1, len;
  static SANE_Byte buf[65536];
  long empty = 0, full = 0, total = 0;

  if (!lib)
    {
      fprintf (stderr, "dlopen: %s\n", dlerror ());
      return 1;
    }
  f_init = (fn_init)dlsym (lib, "sane_pantum_init");
  f_open = (fn_open)dlsym (lib, "sane_pantum_open");
  f_io = (fn_io)dlsym (lib, "sane_pantum_set_io_mode");
  f_fd = (fn_fd)dlsym (lib, "sane_pantum_get_select_fd");
  f_start = (fn_start)dlsym (lib, "sane_pantum_start");
  f_read = (fn_read)dlsym (lib, "sane_pantum_read");
  f_close = (fn_close)dlsym (lib, "sane_pantum_close");
  f_exit = (fn_exit)dlsym (lib, "sane_pantum_exit");

  if (f_init (&ver, NULL) != SANE_STATUS_GOOD)
    return 1;
  st = f_open (dev, &h);
  printf ("open: %d\n", st);
  if (st != SANE_STATUS_GOOD)
    return 1;

  printf ("set_io_mode(FALSE) = %d (0 = GOOD)\n", f_io (h, SANE_FALSE));
  printf ("set_io_mode(TRUE)  = %d (0 = GOOD)\n", f_io (h, SANE_TRUE));
  printf ("get_select_fd      = %d (1 = UNSUPPORTED)\n", f_fd (h, &fd));

  st = f_start (h);
  printf ("start: %d\n", st);
  if (st == SANE_STATUS_GOOD)
    {
      for (;;)
        {
          st = f_read (h, buf, (SANE_Int)sizeof (buf), &len);
          if (st == SANE_STATUS_EOF)
            break;
          if (st != SANE_STATUS_GOOD)
            {
              printf ("read: %d\n", st);
              break;
            }
          if (len == 0)
            empty++;
          else
            {
              full++;
              total += len;
            }
        }
      printf ("reads: %ld empty, %ld with data, %ld bytes\n", empty, full,
              total);
    }
  f_close (h);
  f_exit ();
  return 0;
}
