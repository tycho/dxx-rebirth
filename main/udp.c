// UDP/IP driver
// Taken inspiration from bomberclone - Thanks Steffen!

#define UDP_LEN_HOSTNAME 128
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#ifdef __WINDOWS__
#include <winsock.h>
#include <io.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#endif

#include "error.h"
#include "ipx_drv.h"
#include "network.h"
#include "timer.h"
#include "udp.h"
#include "key.h"
#include "text.h"

int udpsock;
unsigned int myid=0; // My personal ID which I will get from host and will be used for IPX-Node
struct peer_list UDPPeers[MAX_CONNECTIONS]; // The Peer list.

// Receive Configuration: Exchanging Peers, doing Handshakes, etc.
void ipx_udp_ReceiveCFG(char *text, struct _sockaddr *sAddr)
{
	switch (text[4])
	{
		case CFG_FIRSTCONTACT_REQ:
		{
			int i, clientid=0;

			// Check if sAddr is not used already (existing client or if client got this packet)
			for (i = 1; i < MAX_CONNECTIONS; i++)
			{
				if (!memcmp(sAddr,(struct _sockaddr *)&UDPPeers[i].addr,sizeof(struct _sockaddr)))
				{
					clientid=i;
				}
			}

			// If we haven't found a place already...
			if (!clientid)
			{
				for (i = 1; i < MAX_CONNECTIONS; i++) // ... find a free place in list...
				{
					if (!UDPPeers[i].valid) // ... Found it!
					{
						clientid=i;
						break;
					}
				}
			}

			if (!clientid)
				return;

			ubyte outbuf[6];
			UDPPeers[clientid].valid=1;
			UDPPeers[clientid].timestamp=timer_get_fixed_seconds();
			memset(UDPPeers[clientid].hs_list,0,MAX_CONNECTIONS);
			UDPPeers[clientid].hstimeout=0;
			UDPPeers[clientid].relay=0;
			memcpy(&UDPPeers[clientid].addr,sAddr,sizeof(struct _sockaddr)); // Copy address of new client to Peer list
			memcpy(outbuf+0,DXXcfgid,4); // PacketCFG ID
			outbuf[4]=CFG_FIRSTCONTACT_ACK; // CFG Type
			outbuf[5]=clientid; // personal ID for the client
			sendto (udpsock, outbuf, sizeof(outbuf), 0, (struct sockaddr *) sAddr, sizeof(struct _sockaddr)); // Send!
			return;
		}

		case CFG_FIRSTCONTACT_ACK:
		{
			My_Seq.player.node[0] = ipx_MyAddress[4] = myid = text[5]; // got my ID
			memcpy(&UDPPeers[0].addr,sAddr,sizeof(struct _sockaddr)); // Add sender -> host!
			UDPPeers[0].valid=1;
			UDPPeers[0].timestamp=timer_get_fixed_seconds();
			return;
		}

		// got signal from host to connect with a new client
		case CFG_HANDSHAKE_INIT:
		{
			int i;
			struct _sockaddr tmpAddr;
			ubyte outbuf[6];

			memcpy(outbuf+0,DXXcfgid,4); // PacketCFG ID
			outbuf[4]=CFG_HANDSHAKE_PING; // CFG Type
			outbuf[5]=myid; // my ID that will be assigned to the new client
			memcpy(&tmpAddr,text+5,sizeof(struct _sockaddr));
			for (i=0; i<3; i++)
				sendto (udpsock, outbuf, sizeof(outbuf), 0, (struct sockaddr *) &tmpAddr, sizeof(struct _sockaddr)); // send!
			return;
		}

		// Got a Handshake Ping from a connected client -> Add it to peer list and Pong back
		case CFG_HANDSHAKE_PING:
		{
			int i;
			ubyte outbuf[6];

			memcpy(&UDPPeers[(int)text[5]].addr,sAddr,sizeof(struct _sockaddr)); // Copy address of new client to Peer list
			UDPPeers[(int)text[5]].valid=1;
			UDPPeers[(int)text[5]].timestamp=timer_get_fixed_seconds();
			memcpy(outbuf+0,DXXcfgid,4); // PacketCFG ID
			outbuf[4]=CFG_HANDSHAKE_PONG; // CFG Type
			outbuf[5]=myid; // my ID
			for (i=0; i<3; i++)
				sendto (udpsock, outbuf, sizeof(outbuf), 0, (struct sockaddr *) sAddr, sizeof(struct _sockaddr)); // send!
			return;
		}

		// Got response from new client -> Send this to host and add new client to peer list
		case CFG_HANDSHAKE_PONG:
		{
			int i;
			ubyte outbuf[7];

			memcpy(&UDPPeers[(int)text[5]].addr,sAddr,sizeof(struct _sockaddr)); // Copy address of new client to Peer list
			UDPPeers[(int)text[5]].valid=1;
			UDPPeers[(int)text[5]].timestamp=timer_get_fixed_seconds();
			memcpy(outbuf+0,DXXcfgid,4); // PacketCFG ID
			outbuf[4]=CFG_HANDSHAKE_ACK; // CFG Type
			outbuf[5]=myid; // my ID
			outbuf[6]=text[5]; // ID of the added client
			for (i=0; i<3; i++)
				sendto (udpsock, outbuf, sizeof(outbuf), 0, (struct sockaddr *) &UDPPeers[0].addr, sizeof(struct _sockaddr)); // send!
			return;
		}

		// Got a message from a client about a new client -> Add hs_list info!
		case CFG_HANDSHAKE_ACK:
		{
			UDPPeers[(int)text[6]].hs_list[(int)text[5]]=1;
			return;
		}
	}
	return;
}

// Handshaking between clients
// Here the HOST will motivate existing clients to connect to a new one
// If all went good, client can join, if not host will relay this client if !GameArg.MplIpNoRelay or being dumped
int ipx_udp_HandshakeFrame(struct _sockaddr *sAddr, char *inbuf)
{
	int i,checkid=-1;

	if (Network_send_objects)
		return 0;//if we are currently letting someone else join, we don't know if this person can join ok.

	// Find the player we want to Handshake
	for (i=1; i<MAX_CONNECTIONS; i++)
	{
		if (!memcmp(sAddr,(struct sockaddr*)&UDPPeers[i].addr,sizeof(struct _sockaddr)))
		{
			checkid=i;
			UDPPeers[checkid].valid=2;
		}
	}

	if (checkid<0)
		return 0;

	if (UDPPeers[checkid].relay)
		return 1;

	// send Handshake init to all valid players except the new one (checkid)
	for (i=1; i<MAX_CONNECTIONS; i++)
	{
		if (UDPPeers[i].valid>1 && i != checkid  && !UDPPeers[i].relay && UDPPeers[i].hs_list[0] && memcmp(sAddr,(struct sockaddr*)&UDPPeers[i].addr,sizeof(struct _sockaddr)))
		{
			char outbuf[6+sizeof(struct _sockaddr)];

			// send request to connected clients to handshake with new client
			memcpy(outbuf+0,DXXcfgid,4);
			outbuf[4]=CFG_HANDSHAKE_INIT;
			memcpy(outbuf+5,sAddr,sizeof(struct _sockaddr));
			sendto (udpsock, outbuf, sizeof(outbuf), 0, (struct sockaddr *) &UDPPeers[i].addr, sizeof(struct _sockaddr));
		}
	}

	// Now check if Handshake was successful on requesting player - if not, return 0
	for (i=1; i<MAX_CONNECTIONS; i++)
	{
		if (UDPPeers[i].valid>1 && memcmp(sAddr,(struct _sockaddr *)&UDPPeers[i].addr,sizeof(struct _sockaddr)))
		{
			if (UDPPeers[checkid].hs_list[i] != 1 && !UDPPeers[i].relay)
			{
 				if (UDPPeers[checkid].hstimeout > 10)
 				{
					if (!GameArg.MplIpNoRelay)
					{
						printf("Relaying Client #%i\n",checkid);
						UDPPeers[checkid].relay=1;
						memset(UDPPeers[checkid].hs_list,1,MAX_CONNECTIONS);
						return 1;
					}
 				}
 				UDPPeers[checkid].hstimeout++;
				return 0;
			}
		}
	}

	// Set all vals to true since this could be the first client in our list that had no need to Handshake.
	// However in that case this should be true for the next client joning
	memset(UDPPeers[checkid].hs_list,1,MAX_CONNECTIONS);

	return 1;
}

// Check if we got got Disconnect signal by player. If Yes, remove it. 
// Check if we got data from sAddr within the last 10 seconds (NETWORK_TIMEOUT). If yes, update timestamp of peer, otherwise remove it.
void ipx_udp_CheckDisconnect(struct _sockaddr *sAddr, char *text)
{
	int i;

	// Check all connections for Disconnect or Timeout.
	for (i = 0; i < MAX_CONNECTIONS; i++)
	{
		// Find the player we got the packet from
		if (!memcmp(sAddr,(struct _sockaddr *)&UDPPeers[i].addr,sizeof(struct _sockaddr)))
		{
			// Player has left -> PID ENDLEVEL and !connected-flag
			if (text[0]==PID_ENDLEVEL && text[2] == CONNECT_DISCONNECTED)
			{
				UDPPeers[i].valid=0;
				UDPPeers[i].timestamp=0;
				memset(UDPPeers[i].hs_list,0,MAX_CONNECTIONS);
				UDPPeers[i].hstimeout=0;
				UDPPeers[i].relay=0;
				if (i==0) // Host has left - Quit game!
				{
					Function_mode = FMODE_MENU;
					nm_messagebox(NULL, 1, TXT_OK, "Game was closed by host!");
					multi_quit_game = 1;
					multi_leave_menu = 1;
					multi_reset_stuff();
					longjmp(LeaveGame,1);  // because the other crap didn't work right
					return;
				}
			}
			else
			{
				// Update timestamp
				UDPPeers[i].timestamp=timer_get_fixed_seconds();
			}
		}
		else if (UDPPeers[i].valid && UDPPeers[i].timestamp + NETWORK_TIMEOUT <= timer_get_fixed_seconds())
		{
			// Timeout!
			UDPPeers[i].valid=0;
			UDPPeers[i].timestamp=0;
			memset(UDPPeers[i].hs_list,0,MAX_CONNECTIONS);
			UDPPeers[i].hstimeout=0;
			UDPPeers[i].relay=0;
			if (i==0) // Host has left - Quit game!
			{
				Function_mode = FMODE_MENU;
				nm_messagebox(NULL, 1, TXT_OK, "Game was closed by host!");
				multi_quit_game = 1;
				multi_leave_menu = 1;
				multi_reset_stuff();
				longjmp(LeaveGame,1);  // because the other crap didn't work right
				return;
			}
		}
	}
}

// Relay packets over Host
void ipx_udp_PacketRelay(char *text, int len, struct _sockaddr *sAddr)
{
	int i, relayid=0;
	
	// Only host will relay
	if (myid)
		return;

	// Relay PDATA packets only
	if (text[0] != PID_PDATA && text[0] != PID_PDATA_SHORT2)
		return;

	// Check if sender is a relay client and store ID if so
	for (i = 1; i < MAX_CONNECTIONS; i++)
	{
		if (!memcmp(sAddr,(struct _sockaddr *)&UDPPeers[i].addr,sizeof(struct _sockaddr)) && UDPPeers[i].relay)
		{
			relayid=i;
		}
	}

	if (relayid>0) // Relay packets from relay client to all others
	{
		for (i = 1; i < MAX_CONNECTIONS; i++)
		{
			if (memcmp(sAddr,(struct _sockaddr *)&UDPPeers[i].addr,sizeof(struct _sockaddr)) && i != relayid && UDPPeers[i].valid>1)
			{
				sendto (udpsock, text, len, 0, (struct sockaddr *) &UDPPeers[i].addr, sizeof(struct _sockaddr));
			}
		}
	}
	else // Relay packets from normal client to relay clients
	{
		for (i = 1; i < MAX_CONNECTIONS; i++)
		{
			if (memcmp(sAddr,(struct _sockaddr *)&UDPPeers[i].addr,sizeof(struct _sockaddr)) && UDPPeers[i].relay)
			{
				sendto (udpsock, text, len, 0, (struct sockaddr *) &UDPPeers[i].addr, sizeof(struct _sockaddr));
			}
		}
	}
}

// Resolve address
int ipx_udp_DnsFillAddr(char *host, int hostlen, int port, int portlen, struct _sockaddr *sAddr)
{
	struct hostent *he;

#ifdef IPv6
	int socktype=AF_INET6;

	he = gethostbyname2 (host,socktype);

	if (!he)
	{
		socktype=AF_INET;
		he = gethostbyname2 (host,socktype);
	}

	if (!he)
	{
		printf ("ipx_udp_DnsFillAddr (gethostbyname) failed\n");
		nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not resolve address");
		return -1;
	}

	((struct _sockaddr *) sAddr)->sin6_family = socktype; // host byte order
	((struct _sockaddr *) sAddr)->sin6_port = htons (port); // short, network byte order
	((struct _sockaddr *) sAddr)->sin6_flowinfo = 0;
	((struct _sockaddr *) sAddr)->sin6_addr = *((struct in6_addr *) he->h_addr);
	((struct _sockaddr *) sAddr)->sin6_scope_id = 0;
#else
	if ((he = gethostbyname (host)) == NULL) // get the host info
	{
		printf ("ipx_udp_DnsFillAddr (gethostbyname) failed\n");
		nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not resolve address");
		return -1;
	}

	((struct _sockaddr *) sAddr)->sin_family = _af; // host byte order
	((struct _sockaddr *) sAddr)->sin_port = htons (port); // short, network byte order
	((struct _sockaddr *) sAddr)->sin_addr = *((struct in_addr *) he->h_addr);
	memset (&(((struct _sockaddr *) sAddr)->sin_zero), '\0', 8); // zero the rest of the struct
#endif

	return 0;
}

// Connect to a game host - we want to play!
int ipx_udp_ConnectManual(char *textaddr)
{
	struct _sockaddr HostAddr;
	fix start_time = 0;
	ubyte node[6];
	char outbuf[12], inbuf[5];

	// Resolve address
	if (ipx_udp_DnsFillAddr(textaddr, LEN_SERVERNAME, UDP_BASEPORT, LEN_PORT, &HostAddr) < 0)
	{
		return -1;
	}

	network_init();
	N_players = 0;
	memset(node,0,6); // Host is always on ID 0
	memset(inbuf,0,5);
	memset(outbuf,0,12);
	start_time = timer_get_fixed_seconds();

	while (!(!memcmp(inbuf+0,DXXcfgid,4) && inbuf[4] == CFG_FIRSTCONTACT_ACK)) // Yay! got ACK by this host!
	{
		// Cancel this with ESC
		if (key_inkey()==KEY_ESC)
			return 0;

		// Timeout after 10 seconds
		if (timer_get_fixed_seconds() >= start_time + (F1_0*10) || timer_get_fixed_seconds() < start_time)
		{
			nm_messagebox(TXT_ERROR,1,TXT_OK,"No response by host");
			return -1;
		}

		// Send request to get added by host...
		memcpy(outbuf+0,DXXcfgid,4);
		outbuf[4]=CFG_FIRSTCONTACT_REQ;
		sendto (udpsock, outbuf, sizeof(outbuf), 0, (struct sockaddr *) &HostAddr, sizeof(struct _sockaddr));
		timer_delay2(10);
		// ... and wait for answer
		ipx_udp_ReceivePacket(inbuf,6,NULL);
	}

	
	if (get_and_show_netgame_info(null_addr,node,NULL)) //  show netgame info and keep connection alive!
		return network_do_join_game(&Active_games[0]); // join the game actually
	else
		return 0;
}

// Open socket
int ipx_udp_OpenSocket(int port)
{
	int i;

#ifdef __WINDOWS__
	struct _sockaddr sAddr;   // my address information

	if ((udpsock = socket (_af, SOCK_DGRAM, 0)) == -1) {
		printf ("ipx_udp_OpenSocket: socket creation failed\n");
		nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not create socket");
		return -1;
	}

#ifdef IPv6
	sAddr.sin6_family = _pf; // host byte order
	sAddr.sin6_port = htons (GameArg.MplIpBasePort+UDP_BASEPORT);; // short, network byte order
	sAddr.sin6_flowinfo = 0;
	sAddr.sin6_addr = in6addr_any; // automatically fill with my IP
	sAddr.sin6_scope_id = 0;
#else
	sAddr.sin_family = _pf; // host byte order
	sAddr.sin_port = htons (GameArg.MplIpBasePort+UDP_BASEPORT); // short, network byte order
	sAddr.sin_addr.s_addr = INADDR_ANY; // automatically fill with my IP
#endif
	
	memset (&(sAddr.sin_zero), '\0', 8); // zero the rest of the struct
	
	if (bind (udpsock, (struct sockaddr *) &sAddr, sizeof (struct sockaddr)) == -1) {
		printf ("ipx_udp_OpenSocket: bind name to socket failed\n");
		nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not bind name to socket");
		return -1;
	}
#else
	struct addrinfo hints,*res,*sres;
	int err,ai_family_;
	char cport[5];
	
	memset (&hints, '\0', sizeof (struct addrinfo));
	
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = _pf;
	hints.ai_socktype = SOCK_DGRAM;
	
	ai_family_ = 0;

	sprintf(cport,"%i",UDP_BASEPORT+GameArg.MplIpBasePort);

	if ((err = getaddrinfo (NULL, cport, &hints, &res)) == 0)
	{
		sres = res;
		while ((ai_family_ == 0) && (sres))
		{
			if (sres->ai_family == _pf || _pf == PF_UNSPEC)
				ai_family_ = sres->ai_family;
			else
				sres = sres->ai_next;
		}
	
		if (sres == NULL)
			sres = res;
	
		ai_family_ = sres->ai_family;
		if (ai_family_ != _pf && _pf != PF_UNSPEC)
		{
			// ai_family is not identic
			freeaddrinfo (res);
			return -1;
		}
	
		if ((udpsock = socket (sres->ai_family, SOCK_DGRAM, 0)) < 0)
		{
			printf ("ipx_udp_OpenSocket: socket creation failed\n");
			nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not create socket");
			freeaddrinfo (res);
			return -1;
		}
	
		if ((err = bind (udpsock, sres->ai_addr, sres->ai_addrlen)) < 0)
		{
			printf ("ipx_udp_OpenSocket: bind name to socket failed\n");
			nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not bind name to socket");
			close (udpsock);
			freeaddrinfo (res);
			return -1;
		}
	
		freeaddrinfo (res);
	}
	else {
		udpsock = -1;
		printf ("ipx_udp_OpenSocket (getaddrinfo):%s\n", gai_strerror (err));
		nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not get address information:\n%s",gai_strerror (err));
	}
#endif

	// Prepare UDPPeers
	for (i=0; i<MAX_CONNECTIONS;i++)
	{
		memset(&UDPPeers[i].addr,0,sizeof(struct _sockaddr));
		UDPPeers[i].valid=0;
		UDPPeers[i].timestamp=0;
		memset(UDPPeers[i].hs_list,0,MAX_CONNECTIONS);
		UDPPeers[i].hstimeout=0;
		UDPPeers[i].relay=0;
	}
	myid=0;

	return 0;
};


// Closes an existing udp socket
void ipx_udp_CloseSocket(void)
{
	int i;

	if (udpsock != -1)
	{
#ifdef __WINDOWS__
		closesocket(udpsock);
		WSACleanup();
#else
		close (udpsock);
#endif
	}
	udpsock = -1;

	// Prepare UDPPeers
	for (i=0; i<MAX_CONNECTIONS;i++)
	{
		memset(&UDPPeers[i].addr,0,sizeof(struct _sockaddr));
		UDPPeers[i].valid=0;
		UDPPeers[i].timestamp=0;
		memset(UDPPeers[i].hs_list,0,MAX_CONNECTIONS);
		UDPPeers[i].hstimeout=0;
		UDPPeers[i].relay=0;
	}
	myid=0;
}

// Send text to someone
// This function get's IPXHeader as address. The first byte in this header represents the UDPPeers ID, so sAddr can be assigned.
static int ipx_udp_SendPacket(IPXPacket_t *IPXHeader, ubyte *text, int len)
{
	// check if Header is in a sane range for UDPPeers
	if (IPXHeader->Destination.Node[0] >= MAX_CONNECTIONS)
		return 0;

	if (!UDPPeers[IPXHeader->Destination.Node[0]].valid)
		return 0;

	return sendto (udpsock, text, len, 0, (struct sockaddr *) &UDPPeers[IPXHeader->Destination.Node[0]].addr, sizeof(struct _sockaddr));
}

// Gets some text
// Returns 0 if nothing on there
// rd can safely be ignored here
int ipx_udp_ReceivePacket(char *text, int len, struct ipx_recv_data *rd)
{
	unsigned int clen = sizeof (struct _sockaddr), msglen = 0;
	struct _sockaddr sAddr;

	if (udpsock == -1)
		return -1;

	if (ipx_udp_general_PacketReady())
	{
		msglen = recvfrom (udpsock, text, len, 0, (struct sockaddr *) &sAddr, &clen);

		if (msglen < 0)
			return 0;

		if ((msglen >= 0) && (msglen < len))
			text[msglen] = 0;

		// Wrap UDP CFG packets here!
		if (!memcmp(text+0,DXXcfgid,4))
		{
			ipx_udp_ReceiveCFG(text,&sAddr);
			return 0;
		}

		// Check for Disconnect!
		ipx_udp_CheckDisconnect(&sAddr, text);

		// Seems someone wants to enter the game actually.
		// If I am host, init handshakes! if not sccessful, return 0 and never process this player's request signal - cool thing, eh?
		if (text[0] == PID_D1X_REQUEST && myid == 0)
			if (!ipx_udp_HandshakeFrame(&sAddr,text))
				return 0;
				
		ipx_udp_PacketRelay(text, msglen, &sAddr);
	}

	return msglen;
}

int ipx_udp_general_PacketReady(void)
{
	fd_set set;
	struct timeval tv;

	FD_ZERO(&set);
	FD_SET(udpsock, &set);
	tv.tv_sec = tv.tv_usec = 0;
	if (select(udpsock + 1, &set, NULL, NULL, &tv) > 0)
		return 1;
	else
		return 0;
}

struct ipx_driver ipx_udp =
{
	ipx_udp_OpenSocket,
	ipx_udp_CloseSocket,
	ipx_udp_SendPacket,
	ipx_udp_ReceivePacket,
	ipx_udp_general_PacketReady,
	0, //save 4 bytes.  udp/ip is completely inaccessable by the other methods, so we don't need to worry about compatibility.
	NULL, //use the easier ones
	NULL, //use the easier ones
	NULL  //use the easier ones
};
