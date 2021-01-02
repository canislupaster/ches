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
	p_heir,
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
	pawn_promoted = 64,
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
	char check, mate;
	char ai;

	int king;
	char* name;
	char joined;

	vector_t allies;
} player_t;
typedef enum {
	game_pawn_promotion=1
} game_flags_t;
typedef struct {
	vector_t spectators;
} mp_extra_t;
typedef struct {
	game_flags_t flags;

	int board_w, board_h;
	vector_t init_board;
	vector_t board;
	vector_t moves;
	//if player->ai, then not counted
	char last_player;
	unsigned last_move;

	char player; //of current move
	char won;

	vector_t players;

	piece_t piece_swap; //emulating moves is needed when evaluating possible moves or creating a game tree, maybe later augment into a stack
	mp_extra_t m;
} game_t;
int pos_i(game_t* g, int x[2]);
piece_t* board_get(game_t* g, int x[2]);
int board_i(game_t* g, piece_t* ptr);
void board_pos_i(game_t* g, int pos[2], int i);
void board_rot_pos(game_t* g, int rot, int pos[2], int pos_out[2]);
int piece_edible(piece_t* p);
int piece_owned(piece_t* p, char player);
int is_ally(char p_i, player_t* p, char p2);
void print_board(game_t* g);
int piece_long_range(piece_ty ty);
int valid_move(game_t* g, move_t* m, int collision);
int board_pos_next(game_t* g, int* x);
int player_check(game_t* g, char p_i, player_t* player);
void move_noswap(game_t* g, move_t* m, piece_t* from, piece_t* to);
void unmove_noswap(game_t* g, move_t* m, piece_t* from, piece_t* to);
void move_swap(game_t* g, move_t* m);
void piece_moves(game_t* g, piece_t* p, vector_t* moves);
int piece_moves_modified(game_t* g, piece_t* p, int pos[2], move_t* m);
void next_player(game_t* g);
enum {
	move_invalid,
	move_turn,
	move_player,
	move_success
} make_move(game_t* g, move_t* m, int validate, int make, char player);
void undo_move(game_t* g);
game_t parse_board(char* str);
char* move_pgn(game_t* g, move_t* m);
