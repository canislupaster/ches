// Automatically generated header.

#pragma once
#include <emscripten.h>
#include <emscripten/html5.h>
#include "util.h"
#include "vector.h"
#include "hashtable.h"
#define HTML_STACK_SZ 10
typedef enum {
	html_click,
	html_keypress,
	html_onchange,
	html_custom
} html_event_ty;
typedef struct {
	html_event_ty ty;
	unsigned action;

	union {
		struct html_ui* ui;
		void* custom_data;
	};
} html_event_t;
typedef struct html_elem {
	char list; //unnamed
	union {
		char* id; //shared with elem_id; freed by map
		unsigned idx;
	};

	char used; //still in business

	struct html_elem* parent;
	vector_t children;

	vector_t attribs;
	vector_t new_attribs;

	vector_t events;

	char* tag; //not freed nor updated
	char* innertext;
} html_elem_t;
typedef void (*update_t)(struct html_ui*, html_event_t*, void*);
typedef void (*render_t)(struct html_ui*, void*);
typedef struct html_ui {
	int cur_i;
	html_elem_t* cur[HTML_STACK_SZ]; //new elements added as children

	unsigned list_cur_i[HTML_STACK_SZ]; //insertion index, modify for efficiency under insertion and deletion
	html_elem_t* list_cur[HTML_STACK_SZ]; //new elements added unidentified

	map_t elem_id;
	map_t elem_used;

	void* arg;
	update_t update;
	render_t render;

	mtx_t* lock;

	//list element indices are cleared after two cycles
	unsigned prev_list_start, prev_list_idx, list_idx;
	unsigned prev_list_reused;
} html_ui_t;
html_ui_t html_ui_new(mtx_t* mtx);
void html_event(html_ui_t* ui, html_elem_t* elem, html_event_ty ty, unsigned action);
html_elem_t* html_elem_new(html_ui_t* ui, char* tag, char* id, char* txt);
void html_end(html_ui_t* ui);
html_elem_t* html_start_div(html_ui_t* ui, char* id);
html_elem_t* html_p(html_ui_t* ui, char* id, char* text);
html_elem_t* html_button(html_ui_t* ui, char* id, char* text);
html_elem_t* html_input(html_ui_t* ui, char* id, char* val);
char* html_input_value(html_ui_t* ui, char* id);
char* html_local_get(html_ui_t* ui, char* name);
void html_local_set(html_ui_t* ui, char* name, char* val);
void html_render(html_ui_t* ui);
void html_run(html_ui_t* ui, update_t update, render_t render, void* arg);
