#include <emscripten.h>
#include <emscripten/html5.h>

#include "util.h"
#include "vector.h"
#include "hashtable.h"

#define HTML_STACK_SZ 10

typedef char* html_attrib[2];

struct html_elem;
struct html_ui;

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

html_ui_t html_ui_new(mtx_t* mtx) {
	html_ui_t ui = {.elem_id=map_new(), .elem_used=map_new(), .cur=NULL, .cur_i=0, .lock=mtx,
			.prev_list_start=0, .prev_list_idx=0, .prev_list_reused=0};
	map_configure_string_key(&ui.elem_id, sizeof(html_elem_t*));
	map_configure_string_key(&ui.elem_used, 0);

	memset(ui.cur, 0, sizeof(html_elem_t*) * HTML_STACK_SZ);

	MAIN_THREAD_EM_ASM(var elem; var list_elem;); //init globals
	return ui;
}

void html_select_id(char* id) {
	MAIN_THREAD_EM_ASM({
		elem = document.getElementById(UTF8ToString($0));
	}, id);
}

void html_select(html_elem_t* elem) {
	if (elem->list) {
		MAIN_THREAD_EM_ASM({
			elem = list_elem[$0];
		}	, elem->idx);
	} else {
		html_select_id(elem->id);
	}
}

void html_render(html_ui_t* ui);

int html_send(html_ui_t* ui, void* data) {
	mtx_lock(ui->lock);

	ui->update(ui, &(html_event_t){.ty=html_custom, .custom_data=data}, ui->arg);
	html_render(ui);

	mtx_unlock(ui->lock);

	return EM_FALSE;
}

EMSCRIPTEN_KEEPALIVE
int html_cb(html_event_t* ev) {
	mtx_lock(ev->ui->lock);

	ev->ui->update(ev->ui, ev, ev->ui->arg);
	html_render(ev->ui);

	mtx_unlock(ev->ui->lock);

	return EM_FALSE;
}

void html_register_ev(html_elem_t* elem, html_event_t* ev, int rem) {
	char* evstr;
	switch (ev->ty) {
		case html_click: evstr="click"; break;
		case html_keypress: evstr="keypress"; break;
		case html_onchange: evstr="change"; break;
		default: return;
	}

	html_select(elem);
	if (rem) {
		MAIN_THREAD_EM_ASM({
			elem.removeEventListener(UTF8ToString($0), null);
		}, evstr);
	} else {
		MAIN_THREAD_EM_ASM({
			elem.addEventListener(UTF8ToString($0), (ev) => {
				if (_html_cb($1)==1) ev.preventDefault();
			});
		}, evstr, ev);
	}
}

void html_event(html_ui_t* ui, html_elem_t* elem, html_event_ty ty, unsigned action) {
	vector_iterator ev_iter = vector_iterate(&elem->events);
	while (vector_next(&ev_iter)) {
		html_event_t* ev = *(html_event_t**) ev_iter.x;
		if (ev->ty == ty) return;
	}

	html_event_t* ev = heapcpy(sizeof(html_event_t), &(html_event_t) {.ui=ui, .ty=ty, .action=action});
	vector_pushcpy(&elem->events, &ev);
	html_register_ev(elem, ev, 0);
}

void html_elem_free(html_elem_t* elem) {
	vector_iterator attrib_iter = vector_iterate(&elem->attribs);
	while (vector_next(&attrib_iter)) {
		char** attr = attrib_iter.x;
		drop(attr[0]);
		drop(attr[1]);
	}

	vector_iterator ev_iter = vector_iterate(&elem->events);
	while (vector_next(&ev_iter)) {
		html_event_t* ev = *(html_event_t**) ev_iter.x;
		html_register_ev(elem, ev, 1);
		drop(ev);
	}

	html_select(elem);
	MAIN_THREAD_EM_ASM(elem.remove());

	vector_free(&elem->attribs);
	vector_free(&elem->new_attribs);
	vector_free(&elem->children);
	vector_free(&elem->events);
	if (elem->innertext) drop(elem->innertext);

	drop(elem);
}

void html_elem_remove_down(html_ui_t* ui, html_elem_t* elem) {
	vector_iterator child_iter = vector_iterate(&elem->children);
	while (vector_next(&child_iter)) {
		html_elem_t* child = *(html_elem_t**) child_iter.x;
		html_elem_remove_down(ui, child);
	}

	map_remove(&ui->elem_id, &elem->id);
	html_elem_free(elem);
}

void html_elem_remove(html_ui_t* ui, html_elem_t* elem) {
	if (elem->parent) {
		vector_search_remove(&elem->parent->children, &elem);
	}

	html_elem_remove_down(ui, elem);
}

void html_insert_elem(html_elem_t* elem) {
	if (elem->parent) {
		html_select(elem->parent);
	} else {
		MAIN_THREAD_EM_ASM(elem=document.body;);
	}

	if (elem->list) {
		MAIN_THREAD_EM_ASM({
			 elem.appendChild(document.createElement(UTF8ToString($0)));
			 list_elem[$1] = elem;
		}, elem->tag, elem->idx);
	} else {
		MAIN_THREAD_EM_ASM({
			 elem.appendChild(document.createElement(UTF8ToString($0))).id = UTF8ToString($1);
		}, elem->tag, elem->id);
	}

	html_select(elem);
	MAIN_THREAD_EM_ASM((), elem->id);
}

html_elem_t* html_elem_new(html_ui_t* ui, char* tag, char* id, char* txt) {
	char exists;
	html_elem_t** eref;
	html_elem_t* elem;

	html_elem_t* list = ui->cur[ui->cur_i] ? ui->list_cur[ui->cur_i] : NULL;
	if (list) {
		eref = vector_setget(&list->children, ui->list_cur_i[ui->cur_i], &exists);
	} else {
		map_insert_result res = map_insert(&ui->elem_id, &id);
		exists = res.exists;
		eref = res.val;
	}

	if (exists) {
		elem=*(html_elem_t**)eref;

		if (!streq(elem->tag, tag)) {
			html_select(elem);
			MAIN_THREAD_EM_ASM(elem.remove());

			elem->tag = tag;
			html_insert_elem(elem);
		}

		html_select(elem);
		if (elem->parent != ui->cur[ui->cur_i]) {
			MAIN_THREAD_EM_ASM((document.getElementById(UTF8ToString($0)).appendChild(elem)),
												 ui->cur[ui->cur_i]->id);

			if (elem->parent) vector_search_remove(&elem->parent->children, &elem);
			elem->parent = ui->cur[ui->cur_i];
			if (elem->parent) vector_pushcpy(&elem->parent->children, &elem);
		}

		if (txt==NULL) {
			if (elem->innertext!=NULL) MAIN_THREAD_EM_ASM((elem.innerText = "";));
		} else if (!streq(elem->innertext, txt)) {
			MAIN_THREAD_EM_ASM((elem.innerText = UTF8ToString($0);), txt);
		}
	} else {
		elem = heap(sizeof(html_elem_t));
		*(html_elem_t**)eref = elem;

		if (list) {
			elem->idx = ui->prev_list_reused>=ui->prev_list_start ? ui->list_idx++ : ui->prev_list_reused++;
			elem->list=1;
		} else {
			elem->id = id;
			elem->list=0;
		}

		elem->tag = tag;

		elem->parent = ui->cur[ui->cur_i];
		if (elem->parent) vector_pushcpy(&elem->parent->children, &elem);

		elem->children = vector_new(sizeof(html_elem_t*));

		elem->attribs = vector_new(sizeof(html_attrib));
		elem->new_attribs = vector_new(sizeof(html_attrib));
		elem->events = vector_new(sizeof(html_event_t*));

		html_insert_elem(elem);
		html_select(elem);
		if (txt) MAIN_THREAD_EM_ASM((elem.innerText = UTF8ToString($0);), txt);
	}

	elem->innertext = txt;
	elem->used = 1;

	return elem;
}

void html_start(html_ui_t* ui, html_elem_t* elem, int list) {
	unsigned i = ui->cur[ui->cur_i] == NULL ? ui->cur_i : ++ui->cur_i;
	ui->cur[i] = elem;
	ui->list_cur[i] = list ? elem : NULL;
	ui->list_cur_i[i] = 0;
}

void html_end(html_ui_t* ui) {
	ui->cur[ui->cur_i == 0 ? ui->cur_i : ui->cur_i--] = NULL;
}

void html_set_attr(html_elem_t* elem, char* name, char* val) {
	vector_pushcpy(&elem->new_attribs, &(char* [2]) {heapcpystr(name), heapcpystr(val)});
}

void html_class(html_elem_t* elem, char* class) {
	html_set_attr(elem, "class", class);
}

html_elem_t* html_start_div(html_ui_t* ui, char* id) {
	html_elem_t* e = html_elem_new(ui, "div", id, NULL);
	html_start(ui, e, 0);
	return e;
}

html_elem_t* html_img(html_ui_t* ui, char* id, char* src) {
	html_elem_t* e = html_elem_new(ui, "img", id, NULL);
	html_set_attr(e, "src", src);
	return e;
}

html_elem_t* html_p(html_ui_t* ui, char* id, char* text) {
	return html_elem_new(ui, "p", id, text);
}

html_elem_t* html_button(html_ui_t* ui, char* id, char* text) {
	return html_elem_new(ui, "button", id, text);
}

html_elem_t* html_input(html_ui_t* ui, char* id, char* val) {
	html_elem_t* e = html_elem_new(ui, "input", id, NULL);
	if (val) html_set_attr(e, "value", val);
	return e;
}

char* html_input_value(html_ui_t* ui, char* id) {
	html_select_id(id);
	return (char*)MAIN_THREAD_EM_ASM_INT({
		let v = elem.value;
		let len = lengthBytesUTF8(v)+1;
		let buf = _malloc(len);
		stringToUTF8(v, buf, len);
		return buf;
	});
}

char* html_local_get(html_ui_t* ui, char* name) {
	return (char*)MAIN_THREAD_EM_ASM_INT({
		let v = window.localStorage.getItem(UTF8ToString($0));
		if (v==null) return 0;

		let len = lengthBytesUTF8(v)+1;
		let buf = _malloc(len);
		stringToUTF8(v, buf, len);
		return buf;
	}, name);
}

void html_local_set(html_ui_t* ui, char* name, char* val) {
	MAIN_THREAD_EM_ASM((window.localStorage.setItem(UTF8ToString($0), UTF8ToString($1));), name, val);
}

void html_render(html_ui_t* ui) {
	ui->render(ui, ui->arg);

	map_iterator iter = map_iterate(&ui->elem_id);
	while (map_next(&iter)) {
		html_elem_t* elem = *(html_elem_t**) iter.x;
		if (!elem->used) {
			html_elem_remove(ui, elem);
			continue;
		} else {
			elem->used = 0;
		}

		html_select(elem);
		char old[elem->new_attribs.length];
		memset(old, 0, elem->new_attribs.length);

		vector_iterator attrib_iter = vector_iterate(&elem->attribs);
		vector_iterator new_attrib_iter;
		while (vector_next(&attrib_iter)) {
			char** attr = attrib_iter.x;
			char del = 1;

			new_attrib_iter = vector_iterate(&elem->new_attribs);
			while (vector_next(&new_attrib_iter)) {
				char** new_attr = new_attrib_iter.x;
				if (streq(new_attr[0], attr[0]) && streq(new_attr[1], attr[1])) {
					old[new_attrib_iter.i - 1] = 1;
					del = 0;
				}
			}

			if (del) {
				if (streq(attr[0], "class")) {
					MAIN_THREAD_EM_ASM((elem.classList.remove(UTF8ToString($0))), attr[1]);
				} else {
					MAIN_THREAD_EM_ASM((elem.removeAttribute(UTF8ToString($0))), attr[0]);
				}
			}

			drop(attr[0]);
			drop(attr[1]);
		}

		new_attrib_iter = vector_iterate(&elem->new_attribs);
		while (vector_next(&new_attrib_iter)) {
			char** new_attr = new_attrib_iter.x;
			if (!old[new_attrib_iter.i - 1]) {
				if (streq(new_attr[0], "class")) {
					MAIN_THREAD_EM_ASM((elem.classList.add(UTF8ToString($0))), new_attr[1]);
				} else {
					MAIN_THREAD_EM_ASM((elem.setAttribute(UTF8ToString($0), UTF8ToString($1))), new_attr[0], new_attr[1]);
				}
			}
		}

		//swap n clear
		vector_t attribs = elem->attribs;
		elem->attribs = elem->new_attribs;
		elem->new_attribs = attribs;
		vector_clear(&elem->new_attribs);
	}

	for (html_elem_t** e = ui->cur; *e != NULL; e++) *e = NULL;
	ui->prev_list_start = ui->prev_list_start!=0 ? 0 : ui->prev_list_idx;
	ui->prev_list_idx = ui->list_idx;
	ui->prev_list_reused=0;
}

void html_run(html_ui_t* ui, update_t update, render_t render, void* arg) {
	ui->update = update;
	ui->render = render;
	ui->arg = arg;

	mtx_lock(ui->lock);
	html_render(ui);
	mtx_unlock(ui->lock);

	EM_ASM((Module.noExitRuntime = true;));
}
