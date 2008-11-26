/*  EVEmu: EVE Online Server Emulator

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY except by those people which sell it, which
  are required to give you total support for your newly bought product;
  without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
	
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/


#include "../common/common.h"

#include <iostream>
using namespace std;
#include <string.h>
#include <stdio.h>
#include <iomanip>
using namespace std;

#include "TCPConnection.h"
#include "../common/timer.h"
#include "../common/packet_dump.h"
#include "../common/logsys.h"
#include "../common/MiscFunctions.h"

#ifdef FREEBSD //Timothy Whitman - January 7, 2003
	#define MSG_NOSIGNAL 0
#endif

#ifdef WIN32
InitWinsock winsock;
#endif

#define LOOP_GRANULARITY 3	//# of ms between checking our socket/queues

#define TCPN_DEBUG				0
#define TCPN_DEBUG_Console		0
#define TCPN_DEBUG_Memory		0
#define TCPN_LOG_RAW_DATA_OUT	0		//1 = info, 2 = length limited dump, 3 = full dump
#define TCPN_LOG_RAW_DATA_IN	0		//1 = info, 2 = length limited dump, 3 = full dump

//client version
TCPConnection::TCPConnection()
: ConnectionType(Outgoing),
  connection_socket(0),
  id(0),
  rIP(0),
  rPort(0)
{
	pState = TCPS_Ready;
	pFree = false;
	pEcho = false;
	recvbuf = NULL;
	sendbuf = NULL;
	pRunLoop = false;
	charAsyncConnect = 0;
	pAsyncConnect = false;
	m_previousLineEnd = false;
#if TCPN_DEBUG_Memory >= 7
	cout << "Constructor #2 on outgoing TCP# " << GetID() << endl;
#endif
}

//server version
TCPConnection::TCPConnection(int32 ID, SOCKET in_socket, int32 irIP, int16 irPort)
: ConnectionType(Incomming),
  connection_socket(in_socket),
  id(ID),
  rIP(irIP),
  rPort(irPort)
{
	pState = TCPS_Connected;
	pFree = false;
	pEcho = false;
	recvbuf = NULL;
	sendbuf = NULL;
	pRunLoop = false;
	charAsyncConnect = 0;
	pAsyncConnect = false;
	m_previousLineEnd = false;
#if TCPN_DEBUG_Memory >= 7
	cout << "Constructor #2 on incoming TCP# " << GetID() << endl;
#endif
}

TCPConnection::~TCPConnection() {
	FinishDisconnect();
	ClearBuffers();
	if (ConnectionType == Outgoing) {
		MRunLoop.lock();
		pRunLoop = false;
		MRunLoop.unlock();
		MLoopRunning.lock();
		MLoopRunning.unlock();
#if TCPN_DEBUG_Memory >= 6
		cout << "Deconstructor on outgoing TCP# " << GetID() << endl;
#endif
	}
#if TCPN_DEBUG_Memory >= 5
	else {
		cout << "Deconstructor on incomming TCP# " << GetID() << endl;
	}
#endif
	SafeDeleteArray(recvbuf);
	SafeDeleteArray(sendbuf);
	SafeDeleteArray(charAsyncConnect);
}

void TCPConnection::SetState(State_t in_state) {
	MState.lock();
	pState = in_state;
	MState.unlock();
}

TCPConnection::State_t TCPConnection::GetState() const {
	State_t ret;
	MState.lock();
	ret = pState;
	MState.unlock();
	return ret;
}

bool TCPConnection::GetSockName(char *host, uint16 *port)
{
	bool result=false;
	LockMutex lock(&MState);
	if (!Connected())
		return false;

	struct sockaddr_in local;

#ifdef WIN32
	int addrlen;
#else
	socklen_t addrlen;
#endif
	addrlen=sizeof(struct sockaddr_in);
	if (!getsockname(connection_socket,(struct sockaddr *)&local,&addrlen)) {
		unsigned long ip=local.sin_addr.s_addr;
		sprintf(host,"%d.%d.%d.%d",
			*(unsigned char *)&ip,
				*((unsigned char *)&ip+1),
				*((unsigned char *)&ip+2),
				*((unsigned char *)&ip+3));
		*port=ntohs(local.sin_port);

		result=true;
	}

	return result;
}

void TCPConnection::Free() {
	if (ConnectionType == Outgoing) {
		cout << "TCPConnection::Free() called on an Outgoing connection" << endl;
		return;
	}
#if TCPN_DEBUG_Memory >= 5
	cout << "Free on TCP# " << GetID() << endl;
#endif
	Disconnect();
	pFree = true;
}

bool TCPConnection::Send(const uint8* data, int32 size) {
	if (!Connected())
		return false;
	if (!size)
		return true;
	ServerSendQueuePushEnd(data, size);
	return true;
}

//TODO: this send queue could be rewritten to be an actual queue
//to avoid all these memcpy's
void TCPConnection::ServerSendQueuePushEnd(const uint8* head_data, int32 head_size, const uint8* data, int32 data_size) {
	int32 size = head_size + data_size;
	MSendQueue.lock();
	if (sendbuf == NULL) {
		sendbuf = new uint8[size+128];
		sendbuf_size = size+128;
		sendbuf_used = 0;
	}
	else if (size > (sendbuf_size - sendbuf_used)) {
		sendbuf_size += size + 1024;
		uint8* tmp = new uint8[sendbuf_size];
		memcpy(tmp, sendbuf, sendbuf_used);
		SafeDeleteArray(sendbuf);
		sendbuf = tmp;
	}
	memcpy(&sendbuf[sendbuf_used], head_data, head_size);
	sendbuf_used += head_size;
	memcpy(&sendbuf[sendbuf_used], data, data_size);
	sendbuf_used += data_size;
	MSendQueue.unlock();
}


void TCPConnection::ServerSendQueuePushEnd(const uint8* head_data, int32 head_size, uint8** data, int32 data_size) {
	int32 size = head_size + data_size;
	//This is not taking any advantage of the fact that we consume `data` until we put a real queue in here.
	//
	MSendQueue.lock();
	if (sendbuf == NULL) {
		sendbuf = new uint8[size+128];
		sendbuf_size = size+128;
		sendbuf_used = 0;
	}
	else if (size > (sendbuf_size - sendbuf_used)) {
		sendbuf_size += size + 1024;
		uint8* tmp = new uint8[sendbuf_size];
		memcpy(tmp, sendbuf, sendbuf_used);
		SafeDeleteArray(sendbuf);
		sendbuf = tmp;
	}
	memcpy(&sendbuf[sendbuf_used], head_data, head_size);
	sendbuf_used += head_size;
	memcpy(&sendbuf[sendbuf_used], *data, data_size);
	sendbuf_used += data_size;
	MSendQueue.unlock();
	SafeDeleteArray(*data);
}

void TCPConnection::ServerSendQueuePushEnd(const uint8* data, int32 size) {
	MSendQueue.lock();
	if (sendbuf == NULL) {
		sendbuf = new uint8[size+128];
		sendbuf_size = size+128;
		sendbuf_used = 0;
	}
	else if (size > (sendbuf_size - sendbuf_used)) {
		sendbuf_size += size + 1024;
		uint8* tmp = new uint8[sendbuf_size];
		memcpy(tmp, sendbuf, sendbuf_used);
		SafeDeleteArray(sendbuf);
		sendbuf = tmp;
	}
	memcpy(&sendbuf[sendbuf_used], data, size);
	sendbuf_used += size;
	MSendQueue.unlock();
}

void TCPConnection::ServerSendQueuePushEnd(uint8** data, int32 size) {
	MSendQueue.lock();
	if (sendbuf == NULL) {
		sendbuf = *data;
		sendbuf_size = size;
		sendbuf_used = size;
		MSendQueue.unlock();
		*data = NULL;
		return;
	}
	if (size > (sendbuf_size - sendbuf_used)) {
		sendbuf_size += size;
		uint8* tmp = new uint8[sendbuf_size];
		memcpy(tmp, sendbuf, sendbuf_used);
		SafeDeleteArray(sendbuf);
		sendbuf = tmp;
	}
	memcpy(&sendbuf[sendbuf_used], *data, size);
	sendbuf_used += size;
	MSendQueue.unlock();
	SafeDeleteArray(*data);
}

void TCPConnection::ServerSendQueuePushFront(uint8* data, int32 size) {
	MSendQueue.lock();
	if (sendbuf == 0) {
		sendbuf = new uint8[size];
		sendbuf_size = size;
		sendbuf_used = 0;
	}
	else if (size > (sendbuf_size - sendbuf_used)) {
		sendbuf_size += size;
		uint8* tmp = new uint8[sendbuf_size];
		memcpy(&tmp[size], sendbuf, sendbuf_used);
		SafeDeleteArray(sendbuf);
		sendbuf = tmp;
	}
	memcpy(sendbuf, data, size);
	sendbuf_used += size;
	MSendQueue.unlock();
}

bool TCPConnection::ServerSendQueuePop(uint8** data, int32* size) {
	bool ret;
	if (!MSendQueue.trylock())
		return false;
	if (sendbuf) {
		*data = sendbuf;
		*size = sendbuf_used;
		sendbuf = 0;
		ret = true;
	}
	else {
		ret = false;
	}
	MSendQueue.unlock();
	return ret;
}

bool TCPConnection::ServerSendQueuePopForce(uint8** data, int32* size) {
	bool ret;
	MSendQueue.lock();
	if (sendbuf) {
		*data = sendbuf;
		*size = sendbuf_used;
		sendbuf = 0;
		ret = true;
	}
	else {
		ret = false;
	}
	MSendQueue.unlock();
	return ret;
}

char* TCPConnection::PopLine() {
	char* ret;
	if (!MLineOutQueue.trylock())
		return 0;
	ret = (char*) LineOutQueue.pop();
	MLineOutQueue.unlock();
	return ret;
}

bool TCPConnection::LineOutQueuePush(char* line) {
	MLineOutQueue.lock();
	LineOutQueue.push(line);
	MLineOutQueue.unlock();
	return(false);
}


void TCPConnection::FinishDisconnect() {
	MState.lock();
	if (connection_socket != INVALID_SOCKET && connection_socket != 0) {
		if (pState == TCPS_Connected || pState == TCPS_Disconnecting || pState == TCPS_Disconnected) {
			bool sent_something = false;
			SendData(sent_something);
		}
		pState = TCPS_Closing;
		shutdown(connection_socket, 0x01);
		shutdown(connection_socket, 0x00);
#ifdef WIN32
		closesocket(connection_socket);
#else
		close(connection_socket);
#endif
		connection_socket = 0;
		rIP = 0;
		rPort = 0;
		ClearBuffers();
	}
	pState = TCPS_Disconnected;
	MState.unlock();
}

void TCPConnection::Disconnect() {
	MState.lock();
	if(pState == TCPS_Connected || pState == TCPS_Connecting) {
		pState = TCPS_Disconnecting;
	}
	MState.unlock();
}

bool TCPConnection::GetAsyncConnect() {
	bool ret;
	MAsyncConnect.lock();
	ret = pAsyncConnect;
	MAsyncConnect.unlock();
	return ret;
}

bool TCPConnection::SetAsyncConnect(bool iValue) {
	bool ret;
	MAsyncConnect.lock();
	ret = pAsyncConnect;
	pAsyncConnect = iValue;
	MAsyncConnect.unlock();
	return ret;
}

bool TCPConnection::ConnectReady() const {
	State_t s = GetState();
	if (s != TCPS_Ready && s != TCPS_Disconnected)
		return(false);
	return(ConnectionType == Outgoing);
}

void TCPConnection::AsyncConnect(const char* irAddress, int16 irPort) {
	SafeDeleteArray(charAsyncConnect);
	charAsyncConnect = new char[strlen(irAddress) + 1];
	strcpy(charAsyncConnect, irAddress);
	AsyncConnect((int32) 0, irPort);
}

void TCPConnection::AsyncConnect(int32 irIP, int16 irPort) {
	if (ConnectionType != Outgoing) {
		// If this code runs, we got serious problems
		// Crash and burn.
		cout << "TCPConnection::AsyncConnect() call on a Incomming connection object!" << endl;
		return;
	}
	if(!ConnectReady()) {
#if TCPN_DEBUG > 0
		printf("Trying to do async connect in invalid state %s\n", GetState());
#endif
		return;
	}
	MAsyncConnect.lock();
	if (pAsyncConnect) {
		MAsyncConnect.unlock();
#if TCPN_DEBUG > 0
		printf("Trying to do async connect when already doing one.\n");
#endif
		return;
	}
#if TCPN_DEBUG > 0
		printf("Start async connect.\n");
#endif
	pAsyncConnect = true;
	if(irIP != 0)
		SafeDeleteArray(charAsyncConnect);
	rIP = irIP;
	rPort = irPort;
	MAsyncConnect.unlock();
	if (!pRunLoop) {
		pRunLoop = true;
#ifdef WIN32
		_beginthread(TCPConnectionLoop, 0, this);
#else
		pthread_t thread;
		pthread_create(&thread, NULL, TCPConnectionLoop, this);
#endif
	}
	return;
}

bool TCPConnection::Connect(const char* irAddress, int16 irPort, char* errbuf) {
	if (errbuf)
		errbuf[0] = 0;
	int32 tmpIP = ResolveIP(irAddress);
	if (!tmpIP) {
		if (errbuf) {
#ifdef WIN32
			snprintf(errbuf, TCPConnection_ErrorBufferSize, "TCPConnection::Connect(): Couldnt resolve hostname. Error: %i", WSAGetLastError());
#else
			snprintf(errbuf, TCPConnection_ErrorBufferSize, "TCPConnection::Connect(): Couldnt resolve hostname. Error #%i: %s", errno, strerror(errno));
#endif
		}
		return false;
	}
	return ConnectIP(tmpIP, irPort, errbuf);
}

bool TCPConnection::ConnectIP(int32 in_ip, int16 in_port, char* errbuf) {
	if (errbuf)
		errbuf[0] = 0;
	if (ConnectionType != Outgoing) {
		// If this code runs, we got serious problems
		// Crash and burn.
		cout << "TCPConnection::Connect() call on a Incomming connection object!" << endl;
		return false;
	}
	MState.lock();
	if (ConnectReady()) {
		pState = TCPS_Connecting;
	} else {
		MState.unlock();
		SetAsyncConnect(false);
		return false;
	}
	MState.unlock();
	if (!pRunLoop) {
		pRunLoop = true;
#ifdef WIN32
		_beginthread(TCPConnectionLoop, 0, this);
#else
		pthread_t thread;
		pthread_create(&thread, NULL, TCPConnectionLoop, this);
#endif
	}

	connection_socket = INVALID_SOCKET;
    struct sockaddr_in	server_sin;
//    struct in_addr	in;

	if ((connection_socket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET || connection_socket == 0) {
#ifdef WIN32
		if (errbuf)
			snprintf(errbuf, TCPConnection_ErrorBufferSize, "TCPConnection::Connect(): Allocating socket failed. Error: %i", WSAGetLastError());
#else
		if (errbuf)
			snprintf(errbuf, TCPConnection_ErrorBufferSize, "TCPConnection::Connect(): Allocating socket failed. Error: %s", strerror(errno));
#endif
		SetState(TCPS_Ready);
		SetAsyncConnect(false);
		return false;
	}
	server_sin.sin_family = AF_INET;
	server_sin.sin_addr.s_addr = in_ip;
	server_sin.sin_port = htons(in_port);

	// Establish a connection to the server socket.
	if (_doConnect(connection_socket, &server_sin, sizeof (server_sin)) == SOCKET_ERROR) {
#ifdef WIN32
		if (errbuf)
			snprintf(errbuf, TCPConnection_ErrorBufferSize, "TCPConnection::Connect(): connect() failed. Error: %i", WSAGetLastError());
		closesocket(connection_socket);
#else
		if (errbuf)
			snprintf(errbuf, TCPConnection_ErrorBufferSize, "TCPConnection::Connect(): connect() failed. Error: %s", strerror(errno));
		close(connection_socket);
#endif
		connection_socket = 0;
		SetState(TCPS_Ready);
		SetAsyncConnect(false);
		return false;
	}
	int bufsize = 64 * 1024; // 64kbyte recieve buffer, up from default of 8k
	setsockopt(connection_socket, SOL_SOCKET, SO_RCVBUF, (char*) &bufsize, sizeof(bufsize));
#ifdef WIN32
	unsigned long nonblocking = 1;
	ioctlsocket(connection_socket, FIONBIO, &nonblocking);
#else
	fcntl(connection_socket, F_SETFL, O_NONBLOCK);
#endif

	SetEcho(false);
	ClearBuffers();
	
	rIP = in_ip;
	rPort = in_port;
	SetState(TCPS_Connected);
	SetAsyncConnect(false);
	return true;
}

//to clean up some ifdefs
int TCPConnection::_doConnect(int fd, struct sockaddr_in *addr, int addr_len) {
#ifdef WIN32
	return(connect(fd, (PSOCKADDR) addr, addr_len));
#else
	return(connect(fd, (struct sockaddr *) addr, addr_len));
#endif
}

void TCPConnection::ClearBuffers() {
	LockMutex lock1(&MSendQueue);
	LockMutex lock3(&MRunLoop);
	LockMutex lock4(&MState);
	SafeDeleteArray(recvbuf);
	SafeDeleteArray(sendbuf);
	
	char* line = 0;
	while ((line = LineOutQueue.pop()))
		SafeDeleteArray(line);
}

bool TCPConnection::CheckNetActive() {
	MState.lock();
	if (pState == TCPS_Connected || pState == TCPS_Disconnecting) {
		MState.unlock();
		return true;
	}
	MState.unlock();
	return false;
}

/* This is always called from an IO thread. Either the server socket's thread, or a 
 * special thread we create when we make an outbound connection. */
bool TCPConnection::Process() {
	char errbuf[TCPConnection_ErrorBufferSize];
	switch(GetState()) {
	case TCPS_Ready:
	case TCPS_Connecting:
		if (ConnectionType == Outgoing) {
			if (GetAsyncConnect()) {
				if (charAsyncConnect)
					rIP = ResolveIP(charAsyncConnect);
				ConnectIP(rIP, rPort);
			}
		}
		return(true);
	
	case TCPS_Connected:
		// only receive data in the connected state, no others...
		if (!RecvData(errbuf)) {
		    //struct in_addr	in;
			//in.s_addr = GetrIP();
			//cout << inet_ntoa(in) << ":" << GetrPort() << ": " << errbuf << endl;
			return false;
		}
		/* we break to do the send */
		break;
	
	case TCPS_Disconnecting: {
		//waiting for any sending data to go out...
		MSendQueue.lock();
		if(sendbuf) {
			if(sendbuf_used > 0) {
				//something left to send, keep processing...
				MSendQueue.unlock();
				break;
			}
			//else, send buffer is empty.
			SafeDeleteArray(sendbuf);
		} //else, no send buffer, we are done.
		MSendQueue.unlock();
	}
		/* Fallthrough */
	
	case TCPS_Disconnected:
		FinishDisconnect();
		MRunLoop.lock();
		pRunLoop = false;
		MRunLoop.unlock();
//		SetState(TCPS_Ready);	//reset the state in case they want to use it again...
		return(false);
	
	case TCPS_Closing:
		//I dont understand this state...
	
	case TCPS_Error:
		MRunLoop.lock();
		pRunLoop = false;
		MRunLoop.unlock();
		return(false);
	}
	
	/* we get here in connected or disconnecting with more data to send */
	
	bool sent_something = false;
	if (!SendData(sent_something, errbuf)) {
	    struct in_addr	in;
		in.s_addr = GetrIP();
		cout << inet_ntoa(in) << ":" << GetrPort() << ": " << errbuf << endl;
		return false;
	}
	
	return true;
}

bool TCPConnection::RecvData(char* errbuf) {
	if (errbuf)
		errbuf[0] = 0;
	if (!Connected()) {
		return false;
	}

	int	status = 0;
	if (recvbuf == 0) {
		recvbuf = new uint8[5120];
		recvbuf_size = 5120;
		recvbuf_used = 0;
		recvbuf_echo = 0;
	}
	else if ((recvbuf_size - recvbuf_used) < 2048) {
		uint8* tmpbuf = new uint8[recvbuf_size + 5120];
		memcpy(tmpbuf, recvbuf, recvbuf_used);
		recvbuf_size += 5120;
		SafeDeleteArray(recvbuf);
		recvbuf = tmpbuf;
		if (recvbuf_size >= MaxTCPReceiveBufferSize) {
			if (errbuf)
				snprintf(errbuf, TCPConnection_ErrorBufferSize, "TCPConnection::RecvData(): recvbuf_size >= MaxTCPReceiveBufferSize");
			return false;
		}
	}

    status = recv(connection_socket, (char *) &recvbuf[recvbuf_used], (recvbuf_size - recvbuf_used), 0);

    if (status >= 1) {
#if TCPN_LOG_RAW_DATA_IN >= 1
		struct in_addr	in;
		in.s_addr = GetrIP();
		CoutTimestamp(true);
		cout << ": Read " << status << " bytes from network. (recvbuf_used = " << recvbuf_used << ") " << inet_ntoa(in) << ":" << GetrPort();
		cout << endl;
	#if TCPN_LOG_RAW_DATA_IN == 2
		int32 tmp = status;
		if (tmp > 32)
			tmp = 32;
		DumpPacket(&recvbuf[recvbuf_used], status);
	#elif TCPN_LOG_RAW_DATA_IN >= 3
		DumpPacket(&recvbuf[recvbuf_used], status);
	#endif
#endif
		recvbuf_used += status;
		if (!ProcessReceivedData(errbuf))
			return false;
    }
	else if (status == SOCKET_ERROR) {
#ifdef WIN32
		if (!(WSAGetLastError() == WSAEWOULDBLOCK)) {
			if (errbuf)
				snprintf(errbuf, TCPConnection_ErrorBufferSize, "TCPConnection::RecvData(): Error: %i", WSAGetLastError());
			return false;
		}
#else
		if (!(errno == EWOULDBLOCK)) {
			if (errbuf)
				snprintf(errbuf, TCPConnection_ErrorBufferSize, "TCPConnection::RecvData(): Error: %s", strerror(errno));
			return false;
		}
#endif
	} else if (status == 0) {
		snprintf(errbuf, TCPConnection_ErrorBufferSize, "TCPConnection::RecvData(): Connection closed");
		return false;
	}

	return true;
}


bool TCPConnection::GetEcho() {
	bool ret;
	ret = pEcho;
	return ret;
}

void TCPConnection::SetEcho(bool iValue) {
	pEcho = iValue;
}

bool TCPConnection::ProcessReceivedData(char* errbuf) 
{
	if (errbuf != NULL)
		errbuf[0] = 0;
	if (recvbuf == NULL)
		return true;

#if TCPN_DEBUG_Console >= 4
	if (recvbuf_used) {
		cout << "Starting Processing: recvbuf=" << recvbuf_used << endl;
		DumpPacket(recvbuf, recvbuf_used);
	}
#endif

	for (int i=0; i < recvbuf_used; i++) 
	{
		if (GetEcho() && i >= recvbuf_echo) 
		{
			Send(&recvbuf[i], 1);
			recvbuf_echo = i + 1;
		}
		switch(recvbuf[i]) {
		case 0: { // 0 is the code for clear buffer
				if (i==0) {
					recvbuf_used--;
					recvbuf_echo--;
					memmove(recvbuf, &recvbuf[1], recvbuf_used);
					i = -1;
				} else {
					if (i == recvbuf_used) {
						SafeDeleteArray(recvbuf);
						i = -1;
					}
					else {
						uint8* tmpdel = recvbuf;
						recvbuf = new uint8[recvbuf_size];
						memcpy(recvbuf, &tmpdel[i+1], recvbuf_used-i);
						recvbuf_used -= i + 1;
						recvbuf_echo -= i + 1;
						SafeDeleteArray(tmpdel);
						i = -1;
					}
				}
#if TCPN_DEBUG_Console >= 5
				cout << "Removed 0x00" << endl;
				if (recvbuf_used) {
					cout << "recvbuf left: " << recvbuf_used << endl;
					DumpPacket(recvbuf, recvbuf_used);
				}
				else
					cout << "recbuf left: None" << endl;
#endif
				m_previousLineEnd = false;
				break;
			}
			case 10:
			case 13: // newline marker
			{
				char *line = NULL;
				if (i==0) { // empty line
					if(!m_previousLineEnd) {
						//char right before this was NOT a CR, report the empty line.
						line = new char[1];
						line[0] = '\0';
						m_previousLineEnd = true;
					} else {
						m_previousLineEnd = false;
					}
					recvbuf_used--;
					recvbuf_echo--;
					memcpy(recvbuf, &recvbuf[1], recvbuf_used);
					i = -1;
				} else {
					line = new char[i+1];
					memset(line, 0, i+1);
					memcpy(line, recvbuf, i);
#if TCPN_DEBUG_Console >= 3
					cout << "Line Out: " << endl;
					DumpPacket((uint8*) line, i);
#endif
					//line[i] = 0;
					uint8* tmpdel = recvbuf;
					recvbuf = new uint8[recvbuf_size];
					recvbuf_used -= i+1;
					recvbuf_echo -= i+1;
					memcpy(recvbuf, &tmpdel[i+1], recvbuf_used);
#if TCPN_DEBUG_Console >= 5
					cout << "i+1=" << i+1 << endl;
					if (recvbuf_used) {
						cout << "recvbuf left: " << recvbuf_used << endl;
						DumpPacket(recvbuf, recvbuf_used);
					}
					else
						cout << "recbuf left: None" << endl;
#endif
					SafeDeleteArray(tmpdel);
					i = -1;
					m_previousLineEnd = true;
				}
				
				
				if(line != NULL) {
					bool finish_proc = false;
					finish_proc = LineOutQueuePush(line);
					if(finish_proc)
						return(true);	//break early as requested by LineOutQueuePush
				}
				
				break;
			}
			case 8: // backspace
			{
				if (i==0) { // nothing to backspace
					recvbuf_used--;
					recvbuf_echo--;
					memmove(recvbuf, &recvbuf[1], recvbuf_used);
					i = -1;
				} else {
					uint8* tmpdel = recvbuf;
					recvbuf = new uint8[recvbuf_size];
					memcpy(recvbuf, tmpdel, i-1);
					memcpy(&recvbuf[i-1], &tmpdel[i+1], recvbuf_used-i);
					recvbuf_used -= 2;
					recvbuf_echo -= 2;
					SafeDeleteArray(tmpdel);
					i -= 2;
				}
				break;
				m_previousLineEnd = false;
			}
			default:
				m_previousLineEnd = false;
		}
	}
	if (recvbuf_used < 0)
		SafeDeleteArray(recvbuf);
	return true;
}

bool TCPConnection::SendData(bool &sent_something, char* errbuf) {
	if (errbuf)
		errbuf[0] = 0;
	/************ Get first send packet on queue and send it! ************/
	uint8* data = 0;
	int32 size = 0;
	int status = 0;
	if (ServerSendQueuePop(&data, &size)) {
#ifdef WIN32
		status = send(connection_socket, (const char *) data, size, 0);
#else
		status = send(connection_socket, data, size, MSG_NOSIGNAL);
		if(errno==EPIPE) status = SOCKET_ERROR;
#endif
		if (status >= 1) {
#if TCPN_LOG_RAW_DATA_OUT >= 1
			struct in_addr	in;
			in.s_addr = GetrIP();
			CoutTimestamp(true);
			cout << ": Wrote " << status << " bytes to network. " << inet_ntoa(in) << ":" << GetrPort();
			cout << endl;
	#if TCPN_LOG_RAW_DATA_OUT == 2
			int32 tmp = status;
			if (tmp > 32)
				tmp = 32;
			DumpPacket(data, status);
	#elif TCPN_LOG_RAW_DATA_OUT >= 3
			DumpPacket(data, status);
	#endif
#endif
			sent_something = true;
			if (status < (signed)size) {
#if TCPN_LOG_RAW_DATA_OUT >= 1
				struct in_addr	in;
				in.s_addr = GetrIP();
				CoutTimestamp(true);
				cout << ": Pushed " << (size - status) << " bytes back onto the send queue. " << inet_ntoa(in) << ":" << GetrPort();
				cout << endl;
#endif
				// If there's network congestion, the number of bytes sent can be less than
				// what we tried to give it... Push the extra back on the queue for later
				ServerSendQueuePushFront(&data[status], size - status);
			}
			else if (status > (signed)size) {
				cout << "TCPConnection::SendData(): WTF! status > size" << endl;
				Disconnect();
				return false;
			}
			// else if (status == size) {}
		}
		else {
			ServerSendQueuePushFront(data, size);
		}

		SafeDeleteArray(data);
		if (status == SOCKET_ERROR) {
#ifdef WIN32
			if (WSAGetLastError() != WSAEWOULDBLOCK)
#else
			if (errno != EWOULDBLOCK)
#endif
			{
				if (errbuf) {
#ifdef WIN32
					snprintf(errbuf, TCPConnection_ErrorBufferSize, "TCPConnection::SendData(): send(): Errorcode: %i", WSAGetLastError());
#else
					snprintf(errbuf, TCPConnection_ErrorBufferSize, "TCPConnection::SendData(): send(): Errorcode: %s", strerror(errno));
#endif
				}
				
				//if we get an error while disconnecting, just jump to disconnected
				MState.lock();
				if(pState == TCPS_Disconnecting)
					pState = TCPS_Disconnected;
				MState.unlock();
				
				return false;
			}
		}
	}
	return true;
}

ThreadReturnType TCPConnection::TCPConnectionLoop(void* tmp) {
#ifdef WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
	if (tmp == 0) {
		cout << "TCPConnectionLoop(): tmp = 0!" << endl;
		THREAD_RETURN(NULL);
	}
	TCPConnection* tcpc = (TCPConnection*) tmp;
#ifndef WIN32
	_log(COMMON__THREADS, "Starting TCPConnectionLoop with thread ID %d", pthread_self());
#endif
	tcpc->MLoopRunning.lock();
	while (tcpc->RunLoop()) {
		Sleep(LOOP_GRANULARITY);
		if (!tcpc->ConnectReady()) {
			//_CP(TCPConnectionLoop);
			if (!tcpc->Process()) {
				//the processing loop has detecting an error.. 
				//we want to drop the link immediately, so we clear buffers too.
				tcpc->ClearBuffers();
				tcpc->Disconnect();
			}
			Sleep(1);
		}
		else if (tcpc->GetAsyncConnect()) {
			//_CP(TCPConnectionLoop);
			if (tcpc->charAsyncConnect)
				tcpc->Connect(tcpc->charAsyncConnect, tcpc->GetrPort());
			else
				tcpc->ConnectIP(tcpc->GetrIP(), tcpc->GetrPort());
			tcpc->SetAsyncConnect(false);
		}
		else
			Sleep(10);	//nothing to do.
	}
	tcpc->MLoopRunning.unlock();
	
#ifndef WIN32
	_log(COMMON__THREADS, "Ending TCPConnectionLoop with thread ID %d", pthread_self());
#endif
	
	THREAD_RETURN(NULL);
}

bool TCPConnection::RunLoop() {
	bool ret;
	MRunLoop.lock();
	ret = pRunLoop;
	MRunLoop.unlock();
	return ret;
}





