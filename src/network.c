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
#include <openssl/rand.h>
#include "threads.h"
#endif

#ifdef __EMSCRIPTEN__
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ifaddrs.h>
#include <netdb.h>

#include <emscripten.h>
#include <emscripten/websocket.h>
#elif defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#include <WS2tcpip.h>
#else
#include <unistd.h>

#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ifaddrs.h>
#include <netdb.h>

#include "portableendian.h" //64 bit websocket lengths
#endif

#include "vector.h"
#include "hashtable.h"
#include "cfg.h"
#include "util.h"

typedef enum {
	ws_cont=0, ws_txt=1, ws_bin=2, ws_close=8, ws_ping=9, ws_pong=10
} ws_opcode;

int resolve(char* node, int proto, unsigned short port, struct sockaddr_storage* addr, socklen_t* len) {
	struct addrinfo hints;

	memset(&hints, 0, sizeof(hints));

	hints.ai_family = AF_INET;
	hints.ai_protocol = proto;
	hints.ai_socktype = SOCK_STREAM;
	if (!node) hints.ai_flags = AI_NUMERICSERV | AI_PASSIVE;

	struct addrinfo *res;
	char* portstr = heapstr("%i", port);
	if (getaddrinfo(node, portstr, &hints, &res)!=0) return 0;
	drop(portstr);
	if (!res) return 0;

	memcpy(addr, res->ai_addr, res->ai_addrlen);
	*len = res->ai_addrlen;

	freeaddrinfo(res);

	return 1;
}

typedef struct {
	vector_t frames;
	int fin;
} server_msg_t;

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

#define HTTP_LINE_BUF 1024
char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

char* WS_RESP = "HTTP/1.1 101 Switching Protocols\r\n"
								"Connection: Upgrade\r\n"
								"Upgrade: websocket\r\n"
								"Sec-WebSocket-Accept: %s\r\n"
								"\r\n";

#ifndef __EMSCRIPTEN__
void write_frame(int fd, ws_opcode op, vector_t* data) {
	unsigned char hdr[2];

	hdr[0] = op | (1<<7); //FIN - op
	if (data) { //dataless control frame
		if (data->length>125 && data->length<=UINT16_MAX) {
			hdr[1] = 126;
			send(fd, hdr, 2, 0);
			uint16_t l = htons((uint16_t)data->length);
			send(fd, (char*)&l, 2, 0);
		} else if (data->length>125) {
			hdr[1] = 127;
			send(fd, hdr, 2, 0);
			uint64_t l = htobe64((uint64_t)data->length);
			send(fd, (char*)&l, 8, 0);
		} else {
			hdr[1] = (unsigned char)data->length;
			send(fd, hdr, 2, 0);
		}
	}

	if (data) {
		send(fd, data->data, data->length, 0);
	}
}

void read_frame(server_msg_t* msg, int fd, char* hup) {
	unsigned char hdr[2];
	if (recv(fd, hdr, 2, 0)==-1) return;

	msg->fin = (int)(hdr[0]>>7);
	ws_opcode op = hdr[0]&(UCHAR_MAX>>4);

	if (hdr[1]>>7 != 1) { //no mask
		*hup=1; return;
	}

	unsigned char lenb = hdr[1]&(UCHAR_MAX>>1);
	unsigned len;
	if (lenb<126) {
		len = lenb;
	} else if (lenb==126) {
		uint16_t x;
		recv(fd, &x, 2, 0);
		len = (unsigned)ntohs(x);
	} else if (lenb==127) {
		uint64_t x;
		recv(fd, &x, 8, 0);
		len = (unsigned)be64toh(x);
	}

	unsigned mask;
	recv(fd, &mask, 4, 0);

	if (len) {
		char* frame = vector_stock(&msg->frames, len);
		recv(fd, frame, len, 0);

		unsigned i=0;
		for (; i+4<=len; i+=4) *(unsigned*)(frame+i) ^= mask;
		for(; i<len; i++) frame[i] ^= ((char*)&mask)[i%4];
	}

	switch (op) {
		case ws_close: {
			*hup=1; return;
		}
		case ws_ping: {
			write_frame(fd, ws_pong, NULL);
			return;
		}
		default:;
	}
}

server_t start_server(int port, char upgrade) {
#ifdef _WIN32
	WSADATA data;
	if (WSAStartup(MAKEWORD(2,2), &data)!=0) perrorx("winsock failed init");
#endif

	server_t serv;
	serv.conns = vector_new(sizeof(struct pollfd));
	serv.nums = vector_new(sizeof(unsigned));
	if (upgrade) {
		serv.conn_upgrade = vector_new(1);
		serv.msg = vector_new(sizeof(server_msg_t));
	}

	serv.upgrade = upgrade;
	serv.num = 0;

	serv.num_conns = map_new();
	map_configure_uint_key(&serv.num_conns, sizeof(int));
#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
#endif
	int sock = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &(char){1}, sizeof(int));
#else
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
#endif

	struct sockaddr_storage addr;
	socklen_t addrlen;

	if (!resolve(NULL, IPPROTO_TCP, port, &addr, &addrlen)) perrorx("could not resolve server address");

	if (bind(sock, (struct sockaddr*)&addr, addrlen)==-1)
		perrorx("could not start listener");

	listen(sock, 10);
	struct pollfd pfd = {.fd=sock, .events=POLLIN};
	vector_pushcpy(&serv.conns, &pfd);
	vector_pushcpy(&serv.nums, &(unsigned){serv.num++});
	return serv;
}

cur_t server_recv(server_t* serv, unsigned* i) {
	cur_t cur = {.err=0};
	while (1) {
#ifdef _WIN32
		WSAPoll((struct pollfd*)serv->conns.data, serv->conns.length, -1);
#else
		poll((struct pollfd*)serv->conns.data, serv->conns.length, -1);
#endif

		vector_iterator poll_iter = vector_iterate(&serv->conns);
		while (vector_next(&poll_iter)) {
			struct pollfd* pfd = poll_iter.x;
			if (!pfd->revents) continue;

			char hup=0;
			if (poll_iter.i==0) {
				int new = accept(pfd->fd, NULL, NULL);
				vector_pushcpy(&serv->conns, &(struct pollfd){.fd=new, .events=POLLIN});
				vector_pushcpy(&serv->nums, &serv->num);
				if (serv->upgrade) vector_pushcpy(&serv->conn_upgrade, &(char){0});
				map_insertcpy(&serv->num_conns, &serv->num, &new);
				serv->num++;
			} else if (pfd->revents & POLLHUP) {
				*i = *(unsigned*)vector_get(&serv->nums, poll_iter.i);
				hup=1;
			} else if (pfd->revents & POLLIN) {
				*i = *(unsigned*)vector_get(&serv->nums, poll_iter.i);
				char* upgraded = vector_get(&serv->conn_upgrade, poll_iter.i-1);

				if (serv->upgrade && !*upgraded){
					char linebuf[HTTP_LINE_BUF];
					int lb_cur=0;

					char parsed_req=0, is_upgrade=0;
					char* ws_key=NULL;

					while (recv(pfd->fd, linebuf+lb_cur, 1, 0)!=-1) {
						if (linebuf[lb_cur]=='\n') {
							if (lb_cur==1) break;

							lb_cur=0;
							continue;
						} else if (linebuf[lb_cur]=='\r') {
							linebuf[lb_cur] = 0;

							char* lb_ptr = linebuf;
							if (!parsed_req) {
								skip_until(&lb_ptr, " "); //ignore method
								skip_ws(&lb_ptr);
								if (!skip_name(&lb_ptr, "/ ")) {
									hup=1; break;
								}

								parsed_req=1;
							} else if (lb_cur>0) {
								char* k = parse_name(&lb_ptr);
								if (!k) {
									hup=1; break;
								} else if (strlen(k)==0) {
									drop(k);
									hup=1; break;
								}

								if (k[strlen(k)-1]==':') k[strlen(k)-1]=0;
								skip_ws(&lb_ptr);
								char* v = parse_name(&lb_ptr);
								if (!v) {
									drop(k);
									hup=1; break;
								}

								if (streq(k, "Sec-WebSocket-Key")) {
									ws_key=v;
								} else if (streq(k, "Upgrade")) {
									if (streq(v, "websocket")) is_upgrade=1;
									drop(v);
								} else {
									drop(v);
								}

								drop(k);
							}
						}

						if (++lb_cur >= HTTP_LINE_BUF) {
							lb_cur=0; //lmao
						}
					}

					if (parsed_req && ws_key && is_upgrade) {
						*upgraded=1;

						char hash[SHA_DIGEST_LENGTH];
						char* ws_cat = stradd(ws_key, WS_GUID);
						SHA1((unsigned char*)ws_cat, strlen(ws_cat), (unsigned char*)hash);

						char* b64_hash = base64_encode(hash, SHA_DIGEST_LENGTH);
						char* resp = heapstr(WS_RESP, b64_hash);

						if (send(pfd->fd, resp, strlen(resp), 0)==-1) hup=1;

						drop(resp);
						drop(b64_hash);
						drop(ws_cat);
						drop(ws_key);
					} else {
						hup=1;
					}
				} else if (serv->upgrade) {
					char exist=0;
					server_msg_t* msg = vector_setget(&serv->msg, poll_iter.i-1, &exist);
					if (!exist) {
						msg->frames = vector_new(1);
						msg->fin = 0;
					}

					read_frame(msg, pfd->fd, &hup);
					if (msg->fin && !hup) { //pop a message
						cur.left = msg->frames.length;
						cur.start = heapcpy(msg->frames.length, msg->frames.data);
						cur.cur = cur.start;
						cur.err = 0;

						vector_clear(&msg->frames);

						return cur;
					}
				} else {
					if (recv(pfd->fd, (char*)&cur.left, sizeof(unsigned), 0)==-1) continue;
					cur.left = ntohl(cur.left);
					cur.start = heap(cur.left);
					if (recv(pfd->fd, cur.start, (int)cur.left, 0)==-1) {
						drop(cur.start);
						continue;
					}

					cur.cur=cur.start;
					return cur;
				}
			}

			if (hup) {
				if (serv->upgrade) {
					char* upgraded = vector_get(&serv->conn_upgrade, poll_iter.i-1);
					if (*upgraded) {
						write_frame(pfd->fd, ws_close, NULL);
					}

					vector_remove(&serv->conn_upgrade, poll_iter.i-1);

					server_msg_t* msg = vector_get(&serv->msg, poll_iter.i-1);
					if (msg) {
						vector_free(&msg->frames);
						vector_remove(&serv->msg, poll_iter.i-1);
					}
				}

				map_remove(&serv->num_conns, i);

				vector_remove(&serv->conns, poll_iter.i);
				vector_remove(&serv->nums, poll_iter.i);

				poll_iter.i--;

				cur.start=NULL;
				return cur;
			}
		}
	}
}

void server_send(server_t* serv, unsigned i, vector_t* data) {
	int fd = *(int*)map_find(&serv->num_conns, &i);
	if (serv->upgrade) {
		write_frame(fd, ws_bin, data);
	} else {
		unsigned l = htonl(data->length);
		send(fd, (char*)&l, sizeof(unsigned), 0);
		send(fd, data->data, (int)data->length, 0);
	}
}
#endif //EMSCRIPTEN

typedef struct {
#ifdef __EMSCRIPTEN__
	vector_t msg_buf;
#else
	thrd_t recv_thrd;
	int fd;
	int recv;
#endif

	void (*cb)(void*, cur_t cur);
	void* arg;
	int err;
} client_t;

cur_t client_recv(client_t* client) {
	cur_t cur = {0};
#ifdef __EMSCRIPTEN__
	if (client->msg_buf.length==0) {
		cur.err=EAGAIN;
		return cur;
	}

	vector_t* buf = vector_get(&client->msg_buf, 0);
	cur.start=buf->data;
	cur.cur=cur.start;
	cur.left=buf->length;
	cur.err=client->err;

	vector_remove(&client->msg_buf, 0);
#else
	if (recv(client->fd, (char*)&cur.left, sizeof(unsigned), 0)==-1) cur.err=errno;
	else cur.start = heap(cur.left);
	if (recv(client->fd, cur.start, (int)cur.left, 0)==-1) cur.err=errno;
	else cur.cur=cur.start;
#endif

	return cur;
}

//client callbacks
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
void client_recv_cb(char* data, unsigned len, void* udata) {
	client_t* client = udata;
	vector_t buf = vector_new(1);
	vector_stockcpy(&buf, len, data);

	vector_pushcpy(&client->msg_buf, &buf);

	cur_t c = client_recv(client);
	client->cb(client->arg, c);
}

EMSCRIPTEN_KEEPALIVE
void client_err_cb(void* udata) {
	client_t* client = udata;
	client->err = ECOMM;
}
#else
#define NET_TIMEOUT 1000
int client_recv_thrd(client_t* client) {
	while (client->recv) {
#ifdef _WIN32
		struct pollfd pfds[2] = {{.fd=client->fd, .events=POLLIN}};
		if (WSAPoll(pfds, 2, NET_TIMEOUT)==0) continue;
#else
		struct pollfd pfds[1] = {{.fd=client->fd, .events=POLLIN}};
		if (poll(pfds, 1, NET_TIMEOUT)==0) continue;
#endif

		cur_t cur = client_recv(client);
		client->cb(client->arg, cur);
	}

	return 1;
}
#endif

client_t* client_connect(char* serv, int port, void (*cb)(void*, cur_t), void* arg) {
	client_t* client = heapcpy(sizeof(client_t), &(client_t){.err=0, .cb=cb, .arg=arg});

#ifdef _WIN32
	WSADATA data;
	if (WSAStartup(MAKEWORD(2,2), &data)!=0)
		client->err=errno;

#elif defined(__EMSCRIPTEN__)

	MAIN_THREAD_EM_ASM({
		 sock = new WebSocket(UTF8ToString($0));
		 sock.addEventListener("message", (ev) => {
				ev.data.arrayBuffer().then((abuf) => {
					let buf = _malloc(abuf.byteLength);
					HEAP8.set(new Uint8Array(abuf), buf);
					_client_recv_cb(buf, abuf.byteLength, $1);
				});
		 });

		 sock.addEventListener("error", (ev) => {
				 _client_err_cb($1);
		 });
	}, serv, client);

	client->msg_buf = vector_new(sizeof(vector_t));

	int rdy;
	do {
		emscripten_sleep(100);
		rdy = MAIN_THREAD_EM_ASM_INT(return sock.readyState;);

		if (rdy==3) {
			client->err=ECONNREFUSED;
			break;
		}
	} while (rdy==0 && client->err==0);

	return client;
#else

	struct sockaddr_storage addr;
	socklen_t addrlen;

	if (!resolve(serv, IPPROTO_TCP, port, &addr, &addrlen))
		perrorx("could not resolve server");

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(sock, (struct sockaddr*)&addr, addrlen)==-1 && errno!=EINPROGRESS) client->err=errno;
#endif

#ifndef __EMSCRIPTEN__
	if (client->err) return client;

	client->recv = 1;
	thrd_create(&client->recv_thrd, (int(*)(void*))client_recv_thrd, client);
#endif

	return client;
}

void client_send(client_t* client, vector_t* d) {
#ifdef __EMSCRIPTEN__
	EM_ASM({
		sock.send(HEAP8.subarray($0, $0+$1));
	}, d->data, d->length);
#else
	unsigned l = htonl(d->length);
	if (send(client->fd, (char*)&l, sizeof(unsigned), 0)==-1
			|| send(client->fd, d->data, (int)d->length, 0)==-1)
		perrorx("couldnt send to server");
#endif
}

void client_free(client_t* client) {
#ifdef __EMSCRIPTEN__
	EM_ASM(sock.close(););
	vector_iterator msg_iter = vector_iterate(&client->msg_buf);
	while (vector_next(&msg_iter)) {
		vector_free(msg_iter.x);
	}

	vector_free(&client->msg_buf);

#else
	client->recv=0;
	close(client->fd);
#endif

	drop(client);
}

//htonl wrapper
void write_int(vector_t* bytes, int x) {
	vector_stockcpy(bytes, 4, &(int){htonl(x)});
}

void write_uint(vector_t* bytes, unsigned x) {
	write_int(bytes, *((int*)&x));
}

void write_str(vector_t* bytes, char* str) {
	vector_stockcpy(bytes, strlen(str)+1, str);
}

void write_uchr(vector_t* bytes, unsigned char x) {
	vector_pushcpy(bytes, (char*)&x);
}

int read_int(cur_t* cur) {
	if (cur->left<4) {
		cur->err=1;
		return -1;
	}

	cur->left-=4;
	cur->cur+=4;
	return ntohl(*(int*)(cur->cur-4));
}

unsigned read_uint(cur_t* cur) {
	int x = read_int(cur);
	return *((unsigned*)&x);
}

unsigned char read_uchr(cur_t* cur) {
	if (cur->left==0) {
		cur->err=1;
		return 0;
	}

	cur->left--;
	return *(unsigned char*)(cur->cur++);
}

char read_chr(cur_t* cur) {
	if (cur->left==0) {
		cur->err=1;
		return -1;
	}

	cur->left--;
	return *(cur->cur++);
}

char* read_str(cur_t* cur) {
	char* start = cur->cur;
	while ((cur->left--)>0 && *(cur->cur++) != 0);
	if (*(cur->cur-1) != 0) {
		cur->err=1;
		return NULL;
	}

	return heapcpy(cur->cur-start, start);
}
