// Automatically generated header.

#pragma once
#include <math.h>
#include <string.h>
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
extern char* PIECE_STR[];
extern char* PIECE_NAME[];
typedef enum {
	piece_x = 1,
	piece_nx = 2,
	piece_y = 4,
	piece_ny = 8,
	piece_firstmv = 16,
} piece_flags_t;
typedef struct {
	piece_ty ty;
	piece_flags_t flags;
	char player;
} piece_t;
typedef struct __attribute__ ((__packed__)) {
	int from[2];
	int to[2];
	int castle[2]; //castle with piece, castle[0] == -1 otherwise (standard procedure...)
} move_t;
typedef struct {
	int board_rot;
	char check, mate, last_mate;
	char ai;

	vector_t kings;
	char* name;
	char joined;

	vector_t allies;
} player_t;
typedef enum {
	game_win_by_pieces = 1,
} game_flags_t;
typedef struct {
	vector_t spectators;
	unsigned host;
} mp_extra_t;
typedef struct {
	game_flags_t flags;
	vector_t promote_from;
	piece_ty promote_to;
	vector_t castleable;

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
	piece_t piece_swap_from; //needed for eg. promotion, pawn_firstmv, etc
	mp_extra_t m;
} game_t;
int pos_i(game_t* g, int x[2]);
piece_t* board_get(game_t* g, int x[2]);
int board_i(game_t* g, piece_t* ptr);
void board_rot_pos(game_t* g, int rot, int pos[2], int pos_out[2]);
static inline int i2eq(int a[2], int b[2]) {
	return a[0]==b[0]&&a[1]==b[1];
}
static inline int piece_edible(piece_t* p) {
	return p->ty != p_empty && p->ty != p_blocked;
}
static inline int piece_owned(piece_t* p, char player) {
	return p->player == player; //used to also check empty and blocked
}
static inline int is_ally(char p_i, player_t* p, char p2) {
	return p_i==p2 || memchr(p->allies.data, p2, p->allies.length)!=NULL;
}
void print_board(game_t* g);
int valid_move(game_t* g, move_t* m, int collision);
int board_pos_next(game_t* g, int* x);
int player_check(game_t* g, char p_i, player_t* player);
void castle_to_pos(move_t* m, int* pos);
void move_noswap(game_t* g, move_t* m, piece_t* from, piece_t* to);
void unmove_noswap(game_t* g, move_t* m, piece_t* from, piece_t* to, piece_t from_swap, piece_t to_swap);
void move_swap(game_t* g, move_t* m);
void piece_moves(game_t* g, piece_t* p, vector_t* moves, int check);
int piece_moves_modified(game_t* g, piece_t* p, int* pos, int* other);
void next_player(game_t* g);
enum {
	move_invalid,
	move_turn,
	move_player,
	move_success
} make_move(game_t* g, move_t* m, int validate, int make, char player);
void undo_move(game_t* g);
game_t parse_board(char* str, game_flags_t flags);
char* move_pgn(game_t* g, move_t* m);
