/* file I/O for lvc: load file into buffer, save buffer to file. */
#ifndef LVC_IO_H
#define LVC_IO_H

#include "buffer.h"

/* load file at path into buffer (replaces content). returns 0 or -1. */
int lvc_io_load(lvc_buf *b, const char *path);

/* write buffer contents to path atomically (tmp + rename). returns 0 or -1. */
int lvc_io_save(lvc_buf *b, const char *path);

#endif
