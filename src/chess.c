#include <math.h>
#include <string.h>

#include "vector.h"
#include "hashtable.h"
#include "network.h"
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

char* PIECE_STR[] = {"♚","♛","♜","♝","♞","♟", "A", "C", "H", " ","█"};

char* PIECE_NAME[] = {"King","Queen","Rook","Bishop","Knight","Pawn", "Archibishop", "Chancellor", "Heir/Commoner", " ","█"};

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

piece_t PIECE_EMPTY = {.ty=p_empty, .player=-1};

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

int clamp(int x, int min, int max) {
	return x<min?min:(x>max?max:x);
}

int pos_i(game_t* g, int x[2]) {
	return g->board_w*x[1]+x[0];
}

piece_t* board_get(game_t* g, int x[2]) {
	if (x[0]<0 || x[0]>=g->board_w) return NULL;
	return vector_get(&g->board, pos_i(g, x));
}

int board_i(game_t* g, piece_t* ptr) {
	return (int)(((char*)ptr-g->board.data)/g->board.size);
}

void board_pos_i(game_t* g, int pos[2], int i) {
	pos[0] = i%g->board_w;
	pos[1] = i/g->board_w;
}

//at some point, passing positions with pieces is unmanageable,
//even if this is less efficient than passing and board_get
void board_pos(game_t* g, int pos[2], piece_t* ptr) {
	board_pos_i(g, pos, board_i(g, ptr));
}

void pawn_dir(int dir[2], piece_flags_t flags) {
	dir[0] = (int)(flags&piece_x) - (int)(flags&piece_nx);
	dir[1] = ((int)(flags&piece_y) - (int)(flags&piece_ny)) >> 2;
}

int pawn_rot(piece_flags_t flags) {
	int rot=0;
	if (~flags&piece_ny && flags&piece_y) {
		rot=4;
		if (flags&piece_nx) rot--;
		else if (flags&piece_x) rot++;
	} else if (flags&piece_nx) {
		rot=2;
		if (flags&piece_ny) rot--;
	} else if (flags&piece_x) {
		rot=6;
		if (flags&piece_ny) rot++;
	}

	return rot;
}

void board_rot_pos(game_t* g, int rot, int pos[2], int pos_out[2])	{
	if (rot%2==1) {
		pos_out[0] = pos[1];
		pos_out[1] = pos[0];
	} else {
		pos_out[0] = pos[0];
		pos_out[1] = pos[1];
	}

	if (rot>0 && rot<3) {
		pos_out[1] = g->board_h-1-pos_out[1];
	}

	if (rot>1) {
		pos_out[0] = g->board_w-1-pos_out[0];
	}
}

void rot_pos(int rot, int pos[2], int pos_out[2])	{
	if (rot%2==1) {
		pos_out[0] = pos[1];
		pos_out[1] = pos[0];
	} else {
		pos_out[0] = pos[0];
		pos_out[1] = pos[1];
	}

	if (rot>0 && rot<3) {
		pos_out[1] *= -1;
	}

	if (rot>1) {
		pos_out[0] *= -1;
	}
}

struct pawn_adj {int adj[2][2];} pawn_adjacent(int dir[2]) {
	int not[2] = {!dir[0],!dir[1]};
	return (struct pawn_adj){.adj={{not[0]+dir[0],not[1]+dir[1]}, {-not[0]+dir[0],-not[1]+dir[1]}}};
}

static inline int i2eq(int a[2], int b[2]) {
	return a[0]==b[0]&&a[1]==b[1];
}

static inline int piece_edible(piece_t* p) {
	return p->ty != p_empty && p->ty != p_blocked;
}

static inline int piece_owned(piece_t* p, char player) {
	return p->player == player; //previously used to also check empty and blocked
}

static inline int is_ally(char p_i, player_t* p, char p2) {
	return p_i==p2 || memchr(p->allies.data, p2, p->allies.length)!=NULL;
}

//"debugging"
void print_board(game_t* g) {
	vector_iterator board_iter = vector_iterate(&g->board);
	while (vector_next(&board_iter)) {
		if (board_iter.i%g->board_w==0) printf("\n");
		piece_t* p = board_iter.x;
		printf(" %i%s ", piece_edible(p) ? p->player : g->players.length, PIECE_STR[p->ty]);
	}

	printf("\n");
}

//any non long range pieces (non-colliding) are excluded from checking the king after their players move
int piece_long_range(piece_ty ty) {
	switch (ty) {
		case p_bishop: case p_queen: case p_archibishop: case p_rook: case p_chancellor:
			return 1;
		default: return 0;
	}
}

//valid move, no check check
int valid_move_override(game_t* g, piece_t* p, piece_ty override, move_t* m, int collision) {
	int off[2] = {m->to[0] - m->from[0], m->to[1] - m->from[1]};

	switch (override) {
		case p_blocked:
		case p_empty: return 0;
		case p_bishop: {
			if (abs(off[0])!=abs(off[1])) return 0;
			break;
		}
		case p_heir:
		case p_king: {
			if (m->castle[0]!=-1) {
				if (((player_t*)vector_get(&g->players, p->player))->check) return 0;

				piece_t* castle = board_get(g, m->castle);
				//.
				//.
				//.
				if ((~p->flags & piece_firstmv) || (~castle->flags & piece_firstmv)
						|| memchr(g->castleable.data, castle->ty, g->castleable.length)==NULL
						|| (abs(m->castle[0]-m->from[0])<2 && abs(m->castle[1]-m->from[1])<2)
						|| !piece_owned(castle, p->player)
						|| m->to[0]!=(m->castle[0]+m->from[0])/2+clamp(m->castle[0]-m->from[0],-1,1)
						|| m->to[1]!=(m->castle[1]+m->from[1])/2+clamp(m->castle[1]-m->from[1],-1,1))
					return 0;

				off[0] = m->castle[0]-m->from[0];
				off[1] = m->castle[1]-m->from[1];
				collision=1;
				break;
			}
			
			if (abs(off[0])>1||abs(off[1])>1)
				return 0;
			collision=0;
			break;
		}
		case p_rook: {
			if (off[0]!=0 && off[1]!=0) return 0;
			break;
		}
		case p_chancellor:
		case p_archibishop:
		case p_knight: {
			if ((abs(off[1])!=2||abs(off[0])!=1)&&(abs(off[1])!=1||abs(off[0])!=2)) {
				if ((override==p_chancellor && (off[0]==0 || off[1]==0))
						|| (override==p_archibishop && abs(off[0])==abs(off[1]))) {
					break;
				} else {
					return 0;
				}
			}

			collision = 0;

			break;
		}
		case p_queen: {
			if ((abs(off[0])!=abs(off[1])) && off[0]!=0 && off[1]!=0) return 0;
			break;
		}
		case p_pawn: {
			int dir[2];
			pawn_dir(dir, p->flags);
			if (piece_edible(board_get(g, m->to))) {
				struct pawn_adj adj = pawn_adjacent(dir);
				if (!i2eq(off, adj.adj[0]) && !i2eq(off, adj.adj[1])) return 0;
			} else if (p->flags & piece_firstmv) {
				if (!i2eq(off, dir) && !i2eq(off, (int[2]){dir[0]*2, dir[1]*2})) return 0;
			} else if (!i2eq(off, dir)) {
				return 0;
			}

			collision=0;
			break;
		}
	}

	//anything that moves horizontal, vertical, diagonal
	if (collision) {
		int adi = max(abs(off[0]), abs(off[1]));
		if (adi==0) return 0;

		off[0] /= adi;
		off[1] /= adi;

		for (int i=1; i<adi; i++) {
			if (board_get(g, (int[2]){m->from[0]+i*off[0], m->from[1]+i*off[1]})->ty != p_empty) {
				return 0;
			}
		}
	}

	return 1;
}

int valid_move(game_t* g, move_t* m, int collision) {
	piece_t* p = board_get(g, m->from);
	return valid_move_override(g, p, p->ty, m, collision);
}

int board_pos_next(game_t* g, int* x) {
	x[0]++;

	if (x[0]>=g->board_w) {
		x[0]=0;
		x[1]++;

		if (x[1]>=g->board_h) return 0;
	}

	return 1;
}

//check if king is in check. done at end of each move
//if player is already checked, ensures king is not threatened by all pieces
//otherwise only long range
int player_check(game_t* g, char p_i, player_t* player) {
	if (g->flags & game_win_by_pieces) return 0;

	for (int* king=(int*)player->kings.data; *king!=-1; king++) {
		move_t mv = {.from={-1,0}};
		mv.castle[0] = -1;
		board_pos_i(g, mv.to, *king);

		while (board_pos_next(g, mv.from)) {
			piece_t* p = board_get(g, mv.from);
			if (!is_ally(p_i, player, p->player)) {
				if (valid_move(g, &mv, 1)) return 1;
			}
		}
	}

	return 0;
}

int promoteable(game_t* g, piece_t* p, int pos[2]) {
	int dir[2];
	pawn_dir(dir, p->flags);
	//im too lazy to negate this expression and return it directly
	//or it looks nicer this way
	if ((dir[0]!=0 && pos[0]!=(dir[0]==1?g->board_w-1:0))
			|| (dir[1]!=0 && pos[1]!=(dir[1]==1?g->board_h-1:0))) return 0;
	else return 1;
}

void castle_to_pos(move_t* m, int* pos) {
	pos[0]=(m->castle[0]+m->from[0])/2;
	pos[1]=(m->castle[1]+m->from[1])/2;
}

void move_noswap(game_t* g, move_t* m, piece_t* from, piece_t* to) {
	if (m->castle[0]!=-1) {
		int castle_pos[2]; castle_to_pos(m, castle_pos);
		piece_t* castle = board_get(g, m->castle);
		piece_t* castle_to = board_get(g, castle_pos);

		*castle_to = *castle;
		*castle = PIECE_EMPTY;
	}

	*to = *from;
	*from = PIECE_EMPTY;

	player_t* p = vector_get(&g->players, to->player);

	if (to->ty==p_king && ~g->flags&game_win_by_pieces) {
		for (int* king=(int*)p->kings.data; *king!=-1; king++) {
			if (*king==pos_i(g, m->from)) {
				*king = pos_i(g, m->to);
				break;
			}
		}
	}

	if (to->flags & piece_firstmv) {
		to->flags ^= piece_firstmv;
	}

	if (promoteable(g, to, m->to) && memchr(g->promote_from.data, to->ty, g->promote_from.length)!=NULL) {
		to->ty = g->promote_to;
		if (to->ty==p_king) {
			*(int*)vector_insert(&p->kings, 0) = pos_i(g, m->to);
		}
	}
}

void unmove_noswap(game_t* g, move_t* m, piece_t* from, piece_t* to, piece_t from_swap, piece_t to_swap) {
	if (to->ty==p_king && ~g->flags&game_win_by_pieces) {
		player_t* p = vector_get(&g->players, to->player);
		if (from_swap.ty!=p_king) {
			vector_remove(&p->kings, 0);
		} else {
			for (int* king=(int*)p->kings.data; *king!=-1; king++) {
				if (*king==pos_i(g, m->to)) {
					*king = pos_i(g, m->from);
					break;
				}
			}
		}
	}

	*from = from_swap;
	*to = to_swap;

	if (m->castle[0]!=-1) {
		int castle_pos[2]; castle_to_pos(m, castle_pos);
		piece_t* castle = board_get(g, m->castle);
		piece_t* castle_to = board_get(g, castle_pos);

		*castle = *castle_to;
		*castle_to = PIECE_EMPTY;
	}
}

void move_swap(game_t* g, move_t* m) {
	piece_t* from = board_get(g, m->from);
	piece_t* to = board_get(g, m->to);

	g->piece_swap = *to;
	g->piece_swap_from = *from;
	move_noswap(g, m, from, to);
}

void unmove_swap(game_t* g, move_t* m) {
	piece_t* from = board_get(g, m->from);
	piece_t* to = board_get(g, m->to);

	unmove_noswap(g, m, from, to, g->piece_swap_from, g->piece_swap);
}

//this is worse
void piece_moves_rec(game_t* g, move_t* m, piece_ty override, piece_t* p, player_t* player, vector_t* moves) {
	switch (override) {
		case p_blocked:
		case p_empty: return;
		case p_bishop:
		case p_rook:
		case p_queen: {
			for (int sx=-1; sx<=1; sx++) {
				for (int sy=-1; sy<=1; sy++) {
					if ((sx==0&&sy==0)
							|| (override==p_bishop && (sy==0 || sx==0))
							|| (override==p_rook && (sy!=0 && sx!=0)))
						continue;

					m->to[0]=m->from[0]+sx; m->to[1]=m->from[1]+sy;
					piece_t* pt;
					while ((pt=board_get(g, m->to))) {
						vector_pushcpy(moves, m);
						if (pt->ty != p_empty) break;
						m->to[0] += sx; m->to[1] += sy;
					}
				}
			}

			break;
		}
		case p_heir:
		case p_king: {
			int off[2];
			for (off[0]=-1; off[0]<=1; off[0]++) {
				for (off[1]=-1; off[1]<=1; off[1]++) {
					if (off[0]==0 && off[1]==0) continue;

					m->to[0]=m->from[0]+off[0]; m->to[1]=m->from[1]+off[1];
					vector_pushcpy(moves, m);
				}
			}

			if (p->flags & piece_firstmv && !player->check) {
				for (int sx=-1; sx<=1; sx++) {
					for (int sy=-1; sy<=1; sy++) {
						if (sx==0&&sy==0) continue;

						m->to[0]=m->from[0]+sx; m->to[1]=m->from[1]+sy;
						piece_t* pt;
						while ((pt=board_get(g, m->to))) {
							if (pt->ty != p_empty) {
								if ((abs(m->to[0]-m->from[0])>=2 || abs(m->to[1]-m->from[1])>=2) //king cannot castle with adjacent square
										&& pt->flags & piece_firstmv && memchr(g->castleable.data, pt->ty, g->castleable.length)!=NULL
										&& piece_owned(pt, p->player)) {
									m->castle[0]=m->to[0]; m->castle[1]=m->to[1];
									m->to[0]=(m->castle[0]+m->from[0])/2+clamp(m->castle[0]-m->from[0],-1,1);
									m->to[1]=(m->castle[1]+m->from[1])/2+clamp(m->castle[1]-m->from[1],-1,1);
									vector_pushcpy(moves, m);

									m->castle[0]=-1; //reset
								}

								break;
							}

							m->to[0] += sx; m->to[1] += sy;
						}
					}
				}
			}

			break;
		}
		case p_knight: {
			int offs[8][2] = {{1,2}, {2,1}, {-1,2}, {-2,1}, {-1,-2}, {-2,-1}, {1,-2}, {2,-1}};
			for (int i=0; i<8; i++) {
				m->to[0]=m->from[0]+offs[i][0]; m->to[1]=m->from[1]+offs[i][1];
				vector_pushcpy(moves, m);
			}
			break;
		}
		case p_pawn: {
			int dir[2];
			pawn_dir(dir, p->flags);
			struct pawn_adj adj = pawn_adjacent(dir);

			m->to[0]=m->from[0]+dir[0]; m->to[1]=m->from[1]+dir[1];
			piece_t* pt1 = board_get(g, m->to);

			if (pt1 && pt1->ty == p_empty) {
				vector_pushcpy(moves, m);

				if (p->flags & piece_firstmv) {
					m->to[0]+=dir[0]; m->to[1]+=dir[1];
					piece_t* pt2 = board_get(g, m->to);
					if (pt2 && pt2->ty == p_empty) vector_pushcpy(moves, m);
				}
			}

			m->to[0]=m->from[0]+adj.adj[0][0]; m->to[1]=m->from[1]+adj.adj[0][1];
			piece_t* adj1 = board_get(g, m->to);
			if (adj1 && piece_edible(adj1)) vector_pushcpy(moves, m);
			m->to[0]=m->from[0]+adj.adj[1][0]; m->to[1]=m->from[1]+adj.adj[1][1];
			piece_t* adj2 = board_get(g, m->to);
			if (adj2 && piece_edible(adj2)) vector_pushcpy(moves, m);

			break;
		}
		case p_archibishop: {
			piece_moves_rec(g, m, p_bishop, p, player, moves);
			piece_moves_rec(g, m, p_knight, p, player, moves);
			break;
		}
		case p_chancellor: {
			piece_moves_rec(g, m, p_rook, p, player, moves);
			piece_moves_rec(g, m, p_knight, p, player, moves);
			break;
		}
	}
}

void piece_moves(game_t* g, piece_t* p, vector_t* moves, int check) {
	move_t m;
	m.castle[0]=-1;
	board_pos(g, m.from, p);

	char p_i = p->player;
	player_t* player = vector_get(&g->players, p_i);
	piece_moves_rec(g, &m, p->ty, p, player, moves);

	vector_iterator mv_iter = vector_iterate(moves);
	while (vector_next(&mv_iter)) {
		move_t* m2 = mv_iter.x;
		piece_t* pt = board_get(g, m2->to);
		if (!pt
				|| (m2->castle[0]==-1 && pt->ty!=p_empty && is_ally(p_i, player, pt->player))
				|| pt->ty==p_blocked) {
			vector_remove(moves, mv_iter.i);
			mv_iter.i--;
			continue;
		}

		if (!check) continue;

		move_swap(g, m2);
		int end = player_check(g, p_i, player);
		unmove_swap(g, m2);

		if (end) {
			vector_remove(moves, mv_iter.i);
			mv_iter.i--;
		}
	}
}

int piece_moves_modified(game_t* g, piece_t* p, int* pos, int* other) {
	//special case; pawns dont take pieces when colliding, so pos to to is invalid if to is blocking pawn
	if (p->ty==p_pawn) {
		int off[2] = {other[0]-pos[0], other[1]-pos[1]};

		int dir[2];
		pawn_dir(dir, p->flags);
		struct pawn_adj adj = pawn_adjacent(dir);
		return i2eq(off, adj.adj[0]) || i2eq(off, adj.adj[1]) || i2eq(off, dir)
					|| (p->flags & piece_firstmv && i2eq(off, (int[2]){dir[0]*2, dir[1]*2}));
	}

	move_t m = {.from={pos[0], pos[1]}, .to={other[0], other[1]}};
	m.castle[0]=-1;

	//is this seriously faster than just repeating piece_moves
	//update: yes
	return valid_move_override(g, p, p->ty==p_king?p_queen:p->ty, &m, 1); //override king to collide as a queen for castling
}

void next_player(game_t* g) {
	char player = g->player;
	player_t* t_next;

	do {
		g->player = (char)((g->player+1) % g->players.length);
		t_next = vector_get(&g->players, g->player);

		if (g->player == player) {
			g->won=1;
			break;
		}
	} while (t_next->mate);
}

void update_checks_mates(game_t* g, int undo) {
	vector_iterator t_iter = vector_iterate(&g->players);
	while (vector_next(&t_iter)) {
		player_t* t = t_iter.x;
		if (!undo) t->last_mate = t->mate;
		else t->mate = t->last_mate;

		if (!t->last_mate && (g->flags & game_win_by_pieces || player_check(g, (char)(t_iter.i), t))) {
			t->check=!(g->flags&game_win_by_pieces);
			t->mate=1;

			vector_t moves = vector_new(sizeof(move_t));
			vector_iterator board_iter = vector_iterate(&g->board);
			while (vector_next(&board_iter)) {
				piece_t* p = board_iter.x;
				if (piece_owned(p, t_iter.i)) {
					piece_moves(g, p, &moves, 1);
					if (moves.length>0) {
						t->mate=0;
						break;
					}
				}
			}

			vector_free(&moves);
		} else {
			t->check=0;
		}
	}
}

enum {
	move_invalid,
	move_turn,
	move_player,
	move_success
} make_move(game_t* g, move_t* m, int validate, int make, char player) {
	piece_t* from = board_get(g, m->from);
	player_t* t = vector_get(&g->players, player);

	if (validate) {
		if (!from || !t) return move_invalid;
		if (g->player != player || t->mate) return move_turn;
		if (from->ty == p_empty || from->player != player) return move_player;
		if (i2eq(m->from, m->to)) return move_invalid;
	}

	piece_t* to = board_get(g, m->to);

	if (validate) {
		if (!to || to->ty == p_blocked
				|| (m->castle[0]==-1 && to->ty != p_empty && is_ally(player, t, to->player)))
			return move_invalid;
		if (!valid_move(g, m, 1)) return move_invalid;
	}

	if (make || validate) {
		move_swap(g, m);
	}

	if (validate && player_check(g, player, t)) {
		unmove_swap(g, m);
		return move_invalid;
	}

	if (!make && validate) {
		unmove_swap(g, m);
	}

	update_checks_mates(g, 0);

	if (!t->ai) {
		g->last_player = g->player;
		g->last_move = g->moves.length;
	}

	next_player(g);
	vector_pushcpy(&g->moves, m);

	return move_success;
}

void undo_move(game_t* g) {
	vector_removemany(&g->moves, g->last_move, g->moves.length-g->last_move);
	g->player = g->last_player;

	g->last_player=-1;
	g->won=0;

	update_checks_mates(g, 1);
}

game_t parse_board(char* str, game_flags_t flags) {
	game_t g;
	g.board = vector_new(sizeof(piece_t));
	g.players = vector_new(sizeof(player_t));

	g.board_w = 0;
	g.board_h = 0;
	printf("%s\n", str);

	while (1) {
		if (skip_name(&str, "\nAlliance\n")) {
			char* start = str;
			skip_until(&str, "\n");
			char* end1 = str++;
			skip_until(&str, "\n");

			char p_i[2];

			vector_iterator p_iter = vector_iterate(&g.players);
			while (vector_next(&p_iter)) {
				player_t* p = p_iter.x;
				if (strlen(p->name)==end1-start && strncmp(start, p->name, end1-start)==0) {
					p_i[0]=(char)(p_iter.i);
				} else if (strlen(p->name)==str-end1-1 && strncmp(end1+1, p->name, str-end1-1)==0) {
					p_i[1]=(char)(p_iter.i);
				}
			}

			vector_pushcpy(&((player_t*)vector_get(&g.players, p_i[0]))->allies, &p_i[1]);
			vector_pushcpy(&((player_t*)vector_get(&g.players, p_i[1]))->allies, &p_i[0]);
			str++; continue;
		} else if (*str == '\n') {
			break;
		}

		int rot;
		parse_num(&str, &rot);
		skip_ws(&str);

		char* start = str;
		skip_until(&str, "\n");
		player_t* p = vector_pushcpy(&g.players, &(player_t){
			.name=heapcpysubstr(start, str-start),
			.board_rot=rot, .joined=0, .ai=0,
			.allies=vector_new(1), .kings=vector_new(sizeof(int))});
		vector_pushcpy(&p->kings, &(int){-1}); //sentinel for speed (???)
		str++;
	}

	str++;

	int row_wid=0;
	while (*str) {
		if (skip_char(&str, '\n')) {
			for (;row_wid<g.board_w; row_wid++)
				vector_pushcpy(&g.board, &PIECE_EMPTY);

			if (row_wid>g.board_w) {
				for (int y=g.board_h; y>0; y--)
					for (int x=g.board_w; x<row_wid; x++)
						vector_insertcpy(&g.board, g.board_w*y, &PIECE_EMPTY);

				g.board_w=row_wid;
			}

			g.board_h++;
			row_wid=0;

			continue;
		}

		char* ws_begin = str;
		while (*str==' ' && str-ws_begin<3) {
			str++;
		}

		if (str-ws_begin<3 && *str=='\n') continue;

		piece_t* p = vector_push(&g.board);
		p->flags = piece_firstmv;
		row_wid++;

		if (str-ws_begin==3) {
			*p = PIECE_EMPTY;
			continue;
		}

		if (skip_char(&str, 'O')) {
			p->ty = p_blocked;
			p->player=-1;
			continue;
		}

		int player;
		parse_num(&str, &player);
		p->player = (char)player;

		switch (*str) {
			case 'K': {
				p->ty = p_king;
				player_t* t = vector_get(&g.players, p->player);
				if (t) vector_insertcpy(&t->kings, 0, &(int){(int)(g.board.length-1)});
				break;
			}
			case 'P': p->ty=p_pawn; break;
			case 'H': p->ty=p_heir; break;
			case 'Q': p->ty=p_queen; break;
			case 'B': p->ty=p_bishop; break;
			case 'N': p->ty=p_knight; break;
			case 'R': p->ty=p_rook; break;
			case 'A': p->ty=p_archibishop; break;
			case 'C': p->ty=p_chancellor; break;
			default: perrorx("unknown piece type"); break;
		}

		str++;

		if (skip_name(&str, "<")) p->flags|=piece_x|piece_nx;
		else if (skip_name(&str, "^")) p->flags|=piece_y|piece_ny;
		else if (skip_name(&str, ">")) p->flags|=piece_x;
		else if (skip_name(&str, "v")) p->flags|=piece_y;
		else if (skip_name(&str, "↖")) p->flags|=piece_y|piece_ny|piece_x|piece_nx;
		else if (skip_name(&str, "↗")) p->flags|=piece_y|piece_ny|piece_x;
		else if (skip_name(&str, "↙")) p->flags|=piece_y|piece_x|piece_nx;
		else if (skip_name(&str, "↘")) p->flags|=piece_y|piece_x;
	}

	vector_cpy(&g.board, &g.init_board);
	g.moves = vector_new(sizeof(move_t));
	g.last_player = -1;
	g.player = 0;
	g.won=0;
	g.flags = flags;

	g.promote_from = vector_new(1);
	g.castleable = vector_new(1);

	vector_iterator p_iter = vector_iterate(&g.players);
	while (vector_next(&p_iter)) {
		player_t* p = p_iter.x;
		p->mate=0;
		p->check=player_check(&g, p_iter.i, p);
	}

	print_board(&g);
	return g;
}

char* move_pgn(game_t* g, move_t* m) {
	static char alphabet[] = "abcdefghijklmnopqrstuvwxyz";
	if (m->from[0]<sizeof(alphabet) && m->to[0]<sizeof(alphabet)) {
		return heapstr("%c%i %c%i", alphabet[m->from[0]], g->board_h-m->from[1], alphabet[m->to[0]], g->board_h-m->to[1]);
	} else { //are they trying to play shogi?
		return heapstr("%i-%i %i-%i", m->from[0]+1, g->board_h-m->from[1], m->to[0]+1, g->board_h-m->to[1]);
	}
}

