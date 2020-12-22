#include <emscripten.h>
#include <emscripten/html5.h>

#include "util.h"
#include "vector.h"
#include "hashtable.h"
#include "cyclingindex.h"

#define HTML_STACK_SZ 10

typedef char* html_attrib[2];

struct html_elem;
struct html_ui;

typedef enum {
	html_click,
	html_keypress,
	html_onchange,
	html_timeout,
	html_custom
} html_event_ty;

typedef struct {
	html_event_ty ty;
	struct html_ui* ui;
	unsigned action;

	union {
		struct html_elem* elem;
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

html_ui_t html_ui_new() {
	html_ui_t ui = {.elem_id=map_new(), .cur=NULL, .cur_i=0, .cin=cindex_new(2)};
	map_configure_string_key(&ui.elem_id, sizeof(html_elem_t*));

	memset(ui.cur, 0, sizeof(html_elem_t*) * HTML_STACK_SZ);

	MAIN_THREAD_EM_ASM(list_elem=[];); //init globals
	return ui;
}

void html_select_id(char* id) {
	MAIN_THREAD_EM_ASM({
		elem = document.getElementById(UTF8ToString($0));
		if (elem==null) {
			console.log(UTF8ToString($0));
		}
	}, id);
}

void html_select(html_elem_t* elem) {
	if (elem->flags & html_list_child) {
		MAIN_THREAD_EM_ASM({
			elem = list_elem[$0];
		}	, elem->list_i);
	} else {
		html_select_id(elem->id);
	}
}

void html_render(html_ui_t* ui);

void html_send(html_ui_t* ui, unsigned action, void* data) {
	ui->update(ui, &(html_event_t){.ty=html_custom, .ui=ui, .action=action, .custom_data=data}, ui->arg);
	html_render(ui);
}

EMSCRIPTEN_KEEPALIVE
int html_cb(html_event_t* ev) {
	ev->ui->update(ev->ui, ev, ev->ui->arg);
	html_render(ev->ui);

	if (ev->ty==html_timeout) drop(ev);

	return EM_FALSE;
}

void html_settimeout(html_ui_t* ui, unsigned ms, unsigned action, void* data) {
	html_event_t* ev = heapcpy(sizeof(html_event_t), &(html_event_t){.ty=html_timeout, .action=action, .ui=ui, .custom_data=data});

	MAIN_THREAD_EM_ASM({
		window.setTimeout(()=>{
			_html_cb($0);
		}, $1);
	}, ev, ms);
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
		if (ev->ty==ty) {
			if (ev->action!=action) {
				html_register_ev(elem, ev, 1);
			} else {
				return;
			}
		}
	}

	html_event_t* ev = heapcpy(sizeof(html_event_t), &(html_event_t) {.ui=ui, .elem=elem, .ty=ty, .action=action});
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

	if (~elem->flags & html_list_child) map_remove(&ui->elem_id, &elem->id);
	html_elem_free(elem);
}

void html_elem_updateindices(html_elem_t* elem, unsigned from) {
	vector_iterator child_iter = vector_iterate(&elem->parent->children);
	child_iter.i = from;
	while (vector_next(&child_iter)) {
		html_elem_t* e = *(html_elem_t**)child_iter.x;
		e->i = child_iter.i-1;
	}
}

void html_elem_detach(html_elem_t* elem) {
	if (elem->parent) {
		vector_remove(&elem->parent->children, elem->i);
		//update elem indices for reordering on next render or unused deletion
		//i think its faster than doing a search? and i dont have to worry about having a root node
		html_elem_updateindices(elem, elem->i);
	}
}

void html_elem_remove(html_ui_t* ui, html_elem_t* elem) {
	html_elem_detach(elem);
	html_elem_remove_down(ui, elem);
}

void html_insert_elem(html_elem_t* elem) {
	if (elem->parent) {
		html_select(elem->parent);
	} else {
		MAIN_THREAD_EM_ASM(elem=document.body;);
	}

	if (elem->flags & html_list_child) {
		MAIN_THREAD_EM_ASM({
			 list_elem[$1] = elem.appendChild(document.createElement(UTF8ToString($0)));
		}, elem->tag, elem->list_i);
	} else {
		MAIN_THREAD_EM_ASM({
			 elem.appendChild(document.createElement(UTF8ToString($0))).id = UTF8ToString($1);
		}, elem->tag, elem->id);
	}
}

html_elem_t* html_elem_new(html_ui_t* ui, char* tag, char* id, char* txt) {
	char exists;
	html_elem_t** eref;
	html_elem_t* elem;

	html_elem_t* parent = ui->cur[ui->cur_i];
	int list = id==NULL;

	unsigned i = ui->elem_i_cur[parent ? ui->cur_i+1 : 0]++;

	if (list) {
		eref = vector_setget(&parent->children, i, &exists);
	} else {
		map_insert_result res = map_insert(&ui->elem_id, &id);
		exists = res.exists;
		eref = res.val;
	}

	if (exists) elem=*(html_elem_t**)eref;

	if (list && exists) {
		if (~elem->flags & html_list_child) {
			exists=0;
			eref = vector_insert(&parent->children, i);
			html_elem_updateindices(elem, i+1);
		}
	}

	if (exists) {
		if (!streq(elem->tag, tag)) {
			html_select(elem);
			MAIN_THREAD_EM_ASM(elem.remove());

			elem->tag = tag;
			html_insert_elem(elem);
		}

		html_select(elem);
		if (list) {
			elem->list_i = cindex_get(&ui->cin);
			EM_ASM(list_elem[$0]=elem;, elem->list_i);
		} else if (elem->parent != parent || elem->i != i) {
			MAIN_THREAD_EM_ASM((($0 ? document.getElementById(UTF8ToString($0)) : document.body).appendChild(elem)), parent->id);

			html_elem_detach(elem);
			elem->parent = parent;
			elem->i = i;

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

		elem->flags = 0;

		if (list) {
			elem->flags |= html_list_child;
			elem->list_i = cindex_get(&ui->cin);
		} else {
			elem->id = id;
		}

		elem->tag = tag;

		elem->parent = parent;
		elem->i = i;
		if (elem->parent && !list) vector_pushcpy(&elem->parent->children, &elem);

		elem->children = vector_new(sizeof(html_elem_t*));

		elem->attribs = vector_new(sizeof(html_attrib));
		elem->new_attribs = vector_new(sizeof(html_attrib));
		elem->events = vector_new(sizeof(html_event_t*));

		html_insert_elem(elem);
		html_select(elem);
		if (txt) MAIN_THREAD_EM_ASM((elem.innerText = UTF8ToString($0);), txt);
	}

	elem->innertext = txt ? heapcpystr(txt) : NULL;
	elem->flags |= html_used;

	return elem;
}

void html_start(html_ui_t* ui, html_elem_t* elem, int list) {
	unsigned i = ui->cur[ui->cur_i] == NULL ? ui->cur_i : ++ui->cur_i;
	ui->cur[i] = elem;
	ui->elem_i_cur[i+1] = 0;

	if (list) elem->flags |= html_list;
}

void html_end(html_ui_t* ui) {
	ui->cur[ui->cur_i == 0 ? ui->cur_i : ui->cur_i--] = NULL;
}

//is it idiomatic if it makes my code look better act faster and be nicer
//if needed can add more to reduce allocations
char* HTML_ATTR_CLASS = "class";
char* HTML_ATTR_VALUE = "value";

int HTML_ATTR_CONSTS=2;
char* HTML_ATTR_CONST[2] = {"class", "value"};

void html_set_attr(html_elem_t* elem, char* name, char* val) {
	for (int i=0; i<HTML_ATTR_CONSTS; i++) {
		if (HTML_ATTR_CONST[i]==name) {
			vector_pushcpy(&elem->new_attribs, &(char*[2]){name, val ? heapcpystr(val) : NULL});
			return;
		}
	}

	vector_pushcpy(&elem->new_attribs, &(char*[2]){heapcpystr(name), val ? heapcpystr(val) : NULL});
}

html_elem_t* html_start_div(html_ui_t* ui, char* id, int list) {
	html_elem_t* e = html_elem_new(ui, "div", id, NULL);
	html_start(ui, e, list);
	return e;
}

html_elem_t* html_start_select(html_ui_t* ui, char* id) {
	html_elem_t* e = html_elem_new(ui, "select", id, NULL);
	html_start(ui, e, 1);
	return e;
}

html_elem_t* html_start_table(html_ui_t* ui, char* id) {
	html_elem_t* e = html_elem_new(ui, "table", id, NULL);
	html_start(ui, e, 1);
	return e;
}

html_elem_t* html_start_tr(html_ui_t* ui) {
	html_elem_t* e = html_elem_new(ui, "tr", NULL, NULL);
	html_start(ui, e, 1);
	return e;
}

html_elem_t* html_start_td(html_ui_t* ui) {
	html_elem_t* e = html_elem_new(ui, "td", NULL, NULL);
	html_start(ui, e, 1);
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

html_elem_t* html_span(html_ui_t* ui, char* id, char* text) {
	return html_elem_new(ui, "span", id, text);
}

html_elem_t* html_label(html_ui_t* ui, char* id, char* text) {
	return html_elem_new(ui, "label", id, text);
}

html_elem_t* html_button(html_ui_t* ui, char* id, char* text) {
	return html_elem_new(ui, "button", id, text);
}

html_elem_t* html_option(html_ui_t* ui, char* id, char* text) {
	return html_elem_new(ui, "option", id, text);
}

html_elem_t* html_input(html_ui_t* ui, char* id, char* val) {
	html_elem_t* e = html_elem_new(ui, "input", id, NULL);
	if (val) html_set_attr(e, HTML_ATTR_VALUE, val);
	return e;
}

html_elem_t* html_checkbox(html_ui_t* ui, char* id, int checked) {
	html_elem_t* e = html_elem_new(ui, "input", id, NULL);
	html_set_attr(e, "type", "checkbox");
	if (checked) html_set_attr(e, "checked", NULL);
	return e;
}

html_elem_t* html_textarea(html_ui_t* ui, char* id, char* val) {
	html_elem_t* e = html_elem_new(ui, "textarea", id, NULL);
	if (val) html_set_attr(e, HTML_ATTR_VALUE, val);
	return e;
}

char* html_input_value(char* id) {
	html_select_id(id);
	return (char*)MAIN_THREAD_EM_ASM_INT({
		let v = elem.value;
		let len = lengthBytesUTF8(v)+1;
		let buf = _malloc(len);
		stringToUTF8(v, buf, len);
		return buf;
	});
}

int html_checked(char* id) {
	html_select_id(id);
	return MAIN_THREAD_EM_ASM_INT({return elem.checked;});
}

char* html_local_get(char* name) {
	return (char*)MAIN_THREAD_EM_ASM_INT({
		let v = window.localStorage.getItem(UTF8ToString($0));
		if (v==null) return 0;

		let len = lengthBytesUTF8(v)+1;
		let buf = _malloc(len);
		stringToUTF8(v, buf, len);
		return buf;
	}, name);
}

void html_local_set(char* name, char* val) {
	MAIN_THREAD_EM_ASM((window.localStorage.setItem(UTF8ToString($0), UTF8ToString($1));), name, val);
}

void html_elem_update(html_ui_t* ui, html_elem_t* elem) {
	if (~elem->flags & html_used) {
		html_elem_remove(ui, elem);
		return;
	}

	elem->flags ^= html_used;

	if (elem->flags & html_list) {
		vector_iterator child_iter = vector_iterate(&elem->children);
		while (vector_next(&child_iter)) {
			html_elem_t* e = *(html_elem_t**)child_iter.x;
			if (e->flags&html_list_child) html_elem_update(ui, e);
		}

		elem->flags ^= html_list;
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
			//...
			if ((new_attr[0]==attr[0] || streq(new_attr[0], attr[0]))
					&& (new_attr[1] == attr[1] || (new_attr[1] && attr[1] && streq(new_attr[1], attr[1])))) {
				old[new_attrib_iter.i - 1] = 1;
				del = 0;
			}
		}

		if (del) {
			if (attr[0] == HTML_ATTR_CLASS) {
				MAIN_THREAD_EM_ASM((elem.classList.remove(UTF8ToString($0))), attr[1]);
			} else if (attr[0] == HTML_ATTR_VALUE) {
				MAIN_THREAD_EM_ASM((elem.value="";));
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
			if (new_attr[0]==HTML_ATTR_CLASS) {
				MAIN_THREAD_EM_ASM(elem.classList.add(UTF8ToString($0)), new_attr[1]);
			} else if (new_attr[0]==HTML_ATTR_VALUE) {
				MAIN_THREAD_EM_ASM(elem.value = UTF8ToString($0);, new_attr[1]);
			} else if (new_attr[1]==NULL) {
				MAIN_THREAD_EM_ASM(elem.setAttribute(UTF8ToString($0), ""), new_attr[0]);
			} else {
				MAIN_THREAD_EM_ASM(elem.setAttribute(UTF8ToString($0), UTF8ToString($1));, new_attr[0], new_attr[1]);
			}
		}
	}

	//swap n clear
	vector_t attribs = elem->attribs;
	elem->attribs = elem->new_attribs;
	elem->new_attribs = attribs;
	vector_clear(&elem->new_attribs);
}

void html_render(html_ui_t* ui) {
	ui->render(ui, ui->arg);

	map_iterator iter = map_iterate(&ui->elem_id);
	while (map_next(&iter)) {
		html_elem_t* elem = *(html_elem_t**) iter.x;
		html_elem_update(ui, elem);
	}

	for (html_elem_t** e = ui->cur; *e != NULL; e++) *e = NULL;
	ui->elem_i_cur[0] = 0;

	cindex_cycle(&ui->cin);
}

void html_run(html_ui_t* ui, update_t update, render_t render, void* arg) {
	ui->update = update;
	ui->render = render;
	ui->arg = arg;

	html_render(ui);

	MAIN_THREAD_EM_ASM((Module.noExitRuntime = true;));
}
