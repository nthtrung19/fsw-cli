/* FILE: cfdp_config.h -- configuration of the CFDP library, for mcs (ground).
 *
 * This file replaces PUB/cfdp_config.h of CF 2.2.1. The library allows the
 * user to change this file (see its SUMMARY); every other file in src/ and
 * include/ is the unmodified GSFC code.
 *
 * Differences from the CF version:
 *   - No cFS headers (osconfig.h, cf_platform_cfg.h, cf_defs.h): the limits
 *     that came from them are written out below.
 *   - Generic data types have the same SIZES as in the 32-bit (-m32) flight
 *     build, also on a 64-bit host. The CF version uses 'unsigned long' for
 *     u_int_4, which is 8 bytes on x86-64 and breaks the file checksum
 *     (aaa.c reads file data through u_int_4 pointers).
 *   - Limits sized for a ground entity (longer file names, fewer
 *     transactions).
 *   - Message classes off by default; assertions do not abort mcs.
 */

#ifndef H_CFDP_CONFIG
#define H_CFDP_CONFIG 1

#include <stdint.h>
#include <stdio.h>

/**************************/
/*** Generic data types ***/
/**************************/

/* Sizes as in the flight build: u_int_2 is 'unsigned int' there too
 * (4 bytes), u_int_4 and s_int_4 are 4 bytes.
 */
#ifndef boolean
#define  boolean  unsigned char
#endif

#ifndef u_int_1
#define  u_int_1  unsigned char
#endif

#ifndef u_int_2
#define  u_int_2  unsigned int
#endif

#ifndef u_int_4
#define  u_int_4  uint32_t
#endif

#ifndef s_int_1
#define  s_int_1  signed char
#endif

#ifndef s_int_2
#define  s_int_2  signed int
#endif

#ifndef s_int_4
#define  s_int_4  int32_t
#endif

/***********************/
/*** Message Filters ***/
/***********************/

/* Default settings of the message classes ('1' allows, '0' suppresses).
 * Error messages are never filtered. mcs routes all messages to its
 * diagnostics log; they can be enabled at run time with
 * cfdp_enable_message_class().
 */
#define MSG_INDICATIONS 0
#define MSG_DEBUG_MEMORY_USE 0
#define MSG_DEBUG_NAK 0
#define MSG_DEBUG_PDU 0
#define MSG_KERNEL_AUTONOMOUS 0
#define MSG_KERNEL_TRANSACTION 0
#define MSG_NAK_SUMMARY 0
#define MSG_NAK_DETAILS 0
#define MSG_PDU_FILEDATA 0
#define MSG_PDU_NON_FILEDATA 0
#define MSG_PDU_RETRANSMITTED_FD 0
#define MSG_PDU_RETRANSMITTED_MD 0
#define MSG_STATE_ALL 0
#define MSG_STATE_CHANGE 0
#define MSG_TIMER_EXPIRED 0

/********************/
/*** Memory usage ***/
/********************/

/* The maximum number of concurrent transactions (CF: 100). */
#define MAX_CONCURRENT_TRANSACTIONS     32

/* The maximum number of file gaps that can be stored by each transaction
 * (same as CF).
 */
#define MAX_GAPS_PER_TRANSACTION        20

/* The longest file name (CF: OS_MAX_PATH_LEN = 64). Ground paths can be
 * longer; names sent to CF are still limited to 63 characters by mcs.
 */
#define MAX_FILE_NAME_LENGTH            255

/* Entity IDs are 2 bytes (CF accepts only 2-byte IDs). */
#define MAX_ID_LENGTH                   2

/* The largest chunk of file data in one Filedata PDU, incoming or outgoing.
 * CF sends 4000-byte chunks (OutgoingFileChunkSize in its config table).
 */
#define MAX_FILE_CHUNK_SIZE             4096

/* The largest PDU: header (up to 32 bytes) plus the largest chunk. */
#define MAX_PDU_LENGTH                  8192

/* The storage capacity of the generic data structure. */
#define MAX_DATA_LENGTH                 MAX_PDU_LENGTH

/*************************/
/*** Virtual Filestore ***/
/*************************/

#define CFDP_FILE FILE

/* Used only if the default 'temp_file_name' callback is used (mcs
 * registers its own, which writes into the download directory).
 */
#define DEFAULT_TEMP_FILE_NAME_PREFIX   "/tmp/mcs-cfdp"

/* Room for temp file names; must stay below 1024 (aaa.c). */
#define MAX_TEMP_FILE_NAME_LENGTH       511

/*****************************************/
/*** Management Information Base (MIB) ***/
/*****************************************/

/* Defaults, same as CF's library; mcs sets the timers, chunk size and
 * entity ID at run time from the target's "cfdp" block.
 */
#define DEFAULT_ISSUE_EOF_RECV 0
#define DEFAULT_ISSUE_EOF_SENT 1
#define DEFAULT_ISSUE_FILE_SEGMENT_RECV 0
#define DEFAULT_ISSUE_FILE_SEGMENT_SENT 0
#define DEFAULT_ISSUE_RESUMED 1
#define DEFAULT_ISSUE_SUSPENDED 1
#define DEFAULT_ISSUE_TRANSACTION_FINISHED 0
#define DEFAULT_MY_ID "0.23"
#define DEFAULT_RESPONSE_TO_FAULT RESPONSE_CANCEL
#define DEFAULT_SAVE_INCOMPLETE_FILES 0

#define DEFAULT_ACK_LIMIT 2
#define DEFAULT_ACK_TIMEOUT 10
#define DEFAULT_INACTIVITY_TIMEOUT 30
#define DEFAULT_NAK_LIMIT 3
#define DEFAULT_NAK_TIMEOUT 5

#define DEFAULT_OUTGOING_FILE_CHUNK_SIZE    512

/******************/
/*** Assertions ***/
/******************/

/* mcs is an operator tool: an assertion is reported, it never exits. */
#define IS_ASSERT_ALLOWED_TO_EXIT 0

#define ASSERT__(s) \
{ \
   if (!(s)) \
      { \
      msg__ ("Assertion failed in %s at line %u.\n", __func__, __LINE__); \
      if (IS_ASSERT_ALLOWED_TO_EXIT) \
         abort (); \
      } \
}

#define ASSERT__BUG_DETECTED \
{ \
   msg__ ("Bug detected in %s at line %u.\n", __func__, __LINE__); \
   if (IS_ASSERT_ALLOWED_TO_EXIT) \
      abort (); \
} \

#endif
