/* Minimal stand-in for cfe_mission_cfg.h with the settings of the user's mission:
 * CCSDS v1 (MESSAGE_FORMAT_IS_CCSDS_VER_2 undefined). */
#ifndef ORACLE_CFE_MISSION_CFG_H
#define ORACLE_CFE_MISSION_CFG_H

#define CFE_MISSION_SB_TIME_32_16_SUBS 1
#define CFE_MISSION_SB_TIME_32_32_SUBS 2
#define CFE_MISSION_SB_TIME_32_32_M_20 3
#define CFE_MISSION_SB_PACKET_TIME_FORMAT CFE_MISSION_SB_TIME_32_16_SUBS

#define MESSAGE_FORMAT_IS_CCSDS
#undef MESSAGE_FORMAT_IS_CCSDS_VER_2

#endif
