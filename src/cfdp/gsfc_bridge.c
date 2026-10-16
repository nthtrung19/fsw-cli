/* C bridge to the GSFC CFDP library; see gsfc_bridge.h. */

#include "cfdp/gsfc_bridge.h"

#include "cfdp.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void (*m_message) (int level, const char *text) = NULL;

static int m_forward (int level, const char *format, va_list args)
{
    char text[1024];
    int n = vsnprintf (text, sizeof text, format, args);
    size_t len = strlen (text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r'))
    {
        text[--len] = '\0';
    }
    if (m_message != NULL && len > 0)
    {
        m_message (level, text);
    }
    return n;
}

static int m_debug (const char *format, ...)
{
    va_list args;
    va_start (args, format);
    int n = m_forward (MCS_CFDP_DEBUG, format, args);
    va_end (args);
    return n;
}

static int m_info (const char *format, ...)
{
    va_list args;
    va_start (args, format);
    int n = m_forward (MCS_CFDP_INFO, format, args);
    va_end (args);
    return n;
}

static int m_warning (const char *format, ...)
{
    va_list args;
    va_start (args, format);
    int n = m_forward (MCS_CFDP_WARNING, format, args);
    va_end (args);
    return n;
}

static int m_error (const char *format, ...)
{
    va_list args;
    va_start (args, format);
    int n = m_forward (MCS_CFDP_ERROR, format, args);
    va_end (args);
    return n;
}

/* The library asks before starting a transfer whether a link to the partner
 * can be opened; it has no default. mcs's link (the pipeline) is always open. */
static boolean m_output_open (ID my_id, ID partner_id)
{
    (void) my_id;
    (void) partner_id;
    return YES;
}

void mcs_cfdp_register (const mcs_cfdp_callbacks *cb)
{
    m_message = cb->message;
    register_pdu_output_open (m_output_open);
    register_printf_debug (m_debug);
    register_printf_info (m_info);
    register_printf_warning (m_warning);
    register_printf_error (m_error);

    register_pdu_output_ready (cb->output_ready);
    register_pdu_output_send (cb->output_send);
    register_indication (cb->indication);

    register_fopen (cb->open_file);
    register_file_size (cb->file_size);
    register_rename (cb->rename_file);
    register_remove (cb->remove_file);
    register_tmpnam (cb->temp_name);
}

boolean mcs_cfdp_request (const char *request)
{
    return cfdp_give_request (request);
}

boolean mcs_cfdp_give_pdu (const u_int_1 *bytes, u_int_4 length)
{
    CFDP_DATA pdu;
    if (length > MAX_DATA_LENGTH)
    {
        return NO;
    }
    pdu.length = length;
    memcpy (pdu.content, bytes, length);
    return cfdp_give_pdu (pdu);
}

void mcs_cfdp_cycle (void)
{
    cfdp_cycle_each_transaction ();
}

boolean mcs_cfdp_set_mib (const char *param, const char *value)
{
    return cfdp_set_mib_parameter (param, value);
}

boolean mcs_cfdp_progress (TRANSACTION trans, u_int_4 *bytes, u_int_4 *file_size)
{
    return cfdp_transaction_progress (trans, bytes, file_size);
}

void mcs_cfdp_set_trans_seq_num (u_int_4 value)
{
    cfdp_set_trans_seq_num (value);
}

boolean mcs_cfdp_id_from_string (const char *dotted, ID *id)
{
    return cfdp_id_from_string (dotted, id);
}
