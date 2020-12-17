// Automatically generated header.

#pragma once
#include "threads.h"
#include "vector.h"
#include "hashtable.h"
#include "cfg.h"
typedef enum {
	p_empty,
	p_blocked,
	p_king,
	p_queen,
	p_rook,
	p_bishop,
	p_knight,
	p_pawn
} piece_ty;
typedef enum {
	pawn_x = 1,
	pawn_nx = 2,
	pawn_y = 4,
	pawn_ny = 8,
	pawn_firstmv = 16,
	pawn_firstmv_swp = 32
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
typedef struct {
	int board_w, board_h;
	vector_t board;
	vector_t moves;
	char player; //of current move
	char won;
	vector_t players;

	piece_t piece_swap; //emulating moves is needed when evaluating possible moves or creating a game tree, maybe later augment into a stack
} game_t;
piece_t* board_get(game_t* g, int x[2]);
void board_rot_pos(game_t* g, int rot, int pos[2], int pos_out[2]);
void rot_pos(int rot, int pos[2], int pos_out[2]);
char* piece_str(piece_t* p);
int player_col(char p);
enum {
	move_invalid,
	move_turn,
	move_player,
	move_success
} make_move(game_t* g, move_t* m, int validate, char player);
int clamp(int x, int min, int max);
game_t parse_board(char* str);
typedef enum {
	mode_menu,
	mode_gamelist,
	mode_singleplayer,
	mode_multiplayer,
} client_mode_t;
#define MP_PORT 1093
typedef struct {
	char* name;
	game_t g;
	vector_t player_num;
} mp_game_t;
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
void write_players(vector_t* data, game_t* g);
#include "network.h"
void read_players(cur_t* cur, game_t* g, char* joined);
void write_board(vector_t* data, game_t* g);
void read_board(cur_t* cur, game_t* g);
void write_move(vector_t* data, move_t* m);
move_t read_move(cur_t* cur);
void write_moves(vector_t* data, game_t* g);
void game_free(game_t* g);
typedef struct {
	client_mode_t mode;

	client_t net;
	union {
		struct {
			game_t g;
			char player;
		};

		vector_t game_list;
	};

	int recv;
	mtx_t lock;
	thrd_t recv_thrd;

	vector_t hints; //highlight pieces
	move_t select;

	void (*render)(void*);
	void* arg;
} chess_client_t;
void refresh_hints(chess_client_t * client);
void client_make_move(chess_client_t* client);
void chess_client_startrecv(chess_client_t* client);
void chess_client_gamelist(chess_client_t* client);
void chess_client_makegame(chess_client_t* client, char* g_name);
void chess_client_initgame(chess_client_t* client, char* name, client_mode_t mode);
int chess_client_joingame(chess_client_t* client, unsigned i, char* name);
