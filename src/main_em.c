#include <stdio.h>
#include <emscripten.h>

#include "network.h"
#include "chess.h"

#include "imwasm.h"

#define NUM_BOARDS 4
char* boards[NUM_BOARDS*2] = {
#include "../include/default.board"
		,
#include "../include/fourplayer.board"
		,
#include "../include/twovone.board"
		,
#include "../include/capablanca.board"
};

#define DEFAULT_SERVADDR "167.172.222.101"

typedef struct {
	html_ui_t ui;

	char* err;
	char* mp_name;
	chess_client_t client;
	enum {
		menu_main,
		menu_connect,
		menu_chess_sp,
		menu_makegame
	} menustate;

	int menu_customboard;
	int which;
} chess_web_t;

chess_web_t g_web;

typedef enum {
	a_multiplayer,
	a_singleplayer,
	a_netmsg,
	a_connect,
	a_makegame_menu,
	a_makegame,
	a_boardchange,
	a_setmovecursor,
	a_joingame,
	a_select,
	a_back
} action_t;

void handler(void* arg, cur_t cur) {
	chess_web_t* web = arg;

	mp_serv_t msg = chess_client_recvmsg(&web->client, cur);
	printf("recv, %u", web->client.game_list.length);
	html_send(&web->ui, a_netmsg, (void*)msg);
}

void setup_game(chess_web_t* web) {
	web->which = 0;
	web->client.select.from[0] = -1;
}

void update(html_ui_t* ui, html_event_t* ev, chess_web_t* web) {
	web->err=NULL;

	switch (ev->action) {
		case a_netmsg: {
			mp_serv_t msg = *(mp_serv_t*)&ev->custom_data;
			switch (msg) {
				case mp_game_full: {
					web->err = "that game is full, take another gamble";
					break;
				}
				default:;
			}

			break;
		}
		case a_back: {
			switch (web->client.mode) {
				case mode_menu: switch (web->menustate) { //triple-switch
					case menu_makegame: {
						web->client.mode = mode_gamelist; break;
					}
					case menu_connect:
					case menu_chess_sp: {
						web->menustate = menu_main; break;
					}
					default:;
				} break;
				case mode_gamelist: {
					chess_client_disconnect(&web->client);
					if (web->mp_name) drop(web->mp_name);
					web->client.mode = mode_menu;
					web->menustate = menu_main;
					break;
				}
				case mode_singleplayer: {
					chess_client_leavegame(&web->client);

					web->client.mode = mode_menu;
					web->menustate = menu_main;
					break;
				}
				case mode_multiplayer: {
					chess_client_leavegame(&web->client);
					chess_client_gamelist(&web->client);
					web->client.mode = mode_gamelist;
					break;
				}
			}

			break;
		}
		case a_multiplayer: {
			web->menustate = menu_connect; break;
		}
		case a_singleplayer: {
			web->menustate = menu_chess_sp; break;
		}
		case a_connect: {
			char* addr = html_input_value("addr");
			if (strlen(addr)==0) {
				web->err = "address is empty";
				drop(addr);
				return;
			}

			html_local_set("addr", addr);

			web->mp_name = html_input_value("name");
			if (strlen(web->mp_name)!=0) {
				html_local_set("name", web->mp_name);
			} else {
				drop(web->mp_name);
				web->mp_name = NULL;
			}

			web->client.net = client_connect(addr, MP_PORT, handler, web);
			drop(addr);

			if (web->client.net->err) {
				web->err = heapstr("failed to connect; check address. err: %s", strerror(web->client.net->err));
				if (web->mp_name) drop(web->mp_name); break;
			}

			chess_client_gamelist(&web->client);

			web->client.mode = mode_gamelist;
			break;
		}
		case a_makegame_menu: {
			web->client.mode = mode_menu;
			web->menustate = menu_makegame;
			break;
		}
		case a_boardchange: break;
		case a_makegame: {
			char* gname;
			if (web->menustate==menu_makegame) {
				gname = html_input_value("gname");
				if (strlen(gname)==0) {
					web->err="game name is empty"; break;
				}
			}

			char* b_i = html_input_value("boards");
			if (streq(b_i, "custom")) {
				char* b = html_input_value("customboard");
				web->client.g = parse_board(b);
				drop(b);
			} else {
				int i = atoi(b_i);
				web->client.g = parse_board(boards[i*2+1]);
			}

			drop(b_i);

			if (html_checked("pawnpromotion"))
				web->client.g.flags |= game_pawn_promotion;

			chess_client_initgame(&web->client, web->menustate==menu_makegame?mode_multiplayer:mode_singleplayer, 1);
			if (web->menustate==menu_makegame) chess_client_makegame(&web->client, gname, web->mp_name);
			setup_game(web);
			break;
		}
		case a_joingame: {
			if (!chess_client_joingame(&web->client, ev->elem->i, web->mp_name)) {
				web->err = "that game is full, take another gamble lmao";
				break;
			}

			setup_game(web);

			break;
		}
		case a_select: {
			player_t* t = vector_get(&web->client.g.players, web->client.player);

			int select[2];
			select[0] = (int)ev->elem->i;
			select[1] = (int)ev->elem->parent->i;
			board_rot_pos(&web->client.g, t->board_rot, select, web->which==0?web->client.select.from:web->client.select.to);

			if (web->which==1) {
				client_make_move(&web->client);
				web->client.select.from[0] = -1;
				vector_clear(&web->client.hints);
			} else {
				refresh_hints(&web->client);
			}

			web->which=!web->which;
			break;
		}
		case a_setmovecursor: {
			set_move_cursor(&web->client, ev->elem->i);
			break;
		}
	}

	if (web->client.net && web->client.net->err) {
		web->err = heapstr("network error; check address. err: %s", strerror(web->client.net->err));
	}
}

void render(html_ui_t* ui, chess_web_t* web) {
	if (web->err) {
		html_p(ui, "err", web->err);
	}

	if (web->client.mode!=mode_menu || web->menustate!=menu_main) {
		html_elem_t* back = html_button(ui, "back", "back");
		html_event(ui, back, html_click, a_back);
	}

	switch (web->client.mode) {
		case mode_menu: switch (web->menustate) {
			case menu_main: {
				html_p(ui, "p1", "welcome to chess. images by wikimedians Cburnett and spinningspark.");
				html_p(ui, "p2", " also epilepsy warning, some sequences may flash; now consider the following modes:");

				html_start_div(ui, "b", 0);

				html_elem_t* s = html_button(ui, "single", "singleplayer");
				html_event(ui, s, html_click, a_singleplayer);
				html_elem_t* m = html_button(ui, "multi", "multiplayer");
				html_event(ui, m, html_click, a_multiplayer);

				html_end(ui);
				break;
			}
			case menu_connect: {
				html_p(ui, "p1", "server address:");

				char* localaddr = html_local_get("addr");
				if (localaddr==NULL) localaddr=DEFAULT_SERVADDR;
				char* localname = html_local_get("name");

				html_input(ui, "addr", localaddr);

				html_p(ui, "p2", "decide on an alias, or leave blank for default");
				html_input(ui, "name", localname);

				drop(localaddr);
				drop(localname);

				html_elem_t* c = html_button(ui, "connect", "connect");
				html_event(ui, c, html_click, a_connect);
				break;
			}
			case menu_makegame: {
				html_p(ui, "p1", "game name:");
				html_input(ui, "gname", "");
			}
			case menu_chess_sp: {
				html_p(ui, "gtype", "game type:");

				html_elem_t* s = html_start_select(ui, "boards");
				for (int i=0; i<NUM_BOARDS; i++) {
					html_elem_t* e = html_option(ui, NULL, boards[i*2]);
					char* istr = heapstr("%i", i);
					html_set_attr(e, "value", istr);
					drop(istr);
				}

				html_option(ui, NULL, "custom");

				html_end(ui);
				html_event(ui, s, html_onchange, a_boardchange);

				char* b_i = html_input_value("boards");
				if (streq(b_i, "custom")) {
					html_textarea(ui, "customboard", boards[1]);
				}

				drop(b_i);

				html_start_div(ui, "opts", 0);
				html_label(ui, "pawnpromo-label", "pawn promotion permitted?");
				html_checkbox(ui, "pawnpromotion", 1);
				html_end(ui);

				html_elem_t* e = html_button(ui, "make", "make game");
				html_event(ui, e, html_click, a_makegame);
				break;
			}
		} break;
		case mode_gamelist: {
			html_p(ui, "p1", "henceforth, a new match may be made or picked from the following pool");

			html_start_div(ui, "games", 1);
			vector_iterator g_iter = vector_iterate(&web->client.game_list);
			while (vector_next(&g_iter)) {
				char* g = *(char**) g_iter.x;
				html_elem_t* b = html_button(ui, NULL, g);
				html_event(ui, b, html_click, a_joingame);
			}

			html_end(ui);

			html_elem_t* e = html_button(ui, "make", "make game");
			html_event(ui, e, html_click, a_makegame_menu);
			break;
		}
		case mode_multiplayer:
		case mode_singleplayer: {
			player_t* t = vector_get(&web->client.g.players, web->client.player);

			if (web->client.mode == mode_multiplayer) {
				if (web->client.spectating) {
					html_p(ui, "spectating", "(spectating)");
				}

				html_start_div(ui, "spectators", 1);
				vector_iterator spec_iter = vector_iterate(&web->client.g.spectators);
				while (vector_next(&spec_iter)) {
					html_span(ui, NULL, *(char**)spec_iter.x);
					if (spec_iter.i!=web->client.g.spectators.length) {
						html_span(ui, NULL, ", ");
					}
				}

				if (web->client.g.spectators.length==1) {
					html_span(ui, NULL, " is spectating");
				} else if (web->client.g.spectators.length>1) {
					html_span(ui, NULL, " are spectating");
				}

				html_end(ui);
			}


			html_start_div(ui, "wrapper", 1);
			vector_iterator t_iter = vector_iterate(&web->client.g.players);
			while (vector_next(&t_iter)) {
				player_t* p = t_iter.x;
				if (web->client.mode==mode_multiplayer && !p->joined) continue;

				int rel_rot = p->board_rot-t->board_rot;
				if (rel_rot<0) rel_rot=4+rel_rot;
				rel_rot %= 4;

				char* rotclass;
				switch (rel_rot) {
					case 0: rotclass="bottom"; break;
					case 1: rotclass="right"; break;
					case 2: rotclass="up"; break;
					case 3: rotclass="left"; break;
				}

				html_elem_t* d = html_start_div(ui, NULL, 1);
				char* name = p->name;
				if (web->client.g.player==t_iter.i-1)
					name = heapstr(web->client.g.won ? "👑 %s" : "%s's turn", p->name);
				html_p(ui, NULL, name);
				html_end(ui);

				html_set_attr(d, HTML_ATTR_CLASS, "player");
				html_set_attr(d, HTML_ATTR_CLASS, rotclass);
			}

			html_start_div(ui, "moves", 1);
			vector_iterator move_iter = vector_iterate(&web->client.g.moves);
			while (vector_next(&move_iter)) {
				char* pgn = move_pgn(&web->client.g, move_iter.x);
				html_elem_t* p = html_p(ui, NULL, pgn);
				html_event(ui, p, html_click, a_setmovecursor);

				if (move_iter.i-1 == web->client.move_cursor) {
					html_set_attr(p, "style", "font-weight:bold;");
				}

				drop(pgn);
			}

			html_event(ui, html_p(ui, NULL, "now"), html_click, a_setmovecursor);
			html_end(ui);

			html_start_table(ui, "board");
			int pos[2];
			for (pos[1]=0; pos[1]<web->client.g.board_h; pos[1]++) {
				if (pos[1]>0) html_end(ui);
				html_start_tr(ui);

				for (pos[0]=0; pos[0]<web->client.g.board_w; pos[0]++) {
					int bpos[2];
					board_rot_pos(&web->client.g, t->board_rot, pos, bpos);
					piece_t* p = board_get(&web->client.g, bpos);

					html_elem_t* td = html_start_td(ui);

					int col = p->player%4;
					static char* bpieces[] = {"img/king.svg","img/queen.svg","img/rook.svg","img/bishop.svg","img/knight.svg","img/pawn.svg", "img/archibishop.svg", "img/chancellor.svg"};
					static char* wpieces[] = {"img/wking.svg","img/wqueen.svg","img/wrook.svg","img/wbishop.svg","img/wknight.svg","img/wpawn.svg", "img/warchibishop.svg", "img/wchancellor.svg"};
					static char* rpieces[] = {"img/rking.svg","img/rqueen.svg","img/rrook.svg","img/rbishop.svg","img/rknight.svg","img/rpawn.svg", "img/rarchibishop.svg", "img/rchancellor.svg"};
					static char* gpieces[] = {"img/gking.svg","img/gqueen.svg","img/grook.svg","img/gbishop.svg","img/gknight.svg","img/gpawn.svg", "img/garchibishop.svg", "img/gchancellor.svg"};

					switch (p->ty) {
						case p_empty:break;
						case p_blocked:html_img(ui, NULL, "img/blocked.svg");break;
						default: {
							if (col==0) html_img(ui, NULL, wpieces[p->ty]);
							if (col==1) html_img(ui, NULL, bpieces[p->ty]);
							if (col==2) html_img(ui, NULL, rpieces[p->ty]);
							if (col==3) html_img(ui, NULL, gpieces[p->ty]);
						}
					}

					if (p->ty!=p_empty && p->ty!=p_blocked) {
						char* class;
						switch (col) {
							case 0: class="white"; break; case 1: class="black"; break; case 2: class="red"; break; case 3: class="green"; break;
						}

						html_set_attr(td, HTML_ATTR_CLASS, class);
					}

					html_set_attr(td, HTML_ATTR_CLASS, (bpos[0]+bpos[1])%2 ? "dark" : "light");

					if (bpos[0] == web->client.select.from[0] && bpos[1] == web->client.select.from[1]) {
						html_set_attr(td, HTML_ATTR_CLASS, "selected");
					} else if (vector_search(&web->client.hints, &p)!=0) {
						html_set_attr(td, HTML_ATTR_CLASS, "hint");
					}

					html_event(ui, td, html_click, a_select);

					html_end(ui);
				}
			}

			html_end(ui); //tr
			html_end(ui); //table
			html_end(ui); //wrapper

			if (t->check) {
				html_start_div(ui, "flash", 0);
				html_p(ui, "flashtxt", t->mate ? "CHECKMATE!" : "CHECK!");
				html_end(ui);
			}

			break;
		}
	}
}

int main() {
	g_web.ui = html_ui_new();

	g_web.client.mode = mode_menu;
	g_web.menustate = menu_main;
	g_web.menu_customboard=0;

	g_web.err = NULL;
	g_web.client.net = NULL;

	html_run(&g_web.ui, (update_t)update, (render_t)render, &g_web);
}