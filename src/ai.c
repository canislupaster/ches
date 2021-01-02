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
		case p_heir:
		case p_king: return 2;
		default: return 0;
	}
}

#define AI_RANGEVAL 0.0f
#define AI_DIMINISH 1.2f //diminish returns by this, otherwise ai thinks an easily evaded checkmate is inevitable

#define AI_DEPTH 2 //minimum search
#define AI_MAXDEPTH 60 //eventual depth
#define AI_BRANCHDEPTH 4 //depth+exchangedepth
#define AI_EXPECTEDLEN 12800 //more than this number of moves, otherwise extend by log2(expected/len)
#define AI_LEN 10
#define AI_MAXPLAYER 4

int maxdepth(unsigned len) {
	return (char)min(max((int)roundf((log2f(AI_EXPECTEDLEN/(float)len)+1)*AI_BRANCHDEPTH), AI_BRANCHDEPTH), AI_MAXDEPTH);
}

//take shortest checkmate, no matter depth
//weights indexed by how deep mate is or how many other players get turn
//populated to depth or zeroed

typedef struct {
	move_t m;
	piece_t piece_from;
	piece_t piece_to;
	char checks[AI_MAXPLAYER];
	char player;
	char ally;
} branch_t;

typedef struct {
	piece_t* p;
	int pos[2];
	vector_cap_t moves;
	char modified[AI_MAXDEPTH];
} piece_moves_t;

typedef struct {
	vector_t branches;
	char keep; //1 is either stale or completed
	float v;
	unsigned depth;
} superbranch_t;

typedef struct move_vecs {
	vector_t moves; //piece_moves_t
	char ally;

	vector_t sbranches; //at most AI_LEN
	vector_t sbranches_new;
	superbranch_t first;
	superbranch_t* sbranch; //current
	unsigned min;
	char checks[AI_MAXPLAYER];

	char ai_player;
	player_t* ai_p;

	int finddepth;
	int maxdepth;
	int init;
} move_vecs_t;

float piece_value(game_t* g, move_vecs_t* vecs, piece_t* p) {
	vector_iterator p_search = vector_iterate(&vecs->moves);
	float range = 0;
	while (vector_next(&p_search)) {
		piece_moves_t* pmoves = p_search.x;
		if (pmoves->p == p) {
			range = (float) pmoves->moves.vec.length;
			break;
		}
	}

	range /= ((float) (g->board_w * g->board_h));
	return range * AI_RANGEVAL + piecety_value(p->ty);
}

#define CHECKMATE_VAL 1000.0f

float checkmate_value(game_t* g, move_vecs_t* vecs) {
	//stalemate, indesirable to either player
	if (!vecs->checks[g->player]) return (vecs->ally ? 1.0f : -1.0f) * CHECKMATE_VAL;
	else return CHECKMATE_VAL;
}

void branch_push(move_vecs_t* vecs) {
	vector_pushcpy(&vecs->sbranch->branches, &(branch_t){.checks={0}});
}

int branch_init(game_t* g, move_vecs_t* vecs, branch_t* b, unsigned depth, move_t m, char make, char enter) {
	if (make) b->m = m;

	piece_t* from = board_get(g, b->m.from);
	piece_t* to = board_get(g, b->m.to);

	if (make) {
		b->player = g->player;
		b->ally = vecs->ally;

		b->piece_from = *from;
		b->piece_to = *to;
	} else if (enter) { //toggle checks
		for (char i = 0; i < g->players.length; i++) {
			vecs->checks[i] ^= b->checks[i];
		}
	}

	move_noswap(g, &b->m, from, to);

	if (make) {
		vector_iterator player_iter = vector_iterate(&g->players);
		while (vector_next(&player_iter)) {
			char check = (char)player_check(g, player_iter.i, player_iter.x);

			if (check && player_iter.i==b->player) {
				unmove_noswap(g, &b->m, from, to);
				*from = b->piece_from;
				*to = b->piece_to;
				return 0;
			}

			b->checks[player_iter.i] = check!=vecs->checks[player_iter.i];
			if (enter) vecs->checks[player_iter.i] = check;
		}
	}

	if (enter) {
		vector_iterator pmoves_iter = vector_iterate(&vecs->moves);
		while (vector_next(&pmoves_iter)) {
			piece_moves_t* pmoves = pmoves_iter.x;

			if (!piece_edible(pmoves->p)) continue;

			//when having >2 players, update moves if check is still ongoing
			if (pmoves->p==to || piece_moves_modified(g, pmoves->p, pmoves->pos, &b->m)) {
				vector_clear(&pmoves->moves.vec);
				piece_moves(g, pmoves->p, &pmoves->moves.vec, 0);

				pmoves->modified[depth] = 1;
			}
		}

		next_player(g);
		vecs->ally = (char)is_ally(vecs->ai_player, vecs->ai_p, g->player);
	} else {
		unmove_noswap(g, &b->m, from, to);
		*from = b->piece_from;
		*to = b->piece_to;
	}

	return 1;
}

void branch_reenter(game_t* g, move_vecs_t* vecs, branch_t* b, unsigned depth) {
	branch_init(g, vecs, b, depth, (move_t) {0}, 0, 1);
}

void branch_exit(game_t* g, move_vecs_t* vecs, branch_t* b, unsigned depth) {
	piece_t* from = board_get(g, b->m.from);
	piece_t* to = board_get(g, b->m.to);

	unmove_noswap(g, &b->m, from, to);

	*from = b->piece_from;
	*to = b->piece_to;

	vector_iterator pmoves_iter = vector_iterate(&vecs->moves);
	while (vector_next(&pmoves_iter)) {
		piece_moves_t* pmoves = pmoves_iter.x;
		if (pmoves->p==from || pmoves->modified[depth]) {
			vector_clear(&pmoves->moves.vec);
			piece_moves(g, pmoves->p, &pmoves->moves.vec, 0);
			pmoves->modified[depth] = 0;
		}
	}

	for (char i = 0; i < g->players.length; i++) {
		vecs->checks[i] ^= b->checks[i];
	}

	g->player = b->player;
	vecs->ally = b->ally;
}

move_vecs_t g_move_vecs = {.init=0};

unsigned ai_hash_branches(branch_t* branches, unsigned l) {
	unsigned x=1;
	for (unsigned i = 0; i<l; i++) {
		if (branches[i].m.from[0] == -1) break;
		x *= branches[i].m.from[0]+branches[i].m.from[1] + 1;
		x *= branches[i].m.to[0]+branches[i].m.to[1] + 1;
	}

	return x;
}

void sbranch_push(move_vecs_t* vecs, branch_t* len_branches, float v) {
	superbranch_t* min = vector_get(&vecs->sbranches_new, vecs->min);
	superbranch_t* new_sb;

	if (vecs->sbranches_new.length==AI_LEN) {
		if (v <= min->v) { //retain ordering
			return;
		}

		vector_free(&min->branches);
		new_sb = min;
	} else {
		new_sb = vector_pushcpy(&vecs->sbranches_new, vecs->sbranch);
	}

	if (!min || v<min->v) {
		vecs->min = vecs->sbranches_new.length-1;
	} else {
		min = vector_get(&vecs->sbranches_new, 0);
		vecs->min=0;

		vector_iterator sbranch_iter = vector_iterate(&vecs->sbranches_new);

		vector_next(&sbranch_iter);
		while (vector_next(&sbranch_iter)) {
			superbranch_t* sb = sbranch_iter.x;
			if (sb->v < min->v) {
				min = sb;
				vecs->min=sbranch_iter.i;
			}
		}
	}

	if (len_branches) {
		new_sb->branches = vector_new(sizeof(branch_t));
		vector_stockcpy(&new_sb->branches, vecs->sbranch->depth, vecs->sbranch->branches.data);

		unsigned char l = AI_BRANCHDEPTH;
		for (unsigned char i = 0; i < AI_BRANCHDEPTH; i++) {
			if (len_branches[i].m.from[0] == -1) {
				l = i;
				break;
			}
		}

		vector_stockcpy(&new_sb->branches, l, len_branches);
	} else {
		vector_cpy(&vecs->sbranch->branches, &new_sb->branches);
	}

	new_sb->depth = new_sb->branches.length;

	new_sb->v = v;
	new_sb->keep=0;
}

void ai_copy_best(move_vecs_t* vecs, branch_t* best, unsigned depth) {
	memcpy(best, vector_get(&vecs->sbranch->branches, vecs->sbranch->depth), sizeof(branch_t) * depth);
	if (depth < AI_BRANCHDEPTH) {
		best[depth].m.from[0] = -1;
	}
}

float ai_find_move(move_vecs_t* vecs, game_t* g, float v, int depth, branch_t* best) {
	float gain = -INFINITY;

	int exchange = depth >= vecs->finddepth;
	char ally = vecs->ally;

	int moves = 0;

	branch_push(vecs);
	unsigned bdepth = vecs->sbranch->depth + depth;

	//space to find another move / another branch. there is space for the branch in 3 lines
	int space = vecs->sbranch->branches.length < vecs->maxdepth && depth+1 < AI_BRANCHDEPTH;

	move_t m;
	vector_iterator pmoves_iter = vector_iterate(&vecs->moves);
	while (vector_next(&pmoves_iter)) {
		piece_moves_t* pmoves = pmoves_iter.x;
		if (!piece_owned(pmoves->p, g->player)) continue;

		m.from[0] = pmoves->pos[0];
		m.from[1] = pmoves->pos[1];

		vector_iterator move_iter = vector_iterate(&pmoves->moves.vec);
		while (vector_next(&move_iter)) {
			int* pos = move_iter.x;

			m.to[0] = pos[0];
			m.to[1] = pos[1];

			piece_t* target = board_get(g, pos);

			int e = piece_edible(target);
			if (exchange && !e) {
				if (!moves) {
					if (branch_init(g, vecs, vector_get(&vecs->sbranch->branches, bdepth), bdepth, m, 1, 0))
						moves=1;
				}

				continue;
			}

			float v2 = v;
			if (e) v2 += piece_value(g, vecs, target);

			if (!branch_init(g, vecs, vector_get(&vecs->sbranch->branches, bdepth), bdepth, m, 1, (char)space))
				continue;

			if (space) {
				branch_t subbest[AI_BRANCHDEPTH];

				//make the unrealistic assumption that all enemy teams are allied, benefits are shared
				int inv = ally != vecs->ally;
				v2 = ai_find_move(vecs, g, inv ? -v2 : v2, depth + 1, subbest);
				if (inv) v2 *= -1;

				branch_exit(g, vecs, vector_get(&vecs->sbranch->branches, bdepth), bdepth);

				if (depth == 0) {
					sbranch_push(vecs, subbest, v2);
					if (!ally) {
						print_board(g);
						errx("exceptional error #1");
					}
				} else if (v2 > gain) {
					memcpy(best, subbest, AI_BRANCHDEPTH*sizeof(branch_t));
				}
			} else if (v2 > gain) {
				if (depth == 0) {
					sbranch_push(vecs, NULL, v2);
					if (!ally) {
						print_board(g);
						errx("exceptional error #2\n");
					}
				} else {
					ai_copy_best(vecs, best, depth+1);
				}
			}

			if (v2 > gain) gain = v2;
		}
	}

	vector_pop(&vecs->sbranch->branches);

	if (exchange && (gain!=-INFINITY || moves) && gain<v) {
		if (!ally) {
			int oldfd = vecs->finddepth;
			vecs->finddepth = depth+1;
			v = ai_find_move(vecs, g, v, depth, best);
			vecs->finddepth = oldfd;
		} else {
			ai_copy_best(vecs, best, depth);
		}

		return v;
	} else if (gain == -INFINITY) {
		if (depth>0) {
			ai_copy_best(vecs, best, depth);
			return -checkmate_value(g, vecs);
		} else {
			vecs->sbranch->keep=1;
			vecs->sbranch->v = ally ? -checkmate_value(g, vecs) : checkmate_value(g, vecs);
			return 0;
		}
	} else {
		return gain;
	}
}

void ai_make_move(game_t* g, move_t* out_m) {
	if (!g_move_vecs.init) {
		g_move_vecs.first.branches = vector_new(sizeof(branch_t));
		g_move_vecs.sbranches_new = vector_new(sizeof(superbranch_t));
		g_move_vecs.moves = vector_new(sizeof(piece_moves_t));
		g_move_vecs.init = 1;
	}

	vector_clear(&g_move_vecs.first.branches);

	vector_iterator p_iter = vector_iterate(&g->players);
	while (vector_next(&p_iter)) {
		g_move_vecs.checks[p_iter.i] = (char)player_check(g, p_iter.i, p_iter.x);
	}

	g_move_vecs.ai_player = g->player;
	g_move_vecs.ai_p = vector_get(&g->players, g->player);
	g_move_vecs.ally = 1;

	unsigned len = 0;

	vector_iterator pm_iter = vector_iterate(&g_move_vecs.moves);
	while (vector_next(&pm_iter)) {
		piece_moves_t* pmoves = pm_iter.x;
		vector_free(&pmoves->moves.vec);
	}

	vector_clear(&g_move_vecs.moves);

	int pos[2] = {-1, 0};
	while (board_pos_next(g, pos)) {
		piece_t* p = board_get(g, pos);
		piece_moves_t pmoves = {.p=p, .pos={pos[0], pos[1]}};
		memset(pmoves.modified, 0, AI_MAXDEPTH);
		pmoves.moves = vector_alloc(vector_new(sizeof(int[2])), 0);
		piece_moves(g, p, &pmoves.moves.vec, 0);

		len += pmoves.moves.vec.length;

		vector_pushcpy(&g_move_vecs.moves, &pmoves);
	}

	//"depth"
	g_move_vecs.maxdepth = maxdepth(len);
	printf("len %u depth %i\n", len, g_move_vecs.maxdepth);
	//"breadth"
	g_move_vecs.finddepth = AI_DEPTH;

	g_move_vecs.first.depth = 0;

	g_move_vecs.sbranch = &g_move_vecs.first;
	ai_find_move(&g_move_vecs, g, 0, 0, NULL);

	vector_t sbranches_keep = vector_new(sizeof(superbranch_t));

	int cont = 1;
	vector_iterator sbranch_iter;
	while (cont) {
		g_move_vecs.sbranches = g_move_vecs.sbranches_new;
		g_move_vecs.sbranches_new = vector_new(sizeof(superbranch_t));

		cont=0;
		sbranch_iter = vector_iterate(&g_move_vecs.sbranches);
		while (vector_next(&sbranch_iter)) {
			superbranch_t* sbranch = sbranch_iter.x;
			if (sbranch->keep) {
				continue;
			} else if (sbranch->branches.length >= g_move_vecs.maxdepth) {
				sbranch->keep=1;
				continue;
			} else {
				cont = 1;
			}

			vector_iterator branch_iter = vector_iterate(&sbranch->branches);

			g_move_vecs.sbranch = sbranch;
			while (vector_next(&branch_iter)) {
				branch_reenter(g, &g_move_vecs, branch_iter.x, branch_iter.i);
			}

			sbranch->v *= AI_DIMINISH;
			ai_find_move(&g_move_vecs, g, sbranch->v, 0, NULL);

			while (vector_prev(&branch_iter)) {
				branch_exit(g, &g_move_vecs, branch_iter.x, branch_iter.i);
			}
		}

		sbranch_iter = vector_iterate(&g_move_vecs.sbranches);
		while (vector_next(&sbranch_iter)) {
			superbranch_t* sbranch = sbranch_iter.x;
			if (sbranch->keep) {
				vector_pushcpy(&sbranches_keep, sbranch);
			} else {
				vector_free(&sbranch->branches);
			}
		}

		vector_free(&g_move_vecs.sbranches);
	}

	superbranch_t* max = vector_get(&sbranches_keep, 0);
	sbranch_iter = vector_iterate(&sbranches_keep);

	vector_next(&sbranch_iter);
	while (vector_next(&sbranch_iter)) {
		superbranch_t* sb = sbranch_iter.x;
		sb->v *= powf(AI_DIMINISH, (float)(g_move_vecs.maxdepth-sb->branches.length));
		if (sb->v>max->v) max=sb;
	}

	printf("move value: %f\n", max->v);

	branch_t* fbranch = vector_get(&max->branches, 0);
	make_move(g, &fbranch->m, 0, 1, g->player);
	if (out_m) *out_m = fbranch->m;

	sbranch_iter = vector_iterate(&sbranches_keep);
	while (vector_next(&sbranch_iter)) {
		superbranch_t* sb = sbranch_iter.x;
		vector_free(&sb->branches);
	}
}