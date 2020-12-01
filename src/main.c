#include <stdio.h>
#include <ncurses.h>
#include <locale.h>
#include <signal.h>

#include "chess.h"
#include "network.h"

#include "cfg.h"
#include "vector.h"
#include "threads.h"
#include "hashtable.h"
#include "tinydir.h"

const int NUM_BGS=5;
const short BGS[5] = {COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_MAGENTA, COLOR_YELLOW};
const int NUM_FGS=4;
const short FGS[4] = {COLOR_WHITE, COLOR_BLACK, COLOR_BLACK+8, COLOR_CYAN};

chess_client_t g_client;

void center(chess_client_t* client, int row, char* txt) {
	attr_t a;
	short c;
	attr_get(&a, &c, NULL);
	move(row,client->centerx-strlen(txt)/2);
	attr_set(a, c, NULL);
	addstr(txt);
}

char* inputui(chess_client_t* client, char* prompt, int sz, int noempty) {
	clear();
	curs_set(2);
	echo();

	attr_set(A_BOLD,0,NULL);
	center(client, client->centery-1, prompt);
	attr_set(A_NORMAL,0,NULL);

	char* answer = heap(sz+1);

	move(client->centery+1, client->centerx-sz/2);
	do getnstr(answer, sz); while (strlen(answer)==0 && noempty);

	return heapcpystr(answer);
}

#define ALERT_COLOR 1

void alertui(chess_client_t* client, char* msg) {
	clear();
	curs_set(0);
	noecho();

	attr_set(A_BOLD,ALERT_COLOR,NULL);
	center(client, client->centery-1, msg);
	attr_set(A_NORMAL,0,NULL);
	center(client, client->centery+1, "(press any key)");
	getch();
}

#define SELECT_COLOR 1

int selectui(chess_client_t* client, char* prompt, vector_t* options) {
	curs_set(0);
	echo();

	int current=0;

	int ch = -1;
	do {
		if (ch==KEY_UP) current--; else if (ch==KEY_DOWN) current++;
		current=clamp(current, 0, (int)options->length);

		clear();
		attr_set(A_BOLD,0,NULL);
		center(client, client->centery - (int)options->length/2 - 2, prompt);

		vector_iterator opts = vector_iterate(options);
		while (vector_next(&opts)) {
			attr_set(A_NORMAL,current==opts.i-1 ? SELECT_COLOR : 0,NULL);
			center(client, client->centery - (int)options->length/2 + (int)opts.i-1, *(char**)opts.x);
		}
	} while ((ch = getch()) != '\n');

	return current;
}

void setup_game(chess_client_t* client, client_mode_t mode, char* name) {
	vector_t boards = vector_new(sizeof(char*));
	vector_t boards_path = vector_new(sizeof(char*));

	tinydir_dir dir;
	tinydir_file file;

	tinydir_open(&dir, "./");
	for (; dir.has_next; tinydir_next(&dir)) {
		tinydir_readfile(&dir, &file);
		if (streq(file.name, ".") || streq(file.name, "..") || streq(file.name, "./"))
			continue;

		if (streq(ext(file.name), ".board")) {
			vector_pushcpy(&boards, &(char*){heapcpystr(file.name)});
			vector_pushcpy(&boards_path, &(char*){heapcpystr(file.path)});
		}
	}

	tinydir_close(&dir);

	int i = selectui(client, "chess", &boards);
	char* path = *(char**)vector_get(&boards_path, i);

	char* str = read_file(path);
	if (!str) alertui(client, "failed to read board");

	client->g = parse_board(str);

	vector_free_strings(&boards);
	vector_free_strings(&boards_path);

	srand(time(NULL)); //TODO: choice of player
	client->player = 0;
	player_t* t = vector_get(&client->g.players, client->player);
	if (mode==mode_multiplayer) t->joined = 1;
	if (name) t->name = name;
	board_pos_i(&client->g, client->select, t->king);

	g_client.select2[0] = -1;
	g_client.which = 0;
	client->hints = vector_new(sizeof(piece_t*));

	client->mode = mode;
}

void refresh_margin() {
	int w,h;
	getmaxyx(stdscr, h, w);

	g_client.centerx = w/2;
	g_client.centery = h/2;
}

void read_game(cur_t* cur, chess_client_t* client) {
	client->g = (game_t){0};
	read_players(cur, &client->g, NULL);
	read_board(cur, &client->g);
	read_moves(cur, &client->g);
	client->player = read_chr(cur);
	client->mode = mode_multiplayer;
}

void render(chess_client_t* client);

void refresh_hints(chess_client_t* client) {
	piece_t* p = board_get(&client->g, client->select);
	vector_clear(&client->hints);
	if (p->player==client->player) {
		piece_moves(&client->g, p, &client->hints);
	}
}

#define CHESS_CLIENT_TIMEOUT 1000

int chess_client_recv(void* arg) {
	chess_client_t* client = arg;

	while (client->recv) {
		cur_t cur = client_recv_timeout(&client->server, CHESS_CLIENT_TIMEOUT);
		if (cur.err==ETIMEDOUT) continue;
		else if (cur.err!=0) {
			mtx_lock(&client->lock);
			alertui(client, "couldnt read from server");
		} else {
			mtx_lock(&client->lock);
		}

		mp_serv_t msg = (mp_serv_t)read_chr(&cur);

		switch (msg) {
			case mp_game: {
				read_game(&cur, client);
				break;
			}
			case mp_game_joined: {
				char player = read_chr(&cur);
				player_t* p = vector_get(&client->g.players, (unsigned)player);
				p->name = read_str(&cur);
				p->joined = 1;
				break;
			}
			case mp_game_left: {
				char player = read_chr(&cur);
				player_t* p = vector_get(&client->g.players, (unsigned)player);
				p->joined = 0;
				break;
			}
			case mp_move_made: {
				move_t m = read_move(&cur);
				make_move(&client->g, &m, 0, client->g.player);

				refresh_hints(client);
				break;
			}
			default:;
		}

		if (cur.err!=0) alertui(client, "network error, maybe your game is broken");

		render(client);
		mtx_unlock(&client->lock);
		drop(cur.start);
	}

	return 0;
}

void client_make_move(chess_client_t* client) {
	if (client->player != client->g.player) return;

	piece_t* p = board_get(&client->g, client->select2);
	if (vector_search(&client->hints, &p)!=0) {
		move_t m = {.from={client->select[0],client->select[1]}, .to={client->select2[0],client->select2[1]}};
		make_move(&client->g, &m, 0, client->player);

		//switch player or send move
		if (client->mode==mode_singleplayer) {
			client->player = client->g.player;
		} else if (client->mode==mode_multiplayer) {
			vector_t data = vector_new(1);
			vector_pushcpy(&data, &(char){(char)mp_make_move});
			write_move(&data, &m);

			client_send(&client->server, &data);
			vector_free(&data);
		}

		refresh_hints(client);
	}
}

void setup_multiplayer(chess_client_t* client) {
	config_val* addrval = map_find(&client->cfg, &CFG_SERVADDR);

	char* addr = inputui(client, addrval->data.str ? heapstr("type new address or hit enter to use %s", addrval->data.str) : "type new server address", 24, addrval->data.str==NULL);

	if (strlen(addr)>0) {
		addrval->data.str = addr;
		addrval->is_default=0;
	} else {
		drop(addr);
	}

	client->server = client_connect(addrval->data.str, MP_PORT);

	config_val* nameval = map_find(&client->cfg, &CFG_NAME);
	char* name = inputui(client, *nameval->data.str ? heapstr("type username or use %s", nameval->data.str) : "type a username or leave default", 24, 0);
	if (strlen(name)>0) {
		nameval->data.str = name;
		nameval->is_default=0;
	} else {
		drop(name);
	}

	save_configure(&client->cfg, CFG_PATH);

	int full=0;
	vector_t data = vector_new(1);

	do {
		client_send(&client->server, &(vector_t){.data=(char[]){(char)mp_list_game}, .length=1});

		cur_t resp = client_recv(&client->server);
		if (resp.err || (mp_serv_t)read_chr(&resp) != mp_game_list) errx(0, "game list not returned");
		unsigned games = read_uint(&resp);
		vector_t game_names = vector_new(sizeof(char*));
		for (unsigned i=0; i<games; i++) {
			vector_pushcpy(&game_names, &(char*){read_str(&resp)});
		}

		drop(resp.start);

		char* makegame = "make a game";
		vector_pushcpy(&game_names, &makegame);
		int opt = selectui(client, "choose a game, or make one", &game_names);

		vector_pop(&game_names);
		vector_free_strings(&game_names);

		if (opt == game_names.length) {
			char* g_name = inputui(client, "name your game", 24, 1);
			if (strlen(g_name)==0) {
				drop(g_name);
				g_name = nameval->data.str;
			}

			setup_game(client, mode_multiplayer, *nameval->data.str ? nameval->data.str : NULL);
			vector_pushcpy(&data, &(char){(char)mp_make_game});

			write_str(&data, g_name);
			write_players(&data, &client->g);
			write_board(&data, &client->g);

			client_send(&client->server, &data);

			cur_t cur = client_recv(&client->server);
			if (cur.err || (mp_serv_t)read_chr(&cur) != mp_game_made) errx(0, "game failed to create");
			drop(cur.start);
		} else {
			vector_pushcpy(&data, &(char){(char)mp_join_game});
			write_uint(&data, opt);
			write_str(&data, nameval->data.str);
			client_send(&client->server, &data);

			cur_t cur = client_recv(&client->server);
			mp_serv_t msg = (mp_serv_t)read_chr(&cur);
			if (msg == mp_game_full) {
				alertui(client, "game is full, take another gamble");
				full=1; continue;
			} else if (msg == mp_game) {
				full=0;
			} else {
				errx(0, "server did not send the game");
			}

			read_game(&cur, client);
			drop(cur.start);

			player_t* t = vector_get(&client->g.players, client->player);
			board_pos_i(&client->g, client->select, t->king);

			g_client.select2[0] = -1;
			g_client.which = 0;
			client->hints = vector_new(sizeof(piece_t*));
		}
	} while (full);

	client->recv = 1;
	mtx_init(&client->lock, mtx_plain);
	mtx_lock(&client->lock);
	thrd_create(&client->recv_thrd, chess_client_recv, client);

	vector_free(&data);
}

void render(chess_client_t* client) {
	if (client->mode == mode_menu) {
		char* menu[] = {"local", "multiplayer"};
		int mode = selectui(client, "termchess", &(vector_t){.data=(char*)&menu, .size=sizeof(char*), .length=2});
		if (mode==0) {
			setup_game(client, mode_singleplayer, NULL);
		} else if (mode==1) {
			setup_multiplayer(client);
		}

		refresh_margin();
	}

	erase();

	curs_set(0);
	noecho();

	int x_margin = client->centerx-client->g.board_w/2;
	int y_margin = client->centery-client->g.board_h/2;

	player_t* t = vector_get(&client->g.players, client->player);

	int pos[2];
	for (pos[1]=0; pos[1]<client->g.board_h; pos[1]++) {
		move(y_margin+pos[1], x_margin);

		for (pos[0]=0; pos[0]<client->g.board_w; pos[0]++) {
			int bpos[2];
			board_rot_pos(&client->g, t->board_rot, pos, bpos);
			piece_t* p = board_get(&client->g, bpos);

			int bg;
			if (bpos[0] == client->select[0] && bpos[1] == client->select[1]) {
				bg = 2;
			} else if (bpos[0] == client->select2[0] && bpos[1] == client->select2[1]) {
				bg = 3;
			} else if (vector_search(&client->hints, &p)!=0) {
				bg = 4;
			} else {
				bg = (bpos[0]+bpos[1])%2;
			}

			attr_set(A_BOLD, 1 + bg*NUM_FGS + player_col(p->player), NULL);
			addstr(piece_str(p));
		}
	}

	vector_iterator t_iter = vector_iterate(&client->g.players);
	while (vector_next(&t_iter)) {
		player_t* p = t_iter.x;
		int txt_pos[2];

		int rel_rot = p->board_rot-t->board_rot;
		if (rel_rot<0) rel_rot=4+rel_rot;
		rel_rot %= 4;

		int won = client->g.won && t_iter.i-1==client->g.player;
		int turn = t_iter.i-1==client->g.player;
		int slen = (int)(strlen(p->name)+turn*strlen("'s turn"))+won+(client->mode==mode_multiplayer);

		switch (rel_rot) {
			case 0: txt_pos[0]=0; txt_pos[1]=client->g.board_h; break;
			case 2: txt_pos[0]=client->g.board_w-slen; txt_pos[1]=-1; break;
			case 1: txt_pos[0]=client->g.board_w; txt_pos[1]=client->g.board_h-1; break;
			case 3: txt_pos[0]=-slen; txt_pos[1]=0; break;
		}

		move(y_margin+txt_pos[1],txt_pos[0]+x_margin);
		attr_set(A_NORMAL, 1 + ((txt_pos[0]+txt_pos[1])%2)*NUM_FGS + player_col(t_iter.i-1), NULL);

		if (won) addstr( "👑");
		if (client->mode==mode_multiplayer) addstr(p->joined ? "+" : "-");
		addstr(p->name);

		if (turn) addstr("'s turn");
	}


	refresh();
}

void resize_handler(int sig) {
	endwin();
	refresh();
	refresh_margin();

	if (g_client.mode != mode_menu) render(&g_client);
}

int main() {
	g_client.mode = mode_menu;
	g_client.cfg = init_cfg();

	setlocale(LC_ALL, "");
	initscr();

	noecho();
	keypad(stdscr,TRUE);
	curs_set(0);
	timeout(-1);
	mousemask(ALL_MOUSE_EVENTS,NULL);

	struct sigaction sa;
	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = resize_handler;
	sigaction(SIGWINCH, &sa, NULL);

	start_color();

	for (int i=0; i<NUM_BGS; i++) {
		for (int i2=0; i2<NUM_FGS; i2++) {
			init_pair((short)(1+i*NUM_FGS + i2), FGS[i2], BGS[i]);
		}
	}

	refresh_margin();
	render(&g_client);

	while (1) {
		if (g_client.mode==mode_multiplayer)
			mtx_unlock(&g_client.lock);

		int ch = getch();

		if (g_client.mode==mode_multiplayer)
			mtx_lock(&g_client.lock);

		player_t* t = vector_get(&g_client.g.players, g_client.player);
		int off[2] = {0,0};

		int mouse=0;
		switch (ch) {
			case KEY_MOUSE: {
				MEVENT mev;
				getmouse(&mev);

				if (mev.x<g_client.centerx-g_client.g.board_w/2 || mev.x>g_client.centerx+g_client.g.board_w/2
						|| mev.y<g_client.centery-g_client.g.board_h/2 || mev.y>g_client.centery+g_client.g.board_h/2) {
					g_client.which = 0;
					continue;
				}

				board_rot_pos(&g_client.g, t->board_rot, (int[2]){mev.x - g_client.centerx+g_client.g.board_w/2, mev.y - g_client.centery+g_client.g.board_h/2}, g_client.which == 0 ? g_client.select : g_client.select2);
				mouse=1;
				break;
			}
			case KEY_LEFT: off[0]--; break;
			case KEY_RIGHT: off[0]++; break;
			case KEY_UP: off[1]--; break;
			case KEY_DOWN: off[1]++; break;
			case '\e': {
				g_client.which = 0;
				break;
			}
			case '\n': {
				g_client.which++;
				break;
			}
			default: continue;
		}

		int off_out[2]={0,0};
		if (!mouse) rot_pos(t->board_rot, off, off_out);

		if (g_client.which==0) {
			g_client.select[0]+=off_out[0]; g_client.select[1]+=off_out[1];
			g_client.select[0]=clamp(g_client.select[0],0,g_client.g.board_w);
			g_client.select[1]=clamp(g_client.select[1],0,g_client.g.board_h);
			if (mouse) {
				g_client.which++; //"enter" after click
				mouse=0;
			}

			vector_clear(&g_client.hints);

			refresh_hints(&g_client);

			memcpy(g_client.select2, g_client.select, sizeof(int[2]));
		} else if (g_client.which==1) {
			g_client.select2[0]+=off_out[0]; g_client.select2[1]+=off_out[1];
			g_client.select2[0]=clamp(g_client.select2[0],0,g_client.g.board_w);
			g_client.select2[1]=clamp(g_client.select2[1],0,g_client.g.board_h);
		}

		if ((mouse && g_client.which==1) || g_client.which==2) {
			client_make_move(&g_client);

			memcpy(g_client.select2, g_client.select, sizeof(int[2]));
			g_client.which=0;
		}

		render(&g_client);
	}

	endwin();

	return 0;
}
