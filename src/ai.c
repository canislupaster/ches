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

float piece_value(game_t* g, piece_t* p, vector_t* moves) {
	piece_moves(g, p, moves);
	float range = (float)moves->length/((float)(g->board_w*g->board_h));
	return range*40.0f + piecety_value(p->ty);
}

#define AI_DEPTH 2 //base search
#define AI_EXCHANGE_DEPTH 6 //depth with likely exchanges
#define AI_FINDDEPTH 20 //absolute maximum depth, including with extensions
#define AI_EXPECTEDLEN 19 //more than 10 pieces per move, otherwise extend by log2(10/len)

char finddepth(unsigned len) {
	return (char)max((int)roundf(log2f(AI_EXPECTEDLEN/(float)len)),0);
}

//take shortest checkmate, no matter depth
//weights indexed by how deep mate is or how many other players get turn
//populated to depth or zeroed
float CHECKMATE_CURVE[] = {INFINITY, 100, 75, 60, 50, 40, 35};

float checkmate_value(game_t* g, int d) {
	player_t* p = vector_get(&g->players, g->player);
	int check = player_check(g, g->player, p);
	d += (int)g->players.length-2 + 1-check;
	if (d>=AI_EXCHANGE_DEPTH+1) return CHECKMATE_CURVE[AI_EXCHANGE_DEPTH];
	else return CHECKMATE_CURVE[d];
}

typedef struct {
	move_t* m;
	piece_t piece_from;
	piece_t piece_to;
	char player;
} branch_t;

typedef struct move_vecs {
	vector_t moves[AI_FINDDEPTH];
	vector_t target_moves[AI_FINDDEPTH];

	branch_t branches[AI_FINDDEPTH];

	char ai_player;
	//extend depth of search depending on complexity
	char finddepth;

	int init;
} move_vecs_t;

void branch_enter(game_t* g, branch_t* b, move_t* m) {
	b->m = m;

	piece_t* from = board_get(g, b->m->from);
	piece_t* to = board_get(g, b->m->to);

	b->piece_from=*from;
	b->piece_to=*to;

	move_noswap(g, b->m, from, to);

	b->player=g->player;
	next_player(g);
}

void branch_exit(game_t* g, branch_t* b) {
	piece_t* from = board_get(g, b->m->from);
	piece_t* to = board_get(g, b->m->to);

	unmove_noswap(g, b->m, from, to);

	*from = b->piece_from;
	*to = b->piece_to;

	g->player=b->player;
}

move_vecs_t g_move_vecs = {.init=0};

float ai_find_move(move_vecs_t* vecs, game_t* g, float v, char depth, move_t* m_out) {
	if (v<-AI_MAX_LOSS) {
		return CHECKMATE_CURVE[depth+1];
	}

	if (depth>=vecs->finddepth+AI_EXCHANGE_DEPTH) {
		return v;
	}

	float gain=-INFINITY;
	float eqmvs=1;

	move_t m;
	m.from[0]=-1; m.from[1]=0;
	while (board_pos_next(g, m.from)) {
		if (m.from[1]==g->board_h) {
			printf("why1\n");
			abort();
		}
		piece_t* p = board_get(g, m.from);
		if (!piece_owned(p, g->player)) continue;

		piece_moves(g, p, &vecs->moves[depth]);

		vector_iterator move_iter = vector_iterate(&vecs->moves[depth]);
		while (vector_next(&move_iter)) {
			int* pos = move_iter.x;
			piece_t* target = board_get(g, pos);

			if (depth>=vecs->finddepth+AI_DEPTH && target->ty == p_empty)
				continue;

			float v2 = v + piece_value(g, target, &vecs->target_moves[depth]);
			vector_clear(&vecs->target_moves[depth]);

			m.to[0] = pos[0]; m.to[1] = pos[1];
			branch_t* b = &vecs->branches[depth];
			branch_enter(g, b, &m);

			//make the unrealistic assumption that all enemy teams are allied, benefits are shared
			int inv = g->player==vecs->ai_player || b->player==vecs->ai_player;
			v2 = ai_find_move(vecs, g, inv ? -v2 : v2, depth+1, NULL);
			if (inv) v2 *= -1;

			branch_exit(g, b);

			if (m.from[1]==g->board_h) {
				printf("why\n");
				abort();
			}
			if (v2>gain) {
				gain=v2;
				eqmvs=1;
				*m_out = m;
			} else if (depth==0 && v2==gain) {
				if ((float)RAND_MAX/(float)rand() > ++eqmvs)
					*m_out = m;
			}
		}

		vector_clear(&vecs->moves[depth]);
	}

	if (gain==-INFINITY) gain=-checkmate_value(g, depth);
	return gain;
}

void ai_make_move(game_t* g) {
	if (!g_move_vecs.init) {
		for (int i=0; i<AI_FINDDEPTH; i++) {
			g_move_vecs.moves[i] = vector_new(sizeof(int[2]));
			g_move_vecs.target_moves[i] = vector_new(sizeof(int[2]));
		}

		g_move_vecs.init=1;
	}

	g_move_vecs.ai_player = g->player;

	int pos[2] = {-1,0};
	while (board_pos_next(g, pos)) {
		piece_t* p = board_get(g, pos);
		piece_moves(g, p, &g_move_vecs.moves[0]);
	}

	printf("len %i\n", g_move_vecs.moves[0].length);
	g_move_vecs.finddepth = finddepth(g_move_vecs.moves[0].length/g->players.length);
	vector_clear(&g_move_vecs.moves[0]);
	g_move_vecs.finddepth = min(g_move_vecs.finddepth+AI_EXCHANGE_DEPTH, AI_FINDDEPTH-1)-AI_EXCHANGE_DEPTH;

	printf("%i\n", g_move_vecs.finddepth);

	move_t m;
	float v = ai_find_move(&g_move_vecs, g, 0, 0, &m);
	printf("move value: %f\n", v);

	make_move(g, &m, 0, 1, g->player);
}