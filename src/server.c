#include "chess.h"
#include "chessfrontend.h"
#include "network.h"
#include "ai.h"

typedef struct {
	server_t server;
	vector_t games;
	map_t num_joined;
	vector_t num_lobby;

	//filemap_t users;
	//filemap_index_t users_ip;
} chess_server_t;

typedef struct {
	char* name;
	game_t g;
	vector_t player_num;
	unsigned host; //pnum
	char full;
} mp_game_t;

void broadcast(chess_server_t* cserv, vector_t* nums, vector_t* data, unsigned num_exclude) {
	vector_iterator num_iter = vector_iterate(nums);
	while (vector_next(&num_iter)) {
		unsigned i = *(unsigned*)num_iter.x;
		if (i == 0 || i==num_exclude) continue;
		server_send(&cserv->server, i, data);
	}
}

int game_in(chess_server_t* cserv, unsigned i, mp_game_t** mg, unsigned* pnum) {
	mp_game_t** mg_ref = map_find(&cserv->num_joined, &i);
	if (!mg_ref) return 0;

	*mg = *mg_ref;
	*pnum = vector_search(&(*mg_ref)->player_num, &i);
	return 1;
}

void leave_game(chess_server_t* cserv, unsigned i) {
	if (vector_search_remove(&cserv->num_lobby, &i)) return;

	mp_game_t** mg_ref = map_find(&cserv->num_joined, &i);
	if (!mg_ref) return;
	
	mp_game_t* mg = *mg_ref;
	unsigned g_i = vector_search(&cserv->games, &mg);

	int left=0;

	unsigned pnum = 0;
	unsigned pnum_left=0;

	vector_iterator pnum_iter = vector_iterate(&mg->player_num);
	while (vector_next(&pnum_iter)) {
		unsigned* num = pnum_iter.x;
		if (*num == i) {
			*num = 0;
			pnum = pnum_iter.i;

			//remove spectators to preserve indices; they arent mapped to g.players
			if (pnum_iter.i>=mg->g.players.length) {
				vector_remove(&mg->player_num, pnum_iter.i);
				pnum_iter.i--;
			}
		} else if (*num>0 && !left) {
			left=1;
			pnum_left = pnum_iter.i;
		}
	}

	map_remove(&cserv->num_joined, &i);

	vector_t data = vector_new(1);

	if (!left) {
		vector_pushcpy(&data, &(char){mp_game_list_removed});
		write_uint(&data, g_i);
		broadcast(cserv, &cserv->num_lobby, &data, 0);

		vector_remove(&cserv->games, g_i);

		game_free(&mg->g);
		mp_extra_free(&mg->g.m);

		vector_free(&mg->player_num);
		drop(mg->name);
		drop(mg);
	} else {
		pnum_leave(&mg->g, pnum);

		if (pnum<mg->g.players.length && mg->full) {
			mg->full=0;
			
			vector_pushcpy(&data, &(char){mp_game_list_full});
			write_uint(&data, g_i);
			vector_pushcpy(&data, &(char){mg->full});
			broadcast(cserv, &cserv->num_lobby, &data, 0);
			vector_clear(&data);
		}
		
		if (pnum==mg->g.m.host) mg->g.m.host = pnum_left;

		vector_pushcpy(&data, &(char){mp_game_left});
		write_uint(&data, pnum);
		write_uint(&data, mg->g.m.host);

		broadcast(cserv, &mg->player_num, &data, 0);
	}
	
	vector_free(&data);
}

int main(int argc, char** argv) {
	chess_server_t cserv = {.server=start_server(MP_PORT, 1), .games=vector_new(sizeof(mp_game_t*)), .num_joined=map_new(), .num_lobby=vector_new(sizeof(unsigned))};
	map_configure_uint_key(&cserv.num_joined, sizeof(mp_game_t*));

	unsigned i;
	vector_t resp = vector_new(1);

	while (1) {
		cur_t cur = server_recv(&cserv.server, &i);

		mp_client_t op = mp_leave_game;
		if (cur.start) op = (mp_client_t)read_chr(&cur);

		switch (op) {
			case mp_list_game: {
				leave_game(&cserv, i);

				vector_pushcpy(&resp, &(char){mp_game_list});
				write_uint(&resp, cserv.games.length);

				vector_iterator game_iter = vector_iterate(&cserv.games);
				while (vector_next(&game_iter)) {
					mp_game_t* g = *(mp_game_t**)game_iter.x;
					vector_pushcpy(&resp, &(char){g->full});
					write_str(&resp, g->name);
				}

				vector_pushcpy(&cserv.num_lobby, &i);
				break;
			}
			case mp_make_game: {
				leave_game(&cserv, i);

				game_t g;
				char* g_name = read_str(&cur);
				unsigned len = strlen(g_name);
				if (len==0 || len>GAMENAME_MAXLEN) break;

				char joined, full;
				read_game(&cur, &g, &joined, &full);

				if (cur.err) {
					if (g_name) drop(g_name);
					break;
				} else if (joined<0) {
					if (g_name) drop(g_name);
					game_free(&g);
					break;
				}

				mp_game_t* mg = heapcpy(sizeof(mp_game_t), &(mp_game_t){.g=g, .name=g_name, .player_num=vector_new(sizeof(unsigned)), .full=full});
				mg->g.m.spectators = vector_new(sizeof(char*));
				mg->g.m.host = (unsigned)joined;

				map_insertcpy(&cserv.num_joined, &i, &mg);

				vector_populate(&mg->player_num, mg->g.players.length, &(unsigned*){0});
				vector_setcpy(&mg->player_num, (unsigned)joined, &i);

				vector_pushcpy(&cserv.games, &mg);
				
				vector_pushcpy(&resp, &(char){mp_game_list_new});
				vector_pushcpy(&resp, &(char){mg->full});
				write_str(&resp, g_name);
				broadcast(&cserv, &cserv.num_lobby, &resp, 0);
				vector_clear(&resp);	

				vector_pushcpy(&resp, &(char){mp_game_made});

				break;
			}
			case mp_join_game: {
				leave_game(&cserv, i);

				unsigned g_i = read_uint(&cur);
				char* name = read_str(&cur);
				if (cur.err) break;

				if (g_i>=cserv.games.length) {
					drop(name);
					break;
				}

				mp_game_t* mg = *(mp_game_t**)vector_get(&cserv.games, g_i);
				mg->full=1;
				
				vector_iterator p_iter = vector_iterate(&mg->g.players);
				player_t* p;
				unsigned p_i=-1;
				
				while (vector_next(&p_iter)) {
				  player_t* p2 = p_iter.x;
				  if (p2->joined || p2->mate) {
				  	continue;
				  } else if (p_i==-1) {
				  	p=p2; p_i=p_iter.i;
				  } else {
				  	mg->full=0;
				  }
				}

				if (p_i==-1) {
					vector_pushcpy(&mg->g.m.spectators, &name);
				} else {
					p->joined = 1;
					if (strlen(name)>0) {
						drop(p->name);
						p->name = name;
					} else {
						drop(name);
					}
					
					if (mg->full) {
						vector_pushcpy(&resp, &(char){mp_game_list_full});
						write_uint(&resp, g_i);
						vector_pushcpy(&resp, &(char){mg->full});
						broadcast(&cserv, &cserv.num_lobby, &resp, 0);
						vector_clear(&resp);
					}
				}

				unsigned pnum = p_i==-1 ? mg->g.players.length+mg->g.m.spectators.length-1 : p_i;

				vector_pushcpy(&resp, &(char){mp_game_joined});
				write_uint(&resp, pnum);
				write_str(&resp, p_i==-1 ? name : p->name);

				broadcast(&cserv, &mg->player_num, &resp, 0);

				vector_setcpy(&mg->player_num, pnum, &i);
				map_insertcpy(&cserv.num_joined, &i, &mg);

				vector_clear(&resp);
				vector_pushcpy(&resp, &(char){mp_game});

				write_game(&resp, &mg->g);
				write_mp_extra(&resp, &mg->g.m);

				write_uint(&resp, pnum);

				break;
			}
			case mp_make_move: {
				mp_game_t* mg;
				unsigned player;
				move_t m = read_move(&cur);

				if (!game_in(&cserv, i, &mg, &player) || cur.err) break;
				if (make_move(&mg->g, &m, 1, 1, (char)player) != move_success) break;

				vector_pushcpy(&resp, &(char){mp_move_made});
				write_move(&resp, &m);
				broadcast(&cserv, &mg->player_num, &resp, i);
				vector_clear(&resp);

				break;
			}
			case mp_ai_move: {
				mp_game_t* mg;
				unsigned player;
				move_t m = read_move(&cur);

				if (!game_in(&cserv, i, &mg, &player) || cur.err) break;

				player_t* p = vector_get(&mg->g.players, mg->g.player);
				if (player!=mg->g.m.host || mg->g.won || !p->ai) break;

				if (make_move(&mg->g, &m, 1, 1, (char)mg->g.player) != move_success) break;

				vector_pushcpy(&resp, &(char){mp_move_made});
				write_move(&resp, &m);
				broadcast(&cserv, &mg->player_num, &resp, i);

				vector_clear(&resp);

				break;
			}
			case mp_undo_move: {
				mp_game_t* mg;
				unsigned player;
				if (!game_in(&cserv, i, &mg, &player)) break;
				if (mg->g.last_player!=player) break;

				unsigned move_cur = mg->g.moves.length;
				set_move_cursor(&mg->g, &move_cur, mg->g.last_move);

				undo_move(&mg->g);

				vector_pushcpy(&resp, &(char){mp_move_undone});
				broadcast(&cserv, &mg->player_num, &resp, i);
				vector_clear(&resp);
				break;
			}
			case mp_leave_game: {
				leave_game(&cserv, i);
				break;
			}
			default:;
		}

		if (cur.start) drop(cur.start);
		if (resp.length>0) {
			server_send(&cserv.server, i, &resp);
			vector_clear(&resp);
		}
	}
}