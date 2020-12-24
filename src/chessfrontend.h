// Automatically generated header.

#pragma once
#include "vector.h"
typedef enum {
	mode_menu,
	mode_gamelist,
	mode_singleplayer,
	mode_multiplayer,
} client_mode_t;
#define MP_PORT 1093
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
#include "chess.h"
void write_spectators(vector_t* data, mp_extra_t* m);
void write_move(vector_t* data, move_t* m);
#include "network.h"
move_t read_move(cur_t* cur);
void game_free(game_t* g);
void mp_extra_free(mp_extra_t* m);
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
void write_game(vector_t* data, game_t* g);
void read_game(cur_t* cur, game_t* g, char* joined);
void refresh_hints(chess_client_t* client);
void set_move_cursor(game_t* g, unsigned* cur, unsigned i);
void chess_client_set_move_cursor(chess_client_t* client, unsigned i);
void chess_client_initgame(chess_client_t* client, client_mode_t mode, char make);
void pnum_leave(game_t* g, unsigned pnum);
mp_serv_t chess_client_recvmsg(chess_client_t* client, cur_t cur);
int client_make_move(chess_client_t* client);
void chess_client_undo_move(chess_client_t* client);
void chess_client_gamelist(chess_client_t* client);
void chess_client_makegame(chess_client_t* client, char* g_name, char* name);
int chess_client_joingame(chess_client_t* client, unsigned i, char* name);
void chess_client_leavegame(chess_client_t* client);
void chess_client_disconnect(chess_client_t* client);
