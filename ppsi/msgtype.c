#include <ppsi/ppsi.h>

/*
 * PP_NP_GEN/EVT is the event or general message. It selects the socket etc
 * PP_P2P_MECH is used to select a destination address for pdelay frames.
 * the numeric 0..5 is the "controlField" (magic ptpV1 numbers in byte 32).
 * PP_LOG is the kind of logInterval to put in byte 33.
 */

struct pp_msgtype_info pp_msgtype_info[16] = {
	[PPM_SYNC] = {
		"sync", PP_SYNC_LENGTH,
		PP_NP_EVT, PP_E2E_MECH,  0, PP_LOG_SYNC },
	[PPM_DELAY_REQ] = {
		"delay_req", PP_DELAY_REQ_LENGTH,
		PP_NP_EVT, PP_E2E_MECH, 1, 0x7f },
	[PPM_PDELAY_REQ] = {
		"pdelay_req", PP_PDELAY_REQ_LENGTH,
		PP_NP_EVT, PP_P2P_MECH, 5, 0x7f },
	[PPM_PDELAY_RESP] = {
		"pdelay_resp", PP_PDELAY_RESP_LENGTH,
		PP_NP_EVT, PP_P2P_MECH, 5, 0x7f },
	[PPM_FOLLOW_UP] = {
		"follow_up", PP_FOLLOW_UP_LENGTH,
		PP_NP_GEN, PP_E2E_MECH, 2, PP_LOG_SYNC },
	[PPM_DELAY_RESP] = {
		"delay_resp", PP_DELAY_RESP_LENGTH,
		PP_NP_GEN, PP_E2E_MECH, 3, PP_LOG_REQUEST },
	[PPM_PDELAY_R_FUP] = {
		"pdelay_resp_follow_up",  PP_PDELAY_R_FUP_LENGTH,
		PP_NP_GEN, PP_P2P_MECH, 5, 0x7f },
	[PPM_ANNOUNCE] = {
		"announce", PP_ANNOUNCE_LENGTH,
		PP_NP_GEN, PP_E2E_MECH, 5, PP_LOG_ANNOUNCE},
/* We don't use signaling and management, or not in the table-driven code */
	[PPM_SIGNALING] = { "signaling", -1,  PP_NP_GEN, PP_E2E_MECH, 5, 0x7f},
	[PPM_MANAGEMENT] = { "management", -1, PP_NP_GEN, PP_E2E_MECH, 4, 0x7f},
};
