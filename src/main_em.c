#include <stdio.h>
#include <emscripten.h>

#include "network.h"
#include "chess.h"

#include "imwasm.h"

#define NUM_BOARDS 3
char* boards[NUM_BOARDS*2] = {
#include "../include/default.board"
		,
#include "../include/fourplayer.board"
		,
#include "../include/twovone.board"
};

html_ui_t g_ui;

typedef struct {
	char* err;
	char* mp_name;
	chess_client_t client;
	enum {
		menu_main,
		menu_connect,
		menu_chess_sp,
		menu_chess_mp
	} menustate;
} chess_web_t;

chess_web_t g_web;

typedef enum {
	a_multiplayer,
	a_singleplayer,
	a_connect
} action_t;

void update(html_ui_t* ui, html_event_t* ev, chess_web_t* web) {
	web->err=NULL;

	switch (ev->action) {
		case a_multiplayer: {
			web->menustate = menu_connect; break;
		}
		case a_singleplayer: {
			web->menustate = menu_chess_sp; break;
		}
		case a_connect: {
			char* addr = html_input_value(ui, "addr");
			if (strlen(addr)==0) {
				web->err = "address is empty";
				drop(addr);
				return;
			}

			html_local_set(ui, "addr", addr);

			web->mp_name = html_input_value(ui, "name");
			if (strlen(web->mp_name)==0) {
				web->err = "name is empty";
				drop(addr); drop(web->mp_name);
				return;
			}

			html_local_set(ui, "name", web->mp_name);

			web->client.net = client_connect(addr, MP_PORT);
			drop(addr);

			if (web->client.net.err) {
				web->err = heapstr("failed to connect; check address. err: %i", web->client.net.err);
				drop(web->mp_name); return;
			}

			chess_client_startrecv(&web->client);
			chess_client_gamelist(&web->client);

			web->client.mode = mode_gamelist;
			break;
		}
	}
}

void render(html_ui_t* ui, chess_web_t* web) {
	if (web->err) {
		html_p(ui, "err", web->err);
	}

	switch (web->client.mode) {
		case mode_menu: {
			if (web->menustate==menu_main) {
				html_p(ui, "p1", "choose a mode:");

				html_elem_t* e = html_elem_new(ui, "div", "p2", NULL);
				html_start_div(ui, "b");

				html_elem_t* s = html_button(ui, "single", "singleplayer");
				html_event(ui, s, html_click, a_singleplayer);
				html_elem_t* m = html_button(ui, "multi", "multiplayer");
				html_event(ui, m, html_click, a_multiplayer);

				html_end(ui);
			} else if (web->menustate==menu_connect) {
				html_p(ui, "p1", "server address:");
				html_input(ui, "addr", html_local_get(ui, "addr"));
				html_input(ui, "name", html_local_get(ui, "name"));

				html_elem_t* c = html_button(ui, "connect", "connect");
				html_event(ui, c, html_click, a_connect);
			}

			break;
		}
		case mode_gamelist: {
			html_p(ui, "p1", "henceforth, a new match may be made or picked from the following pool");
			break;
		}
	}
}

int main(int argc, char** argv) {
	mtx_init(&g_web.client.lock, mtx_plain);

	g_ui = html_ui_new(&g_web.client.lock);
	g_web.client.render = (void(*)(void*))html_render;
	g_web.client.arg = &g_ui;

	g_web.client.mode = mode_menu;
	g_web.menustate = menu_main;

	g_web.err = NULL;

	html_run(&g_ui, (update_t)update, (render_t)render, &g_web);
}