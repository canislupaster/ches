#include "cfg.h"
#include "filemap.h"
#include "threads.h"
#include "vector.h"
#include "hashtable.h"
#include "network.h"

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
	char* pcs[] = {" ","█","♚","♛","♜","♝","♞","♟"};
	return pcs[p->ty];
}

int player_col(char p) {
	return p % 4;
}

struct pawn_adj {int adj[2][2];} pawn_adjacent(int dir[2]) {
	int not[2] = {!dir[0],!dir[1]};
	return (struct pawn_adj){.adj={{not[0]+dir[0],not[1]+dir[1]}, {-not[0]+dir[0],-not[1]+dir[1]}}};
}

int i2eq(int a[2], int b[2]) {
	return a[0]==b[0]&&a[1]==b[1];
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
		case p_knight: {
			if ((abs(off[1])!=2||abs(off[0])!=1)&&(abs(off[1])!=1||abs(off[0])!=2))
				return 0;
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
			if (board_get(g, m->to)->ty != p_empty) {
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
		if (p->ty != p_empty && p->player != piece->player) {
			board_pos(g, mv.from, p);
			if (valid_move(g, &mv)) return 1;
		}
	}

	return 0;
}

void move_swap(game_t* g, piece_t* from, piece_t* to) {
	g->piece_swap = *to;
	*to = *from;
	from->ty = p_empty;

	if (to->ty==p_king) {
		((player_t*)vector_get(&g->players, to->player))->king = piece_i(g, to);
	} else if (to->ty == p_pawn) {
		to->flags &= ~pawn_firstmv;
		to->flags |= pawn_firstmv_swp;
	}
}

void unmove_swap(game_t* g, piece_t* from, piece_t* to) {
	if (to->ty==p_king) {
		((player_t*)vector_get(&g->players, to->player))->king = piece_i(g, from);
	} else if (to->ty == p_pawn && to->flags & pawn_firstmv_swp) {
		to->flags &= ~pawn_firstmv_swp;
		to->flags |= pawn_firstmv;
	}

	*from = *to;
	*to = g->piece_swap;
}

//this is worse
void piece_moves(game_t* g, piece_t* p, vector_t* moves) {
	player_t* t = vector_get(&g->players, p->player);

	int pos[2];
	board_pos(g,pos,p);

	switch (p->ty) {
		case p_blocked:
		case p_empty: return;
		case p_bishop:
		case p_rook:
		case p_queen: {
			for (int sx=-1; sx<=1; sx++) {
				for (int sy=-1; sy<=1; sy++) {
					if ((p->ty==p_bishop && (sy==0 || sx==0)) || (p->ty==p_rook && (sy!=0 && sx!=0))) continue;

					int to[2] = {pos[0]+sx, pos[1]+sy};
					piece_t* pt;
					while ((pt=board_get(g, to))) {
						vector_pushcpy(moves, &pt);
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
			if (adj1 && adj1->ty!=p_empty) vector_pushcpy(moves, &adj1);
			piece_t* adj2 = board_get(g, (int[2]){pos[0]+adj.adj[1][0], pos[1]+adj.adj[1][1]});
			if (adj2 && adj2->ty!=p_empty) vector_pushcpy(moves, &adj2);

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

		move_swap(g, p, pt);
		int end = endangered(g, vector_get(&g->board, t->king));
		unmove_swap(g, p, pt);

		if (end) {
			vector_remove(moves, mv_iter.i-1);
			mv_iter.i--;
		}
	}
}

int check_mate(game_t* g, char player) {
	vector_t moves = vector_new(sizeof(piece_t*));
	vector_iterator board_iter = vector_iterate(&g->board);
	while (vector_next(&board_iter)) {
		piece_t* p = board_iter.x;
		if (p->ty != p_empty && p->player == player) {
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

enum {
	move_invalid,
	move_turn,
	move_player,
	move_success
} make_move(game_t* g, move_t* m, int validate, char player) {
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

	move_swap(g, from, to);

	if (validate && endangered(g, vector_get(&g->board, t->king))) {
		unmove_swap(g, from, to);
		return move_invalid;
	}

	//update checks & mates
	vector_iterator t_iter = vector_iterate(&g->players);
	while (vector_next(&t_iter)) {
		player_t* t2 = t_iter.x;
		if (endangered(g, vector_get(&g->board, t2->king))) {
			t2->check=1;
			t2->mate = check_mate(g, (char)(t_iter.i-1));
		}
	}

	player_t* t_next;

	do {
		g->player++;
		g->player = (char)(g->player % g->players.length);
		t_next = vector_get(&g->players, g->player);

		if (g->player == player) {
			g->won=1;
			return move_success;
		}
	} while (t_next->mate);

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

		if (skip_char(&str, 'A')) {
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
				if (skip_name(&str, "←")) p->flags=pawn_x|pawn_nx;
				else if (skip_name(&str, "↑")) p->flags=pawn_y|pawn_ny;
				else if (skip_name(&str, "→")) p->flags=pawn_x;
				else if (skip_name(&str, "↓")) p->flags=pawn_y;
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
		}

		str++;
	}

	g.moves = vector_new(sizeof(move_t));
	g.player = 0;
	g.won=0;

	vector_iterator p_iter = vector_iterate(&g.players);
	while (vector_next(&p_iter)) {
		player_t* p = p_iter.x;
		p->mate=0;
		p->check=endangered(&g, vector_get(&g.board, p->king));
	}

	return g;
}

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

// i cant make a thousand structs (some of which have nothing other than a flexible array)
// to document the "protocol" so i guess we use comments to indicate what goes after the op

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

void write_players(vector_t* data, game_t* g) {
	vector_pushcpy(data, &(char){(char)g->players.length});
	vector_pushcpy(data, &(char){g->player});

	vector_iterator p_iter = vector_iterate(&g->players);
	while (vector_next(&p_iter)) {
		player_t* p = p_iter.x;
		write_int(data, p->board_rot);
		write_int(data, p->check);
		write_int(data, p->mate);
		write_int(data, p->joined);
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
	g->player = read_chr(cur);
	for (char i=0; i<num_players; i++) {
		player_t* p = vector_push(&g->players);
		p->board_rot = read_int(cur);
		p->check = read_int(cur);
		p->mate = read_int(cur);
		p->joined = read_int(cur);
		if (joined && p->joined) *joined = i;
		p->name = read_str(cur);

		p->allies = vector_new(1);
		char len = read_chr(cur);
		for (char k=0; k<len; k++) {
			char ally = read_chr(cur);
			vector_pushcpy(&p->allies, &ally);
		}
	}
}

void write_board(vector_t* data, game_t* g) {
	write_int(data, g->board_w);
	write_int(data, g->board_h);
	vector_iterator b_iter = vector_iterate(&g->board);
	while (vector_next(&b_iter)) {
		piece_t* p = b_iter.x;
		//in future htons might be required
		write_uchr(data, (unsigned char)p->ty);
		write_uchr(data, (unsigned char)p->flags);
		vector_pushcpy(data, &p->player);
	}
}

void read_board(cur_t* cur, game_t* g) {
	g->board_w = read_int(cur);
	g->board_h = read_int(cur);
	g->board = vector_new(sizeof(piece_t));

	for (int i=0; i<g->board_w*g->board_h; i++) {
		piece_t* p = vector_push(&g->board);
		p->ty = (piece_ty)read_uchr(cur);
		p->flags = (piece_flags_t)read_uchr(cur);
		p->player = read_chr(cur);

		if (p->ty == p_king) {
			((player_t*)vector_get(&g->players, p->player))->king = piece_i(g, p);
		}
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
	g->moves = vector_new(sizeof(moves));
	for (unsigned i=0; i<moves; i++)
		vector_pushcpy(&g->moves, (move_t[]){read_move(cur)});
}

void game_free(game_t* g) {
	vector_iterator p_iter = vector_iterate(&g->players);
	while (vector_next(&p_iter)) {
		player_t* p = p_iter.x;
		drop(p->name);
	}

	vector_free(&g->players);
	vector_free(&g->board);
	vector_free(&g->moves);
}

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

const char* CFG_SERVADDR = "server";
const char* CFG_NAME = "name";
char* CFG_PATH = "termchess.txt";

map_t init_cfg() {
	map_t cfg = map_new();
	map_configure_string_key(&cfg, sizeof(config_val));
	cfg_add(&cfg, CFG_SERVADDR, cfg_str, (cfg_data){.str=NULL});
	cfg_add(&cfg, CFG_NAME, cfg_str, (cfg_data){.str=""});
	configure(&cfg, CFG_PATH);
	return cfg;
}
