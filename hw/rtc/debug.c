/*
 * lua_device for QEMU
 *
 * Yuri Stepanenko stepanenkoyra@gmail.com   2022
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include <stdio.h>
#include <stdarg.h>
#include "debug.h"


/**
 * \brief The main debug message output function
 */
void DebugMessage(FILE *fd, int level, const char *prefix,
                    const char *suffix, const char *function, int line, const char *errFmt, ...)
{
  va_list arg;

  if (level >= MSG_LEVEL)
  {
    switch (level)
    {
      case MSG_INFO :
        fprintf(fd, "INFO: ");
        break;
      case MSG_WARNING :
        fprintf(fd, "WARNING: ");
        break;
      case MSG_ERROR :
        fprintf(fd, "ERROR: ");
        break;
    }

    if (prefix)
      fprintf(fd, "%s: ", prefix);

#ifdef CONFIG_DBG_SHOW_FUNCTION
    if (line > 0)
      fprintf(fd, "%s: ", function);
#endif

#ifdef CONFIG_DBG_SHOW_LINE_NUM
    if (line > 0)
      fprintf(fd, "@%d - ", line);
#endif

    va_start(arg, errFmt);
    vfprintf(fd, errFmt, arg);
    va_end(arg);

    if (suffix)
      fprintf(fd, "%s", suffix);
  }
}

