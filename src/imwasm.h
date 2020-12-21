// Automatically generated header.

#pragma once
#include <emscripten.h>
#include <emscripten/html5.h>
#include "util.h"
#include "vector.h"
#include "hashtable.h"
#include "cyclingindex.h"
#define HTML_STACK_SZ 10
typedef enum {
	html_click,
	html_keypress,
	html_onchange,
	html_custom
} html_event_ty;
typedef struct {
	html_event_ty ty;
	struct html_elem* elem;
	unsigned action;

	union {
		struct html_ui* ui;
		void* custom_data;
	};
} html_event_t;
typedef enum {
	html_used = 1,
	html_list = 2,
	html_list_child = 4
} html_elem_flags_t;
typedef struct html_elem {
	html_elem_flags_t flags;
	union {
		char* id; //shared with elem_id; freed by map
		unsigned list_i;
	};

	unsigned i; //position in parent's children or document body

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

	unsigned elem_i_cur[HTML_STACK_SZ+1];

	map_t elem_id;

	void* arg;
	update_t update;
	render_t render;

	//list element indices are cleared after two cycles
	cycling_index_t cin;
} html_ui_t;
html_ui_t html_ui_new();
int html_send(html_ui_t* ui, unsigned action, void* data);
void html_event(html_ui_t* ui, html_elem_t* elem, html_event_ty ty, unsigned action);
void html_end(html_ui_t* ui);
extern char* HTML_ATTR_CLASS;
void html_set_attr(html_elem_t* elem, char* name, char* val);
html_elem_t* html_start_div(html_ui_t* ui, char* id, int list);
html_elem_t* html_start_select(html_ui_t* ui, char* id);
html_elem_t* html_start_table(html_ui_t* ui, char* id);
html_elem_t* html_start_tr(html_ui_t* ui);
html_elem_t* html_start_td(html_ui_t* ui);
html_elem_t* html_img(html_ui_t* ui, char* id, char* src);
html_elem_t* html_p(html_ui_t* ui, char* id, char* text);
html_elem_t* html_button(html_ui_t* ui, char* id, char* text);
html_elem_t* html_option(html_ui_t* ui, char* id, char* text);
html_elem_t* html_input(html_ui_t* ui, char* id, char* val);
html_elem_t* html_textarea(html_ui_t* ui, char* id, char* val);
char* html_input_value(char* id);
char* html_local_get(char* name);
void html_local_set(char* name, char* val);
void html_run(html_ui_t* ui, update_t update, render_t render, void* arg);