#include "chess.h"
#include "network.h"

typedef struct {
	server_t server;
	vector_t games;
	map_t num_joined;
	vector_t num_lobby;

	//filemap_t users;
	//filemap_index_t users_ip;
} chess_server_t;

void broadcast(chess_server_t* cserv, vector_t* nums, vector_t* data, unsigned num_exclude) {
	vector_iterator num_iter = vector_iterate(nums);
	while (vector_next(&num_iter)) {
		unsigned i = *(unsigned*)num_iter.x;
		if (i == 0 || i==num_exclude) continue;
		server_send(&cserv->server, i, data);
	}
}

void leave_game(chess_server_t* cserv, unsigned i) {
	if (vector_search_remove(&cserv->num_lobby, &i)) return;

	mp_game_t** mg_ref = map_find(&cserv->num_joined, &i);
	if (!mg_ref) return;
	mp_game_t* mg = *mg_ref;

	int left=0;
	char p_i = 0;
	vector_iterator pnum_iter = vector_iterate(&mg->player_num);
	while (vector_next(&pnum_iter)) {
		unsigned* num = pnum_iter.x;
		if (*num == i) {
			*num = 0;
			p_i = (char)(pnum_iter.i-1);
		} else if (*num>0) {
			left=1;
		}
	}

	map_remove(&cserv->num_joined, &i);

	vector_t data = vector_new(1);
	
	if (!left) {
		unsigned g_i = vector_search(&cserv->games, &mg)-1;

		vector_pushcpy(&data, &(char){(char)mp_game_list_removed});
		write_uint(&data, g_i);
		broadcast(cserv, &cserv->num_lobby, &data, 0);

		vector_remove(&cserv->games, g_i);

		game_free(&mg->g);
		vector_free(&mg->player_num);
		drop(mg->name);
		drop(mg);
	} else {
		player_t* p = vector_get(&mg->g.players, p_i);
		p->joined=0;

		vector_pushcpy(&data, &(char){(char)mp_game_left});
		vector_pushcpy(&data, &(char){p_i});
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

				vector_pushcpy(&resp, &(char){(char)mp_game_list});
				write_uint(&resp, cserv.games.length);

				vector_iterator game_iter = vector_iterate(&cserv.games);
				while (vector_next(&game_iter)) {
					mp_game_t* g = *(mp_game_t**)game_iter.x;
					write_str(&resp, g->name);
				}

				vector_pushcpy(&cserv.num_lobby, &i);
				break;
			}
			case mp_make_game: {
				leave_game(&cserv, i);

				game_t g = {0};
				char* g_name = read_str(&cur);

				char joined;
				read_players(&cur, &g, &joined);
				read_board(&cur, &g);
				g.moves = vector_new(sizeof(move_t));

				if (cur.err||joined<0) {
					game_free(&g);
					break;
				}

				mp_game_t* mg = heapcpy(sizeof(mp_game_t), &(mp_game_t){.g=g, .name=g_name, .player_num=vector_new(sizeof(unsigned))});
				map_insertcpy(&cserv.num_joined, &i, &mg);

				vector_populate(&mg->player_num, g.players.length, &(unsigned){0});
				vector_setcpy(&mg->player_num, (unsigned)joined, &i);

				vector_pushcpy(&cserv.games, &mg);
				
				vector_pushcpy(&resp, &(char){(char)mp_game_list_new});
				write_str(&resp, g_name);
				broadcast(&cserv, &cserv.num_lobby, &resp, 0);
				vector_clear(&resp);	

				vector_pushcpy(&resp, &(char){(char)mp_game_made});

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
				vector_iterator p_iter = vector_iterate(&mg->g.players);
				player_t* p;
				while (vector_next(&p_iter)) {
				  p = p_iter.x;
				  if (p->joined || p->mate) continue;
				  break;
				}

				if (p_iter.i-1==mg->g.players.length) {
					vector_pushcpy(&resp, &(char){(char)mp_game_full});
					break;
				}

				p->joined = 1;
				if (strlen(name)>0) p->name = name;

				vector_pushcpy(&resp, &(char){(char)mp_game_joined});
				vector_pushcpy(&resp, &(char){(char)(p_iter.i-1)});
				write_str(&resp, p->name);

				broadcast(&cserv, &mg->player_num, &resp, 0);

				vector_setcpy(&mg->player_num, p_iter.i-1, &i);
				map_insertcpy(&cserv.num_joined, &i, &mg);

				vector_clear(&resp);
				vector_pushcpy(&resp, &(char){(char)mp_game});

				write_players(&resp, &mg->g);
				write_board(&resp, &mg->g);
				write_moves(&resp, &mg->g);
				vector_pushcpy(&resp, &(char){(char)(p_iter.i-1)});

				break;
			}
			case mp_make_move: {
				mp_game_t** mg_ref = map_find(&cserv.num_joined, &i);
				move_t m = read_move(&cur);
				if (!mg_ref || cur.err) break;

				mp_game_t* mg = *mg_ref;
				unsigned player = vector_search(&mg->player_num, &i)-1;

				if (make_move(&mg->g, &m, 1, (char)player) != move_success) break;

				vector_pushcpy(&resp, &(char){(char)mp_move_made});
				write_move(&resp, &m);
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