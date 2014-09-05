#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <osmocom/core/utils.h>
#include <osmocom/gsm/rsl.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <assert.h>

#include "diag_input.h"
#include "process.h"
#include "session.h"

struct diag_packet {
	uint16_t msg_class;
	uint16_t len;
	uint16_t inner_len;
	uint16_t msg_protocol;
	uint64_t timestamp;
	uint8_t msg_type;
	uint8_t msg_subtype;
	uint8_t data_len;
	uint8_t data[0];
} __attribute__ ((packed));

void diag_init()
{
	session_init(0, 1, CALLBACK_SQLITE);
}

void diag_destroy()
{
	session_destroy();
}

inline
uint32_t get_fn(struct diag_packet *dp)
{
	return ((dp->timestamp>>8)/204800)%FN_MAX;
}

inline
void print_common(struct diag_packet *dp, unsigned len)
{
	printf("%u [%02u] ", get_fn(dp), dp->len);
	printf("%04x/%03u/%03u ", dp->msg_protocol, dp->msg_type, dp->msg_subtype);
	printf("[%03u] %s\n", dp->data_len, osmo_hexdump_nospc(dp->data, len-2-sizeof(struct diag_packet)));
}

struct radio_message * handle_3G(struct diag_packet *dp, unsigned len)
{
	unsigned payload_len;
	struct radio_message *m;

	payload_len = dp->len - 16;

	assert(payload_len < sizeof(m->bb.data));

	m = (struct radio_message *) malloc(sizeof(struct radio_message));

	memset(m, 0, sizeof(struct radio_message));

	m->rat = RAT_UMTS;
	m->bb.fn[0] = get_fn(dp);
	switch (dp->msg_type) {
	case 1:
		m->bb.arfcn[0] = ARFCN_UPLINK;
		break;
	case 3:
		m->bb.arfcn[0] = 0;
		break;
	default:
		free(m);
		return 0;
	}

	m->msg_len = payload_len;
	memcpy(m->bb.data, &dp->data[1], payload_len);

	return m;
}

struct radio_message * handle_4G(struct diag_packet *dp, unsigned len)
{
	switch (dp->msg_protocol) {
	case 0xb0c0: // LTE RRC
		if (dp->data[0]) {
			// Uplink
			//(&dp->data[1], dp->len-16);
		} else {
			// Downlink
			//(&dp->data[1], dp->len-16);
		}
		break;
	case 0xb0ec: // LTE NAS EMM DL
		//(&dp->data[1], dp->len-16);
		break;
	case 0xb0ed: // LTE NAS EMM UL
		//(&dp->data[1], dp->len-16);
		break;
	}

	return 0;
}

struct radio_message * new_l2(uint8_t *data, uint8_t len, uint8_t rat, uint8_t domain, uint32_t fn, uint8_t ul, uint8_t flags)
{
	struct radio_message *m;

	assert(data != 0);
	assert(len < sizeof(m->msg));

	m = (struct radio_message *) malloc(sizeof(struct radio_message));

	if (m == 0)
		return 0;

	memset(m, 0, sizeof(struct radio_message));

	m->rat = rat;
	m->domain = domain;
	switch (flags) {
	case MSG_SDCCH:
	case MSG_SACCH:
		m->chan_nr = 0x41;
		break;
	case MSG_FACCH:
		m->chan_nr = 0x08;
		break;
	case MSG_BCCH:
		m->chan_nr = 0x80;
	}
	m->flags = flags | MSG_DECODED;
	m->msg_len = len;
	m->bb.fn[0] = fn;
	m->bb.arfcn[0] = (ul ? ARFCN_UPLINK : 0);
	memcpy(m->msg, data, len);

	return m;
}

struct radio_message * new_l3(uint8_t *data, uint8_t len, uint8_t rat, uint8_t domain, uint32_t fn, uint8_t ul, uint8_t flags)
{
	uint8_t *lapdm;
	unsigned lapdm_len;
	struct radio_message *m;

	assert(data != 0);

	if (len == 0)
		return 0;

	if (flags & MSG_SACCH) {
		lapdm_len = encapsulate_lapdm(data, len, ul, 1, &lapdm);
	} else {
		lapdm_len = encapsulate_lapdm(data, len, ul, 0, &lapdm);
	}

	if (lapdm_len) {
		m = new_l2(lapdm, lapdm_len, rat, domain, fn, ul, flags);
		free(lapdm);
		return m;
	} else {
		return 0;
	}
}

struct radio_message * handle_nas(struct diag_packet *dp, unsigned len)
{
	/* sanity checks */
	if (dp->msg_subtype + sizeof(struct diag_packet) + 2 > len)
		return 0;
	if (!dp->msg_subtype)
		return 0;

	return new_l3(&dp->data[2], dp->msg_subtype, RAT_GSM, DOMAIN_CS, get_fn(dp), dp->msg_type, MSG_SDCCH);
}

struct radio_message * handle_bcch_and_rr(struct diag_packet *dp, unsigned len)
{
	unsigned dtap_len;

	dtap_len = len - 2 - sizeof(struct diag_packet);

	switch (dp->msg_type) {
	case 0: // SDCCH UL RR
		switch (dp->msg_subtype) {
		case 22: // Classmark change
		case 39: // Paging response
		case 41: // Assignment complete
		case 50: // Ciphering mode complete
		case 52: // GPRS susp. request
		case 96: // UTRAN classmark change
			return new_l3(dp->data, dtap_len, RAT_GSM, DOMAIN_CS, get_fn(dp), 1, MSG_SDCCH);
		default:
			print_common(dp, len);
		}
		break;
	case 4: // SACCH UL
		switch (dp->msg_subtype) {
		case 21: // Measurement report
			return new_l3(dp->data, dtap_len, RAT_GSM, DOMAIN_CS, get_fn(dp), 1, MSG_SACCH);
		default:
			print_common(dp, len);
		}
		break;
	case 128: /* SDCCH DL RR */
		return new_l3(dp->data, dtap_len, RAT_GSM, DOMAIN_CS, get_fn(dp), 0, MSG_SDCCH);
	case 129: /* BCCH */
		return new_l2(dp->data, dp->data_len, RAT_GSM, DOMAIN_CS, get_fn(dp), 0, MSG_BCCH);
	case 131: /* CCCH */
		return new_l2(dp->data, dp->data_len, RAT_GSM, DOMAIN_CS, get_fn(dp), 0, MSG_BCCH);
	case 132: /* SACCH DL RR */
		return new_l3(dp->data, dtap_len, RAT_GSM, DOMAIN_CS, get_fn(dp), 0, MSG_SACCH);
	default:
		print_common(dp, len);
	}

	return 0;
}

void handle_diag(uint8_t *msg, unsigned len)
{
	struct diag_packet *dp = (struct diag_packet *) msg;
	struct radio_message *m = 0;

	if (dp->msg_class != 0x0010) {
		//printf("Class %04x is not supported\n", dp->msg_class);
		return;
	}

	switch(dp->msg_protocol) {
	case 0x412f: // 3G RRC
		m = handle_3G(dp, len);
		break;
	case 0x512f: // GSM RR
		m = handle_bcch_and_rr(dp, len);
		break;
	case 0x5230: // GPRS GMM (doubled msg)
		break;
	case 0x713a: // DTAP (2G, 3G)
		m = handle_nas(dp, len);
		break;
	case 0xb0c0: // LTE RRC
	case 0xb0ec: // LTE NAS EMM DL
	case 0xb0ed: // LTE NAS EMM UL
		m = handle_4G(dp, len);
		break;
	default:
		print_common(dp, len);
	}

	if (m) {
		handle_radio_msg(_s, m);
	}
}
