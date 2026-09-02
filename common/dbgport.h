/*
 * Diagnostic output over QEMU's debug console on port 0xE9.
 *
 * Copyright (C) 2026 qemu-3dfx-build
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef QEMU_DBGPORT_H
#define QEMU_DBGPORT_H

/* VideoDebugPrint and EngDebugPrint both go to DbgPrint, which needs a kernel debugger
 * on the other end. A byte written to port 0xE9 lands in the file behind QEMU's
 * -debugcon instead, so a driver can talk to us with nothing attached at all.
 *
 * Off unless the build sets QEMU_DEBUGCON; both functions then compile away entirely.
 */

#ifdef QEMU_DEBUGCON

#define QEMU_DEBUGCON_PORT 0xE9

static __inline void DbgPortPutChar(char Character)
{
   __asm__ __volatile__("outb %b0, %w1" : : "a"(Character), "Nd"((unsigned short)QEMU_DEBUGCON_PORT));
}

static __inline void DbgPortString(const char *Text)
{
   while (*Text != '\0')
   {
      DbgPortPutChar(*Text);
      Text++;
   }
}

static __inline void DbgPortHex32(unsigned long Value)
{
   static const char HexDigits[] = "0123456789abcdef";
   int ShiftCount;

   DbgPortString("0x");
   for (ShiftCount = 28; ShiftCount >= 0; ShiftCount -= 4)
   {
      unsigned long Nibble = (Value >> ShiftCount) & 0xf;
      DbgPortPutChar(HexDigits[Nibble]);
   }
}

/* Two shapes cover everything we need: a bare line, and a line ending in one number. */
static __inline void DbgPortLine(const char *Text)
{
   DbgPortString(Text);
   DbgPortPutChar('\n');
}

static __inline void DbgPortLineHex(const char *Text, unsigned long Value)
{
   DbgPortString(Text);
   DbgPortHex32(Value);
   DbgPortPutChar('\n');
}

#else /* !QEMU_DEBUGCON */

#define DbgPortString(Text)           ((void)0)
#define DbgPortHex32(Value)           ((void)0)
#define DbgPortLine(Text)             ((void)0)
#define DbgPortLineHex(Text, Value)   ((void)0)

#endif /* QEMU_DEBUGCON */

#endif /* QEMU_DBGPORT_H */
