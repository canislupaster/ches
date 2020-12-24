#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "chess.h"
#include "util.h"

float piecety_value(piece_ty ty) {
	switch (ty) {
		case p_pawn: return 1;
		case p_queen: return 10;
		case p_bishop: return 4;
		case p_rook: return 6;
		case p_knight: return 3;
		case p_chancellor: return 9;
		case p_archibishop: return 7;
		default: return 2;
	}
}

#define AI_MAX_LOSS 10 //queen
#define AI_RANGEVAL 0

#define AI_DEPTH 3 //base search
#define AI_MAXDEPTH 6 //absolute maximum depth, including with extensions
#define AI_EXPECTEDLEN 19 //more than 10 pieces per move, otherwise extend by log2(10/len)
#define AI_MAXPLAYER 127

char finddepth(unsigned len) {
	return (char)max((int)roundf((log2f(AI_EXPECTEDLEN/(float)len)+1)*AI_DEPTH),AI_DEPTH);
}

//take shortest checkmate, no matter depth
//weights indexed by how deep mate is or how many other players get turn
//populated to depth or zeroed
float CHECKMATE_CURVE[] = {INFINITY, 100, 75, 60, 50, 40, 35};

float checkmate_value(game_t* g, int d) {
	player_t* p = vector_get(&g->players, g->player);
	int check = player_check(g, g->player, p);
	d += (int)g->players.length-2 + 1-check;
	if (d>=AI_MAXDEPTH+1) return CHECKMATE_CURVE[AI_MAXDEPTH];
	else return CHECKMATE_CURVE[d];
}

typedef struct {
	move_t* m;
	piece_t piece_from;
	piece_t piece_to;
	char player;
} branch_t;

typedef struct {
	piece_t* p;
	int pos[2];
	vector_t moves;
	char modified[AI_MAXDEPTH];
} piece_moves_t;

typedef struct move_vecs {
	vector_t moves; //piece_moves_t

	branch_t branches[AI_MAXDEPTH];

	char ai_player;
	//extend depth of search depending on complexity
	char finddepth;

	int init;
} move_vecs_t;

float piece_value(game_t* g, move_vecs_t* vecs, piece_t* p) {
	vector_iterator p_search = vector_iterate(&vecs->moves);
	float range=0;
	while (vector_next(&p_search)) {
		piece_moves_t* pmoves = p_search.x;
		if (pmoves->p==p) {
			range = (float)pmoves->moves.length;
			break;
		}
	}
	
	range/=((float)(g->board_w*g->board_h));
	return range*AI_RANGEVAL + piecety_value(p->ty);
}

void branch_enter(game_t* g, move_vecs_t* vecs, char depth, branch_t* b, move_t* m) {
	b->m = m;

	piece_t* from = board_get(g, b->m->from);
	piece_t* to = board_get(g, b->m->to);

	b->piece_from=*from;
	b->piece_to=*to;
	
	move_noswap(g, b->m, from, to);

	vector_iterator pmoves_iter = vector_iterate(&vecs->moves);
	while (vector_next(&pmoves_iter)) {
		piece_moves_t* pmoves = pmoves_iter.x;
		if (!piece_edible(pmoves->p)) continue;
		if (pmoves->p==to || piece_moves_modified(g, pmoves->p, pmoves->pos, b->m)) {
			vector_clear(&pmoves->moves);
			piece_moves(g, pmoves->p, &pmoves->moves);
			pmoves->modified[depth] = 1;
		}
	}

	b->player=g->player;
	next_player(g);
}

void branch_exit(game_t* g, move_vecs_t* vecs, char depth, branch_t* b) {
	piece_t* from = board_get(g, b->m->from);
	piece_t* to = board_get(g, b->m->to);

	unmove_noswap(g, b->m, from, to);

	*from = b->piece_from;
	*to = b->piece_to;

	vector_iterator pmoves_iter = vector_iterate(&vecs->moves);
	while (vector_next(&pmoves_iter)) {
		piece_moves_t* pmoves = pmoves_iter.x;
		if (pmoves->p==to || pmoves->p==from || pmoves->modified[depth]) {
			vector_clear(&pmoves->moves);
			piece_moves(g, pmoves->p, &pmoves->moves);
			pmoves->modified[depth] = 0;
		}
	}

	g->player=b->player;
}

move_vecs_t g_move_vecs = {.init=0};

float ai_find_move(move_vecs_t* vecs, game_t* g, float v, char depth, move_t* m_out) {
	if (v<-AI_MAX_LOSS) {
		return -CHECKMATE_CURVE[depth+1];
	}

	float gain=-INFINITY;
	float eqmvs=1;

	move_t m;
	vector_iterator pmoves_iter = vector_iterate(&vecs->moves);
	while (vector_next(&pmoves_iter)) {
		piece_moves_t* pmoves = pmoves_iter.x;
		if (!piece_owned(pmoves->p, g->player)) continue;

		m.from[0]=pmoves->pos[0]; m.from[1]=pmoves->pos[1];

		vector_iterator move_iter = vector_iterate(&pmoves->moves);
		while (vector_next(&move_iter)) {
			int* pos = move_iter.x;
			piece_t* target = board_get(g, pos);

			int e = piece_edible(target);
			if (depth>=vecs->finddepth && !e)
				continue;

			float v2 = v;
			if (e) v2 += piece_value(g, vecs, target);

			m.to[0] = pos[0]; m.to[1] = pos[1];
			branch_t* b = &vecs->branches[depth];

			if (++depth < AI_MAXDEPTH) {
				branch_enter(g, vecs, depth, b, &m);

				//make the unrealistic assumption that all enemy teams are allied, benefits are shared
				int inv = g->player==vecs->ai_player || b->player==vecs->ai_player;
				v2 = ai_find_move(vecs, g, inv ? -v2 : v2, depth, NULL);
				if (inv) v2 *= -1;

				branch_exit(g, vecs, depth, b);
			}

			depth--;

			if (v2>gain) {
				gain=v2;
				eqmvs=1;
				*m_out = m;
			} else if (depth==0 && v2==gain) {
				if ((float)RAND_MAX/(float)rand() > ++eqmvs)
					*m_out = m;
			}
		}
	}

	if (gain==-INFINITY) gain=-checkmate_value(g, depth);
	return gain;
}

void ai_make_move(game_t* g) {
	if (!g_move_vecs.init) {
		g_move_vecs.moves = vector_new(sizeof(piece_moves_t));
		g_move_vecs.init=1;
	}

	g_move_vecs.ai_player = g->player;

	unsigned len=0;

	vector_clear(&g_move_vecs.moves);

	int pos[2] = {-1,0};
	while (board_pos_next(g, pos)) {
		piece_t* p = board_get(g, pos);
		piece_moves_t pmoves = {.p=p, .pos={pos[0], pos[1]}, .modified={0}};
		pmoves.moves = vector_new(sizeof(int[2]));
		piece_moves(g, p, &pmoves.moves);

		len += pmoves.moves.length;

		vector_pushcpy(&g_move_vecs.moves, &pmoves);
	}

	g_move_vecs.finddepth = finddepth(len/g->players.length);
	printf("len %u depth %i\n", len, g_move_vecs.finddepth);

	move_t m;
	float v = ai_find_move(&g_move_vecs, g, 0, 0, &m);
	printf("move value: %f\n", v);

	make_move(g, &m, 0, 1, g->player);
}