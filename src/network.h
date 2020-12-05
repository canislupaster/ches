// Automatically generated header.

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#ifdef _WIN32
#include <winsock2.h>
#endif
#ifdef _WIN32
#include <WS2tcpip.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif
#ifndef _WIN32
#include <poll.h>
#endif
#ifndef _WIN32
#include <sys/socket.h>
#endif
#ifndef _WIN32
#include <netinet/in.h>
#endif
#ifndef _WIN32
#include <arpa/inet.h>
#endif
#ifndef _WIN32
#include <ifaddrs.h>
#endif
#ifndef _WIN32
#include <netdb.h>
#endif
#include "vector.h"
#include "hashtable.h"
#include "util.h"
typedef struct {
	vector_t conns; //0 is server
	vector_t nums;
	map_t num_conns;
	unsigned num;
} server_t;
typedef struct {
	char* start;
	char* cur;
	unsigned left;
	int err;
} cur_t;
server_t start_server(int port);
cur_t server_recv(server_t* serv, unsigned* i);
void server_send(server_t* serv, unsigned i, vector_t* data);
typedef struct {
	int fd;
	//pipe wfd -> wfd_rd when using select; another thread can be writing
#ifndef _WIN32
	int wfd;
	int wfd_rd;
#endif
} client_t;
client_t client_connect(char* serv, int port);
void client_send(client_t* client, vector_t* d);
cur_t client_recv(client_t* client);
cur_t client_recv_timeout(client_t* client, int ms);
void write_int(vector_t* bytes, int x);
void write_uint(vector_t* bytes, unsigned x);
void write_str(vector_t* bytes, char* str);
void write_uchr(vector_t* bytes, unsigned char x);
int read_int(cur_t* cur);
unsigned read_uint(cur_t* cur);
char read_uchr(cur_t* cur);
char read_chr(cur_t* cur);
char* read_str(cur_t* cur);
