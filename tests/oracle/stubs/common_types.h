/* Minimal stand-in for OSAL common_types.h, enough to compile cFE 6.7 ccsds.c
 * on the host for the oracle test. Not used by mcs itself. */
#ifndef ORACLE_COMMON_TYPES_H
#define ORACLE_COMMON_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;
typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;

#endif
