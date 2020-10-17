/*---------------------------------------------------------------------
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

A UPD/TCP communication module.
NOW: For IPv4 ONLY!

1. Structs for address:
struct sockaddr {
	sa_family_t sa_family;
	char        sa_data[14];
}

	--- IPv4 ---
struct sockaddr_in {
	short int 		sin_family;
	unsigend short it	sin_port;
	struct in_addr		sin_addr;
	unsigned char		sin_zero[8];
}
struct in_addr {
	union {
		struct { u_char s_b1,s_b2,s_b3,s_b4; } S_un_b;
		struct { u_short s_w1,s_w2;} S_un_w;
		u_long S_addr;
	} S_un;
	#define s_addr S_un.S_addr
}

2. sendmsg() v.s. sendto()
   ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
                  const struct sockaddr *dest_addr, socklen_t addrlen);

   ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);
   struct msghdr {
               void         *msg_name;       // optional addrese
               socklen_t     msg_namelen;    // size of address
               struct iovec *msg_iov;        // scatter/gather array
               size_t        msg_iovlen;     // # elements in msg_iov
               void         *msg_control;    // ancillary data, see below
               size_t        msg_controllen; // ancillary data buffer len
               int           msg_flags;      // flags (unused)
   };

3. A UDP Server is calling Non_blocking recvfrom(), while a UDP Client is calling
   blocking recvfrom(). A UDP Server needs to transmit data as quickly as
   possible, while a UDP Client only receives data at most time, responds to the
   server only occasionaly. You can also ajust TimeOut for blocking type func.
   recvfrom().

4. UDP Tx/Rx speeds depends on many facts and situations.
   It's difficult to always gear up with the Server with a right timing:
   packet size, numbers of clients, backcall() strategy, request conflicts,
   system task schedule, ....

   1 UDP server, 2 UDP Clients, keep Two_Way transmitting. --- Seems Good and stable!  Server Tx/Rx=4.5Mbps

5. Datagram sockets permit zero-length datagrams, while for stream sockets is usually a shutdown signal.

6. High speed TCP stream transmission will invoke high CPU load.

7. Any INET API functions, like write/read/recv/recvfrom/send/sendto... etc,only deal with kernel buffers!
   It's the kernel that takes care of all end-to-end TCP/UDP connections,transport and reliablity.

8. When sendto() returns (with timeout), it ONLY finish passing returned bytes of data to the kernel!
   So as recvfrom(), it ONLY returns number of bytes passed from the kernel.
   It may NOT finish sending/receiving compelete data by only one call! You have
   to check returned number of bytes with expected size of data.

9. A big packet needs more/extra time/load for transmitting and coordinating!?
   A big packet is fragmented to fit for link MTU first.
   and will be more possible to interfere other WIFI devices. Select a suitable packet size!
   Pros: To increase packet size in a rarely disturbed network can improve general speed considerablely.
   Cons: it costs more CPU Load, especially for the client of file receiver, and fluctuation on data stream.
   Example: Use bigger packet size in Ethernet than in WiFi net.

10. Linux man recv(): About peer stream socket close:
    "When a stream socket peer has performed an orderly shutdown, the return value will be 0".
    "The value 0 may also be returned if the requested number of bytes to receive from a stream socket was 0."

11. Linux man send(): About return of EPIPE:
    "The local end has been shut down on a connection oriented socket."

12. TCP flags FIN/RST, and TCP_CLOSE_WAIT.
    After first receving FIN from peer end, local sockfd is still OK. If we call send() again, the peer end will reply RST this time!
    It usually needs a while to receive RST,which RST will trigger SIGPIPE, and info.tcpi_state==TCP_CLOSE_WAIT before that.
    ( After catching the SIGPIPE, to call getsockopt(sockfd,IPPROTO_TCP, TCP_INFO, &info, ...) will get info.tcpi_state==TCP_CLOSE! )
    If network is broken before RTS is received, A TCP_CLOSE_WAIT status may last as long as 2 hours!?

13. TCP shutdown(sockfd, ) only shuts down socket receptions and/or transmissions, sockfd is still valid, you can still call getsockopt()
    to get its status;  while if you close() the sockfd, then it will be invalid.

TODO:
1. Auto. reconnect.

Midas Zhou
midaszhou@yahoo.com
--------------------------------------------------------------------------*/
#include <sys/types.h> /* this header file is not required on Linux */
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h> /* inet_ntoa() */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <egi_inet.h>
#include <egi_timer.h>
#include <egi_log.h>

/* ========================  EGI_INETPACK Functions  ========================== */

/*--------------------------------------------------------
Create an EGI_INETPACK with given size.

@headsize:	     Head size of an EGI_INETPACK.
@privdata_size:      size of privdata in EGI_INETPACK.data[]

Return:
	Pointer to EGI_INETPACK	OK
	NULL			Fails
--------------------------------------------------------*/
EGI_INETPACK* inet_ipacket_create(int headsize, int privdata_size)
{
	EGI_INETPACK* ipack=NULL;
	int datasize;

	/* Check input */
	if(headsize<0 || privdata_size<0)
		return NULL;

	/* Datasize */
	datasize=headsize+privdata_size;

	/* Calloc ipack */
	ipack=calloc(1, sizeof(EGI_INETPACK)+datasize);
	if(ipack==NULL) {
		printf("%s: Fail to calloc ipack!\n",__func__);
		return NULL;
	}

	/* Assign memebers */
	ipack->packsize=sizeof(EGI_INETPACK)+datasize;
	ipack->headsize=headsize;

	return ipack;
}


/*---------------------------------------------
	Free an EGI_INETPACK.
----------------------------------------------*/
void inet_ipacket_free(EGI_INETPACK** ipack)
{
	if( ipack==NULL || *ipack==NULL)
		return;

	free(*ipack);
	*ipack=NULL;
}


/*------------------------------------------
Get pointer to  header of EGI_INETPACK.data
-------------------------------------------*/
void*  IPACK_HEADER(EGI_INETPACK *ipack)
{
	if(ipack==NULL)
        	return NULL;
        else if(ipack->headsize<1)
        	return NULL;
        else
                return (char *)ipack +sizeof(EGI_INETPACK);
}


/*------------------------------------------
Get pointer to  privdata of EGI_INETPACK.data
-------------------------------------------*/
void*  IPACK_PRIVDATA(EGI_INETPACK *ipack)
{
	if(ipack==NULL)
		return NULL;
        else if( ipack->packsize <= ipack->headsize+sizeof(EGI_INETPACK) )
		return NULL;
        else
		return (char *)(ipack) +sizeof(EGI_INETPACK) +ipack->headsize;
}


/* ========================  INET MSGDATA Functions  ========================== */


/*--------------------------------------------------------
Create an EGI_INET_MSGDATA with its flexible array data[]
callocated by given size.

@datasize:  Data size in MSGDATA.
Return:
	Pointer to EGI_INET_MSGDATA	OK
	NULL				Fails
--------------------------------------------------------*/
EGI_INET_MSGDATA* inet_msgdata_create(uint32_t datasize)
{
	EGI_INET_MSGDATA* msgdata=NULL;

	/* Datasize is size of flexible array msgdata->data[]. */
	msgdata=calloc(1,sizeof(EGI_INET_MSGDATA)+datasize);
	if(msgdata==NULL) {
		printf("%s: Fail to calloc!\n",__func__);
		return NULL;
	}

   #if 0  /* Write size to msg.datasize[] */
	int i;
        for(i=0; i<4; i++) {  /* 4 bytes, the least significant byte first. */
                msgdata->msg.datasize[i]=(datasize>>(i<<3))&0xFF;
        }
   #else
	msgdata->msg.nl_datasize=htonl(datasize);
   #endif

	return msgdata;
}

/*---------------------------------------------
	Free an EGI_INET_MSGDATA.
----------------------------------------------*/
void inet_msgdata_free(EGI_INET_MSGDATA** msgdata)
{
	if(msgdata==NULL || *msgdata==NULL)
		return;

	free(*msgdata);
	*msgdata=NULL;
}


/*---------------------------------------
Get data size of msgdata->data[]
Return:
	>=0	OK
	<0	Fails
----------------------------------------*/
inline int inet_msgdata_getDataSize(const EGI_INET_MSGDATA *msgdata)
{
	int size;

        if(msgdata==NULL)
                return -1;

	#if 0
	int i;
	for( size=0, i=0; i<4; i++)
		size += msgdata->msg.datasize[i]<<(i<<3);
	#else
	size=ntohl(msgdata->msg.nl_datasize);
	#endif

	return size;
}

/*---------------------------------------------------------
Adjust mem space of MSGDATA.data[] by realloc whole MSGDATA,
nl_datasize will be updated and all data[] cleared.

Note:
1. Usually datasize>=256k will trigger MEM area movement.

Return:
	0	OK
	<0	Fails
---------------------------------------------------------*/
inline int inet_msgdata_reallocData(EGI_INET_MSGDATA **msgdata, int datasize)
{
	void* ptr=NULL;

	if(msgdata==NULL||*msgdata==NULL)
		return -1;
	if(datasize<0)
		return -1;

	ptr=realloc(*msgdata, sizeof(EGI_INET_MSGDATA)+datasize);
	if(ptr==NULL)
		return -2;

	/* Reset nl_datasize and clear data */
	bzero( ptr+sizeof(EGI_INET_MSGDATA), datasize);

	/* Reassign msgdata and nl_datasize */
	*msgdata=ptr;
	(*msgdata)->msg.nl_datasize=htonl(datasize);

	return 0;
}


/*---------------------------------------
Get total size of msgdata:
struct size + data size of msgdata->data[]

Return:
	>=0	OK
	<0	Fails
----------------------------------------*/
inline int inet_msgdata_getTotalSize(const EGI_INET_MSGDATA *msgdata)
{
	int size;

        if(msgdata==NULL)
                return -1;

	#if 0
	int i;
	for( size=0, i=0; i<4; i++)
		size += msgdata->msg.datasize[i]<<(i<<3);
	#else
	size=ntohl(msgdata->msg.nl_datasize);
	#endif

	return sizeof(EGI_INET_MSGDATA)+size;
}


/*---------------------------------------------------
Update time stamp in msgdata.

@msgdata:	Pointer to an EGI_INET_MSGDATA.

Return:
	0	OK
	<0	Fails
----------------------------------------------------*/
inline int inet_msgdata_updateTimeStamp(EGI_INET_MSGDATA *msgdata)
{
	if(msgdata==NULL)
		return -1;

        return gettimeofday(&msgdata->msg.tmstamp, NULL);
}


/*---------------------------------------------------
Load data into a EGI_INET_MSGDATA. Data size MUST
preset in msgdata->datasize[].

@msgdata:	Pointer to an EGI_INET_MSGDATA.
@data:		Pointer to source data.

Return:
	0	OK
	<0	Fails
----------------------------------------------------*/
int inet_msgdata_loadData(EGI_INET_MSGDATA *msgdata, const char *data)
{
        int size;

	/* Check input */
        if(msgdata==NULL)
                return -1;

	/* Get datasize */
	size=inet_msgdata_getDataSize(msgdata);
	if(size<1)
		return -2;

        /* Copy data to msgdata->data */
        memcpy(msgdata->data, data, size);

        return 0;
}


/*---------------------------------------------------
Copy data from a EGI_INET_MSGDATA.

@smgdata:  Pointer to  an EGI_IENT_MSGDATA.
@data:	   Pointer to dest data.
	   The caller MUST ensure enough space!

Return:
	>=0	OK, size of data
	<0	Fails
----------------------------------------------------*/
int inet_msgdata_copyData(const EGI_INET_MSGDATA *msgdata, char *data)
{
        int size;

        /* Check input */
        if(msgdata==NULL || data==NULL )
                return -1;

	size=inet_msgdata_getDataSize(msgdata);
	if(size<0)
		return -2;

        /* Copy data to msgdata->data */
        memcpy(data, msgdata->data, size);

        return size;
}




/*----------------------------------------------------------
Receive MSGDATA from a socket, size of the MSGDATA is already
known.

@fd:		A socket descriptor.
		Timeout and block mode set as expected.
@msgdata:	A pointer to an EGI_INET_MSGDATA, into which
		received data will be filled.

Note:
1. For TCP connection mode, sockfd expected to be BLOCK type.
   For UPD mode, sockfd expected to be NON_BLOCK type ?

Return (see inet_tcp_recv()):
	>0	Socket closed.
	0	OK, a complete MSGDATA received.
	<0	Fails
-----------------------------------------------------------*/
int inet_msgdata_recv(int sockfd, EGI_INET_MSGDATA *msgdata)
{
	int packsize;   /* msg+data size */

	if(msgdata==NULL)
		return -1;

	/* Get msgdata size */
	packsize=inet_msgdata_getTotalSize(msgdata);
	if( packsize < 1 )
		return -2;

	return  inet_tcp_recv(sockfd, (void *)msgdata, packsize);
}


/*---------------------------------------------------------
Send a MSGDATA through a socket.

@fd:		A socket descriptor.
		Timeout and block mode set as expected.

@msgdata:	A pointer to an EGI_INET_MSGDATA which will
		be sent out.

Note:
1. For TCP connection mode, sockfd expected to be BLOCK type.
   For UPD mode, sockfd expected to be NON_BLOCK type ?
2. If packsize > Single TCP/UDP Max Payload, it then will be
   fregmented by kerenl to fit for linker MTU.

Return:
	>0	Peer socket closed.
	0	OK, a complete MSGDATA send out (to kenerl).
	<0	Fails
-----------------------------------------------------------*/
int inet_msgdata_send(int sockfd, const EGI_INET_MSGDATA *msgdata)
{
	int packsize;   /* msg+data size */

	if(msgdata==NULL)
		return -1;

	/* Get msgdata size */
	packsize=inet_msgdata_getTotalSize(msgdata);
	if( packsize < 1 )
		return -2;

	return  inet_tcp_send(sockfd, (void *)msgdata, packsize);
}


	/* ========================  Signal Handling Functions  ========================== */


/*--------------------------------------
	Signal handlers
----------------------------------------*/
void inet_signals_handler(int signum)
{
	switch(signum) {
		case SIGPIPE:
			printf("Catch a SIGPIPPE!\n");
			break;
		case SIGINT:
			printf("Catch a SIGINT!\n");
			break;
		default:
			printf("Catch signum %d\n", signum);
	}

}
__attribute__((weak)) void inet_sigpipe_handler(int signum)
{
	printf("Catch a SIGPIPE!\n");
}

__attribute__((weak)) void inet_sigint_handler(int signum)
{
	printf("Catch a SIGINT!\n");
}


/*--------------------------------------------------
Set a signal acition.

@signum:        Signal number.
@handler:       Handler for the signal

Return:
        0       Ok
        <0      Fail
---------------------------------------------------*/
int inet_register_sigAction(int signum, void(*handler)(int))
{
	struct sigaction sigact;

	// .sa_mask: a  mask of signals which should be blocked during execution of the signal handler
        sigemptyset(&sigact.sa_mask);
	// the signal which triggered the handler will be blocked, unless the SA_NODEFER flag is used.
        //sigact.sa_flags|=SA_NODEFER;
        sigact.sa_handler=handler;
        if(sigaction(signum, &sigact, NULL) <0 ){
                printf("%s: Fail to call sigaction() for signum %d.\n",  __func__, signum);
                return -1;
        }

        return 0;
}

/*-----------------------------
Set default signal actions.
------------------------------*/
int inet_default_sigAction(void)
{
	if( inet_register_sigAction(SIGPIPE,inet_sigpipe_handler)<0 )
		return -1;

//	if( inet_register_sigAction(SIGINT,inet_sigpipe_handler)<0 )
//		return -1;

	return 0;
}


/* ======================== TCP/UDP C/S Functions  ========================== */

/*----------------------------------------------------------
Receive data from sockfd with specified size.

@fd:		A socket descriptor.
		Timeout and block mode set as expected.
@data:		Pointer to data[]
@packsize:	Size expected to receive.

Note:
1. For TCP connection mode, sockfd expected to be BLOCK type.
   For UPD mode, sockfd expected to be NON_BLOCK type ?
2. The function MAY stuck in dead loop?

Return:
	>0	(=1)Peer socket closed.
	0	OK, packisze data received.
	<0	Fails
-----------------------------------------------------------*/
inline int inet_tcp_recv(int sockfd, void *data, size_t packsize)
{
	int nrcv;	/* Number of bytes receied in one round */
	int rcvsize;	/* accumulated size */

	if(data==NULL)
		return -1;
	if(packsize<1)
		return -2;

     /* --------------- Loop recv(BLOCK) Method --------------- */
	/* Big msgdata MAY need serveral rounds of recv()...*/
	rcvsize=0;
	while( rcvsize < packsize ) {
		/* Expect to be BLOCK type socket for TCP link. */
		nrcv=recv(sockfd, data+rcvsize, packsize-rcvsize, MSG_WAITALL); /* Wait all data, but still MAY fail! see man */
		if(nrcv>0) {
			/* count rcvsize  */
			rcvsize += nrcv;

			if( rcvsize==packsize) {
				break; /* OK */
			}
			else if( rcvsize < packsize ) {
				continue; /* -----> continue to recv remaining data */
			}
			else { /* rcvsize > packsize, '>' seems impossible! */
				printf("%s: rcvsize > packsize, impossible!\n",__func__);
				return -3;
			}
	        }
        	else if(nrcv==0) {
               		printf("%s: Peer socket closed!\n",__func__);
			return 1;
        	}
	        else if(nrcv<0) {
                	switch(errno) {
                        	#if(EWOULDBLOCK!=EAGAIN)
                                case EWOULDBLOCK:
                                #endif
                                case EAGAIN:  //EWOULDBLOCK, for datastream it woudl block */
					printf("%s: Timeout! try again...\n",__func__);
					usleep(50000);
					continue;  /* ---> continue to recv while() */
					break;
				default:
		        	  	printf("%s: Err'%s'\n", __func__, strerror(errno));
					return -4;
			}
		}
   	}

	return 0;
}


/*---------------------------------------------------------
Send out data through a socket.

@fd:		A socket descriptor.
		Timeout and block mode set as expected.
@data:		Pointer to data[]
@packsize:	Size expected to send out.

Note:
1. For TCP connection mode, sockfd expected to be BLOCK type.
   For UPD mode, sockfd expected to be NON_BLOCK type ?
2. If packsize > Single TCP/UDP Max Payload, it then will be
   fregmented by kerenl to fit for linker MTU.
3. The function MAY stuck in dead loop?

Return:
	>0	(=1)Peer socket closed.
	0	OK, data sent out (to kenerl).
	<0	Fails
-----------------------------------------------------------*/
inline int inet_tcp_send(int sockfd, const void *data, size_t packsize)
{
	int nsnd;	/* Number of bytes sent out in one round */
	int sndsize;	/* accumulated size */

	if(data==NULL)
		return -1;
	if(packsize<1)
		return -2;

	 /* --------------- Loop send(BLOCK) Method --------------- */
	/* Big msgdata MAY need serveral rounds of recv()...*/
	sndsize=0;
	while( sndsize < packsize ) {
		/* Expect to be BLOCK type socket for TCP link. */
		nsnd=send(sockfd, data+sndsize, packsize-sndsize, 0); /* In connectiong_mode, flags: MSG_CONFIRM, MSG_DONTWAIT */
		if(nsnd>0) {
			/* count rcvsize  */
			sndsize += nsnd;

			if( sndsize==packsize) {
				break; /* OK */
			}
			else if( sndsize < packsize ) {
				continue; /* -----> continue to send remaining data */
			}
			else { /* sndsize > packsize, '>' seems impossible! */
				printf("%s: sndsize > packsize, impossible!\n", __func__);
				return -3;
			}
	        }
	        else if(nsnd<0) {
                	switch(errno) {
                        	#if(EWOULDBLOCK!=EAGAIN)
                                case EWOULDBLOCK:
                                #endif
                                case EAGAIN:  //EWOULDBLOCK, for datastream it woudl block */
					printf("%s: Timeout! try again...\n",__func__);
					usleep(50000);
					continue;  /* ---> continue to recv while() */
					break;
				case EPIPE:
		               		printf("%s: Peer socket closed!\n",__func__);
					return 1;
				default:
		        	       	printf("%s: Err'%s'\n", __func__, strerror(errno));
					return -4;
			}
		}
		else { /* nsnd==0 */
			printf("%s: snd==0!\n", __func__);
		}
   	}

	return 0;
}


/*---------------------------------------------------------
Get TCP connection state value as defined in netinet/tcp.h

Note:
1. An illegal sockfd will cause segment fault!

@sockfd: Socket file descriptor.

Return:
	>0	TCP state value
	<0	Fails
-----------------------------------------------------------*/
inline int inet_tcp_getState(int sockfd)
{
	const static char str_states[13][32]={
  "TCP_states_0",      /* =0 */
  "TCP_ESTABLISHED",
  "TCP_SYN_SENT",
  "TCP_SYN_RECV",
  "TCP_FIN_WAIT1",
  "TCP_FIN_WAIT2",
  "TCP_TIME_WAIT",
  "TCP_CLOSE",
  "TCP_CLOSE_WAIT",
  "TCP_LAST_ACK",
  "TCP_LISTEN",
  "TCP_CLOSING",
  "TCP_state_undefine"  /* =12 */
  	};

	int state;
	int index;
	struct tcp_info info;
        int len=sizeof(info);

        if( getsockopt(sockfd,IPPROTO_TCP, TCP_INFO, &info, (socklen_t *)&len)==0 ) {
		state=info.tcpi_state;
		index= ( state>=0 && state<12) ? state : 12;
		printf("%s: '%s'\n", __func__, str_states[index]);
		return state;
        }
        else {
		printf("%s: getsockopt Err'%s'.\n",__func__, strerror(errno));
		return -1;
	}
}

/*-----------------------------------------------------
Create an UDP server.

@strIP:	UDP IP address.
	If NULL, auto selected by htonl(INADDR_ANY).
@Port:  Port Number.
	If 0, auto selected by system.
@domain:  If ==6, IPv6, otherwise IPv4.
	TODO: IPv6 connection.
Return:
	Pointer to an EGI_UPD_SERV	OK
	NULL				Fails
-------------------------------------------------------*/
EGI_UDP_SERV* inet_create_udpServer(const char *strIP, unsigned short port, int domain)
{
	struct timeval timeout={10,0}; /* Default time out send/receive */
	EGI_UDP_SERV *userv=NULL;

	/* Calloc EGI_UDP_SERV */
	userv=calloc(1,sizeof(EGI_UDP_SERV));
	if(userv==NULL)
		return NULL;

	/*--------------------------------------------------
       	AF_INET             IPv4 Internet protocols
       	AF_INET6            IPv6 Internet protocols
       	AF_IPX              IPX - Novell protocols
       	AF_NETLINK          Kernel user interface device
       	AF_X25              ITU-T X.25 / ISO-8208 protocol
       	AF_AX25             Amateur radio AX.25 protocol
       	AF_ATMPVC           Access to raw ATM PVCs
       	AF_APPLETALK        AppleTalk
       	AF_PACKET           Low level packet interface
       	AF_ALG              Interface to kernel crypto API
	---------------------------------------------------*/
	#if 0
	if(domain==6)
		domain=AF_INET6;
	else
		domain=AF_INET;
	#else
	domain=AF_INET;
	#endif

	/* Create UDP socket fd */
	userv->sockfd=socket(domain, SOCK_DGRAM|SOCK_CLOEXEC, 0);
	if(userv->sockfd<0) {
		printf("%s: Fail to create datagram socket, '%s'\n", __func__, strerror(errno));
		free(userv);
		return NULL;
	}

	/* 2. Init. sockaddr */
	userv->addrSERV.sin_family=domain;
	if(strIP==NULL)
		userv->addrSERV.sin_addr.s_addr=htonl(INADDR_ANY);
	else
		userv->addrSERV.sin_addr.s_addr=inet_addr(strIP);
	userv->addrSERV.sin_port=htons(port);

	/* 3. Bind socket with sockaddr(Assign sockaddr to socekt) */
	if( bind(userv->sockfd,(struct sockaddr *)&(userv->addrSERV), sizeof(userv->addrSERV)) < 0) {
		printf("%s: Fail to bind sockaddr, Err'%s'\n", __func__, strerror(errno));
		free(userv);
		return NULL;
	}

        /* 4. Set default SND/RCV timeout,  recvfrom() and sendto() can set flag MSG_DONTWAIT. */
        if( setsockopt(userv->sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout)) <0 ) {
                printf("%s: Fail to setsockopt for snd_timeout, Err'%s'\n", __func__, strerror(errno));
		free(userv);
		return NULL;
        }
        if( setsockopt(userv->sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) <0 ) {
                printf("%s: Fail to setsockopt for recv_timeout, Err'%s'\n", __func__, strerror(errno));
		free(userv);
		return NULL;
        }

	printf("%s: An EGI UDP server created at %s:%d.\n", __func__, inet_ntoa(userv->addrSERV.sin_addr), ntohs(userv->addrSERV.sin_port));

	/* OK */
	return userv;
}


/* ------------------------------------------
Close an UDP SERV and release resoruces.

Return:
	0	OK
	<0	Fails
--------------------------------------------*/
int inet_destroy_udpServer(EGI_UDP_SERV **userv)
{
	if(userv==NULL || *userv==NULL)
		return 0;

        /* Close sockfd */
        if(close((*userv)->sockfd)<0) {
                printf("%s: Fail to close datagram socket, Err'%s'\n", __func__, strerror(errno));
		return -1;
	}

	/* Free mem */
	free(*userv);
	*userv=NULL;

	return 0;
}


/*--------------------------------------------------------------
UDP service routines:  Receive data from client and pass
it to the caller AND get data from the caller and send it
to client.

	------ One Routine, One caller -------

NOTE:
1. For a UDP server, the routine process always start with
   recvfrom().
2. If sndCLIT is meaningless, then sendto() will NOT execute.
   SO a caller may stop server to sendto() by clearing sndCLIT.

   			!!! WARNING !!!
   The counter peer client should inform routine Caller to clear/reset
   sndCLIT before close the connection! OR the routine will NEVER
   stop sending data, even the peer client is closed.
   UDP is a kind of connectionless link!

3. nrcv<0. means no data from client.

TODO.
1. IPv6 connection.
2. Use select/poll to monitor IO fd.

@userv: Pointer to an EGI_UDP_SERV.

Return:
	0	OK
	<0	Fails
--------------------------------------------------------------*/
int inet_udpServer_routine(EGI_UDP_SERV *userv)
{
	struct sockaddr_in rcvCLIT={0};   /* Client for recvfrom() */
	struct sockaddr_in sndCLIT={0};   /* Client for sendto() */
	int nrcv,nsnd;
	socklen_t clen;
	char rcvbuff[EGI_MAX_UDP_PDATA_SIZE]; /* EtherNet packet payload MAX. 46-MTU(1500) bytes,  UPD packet payload MAX. 2^16-1-8-20=65507 */
	char sndbuff[EGI_MAX_UDP_PDATA_SIZE];
	int  sndsize;	    /* size of to_send_data */

	/* Check input */
	if( userv==NULL )
		return -1;
	if( userv->callback==NULL ) {
		printf("%s: Callback is NOT defined!\n", __func__);
		return -2;
	}

	/*** Loop service processing: ***/
	printf("%s: Start UDP service loop processing...\n", __func__);
	while(1) {
		/* ---- UDP Server: NON_Blocking recvfrom() ---- */
                /*  MSG_DONTWAIT (since Linux 2.2)
                 *  Enables  nonblocking  operation; if the operation would block, EAGAIN or EWOULDBLOCK is returned.
                 */
		clen=sizeof(rcvCLIT); /*  Before callING recvfrom(), it should be initialized */
		nrcv=recvfrom(userv->sockfd, rcvbuff, sizeof(rcvbuff), MSG_CMSG_CLOEXEC|MSG_DONTWAIT, &rcvCLIT, &clen); /* MSG_DONTWAIT, MSG_ERRQUEUE */
		/* TODO: What if clen != sizeof( struct sockaddr_in ) ! */
		if( clen != sizeof(rcvCLIT) )
			printf("%s: clen != sizeof(rcvCLIT)!\n",__func__);
		/* Datagram sockets in various domains permit zero-length datagrams */
		if(nrcv<0) {
			//printf("%s: Fail to call recvfrom(), Err'%s'\n", __func__, strerror(errno));
			switch( errno ) {
                                #if(EWOULDBLOCK!=EAGAIN)
                                case EWOULDBLOCK:
                                #endif
				case EAGAIN:
					printf("%s: errno==EAGAIN/EWOULDBLOCK.\n",__func__);
					break;
				default:
					return -2;
			}
		}

		/* ---NOW---: nrcv>0 || nrcv<0 || nrc==0 */

	        #if 0 /* --- TEST --- */
		printf("Receive from client '%s:%d' with data: '%s'\n",
			inet_ntoa(rcvCLIT.sin_addr), ntohs(rcvCLIT.sin_port), rcvbuff);
		#endif

	    	/* Process callback: Pass received data and get next send data */

		/***  CALLBACK ( const struct sockaddr_in *rcvAddr, const char *rcvData, int rcvSize,
                 *                     struct sockaddr_in *sndAddr,       char *sndBuff, int *sndSize);
		 *   Pass out: rcvCLIT, rcvbuff, rcvsize
		 *   Take in:  sndCLIT, sndbuff, sndsize
		 */
		sndsize=0; /* reset first */
		userv->callback(&rcvCLIT, rcvbuff, nrcv,  &sndCLIT, sndbuff, &sndsize);

                /* Send data to server */
		clen=sizeof(sndCLIT); /* Always SAME here!*/

		/* Sendto: Only If sndCLIT is meaningful and sndaSize >0 */
		if( sndCLIT.sin_addr.s_addr !=0  && sndsize>0 ) {
       		        nsnd=sendto(userv->sockfd, sndbuff, sndsize, 0, &sndCLIT, clen); /* flags: MSG_CONFIRM, MSG_DONTWAI */
        		if(nsnd<0){
       	        		printf("%s: Fail to sendto() data to client '%s:%d', Err'%s'\n",
						__func__, inet_ntoa(sndCLIT.sin_addr), ntohs(sndCLIT.sin_port), strerror(errno));
                	        //GO ON... return -3;
	        	}
       	        	else if(nsnd!=sndsize) {
        	        	printf("%s: WARNING! Send %d of total %d bytes to '%s:%d'!\n",
						__func__, nsnd, sndsize, inet_ntoa(sndCLIT.sin_addr), ntohs(sndCLIT.sin_port) );
			}
               	}

	    	/* END one round routine. sleep if NO DATA received */
		if(nrcv<0)
	    		usleep(5000);
	}

	return 0;
}


/*-----------------------------------------------------------------
A TEST callback routine for UPD server.
1. Tasks in callback function should be simple and short, normally
just to pass out received data to the Caller, and take in data to
the UDP server.
2. In this TESTcallback, the server do the 'count+1' job, and then
   respond the count result to the client! There may be several
   clients, but OK, it always send to the sndAddr=rcvAddr!


@rcvAddr:       Client address, from which rcvData was received.
@rcvData:       Data received from rcvAddr
@rcvSize:       Data size of rcvData.

@sndAddr:       Client address, to which sndBuff will be sent.
@sndBuff:       Data to send to sndAddr
@sndSize:       Data size of sndBuff

Return:
	0	OK
	<0	Fails
------------------------------------------------------------------*/
int inet_udpServer_TESTcallback( const struct sockaddr_in *rcvAddr, const char *rcvData, int rcvSize,
                                       struct sockaddr_in *sndAddr, 	  char *sndBuff, int *sndSize)
{
	int  psize=EGI_MAX_UDP_PDATA_SIZE; /* EGI_MAX_UDP_PDATA_SIZE=1024;  UDP packet payload size, in bytes. */
	char buff[psize];
	char *pt=NULL;
	unsigned int clitcount;	      /* Counter in Client message */
	static struct sockaddr_in tmpAddr={0};

	/* Only if received data */
	if( rcvSize < 0) {
		*sndSize=0;
		return 0;
	}

	/* Check whether sender's addres changes! */
	if(rcvAddr->sin_port != tmpAddr.sin_port || rcvAddr->sin_addr.s_addr != tmpAddr.sin_addr.s_addr ) {
		tmpAddr=*rcvAddr;
	}

	/* ---- 1. Take data from EGI_UDP_SERV ---- */

	/*  Process/Prepare send data */
	/* Get data */
	memcpy(buff, rcvData, rcvSize);

	/* For test purpose only, to set an EOF for string. so we may NOT need to memset(buff,0,psize)  */
	if(psize>1024)
		buff[1024]=0;
	else
		buff[psize-1]=0; /* set EOF */

	/* Get count in client message and respond with clitcount+1. */
        pt=strstr(buff,"=");
        if(pt!=NULL) {
                  clitcount=strtoul(pt+1,NULL,10);
		  //sprintf(buff,"clitcount=%u, svrcount=%u   --- drops=%d ---", clitcount, count, clitcount-count);
                  sprintf(buff,"Respond count=%u", clitcount+1);
        }
        else {
                 //sprintf(buff,"Received count=?");
                  sprintf(buff,"Respond count=%u", clitcount+1);  /* Old clitcount */
	}

	/* ---- 2. Pass data to EGI_UDP_SERV ---- */
	/* 1k packet payload */
	*sndSize=psize;
	memcpy(sndBuff, buff, psize);
	*sndAddr=*rcvAddr;  /* Send to the same client */

	return 0;
}


/*------------------------------------------------------------
Create an UDP client.
And always send out a zero length datagram to probe the server.
as a sign to the server that a new client/session MAY begin.

@strIP:	Server UDP IP address.
@Port:  Server Port Number.
@domain:  If ==6, IPv6, otherwise IPv4.
	TODO: IPv6 connection.
Return:
	Pointer to an EGI_UPD_SERV	OK
	NULL				Fails
-------------------------------------------------------------*/
EGI_UDP_CLIT* inet_create_udpClient(const char *servIP, unsigned short servPort, int domain)
{
	struct timeval timeout={10,0}; /* Default time out */
        socklen_t clen;
	EGI_UDP_CLIT *uclit=NULL;

	/* Check input */
	if(servIP==NULL)
		return NULL;

	/* Calloc EGI_UDP_CLIT */
	uclit=calloc(1,sizeof(EGI_UDP_CLIT));
	if(uclit==NULL)
		return NULL;

	/* Set domain */
	#if 0
	if(domain==6)
		domain=AF_INET6;
	else
		domain=AF_INET;
	#else
	domain=AF_INET;
	#endif

	/* Create UDP socket fd */
	uclit->sockfd=socket(domain, SOCK_DGRAM|SOCK_CLOEXEC, 0);
	if(uclit->sockfd<0) {
		printf("%s: Fail to create datagram socket, '%s'\n", __func__, strerror(errno));
		free(uclit);
		return NULL;
	}

	/* 2. Init. sockaddr */
	uclit->addrSERV.sin_family=domain;
	uclit->addrSERV.sin_addr.s_addr=inet_addr(servIP);
	uclit->addrSERV.sin_port=htons(servPort);

	uclit->addrME.sin_family=domain;
	uclit->addrME.sin_addr.s_addr=htonl(INADDR_ANY);
	printf("%s: Initial addrME at '%s:%d'\n", __func__, inet_ntoa(uclit->addrME.sin_addr), ntohs(uclit->addrME.sin_port));

	/* NOTE: You may bind CLIENT to a given port, mostly not necessary. If 0, let kernel decide. */
	if( bind(uclit->sockfd,(struct sockaddr *)&(uclit->addrME), sizeof(uclit->addrME)) < 0)
		printf("%s: Fail to bind ME to sockfd, Err'%s'\n", __func__, strerror(errno));


        /* 3. Set default SND/RCV timeout,   recvfrom() and sendto() can set flag MSG_DONTWAIT. */
        if( setsockopt(uclit->sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout)) <0 ) {
                printf("%s: Fail to setsockopt for snd_timeout, Err'%s'\n", __func__, strerror(errno));
		free(uclit);
		return NULL;
        }
        if( setsockopt(uclit->sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) <0 ) {
                printf("%s: Fail to setsockopt for recv_timeout, Err'%s'\n", __func__, strerror(errno));
		free(uclit);
		return NULL;
        }

	/* 4. To probe the server */
   //#if 0  /* Send 0 data to probe the Server, Only to trigger to let kernel to assign a port number, ignore errors. */
	int nsnd;
	/* For Datagram, dest_addr and addrlen MUST NOT be NULL and 0 */
        nsnd=sendto(uclit->sockfd, NULL, 0, 0, (struct sockaddr *)&uclit->addrSERV,sizeof( typeof(uclit->addrSERV)));
	printf("%s: Sendto() %d lenght datagram to probe the server!\n",__func__, nsnd);
   //#else
	/*** For UDP is an unconnected link, it always return OK for connect()!!!
	 * After calling connect(), the kernel would save link information and relate it to an IMCP!
	 * Example:
         *   ----If call connect() only:
         *   It returns several ECONNREFUSED errors when call recvfrom(BLOCK), if the counter part server is NOT ready!
	 *   and  EAGAIN/EWOULDBLOCK for timeout.
	 *   ----if call sendto() only :
	 *   It returns only EAGAIN/EWOULDBLOCK for timeout when call recvfrom(BLOCK), if the counter part server is NOT ready!
	 *   man: "Connectionless sockets may dissolve the association by connecting to an address with the  sa_family
       	 *   member of sockaddr set to AF_UNSPEC (supported on Linux since kernel 2.2). "
         */
        if( connect(uclit->sockfd, (struct sockaddr *)&uclit->addrSERV, sizeof( typeof(uclit->addrSERV)) )==0 ) {
		printf("%s: Connect to UDP server at %s:%d successfully!\n",
						__func__, inet_ntoa(uclit->addrSERV.sin_addr), ntohs(uclit->addrSERV.sin_port) );
	} else {
                printf("%s: Fail to connect to the UDP server, Err'%s'\n", __func__, strerror(errno));
	}
    //#endif

	/*** Try to getsockname for ME. TODO: Returned addrME.sin_addr is NOT correct, it appears to truncated addrSERV !!!
         *	NOT for UDP ?
         */
        clen=sizeof(uclit->addrME);
        if( getsockname(uclit->sockfd, (struct sockaddr *)&(uclit->addrME), &clen)!=0 ) {
                printf("%s: Fail to getsockname for client, Err'%s'\n", __func__, strerror(errno));
        }
	else
		printf("%s: An EGI UDP client created at ???%s:%d, targeting to the server at %s:%d.\n",
			 __func__, inet_ntoa(uclit->addrME.sin_addr), ntohs(uclit->addrME.sin_port),
				   inet_ntoa(uclit->addrSERV.sin_addr), ntohs(uclit->addrSERV.sin_port)	  );

	/* OK */
	return uclit;
}


/* ------------------------------------------
Close an UDP Client and release resoruces.

Return:
	0	OK
	<0	Fails
--------------------------------------------*/
int inet_destroy_udpClient(EGI_UDP_CLIT **uclit)
{
	if(uclit==NULL || *uclit==NULL)
		return 0;

        /* Close sockfd */
        if(close((*uclit)->sockfd)<0) {
                printf("%s: Fail to close datagram socket, Err'%s'\n", __func__, strerror(errno));
		return -1;
	}

	/* Free mem */
	free(*uclit);
	*uclit=NULL;

	return 0;
}


/*------------------------------------------------------------
UDP client routines: Send data to the Server, and wait for
response. then loop routine process: receive data from the
Server and pass it to the Caller, while get data from the Caller
and send it to the Server.

	------ One Routine, One caller -------

NOTE:
1. For a UDP client, the routine process always start with taking
   in send_data from the Caller and then sendto() the UDP server.

!!!TODO.
1. IPv6 connection.
2. Use select/poll to monitor IO fd.

@userv: Pointer to an EGI_UDP_CLIT.

Return:
	0	OK
	<0	Fails
--------------------------------------------------------------*/
int inet_udpClient_routine(EGI_UDP_CLIT *uclit)
{
	struct sockaddr_in addrSENDER;   /* Address of the sender(server) from recvfrom(), expected to be same as uclit->addrSERV */
	socklen_t sndlen;
	int nrcv,nsnd;
	socklen_t svrlen;
	char rcvbuff[EGI_MAX_UDP_PDATA_SIZE]; /* EtherNet packet payload MAX. 46-MTU(1500) bytes,  UPD packet payload MAX. 2^16-1-8-20=65507 */
	char sndbuff[EGI_MAX_UDP_PDATA_SIZE];
	int  sndsize=-1;	    /* size of to_send_data */

	/* Check input */
	if( uclit==NULL )
		return -1;
	if( uclit->callback==NULL ) {
		printf("%s: Callback is NOT defined!\n", __func__);
		return -2;
	}

        /* Get sock addr length */
        svrlen=sizeof(typeof(uclit->addrSERV));

	/*** Loop service processing ***/
	printf("%s: Start UDP client loop processing...\n", __func__);
	nrcv=0; /* Reset it first */
	while(1) {

		/***  CALLBACK ( const struct sockaddr_in *rcvAddr, const char *rcvData, int rcvSize,
                 *                     				     	  char *sndBuff, int *sndSize);
		 *   Pass out: rcvAddr, rcvbuff, rcvsize
		 *   Take in:   	sndbuff, sndsize
		 */
		sndsize=-1; /* Reset it first */
		uclit->callback(&addrSENDER, rcvbuff, nrcv, sndbuff, &sndsize); /* Init. nrcv=0 */

		/* Send data to ther Server */
		if(sndsize>-1) /* permit zero-length datagrams */
		{
                	nsnd=sendto(uclit->sockfd, sndbuff, sndsize, 0, &uclit->addrSERV, svrlen); /* flags: MSG_CONFIRM, MSG_DONTWAI */
                        if(nsnd<0){
                        	printf("%s: Fail to sendto() data to UDP server '%s:%d', Err'%s'\n",
                                             __func__, inet_ntoa(uclit->addrSERV.sin_addr), ntohs(uclit->addrSERV.sin_port), strerror(errno));
                                            //GO ON... return;
			}
                        else if(nsnd!=sndsize) {
                        	printf("%s: WARNING! Send %d of total %d bytes to '%s:%d'!\n",
                                            __func__, nsnd, sndsize, inet_ntoa(uclit->addrSERV.sin_addr), ntohs(uclit->addrSERV.sin_port) );
			}

		}

		/* ---- UDP Client: Blocking recvfrom() ---- */
                /*  MSG_DONTWAIT (since Linux 2.2)
                 *  Enables  nonblocking  operation; if the operation would block, EAGAIN or EWOULDBLOCK is returned.
                 */
		sndlen=sizeof(addrSENDER); /*  Before the call, it should be initialized to adddrSENDR */
		nrcv=recvfrom(uclit->sockfd, rcvbuff, sizeof(rcvbuff), MSG_CMSG_CLOEXEC, &addrSENDER, &sndlen); /* MSG_DONTWAIT, MSG_ERRQUEUE */
		/* TODO: What if sndlen > sizeof( addrSENDER ) ! */
		if(sndlen>sizeof(addrSENDER))
			printf("%s: sndlen > sizeof(addrSENDER)!\n",__func__);
		/* Datagram sockets in various domains permit zero-length datagrams */
		if(nrcv<0) {
			printf("%s: Fail to call recvfrom(), Err'%s'\n", __func__, strerror(errno));
			switch(errno) {
				case EAGAIN:  /* Note: EAGAIN == EWOULDBLOCK */
					printf("%s: errno==EAGAIN or EWOULDBLOCK.\n",__func__);
					break;
				case ECONNREFUSED: /* Usually the counter part is NOT ready */
					printf("%s: errno==ECONNREFUSED.\n",__func__);
				     	break;
				default:
					return -2;
			}

                        /***
                         * 1. The socket is marked nonblocking and the receive operation would block,
                         * 2. or a receive timeout had been set and the timeout expired before data was received.
                         */

			/* NOTE: too fast calling recvfrom() will interfere other UDP sessions...? */
			//BLOCKED: usleep(5000);
			continue;
		}

		/* Datagram sockets in various domains permit zero-length datagrams. */
                //else if(nrcv==0) {
                //        usleep(10000);
                //        continue;
                //}

		/* ELSE: (nrcv>=0) */
		#if 0
                printf("%s: %d bytes from server '%s:%d'.\n",
                           __func__, nrcv, inet_ntoa(addrSENDER.sin_addr), ntohs(addrSENDER.sin_port));
		#endif

                /* Loop Session gap */
                usleep(5000); //500000);
	}
}


/*-------------------------------------------------------------------
A TEST callback routine for UPD client.
1. Tasks in callback function should be simple and short, normally
   just to pass out received data to the Caller, and take in data to
   the UDP client to send out.
2. In this TESTcallback, the client just bounce backe received count
   number, and let the server do 'count++' job. If received count
   number is NOT as expected svrcount=count+1, then a packet must have
   been dropped/missed.

@rcvAddr:       Server/Sender address, from which rcvData was received.
@rcvData:       Data received from rcvAddr
@rcvSize:       Data size of rcvData.

@sndBuff:       Data to send to UDP server
@sndSize:       Data size of sndBuff

Return:
	0	OK
	<0	Fails
--------------------------------------------------------------------*/
int inet_udpClient_TESTcallback( const struct sockaddr_in *rcvAddr, const char *rcvData, int rcvSize,
                                        	  		          char *sndBuff, int *sndSize )
{
	int  psize=EGI_MAX_UDP_PDATA_SIZE; /* EGI_MAX_UDP_PDATA_SIZE=1024;  UDP packet payload size, in bytes. */
	char buff[psize];
	static unsigned int count=0;  	   /* Counter for sended packets, start from 0, the  server do the 'count+1' job! */
	unsigned int svrcount=0;	   /* count from the server */
	char *pt=NULL;
	int miss=0;			   /* number of droped or delayed packets */

	/* Only received data, Init. rcvSize=0  */
	if( rcvSize<0 ) {
		return 0;
	}

	/* ---- 1. Take data from EGI_UDP_CLIT ---- */
	/* Get data */
	if( rcvSize>0 ) {
		memcpy(buff, rcvData, rcvSize);  /* Server set EOF */
                printf("%d bytes from server '%s:%d' with data: '%s'\n",
                             	rcvSize, inet_ntoa(rcvAddr->sin_addr), ntohs(rcvAddr->sin_port), buff);

		/* Extract count from the server, the server do the 'count+1' job! */
        	pt=strstr(buff,"=");
        	if(pt!=NULL) {
                  	svrcount=strtoul(pt+1,NULL,10);

			/* Check drops */
			if(svrcount != count+1)
				miss++;

			/* Bounce back to server, who will do the 'count+1' job. */
			count=svrcount;

			printf("Received svrcount=%d, --- miss:%d ---\n", svrcount,miss);
		}
		/* ELSE: re_send count */
	}

	/* Process/Prepare send data to EGI_UDP_CLIT */
        //Set EOF instead, memset(buff,0,psize);
        /* For test purpose only, to set an EOF for string. so we may NOT need to memset(buff,0,psize). */
        if(psize>1024)
                buff[1024]=0;
        else
                buff[psize-1]=0; /* set EOF */

        sprintf(buff,"Hello from client, count=%d. --- miss:%d ---", count, miss); /* start from 0 */

	/* ---- 2. Pass data to UDP client ---- */
	*sndSize=psize;
	memcpy(sndBuff, buff, psize);

	return 0;
}


/*-----------------------------------------------------
Create a TCP server, socket default in BLOCK mode.

@strIP:	TCP IP address.
	If NULL, auto selected by htonl(INADDR_ANY).
@Port:  Port Number.
	If 0, auto selected by system.
@domain:  If ==6, IPv6, otherwise IPv4.
	TODO: IPv6 connection.
Return:
	Pointer to an EGI_TCP_SERV	OK
	NULL				Fails
-------------------------------------------------------*/
EGI_TCP_SERV* inet_create_tcpServer(const char *strIP, unsigned short port, int domain)
{
	EGI_TCP_SERV *userv=NULL;
	struct timeval snd_timeout={TCP_SERV_SNDTIMEO,0}; /* Default time out for send */
	struct timeval rcv_timeout={TCP_SERV_RCVTIMEO,0}; /* Default time out for receive and accept() */

	/* Calloc EGI_TCP_SERV */
	userv=calloc(1,sizeof(EGI_TCP_SERV));
	if(userv==NULL)
		return NULL;

	/* Default backlog */
	userv->backlog=MAX_TCP_BACKLOG;

	/*--------------------------------------------------
       	AF_INET             IPv4 Internet protocols
       	AF_INET6            IPv6 Internet protocols
       	AF_IPX              IPX - Novell protocols
       	AF_NETLINK          Kernel user interface device
       	AF_X25              ITU-T X.25 / ISO-8208 protocol
       	AF_AX25             Amateur radio AX.25 protocol
       	AF_ATMPVC           Access to raw ATM PVCs
       	AF_APPLETALK        AppleTalk
       	AF_PACKET           Low level packet interface
       	AF_ALG              Interface to kernel crypto API
	---------------------------------------------------*/
	#if 0
	if(domain==6)
		domain=AF_INET6;
	else
		domain=AF_INET;
	#else
	domain=AF_INET;
	#endif

	/* Create TCP socket fd */
	userv->sockfd=socket(domain, SOCK_STREAM|SOCK_CLOEXEC, 0);
	if(userv->sockfd<0) {
		printf("%s: Fail to create stream socket, '%s'\n", __func__, strerror(errno));
		free(userv);
		return NULL;
	}

	/* 2. Init. sockaddr */
	userv->addrSERV.sin_family=domain;
	if(strIP==NULL)
		userv->addrSERV.sin_addr.s_addr=htonl(INADDR_ANY);
	else
		userv->addrSERV.sin_addr.s_addr=inet_addr(strIP);
	userv->addrSERV.sin_port=htons(port);

	/* 3. Bind socket with sockaddr(Assign sockaddr to socekt) */
	if( bind(userv->sockfd,(struct sockaddr *)&(userv->addrSERV), sizeof(userv->addrSERV)) < 0) {
		printf("%s: Fail to bind sockaddr, Err'%s'\n", __func__, strerror(errno));
		free(userv);
		return NULL;
	}

        /* 4. Set default SND/RCV timeout, recvfrom() and sendto() can set flag MSG_DONTWAIT.  */
        if( setsockopt(userv->sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&snd_timeout, sizeof(snd_timeout)) <0 ) {
                printf("%s: Fail to setsockopt for snd_timeout, Err'%s'\n", __func__, strerror(errno));
		free(userv);
		return NULL;
        }
        if( setsockopt(userv->sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&rcv_timeout, sizeof(rcv_timeout)) <0 ) {
                printf("%s: Fail to setsockopt for recv_timeout, Err'%s'\n", __func__, strerror(errno));
		free(userv);
		return NULL;
        }
	printf("%s: An TCP server created at %s:%d.\n", __func__, inet_ntoa(userv->addrSERV.sin_addr), ntohs(userv->addrSERV.sin_port));

	/* 5. If port==0, Get system allocated port number */
	if(userv->addrSERV.sin_port == 0) {
	        socklen_t clen=sizeof(userv->addrSERV);
        	if( getsockname(userv->sockfd, (struct sockaddr *)&(userv->addrSERV), &clen)!=0 ) {
                	printf("%s: Fail to getsockname, Err'%s'\n", __func__, strerror(errno));
	        }
		else
			printf("%s: system auto. allocates port number: %d\n", __func__, ntohs(userv->addrSERV.sin_port));
	}

	/* OK */
	return userv;
}


/* ------------------------------------------
Close a TCP SERV and release resoruces.

Return:
	0	OK
	<0	Fails
--------------------------------------------*/
int inet_destroy_tcpServer(EGI_TCP_SERV **userv)
{
	if(userv==NULL || *userv==NULL)
		return 0;

	/* Make sure all sessions/clients have been safely closed/disconnected! */

        /* Close main sockfd */
        if(close((*userv)->sockfd)<0) {
                printf("%s: Fail to close datagram socket, Err'%s'\n", __func__, strerror(errno));
		return -1;
	}

	/* Free mem */
	free(*userv);
	*userv=NULL;

	return 0;
}


/*-----------------------------------------------------------------
TCP service routines:
1. Accept new clients and put to session list, then start a thread
   to precess its C/S session.
2. Check and recount session list, //sort active/living sessions.
   

TODO.
1. IPv6 connection.
2. Use select/poll to monitor IO fd.
3. Session processing: Multi_threads OR Select/Poll.
   One Session Thread for One Client Mode    (Multi_threads server)
   One Session Thread for Muti Clients Mode  (Select/Poll server )

@userv: Pointer to an EGI_TCP_SERV.

Return:
	0	OK
	<0	Fails
-----------------------------------------------------------------*/
int inet_tcpServer_routine(EGI_TCP_SERV *userv)
{
	int i;
	int csfd=0;			   /* fd for c/s */
	struct sockaddr_in addrCLIT={0};   /* Client for recvfrom() */
	socklen_t addrLen;
	int index=0;			   /* index as of userv->session[index] */

	/* Check input */
	if( userv==NULL )
		return -1;
	if( userv->session_handler==NULL ) {
		printf("%s: session_handler is NOT defined!\n", __func__);
		return -2;
	}

	/* Start to listen */
	if( listen(userv->sockfd, userv->backlog)<0 ) {
		printf("%s: Fail to listen() sockfd, Err'%s'\n", __func__, strerror(errno));
		return -3;
	}

	/* TODO & TBD: Or launch a Select/Poll session process at very beginning. */

	/*** Loop service processing: ***/
	printf("%s: Start TCP server routine, loop processing...\n", __func__);
	index=0; /* init. index */

	userv->active=true; /* Set flag on */
	while( userv->cmd !=1 ) {
		/* Re_count clients, and get an available userv->sessions[] for next new client. */
		userv->ccnt=0;
		for(i=0; i<EGI_MAX_TCP_CLIENTS; i++) {
			if( index<0 && userv->sessions[i].alive==false )
				index=i;  /* availble index for next session */
			if( userv->sessions[i].alive==true )
				userv->ccnt++;
		}
//		printf("%s: Recount active sessions userv->ccnt=%d\n", __func__, userv->ccnt);

 #if 0	////////////////* Re_count clients and Re_arrange userv->sessions[], some sessions might end. */
		int j;
		userv->ccnt=0; j=0;
		for(i=0; i<EGI_MAX_TCP_CLIENTS; i++) {
			if( userv->sessions[i].alive==false ) {
				/* Find next active session and swap */
				if(j<i+1)j=i+1;
				for(; j<EGI_MAX_TCP_CLIENTS; j++) {
					if( userv->sessions[j].alive==true ) {
						/* swap */
						userv->sessions[i]=userv->sessions[j];
						userv->sessions[j].alive=false; /* or clear it */
						userv->ccnt++;
					}
				}
				/* Search to the END */
				if(j>=EGI_MAX_TCP_CLIENTS)
					break;
			}
			else {
				userv->ccnt++;
			}
		}
		printf("%s: Recount and sort sessions, userv->ccnt=%d\n", __func__, userv->ccnt);
 #endif ///////////////////////////////////

		/* Max. clients */
		//if(userv->ccnt==EGI_MAX_TCP_CLIENTS) {
		if(index<0) {
			printf("%s: Clients number hits Max. limit of %d!\n",__func__, EGI_MAX_TCP_CLIENTS);
			tm_delayms(500);
			continue;
		}

		/* Accept clients */
		addrLen=sizeof(struct sockaddr);
		/* NONBLOCK NO use for accept4()!  flags are applied on the new file descriptor!! */
		//csfd=accept4(userv->sockfd, (struct sockaddr *)&addrCLIT, &addrLen, SOCK_NONBLOCK|SOCK_CLOEXEC);
		csfd=accept(userv->sockfd, (struct sockaddr *)&addrCLIT, &addrLen); /* timeout same as rcv_timeout set in socket() ! */
		if(csfd<0) {
			switch(errno) {
                                #if(EWOULDBLOCK!=EAGAIN)
                                case EWOULDBLOCK:
                                #endif
				case EAGAIN:
				case EINTR:
					tm_delayms(10); /* NONBLOCKING */
					continue;  /* ---> continue to while() */
					break;
				default:
					printf("%s: Fail to accept a new client, errno=%d Err'%s'.  continue...\n",
												__func__, errno, strerror(errno));
					tm_delayms(20);
					/* TODO: End routine if it's a deadly failure!  */
					continue;  /* ---> whatever, continue to while() */
			}
		}

		/****** NOTE:
		 * 1. man "On  Linux, the new socket returned by accept() does not inherit file status flags such as O_NONBLOCK and
		 *    O_ASYNC from the listening socket."
		 * 2. However, TEST shows the new socket inherits SO_RCVTIMEO TimeOut values!
                 ***/

		/* Proceed for a new client ...  */
		userv->sessions[index].sessionID=index;
		userv->sessions[index].csFD=csfd;
		userv->sessions[index].addrCLIT=addrCLIT;

		/* Start a thread to process C/S ssesion, TODO & TBD: Or A Select/Poll session process at beginning. */
		if( pthread_create( &userv->sessions[index].csthread, NULL, userv->session_handler,
									(void *)(userv->sessions+index)) <0 ) {
			printf("%s: Fail to start C/S session thread!\n", __func__);
			close(csfd);
		}
		else {
			/* pthread_create() returns 0 does NOT necessarily means the thread is running!?f
			 * Let thread to set the flag??!
			 */
			userv->sessions[index].alive=true;

			/* Note: At this point some sessions might end! */
			userv->ccnt++;

			printf("%s: Accept a new TCP client from %s:%d, totally %d clients now.\n",
					__func__, inet_ntoa(addrCLIT.sin_addr), ntohs(addrCLIT.sin_port), userv->ccnt );

			/* Reset index to -1, as it was used. */
			index=-1;
		}
	}

	userv->active=false; /* Set flag off */
	return 0;
}


/* (TEST) -------------  A detached thread function  -------------------

A TEST TCP server session handler runs as a detached thread,
in One_Thread_For_One_Client mode.

Note:
1. This TEST session handler works with a TCP client that runs
    TEST_inet_tcpClient_routine().
2. It works in a Ping-Pong mode:
   It Waits a request MSGDATA from a client, then reply back with
   Err/repeats msg, timestamp and other data. The reply packet are
   in the same size of the request MSGDATA!
   For the request MSGDATA, it first recvs MSG part, then DATA part.
3. Size of request MSGDATA shall be at least sizeof(EGI_INET_MSGDATA)
4. It means the client CAN adjust the size of MSGDATA packet!

@arg: Pointer to pass a EGI_TCP_SERV_SESSION.
------------------------------------------------------------------------*/
void* TEST_tcpServer_session_handler(void *arg)
{
	int i;
	EGI_TCP_SERV_SESSION *session=(EGI_TCP_SERV_SESSION *)arg;
	if(session==NULL)
		return (void *)-1;

	struct sockaddr_in *addrCLIT=&session->addrCLIT;
	int sfd=session->csFD;
	struct timeval tm_stamp;
	EGI_INET_MSGDATA *msgdata=NULL;

	int nsnd,nrcv;		/* returned bytes of each call  */
	int sndsize;		/* accumulated size */
	int rcvsize;

	int msgsize=sizeof(EGI_INET_MSGDATA);   /* MSGDATA without data */
	int datasize;			/* size of MSGDATA.data[] */
        int packsize; //msgsize+datasize;       /* packsize=msgsize+datasize */

        /* Err and Repeats counter */
        int sndErrs=0;
        int sndRepeats=0;
        int sndZeros=0;
        int sndEagains=0;
        int rcvErrs=0;
        int rcvRepeats=0;
	int rcvEagains=0;

	/* Detach thread */
	if(pthread_detach(pthread_self())!=0) {
		printf("Session_%d: Fail to detach thread for TCP session with client '%s:%d'.\n",
						session->sessionID, inet_ntoa(addrCLIT->sin_addr), ntohs(addrCLIT->sin_port) );
		// go on...
	}

	msgdata=inet_msgdata_create(0);  /* Without data[] */
	if(msgdata==NULL) {
		printf("Fail to create a MSGDATA!\n");

		session->alive=false;
		close(sfd);
		session->csFD=-1;
		pthread_exit(NULL);
	}


   /* Session loop process */
   printf("Session_%d: Enter C/S session processing loop ...\n", session->sessionID);
   while( session->cmd !=1 ) {

	/* 1. Wait for a request MSGDATA from client */
	/*** Note:
	 * A. Test shows that recv() without MSG_WAITALL may needs several calls to finish receiving a complete packet.
	 *    1.1 It also depends on timeout, a big timeout value results in less call to complete.
	 *    1.2 Size of the data packet also applys, small size is much easier to be accomplished in one time.
         * B. Test packsize<32KB shows that recv( ) with MSG_WAITALL in most case finishs in one round, rarely in 3-4 rounds when
         *    network is jammed and/or CPU load too high.
	 */
   	rcvsize=0;
	packsize=msgsize;  /* First set packsize as msgsize */
    	while( rcvsize < packsize ) {
		/* Receive MSG, then DATA */
		nrcv=recv(sfd, (void *)msgdata+rcvsize, packsize-rcvsize, MSG_WAITALL); /* Wait all data, but still may fail? see man */
		if(nrcv>0) {
			/* count rcvsize  */
			rcvsize += nrcv;

			/* (A) Receive MSGDATA Head first */
			if( rcvsize==msgsize) {
				/* ----- Realloc msgdata.  TODO: If same size? ----- */
				datasize=inet_msgdata_getDataSize(msgdata);
				if( inet_msgdata_reallocData(&msgdata, datasize)==0 ) {
					packsize += datasize;  /* Increase packsize */
				}
			}
			/* (B) OK! Finishing recv() MSG+DATA */
			else if( rcvsize==packsize) {
				break; /* OK */
			}
			/* (C) Not finish yet */
			else if( rcvsize < packsize ) {
				rcvRepeats ++;
        	        	printf("Session_%d: rcvsize(%d) < msgsize(%d) bytes!\n", session->sessionID, rcvsize, msgsize);
				continue; /* -----> continue to recv remaining data */
			}
			/* (D) Impossible */
			else { /* rcvsize > msgsize, '>' seems impossible! */
				rcvErrs ++;
        	        	printf("Session_%d: recv MSG data error! rcvsize(%d) > msgsize(%d) bytes!\n",session->sessionID, rcvsize, msgsize);
			}
	        }
        	else if(nrcv==0) {
               		printf("Session_%d: The client ends session! exit handler thread now!\n", session->sessionID);
			/* Reset session and quit thread */
			session->alive=false;
			close(sfd);
			pthread_exit(NULL);
        	}
	        else {  /* nrcv<0 */
       	        	switch(errno) {
                                #if(EWOULDBLOCK!=EAGAIN)
       	                        case EWOULDBLOCK:
               	                #endif
               	        	case EAGAIN:
					rcvEagains ++;
					usleep(5000);
					continue; /* ---> continue to recv() */
                                	break;
        	                default: {
					rcvErrs ++;
		        	        printf("Session_%d: recv() Err'%s'\n", session->sessionID, strerror(errno));
					usleep(10000);
					continue; /* ---> continue to recv() */
				}
			}
		}
   	}

	/* Parse MSG */
	// printf("Session_%d: Client Msg: %s\n", session->sessionID, msgdata->msg.cmsg);

	/* 2.  Prepare reply MSGDATA, same size as request MSGDATA! */
	/* 2.1 Put MSG head */
	snprintf(msgdata->msg.cmsg, MSGDATA_CMSG_SIZE-1,
		 "SVR Msg: sndErrs=%d, sndRepeats=%d, sndEagains=%d, sndZeros=%d, rcvErrs=%d, rcvRepeats=%d, rcvEagains=%d",
						sndErrs, sndRepeats, sndEagains, sndZeros, rcvErrs, rcvRepeats, rcvEagains);
	/* 2.2 Put timeval at msgdata->data[] */
	gettimeofday(&tm_stamp, NULL);
	msgdata->msg.tmstamp=tm_stamp;

	/* 2.3 Put some data, same datasize as request MSGDATA's */
	if( datasize > 100-1 ) {
		for(i=0;i<100;i++)
			msgdata->data[i]=i;
	}

	/* 3. Send Msg+Data out, same size as request MSGDATA! */
   	sndsize=0;
   	while( sndsize < packsize ) {
		//nsnd=write(sfd, buff, datasize);
        	nsnd=send(sfd, (void *)msgdata+sndsize, packsize-sndsize, 0 ); /* In connectiong_mode, flags: MSG_CONFIRM, MSG_DONTWAIT */
		if(nsnd>0) {
			/* Count sndsize */
			sndsize += nsnd;

			if( sndsize==packsize ) {
				/* OK */
			}
			else if( sndsize < packsize ) {
				sndRepeats ++;
				printf("%s: sndsize < packsize! continue to send.\n",__func__);
				continue; /* ------> continue to sendto() */
			}
			else { /* sndsize > packsize, impossible! */
				sndErrs ++;
				printf("%s: sndsize < packsize! continue to send.\n",__func__);
			}
		}
		else if(nsnd<0) {
			switch(errno) {
				#if(EWOULDBLOCK!=EAGAIN)
				case EWOULDBLOCK:
				#endif
				case EAGAIN:  //EWOULDBLOCK, for datastream it woudl block */
					sndEagains ++;
					printf("Session_%d: Fail to send data to client '%s:%d', Err'%s'\n",
						 session->sessionID, inet_ntoa(addrCLIT->sin_addr), ntohs(addrCLIT->sin_port), strerror(errno));
					continue; /* ------> continue to sendto() */
					break;
				case EPIPE:
					printf("Session_%d: An EPIPE signals that client at '%s:%d' quits the session! End session handler now... \n",
							 session->sessionID, inet_ntoa(addrCLIT->sin_addr), ntohs(addrCLIT->sin_port) );

					/* Reset session and quit thread */
					session->alive=false;
					close(sfd);
					pthread_exit(NULL);
					break;
				default: {
					sndErrs ++;
		        		printf("Session_%d: Fail to send data to client '%s:%d', Err'%s'\n",
                	                       session->sessionID, inet_ntoa(addrCLIT->sin_addr), ntohs(addrCLIT->sin_port), strerror(errno));
					continue; /* ------> continue to sendto() */
				}
			}
		}
		else /* nsnd==0 */ {
			sndZeros ++;
			printf("Session_%d: nsnd==0!\n",session->sessionID);
			continue; /* ------> continue to sendto() */
		}

	} /* End of while() send() */

	/* Time delay */
	/* BY CLIENT */
	//usleep(50000);

   }  /* End of session while() */

	session->cmd=0;
	session->alive=false;
	close(sfd);
	session->csFD=-1;

	return (void *)0;
}


/*-----------------------------------------------------
Create a TCP client.

@strIP:	Server TCP IP address.
@Port:  Server Port Number.
@domain:  If ==6, IPv6, otherwise IPv4.
	TODO: IPv6 connection.
Return:
	Pointer to an EGI_TCP_CLIT	OK
	NULL				Fails
-------------------------------------------------------*/
EGI_TCP_CLIT* inet_create_tcpClient(const char *servIP, unsigned short servPort, int domain)
{
	EGI_TCP_CLIT *uclit=NULL;
        struct timeval snd_timeout={TCP_CLIT_SNDTIMEO,0}; /* Default time out for send */
        struct timeval rcv_timeout={TCP_CLIT_RCVTIMEO,0}; /* Default time out for receive and accept() */

	/* Check input */
	if(servIP==NULL)
		return NULL;

	/* Calloc EGI_TCP_CLIT */
	uclit=calloc(1,sizeof(EGI_TCP_CLIT));
	if(uclit==NULL)
		return NULL;

	/* Set domain */
	#if 0
	if(domain==6)
		domain=AF_INET6;
	else
		domain=AF_INET;
	#else
	domain=AF_INET;
	#endif

	/* 1. Create TCP socket fd */
	uclit->sockfd=socket(domain, SOCK_STREAM|SOCK_CLOEXEC, 0);
	if(uclit->sockfd<0) {
		printf("%s: Fail to create stream socket, '%s'\n", __func__, strerror(errno));
		free(uclit);
		return NULL;
	}

	/* 2. Init. sockaddr */
	uclit->addrSERV.sin_family=domain;
	uclit->addrSERV.sin_addr.s_addr=inet_addr(servIP);
	uclit->addrSERV.sin_port=htons(servPort);

        /* 3. Set default SND/RCV timeout,   recvfrom() and sendto() can set flag MSG_DONTWAIT.  */
        if( setsockopt(uclit->sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&snd_timeout, sizeof(snd_timeout)) <0 ) {
                printf("%s: Fail to setsockopt for snd_timeout, Err'%s'\n", __func__, strerror(errno));
		free(uclit);
		return NULL;
        }
        if( setsockopt(uclit->sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&rcv_timeout, sizeof(rcv_timeout)) <0 ) {
                printf("%s: Fail to setsockopt for recv_timeout, Err'%s'\n", __func__, strerror(errno));
		free(uclit);
		return NULL;
        }

	/* 4. Connect to the server */
	if( connect(uclit->sockfd, (struct sockaddr *)&uclit->addrSERV, sizeof(struct sockaddr)) <0 ) {
                printf("%s: Fail to connect() to the server, Err'%s'\n", __func__, strerror(errno));
		free(uclit);
		return NULL;
	}

	/* Try to getsockname */
        socklen_t clen=sizeof(uclit->addrME);
        if( getsockname(uclit->sockfd, (struct sockaddr *)&(uclit->addrME), &clen)!=0 ) {
                printf("%s: Fail to getsockname for client, Err'%s'\n", __func__, strerror(errno));
        }
	else
		printf("%s: An EGI TCP client created at ???%s:%d, targeting to the server at %s:%d.\n",
			 __func__, inet_ntoa(uclit->addrME.sin_addr), ntohs(uclit->addrME.sin_port),
				   inet_ntoa(uclit->addrSERV.sin_addr), ntohs(uclit->addrSERV.sin_port)	  );

	/* OK */
	return uclit;
}


/* ------------------------------------------
Close an TCP Client and release resoruces.

Return:
	0	OK
	<0	Fails
--------------------------------------------*/
int inet_destroy_tcpClient(EGI_TCP_CLIT **uclit)
{
	if(uclit==NULL || *uclit==NULL)
		return 0;

	/* Make sure its session has been safely closed! */

        /* Close sockfd */
        if(close((*uclit)->sockfd)<0) {
                printf("%s: Fail to close datagram socket, Err'%s'\n", __func__, strerror(errno));
		return -1;
	}

	/* Free mem */
	free(*uclit);
	*uclit=NULL;

	return 0;
}


/*( TEST ) --------------------------------------------------------
A TCP client rountine for TEST purpose.

Note:
1. This TCP client routine works with a TCP server tat runs
   TEST_tcpServer_session_handler().
2. Routine works in a Ping-Pong mode:
   Send a request MSGDATA to server, and wait for reply. then receive
   reply from the server and parse it.
   The server SHALL reply a MSGDATA just in the same size of the
   request MSGDATA.
3. Size of request MSGDATA shall be at least sizeof(EGI_INET_MSGDATA)
4. Log errors and repeats.

@uclit:		Pointer to an EGI_TCP_CLIT
@packsize:	Each packet size, whole size of a MSGDATA.
		>=sizeof(EGI_INET_MSGDATA)
@gms:		Interval sleep in ms.

Return:
	>0	Peer closed socket.
	0	OK
	<0	Fails
-------------------------------------------------------------------*/
int TEST_tcpClient_routine(EGI_TCP_CLIT *uclit, int packsize, int gms)
{
        EGI_CLOCK eclock={0};           /* For TR/TX speed test */
        EGI_CLOCK eclock2={0};          /* For LOG write */
	int tmms;

	EGI_INET_MSGDATA *msgdata=NULL; /* MSG+DATA, use same MSGDATA for send and recv  */

        int nsnd,nrcv;          /* returned bytes for each call  */
        int sndsize;            /* accumulated size */
        int rcvsize;

	// packsize=msgsize+datasize
        int msgsize=sizeof(EGI_INET_MSGDATA);      /* MSGDATA withou data[] */
	int datasize;  		/* Expected datasize =packsize-msgsize */

	/* Err and Repeats counter */
	int sndErrs=0;
	int sndRepeats=0;
	int sndZeros=0;
	int sndEagains=0;
	int rcvErrs=0;
	int rcvRepeats=0;
	int rcvEagains=0;

	if(uclit==NULL)
		return -1;

	/* Packsize limit */
	if(packsize<msgsize)
		packsize=msgsize;

	/* Create a MSGDATA */
	datasize=packsize-msgsize;
	msgdata=inet_msgdata_create(datasize);
	if(msgdata==NULL)
		return -2;

	/* Start eclock2 for logging data */
	egi_clock_start(&eclock2);

	/* Routine loop ... */
	while(1) {

		/* Start eclock for speed test */
		egi_clock_start(&eclock);

		/* DELAY: control speed */
		tm_delayms(gms);

	        /* 1. Prepare request Msg  */
	        sprintf(msgdata->msg.cmsg, "TCP client %d request for data.", getpid());
		inet_msgdata_updateTimeStamp(msgdata);  /* Put timestamp */

		/* 2. Send a packet to server to request data reply, the server will reply same size packet! */
		sndsize=0;
        	while( sndsize < packsize) {
			/* In connection_mode, flags: MSG_CONFIRM, MSG_DONTWAIT */
	        	nsnd=send(uclit->sockfd, (void *)msgdata+sndsize, packsize-sndsize, 0);
			if(nsnd>0) {
				/* Count sndsize */
				sndsize += nsnd;

				if(sndsize==packsize) {
					break; /* OK */
				}
				else if( sndsize < packsize ) {
					sndRepeats ++;
					EGI_PLOG(LOGLV_TEST," sndsize < packsize! continue to sendto() \n" );
					continue; /* ------> continue sendto() */
				}
				else { /* sndsize > msgsize, impossible! */
					sndErrs ++;
					EGI_PLOG(LOGLV_TEST,"sndsize >packsize, send data error!");
				}
			}
	        	else if(nsnd<0) {
        	        	switch(errno) {
	                                #if(EWOULDBLOCK!=EAGAIN)
        	                        case EWOULDBLOCK:
                	                #endif
                	        	case EAGAIN:  //EWOULDBLOCK, for datastream it woudl block */
						sndEagains ++;
                        	             	EGI_PLOG(LOGLV_TEST,"Fail to send MSGDATA to the server, Err'%s'.", strerror(errno));
						continue; /* ------> continue sendto() */
	                                	break;
		                        case EPIPE:
        		                     	EGI_PLOG(LOGLV_TEST,"An EPIPE signals that the server quits the session! quit now!\n");
						return 1;
	                                	break;
	        	                default: {
						sndErrs ++;
                        	             	EGI_PLOG(LOGLV_TEST,"Fail to send MSGDATA to the server, Err'%s'.", strerror(errno));
						continue; /* --------> continue to sendto() */
					}
                		}
         		}
	        	else  {  /* nsnd==0,  If send(size<=0), it will return 0!  */
				sndZeros ++;
        	        	EGI_PLOG(LOGLV_TEST," ----- nsnd==0! -----");
         		}
	   	}

		/* 3. Receive data from server, packsize SHALL be the same! */
		rcvsize=0;
		while( rcvsize < packsize ) {
			//nrcv=read(uclit->sockfd, buff, sizeof(buff));
			/*** set MSG_WAITALL to wait for all data. however, "the call may still return less data, if a signal is caught,
        	         *   an error or disconnect occurs, or the next data to be received is of a different type than that returned."(man)
			 *   stream socket:  all data is deamed as a stream, while as you can adjust datasize each time for recv()...
			 *   you may select datasize same as the sending end, but NOT necessary.
			 */
			nrcv=recv(uclit->sockfd, (void *)msgdata+rcvsize, packsize-rcvsize, MSG_WAITALL); /* self_defined datasize! */
			if(nrcv>0) {
				/* counter rcvsize */
				rcvsize += nrcv;

				if( rcvsize == packsize ) {
					/* OK */
				}
				else if( rcvsize < packsize ) {
					rcvRepeats ++;
					EGI_PLOG(LOGLV_TEST,"nrcv=%d < packsize=%d bytes", nrcv, packsize);

				}
				else  {  /* rcvsize > packsize, impossible! */
					rcvErrs ++;
					EGI_PLOG(LOGLV_TEST," --- rcvsize > packsize, impossible! --- ");
				}
			}
			else if(nrcv==0) {
				EGI_PLOG(LOGLV_TEST, "The server end session! quit now...");
				return 2;
			}
			else {  /* nrcv<0 */
	       	        	switch(errno) {
        	                        #if(EWOULDBLOCK!=EAGAIN)
       	        	                case EWOULDBLOCK:
               	        	        #endif
               	        		case EAGAIN:
						rcvEagains ++;
						usleep(5000);
						continue; /* ---> continue recv() */
                	                	break;
        	        	        default: {
						rcvErrs ++;
						EGI_PLOG(LOGLV_TEST, "recv(): Err'%s'.", strerror(errno));
						usleep(10000);
						continue; /* ---> continue to recv() */
					}
				}

				rcvErrs ++;
			}

		} /* End while() recv() */

		/* 4. Parse received MSGDATA */
		printf("Client_%d: %s\n", getpid(), msgdata->msg.cmsg);
		if( nrcv >= sizeof(EGI_INET_MSGDATA) ) {
			#if 0 /* TEST---- */
			printf("nl_datasize=%d, Datasize=%d\n", msgdata->msg.nl_datasize, inet_msgdata_getDataSize(msgdata));
			printf("Server time stamp: %ld.%06ld\n", msgdata->msg.tmstamp.tv_sec, msgdata->msg.tmstamp.tv_usec);
        		for(i=0;i<100;i++)
		                printf("%d ", msgdata->data[i]);
			printf("\n");
			#endif
		}
                //printf("sndErrs=%d, sndRepeats=%d, sndEagains=%d, sndZeros=%d, rcvErrs=%d, rcvRepeats=%d, rcvEagains=%d\n",
	        //                                        sndErrs, sndRepeats, sndEagains, sndZeros, rcvErrs, rcvRepeats, rcvEagains);

		/* Stop ECLOCK for Rx/Tx */
		egi_clock_stop(&eclock);
		tmms=egi_clock_readCostUsec(&eclock)/1000;

		/* Pause/Stop ECLOCK2 for LOG */
		if(egi_clock_pause(&eclock2)<0)printf("CLOCK2 fails!\n");
		if(eclock2.tm_cost.tv_sec > 60-1 ) {  /* Log every 60s */
		       EGI_PLOG(LOGLV_TEST, "Client %d: %s",getpid(), msgdata->msg.cmsg);  /* Server MSG with errs */
		       EGI_PLOG(LOGLV_TEST, "Client %d: sndErrs=%d, sndRepeats=%d, sndEagains=%d, sndZeros=%d, rcvErrs=%d, rcvRepeats=%d, rcvEagains=%d\n",
							getpid(), sndErrs, sndRepeats, sndEagains, sndZeros, rcvErrs, rcvRepeats, rcvEagains);
			/* Stop timing, for next round */
			egi_clock_stop(&eclock2);
		}
		egi_clock_start(&eclock2); /* Restart/start again  */

		/* This is instant speed, most time much bigger.., but some tmms is also much bigger.  */
		printf("Client_%d: Cost:%dms, RX:%dkBps \n\n", getpid(), tmms, packsize*1000/1024/tmms );

	}

	return 0;
}
