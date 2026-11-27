/* The disassembler of src/code.cpp: Zydis and Zycore, amalgamated into
   third_party/zydis without their encoder, compiled into the library on x86
   and x64 only. */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define ZYDIS_STATIC_BUILD
#define ZYCORE_STATIC_BUILD
#define ZYDIS_DISABLE_ENCODER
#include <Zydis.c>
#else
/* ISO C wants a translation unit to declare something. */
typedef int uih_no_disassembler;
#endif
