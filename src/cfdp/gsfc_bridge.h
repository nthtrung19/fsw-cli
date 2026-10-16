#ifndef MCS_CFDP_GSFC_BRIDGE_H
#define MCS_CFDP_GSFC_BRIDGE_H

/* C bridge to the GSFC CFDP library (external/cfdp-gsfc).
 *
 * The library's public headers use C++ keywords as parameter names, so C++
 * code includes only this header (and, through it, the C++-safe
 * cfdp_data_structures.h). gsfc_bridge.c includes the real headers, so the C
 * compiler checks every call against the library's own prototypes.
 *
 * The library keeps all of its state in globals and is not thread-safe: call
 * these functions from one thread only (see cfdp/gsfc_engine.hpp).
 */

#include "cfdp_data_structures.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Message levels passed to mcs_cfdp_callbacks.message. */
enum { MCS_CFDP_DEBUG = 0, MCS_CFDP_INFO = 1, MCS_CFDP_WARNING = 2, MCS_CFDP_ERROR = 3 };

/* Everything the library needs from its user. Every member must be set. */
typedef struct
{
    boolean (*output_ready) (PDU_TYPE type, TRANSACTION trans, ID partner);
    void (*output_send) (TRANSACTION trans, ID partner, CFDP_DATA *pdu);
    void (*indication) (INDICATION_TYPE type, TRANS_STATUS status);
    void (*message) (int level, const char *text);          /* one formatted library message */
    CFDP_FILE *(*open_file) (const char *name, const char *mode);
    u_int_4 (*file_size) (const char *name);
    int (*rename_file) (const char *from, const char *to);  /* 0 on success, like rename() */
    int (*remove_file) (const char *name);                  /* 0 on success, like remove() */
    char *(*temp_name) (char *buffer);                      /* buffer holds 1024 chars */
} mcs_cfdp_callbacks;

/* Registers all callbacks (replacing the library's stdio defaults where given). */
void mcs_cfdp_register (const mcs_cfdp_callbacks *callbacks);

/* A User Request in the library's text syntax (cfdp_syntax.h), e.g.
 * "PUT -class1 a.bin 0.24 /cf/a.bin" or "CANCEL 0.24_7". At most 128 chars. */
boolean mcs_cfdp_request (const char *request);

/* One received PDU (CFDP bytes only, no CCSDS header). */
boolean mcs_cfdp_give_pdu (const u_int_1 *bytes, u_int_4 length);

/* Gives every transaction a chance to act (timers, at most one directive and
 * one Filedata PDU per transaction). */
void mcs_cfdp_cycle (void);

boolean mcs_cfdp_set_mib (const char *param, const char *value);

boolean mcs_cfdp_progress (TRANSACTION trans, u_int_4 *bytes, u_int_4 *file_size);

void mcs_cfdp_set_trans_seq_num (u_int_4 value);

/* "0.24" -> ID. */
boolean mcs_cfdp_id_from_string (const char *dotted, ID *id);

#ifdef __cplusplus
}
#endif

#endif
