#include "rtsp_server.h"
#include <string.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "esp_system.h"

static const char *TAG = "rtsp";

// SDP for H.264
static const char *sdp_template =
	"v=0\r\n"
	"o=- %u %u IN IP4 %s\r\n"
	"s=Connected Experimental 0.1\r\n"
	"c=IN IP4 0.0.0.0\r\n"
	"t=0 0\r\n"
	"m=video %d RTP/AVP 96\r\n"
	"a=rtpmap:96 H264/90000\r\n"
	"a=fmtp:96 packetization-mode=1;profile-level-id=42001f\r\n"
	"a=control:track0\r\n";

#define MAX_CLIENTS 4
#define RTSP_PORT 8554
#define RTP_PORT 5004
#define RTCP_PORT 5005
#define RTSP_BUF 2048
#define RTP_MTU 1400

typedef struct
{
	int sock;
	int rtp_sock;
	int rtcp_sock;
	rtsp_state_t state;
	uint32_t session;
	uint16_t rtp_seq;
	uint32_t ssrc;
	struct sockaddr_in addr;
	uint16_t rtp_port;
	uint16_t rtcp_port;
	bool active;
} client_t;

static client_t s_clients[MAX_CLIENTS];
static int s_listen_sock = -1;
static TaskHandle_t s_server_task = NULL;
static bool s_running = false;
static uint8_t s_sps[256], s_pps[256];
static size_t s_sps_len, s_pps_len;
static bool s_sps_pps_ready = false;

static const uint8_t *find_nal(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len - 3; i++)
	{
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
			return &data[i];
		if (i < len - 4 && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
			return &data[i];
	}
	return NULL;
}

static esp_err_t send_rtp(client_t *c, const uint8_t *data, size_t len, bool m, uint32_t ts)
{
	if (!c->active || c->state != RTSP_STATE_PLAYING)
		return ESP_OK;

	uint8_t pkt[12 + RTP_MTU];
	pkt[0] = 0x80;
	pkt[1] = 96 | (m ? 0x80 : 0);
	pkt[2] = (c->rtp_seq >> 8) & 0xFF;
	pkt[3] = c->rtp_seq & 0xFF;
	pkt[4] = (ts >> 24) & 0xFF;
	pkt[5] = (ts >> 16) & 0xFF;
	pkt[6] = (ts >> 8) & 0xFF;
	pkt[7] = ts & 0xFF;
	pkt[8] = (c->ssrc >> 24) & 0xFF;
	pkt[9] = (c->ssrc >> 16) & 0xFF;
	pkt[10] = (c->ssrc >> 8) & 0xFF;
	pkt[11] = c->ssrc & 0xFF;
	c->rtp_seq++;

	memcpy(pkt + 12, data, len);

	struct sockaddr_in dest = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = c->addr.sin_addr.s_addr,
		.sin_port = htons(c->rtp_port)};

	int sent = sendto(c->rtp_sock, pkt, len + 12, 0, (struct sockaddr *)&dest, sizeof(dest));
	if (sent < 0)
	{
		ESP_LOGE(TAG, "Failed to send RTP packet: errno %d", errno);
		return ESP_FAIL;
	}
	return ESP_OK;
}

static esp_err_t send_nal(client_t *c, const uint8_t *nal, size_t len, uint32_t ts)
{
	if (len <= RTP_MTU)
	{
		return send_rtp(c, nal, len, true, ts);
	}

	// FU-A fragment
	uint8_t hdr = nal[0];
	uint8_t fu_ind = (hdr & 0xE0) | 28;
	const uint8_t *data = nal + 1;
	size_t rem = len - 1;
	bool first = true;

	while (rem > 0)
	{
		size_t sz = (rem > RTP_MTU - 2) ? (RTP_MTU - 2) : rem;
		uint8_t fu_hdr = (hdr & 0x1F);
		if (first)
		{
			fu_hdr |= 0x80;
			first = false;
		}
		if (sz == rem)
			fu_hdr |= 0x40;

		uint8_t frag[RTP_MTU];
		frag[0] = fu_ind;
		frag[1] = fu_hdr;
		memcpy(frag + 2, data, sz);

		esp_err_t ret = send_rtp(c, frag, sz + 2, (sz == rem), ts);
		if (ret != ESP_OK)
			return ret;

		data += sz;
		rem -= sz;
	}
	return ESP_OK;
}

static int parse_cseq(const char *req)
{
	const char *p = strstr(req, "CSeq:");
	return p ? atoi(p + 5) : 1;
}

static void handle_options(client_t *c, const char *req)
{
	char rsp[256];
	int cseq = parse_cseq(req);
	snprintf(rsp, sizeof(rsp),
			 "RTSP/1.0 200 OK\r\nCSeq: %d\r\nPublic: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n", cseq);
	send(c->sock, rsp, strlen(rsp), 0);
}

static void handle_describe(client_t *c, const char *req)
{
	char ip[16], sdp[512], rsp[1024];
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	getsockname(c->sock, (struct sockaddr *)&addr, &len);
	inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));

	uint32_t sid = esp_random();

	// Build SDP
	snprintf(sdp, sizeof(sdp), sdp_template,
			 (unsigned int)sid, (unsigned int)sid, ip, RTP_PORT);

	int cseq = parse_cseq(req);
	snprintf(rsp, sizeof(rsp),
			 "RTSP/1.0 200 OK\r\nCSeq: %d\r\nContent-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s",
			 cseq, (int)strlen(sdp), sdp);
	send(c->sock, rsp, strlen(rsp), 0);
	ESP_LOGI(TAG, "Sent DESCRIBE response");
}

static void handle_setup(client_t *c, const char *req)
{
	const char *p = strstr(req, "client_port=");
	if (p)
	{
		sscanf(p, "client_port=%hu-%hu", &c->rtp_port, &c->rtcp_port);
	}
	c->session = esp_random();
	c->ssrc = esp_random();
	c->state = RTSP_STATE_READY;

	char rsp[256];
	int cseq = parse_cseq(req);
	snprintf(rsp, sizeof(rsp),
			 "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %08" PRIX32 "\r\nTransport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n\r\n",
			 cseq, c->session, c->rtp_port, c->rtcp_port, RTP_PORT, RTCP_PORT);
	send(c->sock, rsp, strlen(rsp), 0);
}

static void handle_play(client_t *c, const char *req)
{
	c->state = RTSP_STATE_PLAYING;
	c->active = true;

	char rsp[256];
	int cseq = parse_cseq(req);
	snprintf(rsp, sizeof(rsp),
			 "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %08" PRIX32 "\r\nRange: npt=0.000-\r\n\r\n",
			 cseq, c->session);
	send(c->sock, rsp, strlen(rsp), 0);

	// Send SPS/PPS
	if (s_sps_len > 0 && s_pps_len > 0)
	{
		const uint8_t *nal = find_nal(s_sps, s_sps_len);
		if (nal)
		{
			// Skip start code
			size_t skip = (nal[0] == 0 && nal[1] == 0 && nal[2] == 1) ? 3 : ((nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) ? 4 : 0);
			send_nal(c, nal + skip, s_sps_len - (nal - s_sps) - skip, 0);
		}
		nal = find_nal(s_pps, s_pps_len);
		if (nal)
		{
			// Skip start code
			size_t skip = (nal[0] == 0 && nal[1] == 0 && nal[2] == 1) ? 3 : ((nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) ? 4 : 0);
			send_nal(c, nal + skip, s_pps_len - (nal - s_pps) - skip, 0);
		}
	}
}

static void handle_teardown(client_t *c, const char *req)
{
	c->state = RTSP_STATE_TEARDOWN;
	c->active = false;

	char rsp[128];
	int cseq = parse_cseq(req);
	snprintf(rsp, sizeof(rsp), "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %08" PRIX32 "\r\n\r\n", cseq, c->session);
	send(c->sock, rsp, strlen(rsp), 0);
}

static void client_task(void *arg)
{
	client_t *c = (client_t *)arg;
	char buf[RTSP_BUF];

	char ip_str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &c->addr.sin_addr, ip_str, sizeof(ip_str));
	uint16_t port = ntohs(c->addr.sin_port);
	ESP_LOGI(TAG, "New client connected (%s:%d)", ip_str, port);

	while (c->state != RTSP_STATE_TEARDOWN)
	{
		int len = recv(c->sock, buf, sizeof(buf) - 1, 0);
		if (len <= 0)
			break;
		buf[len] = 0;

		if (strstr(buf, "OPTIONS"))
		{
			handle_options(c, buf);
			ESP_LOGI(TAG, "OPTIONS sent (client=%s:%d)", ip_str, port);
		}
		else if (strstr(buf, "DESCRIBE"))
		{
			handle_describe(c, buf);
			ESP_LOGI(TAG, "DESCRIBE sent (client=%s:%d)", ip_str, port);
		}
		else if (strstr(buf, "SETUP"))
		{
			handle_setup(c, buf);
			ESP_LOGI(TAG, "SETUP sent (client=%s:%d, ports=%d-%d)", ip_str, port, c->rtp_port, c->rtcp_port);
		}
		else if (strstr(buf, "PLAY"))
		{
			handle_play(c, buf);
			ESP_LOGI(TAG, "PLAY sent (client=%s:%d)", ip_str, port);
		}
		else if (strstr(buf, "TEARDOWN"))
		{
			handle_teardown(c, buf);
			ESP_LOGI(TAG, "TEARDOWN sent (client=%s:%d)", ip_str, port);
			break;
		}
	}

	ESP_LOGI(TAG, "Client disconnected (%s:%d)", ip_str, port);
	close(c->sock);
	close(c->rtp_sock);
	close(c->rtcp_sock);
	c->active = false;
	vTaskDelete(NULL);
}

static void server_task(void *arg)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(RTSP_PORT)};

	s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	int opt = 1;
	setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	bind(s_listen_sock, (struct sockaddr *)&addr, sizeof(addr));
	listen(s_listen_sock, MAX_CLIENTS);

	ESP_LOGI(TAG, "Listening on port %d", RTSP_PORT);
	s_running = true;

	while (s_running)
	{
		struct sockaddr_in src;
		socklen_t slen = sizeof(src);
		int sock = accept(s_listen_sock, (struct sockaddr *)&src, &slen);
		if (sock < 0)
			continue;

		client_t *c = NULL;
		for (int i = 0; i < MAX_CLIENTS; i++)
		{
			if (!s_clients[i].active)
			{
				c = &s_clients[i];
				break;
			}
		}
		if (!c)
		{
			close(sock);
			continue;
		}

		memset(c, 0, sizeof(*c));
		c->sock = sock;
		c->addr = src;
		c->state = RTSP_STATE_INIT;

		// Create RTP socket
		c->rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		struct sockaddr_in rtp = {
			.sin_family = AF_INET,
			.sin_addr.s_addr = htonl(INADDR_ANY),
			.sin_port = htons(RTP_PORT)};
		bind(c->rtp_sock, (struct sockaddr *)&rtp, sizeof(rtp));

		// Create RTCP socket
		c->rtcp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		struct sockaddr_in rtcp = {
			.sin_family = AF_INET,
			.sin_addr.s_addr = htonl(INADDR_ANY),
			.sin_port = htons(RTCP_PORT)};
		bind(c->rtcp_sock, (struct sockaddr *)&rtcp, sizeof(rtcp));

		xTaskCreate(client_task, "rtsp_client", 8192, c, 5, NULL);
	}

	close(s_listen_sock);
	vTaskDelete(NULL);
}

esp_err_t rtsp_server_init(void)
{
	memset(s_clients, 0, sizeof(s_clients));
	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		s_clients[i].sock = -1;
		s_clients[i].rtp_sock = -1;
		s_clients[i].rtcp_sock = -1;
	}
	return ESP_OK;
}

esp_err_t rtsp_server_start(void)
{
	if (s_server_task)
		return ESP_ERR_INVALID_STATE;
	BaseType_t ret = xTaskCreate(server_task, "rtsp_server", 8192, NULL, 5, &s_server_task);
	return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void rtsp_server_stop(void)
{
	s_running = false;
	if (s_listen_sock >= 0)
		close(s_listen_sock);
	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		if (s_clients[i].sock >= 0)
			close(s_clients[i].sock);
		if (s_clients[i].rtp_sock >= 0)
			close(s_clients[i].rtp_sock);
		if (s_clients[i].rtcp_sock >= 0)
			close(s_clients[i].rtcp_sock);
	}
}

esp_err_t rtsp_send_h264_frame(const uint8_t *data, size_t len, uint32_t ts)
{
	if (!data || len == 0)
		return ESP_ERR_INVALID_ARG;

	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		if (s_clients[i].active && s_clients[i].state == RTSP_STATE_PLAYING)
		{
			const uint8_t *nal = find_nal(data, len);

			while (nal)
			{
				const uint8_t *next = find_nal(nal + 3, len - (nal - data) - 3);
				size_t nal_len = next ? (next - nal) : (len - (nal - data));

				// Skip start code - check 3-byte first, then 4-byte
				const uint8_t *p = nal;
				size_t skip;
				if (p[0] == 0 && p[1] == 0 && p[2] == 1)
				{
					skip = 3; // 3-byte start code
				}
				else if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
				{
					skip = 4; // 4-byte start code
				}
				else
				{
					skip = 0; // No start code (shouldn't happen)
				}

				send_nal(&s_clients[i], p + skip, nal_len - skip, ts);
				nal = next;
			}
		}
	}

	return ESP_OK;
}

esp_err_t rtsp_set_sps_pps(const uint8_t *sps, size_t sps_len, const uint8_t *pps, size_t pps_len)
{
	if (sps && sps_len <= sizeof(s_sps))
	{
		memcpy(s_sps, sps, sps_len);
		s_sps_len = sps_len;
	}
	if (pps && pps_len <= sizeof(s_pps))
	{
		memcpy(s_pps, pps, pps_len);
		s_pps_len = pps_len;
	}
	s_sps_pps_ready = (sps && pps);
	ESP_LOGI(TAG, "SPS/PPS stored: SPS=%d bytes, PPS=%d bytes", s_sps_len, s_pps_len);
	return ESP_OK;
}
