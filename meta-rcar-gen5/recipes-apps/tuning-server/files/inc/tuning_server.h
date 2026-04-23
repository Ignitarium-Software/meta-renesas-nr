#ifndef __AWE_SERVER_H__
#define __AWE_SERVER_H__

#include <stdint.h>

#define AWE_PACKET_MAX_WORDS    264u
#define AWE_MAX_PKT_LEN    (AWE_PACKET_MAX_WORDS * 4u) /* 1056 B */

/* maximum rpmessage pkt len that can be transmitted in a single instance.
 * Default maximum pkt length is 512B, in which 16 bytes of data will be occupied
 * by message header.
 */
#define RPMSG_PKT_LEN         (496)

#define PKT_TYPE_AWE_DATA 0
#define PKT_TYPE_AWE_RESP 1
#define PKT_TYPE_OTHER 2

typedef struct __attribute__((packed))
{
	uint32_t type;	   /* type of the packet */
    uint32_t pk_ts;        /* timestamp */
    uint32_t chunk_len;    /* Valid payload size */
    uint16_t chunk_id;     /* Chunk index within frame */
    uint16_t flags;        /* Start/End markers */
} DspBridgeHdr;

#define FLAG_SOF_MSK  0x01   /* Start of frame */
#define FLAG_EOF_MSK  0x02   /* End of frame */

#define RPMSG_DATA_CHUNK_LEN  (RPMSG_PKT_LEN - sizeof(DspBridgeHdr))

/**
 * @brief verify packet CRC
 */
int verify_pkt_crc(uint32_t *buf);

/**
 * @brief Fragment the buffer into RPMSG_CHUNK_SIZE bytes
 * and send it via rpmessage
 */
int send_awe_pkts_fully(int rpmsg_fd, uint8_t *buffer, int len);

/**
 * @brief Aggregate the fragmented rp message chunks and store in a
 *        single buffer.
 */
bool aggregate_awe_pkts(uint8_t *buf, uint32_t len);

/**
 * @brief send data to the host server.
 */
int send_response(int client_fd);

#endif /* __AWE_SERVER_H__  */
