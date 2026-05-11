#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/socket.h>
#include "tuning_server.h"

uint8_t rp_packet[RPMSG_PKT_LEN];
uint8_t op_buffer[AWE_MAX_PKT_LEN];

typedef struct {
	uint32_t length;
	uint32_t expected_chunk_id;
	uint8_t in_progress;
} tuning_rx_ctx_t;

tuning_rx_ctx_t rx_ctx;

int verify_pkt_crc(uint32_t *buf)
{
	int num_pkt = buf[0] >> 16;
	uint32_t crc = 0;

	for (int i = 0; i < (num_pkt-1); i++)
	{
		crc ^= buf[i];
	}

	if (crc != buf[num_pkt-1]) {
		printf("Incorrect CRC\n");
		return -1;
	}

	return 0;
}

int send_awe_pkts_fully(int rpmsg_fd, uint8_t *buffer, int len)
{
	int id = 0, ret = 0, offset = 0;

	/* fragment packet into size of RPMSG_DATA_CHUNK_LEN chunks */
	while (offset < len) {

		int chunk_size = (len - offset > RPMSG_DATA_CHUNK_LEN) ? RPMSG_DATA_CHUNK_LEN : (len - offset);

		/* insert rp message packet header */
		DspBridgeHdr rp_hdr;
		rp_hdr.type = PKT_TYPE_AWE_DATA;
		rp_hdr.chunk_id = id++;
		rp_hdr.chunk_len = chunk_size;

		rp_hdr.flags = 0;
		if (offset == 0) {
			rp_hdr.flags |= FLAG_SOF_MSK;
		}
		if (offset + chunk_size >= len) {
			rp_hdr.flags |= FLAG_EOF_MSK;
		}

		memcpy(rp_packet, &rp_hdr, sizeof(DspBridgeHdr));
		memcpy((rp_packet + sizeof(DspBridgeHdr)), (buffer + offset), chunk_size);

#ifdef DEBUG_PRINT
		printf("-------------Chunk size : %d, offset : %d\n", chunk_size, offset);
		for(int i = 0; i < chunk_size; i++) {
			printf("%x ", buffer[i]);
		}
		printf("\n **************************************\n");
#endif
		ret = write(rpmsg_fd, rp_packet, RPMSG_PKT_LEN);
		if (ret < 0) {
			perror("RPMsg write failed");
			break;
		}
		offset += chunk_size;
	}

	return 0;
}

bool aggregate_awe_pkts(uint8_t *buf, uint32_t len)
{
	if (buf == NULL || len == 0U) {
		printf("Invalid parameters\n");
		return false;
	}

	DspBridgeHdr *hdr = (DspBridgeHdr *)buf;
	uint8_t *payload = (uint8_t *)buf + sizeof(DspBridgeHdr);

	if (hdr->type !=  PKT_TYPE_AWE_DATA) {
		printf("Incorrect packet header\n");
		printf("Type : %d, chunk_id = %d, chunk_len = %d, flags = %d\n", hdr->type, hdr->chunk_id, hdr->chunk_len, hdr->flags);
		return false;
	}

	uint8_t is_sof = hdr->flags & FLAG_SOF_MSK;
	uint8_t is_eof = hdr->flags & FLAG_EOF_MSK;

	if (is_sof) {
		/* Start of Frame */
		rx_ctx.length = 0;
		rx_ctx.expected_chunk_id = 0;
		rx_ctx.in_progress = 1;
	}

	/* Ignore if no active packet */
	if (rx_ctx.in_progress != 1) {
		return false;
	}

	/* Order check */
	if (hdr->chunk_id != rx_ctx.expected_chunk_id) {
		printf("ERROR: Out-of-order chunk!\n");
		rx_ctx.in_progress = 0;
		return false;
	}

	/* Bounds check */
	if (rx_ctx.length + hdr->chunk_len > AWE_MAX_PKT_LEN) {
		printf("ERROR: Buffer overflow!\n");
		rx_ctx.in_progress = 0;
		return false;
	}

	/* Copy payload */
	memcpy(&op_buffer[rx_ctx.length], payload, hdr->chunk_len);
	rx_ctx.length += hdr->chunk_len;

	rx_ctx.expected_chunk_id++;

	/* End of Frame */
	if (is_eof) {
		rx_ctx.in_progress = 0;
		return true;
	}

	return false;
}

int send_response(int client_fd)
{
	return send(client_fd, op_buffer, rx_ctx.length, 0);
}
