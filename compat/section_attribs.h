/*
 * Replacement for ReactOS' sdk/include/reactos/section_attribs.h.
 *
 * ReactOS builds with MSVC as well, where CODE_SEG() maps to __declspec(code_seg()).
 * We only ever build with GCC, so the section attribute is enough.
 */

#ifndef SECTION_ATTRIBS_H
#define SECTION_ATTRIBS_H

#define CODE_SEG(segment) __attribute__((section(segment)))
#define DATA_SEG(segment) __attribute__((section(segment)))

#endif /* SECTION_ATTRIBS_H */
