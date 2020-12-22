#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "chess.h"

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

float piece_value(game_t* g, piece_t* p, vector_t* moves) {
	piece_moves(g, p, moves);
	float range = (float)moves->length/((float)(g->board_w*g->board_h));
	return range*2.0f + piecety_value(p->ty);
}

typedef struct {
	move_t m;
	piece_t piece_swap;
	char player;
} branch_t;

branch_t branch_enter(game_t* g, move_t m) {
	branch_t b;
	b.m = m;
	move_swap(g, &b.m);
	b.piece_swap=g->piece_swap;
	b.player=g->player;
	next_player(g, 1);

	return b;
}

void branch_exit(game_t* g, branch_t* b) {
	g->piece_swap=b->piece_swap;
	unmove_swap(g, board_get(g, b->m.from), board_get(g, b->m.to));
	next_player(g, 0);
	g->player=b->player;
	g->won=0;
}

#define AI_DEPTH 1
#define AI_EXCHANGE_DEPTH 2

typedef struct {
	vector_t moves[AI_DEPTH*2];
	vector_t target_moves[AI_DEPTH*2];

	vector_t exchange_moves[AI_EXCHANGE_DEPTH*2];
	vector_t exchangetarget_moves[AI_EXCHANGE_DEPTH*2];
	vector_t exchangenext_moves[AI_EXCHANGE_DEPTH*2];

	int init;
} move_vecs_t;

move_vecs_t g_move_vecs = {.init=0};

//finds most worthwhile exchange, rets value
float find_exchange(move_vecs_t* vecs, game_t* g, move_t* m_out, int depth, char* exists) {
	*exists = 0;
	if (depth==0) return 0;

	move_t m;
	float gain;

	for (m.from[0]=0; m.from[0]<g->board_w; m.from[0]++) {
		for (m.from[1]=0; m.from[1]<g->board_h; m.from[1]++) {
			piece_t* p = board_get(g, m.from);
			
			if (piece_owned(p, g->player)) {
				piece_moves(g, p, &vecs->exchange_moves[depth]);
				vector_iterator move_iter = vector_iterate(&vecs->exchange_moves[depth]);
				while (vector_next(&move_iter)) {
					piece_t* target = *(piece_t**)move_iter.x;
					if (target->ty != p_empty && target->player != g->player) {
						float v = g->won ? INFINITY : piece_value(g, target, &vecs->exchangetarget_moves[depth]);
						vector_clear(&vecs->exchangetarget_moves[depth]);

						board_pos(g, m.to, target);
						branch_t branch = branch_enter(g, m);

						char subexists=1;
						if (!g->won) {
							v += piece_value(g, target, &vecs->exchangenext_moves[depth]);
							vector_clear(&vecs->exchangenext_moves[depth]);
							float subv = find_exchange(vecs, g, NULL, depth-1, &subexists);
							if (subexists) v-=subv;
						} else {
							v = INFINITY;
						}

						branch_exit(g, &branch);

						if (!*exists || v>gain) {
							gain=v;
							*exists=1;
							*m_out = branch.m;

							if (v==INFINITY) break;
						}
					}
				}

				vector_clear(&vecs->exchange_moves[depth]);
			}
		}
	}

	return *exists ? gain : 0;
}

float ai_find_move(move_vecs_t* vecs, game_t* g, int depth, move_t* m_out) {
	char exists;
	float max = find_exchange(vecs, g, m_out, AI_EXCHANGE_DEPTH*2-1, &exists);
	if (!exists) max=-INFINITY;

	if (depth==0) {
		printf("leaf %f\n", exists ? max : 0);
		return exists ? max : 0; //set to zero for leaf, which is used as an offset. otherwise continue searching
	}

	float eqmvs=1;

	//awfully similar
	move_t m;
	for (m.from[0]=0; m.from[0]<g->board_w; m.from[0]++) {
		for (m.from[1]=0; m.from[1]<g->board_h; m.from[1]++) {
			piece_t* p = board_get(g, m.from);

			if (piece_owned(p, g->player)) {
				piece_moves(g, p, &vecs->moves[depth]);

				vector_iterator move_iter = vector_iterate(&vecs->moves[depth]);
				while (vector_next(&move_iter)) {
					piece_t* target = *(piece_t**)move_iter.x;
					float v = piece_value(g, target, &vecs->target_moves[depth]);
					vector_clear(&vecs->target_moves[depth]);

					board_pos(g, m.to, target);
					branch_t branch = branch_enter(g, m);

					move_t subm;
					if (g->won) v = INFINITY;
					else v -= ai_find_move(vecs, g, depth-1, &subm);

					if (v>max) {
						max=v;
						eqmvs=1;
						*m_out = m;
					} else if (v==max) {
						eqmvs++;
						if ((float)RAND_MAX/(float)rand() < eqmvs)
							*m_out = m;
					}

					branch_exit(g, &branch);
				}

				vector_clear(&vecs->moves[depth]);
			}
		}
	}

	printf("ret %f\n", max);
	return max;
}

void ai_make_move(game_t* g) {
	if (!g_move_vecs.init) {
		for (int i=0; i<AI_DEPTH*2; i++) {
			g_move_vecs.moves[i] = vector_new(sizeof(move_t));
			g_move_vecs.target_moves[i] = vector_new(sizeof(move_t));
		}

		for (int i=0; i<AI_EXCHANGE_DEPTH*2; i++) {
			g_move_vecs.exchange_moves[i] = vector_new(sizeof(move_t));
			g_move_vecs.exchangetarget_moves[i] = vector_new(sizeof(move_t));
			g_move_vecs.exchangenext_moves[i] = vector_new(sizeof(move_t));
		}

		g_move_vecs.init=1;
	}

	move_t m;
	ai_find_move(&g_move_vecs, g, AI_DEPTH*2-1, &m);

	make_move(g, &m, 0, 1, g->player);
}