// Automatically generated header.

#pragma once
#include "cfg.h"
#include "filemap.h"
#include "threads.h"
#include "vector.h"
#include "hashtable.h"
typedef enum {
	p_empty,
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

	piece_t* king;
	char* name;
	int joined;
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
void board_pos(game_t* g, int pos[2], piece_t* ptr);
void board_rot_pos(game_t* g, int rot, int pos[2], int pos_out[2]);
void rot_pos(int rot, int pos[2], int pos_out[2]);
char* piece_str(piece_t* p);
int player_col(char p);
void piece_moves(game_t* g, piece_t* p, vector_t* moves);
enum {
	move_invalid,
	move_turn,
	move_player,
	move_success
} make_move(game_t* g, move_t* m, int validate, char player);
int clamp(int x, int min, int max);
game_t default_chess();
typedef enum {
	mode_menu,
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
void read_moves(cur_t* cur, game_t* g);
void game_free(game_t* g);
typedef struct {
	server_t server;
	vector_t games;
	map_t num_joined;

	//filemap_t users;
	//filemap_index_t users_ip;
} chess_server_t;
typedef struct {
	map_t cfg;
	client_mode_t mode;

	client_t server;
	game_t g;
	int select[2];
	int select2[2];
	int which;
	vector_t hints; //highlight pieces
	char player;

	int recv;
	mtx_t lock;
	thrd_t recv_thrd;

	int centerx, centery;
} chess_client_t;
extern char* CFG_SERVADDR;
extern char* CFG_NAME;
extern char* CFG_PATH;
map_t init_cfg();
