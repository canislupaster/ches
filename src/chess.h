// Automatically generated header.

#pragma once
#include "vector.h"
#include "hashtable.h"
#include "cfg.h"
typedef enum {
	p_king,
	p_queen,
	p_rook,
	p_bishop,
	p_knight,
	p_pawn,
	p_archibishop,
	p_chancellor,
	p_empty,
	p_blocked
} piece_ty;
typedef enum {
	pawn_x = 1,
	pawn_nx = 2,
	pawn_y = 4,
	pawn_ny = 8,
	pawn_firstmv = 16,
	pawn_firstmv_swp = 32,
	pawn_promoted = 64
} piece_flags_t;
typedef struct {
	piece_ty ty;
	piece_flags_t flags;
	char player;
} piece_t;
typedef struct __attribute__ ((__packed__)) {
	int from[2];
	int to[2];
} move_t;
typedef struct {
	int board_rot;
	int check, mate;

	int king;
	char* name;
	int joined;

	vector_t allies;
} player_t;
typedef enum {
	game_pawn_promotion=1
} game_flags_t;
typedef struct {
	game_flags_t flags;

	int board_w, board_h;
	vector_t init_board;
	vector_t board;
	vector_t moves;
	char player; //of current move
	char won;

	vector_t players;
	vector_t spectators;

	piece_t piece_swap; //emulating moves is needed when evaluating possible moves or creating a game tree, maybe later augment into a stack
} game_t;
piece_t* board_get(game_t* g, int x[2]);
void board_pos(game_t* g, int pos[2], piece_t* ptr);
void board_rot_pos(game_t* g, int rot, int pos[2], int pos_out[2]);
void move_swap(game_t* g, move_t* m);
void unmove_swap(game_t* g, piece_t* from, piece_t* to);
void piece_moves(game_t* g, piece_t* p, vector_t* moves);
void next_player(game_t* g, int next);
enum {
	move_invalid,
	move_turn,
	move_player,
	move_success
} make_move(game_t* g, move_t* m, int validate, int make, char player);
game_t parse_board(char* str);
char* move_pgn(game_t* g, move_t* m);
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
	mp_game_left //unsigned player
} mp_serv_t;
void write_spectators(vector_t* data, game_t* g);
void write_move(vector_t* data, move_t* m);
#include "network.h"
move_t read_move(cur_t* cur);
void game_free(game_t* g);
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
void set_move_cursor(chess_client_t* client, unsigned i);
void chess_client_initgame(chess_client_t* client, client_mode_t mode, char make);
void pnum_leave(game_t* g, unsigned pnum);
mp_serv_t chess_client_recvmsg(chess_client_t* client, cur_t cur);
int client_make_move(chess_client_t* client);
void chess_client_gamelist(chess_client_t* client);
void chess_client_makegame(chess_client_t* client, char* g_name, char* name);
int chess_client_joingame(chess_client_t* client, unsigned i, char* name);
void chess_client_leavegame(chess_client_t* client);
void chess_client_disconnect(chess_client_t* client);
