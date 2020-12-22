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
	vector_t spectators;
	char takeback; //takeback pending for cur player
} mp_extra_t;

typedef struct {
	game_flags_t flags;

	int board_w, board_h;
	vector_t init_board;
	vector_t board;
	vector_t moves;
	char player; //of current move
	char won;

	vector_t players;

	piece_t piece_swap; //emulating moves is needed when evaluating possible moves or creating a game tree, maybe later augment into a stack
	mp_extra_t m;
} game_t;

piece_t* board_get(game_t* g, int x[2]) {
	if (x[0]<0 || x[0]>=g->board_w) return NULL;
	return vector_get(&g->board, g->board_w*x[1] + x[0]);
}

int piece_i(game_t* g, piece_t* ptr) {
	return (int)(((char*)ptr-g->board.data)/g->board.size);
}

void board_pos_i(game_t* g, int pos[2], int i) {
	pos[0] = i%g->board_w;
	pos[1] = i/g->board_w;
}

void board_pos(game_t* g, int pos[2], piece_t* ptr) {
	board_pos_i(g, pos, piece_i(g, ptr));
}

void pawn_dir(int dir[2], piece_flags_t flags) {
	dir[0] = (int)(flags&pawn_x) - (int)(flags&pawn_nx);
	dir[1] = ((int)(flags&pawn_y) - (int)(flags&pawn_ny))>>2;
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
		pos_out[1] = (rot%2==1?g->board_w:g->board_h)-1-pos_out[1];
	}

	if (rot>1) {
		pos_out[0] = (rot%2==1?g->board_h:g->board_w)-1-pos_out[0];
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

char* piece_str(piece_t* p) {
	char* pcs[] = {"♚","♛","♜","♝","♞","♟", " ","█"};
	return pcs[p->ty];
}

struct pawn_adj {int adj[2][2];} pawn_adjacent(int dir[2]) {
	int not[2] = {!dir[0],!dir[1]};
	return (struct pawn_adj){.adj={{not[0]+dir[0],not[1]+dir[1]}, {-not[0]+dir[0],-not[1]+dir[1]}}};
}

int i2eq(int a[2], int b[2]) {
	return a[0]==b[0]&&a[1]==b[1];
}

int piece_edible(piece_t* p) {
	return p->ty != p_empty && p->ty != p_blocked;
}

int piece_owned(piece_t* p, char player) {
	return p->ty != p_empty && p->ty != p_blocked && p->player == player;
}

//valid move, no check check
int valid_move(game_t* g, move_t* m) {
	if (m->to[0]>=g->board_w||m->to[0]<0||m->to[1]>=g->board_h||m->to[1]<0) return 0;

	int off[2] = {m->to[0] - m->from[0], m->to[1] - m->from[1]};
	int h = abs(off[0])>=abs(off[1]);
	int v = abs(off[1])>=abs(off[0]);

	piece_t* p = board_get(g, m->from);
	int collision=1;
	switch (p->ty) {
		case p_blocked:
		case p_empty: return 0;
		case p_bishop: {
			if (!h || !v) return 0;
			break;
		}
		case p_king: { //TODO: castling
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
				if ((p->ty==p_chancellor && (off[0]==0 || off[1]==0)) || (p->ty==p_archibishop && h && v)) {
					collision=1;
					break;
				} else {
					return 0;
				}
			}

			collision = 0;

			break;
		}
		case p_queen: {
			if ((!h || !v) && off[0]!=0 && off[1]!=0) return 0;
			break;
		}
		case p_pawn: {
			int dir[2];
			pawn_dir(dir, p->flags);
			if (piece_edible(board_get(g, m->to))) {
				struct pawn_adj adj = pawn_adjacent(dir);
				if (!i2eq(off, adj.adj[0]) && !i2eq(off, adj.adj[1])) return 0;
			} else if (p->flags & pawn_firstmv) {
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
		int di = h ? off[0] : off[1];
		int adi = abs(di);

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

//check if king is in check. done at end of each move
//could be optimized into caching checks and then ensuring that a move doesnt expose to long-range pieces but eh
int endangered(game_t* g, piece_t* piece) {
	move_t mv;
	board_pos(g, mv.to, piece);

	vector_iterator board_iter = vector_iterate(&g->board);
	while (vector_next(&board_iter)) {
		piece_t* p = board_iter.x;
		if (piece_edible(p) && p->player != piece->player) {
			board_pos(g, mv.from, p);
			if (valid_move(g, &mv)) return 1;
		}
	}

	return 0;
}

//for check checks, only moves piece
void move_swap_check(game_t* g, piece_t* from, piece_t* to) {
	g->piece_swap = *to;
	*to = *from;
	from->ty = p_empty;

	if (to->ty==p_king) {
		((player_t*)vector_get(&g->players, to->player))->king = piece_i(g, to);
	}
}

int pawn_promote(game_t* g, piece_t* p, int pos[2]) {
	int dir[2];
	pawn_dir(dir, p->flags);
	//im too lazy to negate this expression and return it directly
	//or it looks nicer this way
	if ((dir[0]!=0 && pos[0]!=(dir[0]==1?g->board_w-1:0))
			|| (dir[1]!=0 && pos[1]!=(dir[1]==1?g->board_h-1:0))) return 0;
	else return 1;
}

void move_swap(game_t* g, move_t* m) {
	piece_t* from = board_get(g, m->from);
	piece_t* to = board_get(g, m->to);

	g->piece_swap = *to;
	*to = *from;
	from->ty = p_empty;

	if (to->ty==p_king) {
		((player_t*)vector_get(&g->players, to->player))->king = piece_i(g, to);
	} else if (to->ty == p_pawn) {
		to->flags &= ~(pawn_firstmv_swp | pawn_promoted);
		
		if (to->flags & pawn_firstmv) {
			to->flags ^= pawn_firstmv;
			to->flags |= pawn_firstmv_swp;
		}

		if (g->flags & game_pawn_promotion && pawn_promote(g, to, m->to)) {
			to->flags |= pawn_promoted;
			to->ty = p_queen; //knights arent that good anyways
		}
	}
}

void unmove_swap_check(game_t* g, piece_t* from, piece_t* to) {
	if (to->ty==p_king) {
		((player_t*)vector_get(&g->players, to->player))->king = piece_i(g, from);
	}

	*from = *to;
	*to = g->piece_swap;
}

void unmove_swap(game_t* g, piece_t* from, piece_t* to) {
	unmove_swap_check(g, from, to);
	if (from->ty == p_pawn) {
		if (from->flags & pawn_firstmv_swp) {
			from->flags |= pawn_firstmv;
		} else if (from->flags & pawn_promoted) {
			from->ty = p_pawn;
		}
	}
}

//this is worse
void piece_moves_rec(game_t* g, player_t* t, int pos[2], piece_ty override, piece_t* p, vector_t* moves) {
	switch (override) {
		case p_blocked:
		case p_empty: return;
		case p_bishop:
		case p_rook:
		case p_queen: {
			for (int sx=-1; sx<=1; sx++) {
				for (int sy=-1; sy<=1; sy++) {
					if ((override==p_bishop && (sy==0 || sx==0)) || (override==p_rook && (sy!=0 && sx!=0))) continue;

					int to[2] = {pos[0]+sx, pos[1]+sy};
					piece_t* pt;
					while ((pt=board_get(g, to))) {
						if (pt->ty != p_blocked) vector_pushcpy(moves, &pt);
						if (pt->ty != p_empty) break;
						to[0] += sx; to[1] += sy;
					}
				}
			}

			break;
		}
		case p_king: {
			int off[2];
			for (off[0]=-1; off[0]<=1; off[0]++) {
				for (off[1]=-1; off[1]<=1; off[1]++) {
					if (off[0]==0 && off[1]==0) continue;

					piece_t* pt = board_get(g, (int[2]){pos[0]+off[0],pos[1]+off[1]});
					if (pt) vector_pushcpy(moves, &pt);
				}
			}

			break;
		}
		case p_knight: {
			int offs[8][2] = {{1,2}, {2,1}, {-1,2}, {-2,1}, {-1,-2}, {-2,-1}, {1,-2}, {2,-1}};
			for (int i=0; i<8; i++) {
				int to[2] = {pos[0]+offs[i][0], pos[1]+offs[i][1]};

				piece_t* pt = board_get(g, to);
				if (pt) vector_pushcpy(moves, &pt);
			}
			break;
		}
		case p_pawn: {
			int dir[2];
			pawn_dir(dir, p->flags);
			struct pawn_adj adj = pawn_adjacent(dir);

			int to1[2] = {pos[0]+dir[0], pos[1]+dir[1]};
			piece_t* pt1 = board_get(g, to1);

			if (pt1 && pt1->ty == p_empty) {
				vector_pushcpy(moves, &pt1);

				if (p->flags & pawn_firstmv) {
					int to2[2] = {pos[0]+dir[0]*2, pos[1]+dir[1]*2};
					piece_t* pt2 = board_get(g, to2);
					if (pt2 && pt2->ty == p_empty) vector_pushcpy(moves, &pt2);
				}
			}

			piece_t* adj1 = board_get(g, (int[2]){pos[0]+adj.adj[0][0], pos[1]+adj.adj[0][1]});
			if (adj1 && piece_edible(adj1)) vector_pushcpy(moves, &adj1);
			piece_t* adj2 = board_get(g, (int[2]){pos[0]+adj.adj[1][0], pos[1]+adj.adj[1][1]});
			if (adj2 && piece_edible(adj2)) vector_pushcpy(moves, &adj2);

			break;
		}
		case p_archibishop: {
			piece_moves_rec(g, t, pos, p_bishop, p, moves);
			piece_moves_rec(g, t, pos, p_knight, p, moves);
			break;
		}
		case p_chancellor: {
			piece_moves_rec(g, t, pos, p_rook, p, moves);
			piece_moves_rec(g, t, pos, p_knight, p, moves);
			break;
		}
	}

	vector_iterator mv_iter = vector_iterate(moves);
	while (vector_next(&mv_iter)) {
		piece_t* pt = *(piece_t**)mv_iter.x;
		if ((pt->ty != p_empty && pt->player==p->player) || pt->ty==p_blocked) {
			vector_remove(moves, mv_iter.i-1);
			mv_iter.i--;
			continue;
		}

		move_swap_check(g, p, pt);
		int end = endangered(g, vector_get(&g->board, t->king));
		unmove_swap_check(g, p, pt);

		if (end) {
			vector_remove(moves, mv_iter.i-1);
			mv_iter.i--;
		}
	}
}

void piece_moves(game_t* g, piece_t* p, vector_t* moves) {
	player_t* t = vector_get(&g->players, p->player);

	int pos[2];
	board_pos(g,pos,p);

	piece_moves_rec(g, t, pos, p->ty, p, moves);
}

int check_mate(game_t* g, char player) {
	vector_t moves = vector_new(sizeof(piece_t*));
	vector_iterator board_iter = vector_iterate(&g->board);
	while (vector_next(&board_iter)) {
		piece_t* p = board_iter.x;
		if (piece_owned(p, player)) {
			piece_moves(g, p, &moves);
			if (moves.length>0) {
				vector_free(&moves);
				return 0;
			}
		}
	}

	vector_free(&moves);
	return 1;
}

//update checks & mates, then find another player or set won if next is set
void next_player(game_t* g, int next) {
	char player = g->player;

	vector_iterator t_iter = vector_iterate(&g->players);
	while (vector_next(&t_iter)) {
		player_t* t2 = t_iter.x;
		if (endangered(g, vector_get(&g->board, t2->king))) {
			t2->check=1;
			t2->mate = check_mate(g, (char)(t_iter.i-1));
		} else {
			t2->check=0;
			t2->mate=0;
		}
	}

	if (next) {
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
		if (g->player != player || t->mate) return move_turn;
		if (!from || !t) return move_invalid;
		if (from->ty == p_empty || from->player != player) return move_player;
		if (i2eq(m->from, m->to)) return move_invalid;
	}

	piece_t* to = board_get(g, m->to);

	if (validate) {
		if (to->ty == p_blocked || (to->ty != p_empty && to->player == player))
			return move_invalid;
		if (!valid_move(g, m)) return move_invalid;
	}

	if (make || validate) {
		move_swap(g, m);
	}

	if (validate && endangered(g, vector_get(&g->board, t->king))) {
		unmove_swap(g, from, to);
		return move_invalid;
	}

	if (!make && validate) {
		unmove_swap(g, from, to);
	}

	next_player(g, 1);
	vector_pushcpy(&g->moves, m);

	return move_success;
}

int clamp(int x, int min, int max) {
	return x<min?min:(x>=max?(max-1):x);
}

game_t parse_board(char* str) {
	game_t g;
	g.board = vector_new(sizeof(piece_t));
	g.players = vector_new(sizeof(player_t));

	g.board_w = 0;
	g.board_h = 0;
	printf("%s\n", str);

	while (*str != '\n') {
		if (skip_name(&str, "Alliance")) {
			skip_ws(&str);
			char* start = str;
			skip_until(&str, "\n");
			char* end1 = str++;
			skip_until(&str, "\n");

			char p_i[2];

			vector_iterator p_iter = vector_iterate(&g.players);
			while (vector_next(&p_iter)) {
				player_t* p = p_iter.x;
				if (strlen(p->name)==end1-start && strncmp(start, p->name, end1-start)==0) {
					p_i[0]=(char)(p_iter.i-1);
				} else if (strlen(p->name)==str-end1+1 && strncmp(end1+1, p->name, str-end1-1)==0) {
					p_i[1]=(char)(p_iter.i-1);
				}
			}

			vector_pushcpy(&((player_t*)vector_get(&g.players, p_i[0]))->allies, &p_i[1]);
			vector_pushcpy(&((player_t*)vector_get(&g.players, p_i[1]))->allies, &p_i[0]);
			str++; continue;
		}

		int rot;
		parse_num(&str, &rot);
		skip_ws(&str);

		char* start = str;
		skip_until(&str, "\n");
		vector_pushcpy(&g.players, &(player_t){.name=heapcpysubstr(start, str-start), .board_rot=rot, .joined=0});
		str++;
	}

	str++;

	int row_wid=0;
	while (*str) {
		if (skip_char(&str, '\n')) {
			for (;row_wid<g.board_w; row_wid++)
				vector_pushcpy(&g.board, &(piece_t){.ty=p_empty});

			if (row_wid>g.board_w) {
				for (int y=g.board_h-1; y>=0; y--) {
					vector_insertcpy(&g.board, g.board_w*y, &(piece_t){.ty=p_empty});
				}

				g.board_w=row_wid;
			}

			g.board_h++;
			row_wid=0;

			continue;
		}

		piece_t* p = vector_push(&g.board);
		row_wid++;

		if (skip_name(&str, "   ")) {
			p->ty = p_empty;
			continue;
		} else {
			while (*str==' ') str++;
		}

		if (skip_char(&str, 'O')) {
			p->ty = p_blocked;
			p->player=0;
			continue;
		}

		int player;
		parse_num(&str, &player);
		p->player = (char)player;

		switch (*str) {
			case 'P': {
				str++;
				p->ty = p_pawn;
				if (skip_name(&str, "<")) p->flags=pawn_x|pawn_nx;
				else if (skip_name(&str, "^")) p->flags=pawn_y|pawn_ny;
				else if (skip_name(&str, ">")) p->flags=pawn_x;
				else if (skip_name(&str, "v")) p->flags=pawn_y;
				else if (skip_name(&str, "↖")) p->flags=pawn_y|pawn_ny|pawn_x|pawn_nx;
				else if (skip_name(&str, "↗")) p->flags=pawn_y|pawn_ny|pawn_x;
				else if (skip_name(&str, "↙")) p->flags=pawn_y|pawn_x|pawn_nx;
				else if (skip_name(&str, "↘")) p->flags=pawn_y|pawn_x;

				p->flags |= pawn_firstmv;

				str--;
				break;
			}
			case 'K': {
				p->ty = p_king;
				player_t* t = vector_get(&g.players, p->player);
				if (t) t->king = (int)g.board.length-1;
				break;
			}
			case 'Q': p->ty=p_queen; break;
			case 'B': p->ty=p_bishop; break;
			case 'N': p->ty=p_knight; break;
			case 'R': p->ty=p_rook; break;
			case 'A': p->ty=p_archibishop; break;
			case 'C': p->ty=p_chancellor; break;
			default: perrorx("unknown piece type"); break;
		}

		str++;
	}

	vector_cpy(&g.board, &g.init_board);
	g.moves = vector_new(sizeof(move_t));
	g.player = 0;
	g.won=0;
	g.flags=0;

	vector_iterator p_iter = vector_iterate(&g.players);
	while (vector_next(&p_iter)) {
		player_t* p = p_iter.x;
		p->mate=0;
		p->check=endangered(&g, vector_get(&g.board, p->king));
	}

	return g;
}

char* move_pgn(game_t* g, move_t* m) {
	static char alphabet[] = "abcdefghijklmnopqrstuvwxyz";
	if (m->from[0]<sizeof(alphabet) && m->to[0]<sizeof(alphabet)) {
		return heapstr("%c%i %c%i", alphabet[m->from[0]], g->board_h-m->from[1], alphabet[m->to[0]], g->board_h-m->to[1]);
	} else { //are they trying to play shogi?
		return heapstr("%i-%i %i-%i", m->from[0], g->board_h-m->from[1], m->to[0], g->board_h-m->to[1]);
	}
}

