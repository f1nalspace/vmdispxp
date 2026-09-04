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
 *
 * They copy a machine word at a time wherever the alignment allows it. That is not
 * premature: the shadow buffer hands every dirty rectangle to memcpy, so a byte loop
 * here would cost more than the whole shadow saves.
 */

#include <stddef.h>

typedef unsigned long CopyWord;

static const size_t bytesPerCopyWord = sizeof(CopyWord);

void *memcpy(void *Destination, const void *Source, size_t Count)
{
   unsigned char *DestinationBytes = (unsigned char *)Destination;
   const unsigned char *SourceBytes = (const unsigned char *)Source;
   size_t Index = 0;

   /* Word copies only pay off when both sides sit at the same offset inside a word;
    * otherwise every word would have to be assembled from two, which is slower than
    * letting the byte loop run. */
   size_t DestinationOffsetInWord = ((size_t)DestinationBytes) % bytesPerCopyWord;
   size_t SourceOffsetInWord = ((size_t)SourceBytes) % bytesPerCopyWord;

   if (DestinationOffsetInWord == SourceOffsetInWord && Count >= bytesPerCopyWord * 2)
   {
      size_t BytesUntilAligned = (bytesPerCopyWord - DestinationOffsetInWord) % bytesPerCopyWord;
      size_t WordCount;
      size_t WordIndex;
      CopyWord *DestinationWords;
      const CopyWord *SourceWords;

      while (Index < BytesUntilAligned)
      {
         DestinationBytes[Index] = SourceBytes[Index];
         Index++;
      }

      DestinationWords = (CopyWord *)(DestinationBytes + Index);
      SourceWords = (const CopyWord *)(SourceBytes + Index);
      WordCount = (Count - Index) / bytesPerCopyWord;

      for (WordIndex = 0; WordIndex < WordCount; WordIndex++)
         DestinationWords[WordIndex] = SourceWords[WordIndex];

      Index += WordCount * bytesPerCopyWord;
   }

   while (Index < Count)
   {
      DestinationBytes[Index] = SourceBytes[Index];
      Index++;
   }

   return Destination;
}

void *memset(void *Destination, int Value, size_t Count)
{
   unsigned char *DestinationBytes = (unsigned char *)Destination;
   unsigned char ValueByte = (unsigned char)Value;
   size_t Index = 0;

   if (Count >= bytesPerCopyWord * 2)
   {
      size_t DestinationOffsetInWord = ((size_t)DestinationBytes) % bytesPerCopyWord;
      size_t BytesUntilAligned = (bytesPerCopyWord - DestinationOffsetInWord) % bytesPerCopyWord;
      size_t WordCount;
      size_t WordIndex;
      CopyWord *DestinationWords;
      CopyWord ValueWord = 0;
      size_t ByteIndex;

      for (ByteIndex = 0; ByteIndex < bytesPerCopyWord; ByteIndex++)
         ValueWord = (ValueWord << 8) | ValueByte;

      while (Index < BytesUntilAligned)
      {
         DestinationBytes[Index] = ValueByte;
         Index++;
      }

      DestinationWords = (CopyWord *)(DestinationBytes + Index);
      WordCount = (Count - Index) / bytesPerCopyWord;

      for (WordIndex = 0; WordIndex < WordCount; WordIndex++)
         DestinationWords[WordIndex] = ValueWord;

      Index += WordCount * bytesPerCopyWord;
   }

   while (Index < Count)
   {
      DestinationBytes[Index] = ValueByte;
      Index++;
   }

   return Destination;
}
