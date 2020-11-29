#include <sys/fcntl.h>
#include <sys/poll.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>

#include <err.h>
#include <errno.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ifaddrs.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>

#include "vector.h"
#include "hashtable.h"
#include "util.h"

int resolve(char* node, int proto, unsigned short port, struct sockaddr_storage* addr, socklen_t* len) {
	struct addrinfo hints;

	memset(&hints, 0, sizeof(hints));

	hints.ai_family = AF_INET;
	hints.ai_protocol = proto;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV | AI_PASSIVE;

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

server_t start_server(int port) {
	server_t serv;
	serv.conns = vector_new(sizeof(struct pollfd));
	serv.nums = vector_new(sizeof(unsigned));
	serv.num = 0;

	serv.num_conns = map_new();
	map_configure_uint_key(&serv.num_conns, sizeof(int));

	signal(SIGPIPE, SIG_IGN);

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

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
		poll((struct pollfd*)serv->conns.data, serv->conns.length, -1);

		vector_iterator poll_iter = vector_iterate(&serv->conns);
		while (vector_next(&poll_iter)) {
			struct pollfd* pfd = poll_iter.x;
			if (pfd->revents) {
				if (poll_iter.i==1) {
					int new = accept(pfd->fd, NULL, NULL);
					vector_pushcpy(&serv->conns, &(struct pollfd){.fd=new, .events=POLLIN});
					vector_pushcpy(&serv->nums, &serv->num);
					map_insertcpy(&serv->num_conns, &serv->num, &new);
					serv->num++;
				} else if (pfd->revents & POLLHUP) {
					*i = *(unsigned*)vector_get(&serv->nums, poll_iter.i-1);
					map_remove(&serv->num_conns, i);

					vector_remove(&serv->conns, poll_iter.i-1);
					vector_remove(&serv->nums, poll_iter.i-1);

					poll_iter.i--;

					cur.start=NULL;
					return cur;
				} else if (pfd->revents & POLLIN){
					*i = *(unsigned*)vector_get(&serv->nums, poll_iter.i-1);
					if (read(pfd->fd, &cur.left, sizeof(unsigned))==-1) continue;
					cur.start = heap(cur.left);
					if (read(pfd->fd, cur.start, cur.left)==-1) {
						drop(cur.start);
						continue;
					}

					cur.cur=cur.start;
					return cur;
				}
			}
		}
	}
}

void server_send(server_t* serv, unsigned i, vector_t* data) {
	int fd = *(int*)map_find(&serv->num_conns, &i);
	write(fd, &data->length, sizeof(unsigned));
	write(fd, data->data, data->length);
}

typedef struct {
	int fd;
	//pipe wfd -> wfd_rd when using select; another thread can be writing
	int wfd;
	int wfd_rd;
} client_t;

client_t client_connect(char* serv, int port) {
	struct sockaddr_storage addr;
	socklen_t addrlen;

	if (!resolve(serv, IPPROTO_TCP, port, &addr, &addrlen))
		perrorx("could not resolve server");

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(sock, (struct sockaddr*)&addr, addrlen)==-1)
		perrorx("could not connect to server");

	return (client_t){.fd=sock, .wfd=sock};
}

void client_send(client_t* client, vector_t* d) {
	if (write(client->wfd, &d->length, sizeof(unsigned))==-1 || write(client->wfd, d->data, d->length)==-1) perrorx("couldnt send to server");
}

cur_t client_recv(client_t* client) {
	cur_t cur = {0};
	if (read(client->fd, &cur.left, sizeof(unsigned))==-1) cur.err=errno;
	else cur.start = heap(cur.left);
	if (read(client->fd, cur.start, cur.left)==-1) cur.err=errno;
	else cur.cur=cur.start;
	return cur;
}

cur_t client_recv_timeout(client_t* client, int ms) {
	if (client->fd == client->wfd) {
		int p[2];
		pipe(p);
		client->wfd = p[1];
		client->wfd_rd = p[0];

	}

	struct pollfd pfds[2] = {{.fd=client->fd, .events=POLLIN}, {.fd=client->wfd_rd, .events=POLLIN}};
	do {
		if (poll(pfds, 2, ms)==0) return (cur_t){.err=ETIMEDOUT};

		if (pfds[1].revents & POLLIN) {
			char buf[PIPE_BUF];
			size_t sz = read(client->wfd_rd, buf, PIPE_BUF);
			write(client->fd, buf, sz);
		}
	} while ((pfds[1].revents&POLLIN) || !(pfds[0].revents&POLLIN));

	return client_recv(client);
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
