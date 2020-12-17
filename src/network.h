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
#ifndef __EMSCRIPTEN__
#include <openssl/sha.h>
#endif
#ifndef __EMSCRIPTEN__
#include <openssl/rand.h>
#endif
#ifdef __EMSCRIPTEN__
#include <unistd.h>
#endif
#ifdef __EMSCRIPTEN__
#include <sys/socket.h>
#endif
#ifdef __EMSCRIPTEN__
#include <netinet/in.h>
#endif
#ifdef __EMSCRIPTEN__
#include <arpa/inet.h>
#endif
#ifdef __EMSCRIPTEN__
#include <ifaddrs.h>
#endif
#ifdef __EMSCRIPTEN__
#include <netdb.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#if (defined(_WIN32)) && !defined(__EMSCRIPTEN__)
#include <winsock2.h>
#endif
#if (defined(_WIN32)) && !defined(__EMSCRIPTEN__)
#include <windows.h>
#endif
#if (defined(_WIN32)) && !defined(__EMSCRIPTEN__)
#include <WS2tcpip.h>
#endif
#if !((defined(_WIN32)) || defined(__EMSCRIPTEN__))
#include <unistd.h>
#endif
#if !((defined(_WIN32)) || defined(__EMSCRIPTEN__))
#include <poll.h>
#endif
#if !((defined(_WIN32)) || defined(__EMSCRIPTEN__))
#include <sys/socket.h>
#endif
#if !((defined(_WIN32)) || defined(__EMSCRIPTEN__))
#include <netinet/in.h>
#endif
#if !((defined(_WIN32)) || defined(__EMSCRIPTEN__))
#include <arpa/inet.h>
#endif
#if !((defined(_WIN32)) || defined(__EMSCRIPTEN__))
#include <ifaddrs.h>
#endif
#if !((defined(_WIN32)) || defined(__EMSCRIPTEN__))
#include <netdb.h>
#endif
#include "vector.h"
#include "hashtable.h"
#include "cfg.h"
#include "util.h"
typedef struct {
	char upgrade;
	vector_t conn_upgrade;
	vector_t msg;

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
#ifndef __EMSCRIPTEN__
server_t start_server(int port, char upgrade);
#endif
#ifndef __EMSCRIPTEN__
cur_t server_recv(server_t* serv, unsigned* i);
#endif
#ifndef __EMSCRIPTEN__
void server_send(server_t* serv, unsigned i, vector_t* data);
#endif
typedef struct {
	int err;

	int fd;
#ifdef __EMSCRIPTEN__
	int cb_init;

	mtx_t cnd_mtx;
	cnd_t recv;
#endif
} client_t;
client_t client_connect(char* serv, int port);
void client_send(client_t* client, vector_t* d);
cur_t client_recv(client_t* client);
int client_recv_timeout(client_t* client, int ms);
void write_int(vector_t* bytes, int x);
void write_uint(vector_t* bytes, unsigned x);
void write_str(vector_t* bytes, char* str);
void write_uchr(vector_t* bytes, unsigned char x);
int read_int(cur_t* cur);
unsigned read_uint(cur_t* cur);
unsigned char read_uchr(cur_t* cur);
char read_chr(cur_t* cur);
char* read_str(cur_t* cur);
