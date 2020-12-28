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

// i cant make a thousand structs (some of which have nothing other than a flexible array)
// to document the "protocol" so i guess we use comments to indicate what goes after the op

typedef enum {
	mp_list_game, //nothing
	mp_make_game, //game name, players, board
	mp_join_game, //game id, player name
	mp_make_move, //move_t
	mp_leave_game, //nothing
	mp_undo_move
} mp_client_t;

typedef enum {
	mp_game_list, //unsigned games, game names
	mp_game_list_new, //game name
	mp_game_list_removed, //unsigned game

	mp_game_full,
	mp_game_made,

	mp_game, //players, board, moves, player #

	mp_game_joined, //join
	mp_move_made, //packed move_t
	mp_game_left, //unsigned player

	mp_move_undone
} mp_serv_t;

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

void read_players(cur_t* cur, game_t* g, char* joined) {
	if (joined) *joined = -1;
	g->players = vector_new(sizeof(player_t));
	char num_players = read_chr(cur);
	if (num_players==0) cur->err=1;
	g->last_player = read_chr(cur);
	g->player = read_chr(cur);

	for (char i=0; i<num_players; i++) {
		player_t* p = vector_push(&g->players);
		p->board_rot = read_int(cur);
		p->check = read_chr(cur);
		p->ai = read_chr(cur);
		p->mate = read_chr(cur);
		p->joined = read_chr(cur);
		if (joined && !p->ai && p->joined) *joined = i;
		p->name = read_str(cur);

		p->allies = vector_new(1);
		char len = read_chr(cur);
		for (char k=0; k<len; k++) {
			char ally = read_chr(cur);
			vector_pushcpy(&p->allies, &ally);
		}
	}
}

void read_spectators(cur_t* cur, mp_extra_t* extra) {
	unsigned slen = read_uint(cur);
	extra->spectators = vector_new(sizeof(char*));
	for (unsigned i=0; i<slen; i++) {
		char* name = read_str(cur);
		vector_pushcpy(&extra->spectators, &name);
	}
}

void write_spectators(vector_t* data, mp_extra_t* m) {
	write_uint(data, m->spectators.length);
	vector_iterator spec_iter = vector_iterate(&m->spectators);
	while (vector_next(&spec_iter)) {
		write_str(data, *(char**)spec_iter.x);
	}
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
	g->board_w = read_int(cur);
	g->board_h = read_int(cur);
	g->board = vector_new(sizeof(piece_t));

	for (int i=0; i<g->board_w*g->board_h; i++) {
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

			pp->king = board_i(g, p);
		}
	}
}

void read_initboard(cur_t* cur, game_t* g) {
	g->init_board = vector_new(sizeof(piece_t));
	for (int i=0; i<g->board_w*g->board_h; i++) {
		piece_t* p = vector_push(&g->init_board);
		p->ty = (piece_ty)read_uchr(cur);
		p->flags = (piece_flags_t)read_uint(cur);
		p->player = read_chr(cur);
	}
}

void write_move(vector_t* data, move_t* m) {
	write_int(data, m->from[0]);
	write_int(data, m->from[1]);
	write_int(data, m->to[0]);
	write_int(data, m->to[1]);
}

move_t read_move(cur_t* cur) {
	return (move_t){.from={read_int(cur), read_int(cur)}, .to={read_int(cur), read_int(cur)}};
}

void write_moves(vector_t* data, game_t* g) {
	write_uint(data, g->moves.length);
	vector_iterator m_iter = vector_iterate(&g->moves);
	while (vector_next(&m_iter)) write_move(data, m_iter.x);
}

void read_moves(cur_t* cur, game_t* g) {
	unsigned moves = read_uint(cur);
	g->moves = vector_new(sizeof(move_t));
	for (unsigned i=0; i<moves; i++)
		vector_pushcpy(&g->moves, &(move_t[]){read_move(cur)});
}

void game_free(game_t* g) {
	vector_iterator p_iter = vector_iterate(&g->players);
	while (vector_next(&p_iter)) {
		player_t* p = p_iter.x;
		drop(p->name);
	}

	vector_free(&g->players);
	vector_free(&g->board);
	vector_free(&g->init_board);
	vector_free(&g->moves);
}

void mp_extra_free(mp_extra_t* m) {
	vector_free(&m->spectators);
}

typedef struct {
	client_mode_t mode;

	client_t* net;

	game_t g;

	char player;
	unsigned move_cursor;
	vector_t spectators;
	char spectating;

	vector_t game_list;

	int recv;

	vector_t hints; //highlight pieces
	move_t select;
} chess_client_t;

void write_game(vector_t* data, game_t* g) {
	write_uint(data, (unsigned)g->flags);
	write_players(data, g);
	write_board(data, g);
	write_boardvec(data, &g->init_board);
	write_moves(data, g);
}

void read_game(cur_t* cur, game_t* g, char* joined) {
	g->won=0;
	g->flags = read_uint(cur);
	read_players(cur, g, joined);
	read_board(cur, g);
	read_initboard(cur, g);
	read_moves(cur, g);
}

//run whenever select changes or game update
void refresh_hints(chess_client_t* client) {
	piece_t* p = board_get(&client->g, client->select.from);
	vector_clear(&client->hints);
	if (!p) return;
	if (p->player==client->player) {
		piece_moves(&client->g, p, &client->hints);
	}
}

void set_move_cursor(game_t* g, unsigned* cur, unsigned i) {
	if (i<*cur) {
		*cur=0;
		vector_free(&g->board);
		vector_cpy(&g->init_board, &g->board);
	}

	for (;*cur<i;(*cur)++) {
		move_t* m = vector_get(&g->moves, *cur);
		move_swap(g, m);
	}
}

void chess_client_set_move_cursor(chess_client_t* client, unsigned i) {
	set_move_cursor(&client->g, &client->move_cursor, i);
}

void chess_client_ai(chess_client_t* client) {
	while (!client->g.won) {
		player_t* p = vector_get(&client->g.players, client->g.player);
		if (!p->ai) break;

		ai_make_move(&client->g, NULL);
	}

	client->move_cursor = client->g.moves.length;
	if (!client->g.won) client->player = client->g.player;
}

void chess_client_initgame(chess_client_t* client, client_mode_t mode, char make) {
	client->mode = mode;

	if (make) {
		chess_client_ai(client);

		client->spectating = 0;
		client->g.m.spectators = vector_new(sizeof(char*));
	}

	client->hints = vector_new(sizeof(int[2]));
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
}

mp_serv_t chess_client_recvmsg(chess_client_t* client, cur_t cur) {
	mp_serv_t msg = (mp_serv_t)read_chr(&cur);

	switch (msg) {
		case mp_game: {
			read_game(&cur, &client->g, NULL);
			read_spectators(&cur, &client->g.m);

			unsigned pnum = read_uint(&cur);
			if (pnum >= client->g.players.length) {
				client->spectating=1;
				client->player=0;
			} else {
				client->spectating=0;
				client->player = (char)pnum;
			}

			if (client->mode != mode_multiplayer) {
				chess_client_initgame(client, mode_multiplayer, 0);
				chess_client_set_move_cursor(client, client->g.moves.length);
			}

			break;
		}
		case mp_game_list: {
			unsigned games = read_uint(&cur);
			for (unsigned i=0; i<games; i++) {
				vector_pushcpy(&client->game_list, &(char*){read_str(&cur)});
			}

			break;
		}
		case mp_game_list_new: {
			char* g_name = read_str(&cur);
			vector_pushcpy(&client->game_list, &g_name);
			break;
		}
		case mp_game_list_removed: {
			drop(vector_removeptr(&client->game_list, read_uint(&cur)));
			break;
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

int client_make_move(chess_client_t* client) {
	if (client->spectating || client->player != client->g.player
			|| client->move_cursor!=client->g.moves.length) return 0;

	if (vector_search(&client->hints, client->select.to)!=-1) {
		make_move(&client->g, &client->select, 0, 1, client->player);

		if (client->mode==mode_singleplayer) {
			chess_client_ai(client);
			refresh_hints(client);
		} else if (client->mode==mode_multiplayer) {
			client->move_cursor++;

			vector_t data = vector_new(1);
			vector_pushcpy(&data, &(char){mp_make_move});
			write_move(&data, &client->select);

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
	}

	chess_client_moveundone(client);
}

void chess_client_gamelist(chess_client_t* client) {
	client->game_list = vector_new(sizeof(char*));
	client_send(client->net, &(vector_t){.data=(char[]){mp_list_game}, .length=1});
}

void chess_client_makegame(chess_client_t* client, char* g_name, char* name) {
	vector_free_strings(&client->game_list); //free here, in joingame, free at mp_game in case of full

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
	vector_free_strings(&client->game_list);
	client->net = NULL;
}
