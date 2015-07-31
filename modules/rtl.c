/* vim: tabstop=4 shiftwidth=4 noexpandtab
 * This file is part of ToaruOS and is released under the terms
 * of the NCSA / University of Illinois License - see LICENSE.md
 * Copyright (C) 2014 Kevin Lange
 */
#include <module.h>
#include <logging.h>
#include <printf.h>
#include <pci.h>
#include <mem.h>
#include <list.h>
#include <pipe.h>
#include <ipv4.h>
#include <mod/shell.h>
#include <mod/net.h>

/* XXX move this to ipv4? */
extern size_t print_dns_name(fs_node_t * tty, struct dns_packet * dns, size_t offset);

static uint32_t rtl_device_pci = 0x00000000;

static void find_rtl(uint32_t device, uint16_t vendorid, uint16_t deviceid, void * extra) {
	if ((vendorid == 0x10ec) && (deviceid == 0x8139)) {
		*((uint32_t *)extra) = device;
	}
}

#define RTL_PORT_MAC     0x00
#define RTL_PORT_MAR     0x08
#define RTL_PORT_TXSTAT  0x10
#define RTL_PORT_TXBUF   0x20
#define RTL_PORT_RBSTART 0x30
#define RTL_PORT_CMD     0x37
#define RTL_PORT_RXPTR   0x38
#define RTL_PORT_RXADDR  0x3A
#define RTL_PORT_IMR     0x3C
#define RTL_PORT_ISR     0x3E
#define RTL_PORT_TCR     0x40
#define RTL_PORT_RCR     0x44
#define RTL_PORT_RXMISS  0x4C
#define RTL_PORT_CONFIG  0x52

static list_t * net_queue = NULL;
//static volatile uint8_t net_queue_lock = 0;

static spin_lock_t net_queue_lock = { 0 };

static int rtl_irq = 0;
static uint32_t rtl_iobase = 0;
static uint8_t * rtl_rx_buffer;
static uint8_t * rtl_tx_buffer[5];
static uint8_t mac[6];

static uint8_t * last_packet = NULL;

static uintptr_t rtl_rx_phys;
static uintptr_t rtl_tx_phys[5];

static uint32_t cur_rx = 0;
static int dirty_tx = 0;
static int next_tx = 0;

static list_t * rx_wait;

static fs_node_t * irc_socket;

static spin_lock_t irc_tty_lock = { 0 };
//static volatile uint8_t irc_tty_lock = 0;
//static struct netif rtl_netif;

static fs_node_t *_atty = NULL;

static char irc_input[400] = {'\0'};
static char irc_prompt[100] = {'\0'};
static char irc_nick[32] = {'\0'};
static char irc_payload[512];

static char * fgets(char * buf, int size, fs_node_t * stream) {
	char * x = buf;
	int collected = 0;

	while (collected < size) {
		int r = read_fs(stream, 0, 1, (unsigned char *)x);
		collected += r;

		if (r == -1) return NULL;
		if (!r) break;
		if (*x == '\n') break;

		x += r;
	}

	x++;
	*x = '\0';
	return buf;
}

//static volatile uint8_t _lock;
static spin_lock_t _lock;
static int next_tx_buf(void) {
	int out;
	spin_lock(_lock);
	out = next_tx;
	next_tx++;
	if (next_tx == 4) {
		next_tx = 0;
	}
	spin_unlock(_lock);
	return out;
}

static void irc_send(char * payload) {
	// int my_tx = next_tx_buf();
	// int l = strlen(payload);
	// size_t packet_size = write_tcp_packet(rtl_tx_buffer[my_tx], (uint8_t *)payload, l, (TCP_FLAGS_ACK | DATA_OFFSET_5));
	// seq_no += l;

	// outportl(rtl_iobase + RTL_PORT_TXBUF + 4 * my_tx, rtl_tx_phys[my_tx]);
	// outportl(rtl_iobase + RTL_PORT_TXSTAT + 4 * my_tx, packet_size);
}

static void handle_irc_packet(fs_node_t * tty, size_t size, uint8_t * packet) {
	char * c = (char *)packet;

	while ((uintptr_t)c < (uintptr_t)packet + size) {
		char * e = strstr(c, "\r\n");

		if ((uintptr_t)e > (uintptr_t)packet + size) {
			break;
		}
		spin_lock(irc_tty_lock);

		if (!e) {
			/* XXX */
			c[size-1] = '\0';
			fprintf(tty, "\r\033[36m%s\033[0m\033[K\n", c);
			goto prompt_;
		}

		e[0] = '\0';

		if (startswith(c, "PING")) {
			char tmp[100];
			char * t = strstr(c, ":");
			sprintf(tmp, "PONG %s\r\n", t);
			irc_send(tmp);
			goto prompt_;
		}

		char * user;
		char * command;
		char * channel;
		char * message;

		user = c;

		command = strstr(user, " ");
		if (!command) {
			fprintf(tty, "\r\033[36m%s\033[0m\033[K\n", user);
			goto prompt_;
		}
		command[0] = '\0';
		command++;

		channel = strstr(command, " ");
		if (!channel) {
			fprintf(tty, "\r\033[36m%s %s\033[0m\033[K\n", user, command);
			goto prompt_;
		}
		channel[0] = '\0';
		channel++;

		if (!strcmp(command, "PRIVMSG")) {
			message = strstr(channel, " ");
			if (!message) {
				fprintf(tty, "\r\033[36m%s %s %s\033[0m\033[K\n", user, command, channel);
				goto prompt_;
			}
			message[0] = '\0';
			message++;
			if (message[0] == ':') { message++; }
			if (user[0] == ':') { user++; }
			char * t = strstr(user, "!");
			if (t) { t[0] = '\0'; }
			t = strstr(user, "@");
			if (t) { t[0] = '\0'; }
			uint16_t hr, min, sec;
			get_time(&hr, &min, &sec);

			if (startswith(message, "\001ACTION ")) {
				message = message + 8;
				char * x = strstr(message, "\001");
				if (x) *x = '\0';
				fprintf(tty, "\r%2d:%2d:%2d * \033[32m%s\033[0m:\033[34m%s\033[0m %s\033[K\n", hr, min, sec, user, channel, message);
			} else {
				fprintf(tty, "\r%2d:%2d:%2d \033[90m<\033[32m%s\033[0m:\033[34m%s\033[90m>\033[0m %s\033[K\n", hr, min, sec, user, channel, message);
			}
		} else {
			fprintf(tty, "\r\033[36m%s %s %s\033[0m\033[K\n", user, command, channel);
		}

prompt_:
		/* Redraw prompt */
		fprintf(tty, "%s", irc_prompt);
		fprintf(tty, "%s", irc_input);

		spin_unlock(irc_tty_lock);

		if (!e) break;

		c = e + 2;
	}
}

static void rtl_ircd(void * data, char * name) {
	fs_node_t * tty = data;
	char * buf = malloc(4096);

	while (1) {
		char * result = fgets(buf, 4095, irc_socket);
		if (!result) continue;
		size_t len = strlen(buf);
		if (!len) continue;

		handle_irc_packet(tty, len, (unsigned char *)buf);
	}
}

void* rtl_dequeue() {
	while (!net_queue->length) {
		sleep_on(rx_wait);
	}

	spin_lock(net_queue_lock);
	node_t * n = list_dequeue(net_queue);
	void* value = (struct ethernet_packet *)n->value;
	free(n);
	spin_unlock(net_queue_lock);

	return value;
}

void rtl_enqueue(void * buffer) {
	/* XXX size? source? */
	spin_lock(net_queue_lock);
	list_insert(net_queue, buffer);
	spin_unlock(net_queue_lock);
}

uint8_t* rtl_get_mac() {
	return mac;
}

void rtl_send_packet(uint8_t* payload, size_t payload_size) {
	int my_tx = next_tx_buf();
	memcpy(rtl_tx_buffer[my_tx], payload, payload_size);

	outportl(rtl_iobase + RTL_PORT_TXBUF + 4 * my_tx, rtl_tx_phys[my_tx]);
	outportl(rtl_iobase + RTL_PORT_TXSTAT + 4 * my_tx, payload_size);
}

struct ethernet_packet* rtl_get_packet(void) {
	return (struct ethernet_packet*)rtl_dequeue();
}

static int rtl_irq_handler(struct regs *r) {
	uint16_t status = inports(rtl_iobase + RTL_PORT_ISR);
	if (!status) {
		return 0;
	}
	outports(rtl_iobase + RTL_PORT_ISR, status);

	irq_ack(rtl_irq);

	if (status & 0x01 || status & 0x02) {
		/* Receive */
		while((inportb(rtl_iobase + RTL_PORT_CMD) & 0x01) == 0) {
			int offset = cur_rx % 0x2000;

#if 0
			uint16_t buf_addr = inports(rtl_iobase + RTL_PORT_RXADDR);
			uint16_t buf_ptr  = inports(rtl_iobase + RTL_PORT_RXPTR);
			uint8_t  cmd      = inportb(rtl_iobase + RTL_PORT_CMD);
#endif

			uint32_t * buf_start = (uint32_t *)((uintptr_t)rtl_rx_buffer + offset);
			uint32_t rx_status = buf_start[0];
			int rx_size = rx_status >> 16;

			if (rx_status & (0x0020 | 0x0010 | 0x0004 | 0x0002)) {
				debug_print(WARNING, "rx error :(");
			} else {
				uint8_t * buf_8 = (uint8_t *)&(buf_start[1]);

				last_packet = malloc(rx_size);

				uintptr_t packet_end = (uintptr_t)buf_8 + rx_size;
				if (packet_end > (uintptr_t)rtl_rx_buffer + 0x2000) {
					size_t s = ((uintptr_t)rtl_rx_buffer + 0x2000) - (uintptr_t)buf_8;
					memcpy(last_packet, buf_8, s);
					memcpy((void *)((uintptr_t)last_packet + s), rtl_rx_buffer, rx_size - s);
				} else {
					memcpy(last_packet, buf_8, rx_size);
				}

				rtl_enqueue(last_packet);
			}

			cur_rx = (cur_rx + rx_size + 4 + 3) & ~3;
			outports(rtl_iobase + RTL_PORT_RXPTR, cur_rx - 16);
		}
		wakeup_queue(rx_wait);
	}

	if (status & 0x08 || status & 0x04) {
		unsigned int i = inportl(rtl_iobase + RTL_PORT_TXSTAT + 4 * dirty_tx);
		(void)i;
		dirty_tx++;
		if (dirty_tx == 5) dirty_tx = 0;
	}

	return 1;
}

static void rtl_netd(void * data, char * name) {
	fs_node_t * tty = data;

#if 0
	{
		fprintf(tty, "Sending DNS query...\n");
		uint8_t queries[] = {
			3,'i','r','c',
			8,'f','r','e','e','n','o','d','e',
			3,'n','e','t',
			0,
			0x00, 0x01, /* A */
			0x00, 0x01, /* IN */
		};

		int my_tx = next_tx_buf();
		size_t packet_size = write_dns_packet(rtl_tx_buffer[my_tx], sizeof(queries), queries);

		outportl(rtl_iobase + RTL_PORT_TXBUF + 4 * my_tx, rtl_tx_phys[my_tx]);
		outportl(rtl_iobase + RTL_PORT_TXSTAT + 4 * my_tx, packet_size);
	}

	sleep_on(rx_wait);
	parse_dns_response(tty, last_packet);
#endif

#if 0

	{
		fprintf(tty, "Sending DNS query...\n");
		uint8_t queries[] = {
			7,'n','y','a','n','c','a','t',
			5,'d','a','k','k','o',
			2,'u','s',
			0,
			0x00, 0x01, /* A */
			0x00, 0x01, /* IN */
		};

		int my_tx = next_tx_buf();
		size_t packet_size = write_dns_packet(rtl_tx_buffer[my_tx], sizeof(queries), queries);

		outportl(rtl_iobase + RTL_PORT_TXBUF + 4 * my_tx, rtl_tx_phys[my_tx]);
		outportl(rtl_iobase + RTL_PORT_TXSTAT + 4 * my_tx, packet_size);
	}

	sleep_on(rx_wait);
	parse_dns_response(tty, last_packet);
#endif
#if 0
	seq_no = krand();

	{
		fprintf(tty, "Sending TCP syn\n");
		int my_tx = next_tx_buf();
		uint8_t payload[] = { 0 };
		size_t packet_size = write_tcp_packet(rtl_tx_buffer[my_tx], payload, 0, (TCP_FLAGS_SYN | DATA_OFFSET_5));

		outportl(rtl_iobase + RTL_PORT_TXBUF + 4 * my_tx, rtl_tx_phys[my_tx]);
		outportl(rtl_iobase + RTL_PORT_TXSTAT + 4 * my_tx, packet_size);

		seq_no += 1;
		ack_no = 0;
	}

	{
		struct ethernet_packet * eth = net_receive();
		uint16_t eth_type = ntohs(eth->type);

		fprintf(tty, "Ethernet II, Src: (%2x:%2x:%2x:%2x:%2x:%2x), Dst: (%2x:%2x:%2x:%2x:%2x:%2x) [type=%4x)\n",
				eth->source[0], eth->source[1], eth->source[2],
				eth->source[3], eth->source[4], eth->source[5],
				eth->destination[0], eth->destination[1], eth->destination[2],
				eth->destination[3], eth->destination[4], eth->destination[5],
				eth_type);


		struct ipv4_packet * ipv4 = (struct ipv4_packet *)eth->payload;
		uint32_t src_addr = ntohl(ipv4->source);
		uint32_t dst_addr = ntohl(ipv4->destination);
		uint16_t length   = ntohs(ipv4->length);

		char src_ip[16];
		char dst_ip[16];

		ip_ntoa(src_addr, src_ip);
		ip_ntoa(dst_addr, dst_ip);

		fprintf(tty, "IP packet [%s → %s] length=%d bytes\n",
				src_ip, dst_ip, length);

		struct tcp_header * tcp = (struct tcp_header *)ipv4->payload;

		if (seq_no != ntohl(tcp->ack_number)) {
			fprintf(tty, "[eth] Expected ack number of 0x%x, got 0x%x\n",
					seq_no,
					ntohl(tcp->ack_number));
			fprintf(tty, "[eth] Bailing...\n");
			return;
		}

		ack_no = ntohl(tcp->seq_number) + 1;
		free(eth);
	}

	{
		fprintf(tty, "Sending TCP ack\n");
		int my_tx = next_tx_buf();
		uint8_t payload[] = { 0 };
		size_t packet_size = write_tcp_packet(rtl_tx_buffer[my_tx], payload, 0, (TCP_FLAGS_ACK | DATA_OFFSET_5));

		outportl(rtl_iobase + RTL_PORT_TXBUF + 4 * my_tx, rtl_tx_phys[my_tx]);
		outportl(rtl_iobase + RTL_PORT_TXSTAT + 4 * my_tx, packet_size);
	}

	fprintf(tty, "[eth] s-next=0x%x, r-next=0x%x\n", seq_no, ack_no);
#endif
#if 0

	{
		irc_send(
			"GET / HTTP/1.1\r\n"
			"Host: forum.osdev.org\r\n"
			"Cookie: phpbb3_9i66l_u=11616; phpbb3_9i66l_k=ebe8e4f9892d97ab; phpbb3_9i66l_sid=d99d2e26e2a503fdfbe13e9b794dae23\r\n"
			"\r\n");
	}
#endif

	// irc_socket = make_pipe(4096);
	// vfs_mount("/dev/net_irc", irc_socket);

	// create_kernel_tasklet(rtl_ircd, "[ircd]", tty);



	_atty = tty;

	fprintf(tty, "rtl_netd: Initializing netif functions\n");

	init_netif_funcs(rtl_get_mac, rtl_get_packet, rtl_send_packet);
	create_kernel_tasklet(net_handler, "[eth]", tty);

	fprintf(tty, "rtl_netd: net_handler has been started\n");
}

static int tty_readline(fs_node_t * dev, char * linebuf, int max) {
	int read = 0;
	tty_set_unbuffered(dev);
	while (read < max) {
		uint8_t buf[1];
		int r = read_fs(dev, 0, 1, (unsigned char *)buf);
		if (!r) {
			debug_print(WARNING, "Read nothing?");
			continue;
		}
		spin_lock(irc_tty_lock);
		linebuf[read] = buf[0];
		if (buf[0] == '\n') {
			linebuf[read] = 0;
			spin_unlock(irc_tty_lock);
			break;
		} else if (buf[0] == 0x08) {
			if (read > 0) {
				fprintf(dev, "\010 \010");
				read--;
				linebuf[read] = 0;
			}
		} else if (buf[0] < ' ') {
			switch (buf[0]) {
				case 0x0C: /* ^L */
					/* Should reset display here */
					spin_unlock(irc_tty_lock);
					break;
				default:
					/* do nothing */
					spin_unlock(irc_tty_lock);
					break;
			}
		} else {
			fprintf(dev, "%c", buf[0]);
			read += r;
		}
		spin_unlock(irc_tty_lock);
	}
	tty_set_buffered(dev);
	return read;
}

DEFINE_SHELL_FUNCTION(irc_test, "irc test") {
	// char * payloads[] = {
	// 	"NICK toarutest\r\nUSER toaru 0 * :Toaru Test\r\nJOIN #levchins\r\n\0\0\0",
	// 	"PRIVMSG #levchins :99 bottles of beer on the wall\r\n\0\0",
	// 	"PRIVMSG #levchins :99 bottles of beer\r\n\0\0",
	// 	"PRIVMSG #levchins :Take one down\r\n\0\0",
	// 	"PRIVMSG #levchins :pass it around\r\n\0\0",
	// 	"PRIVMSG #levchins :98 bottles of beer on the wall\r\n\0\0",
	// 	"PART #levchins :Thank you, and good night!\r\n\0\0",
	// 	"QUIT\r\n\0\0",
	// };
	// for (unsigned int i = 0; i < sizeof(payloads) / sizeof(uint8_t *); ++i) {
	// 	int my_tx = next_tx_buf();
	// 	int l = strlen(payloads[i]);
	// 	size_t packet_size = write_tcp_packet(rtl_tx_buffer[my_tx], (uint8_t *)payloads[i], l, (TCP_FLAGS_ACK | DATA_OFFSET_5));

	// 	seq_no += l;

	// 	outportl(rtl_iobase + RTL_PORT_TXBUF + 4 * my_tx, rtl_tx_phys[my_tx]);
	// 	outportl(rtl_iobase + RTL_PORT_TXSTAT + 4 * my_tx, packet_size);

	// 	unsigned long s, ss;
	// 	relative_time(0, 500, &s, &ss);
	// 	sleep_until((process_t *)current_process, s, ss);
	// 	switch_task(0);
	// }

	return 0;
}

DEFINE_SHELL_FUNCTION(irc_init, "irc connector") {
	if (argc < 2) {
		fprintf(tty, "Specify a username\n");
		return 1;
	}

	memcpy(irc_nick, argv[1], strlen(argv[1])+1);

	sprintf(irc_payload, "NICK %s\r\nUSER %s * 0 :%s\r\n", irc_nick, irc_nick, irc_nick);

	irc_send(irc_payload);

	return 0;
}

static char * log_channel = NULL;

static uint32_t irc_write(fs_node_t * node, uint32_t offset, uint32_t size, uint8_t *buffer) {
	sprintf(irc_payload, "PRIVMSG %s :%s\r\n", log_channel, buffer);
	irc_send(irc_payload);
	return size;
}

static fs_node_t _irc_log_fnode = {
	.name  = "irc_log",
	.write  = irc_write,
};

DEFINE_SHELL_FUNCTION(irc_log, "spew debug log to irc") {

	if (argc < 2) {
		fprintf(tty, "Need a channel to log to.\n");
		return 1;
	}

	if (!strlen(irc_nick)) {
		fprintf(tty, "Did you run irc_init?\n");
		return 1;
	}

	fprintf(tty, "May the gods have mercy on your soul.\n");

	log_channel = strdup(argv[1]);

	sprintf(irc_payload, "JOIN %s\r\n", log_channel);
	irc_send(irc_payload);

	debug_file = &_irc_log_fnode;
	if (argc > 2) {
		debug_level = atoi(argv[2]);
	}

	return 0;
}

DEFINE_SHELL_FUNCTION(irc_join, "irc channel tool") {

	if (argc < 2) {
		fprintf(tty, "Specify a channel.\n");
		return 1;
	}

	char * channel = argv[1];

	sprintf(irc_payload, "JOIN %s\r\n", channel);
	irc_send(irc_payload);

	sprintf(irc_prompt, "\r[%s] ", channel);

	while (1) {
		fprintf(tty, irc_prompt);
		int c = tty_readline(tty, irc_input, 400);

		spin_lock(irc_tty_lock);

		irc_input[c] = '\0';

		if (startswith(irc_input, "/part")) {
			fprintf(tty, "\n");
			sprintf(irc_payload, "PART %s\r\n", channel);
			irc_send(irc_payload);
			spin_unlock(irc_tty_lock);
			break;
		}

		if (startswith(irc_input, "/me ")) {
			char * m = strstr(irc_input, " ");
			m++;
			uint16_t hr, min, sec;
			get_time(&hr, &min, &sec);
			fprintf(tty, "\r%2d:%2d:%2d * \033[35m%s\033[0m:\033[34m%s\033[0m %s\n\033[K", hr, min, sec, irc_nick, channel, m);
			sprintf(irc_payload, "PRIVMSG %s :\1ACTION %s\1\r\n", channel, m);
			irc_send(irc_payload);
		} else {
			uint16_t hr, min, sec;
			get_time(&hr, &min, &sec);
			fprintf(tty, "\r%2d:%2d:%2d \033[90m<\033[35m%s\033[0m:\033[34m%s\033[90m>\033[0m %s\n\033[K", hr, min, sec, irc_nick, channel, irc_input);
			sprintf(irc_payload, "PRIVMSG %s :%s\r\n", channel, irc_input);
			irc_send(irc_payload);
		}

		memset(irc_input, 0x00, sizeof(irc_input));
		spin_unlock(irc_tty_lock);
	}
	memset(irc_prompt, 0x00, sizeof(irc_prompt));
	memset(irc_input, 0x00, sizeof(irc_input));

	return 0;
}

DEFINE_SHELL_FUNCTION(http, "Open a prompt to send HTTP commands.") {
	char tmp[100];
	char * payload = malloc(10000);

	while (1) {
		fprintf(tty, "http> ");
		tty_readline(tty, tmp, 100);

		if (startswith(tmp, "/quit")) {
			break;
		} else if (startswith(tmp, "get ")) {
			char * m = strstr(tmp, " ");
			m++;

			sprintf(payload,
				"GET %s HTTP/1.1\r\n"
				"Host: %s\r\n"
				"Cookie: phpbb3_9i66l_u=11616; phpbb3_9i66l_k=ebe8e4f9892d97ab; phpbb3_9i66l_sid=d99d2e26e2a503fdfbe13e9b794dae23\r\n"
				"\r\n", m, "forum.osdev.org");

			irc_send(payload);
		} else if (startswith(tmp, "post")) {

			/* /posting.php?mode=post&f=7 */

			char * content =
"-----------------------------2611311029845263341299213952\r\n"
"Content-Disposition: form-data; name=\"subject\"\r\n"
"\r\n"
"test post please ignore\r\n"
"-----------------------------2611311029845263341299213952\r\n"
"Content-Disposition: form-data; name=\"addbbcode20\"\r\n"
"\r\n"
"100\r\n"
"-----------------------------2611311029845263341299213952\r\n"
"Content-Disposition: form-data; name=\"helpbox\"\r\n"
"\r\n"
"Tip: Styles can be applied quickly to selected text.\r\n"
"-----------------------------2611311029845263341299213952\r\n"
"Content-Disposition: form-data; name=\"message\"\r\n"
"\r\n"
"test post please ignore\r\n"
"-----------------------------2611311029845263341299213952\r\n"
"Content-Disposition: form-data; name=\"attach_sig\"\r\n"
"\r\n"
"on\r\n"
"-----------------------------2611311029845263341299213952\r\n"
"Content-Disposition: form-data; name=\"post\"\r\n"
"\r\n"
"Submit\r\n"
"-----------------------------2611311029845263341299213952\r\n"
"Content-Disposition: form-data; name=\"fileupload\"; filename=\"\"\r\n"
"Content-Type: application/octet-stream\r\n"
"\r\n"
"\r\n"
"-----------------------------2611311029845263341299213952\r\n"
"Content-Disposition: form-data; name=\"filecomment\"\r\n"
"\r\n"
"\r\n"
"-----------------------------2611311029845263341299213952\r\n"
"Content-Disposition: form-data; name=\"lastclick\"\r\n"
"\r\n"
"1424062664\r\n"
"-----------------------------2611311029845263341299213952\r\n"
"Content-Disposition: form-data; name=\"creation_time\"\r\n"
"\r\n"
"1424062664\r\n"
"-----------------------------2611311029845263341299213952\r\n"
"Content-Disposition: form-data; name=\"form_token\"\r\n"
"\r\n"
"3fdbc52648cb6f50b72df5bbd5e145bc333cfc0e\r\n"
"-----------------------------2611311029845263341299213952--\r\n";

			sprintf(payload,
				"POST %s HTTP/1.1\r\n"
				"Host: %s\r\n"
				"Cookie: phpbb3_9i66l_u=11616; phpbb3_9i66l_k=ebe8e4f9892d97ab; phpbb3_9i66l_sid=d99d2e26e2a503fdfbe13e9b794dae23\r\n"
				"Referer: http://forum.osdev.org/posting.php?mode=post&f=7\r\n"
				"User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:35.0) Gecko/20100101 Firefox/35.0\r\n"

				"Content-Type: multipart/form-data; boundary=---------------------------2611311029845263341299213952\r\n"
				"Content-Length: %d\r\n"
				"\r\n"
				"%s",
				"/posting.php?mode=post&f=7&sid=d99d2e26e2a503fdfbe13e9b794dae23",
				"forum.osdev.org",
				strlen(content),
				content);

			irc_send(payload);


		}

	}

	return 0;
}

DEFINE_SHELL_FUNCTION(rtl, "rtl8139 experiments") {
	if (rtl_device_pci) {
		fprintf(tty, "Located an RTL 8139: 0x%x\n", rtl_device_pci);

		uint16_t command_reg = pci_read_field(rtl_device_pci, PCI_COMMAND, 4);
		fprintf(tty, "COMMAND register before: 0x%4x\n", command_reg);
		if (command_reg & (1 << 2)) {
			fprintf(tty, "Bus mastering already enabled.\n");
		} else {
			command_reg |= (1 << 2); /* bit 2 */
			fprintf(tty, "COMMAND register after:  0x%4x\n", command_reg);
			pci_write_field(rtl_device_pci, PCI_COMMAND, 4, command_reg);
			command_reg = pci_read_field(rtl_device_pci, PCI_COMMAND, 4);
			fprintf(tty, "COMMAND register after:  0x%4x\n", command_reg);
		}

		rtl_irq = pci_read_field(rtl_device_pci, PCI_INTERRUPT_LINE, 1);
		fprintf(tty, "Interrupt Line: %x\n", rtl_irq);
		irq_install_handler(rtl_irq, rtl_irq_handler);

		uint32_t rtl_bar0 = pci_read_field(rtl_device_pci, PCI_BAR0, 4);
		uint32_t rtl_bar1 = pci_read_field(rtl_device_pci, PCI_BAR1, 4);

		fprintf(tty, "BAR0: 0x%8x\n", rtl_bar0);
		fprintf(tty, "BAR1: 0x%8x\n", rtl_bar1);

		rtl_iobase = 0x00000000;

		if (rtl_bar0 & 0x00000001) {
			rtl_iobase = rtl_bar0 & 0xFFFFFFFC;
		} else {
			fprintf(tty, "This doesn't seem right! RTL8139 should be using an I/O BAR; this looks like a memory bar.");
		}

		fprintf(tty, "RTL iobase: 0x%x\n", rtl_iobase);

		rx_wait = list_create();

		fprintf(tty, "Determining mac address...\n");
		for (int i = 0; i < 6; ++i) {
			mac[i] = inports(rtl_iobase + RTL_PORT_MAC + i);
		}

		fprintf(tty, "%2x:%2x:%2x:%2x:%2x:%2x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

		fprintf(tty, "Enabling RTL8139.\n");
		outportb(rtl_iobase + RTL_PORT_CONFIG, 0x0);

		fprintf(tty, "Resetting RTL8139.\n");
		outportb(rtl_iobase + RTL_PORT_CMD, 0x10);
		while ((inportb(rtl_iobase + 0x37) & 0x10) != 0) { }

		fprintf(tty, "Done resetting RTL8139.\n");

		for (int i = 0; i < 5; ++i) {
			rtl_tx_buffer[i] = (void*)kvmalloc_p(0x1000, &rtl_tx_phys[i]);
			for (int j = 0; j < 60; ++j) {
				rtl_tx_buffer[i][j] = 0xF0;
			}
		}

		rtl_rx_buffer = (uint8_t *)kvmalloc_p(0x3000, &rtl_rx_phys);
		memset(rtl_rx_buffer, 0x00, 0x3000);

		fprintf(tty, "Buffers:\n");
		fprintf(tty, "   rx 0x%x [phys 0x%x and 0x%x and 0x%x]\n", rtl_rx_buffer, rtl_rx_phys, map_to_physical((uintptr_t)rtl_rx_buffer + 0x1000), map_to_physical((uintptr_t)rtl_rx_buffer + 0x2000));

		for (int i = 0; i < 5; ++i) {
			fprintf(tty, "   tx 0x%x [phys 0x%x]\n", rtl_tx_buffer[i], rtl_tx_phys[i]);
		}

		fprintf(tty, "Initializing receive buffer.\n");
		outportl(rtl_iobase + RTL_PORT_RBSTART, rtl_rx_phys);

		fprintf(tty, "Enabling IRQs.\n");
		outports(rtl_iobase + RTL_PORT_IMR,
			0x8000 | /* PCI error */
			0x4000 | /* PCS timeout */
			0x40   | /* Rx FIFO over */
			0x20   | /* Rx underrun */
			0x10   | /* Rx overflow */
			0x08   | /* Tx error */
			0x04   | /* Tx okay */
			0x02   | /* Rx error */
			0x01     /* Rx okay */
		); /* TOK, ROK */

		fprintf(tty, "Configuring transmit\n");
		outportl(rtl_iobase + RTL_PORT_TCR,
			0
		);

		fprintf(tty, "Configuring receive buffer.\n");
		outportl(rtl_iobase + RTL_PORT_RCR,
			(0)       | /* 8K receive */
			0x08      | /* broadcast */
			0x01        /* all physical */
		);

		fprintf(tty, "Enabling receive and transmit.\n");
		outportb(rtl_iobase + RTL_PORT_CMD, 0x08 | 0x04);

		fprintf(tty, "Resetting rx stats\n");
		outportl(rtl_iobase + RTL_PORT_RXMISS, 0);

		net_queue = list_create();

#if 1
		{
			fprintf(tty, "Sending DHCP discover\n");
			size_t packet_size = write_dhcp_packet(rtl_tx_buffer[next_tx]);

			outportl(rtl_iobase + RTL_PORT_TXBUF + 4 * next_tx, rtl_tx_phys[next_tx]);
			outportl(rtl_iobase + RTL_PORT_TXSTAT + 4 * next_tx, packet_size);

			next_tx++;
			if (next_tx == 4) {
				next_tx = 0;
			}
		}

		{
			struct ethernet_packet * eth = (struct ethernet_packet *)rtl_dequeue();
			uint16_t eth_type = ntohs(eth->type);

			fprintf(tty, "Ethernet II, Src: (%2x:%2x:%2x:%2x:%2x:%2x), Dst: (%2x:%2x:%2x:%2x:%2x:%2x) [type=%4x)\n",
					eth->source[0], eth->source[1], eth->source[2],
					eth->source[3], eth->source[4], eth->source[5],
					eth->destination[0], eth->destination[1], eth->destination[2],
					eth->destination[3], eth->destination[4], eth->destination[5],
					eth_type);


			struct ipv4_packet * ipv4 = (struct ipv4_packet *)eth->payload;
			uint32_t src_addr = ntohl(ipv4->source);
			uint32_t dst_addr = ntohl(ipv4->destination);
			uint16_t length   = ntohs(ipv4->length);

			char src_ip[16];
			char dst_ip[16];

			ip_ntoa(src_addr, src_ip);
			ip_ntoa(dst_addr, dst_ip);

			fprintf(tty, "IP packet [%s → %s] length=%d bytes\n",
					src_ip, dst_ip, length);

			struct udp_packet * udp = (struct udp_packet *)ipv4->payload;;
			uint16_t src_port = ntohs(udp->source_port);
			uint16_t dst_port = ntohs(udp->destination_port);
			uint16_t udp_len  = ntohs(udp->length);

			fprintf(tty, "UDP [%d → %d] length=%d bytes\n",
					src_port, dst_port, udp_len);

			struct dhcp_packet * dhcp = (struct dhcp_packet *)udp->payload;
			uint32_t yiaddr = ntohl(dhcp->yiaddr);

			char yiaddr_ip[16];
			ip_ntoa(yiaddr, yiaddr_ip);
			fprintf(tty,  "DHCP Offer: %s\n", yiaddr_ip);

			free(eth);
		}

#endif

		fprintf(tty, "Card is configured, going to start worker thread now.\n");

		create_kernel_tasklet(rtl_netd, "[netd]", tty);
		fprintf(tty, "Back from starting the worker thread.\n");
	} else {
		return -1;
	}
	return 0;
}

static int init(void) {
	BIND_SHELL_FUNCTION(rtl);
	BIND_SHELL_FUNCTION(irc_test);
	BIND_SHELL_FUNCTION(irc_init);
	BIND_SHELL_FUNCTION(irc_join);
	BIND_SHELL_FUNCTION(irc_log);
	BIND_SHELL_FUNCTION(http);

	pci_scan(&find_rtl, -1, &rtl_device_pci);
	if (!rtl_device_pci) {
		debug_print(ERROR, "No RTL 8139 found?");
		return 1;
	}
	return 0;
}

static int fini(void) {
	return 0;
}

MODULE_DEF(rtl, init, fini);
MODULE_DEPENDS(debugshell);
MODULE_DEPENDS(net);
