#include <stdio.h>
#ifdef _WIN32
#define PDC_NCMOUSE
#endif
#include <curses.h>
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

typedef struct {
	chess_client_t client;

	map_t cfg;

	int which, select_current;

	vector_t game_list_opts;

	int centerx, centery;
} chess_term_t;

const char* CFG_SERVADDR = "server";
const char* CFG_NAME = "name";
char* CFG_PATH = "termchess.txt";

map_t init_cfg() {
	map_t cfg = map_new();
	map_configure_string_key(&cfg, sizeof(config_val));
	cfg_add(&cfg, CFG_SERVADDR, cfg_str, (cfg_data){.str=NULL});
	cfg_add(&cfg, CFG_NAME, cfg_str, (cfg_data){.str=""});
	configure(&cfg, CFG_PATH);
	return cfg;
}

chess_term_t g_term;

void center(chess_term_t* term, int row, char* txt) {
	attr_t a;
	short c;
	attr_get(&a, &c, NULL);
	move(row, term->centerx-strlen(txt)/2);
	attr_set(a, c, NULL);
	addstr(txt);
}

char* inputui(chess_term_t* term, char* prompt, int sz, int noempty) {
	clear();
	curs_set(2);
	echo();

	attr_set(A_BOLD,0,NULL);
	center(term, term->centery-1, prompt);
	attr_set(A_NORMAL,0,NULL);

	char* answer = heap(sz+1);

	move(term->centery+1, term->centerx-sz/2);
	do getnstr(answer, sz); while (strlen(answer)==0 && noempty);

	return heapcpystr(answer);
}

#define ALERT_COLOR 1

void alertui(chess_term_t* term, char* msg) {
	clear();
	curs_set(0);
	noecho();

	attr_set(A_BOLD,ALERT_COLOR,NULL);
	center(term, term->centery-1, msg);
	attr_set(A_NORMAL,0,NULL);
	center(term, term->centery+1, "(press any key)");
	getch();
}

#define SELECT_COLOR 1

void selectui_refresh(chess_term_t* term, char* prompt, vector_t* options) {
	if (term->client.recv) mtx_lock(&term->client.lock);

	static char* prompt_static;
	if (prompt) prompt_static=prompt;

	term->select_current = clamp(term->select_current, 0, (int)options->length);

	clear();
	attr_set(A_BOLD,0,NULL);
	center(term, term->centery - (int)options->length/2 - 2, prompt_static);

	vector_iterator opts = vector_iterate(options);
	while (vector_next(&opts)) {
		attr_set(A_NORMAL,term->select_current==opts.i-1 ? SELECT_COLOR : 0,NULL);

		move(term->centery-(int)options->length/2+(int)opts.i-1, 0);
		clrtoeol();

		center(term, term->centery - (int)options->length/2 + (int)opts.i-1, *(char**)opts.x);
	}

	refresh();
	if (term->client.recv) mtx_unlock(&term->client.lock);
}

int selectui(chess_term_t* term, char* prompt, vector_t* options) {
	curs_set(0);
	echo();

	term->select_current=0;

	int ch = -1;
	do {
		if (ch==KEY_UP) term->select_current--; else if (ch==KEY_DOWN) term->select_current++;
		selectui_refresh(term, prompt, options);
	} while ((ch = getch()) != '\n');

	return term->select_current;
}

void setup_game(chess_term_t* term, client_mode_t mode, char* name) {
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

	int i = selectui(term, "chess", &boards);
	char* path = *(char**)vector_get(&boards_path, i);

	char* str = read_file(path);
	if (!str) alertui(term, "failed to read board");

	term->client.g = parse_board(str);

	vector_free_strings(&boards);
	vector_free_strings(&boards_path);

	chess_client_initgame(&term->client, name, mode);
	term->which = 0;
}

void refresh_margin() {
	int w,h;
	getmaxyx(stdscr, h, w);

	g_term.centerx = w/2;
	g_term.centery = h/2;
}

void render(chess_term_t* term);

void game_list_options(chess_term_t* term) {
	vector_cpy(&term->client.game_list, &term->game_list_opts);

	char* makegame = "make a game";
	vector_pushcpy(&term->game_list_opts, &makegame);
}

void setup_multiplayer(chess_term_t* term) {
	config_val* addrval = map_find(&term->cfg, &CFG_SERVADDR);

	char* addr = inputui(term, addrval->data.str ? heapstr("type new address or hit enter to use %s", addrval->data.str) : "type new server address", 24, addrval->data.str==NULL);

	if (strlen(addr)>0) {
		addrval->data.str = addr;
		addrval->is_default=0;
	} else {
		drop(addr);
	}

	term->client.net = client_connect(addrval->data.str, MP_PORT);
	if (term->client.net.err) {
		perrorx("failed to connect");
	}

	config_val* nameval = map_find(&term->cfg, &CFG_NAME);
	char* name = inputui(term, *nameval->data.str ? heapstr("type username or use %s", nameval->data.str) : "type a username or leave default", 24, 0);
	if (strlen(name)>0) {
		nameval->data.str = name;
		nameval->is_default=0;
	} else {
		drop(name);
	}

	save_configure(&term->cfg, CFG_PATH);

	term->client.mode = mode_gamelist;

	int full=0;
	mtx_init(&term->client.lock, mtx_plain);
	chess_client_startrecv(&term->client);

	mtx_lock(&term->client.lock);
	chess_client_gamelist(&term->client);
	game_list_options(term);

	do {
		mtx_unlock(&term->client.lock);
		int opt = selectui(term, "choose a game, or make one", &term->game_list_opts);

		mtx_lock(&term->client.lock);
		if (opt == term->game_list_opts.length-1) {
			term->client.mode = mode_menu;
			char* g_name = inputui(term, "name your game", 24, 1);
			if (strlen(g_name)==0) {
				drop(g_name);
				g_name = nameval->data.str;
			}

			setup_game(term, mode_multiplayer, *nameval->data.str ? nameval->data.str : NULL);
			chess_client_makegame(&term->client, g_name);
		} else {
			if (!chess_client_joingame(&term->client, opt, nameval->data.str ? nameval->data.str : NULL)) {
				full=1;
				alertui(term, "that game is full, take another gamble");
			} else {
				term->which = 0;
				full=0;
			}
		}
	} while (full);

	mtx_unlock(&term->client.lock);
	term->client.mode = mode_multiplayer;
}

void render(chess_term_t* term) {
	if (term->client.mode == mode_menu) { //blocks here
		char* menu[] = {"local", "multiplayer"};
		int mode = selectui(term, "termchess", &(vector_t){.data=(char*)&menu, .size=sizeof(char*), .length=2});
		if (mode==0) {
			setup_game(term, mode_singleplayer, NULL);
		} else if (mode==1) {
			setup_multiplayer(term);
		}

		refresh_margin();
	} else if (term->client.mode == mode_gamelist) { //game list change
		game_list_options(term);
		selectui_refresh(term, NULL, &term->game_list_opts);
		return;
	}

	erase();

	curs_set(0);
	noecho();

	int x_margin = term->centerx-term->client.g.board_w/2;
	int y_margin = term->centery-term->client.g.board_h/2;

	player_t* t = vector_get(&term->client.g.players, term->client.player);

	int pos[2];
	for (pos[1]=0; pos[1]<term->client.g.board_h; pos[1]++) {
		move(y_margin+pos[1], x_margin);

		for (pos[0]=0; pos[0]<term->client.g.board_w; pos[0]++) {
			int bpos[2];
			board_rot_pos(&term->client.g, t->board_rot, pos, bpos);
			piece_t* p = board_get(&term->client.g, bpos);

			int bg;
			if (bpos[0] == term->client.select.from[0] && bpos[1] == term->client.select.from[1]) {
				bg = 2;
			} else if (bpos[0] == term->client.select.to[0] && bpos[1] == term->client.select.to[1]) {
				bg = 3;
			} else if (vector_search(&term->client.hints, &p)!=0) {
				bg = 4;
			} else {
				bg = (bpos[0]+bpos[1])%2;
			}

			attr_set(A_BOLD, 1 + bg*NUM_FGS + player_col(p->player), NULL);
			addstr(piece_str(p));
		}
	}

	vector_iterator t_iter = vector_iterate(&term->client.g.players);
	while (vector_next(&t_iter)) {
		player_t* p = t_iter.x;
		int txt_pos[2];

		int rel_rot = p->board_rot-t->board_rot;
		if (rel_rot<0) rel_rot=4+rel_rot;
		rel_rot %= 4;

		int won = term->client.g.won && t_iter.i-1==term->client.g.player;
		int turn = t_iter.i-1==term->client.g.player;
		int slen = (int)(strlen(p->name)+turn*strlen("'s turn"))+won+(term->client.mode==mode_multiplayer);

		switch (rel_rot) {
			case 0: txt_pos[0]=0; txt_pos[1]=term->client.g.board_h; break;
			case 2: txt_pos[0]=term->client.g.board_w-slen; txt_pos[1]=-1; break;
			case 1: txt_pos[0]=term->client.g.board_w; txt_pos[1]=term->client.g.board_h-1; break;
			case 3: txt_pos[0]=-slen; txt_pos[1]=0; break;
		}

		move(y_margin+txt_pos[1],txt_pos[0]+x_margin);
		attr_set(A_NORMAL, 1 + abs((txt_pos[0]+txt_pos[1])%2)*NUM_FGS + player_col(t_iter.i-1), NULL);

		if (won) addstr( "👑");
		if (term->client.mode==mode_multiplayer) addstr(p->joined ? "+" : "-");
		addstr(p->name);

		if (turn) addstr("'s turn");
	}

	refresh();
}

int main(int argc, char** argv) {
	g_term.client.render = (void(*)(void*))render;
	g_term.client.arg = &g_term;
	g_term.client.recv=0;
	
	g_term.client.mode = mode_menu;
	g_term.cfg = init_cfg();

	setlocale(LC_ALL, "");
	initscr();

	noecho();
	keypad(stdscr,TRUE);
	curs_set(0);
	timeout(-1);
	mousemask(ALL_MOUSE_EVENTS,NULL);

	start_color();

	for (int i=0; i<NUM_BGS; i++) {
		for (int i2=0; i2<NUM_FGS; i2++) {
			init_pair((short)(1+i*NUM_FGS + i2), FGS[i2], BGS[i]);
		}
	}

	refresh_margin();

	while (1) {
		if (g_term.client.recv) mtx_unlock(&g_term.client.lock);
		render(&g_term);

		int ch = getch();

		if (g_term.client.recv) mtx_lock(&g_term.client.lock);

		player_t* t = vector_get(&g_term.client.g.players, g_term.client.player);
		int off[2] = {0,0};

		int mouse=0;
		switch (ch) {
			case KEY_RESIZE: {
				endwin();
				refresh();
				refresh_margin();

				if (g_term.client.mode != mode_menu) render(&g_term);
				break;
			}
			case KEY_MOUSE: {
				MEVENT mev;
				getmouse(&mev);

				if (mev.x<g_term.centerx-g_term.client.g.board_w/2 || mev.x>g_term.centerx+g_term.client.g.board_w/2
						|| mev.y<g_term.centery-g_term.client.g.board_h/2 || mev.y>g_term.centery+g_term.client.g.board_h/2) {
					g_term.which = 0;
					continue;
				}

				board_rot_pos(&g_term.client.g, t->board_rot, (int[2]){mev.x - g_term.centerx+g_term.client.g.board_w/2, mev.y - g_term.centery+g_term.client.g.board_h/2}, g_term.which == 0 ? g_term.client.select.from : g_term.client.select.to);
				mouse=1;
				break;
			}
			case KEY_LEFT: off[0]--; break;
			case KEY_RIGHT: off[0]++; break;
			case KEY_UP: off[1]--; break;
			case KEY_DOWN: off[1]++; break;
			case '\e': {
				g_term.which = 0;
				break;
			}
			case '\n': {
				g_term.which++;
				break;
			}
			default: continue;
		}

		int off_out[2]={0,0};
		if (!mouse) rot_pos(t->board_rot, off, off_out);

		if (g_term.which==0) {
			g_term.client.select.from[0]+=off_out[0]; g_term.client.select.from[1]+=off_out[1];
			g_term.client.select.from[0]=clamp(g_term.client.select.from[0], 0, g_term.client.g.board_w);
			g_term.client.select.from[1]=clamp(g_term.client.select.from[1], 0, g_term.client.g.board_h);
			if (mouse) {
				g_term.which++; //"enter" after click
				mouse=0;
			}

			refresh_hints(&g_term.client);

			memcpy(g_term.client.select.to, g_term.client.select.from, sizeof(int[2]));
		} else if (g_term.which==1) {
			g_term.client.select.to[0]+=off_out[0]; g_term.client.select.to[1]+=off_out[1];
			g_term.client.select.to[0]=clamp(g_term.client.select.to[0], 0, g_term.client.g.board_w);
			g_term.client.select.to[1]=clamp(g_term.client.select.to[1], 0, g_term.client.g.board_h);
		}

		if ((mouse && g_term.which==1) || g_term.which==2) {
			client_make_move(&g_term.client);

			memcpy(g_term.client.select.to, g_term.client.select.from, sizeof(int[2]));
			g_term.which=0;
		}
	}

	endwin();

	return 0;
}
