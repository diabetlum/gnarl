#define TAG		"BNEP"
#define LOG_LOCAL_LEVEL	ESP_LOG_INFO
#include <esp_log.h>

#include <btstack_config.h>
#include <btstack.h>
#include <btstack_run_loop_freertos.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <netif/ethernet.h>

struct netif bnep_netif;

static uint16_t bnep_cid;

static QueueHandle_t outgoing_queue;

static struct pbuf *next_packet;	// only modified from btstack context

static void netif_link_up(bd_addr_t network_address) {
	memcpy(bnep_netif.hwaddr, network_address, BD_ADDR_LEN);
	bnep_netif.flags |= NETIF_FLAG_LINK_UP;
	netif_set_up(&bnep_netif);
}

static void netif_link_down(void) {
	bnep_netif.flags &= ~NETIF_FLAG_LINK_UP;
	netif_set_down(&bnep_netif);
}

static void packet_processed(void) {
	pbuf_free_callback(next_packet);
	next_packet = 0;
}

static void handle_outgoing(void *unused) {
	if (next_packet) {
		ESP_LOGD(TAG, "handle_outgoing: previous packet not yet sent");
		return;
	}
	xQueueReceive(outgoing_queue, &next_packet, portMAX_DELAY);
	bnep_request_can_send_now_event(bnep_cid);
}

static void trigger_outgoing_process(void) {
	btstack_run_loop_freertos_execute_code_on_main_thread(handle_outgoing, 0);
}

static void send_next_packet(void) {
	if (!next_packet) {
		ESP_LOGE(TAG, "send_next_packet: no packet queued");
		return;
	}
	static uint8_t buffer[HCI_ACL_PAYLOAD_SIZE];
	uint32_t len = btstack_min(sizeof(buffer), next_packet->tot_len);
	pbuf_copy_partial(next_packet, buffer, len, 0);
	ESP_LOGD(TAG, "send_next_packet: bnep_send %d bytes", len);
	bnep_send(bnep_cid, buffer, len);
	packet_processed();
	if (uxQueueMessagesWaiting(outgoing_queue) != 0) {
		trigger_outgoing_process();
	}
}

static void discard_packets(void) {
	if (next_packet) {
		packet_processed();
	}
	xQueueReset(outgoing_queue);
}

static void receive_packet(const uint8_t *packet, uint16_t size) {
	ESP_LOGD(TAG, "receive_packet: %d bytes", size);
	struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
	if (p == 0) {
		ESP_LOGE(TAG, "receive_packet: pbuf_alloc failed");
		return;
	}
	struct pbuf *q = p;
	while (q && size) {
		memcpy(q->payload, packet, q->len);
		packet += q->len;
		size -= q->len;
		q = q->next;
	}
	if (size != 0) {
		ESP_LOGE(TAG, "receive_packet: %d bytes remaining after copying packet into pbuf", size);
		pbuf_free_callback(p);
		return;
	}
	int r = bnep_netif.input(p, &bnep_netif);
	if (r != ERR_OK) {
		ESP_LOGE(TAG, "receive_packet: IP input error %d", r);
		pbuf_free_callback(p);
	}
}

void handle_bnep_packet(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
	bd_addr_t local_addr;
	switch (packet_type) {
        case HCI_EVENT_PACKET:
		switch (hci_event_packet_get_type(packet)) {
                case BNEP_EVENT_CHANNEL_OPENED:
			if (bnep_event_channel_opened_get_status(packet) != 0)
				break;
			bnep_cid = bnep_event_channel_opened_get_bnep_cid(packet);
			ESP_LOGD(TAG, "BNEP channel opened: CID = %x", bnep_cid);
			gap_local_bd_addr(local_addr);
			netif_link_up(local_addr);
			break;

                case BNEP_EVENT_CHANNEL_CLOSED:
			ESP_LOGD(TAG, "BNEP channel closed");
			bnep_cid = 0;
			discard_packets();
			netif_link_down();
			break;
                case BNEP_EVENT_CAN_SEND_NOW:
			send_next_packet();
			break;
		}
		break;
        case BNEP_DATA_PACKET:
		receive_packet(packet, size);
		break;
	}
}

static err_t link_output(struct netif *netif, struct pbuf *p) {
	ESP_LOGD(TAG, "link_output: length = %d, total = %d", p->len, p->tot_len);
	if (bnep_cid == 0) {
		ESP_LOGD(TAG, "link_output: BNEP CID = 0");
		return ERR_OK;
	}
	pbuf_ref(p);
	int queue_empty = uxQueueMessagesWaiting(outgoing_queue) == 0;
	xQueueSendToBack(outgoing_queue, &p, portMAX_DELAY);
	if (queue_empty) {
		trigger_outgoing_process();
	}
	return ERR_OK;
}

static err_t bnep_netif_init(struct netif *netif) {
	netif->name[0] = 'b';
	netif->name[1] = 't';
	netif->hwaddr_len = BD_ADDR_LEN;
	netif->mtu = 1600;
	netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_UP;
	netif->output = etharp_output;
	netif->linkoutput = link_output;
	return ERR_OK;
}

void bnep_interface_init(void) {
	tcpip_init(0, 0);

	ip4_addr_t ipaddr, netmask, gw;
	IP4_ADDR(&ipaddr, 0U, 0U, 0U, 0U);
	IP4_ADDR(&netmask, 0U, 0U, 0U, 0U);
	IP4_ADDR(&gw, 0U, 0U, 0U, 0U);

	outgoing_queue = xQueueCreate(TCP_SND_QUEUELEN, sizeof(struct pbuf *));
	if (outgoing_queue == 0) {
		ESP_LOGE(TAG, "cannot allocate outgoing queue");
	}

	netif_add(&bnep_netif, &ipaddr, &netmask, &gw, 0, bnep_netif_init, ethernet_input);
	netif_set_default(&bnep_netif);
}