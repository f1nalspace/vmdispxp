/*
 * The handful of C runtime functions the display driver needs, linked in statically.
 *
 * Copyright (C) 2026 qemu-3dfx-build
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

/* An XPDM display driver runs inside win32k and may import from win32k.sys only.
 * Linking against ntoskrnl for memcpy and memset produces a second import
 * descriptor, and EngLoadImage then refuses the driver -- it loads without a word
 * of complaint anywhere. ReactOS avoids this by linking its own libcntpr; these
 * two functions are all we need of it.
 *
 * Built with -fno-tree-loop-distribute-patterns so that GCC does not recognise the
 * loops below and replace them with calls to themselves.
 */

#include <stddef.h>

void *memcpy(void *Destination, const void *Source, size_t Count)
{
   unsigned char *DestinationBytes = (unsigned char *)Destination;
   const unsigned char *SourceBytes = (const unsigned char *)Source;
   size_t Index;

   for (Index = 0; Index < Count; Index++)
      DestinationBytes[Index] = SourceBytes[Index];

   return Destination;
}

void *memset(void *Destination, int Value, size_t Count)
{
   unsigned char *DestinationBytes = (unsigned char *)Destination;
   unsigned char ValueByte = (unsigned char)Value;
   size_t Index;

   for (Index = 0; Index < Count; Index++)
      DestinationBytes[Index] = ValueByte;

   return Destination;
}
