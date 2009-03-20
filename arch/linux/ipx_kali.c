/* IPX driver for KaliNix interface */
#include <stdio.h>
#include <string.h>
#include <netinet/in.h> /* for htons & co. */
#include "ipxdrv.h"
#include "ukali.h"
#include "console.h"

static int open_sockets = 0;
static int dynamic_socket = 0x401;

int have_empty_address() {
	int i;
	for (i = 0; i < 10 && !MyAddress[i]; i++) ;
	return i == 10;
}

int kali_get_my_address(void)
{

	kaliaddr_ipx mKaliAddr;

	if (!have_empty_address())
		return 0;

	if (KaliGetNodeNum(&mKaliAddr) < 0)
		return -1;

	memset(MyAddress, 0, 4);
	memcpy(MyAddress + 4, mKaliAddr.sa_nodenum, sizeof(mKaliAddr.sa_nodenum));

	return 0;
}

int kali_open_socket(socket_t *sk, int port)
{
	con_printf(CON_DEBUG,"kali: OpenSocket on port(%d)\n", port);

	if (!open_sockets) {
		if (have_empty_address()) {
			if (kali_get_my_address() < 0) {
				con_printf(CON_CRITICAL,"kali: Error communicating with KaliNix\n");
				return -1;
			}
		}
	}
	if (!port)
		port = dynamic_socket++;

	if ((sk->fd = KaliOpenSocket(htons(port))) < 0) {
		con_printf(CON_CRITICAL,"kali: OpenSocket Failed on port(%d)\n", port);
		sk->fd = -1;
		return -1;
	}
	open_sockets++;
	sk->socket = port;
	return 0;
}

void kali_close_socket(socket_t *mysock)
{
	if (!open_sockets) {
		con_printf(CON_CRITICAL,"kali: close w/o open\n");
		return;
	}
	con_printf(CON_DEBUG,"kali: CloseSocket on port(%d)\n", mysock->socket);
	KaliCloseSocket(mysock->fd);
	if (--open_sockets) {
		con_printf(CON_URGENT,"kali: (closesocket) %d sockets left\n", open_sockets);
		return;
	}
}

int kali_send_packet(socket_t *mysock, IPXPacket_t *IPXHeader, u_char *data, int dataLen)
{
	kaliaddr_ipx toaddr;
	int i;
 
	memcpy(toaddr.sa_nodenum, IPXHeader->Destination.Node, sizeof(toaddr.sa_nodenum));
	toaddr.sa_socket=htons(mysock->socket);

	if ((i = KaliSendPacket(mysock->fd, (char *)data, dataLen, &toaddr)) < 0)
		return -1;

	return i;
}

int kali_receive_packet(socket_t *s, char *outbuf, int outbufsize, struct recv_data *rd)
{
	int size;
	kaliaddr_ipx fromaddr;

	if ((size = KaliReceivePacket(s->fd, outbuf, outbufsize, &fromaddr)) < 0)
		return -1;

	rd->dst_socket = s->socket;
	rd->src_socket = ntohs(fromaddr.sa_socket);
	memcpy(rd->src_node, fromaddr.sa_nodenum, sizeof(fromaddr.sa_nodenum));
	memset(rd->src_network, 0, 4);
	rd->pkt_type = 0;

	return size;
}

static int kali_general_packet_ready(socket_t *s)
{
	return ipxdrv_general_packet_ready(s->fd);
}

struct net_driver ipxdrv_kali = {
	kali_open_socket,
	kali_close_socket,
	kali_send_packet,
	kali_receive_packet,
	kali_general_packet_ready,
	1,
	NETPROTO_KALINIX
};
