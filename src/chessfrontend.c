#include "ai.h"
#include "chess.h"
#include "network.h"
#include "vector.h"

typedef enum {
	mode_menu,
	mode_gamelist,
	mode_singleplayer,
	mode_multiplayer,
} client_mode_t;

#define MP_PORT 1093
#define PLAYERNAME_MAXLEN 20
#define GAMENAME_MAXLEN 20

// i cant make a thousand structs (some of which have nothing other than a flexible array)
// to document the "protocol" so i guess we use comments to indicate what goes after the op

typedef enum {
	mp_list_game, //nothing
	mp_make_game, //game name, players, board
	mp_join_game, //game id, player name
	mp_make_move, //move_t
	mp_ai_move,
	mp_leave_game, //nothing
	mp_undo_move
} mp_client_t;

typedef enum {
	mp_game_list, //unsigned games, game names
	mp_game_list_new, //game name
	mp_game_list_full,
	mp_game_list_removed, //unsigned game

	mp_game_made,

	mp_game, //players, board, moves, player #

	mp_game_joined, //join
	mp_move_made, //packed move_t
	mp_game_left, //unsigned player, unsigned host (new host)

	mp_move_undone
} mp_serv_t;

void players_free(game_t* g) {
	vector_iterator p_iter = vector_iterate(&g->players);
	while (vector_next(&p_iter)) {
		player_t* p = p_iter.x;
		drop(p->name);
		vector_free(&p->allies);
		vector_free(&p->kings);
	}

	vector_free(&g->players);
}

void game_free(game_t* g) {
	vector_free(&g->promote_from);
	vector_free(&g->castleable);

	players_free(g);
	vector_free(&g->board);
	vector_free(&g->init_board);
	vector_free(&g->moves);
}

void write_players(vector_t* data, game_t* g) {
	vector_pushcpy(data, &(char){(char)g->players.length});
	vector_pushcpy(data, &(char){g->last_player});
	vector_pushcpy(data, &(char){g->player});

	vector_iterator p_iter = vector_iterate(&g->players);
	while (vector_next(&p_iter)) {
		player_t* p = p_iter.x;
		write_int(data, p->board_rot);
		vector_pushcpy(data, &p->check);
		vector_pushcpy(data, &p->ai);
		vector_pushcpy(data, &p->mate);
		vector_pushcpy(data, &p->joined);
		write_str(data, p->name);

		vector_pushcpy(data, &(char){(char)p->allies.length});
		vector_iterator a_iter = vector_iterate(&p->allies);
		while (vector_next(&a_iter)) vector_pushcpy(data, a_iter.x);
	}
}

void read_players(cur_t* cur, game_t* g, char* joined, char* full) {
	if (joined) *joined = -1;
	if (full) *full = 1;

	g->players = vector_new(sizeof(player_t));
	char num_players = read_chr(cur);
	if (num_players<=0) cur->err=1;
	g->last_player = read_chr(cur);
	g->player = read_chr(cur);

	for (char i=0; i<num_players; i++) {
		if (cur->err) {
			return;
		}

		player_t* p = vector_push(&g->players);
		p->board_rot = read_int(cur);
		p->check = read_chr(cur);
		p->ai = read_chr(cur);
		p->mate = read_chr(cur);
		p->joined = read_chr(cur);

		if (joined && !p->ai && p->joined) *joined = i;
		else if (full && !p->ai) *full = 0;

		p->name = read_str(cur);

		if (cur->err==0 && strlen(p->name)>PLAYERNAME_MAXLEN) {
			cur->err=1;
		}

		p->kings = vector_new(sizeof(int));
		vector_pushcpy(&p->kings, &(int){-1});

		p->allies = vector_new(1);
		char len = read_chr(cur);
		for (char k=0; k<len; k++) {
			if (cur->err) {
				return;
			}

			char ally = read_chr(cur);
			vector_pushcpy(&p->allies, &ally);
		}
	}
}

void read_mp_extra(cur_t* cur, mp_extra_t* extra) {
	unsigned slen = read_uint(cur);
	extra->spectators = vector_new(sizeof(char*));
	for (unsigned i=0; i<slen; i++) {
		if (cur->err) return;

		char* name = read_str(cur);
		vector_pushcpy(&extra->spectators, &name);
	}

	extra->host = read_uint(cur);
}

void write_mp_extra(vector_t* data, mp_extra_t* extra) {
	write_uint(data, extra->spectators.length);
	vector_iterator spec_iter = vector_iterate(&extra->spectators);
	while (vector_next(&spec_iter)) {
		write_str(data, *(char**)spec_iter.x);
	}

	write_uint(data, extra->host);
}

void write_boardvec(vector_t* data, vector_t* board) {
	vector_iterator b_iter = vector_iterate(board);
	while (vector_next(&b_iter)) {
		piece_t* p = b_iter.x;
		//in future htons might be required
		write_uchr(data, (unsigned char)p->ty);
		write_uint(data, (unsigned)p->flags);
		vector_pushcpy(data, &p->player);
	}
}

void write_board(vector_t* data, game_t* g) {
	write_int(data, g->board_w);
	write_int(data, g->board_h);
	write_boardvec(data, &g->board);
}

void read_board(cur_t* cur, game_t* g) {
	g->board = vector_new(sizeof(piece_t));
	g->board_w = read_int(cur);
	g->board_h = read_int(cur);

	for (int i=0; i<g->board_w*g->board_h; i++) {
		if (cur->err) return;

		piece_t* p = vector_push(&g->board);
		p->ty = (piece_ty)read_uchr(cur);
		p->flags = (piece_flags_t)read_uint(cur);
		p->player = read_chr(cur);

		if (p->ty == p_king) {
			player_t* pp = vector_get(&g->players, p->player);
			if (!pp) {
				cur->err=1;
				return;
			}

			vector_insertcpy(&pp->kings, 0, &(int){board_i(g, p)});
		}
	}
}

void read_initboard(cur_t* cur, game_t* g) {
	g->init_board = vector_new(sizeof(piece_t));
	for (int i=0; i<g->board_w*g->board_h; i++) {
		if (cur->err) return;

		piece_t* p = vector_push(&g->init_board);
		p->ty = (piece_ty)read_uchr(cur);
		p->flags = (piece_flags_t)read_uint(cur);
		p->player = read_chr(cur);
	}
}

void write_move(vector_t* data, move_t* m) {
	if (m->castle[0]==-1) {
		write_int(data, -1);
	} else {
		write_int(data, m->castle[0]);
		write_int(data, m->castle[1]);
	}

	write_int(data, m->from[0]);
	write_int(data, m->from[1]);
	write_int(data, m->to[0]);
	write_int(data, m->to[1]);
}

move_t read_move(cur_t* cur) {
	move_t m;

	m.castle[0]=read_int(cur);
	if (m.castle[0]!=-1) {
		m.castle[1]=read_int(cur);
	}

	m.from[0]=read_int(cur); m.from[1]=read_int(cur);
	m.to[0]=read_int(cur); m.to[1]=read_int(cur);
	return m;
}

void write_moves(vector_t* data, game_t* g) {
	write_uint(data, g->moves.length);
	vector_iterator m_iter = vector_iterate(&g->moves);
	while (vector_next(&m_iter)) write_move(data, m_iter.x);
}

void read_moves(cur_t* cur, game_t* g) {
	unsigned moves = read_uint(cur);
	g->moves = vector_new(sizeof(move_t));
	for (unsigned i=0; i<moves; i++) {
		if (cur->err) return;
		vector_pushcpy(&g->moves, &(move_t[]){read_move(cur)});
	}
}

void write_game(vector_t* data, game_t* g) {
	write_uint(data, (unsigned)g->flags);

	vector_pushcpy(data, &(char){(char)g->promote_from.length});
	vector_iterator promote_iter = vector_iterate(&g->promote_from);
	while (vector_next(&promote_iter)) vector_pushcpy(data, promote_iter.x);

	vector_pushcpy(data, &(char){(char)g->promote_to});

	vector_pushcpy(data, &(char){(char)g->castleable.length});
	vector_iterator castle_iter = vector_iterate(&g->castleable);
	while (vector_next(&castle_iter)) vector_pushcpy(data, castle_iter.x);

	write_players(data, g);
	write_board(data, g);
	write_boardvec(data, &g->init_board);
	write_moves(data, g);
}

void read_game(cur_t* cur, game_t* g, char* joined, char* full) {
	g->won=0;
	g->flags = read_uint(cur);

	g->promote_from = vector_new(1);
	char pfrom = read_chr(cur);
	for (char i=0; i<pfrom; i++) {
		if (cur->err) {
			vector_free(&g->promote_from);
			return;
		}

		vector_pushcpy(&g->promote_from, &(char){read_chr(cur)});
	}

	g->promote_to = (piece_ty)read_chr(cur);

	g->castleable = vector_new(1);
	char castleable = read_chr(cur);
	for (char i=0; i<castleable; i++) {
		if (cur->err) {
			vector_free(&g->promote_from);
			vector_free(&g->castleable);
			return;
		}

		vector_pushcpy(&g->castleable, &(char){read_chr(cur)});
	}

	read_players(cur, g, joined, full);
	read_board(cur, g);
	read_initboard(cur, g);
	read_moves(cur, g);

	if (cur->err) {
		game_free(g);
	}
}

void mp_extra_free(mp_extra_t* m) {
	vector_free(&m->spectators);
}

typedef struct {
	char full;
	char* name;
} game_listing_t;

typedef struct {
	client_mode_t mode;

	client_t* net;

	game_t g;

	unsigned pnum;
	char player;
	unsigned move_cursor;
	vector_t spectators;
	char spectating;

	vector_t game_list;

	int recv;

	vector_t hints; //highlight pieces
	struct {int from[2]; int to[2];} select;
} chess_client_t;

//run whenever select changes or game update
void refresh_hints(chess_client_t* client) {
	piece_t* p = board_get(&client->g, client->select.from);
	vector_clear(&client->hints);
	if (!p) return;
	if (p->player==client->player) {
		piece_moves(&client->g, p, &client->hints, 1);
	}
}

void set_move_cursor(game_t* g, unsigned* cur, unsigned i) {
	if (i<*cur) {
		*cur=0;
		vector_free(&g->board);
		vector_cpy(&g->init_board, &g->board);

		vector_iterator p_iter = vector_iterate(&g->players);
		while (vector_next(&p_iter)) {
			player_t* p = p_iter.x;
			vector_clear(&p->kings);
			vector_pushcpy(&p->kings, &(int){-1});
		}

		int pos[2] = {-1, 0};
		while (board_pos_next(g, pos)) {
			piece_t* k = board_get(g, pos);
			if (k->ty==p_king) {
				player_t* p = vector_get(&g->players, k->player);
				vector_insertcpy(&p->kings, 0, &(int){pos_i(g, pos)});
			}
		}
	}

	for (;*cur<i;(*cur)++) {
		move_t* m = vector_get(&g->moves, *cur);
		move_swap(g, m);
	}
}

void chess_client_set_move_cursor(chess_client_t* client, unsigned i) {
	set_move_cursor(&client->g, &client->move_cursor, i);
}

int chess_client_ai(chess_client_t* client) {
	int ret=0;
	move_t m;

	while (!client->g.won) {
		player_t* p = vector_get(&client->g.players, client->g.player);
		if (!p->ai) break;

		ai_make_move(&client->g, &m);
		ret=1;

		if (client->mode==mode_multiplayer) {
			vector_t data = vector_new(1);

			vector_pushcpy(&data, &(char){mp_ai_move});
			write_move(&data, &m);
			client_send(client->net, &data);

			vector_free(&data);
		}
	}

	client->move_cursor = client->g.moves.length;
	if (client->mode==mode_singleplayer && !client->g.won) client->player = client->g.player;
	return ret;
}

void chess_client_initgame(chess_client_t* client, client_mode_t mode, char make) {
	client->mode = mode;

	if (make) {
		chess_client_ai(client);

		client->spectating = 0;
		client->g.m.spectators = vector_new(sizeof(char*));

		client->pnum = (unsigned)client->player;
		client->g.m.host = client->pnum;
	}

	client->hints = vector_new(sizeof(move_t));
}

void pnum_leave(game_t* g, unsigned pnum) {
	if (pnum<g->players.length) {
		player_t* p = vector_get(&g->players, pnum);
		p->joined=0;
	} else {
		drop(vector_removeptr(&g->m.spectators, pnum-g->players.length));
	}
}

void chess_client_moveundone(chess_client_t* client) {
	if (client->move_cursor==client->g.moves.length) {
		chess_client_set_move_cursor(client, client->g.last_move);
	}

	undo_move(&client->g);
	refresh_hints(client);
}

void chess_client_gamelist_free(chess_client_t* client) {
	vector_iterator gl_iter = vector_iterate(&client->game_list);
	while (vector_next(&gl_iter)) {
		game_listing_t* gl = gl_iter.x;
		drop(gl->name);
	}

	vector_free(&client->game_list);
}

mp_serv_t chess_client_recvmsg(chess_client_t* client, cur_t cur) {
	mp_serv_t msg = (mp_serv_t)read_chr(&cur);

	switch (msg) {
		case mp_game: {
			read_game(&cur, &client->g, NULL, NULL);
			read_mp_extra(&cur, &client->g.m);

			client->pnum = read_uint(&cur);
			if (client->pnum >= client->g.players.length) {
				client->spectating=1;
				client->player=0;
			} else {
				client->spectating=0;
				client->player = (char)client->pnum;
			}

			if (client->mode != mode_multiplayer) {
				chess_client_gamelist_free(client);
				chess_client_initgame(client, mode_multiplayer, 0);
				client->move_cursor = client->g.moves.length;
			}

			break;
		}
		case mp_game_list: {
			unsigned games = read_uint(&cur);
			for (unsigned i=0; i<games; i++) {
				vector_pushcpy(&client->game_list, &(game_listing_t){.full=read_chr(&cur), .name=read_str(&cur)});
			}

			break;
		}
		case mp_game_list_new: {
			vector_pushcpy(&client->game_list, &(game_listing_t){.full=read_chr(&cur), .name=read_str(&cur)});
			break;
		}
		case mp_game_list_removed: {
			game_listing_t* gl = vector_get(&client->game_list, read_uint(&cur));
			drop(gl->name);
			vector_remove_element(&client->game_list, gl);
			break;
		}
		case mp_game_list_full: {
			game_listing_t* gl = vector_get(&client->game_list, read_uint(&cur));
			gl->full = read_chr(&cur);
		}
		case mp_game_joined: {
			unsigned player = read_uint(&cur);
			char* name = read_str(&cur);
			if (player>=client->g.players.length) {
				vector_pushcpy(&client->g.m.spectators, &name);
			} else {
				player_t* p = vector_get(&client->g.players, player);
				drop(p->name);
				p->name = name;
				p->joined = 1;
			}

			break;
		}
		case mp_game_left: {
			unsigned pnum = read_uint(&cur);
			pnum_leave(&client->g, pnum);
			client->g.m.host = read_uint(&cur);
			break;
		}
		case mp_move_made: {
			move_t m = read_move(&cur);
			if (client->move_cursor==client->g.moves.length) {
				make_move(&client->g, &m, 0, 1, client->g.player);
				client->move_cursor++;
				refresh_hints(client);
			} else {
				make_move(&client->g, &m, 0, 0, client->g.player);
			}

			break;
		}
		case mp_move_undone: {
			chess_client_moveundone(client);
			break;
		}
		default:;
	}

	drop(cur.start);

	return msg;
}

move_t* client_hint_search(chess_client_t* client, int to[2]) {
	vector_iterator hint_iter = vector_iterate(&client->hints);
	while (vector_next(&hint_iter)) {
		move_t* m = hint_iter.x;
		if (i2eq(m->to, to) || (m->castle[0]!=-1 && i2eq(m->castle, to))) {
			return m;
		}
	}

	return NULL;
}

int client_make_move(chess_client_t* client) {
	if (client->spectating || client->player != client->g.player
			|| client->move_cursor!=client->g.moves.length) return 0;

	move_t* m;
	if ((m=client_hint_search(client, client->select.to))) {
		make_move(&client->g, m, 0, 1, client->player);

		if (client->mode==mode_multiplayer) {
			client->move_cursor++;

			vector_t data = vector_new(1);
			vector_pushcpy(&data, &(char){mp_make_move});
			write_move(&data, m);

			client_send(client->net, &data);
			vector_free(&data);
		}

		return 1;
	} else {
		return 0;
	}
}

void chess_client_undo_move(chess_client_t* client) {
	if (client->mode==mode_multiplayer) {
		vector_t data = vector_new(1);
		vector_pushcpy(&data, &(char){mp_undo_move});
		client_send(client->net, &data);
		vector_free(&data);
	} else {
		client->player = client->g.last_player;
	}

	chess_client_moveundone(client);
}

void chess_client_gamelist(chess_client_t* client) {
	client->game_list = vector_new(sizeof(game_listing_t));
	client_send(client->net, &(vector_t){.data=(char[]){mp_list_game}, .length=1});
}

void chess_client_makegame(chess_client_t* client, char* g_name, char* name) {
	chess_client_gamelist_free(client);

	player_t* t = vector_get(&client->g.players, client->player);
	t->joined = 1;
	if (name) t->name = heapcpystr(name);

	vector_t data = vector_new(1);

	vector_pushcpy(&data, &(char){mp_make_game});

	write_str(&data, g_name);
	write_game(&data, &client->g);

	client_send(client->net, &data);

	vector_free(&data);
}

int chess_client_joingame(chess_client_t* client, unsigned i, char* name) {
	vector_t data = vector_new(1);

	vector_pushcpy(&data, &(char){mp_join_game});
	write_uint(&data, i);
	write_str(&data, name ? name : "");

	client_send(client->net, &data);

	vector_free(&data);

	return 1;
}

void chess_client_leavegame(chess_client_t* client) {
	if (client->mode==mode_multiplayer) {
		vector_t data = vector_new(1);

		vector_pushcpy(&data, &(char){mp_leave_game});
		client_send(client->net, &data);
		vector_free(&data);
		mp_extra_free(&client->g.m);
	}

	vector_free(&client->hints);
	game_free(&client->g);
}

void chess_client_disconnect(chess_client_t* client) {
	client_free(client->net);
	chess_client_gamelist_free(client);
	client->net = NULL;
}
