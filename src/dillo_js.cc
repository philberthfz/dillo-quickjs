/*
 * File: dillo_js.cc
 *
 * QuickJS Embedded JavaScript Engine Integration for Dillo.
 *
 * Implements:
 * - Runtime & Context lifecycle
 * - Window BOM: alert(), confirm(), prompt(), setTimeout(), setInterval(),
 *   clearTimeout(), clearInterval()
 * - Navigator BOM: userAgent, appName, appVersion, platform
 * - Location BOM: href (getter/setter), reload(), replace(), assign(), origin, protocol, host, hostname, port, pathname, search, hash
 * - Document DOM (Level 0 / 1 / 2 subset):
 *   - title (getter/setter), URL, location, documentElement, head, body
 *   - forms, links, images, scripts collections
 *   - getElementById(), getElementsByTagName(), getElementsByClassName(),
 *     querySelector(), querySelectorAll(), createElement()
 * - Element DOM & Attributes:
 *   - tagName, id, className, classList (add, remove, toggle, contains, replace)
 *   - style (CSSStyleDeclaration with camelCase / kebab-case getters/setters)
 *   - getAttribute(), setAttribute(), removeAttribute(), hasAttribute(), toggleAttribute()
 *   - innerHTML, textContent, innerText
 *   - parentElement, parentNode, children, childNodes, firstElementChild, lastElementChild,
 *     nextElementSibling, previousElementSibling
 *   - click()
 * - EventTarget & Event Subsystem:
 *   - Event, CustomEvent, MouseEvent, SubmitEvent classes
 *   - addEventListener(), removeEventListener(), dispatchEvent() on Window, Document, and Element
 *   - Event propagation: Capture, Target, and Bubbling phases
 *   - preventDefault() and stopPropagation()
 *   - Inline HTML event attributes: onclick, onsubmit, onchange, oninput, onload,
 *     onunload, onreset, onkeydown, onkeyup, onmouseover, onmouseout, etc.
 * - Form & Input Bindings:
 *   - form.submit(), form.reset(), form.elements, form.length, form.action, form.method
 *   - input.value, input.name, input.type, input.checked, input.form
 *   - anchor.href
 * - Console: log(), info(), warn(), error()
 */

#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || defined(__OpenBSD__)
#undef _POSIX_C_SOURCE
#ifndef __BSD_VISIBLE
#define __BSD_VISIBLE 1
#endif
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "dillo_js.hh"

#ifdef ENABLE_JS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#if __has_include(<quickjs/quickjs.h>)
#include <quickjs/quickjs.h>
#elif __has_include(<quickjs.h>)
#include <quickjs.h>
#else
#include <quickjs/quickjs.h>
#endif
}
#include <FL/Fl.H>
#include <pthread.h>
#include <curl/curl.h>

#include "dialog.hh"
#include "uicmd.hh"
#include "html.hh"
#include "form.hh"
#include "prefs.h"
#include "cookies.h"
#include "msg.h"
#include "dlib/dlib.h"
#include "IO/Url.h"

/* -------------------------------------------------------------------------
 * Internal Structures & Globals
 * ------------------------------------------------------------------------- */

static JSRuntime *g_js_runtime = NULL;

static JSClassID js_element_class_id = 0;
static JSClassID js_classlist_class_id = 0;
static JSClassID js_style_class_id = 0;
static JSClassID js_event_class_id = 0;
static JSClassID js_xhr_class_id = 0;
static JSClassID js_response_class_id = 0;
static JSClassID js_storage_class_id = 0;

static std::string to_kebab_case(const std::string &str)
{
   std::string res;
   for (size_t i = 0; i < str.size(); i++) {
      char c = str[i];
      if (isupper((unsigned char)c)) {
         res += '-';
         res += tolower((unsigned char)c);
      } else {
         res += c;
      }
   }
   return res;
}

struct DilloEventListener {
   std::string type;
   JSValue callback;
   bool capture;
   bool once;
};

struct DilloDOMElement {
   int id_num;
   std::string tag_name; /* Uppercase: "FORM", "INPUT", "A", "DIV", etc. */
   std::string id;
   std::string class_name;
   std::vector<std::pair<std::string, std::string> > attributes;
   std::map<std::string, std::string> inline_styles;
   std::vector<DilloEventListener> listeners;
   std::map<std::string, JSValue> on_handlers; /* "onclick", "onsubmit", etc. */

   DilloDOMElement *parent;
   std::vector<DilloDOMElement*> children;

   void *dillo_html;     /* DilloHtml* */
   void *dillo_form;     /* DilloHtmlForm* */
   void *dillo_input;    /* DilloHtmlInput* */
   int link_idx;         /* >= 0 if <a> */
   int img_idx;          /* >= 0 if <img> */

   std::string text_content;
   JSValue js_obj;       /* Cached JS object wrapper */

   DilloDOMElement()
      : id_num(0), parent(NULL), dillo_html(NULL), dillo_form(NULL),
        dillo_input(NULL), link_idx(-1), img_idx(-1), js_obj(JS_UNDEFINED) {}

   const char *get_attribute(const std::string &name) const {
      for (size_t i = 0; i < attributes.size(); i++) {
         if (strcasecmp(attributes[i].first.c_str(), name.c_str()) == 0) {
            return attributes[i].second.c_str();
         }
      }
      return NULL;
   }

   void parse_style_string(const std::string &style_str) {
      inline_styles.clear();
      size_t pos = 0;
      while (pos < style_str.size()) {
         size_t semi = style_str.find(';', pos);
         std::string decl = (semi == std::string::npos) ? style_str.substr(pos) : style_str.substr(pos, semi - pos);
         size_t colon = decl.find(':');
         if (colon != std::string::npos) {
            std::string k = decl.substr(0, colon);
            std::string v = decl.substr(colon + 1);
            while (!k.empty() && isspace((unsigned char)k.front())) k.erase(0, 1);
            while (!k.empty() && isspace((unsigned char)k.back())) k.pop_back();
            while (!v.empty() && isspace((unsigned char)v.front())) v.erase(0, 1);
            while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
            if (!k.empty()) inline_styles[to_kebab_case(k)] = v;
         }
         if (semi == std::string::npos) break;
         pos = semi + 1;
      }
   }

   void set_attribute(const std::string &name, const std::string &value) {
      for (size_t i = 0; i < attributes.size(); i++) {
         if (strcasecmp(attributes[i].first.c_str(), name.c_str()) == 0) {
            attributes[i].second = value;
            if (strcasecmp(name.c_str(), "id") == 0) id = value;
            if (strcasecmp(name.c_str(), "class") == 0) class_name = value;
            if (strcasecmp(name.c_str(), "style") == 0) parse_style_string(value);
            return;
         }
      }
      attributes.push_back(std::make_pair(name, value));
      if (strcasecmp(name.c_str(), "id") == 0) id = value;
      if (strcasecmp(name.c_str(), "class") == 0) class_name = value;
      if (strcasecmp(name.c_str(), "style") == 0) parse_style_string(value);
   }

   void remove_attribute(const std::string &name) {
      for (std::vector<std::pair<std::string, std::string> >::iterator it = attributes.begin();
           it != attributes.end(); ++it) {
         if (strcasecmp(it->first.c_str(), name.c_str()) == 0) {
            attributes.erase(it);
            if (strcasecmp(name.c_str(), "id") == 0) id.clear();
            if (strcasecmp(name.c_str(), "class") == 0) class_name.clear();
            return;
         }
      }
   }

   bool has_attribute(const std::string &name) const {
      return get_attribute(name) != NULL;
   }

   bool has_class(const std::string &cls) const {
      if (cls.empty() || class_name.empty()) return false;
      size_t start = 0;
      while (start < class_name.size()) {
         while (start < class_name.size() && isspace((unsigned char)class_name[start])) start++;
         if (start >= class_name.size()) break;
         size_t end = start;
         while (end < class_name.size() && !isspace((unsigned char)class_name[end])) end++;
         if (class_name.substr(start, end - start) == cls) return true;
         start = end;
      }
      return false;
   }

   void add_class(const std::string &cls) {
      if (has_class(cls)) return;
      if (!class_name.empty() && class_name[class_name.size() - 1] != ' ')
         class_name += " ";
      class_name += cls;
      set_attribute("class", class_name);
   }

   void remove_class(const std::string &cls) {
      if (class_name.empty()) return;
      std::vector<std::string> tokens;
      size_t start = 0;
      while (start < class_name.size()) {
         while (start < class_name.size() && isspace((unsigned char)class_name[start])) start++;
         if (start >= class_name.size()) break;
         size_t end = start;
         while (end < class_name.size() && !isspace((unsigned char)class_name[end])) end++;
         std::string tok = class_name.substr(start, end - start);
         if (tok != cls) tokens.push_back(tok);
         start = end;
      }
      std::string new_cls;
      for (size_t i = 0; i < tokens.size(); i++) {
         if (i > 0) new_cls += " ";
         new_cls += tokens[i];
      }
      class_name = new_cls;
      set_attribute("class", class_name);
   }

   bool toggle_class(const std::string &cls) {
      if (has_class(cls)) {
         remove_class(cls);
         return false;
      } else {
         add_class(cls);
         return true;
      }
   }

   std::string get_text_recursive() const {
      std::string txt = text_content;
      for (size_t i = 0; i < children.size(); i++) {
         if (children[i]) txt += children[i]->get_text_recursive();
      }
      return txt;
   }
};

struct DilloJSTimer {
   int id;
   void *ctx_wrapper; /* DilloJSContext* */
   JSValue func;
   double interval_ms;
   bool is_interval;
   bool active;
};

struct DilloXHR {
   void *ctx_wrapper; /* DilloJSContext* */
   int ready_state;   /* 0=UNSENT, 1=OPENED, 2=HEADERS_RECEIVED, 3=LOADING, 4=DONE */
   int status;
   std::string status_text;
   std::string method;
   std::string url;
   bool is_async;
   std::string request_body;
   std::map<std::string, std::string> request_headers;
   std::map<std::string, std::string> response_headers;
   std::string response_text;
   pthread_t worker_thread;
   bool thread_running;
   bool aborted;
   bool completed;
   JSValue js_self;
   JSValue onreadystatechange;
   JSValue onload;
   JSValue onerror;
   JSValue onabort;

   DilloXHR() : ctx_wrapper(NULL), ready_state(0), status(0),
                status_text(""), method("GET"), url(""), is_async(true),
                thread_running(false), aborted(false), completed(false),
                js_self(JS_UNDEFINED), onreadystatechange(JS_UNDEFINED),
                onload(JS_UNDEFINED), onerror(JS_UNDEFINED), onabort(JS_UNDEFINED) {}
};

struct DilloJSContext {
   JSContext *ctx;
   void *html;           /* DilloHtml* */
   std::string page_url;
   std::string custom_title;
   std::vector<DilloJSTimer*> timers;
   int next_timer_id;

   std::vector<DilloXHR*> active_xhrs;

   int next_element_id;
   DilloDOMElement *root_element;
   DilloDOMElement *head_element;
   DilloDOMElement *body_element;
   DilloDOMElement *current_parent;

   std::vector<DilloDOMElement*> all_elements;
   std::map<std::string, DilloDOMElement*> id_map;
   std::map<void*, DilloDOMElement*> form_map;
   std::map<void*, DilloDOMElement*> input_map;
   std::map<int, DilloDOMElement*> link_map;

   std::vector<DilloEventListener> window_listeners;
   std::map<std::string, JSValue> window_on_handlers;
   std::vector<DilloEventListener> document_listeners;
   std::map<std::string, JSValue> document_on_handlers;

   JSValue window_obj;
   JSValue document_obj;

   JSValue js_event_proto;
   JSValue js_element_proto;
   JSValue js_form_proto;
   JSValue js_input_proto;
   JSValue js_anchor_proto;
   JSValue js_image_proto;
   JSValue js_classlist_proto;
   JSValue js_style_proto;
   JSValue js_xhr_proto;
   JSValue js_response_proto;
   JSValue js_storage_proto;
   void *session_storage;
};

/* -------------------------------------------------------------------------
 * Forward Declarations & Helpers
 * ------------------------------------------------------------------------- */

static JSValue js_get_or_create_element_object(JSContext *ctx, DilloDOMElement *elem);
static bool js_dispatch_event_internal(DilloJSContext *dctx, DilloDOMElement *target_elem,
                                       JSValueConst target_val, JSValueConst event_obj);
static JSValue js_create_event_object(JSContext *ctx, const char *type, bool bubbles, bool cancelable);
static bool matches_compound_selector(DilloDOMElement *elem, const std::string &sel);
static void query_selector_all_internal(DilloDOMElement *node, const std::string &selector,
                                        std::vector<DilloDOMElement*> &out);

static void js_define_get_set(JSContext *ctx, JSValueConst obj, const char *prop,
                              JSCFunction *getter, JSCFunction *setter)
{
   JSAtom atom = JS_NewAtom(ctx, prop);
   JSValue g = getter ? JS_NewCFunction(ctx, getter, prop, 0) : JS_UNDEFINED;
   JSValue s = setter ? JS_NewCFunction(ctx, setter, prop, 1) : JS_UNDEFINED;
   JS_DefinePropertyGetSet(ctx, obj, atom, g, s, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
   JS_FreeAtom(ctx, atom);
}

/* -------------------------------------------------------------------------
 * Finalizers
 * ------------------------------------------------------------------------- */

static void js_element_finalizer(JSRuntime *rt, JSValue val)
{
   (void)rt; (void)val;
}

static void js_classlist_finalizer(JSRuntime *rt, JSValue val)
{
   (void)rt; (void)val;
}

static void js_style_finalizer(JSRuntime *rt, JSValue val)
{
   (void)rt; (void)val;
}

static void js_event_finalizer(JSRuntime *rt, JSValue val)
{
   (void)rt; (void)val;
}

/* -------------------------------------------------------------------------
 * Timer Trampolines (FLTK)
 * ------------------------------------------------------------------------- */

static void js_timer_fltk_callback(void *data)
{
   DilloJSTimer *timer = (DilloJSTimer *)data;
   if (!timer || !timer->active) return;

   DilloJSContext *dctx = (DilloJSContext *)timer->ctx_wrapper;
   if (!dctx || !dctx->ctx) return;

   JSContext *ctx = dctx->ctx;
   JSValue global = JS_GetGlobalObject(ctx);
   JSValue ret = JS_Call(ctx, timer->func, global, 0, NULL);
   JS_FreeValue(ctx, global);

   if (JS_IsException(ret)) {
      JSValue exc = JS_GetException(ctx);
      const char *exc_str = JS_ToCString(ctx, exc);
      if (exc_str) {
         MSG_ERR("[Dillo QuickJS Timer Error] %s\n", exc_str);
         JS_FreeCString(ctx, exc_str);
      }
      JS_FreeValue(ctx, exc);
   }
   JS_FreeValue(ctx, ret);

   if (timer->active && timer->is_interval) {
      Fl::add_timeout(timer->interval_ms / 1000.0, js_timer_fltk_callback, timer);
   } else if (!timer->is_interval) {
      timer->active = false;
   }
}

/* -------------------------------------------------------------------------
 * BOM: Window methods (alert, confirm, prompt, setTimeout, etc.)
 * ------------------------------------------------------------------------- */

static JSValue js_window_alert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   const char *msg = "";
   if (argc > 0) msg = JS_ToCString(ctx, argv[0]);
   a_Dialog_msg("JavaScript Alert", msg ? msg : "");
   if (argc > 0 && msg) JS_FreeCString(ctx, msg);
   return JS_UNDEFINED;
}

static JSValue js_window_confirm(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   const char *msg = "";
   if (argc > 0) msg = JS_ToCString(ctx, argv[0]);
   int choice = a_Dialog_choice("JavaScript Confirm", msg ? msg : "", "OK", "Cancel", NULL);
   if (argc > 0 && msg) JS_FreeCString(ctx, msg);
   return JS_NewBool(ctx, choice == 1);
}

static JSValue js_window_prompt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   const char *msg = "";
   if (argc > 0) msg = JS_ToCString(ctx, argv[0]);
   const char *input = a_Dialog_input("JavaScript Prompt", msg ? msg : "");
   if (argc > 0 && msg) JS_FreeCString(ctx, msg);
   if (input) return JS_NewString(ctx, input);
   return JS_NULL;
}

static JSValue js_window_set_timeout(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (!dctx) return JS_NewInt32(ctx, 0);

   double delay_ms = 0.0;
   if (argc > 1) JS_ToFloat64(ctx, &delay_ms, argv[1]);
   if (delay_ms < 1.0) delay_ms = 1.0;

   DilloJSTimer *timer = new DilloJSTimer();
   timer->id = ++dctx->next_timer_id;
   timer->ctx_wrapper = dctx;
   timer->func = JS_DupValue(ctx, argv[0]);
   timer->interval_ms = delay_ms;
   timer->is_interval = false;
   timer->active = true;

   dctx->timers.push_back(timer);
   Fl::add_timeout(delay_ms / 1000.0, js_timer_fltk_callback, timer);

   return JS_NewInt32(ctx, timer->id);
}

static JSValue js_window_set_interval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (!dctx) return JS_NewInt32(ctx, 0);

   double delay_ms = 10.0;
   if (argc > 1) JS_ToFloat64(ctx, &delay_ms, argv[1]);
   if (delay_ms < 10.0) delay_ms = 10.0;

   DilloJSTimer *timer = new DilloJSTimer();
   timer->id = ++dctx->next_timer_id;
   timer->ctx_wrapper = dctx;
   timer->func = JS_DupValue(ctx, argv[0]);
   timer->interval_ms = delay_ms;
   timer->is_interval = true;
   timer->active = true;

   dctx->timers.push_back(timer);
   Fl::add_timeout(delay_ms / 1000.0, js_timer_fltk_callback, timer);

   return JS_NewInt32(ctx, timer->id);
}

static JSValue js_window_clear_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc < 1) return JS_UNDEFINED;
   int32_t timer_id = 0;
   JS_ToInt32(ctx, &timer_id, argv[0]);
   if (timer_id <= 0) return JS_UNDEFINED;

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (!dctx) return JS_UNDEFINED;

   for (size_t i = 0; i < dctx->timers.size(); i++) {
      DilloJSTimer *t = dctx->timers[i];
      if (t && t->id == timer_id) {
         if (t->active) {
            Fl::remove_timeout(js_timer_fltk_callback, t);
            t->active = false;
         }
         break;
      }
   }
   return JS_UNDEFINED;
}

/* -------------------------------------------------------------------------
 * BOM: Console (log, info, warn, error)
 * ------------------------------------------------------------------------- */

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   printf("[Dillo JS console.log]");
   for (int i = 0; i < argc; i++) {
      const char *str = JS_ToCString(ctx, argv[i]);
      if (str) {
         printf(" %s", str);
         JS_FreeCString(ctx, str);
      }
   }
   printf("\n");
   fflush(stdout);
   return JS_UNDEFINED;
}

static JSValue js_console_warn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   fprintf(stderr, "[Dillo JS console.warn]");
   for (int i = 0; i < argc; i++) {
      const char *str = JS_ToCString(ctx, argv[i]);
      if (str) {
         fprintf(stderr, " %s", str);
         JS_FreeCString(ctx, str);
      }
   }
   fprintf(stderr, "\n");
   fflush(stderr);
   return JS_UNDEFINED;
}

static JSValue js_console_error(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   fprintf(stderr, "[Dillo JS console.error]");
   for (int i = 0; i < argc; i++) {
      const char *str = JS_ToCString(ctx, argv[i]);
      if (str) {
         fprintf(stderr, " %s", str);
         JS_FreeCString(ctx, str);
      }
   }
   fprintf(stderr, "\n");
   fflush(stderr);
   return JS_UNDEFINED;
}

/* -------------------------------------------------------------------------
 * BOM: Location (href, reload, assign, replace)
 * ------------------------------------------------------------------------- */

static JSValue js_location_get_href(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (!dctx) return JS_NewString(ctx, "");
   const char *u = a_Html_get_url(dctx->html);
   return JS_NewString(ctx, (u && *u) ? u : dctx->page_url.c_str());
}

static JSValue js_location_set_href(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc < 1) return JS_UNDEFINED;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (!dctx || !dctx->html) return JS_UNDEFINED;

   const char *target = JS_ToCString(ctx, argv[0]);
   if (target) {
      void *bw = a_Html_get_bw(dctx->html);
      if (bw) a_UIcmd_open_urlstr(bw, target);
      JS_FreeCString(ctx, target);
   }
   return JS_UNDEFINED;
}

static JSValue js_location_assign(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc > 0) return js_location_set_href(ctx, this_val, argc, argv);
   return JS_UNDEFINED;
}

static JSValue js_location_replace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc > 0) return js_location_set_href(ctx, this_val, argc, argv);
   return JS_UNDEFINED;
}

static JSValue js_location_reload(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (dctx && dctx->html) {
      void *bw = a_Html_get_bw(dctx->html);
      if (bw) a_UIcmd_reload(bw);
   }
   return JS_UNDEFINED;
}

/* -------------------------------------------------------------------------
 * DOM: Event System (Event, MouseEvent, SubmitEvent, EventTarget)
 * ------------------------------------------------------------------------- */

static JSValue js_event_prevent_default(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   JSValue cancelable = JS_GetPropertyStr(ctx, this_val, "cancelable");
   if (JS_ToBool(ctx, cancelable)) {
      JS_SetPropertyStr(ctx, this_val, "defaultPrevented", JS_NewBool(ctx, 1));
   }
   JS_FreeValue(ctx, cancelable);
   return JS_UNDEFINED;
}

static JSValue js_event_stop_propagation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   JS_SetPropertyStr(ctx, this_val, "cancelBubble", JS_NewBool(ctx, 1));
   return JS_UNDEFINED;
}

static JSValue js_event_stop_immediate_propagation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   JS_SetPropertyStr(ctx, this_val, "cancelBubble", JS_NewBool(ctx, 1));
   JS_SetPropertyStr(ctx, this_val, "_stopImmediatePropagation", JS_NewBool(ctx, 1));
   return JS_UNDEFINED;
}

static JSValue js_event_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_ThrowTypeError(ctx, "Event constructor requires at least 1 argument");
   const char *type = JS_ToCString(ctx, argv[0]);
   if (!type) return JS_EXCEPTION;

   bool bubbles = false;
   bool cancelable = false;
   if (argc > 1 && JS_IsObject(argv[1])) {
      JSValue b = JS_GetPropertyStr(ctx, argv[1], "bubbles");
      if (!JS_IsUndefined(b)) bubbles = JS_ToBool(ctx, b);
      JS_FreeValue(ctx, b);

      JSValue c = JS_GetPropertyStr(ctx, argv[1], "cancelable");
      if (!JS_IsUndefined(c)) cancelable = JS_ToBool(ctx, c);
      JS_FreeValue(ctx, c);
   }

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   JSValue obj = JS_NewObjectProtoClass(ctx, dctx ? dctx->js_event_proto : JS_NULL, js_event_class_id);

   JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, type));
   JS_SetPropertyStr(ctx, obj, "bubbles", JS_NewBool(ctx, bubbles));
   JS_SetPropertyStr(ctx, obj, "cancelable", JS_NewBool(ctx, cancelable));
   JS_SetPropertyStr(ctx, obj, "defaultPrevented", JS_NewBool(ctx, 0));
   JS_SetPropertyStr(ctx, obj, "cancelBubble", JS_NewBool(ctx, 0));
   JS_SetPropertyStr(ctx, obj, "_stopImmediatePropagation", JS_NewBool(ctx, 0));
   JS_SetPropertyStr(ctx, obj, "target", JS_NULL);
   JS_SetPropertyStr(ctx, obj, "currentTarget", JS_NULL);
   JS_SetPropertyStr(ctx, obj, "timeStamp", JS_NewInt64(ctx, 0));

   JS_SetPropertyStr(ctx, obj, "preventDefault", JS_NewCFunction(ctx, js_event_prevent_default, "preventDefault", 0));
   JS_SetPropertyStr(ctx, obj, "stopPropagation", JS_NewCFunction(ctx, js_event_stop_propagation, "stopPropagation", 0));
   JS_SetPropertyStr(ctx, obj, "stopImmediatePropagation", JS_NewCFunction(ctx, js_event_stop_immediate_propagation, "stopImmediatePropagation", 0));

   JS_FreeCString(ctx, type);
   return obj;
}

static JSValue js_create_event_object(JSContext *ctx, const char *type, bool bubbles, bool cancelable)
{
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   JSValue obj = JS_NewObjectProtoClass(ctx, dctx ? dctx->js_event_proto : JS_NULL, js_event_class_id);

   JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, type));
   JS_SetPropertyStr(ctx, obj, "bubbles", JS_NewBool(ctx, bubbles));
   JS_SetPropertyStr(ctx, obj, "cancelable", JS_NewBool(ctx, cancelable));
   JS_SetPropertyStr(ctx, obj, "defaultPrevented", JS_NewBool(ctx, 0));
   JS_SetPropertyStr(ctx, obj, "cancelBubble", JS_NewBool(ctx, 0));
   JS_SetPropertyStr(ctx, obj, "_stopImmediatePropagation", JS_NewBool(ctx, 0));
   JS_SetPropertyStr(ctx, obj, "target", JS_NULL);
   JS_SetPropertyStr(ctx, obj, "currentTarget", JS_NULL);
   JS_SetPropertyStr(ctx, obj, "timeStamp", JS_NewInt64(ctx, 0));

   JS_SetPropertyStr(ctx, obj, "preventDefault", JS_NewCFunction(ctx, js_event_prevent_default, "preventDefault", 0));
   JS_SetPropertyStr(ctx, obj, "stopPropagation", JS_NewCFunction(ctx, js_event_stop_propagation, "stopPropagation", 0));
   JS_SetPropertyStr(ctx, obj, "stopImmediatePropagation", JS_NewCFunction(ctx, js_event_stop_immediate_propagation, "stopImmediatePropagation", 0));

   return obj;
}

/* -------------------------------------------------------------------------
 * EventTarget Implementation (Window, Document, Element)
 * ------------------------------------------------------------------------- */

static JSValue js_event_target_add_event_listener(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
   const char *type_str = JS_ToCString(ctx, argv[0]);
   if (!type_str) return JS_UNDEFINED;

   bool capture = false;
   bool once = false;
   if (argc > 2) {
      if (JS_IsBool(argv[2])) {
         capture = JS_ToBool(ctx, argv[2]);
      } else if (JS_IsObject(argv[2])) {
         JSValue cap = JS_GetPropertyStr(ctx, argv[2], "capture");
         if (!JS_IsUndefined(cap)) capture = JS_ToBool(ctx, cap);
         JS_FreeValue(ctx, cap);

         JSValue on = JS_GetPropertyStr(ctx, argv[2], "once");
         if (!JS_IsUndefined(on)) once = JS_ToBool(ctx, on);
         JS_FreeValue(ctx, on);
      }
   }

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);

   DilloEventListener listener;
   listener.type = type_str;
   listener.callback = JS_DupValue(ctx, argv[1]);
   listener.capture = capture;
   listener.once = once;

   if (elem) {
      elem->listeners.push_back(listener);
   } else if (dctx && JS_VALUE_GET_PTR(this_val) == JS_VALUE_GET_PTR(dctx->document_obj)) {
      dctx->document_listeners.push_back(listener);
   } else if (dctx) {
      dctx->window_listeners.push_back(listener);
   }

   JS_FreeCString(ctx, type_str);
   return JS_UNDEFINED;
}

static JSValue js_event_target_remove_event_listener(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
   const char *type_str = JS_ToCString(ctx, argv[0]);
   if (!type_str) return JS_UNDEFINED;

   bool capture = false;
   if (argc > 2) {
      if (JS_IsBool(argv[2])) capture = JS_ToBool(ctx, argv[2]);
      else if (JS_IsObject(argv[2])) {
         JSValue cap = JS_GetPropertyStr(ctx, argv[2], "capture");
         if (!JS_IsUndefined(cap)) capture = JS_ToBool(ctx, cap);
         JS_FreeValue(ctx, cap);
      }
   }

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);

   std::vector<DilloEventListener> *target_vec = NULL;
   if (elem) target_vec = &elem->listeners;
   else if (dctx && JS_VALUE_GET_PTR(this_val) == JS_VALUE_GET_PTR(dctx->document_obj)) target_vec = &dctx->document_listeners;
   else if (dctx) target_vec = &dctx->window_listeners;

   if (target_vec) {
      for (std::vector<DilloEventListener>::iterator it = target_vec->begin(); it != target_vec->end(); ++it) {
         if (it->type == type_str && it->capture == capture &&
             JS_VALUE_GET_PTR(it->callback) == JS_VALUE_GET_PTR(argv[1])) {
            JS_FreeValue(ctx, it->callback);
            target_vec->erase(it);
            break;
         }
      }
   }

   JS_FreeCString(ctx, type_str);
   return JS_UNDEFINED;
}

static JSValue js_event_target_dispatch_event(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1 || !JS_IsObject(argv[0])) return JS_NewBool(ctx, 0);
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (!dctx) return JS_NewBool(ctx, 0);

   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   bool not_prevented = js_dispatch_event_internal(dctx, elem, this_val, argv[0]);
   return JS_NewBool(ctx, not_prevented);
}

/* Internal dispatcher handling capture, target, and bubbling phases */
static bool js_dispatch_event_internal(DilloJSContext *dctx, DilloDOMElement *target_elem,
                                       JSValueConst target_val, JSValueConst event_obj)
{
   JSContext *ctx = dctx->ctx;
   JS_SetPropertyStr(ctx, event_obj, "target", JS_DupValue(ctx, target_val));

   JSValue type_val = JS_GetPropertyStr(ctx, event_obj, "type");
   const char *type_str = JS_ToCString(ctx, type_val);
   std::string event_type = type_str ? type_str : "";
   JS_FreeCString(ctx, type_str);
   JS_FreeValue(ctx, type_val);

   JSValue bubbles_val = JS_GetPropertyStr(ctx, event_obj, "bubbles");
   bool bubbles = JS_ToBool(ctx, bubbles_val);
   JS_FreeValue(ctx, bubbles_val);

   std::string on_prop_name = "on" + event_type;

   /* Helper to invoke a single listener */
   struct Invoker {
      static void call_listener(JSContext *c, JSValue callback, JSValue cur_target, JSValue ev) {
         JS_SetPropertyStr(c, ev, "currentTarget", JS_DupValue(c, cur_target));
         JSValue ret = JS_Call(c, callback, cur_target, 1, &ev);
         if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(c);
            const char *es = JS_ToCString(c, exc);
            if (es) {
               MSG_ERR("[Dillo QuickJS Event Handler Exception] %s\n", es);
               JS_FreeCString(c, es);
            }
            JS_FreeValue(c, exc);
         }
         JS_FreeValue(c, ret);
      }

      static void call_inline(JSContext *c, JSValue handler, JSValue cur_target, JSValue ev) {
         if (!JS_IsFunction(c, handler)) return;
         JS_SetPropertyStr(c, ev, "currentTarget", JS_DupValue(c, cur_target));
         JSValue ret = JS_Call(c, handler, cur_target, 1, &ev);
         if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(c);
            const char *es = JS_ToCString(c, exc);
            if (es) {
               MSG_ERR("[Dillo QuickJS Inline Handler Exception] %s\n", es);
               JS_FreeCString(c, es);
            }
            JS_FreeValue(c, exc);
         } else if (JS_IsBool(ret) && !JS_ToBool(c, ret)) {
            /* Inline handler returning false cancels event default! */
            JS_SetPropertyStr(c, ev, "defaultPrevented", JS_NewBool(c, 1));
         }
         JS_FreeValue(c, ret);
      }
   };

   /* Build ancestor list: from target parent up to root */
   std::vector<DilloDOMElement*> ancestors;
   for (DilloDOMElement *curr = target_elem ? target_elem->parent : NULL; curr; curr = curr->parent) {
      ancestors.push_back(curr);
   }

   /* Check cancelBubble helper */
   #define IS_CANCELED() ({ \
      JSValue cb_val = JS_GetPropertyStr(ctx, event_obj, "cancelBubble"); \
      bool is_c = JS_ToBool(ctx, cb_val); \
      JS_FreeValue(ctx, cb_val); \
      is_c; \
   })

   /* 1. Capture Phase: window -> document -> root down to target parent */
   for (size_t i = 0; i < dctx->window_listeners.size(); i++) {
      if (dctx->window_listeners[i].capture && dctx->window_listeners[i].type == event_type) {
         Invoker::call_listener(ctx, dctx->window_listeners[i].callback, dctx->window_obj, event_obj);
         if (IS_CANCELED()) goto finish;
      }
   }
   for (size_t i = 0; i < dctx->document_listeners.size(); i++) {
      if (dctx->document_listeners[i].capture && dctx->document_listeners[i].type == event_type) {
         Invoker::call_listener(ctx, dctx->document_listeners[i].callback, dctx->document_obj, event_obj);
         if (IS_CANCELED()) goto finish;
      }
   }
   for (int i = (int)ancestors.size() - 1; i >= 0; i--) {
      DilloDOMElement *anc = ancestors[i];
      JSValue anc_val = js_get_or_create_element_object(ctx, anc);
      for (size_t l = 0; l < anc->listeners.size(); l++) {
         if (anc->listeners[l].capture && anc->listeners[l].type == event_type) {
            Invoker::call_listener(ctx, anc->listeners[l].callback, anc_val, event_obj);
            if (IS_CANCELED()) { JS_FreeValue(ctx, anc_val); goto finish; }
         }
      }
      JS_FreeValue(ctx, anc_val);
   }

   /* 2. Target Phase: on target itself */
   if (target_elem) {
      for (size_t l = 0; l < target_elem->listeners.size(); l++) {
         if (target_elem->listeners[l].type == event_type) {
            Invoker::call_listener(ctx, target_elem->listeners[l].callback, target_val, event_obj);
            if (IS_CANCELED()) goto finish;
         }
      }
      if (target_elem->on_handlers.count(on_prop_name)) {
         Invoker::call_inline(ctx, target_elem->on_handlers[on_prop_name], target_val, event_obj);
         if (IS_CANCELED()) goto finish;
      }
   } else if (JS_VALUE_GET_PTR(target_val) == JS_VALUE_GET_PTR(dctx->document_obj)) {
      for (size_t l = 0; l < dctx->document_listeners.size(); l++) {
         if (dctx->document_listeners[l].type == event_type) {
            Invoker::call_listener(ctx, dctx->document_listeners[l].callback, target_val, event_obj);
            if (IS_CANCELED()) goto finish;
         }
      }
      if (dctx->document_on_handlers.count(on_prop_name)) {
         Invoker::call_inline(ctx, dctx->document_on_handlers[on_prop_name], target_val, event_obj);
      }
   } else {
      for (size_t l = 0; l < dctx->window_listeners.size(); l++) {
         if (dctx->window_listeners[l].type == event_type) {
            Invoker::call_listener(ctx, dctx->window_listeners[l].callback, target_val, event_obj);
            if (IS_CANCELED()) goto finish;
         }
      }
      if (dctx->window_on_handlers.count(on_prop_name)) {
         Invoker::call_inline(ctx, dctx->window_on_handlers[on_prop_name], target_val, event_obj);
      }
   }

   /* 3. Bubbling Phase: walk up ancestors to document and window if bubbles is true */
   if (bubbles) {
      for (size_t i = 0; i < ancestors.size(); i++) {
         DilloDOMElement *anc = ancestors[i];
         JSValue anc_val = js_get_or_create_element_object(ctx, anc);
         for (size_t l = 0; l < anc->listeners.size(); l++) {
            if (!anc->listeners[l].capture && anc->listeners[l].type == event_type) {
               Invoker::call_listener(ctx, anc->listeners[l].callback, anc_val, event_obj);
               if (IS_CANCELED()) { JS_FreeValue(ctx, anc_val); goto finish; }
            }
         }
         if (anc->on_handlers.count(on_prop_name)) {
            Invoker::call_inline(ctx, anc->on_handlers[on_prop_name], anc_val, event_obj);
            if (IS_CANCELED()) { JS_FreeValue(ctx, anc_val); goto finish; }
         }
         JS_FreeValue(ctx, anc_val);
      }

      for (size_t l = 0; l < dctx->document_listeners.size(); l++) {
         if (!dctx->document_listeners[l].capture && dctx->document_listeners[l].type == event_type) {
            Invoker::call_listener(ctx, dctx->document_listeners[l].callback, dctx->document_obj, event_obj);
            if (IS_CANCELED()) goto finish;
         }
      }
      if (dctx->document_on_handlers.count(on_prop_name)) {
         Invoker::call_inline(ctx, dctx->document_on_handlers[on_prop_name], dctx->document_obj, event_obj);
         if (IS_CANCELED()) goto finish;
      }

      for (size_t l = 0; l < dctx->window_listeners.size(); l++) {
         if (!dctx->window_listeners[l].capture && dctx->window_listeners[l].type == event_type) {
            Invoker::call_listener(ctx, dctx->window_listeners[l].callback, dctx->window_obj, event_obj);
            if (IS_CANCELED()) goto finish;
         }
      }
      if (dctx->window_on_handlers.count(on_prop_name)) {
         Invoker::call_inline(ctx, dctx->window_on_handlers[on_prop_name], dctx->window_obj, event_obj);
      }
   }

finish:
   #undef IS_CANCELED
   JSValue dp_val = JS_GetPropertyStr(ctx, event_obj, "defaultPrevented");
   bool def_prevented = JS_ToBool(ctx, dp_val);
   JS_FreeValue(ctx, dp_val);
   return !def_prevented;
}

/* -------------------------------------------------------------------------
 * DOM: CSSStyleDeclaration (element.style)
 * ------------------------------------------------------------------------- */

static JSValue js_style_get_prop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_NewString(ctx, "");
   const char *prop = JS_ToCString(ctx, argv[0]);
   if (!prop) return JS_NewString(ctx, "");

   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_style_class_id);
   std::string val_str;
   if (elem) {
      std::string kebab = to_kebab_case(prop);
      if (elem->inline_styles.count(kebab)) val_str = elem->inline_styles[kebab];
      else if (elem->inline_styles.count(prop)) val_str = elem->inline_styles[prop];
   }
   JS_FreeCString(ctx, prop);
   return JS_NewString(ctx, val_str.c_str());
}

static JSValue js_style_set_prop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 2) return JS_UNDEFINED;
   const char *prop = JS_ToCString(ctx, argv[0]);
   const char *val = JS_ToCString(ctx, argv[1]);
   if (!prop) {
      if (val) JS_FreeCString(ctx, val);
      return JS_UNDEFINED;
   }

   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_style_class_id);
   if (elem) {
      std::string kebab = to_kebab_case(prop);
      elem->inline_styles[kebab] = val ? val : "";
      /* Reconstruct style attribute */
      std::string full_style;
      for (std::map<std::string, std::string>::iterator it = elem->inline_styles.begin();
           it != elem->inline_styles.end(); ++it) {
         if (!it->second.empty()) {
            full_style += it->first + ": " + it->second + "; ";
         }
      }
      elem->set_attribute("style", full_style);
   }

   JS_FreeCString(ctx, prop);
   if (val) JS_FreeCString(ctx, val);
   return JS_UNDEFINED;
}

static JSValue js_style_remove_prop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_NewString(ctx, "");
   const char *prop = JS_ToCString(ctx, argv[0]);
   if (!prop) return JS_NewString(ctx, "");

   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_style_class_id);
   std::string old_val;
   if (elem) {
      std::string kebab = to_kebab_case(prop);
      if (elem->inline_styles.count(kebab)) {
         old_val = elem->inline_styles[kebab];
         elem->inline_styles.erase(kebab);
      }
   }
   JS_FreeCString(ctx, prop);
   return JS_NewString(ctx, old_val.c_str());
}

static JSValue js_style_get_csstext(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_style_class_id);
   if (!elem) return JS_NewString(ctx, "");
   const char *s = elem->get_attribute("style");
   return JS_NewString(ctx, s ? s : "");
}

static JSValue js_style_set_csstext(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_UNDEFINED;
   const char *text = JS_ToCString(ctx, argv[0]);
   if (!text) return JS_UNDEFINED;

   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_style_class_id);
   if (elem) {
      elem->inline_styles.clear();
      elem->set_attribute("style", text);
      /* Parse declarations */
      std::string s(text);
      size_t pos = 0;
      while (pos < s.size()) {
         size_t semi = s.find(';', pos);
         std::string decl = (semi == std::string::npos) ? s.substr(pos) : s.substr(pos, semi - pos);
         size_t colon = decl.find(':');
         if (colon != std::string::npos) {
            std::string k = decl.substr(0, colon);
            std::string v = decl.substr(colon + 1);
            while (!k.empty() && isspace((unsigned char)k.front())) k.erase(0, 1);
            while (!k.empty() && isspace((unsigned char)k.back())) k.pop_back();
            while (!v.empty() && isspace((unsigned char)v.front())) v.erase(0, 1);
            while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
            if (!k.empty()) elem->inline_styles[to_kebab_case(k)] = v;
         }
         if (semi == std::string::npos) break;
         pos = semi + 1;
      }
   }
   JS_FreeCString(ctx, text);
   return JS_UNDEFINED;
}

/* Macro to generate style property getter/setter pairs */
#define DEF_STYLE_PROP(name, css_name) \
   static JSValue js_style_get_##name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { \
      (void)argc; (void)argv; \
      DilloDOMElement *e = (DilloDOMElement*)JS_GetOpaque(this_val, js_style_class_id); \
      if (!e) return JS_NewString(ctx, ""); \
      return JS_NewString(ctx, e->inline_styles.count(css_name) ? e->inline_styles[css_name].c_str() : ""); \
   } \
   static JSValue js_style_set_##name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { \
      if (argc < 1) return JS_UNDEFINED; \
      const char *v = JS_ToCString(ctx, argv[0]); \
      DilloDOMElement *e = (DilloDOMElement*)JS_GetOpaque(this_val, js_style_class_id); \
      if (e) { \
         e->inline_styles[css_name] = v ? v : ""; \
         std::string fs; \
         for (std::map<std::string,std::string>::iterator it=e->inline_styles.begin(); it!=e->inline_styles.end(); ++it) \
            if (!it->second.empty()) fs += it->first + ": " + it->second + "; "; \
         e->set_attribute("style", fs); \
      } \
      if (v) JS_FreeCString(ctx, v); \
      return JS_UNDEFINED; \
   }

DEF_STYLE_PROP(display, "display")
DEF_STYLE_PROP(visibility, "visibility")
DEF_STYLE_PROP(color, "color")
DEF_STYLE_PROP(backgroundColor, "background-color")
DEF_STYLE_PROP(fontSize, "font-size")
DEF_STYLE_PROP(width, "width")
DEF_STYLE_PROP(height, "height")
DEF_STYLE_PROP(margin, "margin")
DEF_STYLE_PROP(padding, "padding")
DEF_STYLE_PROP(border, "border")
DEF_STYLE_PROP(top, "top")
DEF_STYLE_PROP(left, "left")
DEF_STYLE_PROP(zIndex, "z-index")

static JSValue js_create_style_object(JSContext *ctx, DilloDOMElement *elem)
{
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   JSValue obj = JS_NewObjectProtoClass(ctx, dctx ? dctx->js_style_proto : JS_NULL, js_style_class_id);
   JS_SetOpaque(obj, elem);

   JS_SetPropertyStr(ctx, obj, "getPropertyValue", JS_NewCFunction(ctx, js_style_get_prop, "getPropertyValue", 1));
   JS_SetPropertyStr(ctx, obj, "setProperty", JS_NewCFunction(ctx, js_style_set_prop, "setProperty", 2));
   JS_SetPropertyStr(ctx, obj, "removeProperty", JS_NewCFunction(ctx, js_style_remove_prop, "removeProperty", 1));

   js_define_get_set(ctx, obj, "cssText", (JSCFunction*)js_style_get_csstext, (JSCFunction*)js_style_set_csstext);

   js_define_get_set(ctx, obj, "display", (JSCFunction*)js_style_get_display, (JSCFunction*)js_style_set_display);
   js_define_get_set(ctx, obj, "visibility", (JSCFunction*)js_style_get_visibility, (JSCFunction*)js_style_set_visibility);
   js_define_get_set(ctx, obj, "color", (JSCFunction*)js_style_get_color, (JSCFunction*)js_style_set_color);
   js_define_get_set(ctx, obj, "backgroundColor", (JSCFunction*)js_style_get_backgroundColor, (JSCFunction*)js_style_set_backgroundColor);
   js_define_get_set(ctx, obj, "fontSize", (JSCFunction*)js_style_get_fontSize, (JSCFunction*)js_style_set_fontSize);
   js_define_get_set(ctx, obj, "width", (JSCFunction*)js_style_get_width, (JSCFunction*)js_style_set_width);
   js_define_get_set(ctx, obj, "height", (JSCFunction*)js_style_get_height, (JSCFunction*)js_style_set_height);
   js_define_get_set(ctx, obj, "margin", (JSCFunction*)js_style_get_margin, (JSCFunction*)js_style_set_margin);
   js_define_get_set(ctx, obj, "padding", (JSCFunction*)js_style_get_padding, (JSCFunction*)js_style_set_padding);
   js_define_get_set(ctx, obj, "border", (JSCFunction*)js_style_get_border, (JSCFunction*)js_style_set_border);
   js_define_get_set(ctx, obj, "top", (JSCFunction*)js_style_get_top, (JSCFunction*)js_style_set_top);
   js_define_get_set(ctx, obj, "left", (JSCFunction*)js_style_get_left, (JSCFunction*)js_style_set_left);
   js_define_get_set(ctx, obj, "zIndex", (JSCFunction*)js_style_get_zIndex, (JSCFunction*)js_style_set_zIndex);

   return obj;
}

/* -------------------------------------------------------------------------
 * DOM: DOMTokenList (element.classList)
 * ------------------------------------------------------------------------- */

static JSValue js_classlist_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_classlist_class_id);
   if (!elem) return JS_UNDEFINED;
   for (int i = 0; i < argc; i++) {
      const char *c = JS_ToCString(ctx, argv[i]);
      if (c) {
         elem->add_class(c);
         JS_FreeCString(ctx, c);
      }
   }
   return JS_UNDEFINED;
}

static JSValue js_classlist_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_classlist_class_id);
   if (!elem) return JS_UNDEFINED;
   for (int i = 0; i < argc; i++) {
      const char *c = JS_ToCString(ctx, argv[i]);
      if (c) {
         elem->remove_class(c);
         JS_FreeCString(ctx, c);
      }
   }
   return JS_UNDEFINED;
}

static JSValue js_classlist_toggle(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_NewBool(ctx, 0);
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_classlist_class_id);
   if (!elem) return JS_NewBool(ctx, 0);

   const char *c = JS_ToCString(ctx, argv[0]);
   if (!c) return JS_NewBool(ctx, 0);

   bool res = false;
   if (argc > 1) {
      bool force = JS_ToBool(ctx, argv[1]);
      if (force) { elem->add_class(c); res = true; }
      else { elem->remove_class(c); res = false; }
   } else {
      res = elem->toggle_class(c);
   }
   JS_FreeCString(ctx, c);
   return JS_NewBool(ctx, res);
}

static JSValue js_classlist_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_NewBool(ctx, 0);
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_classlist_class_id);
   if (!elem) return JS_NewBool(ctx, 0);

   const char *c = JS_ToCString(ctx, argv[0]);
   bool res = c ? elem->has_class(c) : false;
   if (c) JS_FreeCString(ctx, c);
   return JS_NewBool(ctx, res);
}

static JSValue js_classlist_replace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 2) return JS_NewBool(ctx, 0);
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_classlist_class_id);
   if (!elem) return JS_NewBool(ctx, 0);

   const char *old_c = JS_ToCString(ctx, argv[0]);
   const char *new_c = JS_ToCString(ctx, argv[1]);
   bool found = false;
   if (old_c && new_c && elem->has_class(old_c)) {
      elem->remove_class(old_c);
      elem->add_class(new_c);
      found = true;
   }
   if (old_c) JS_FreeCString(ctx, old_c);
   if (new_c) JS_FreeCString(ctx, new_c);
   return JS_NewBool(ctx, found);
}

static JSValue js_classlist_get_length(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_classlist_class_id);
   if (!elem || elem->class_name.empty()) return JS_NewInt32(ctx, 0);

   int count = 0;
   size_t start = 0;
   while (start < elem->class_name.size()) {
      while (start < elem->class_name.size() && isspace((unsigned char)elem->class_name[start])) start++;
      if (start >= elem->class_name.size()) break;
      count++;
      while (start < elem->class_name.size() && !isspace((unsigned char)elem->class_name[start])) start++;
   }
   return JS_NewInt32(ctx, count);
}

static JSValue js_classlist_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_classlist_class_id);
   return JS_NewString(ctx, elem ? elem->class_name.c_str() : "");
}

static JSValue js_create_classlist_object(JSContext *ctx, DilloDOMElement *elem)
{
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   JSValue obj = JS_NewObjectProtoClass(ctx, dctx ? dctx->js_classlist_proto : JS_NULL, js_classlist_class_id);
   JS_SetOpaque(obj, elem);

   JS_SetPropertyStr(ctx, obj, "add", JS_NewCFunction(ctx, js_classlist_add, "add", 1));
   JS_SetPropertyStr(ctx, obj, "remove", JS_NewCFunction(ctx, js_classlist_remove, "remove", 1));
   JS_SetPropertyStr(ctx, obj, "toggle", JS_NewCFunction(ctx, js_classlist_toggle, "toggle", 1));
   JS_SetPropertyStr(ctx, obj, "contains", JS_NewCFunction(ctx, js_classlist_contains, "contains", 1));
   JS_SetPropertyStr(ctx, obj, "replace", JS_NewCFunction(ctx, js_classlist_replace, "replace", 2));
   JS_SetPropertyStr(ctx, obj, "toString", JS_NewCFunction(ctx, js_classlist_to_string, "toString", 0));

   js_define_get_set(ctx, obj, "length", (JSCFunction*)js_classlist_get_length, NULL);

   return obj;
}

/* -------------------------------------------------------------------------
 * DOM: Element Methods & Properties
 * ------------------------------------------------------------------------- */

static JSValue js_element_get_tag_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   return JS_NewString(ctx, elem ? elem->tag_name.c_str() : "");
}

static JSValue js_element_get_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   return JS_NewString(ctx, elem ? elem->id.c_str() : "");
}

static JSValue js_element_set_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_UNDEFINED;
   const char *new_id = JS_ToCString(ctx, argv[0]);
   if (new_id) {
      DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
      if (elem) {
         elem->id = new_id;
         elem->set_attribute("id", new_id);
         DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
         if (dctx) dctx->id_map[elem->id] = elem;
      }
      JS_FreeCString(ctx, new_id);
   }
   return JS_UNDEFINED;
}

static JSValue js_element_get_class_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   return JS_NewString(ctx, elem ? elem->class_name.c_str() : "");
}

static JSValue js_element_set_class_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_UNDEFINED;
   const char *c = JS_ToCString(ctx, argv[0]);
   if (c) {
      DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
      if (elem) elem->set_attribute("class", c);
      JS_FreeCString(ctx, c);
   }
   return JS_UNDEFINED;
}

static JSValue js_element_get_class_list(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (!elem) return JS_UNDEFINED;
   return js_create_classlist_object(ctx, elem);
}

static JSValue js_element_get_style(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (!elem) return JS_UNDEFINED;
   return js_create_style_object(ctx, elem);
}

static JSValue js_element_get_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_NULL;
   const char *name = JS_ToCString(ctx, argv[0]);
   if (!name) return JS_NULL;

   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   JSValue res = JS_NULL;
   if (elem) {
      const char *val = elem->get_attribute(name);
      if (val) res = JS_NewString(ctx, val);
   }
   JS_FreeCString(ctx, name);
   return res;
}

static JSValue js_element_set_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 2) return JS_UNDEFINED;
   const char *name = JS_ToCString(ctx, argv[0]);
   const char *val = JS_ToCString(ctx, argv[1]);
   if (name && val) {
      DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
      if (elem) elem->set_attribute(name, val);
   }
   if (name) JS_FreeCString(ctx, name);
   if (val) JS_FreeCString(ctx, val);
   return JS_UNDEFINED;
}

static JSValue js_element_remove_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_UNDEFINED;
   const char *name = JS_ToCString(ctx, argv[0]);
   if (name) {
      DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
      if (elem) elem->remove_attribute(name);
      JS_FreeCString(ctx, name);
   }
   return JS_UNDEFINED;
}

static JSValue js_element_has_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_NewBool(ctx, 0);
   const char *name = JS_ToCString(ctx, argv[0]);
   bool res = false;
   if (name) {
      DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
      if (elem) res = elem->has_attribute(name);
      JS_FreeCString(ctx, name);
   }
   return JS_NewBool(ctx, res);
}

static JSValue js_element_toggle_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_NewBool(ctx, 0);
   const char *name = JS_ToCString(ctx, argv[0]);
   bool res = false;
   if (name) {
      DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
      if (elem) {
         if (elem->has_attribute(name)) {
            elem->remove_attribute(name);
            res = false;
         } else {
            elem->set_attribute(name, "");
            res = true;
         }
      }
      JS_FreeCString(ctx, name);
   }
   return JS_NewBool(ctx, res);
}

static JSValue js_element_get_text_content(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (!elem) return JS_NewString(ctx, "");
   std::string t = elem->get_text_recursive();
   while (!t.empty() && isspace((unsigned char)t.front())) t.erase(0, 1);
   while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
   return JS_NewString(ctx, t.c_str());
}

static JSValue js_element_set_text_content(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_UNDEFINED;
   const char *txt = JS_ToCString(ctx, argv[0]);
   if (txt) {
      DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
      if (elem) {
         elem->text_content = txt;
         elem->children.clear();
      }
      JS_FreeCString(ctx, txt);
   }
   return JS_UNDEFINED;
}

static JSValue js_element_get_parent_element(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && elem->parent) return js_get_or_create_element_object(ctx, elem->parent);
   return JS_NULL;
}

static JSValue js_element_get_children(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   JSValue arr = JS_NewArray(ctx);
   if (elem) {
      for (size_t i = 0; i < elem->children.size(); i++) {
         JS_SetPropertyUint32(ctx, arr, i, js_get_or_create_element_object(ctx, elem->children[i]));
      }
   }
   return arr;
}

static JSValue js_element_get_first_element_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && !elem->children.empty()) return js_get_or_create_element_object(ctx, elem->children.front());
   return JS_NULL;
}

static JSValue js_element_get_last_element_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && !elem->children.empty()) return js_get_or_create_element_object(ctx, elem->children.back());
   return JS_NULL;
}

static JSValue js_element_get_next_element_sibling(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && elem->parent) {
      const std::vector<DilloDOMElement*> &siblings = elem->parent->children;
      for (size_t i = 0; i < siblings.size(); i++) {
         if (siblings[i] == elem && i + 1 < siblings.size()) {
            return js_get_or_create_element_object(ctx, siblings[i + 1]);
         }
      }
   }
   return JS_NULL;
}

static JSValue js_element_get_prev_element_sibling(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && elem->parent) {
      const std::vector<DilloDOMElement*> &siblings = elem->parent->children;
      for (size_t i = 0; i < siblings.size(); i++) {
         if (siblings[i] == elem && i > 0) {
            return js_get_or_create_element_object(ctx, siblings[i - 1]);
         }
      }
   }
   return JS_NULL;
}

static JSValue js_element_click(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (dctx && elem) {
      JSValue ev = js_create_event_object(ctx, "click", true, true);
      js_dispatch_event_internal(dctx, elem, this_val, ev);
      JS_FreeValue(ctx, ev);
   }
   return JS_UNDEFINED;
}

static JSValue js_element_query_selector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_element_query_selector_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

/* -------------------------------------------------------------------------
 * DOM: Form & Input Specific Getters & Methods
 * ------------------------------------------------------------------------- */

static JSValue js_form_submit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && elem->dillo_form) {
      a_Html_form_submit2(elem->dillo_form);
   }
   return JS_UNDEFINED;
}

static JSValue js_form_reset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && elem->dillo_form) {
      a_Html_form_reset2(elem->dillo_form);
   }
   return JS_UNDEFINED;
}

static JSValue js_form_get_action(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && elem->dillo_form) return JS_NewString(ctx, a_Html_form_get_action(elem->dillo_form));
   return JS_NewString(ctx, "");
}

static JSValue js_form_get_method(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && elem->dillo_form) return JS_NewString(ctx, a_Html_form_get_method(elem->dillo_form));
   return JS_NewString(ctx, "GET");
}

static JSValue js_form_get_length(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && elem->dillo_form) return JS_NewInt32(ctx, a_Html_form_get_input_count(elem->dillo_form));
   return JS_NewInt32(ctx, 0);
}

static JSValue js_form_get_elements(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   JSValue arr = JS_NewArray(ctx);
   if (elem && elem->dillo_form) {
      int cnt = a_Html_form_get_input_count(elem->dillo_form);
      DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
      for (int i = 0; i < cnt; i++) {
         void *inp = a_Html_form_get_input(elem->dillo_form, i);
         if (inp && dctx && dctx->input_map.count(inp)) {
            JSValue io = js_get_or_create_element_object(ctx, dctx->input_map[inp]);
            JS_SetPropertyUint32(ctx, arr, i, io);
            const char *name = a_Html_input_get_name(inp);
            if (name && name[0]) {
               JS_SetPropertyStr(ctx, arr, name, JS_DupValue(ctx, io));
               JS_SetPropertyStr(ctx, this_val, name, JS_DupValue(ctx, io));
            }
         }
      }
   }
   return arr;
}

static JSValue js_input_get_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && elem->dillo_input) return JS_NewString(ctx, a_Html_input_get_value(elem->dillo_input));
   if (elem) {
      const char *v = elem->get_attribute("value");
      if (v) return JS_NewString(ctx, v);
   }
   return JS_NewString(ctx, "");
}

static JSValue js_input_set_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_UNDEFINED;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   const char *v = JS_ToCString(ctx, argv[0]);
   if (v) {
      if (elem && elem->dillo_input) a_Html_input_set_value(elem->dillo_input, v);
      if (elem) elem->set_attribute("value", v);
      JS_FreeCString(ctx, v);
   }
   return JS_UNDEFINED;
}

static JSValue js_input_get_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && elem->dillo_input) return JS_NewString(ctx, a_Html_input_get_name(elem->dillo_input));
   if (elem) {
      const char *n = elem->get_attribute("name");
      if (n) return JS_NewString(ctx, n);
   }
   return JS_NewString(ctx, "");
}

static JSValue js_input_get_type(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && elem->dillo_input) return JS_NewString(ctx, a_Html_input_get_type(elem->dillo_input));
   if (elem) {
      const char *t = elem->get_attribute("type");
      if (t) return JS_NewString(ctx, t);
   }
   return JS_NewString(ctx, "text");
}

static JSValue js_input_get_checked(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem && elem->dillo_input) return JS_NewBool(ctx, a_Html_input_get_checked(elem->dillo_input));
   if (elem) return JS_NewBool(ctx, elem->has_attribute("checked"));
   return JS_NewBool(ctx, 0);
}

static JSValue js_input_set_checked(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_UNDEFINED;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   int chk = JS_ToBool(ctx, argv[0]);
   if (elem && elem->dillo_input) a_Html_input_set_checked(elem->dillo_input, chk);
   if (elem) {
      if (chk) elem->set_attribute("checked", "checked");
      else elem->remove_attribute("checked");
   }
   return JS_UNDEFINED;
}

static JSValue js_anchor_get_href(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem) {
      const char *h = elem->get_attribute("href");
      if (h) return JS_NewString(ctx, h);
   }
   return JS_NewString(ctx, "");
}

static JSValue js_anchor_set_href(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_UNDEFINED;
   const char *h = JS_ToCString(ctx, argv[0]);
   if (h) {
      DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
      if (elem) elem->set_attribute("href", h);
      JS_FreeCString(ctx, h);
   }
   return JS_UNDEFINED;
}

static JSValue js_image_get_src(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   if (elem) {
      const char *s = elem->get_attribute("src");
      if (s) return JS_NewString(ctx, s);
   }
   return JS_NewString(ctx, "");
}

/* -------------------------------------------------------------------------
 * DOM: CSS Query Selector Engine
 * ------------------------------------------------------------------------- */

static bool matches_single_part(DilloDOMElement *elem, const std::string &part)
{
   if (!elem || part.empty()) return false;
   if (part == "*") return true;

   size_t i = 0;
   if (isalpha((unsigned char)part[0])) {
      size_t tag_end = 0;
      while (tag_end < part.size() && (isalnum((unsigned char)part[tag_end]) || part[tag_end] == '-')) {
         tag_end++;
      }
      std::string tag = part.substr(0, tag_end);
      if (strcasecmp(elem->tag_name.c_str(), tag.c_str()) != 0) return false;
      i = tag_end;
   }

   while (i < part.size()) {
      if (part[i] == '#') {
         size_t id_start = ++i;
         while (i < part.size() && (isalnum((unsigned char)part[i]) || part[i] == '-' || part[i] == '_')) i++;
         std::string id_str = part.substr(id_start, i - id_start);
         if (elem->id != id_str) return false;
      } else if (part[i] == '.') {
         size_t cls_start = ++i;
         while (i < part.size() && (isalnum((unsigned char)part[i]) || part[i] == '-' || part[i] == '_')) i++;
         std::string cls_str = part.substr(cls_start, i - cls_start);
         if (!elem->has_class(cls_str)) return false;
      } else if (part[i] == '[') {
         size_t attr_start = ++i;
         while (i < part.size() && part[i] != '=' && part[i] != ']') i++;
         std::string attr_name = part.substr(attr_start, i - attr_start);
         while (!attr_name.empty() && isspace((unsigned char)attr_name.back())) attr_name.pop_back();
         while (!attr_name.empty() && isspace((unsigned char)attr_name.front())) attr_name.erase(0, 1);

         if (i < part.size() && part[i] == '=') {
            i++;
            while (i < part.size() && isspace((unsigned char)part[i])) i++;
            char quote = 0;
            if (i < part.size() && (part[i] == '"' || part[i] == '\'')) quote = part[i++];
            size_t val_start = i;
            while (i < part.size() && ((quote && part[i] != quote) || (!quote && part[i] != ']'))) i++;
            std::string attr_val = part.substr(val_start, i - val_start);
            if (quote && i < part.size() && part[i] == quote) i++;
            while (i < part.size() && part[i] != ']') i++;
            if (i < part.size() && part[i] == ']') i++;

            const char *actual_val = elem->get_attribute(attr_name);
            if (!actual_val || attr_val != actual_val) return false;
         } else {
            if (i < part.size() && part[i] == ']') i++;
            if (!elem->has_attribute(attr_name)) return false;
         }
      } else {
         i++;
      }
   }
   return true;
}

static bool matches_compound_selector(DilloDOMElement *elem, const std::string &sel)
{
   if (!elem || sel.empty()) return false;

   /* Handle comma-separated list */
   if (sel.find(',') != std::string::npos) {
      size_t pos = 0;
      while (pos < sel.size()) {
         size_t comma = sel.find(',', pos);
         std::string sub = (comma == std::string::npos) ? sel.substr(pos) : sel.substr(pos, comma - pos);
         while (!sub.empty() && isspace((unsigned char)sub.front())) sub.erase(0, 1);
         while (!sub.empty() && isspace((unsigned char)sub.back())) sub.pop_back();
         if (matches_compound_selector(elem, sub)) return true;
         if (comma == std::string::npos) break;
         pos = comma + 1;
      }
      return false;
   }

   /* Parse tokens separated by space or > */
   struct SelToken {
      std::string part;
      char combinator; /* ' ' = descendant, '>' = direct child */
   };
   std::vector<SelToken> tokens;
   size_t idx = 0;
   char next_comb = ' ';

   while (idx < sel.size()) {
      while (idx < sel.size() && isspace((unsigned char)sel[idx])) idx++;
      if (idx >= sel.size()) break;
      if (sel[idx] == '>') {
         next_comb = '>';
         idx++;
         continue;
      }
      size_t tok_start = idx;
      while (idx < sel.size() && !isspace((unsigned char)sel[idx]) && sel[idx] != '>') idx++;
      SelToken tok;
      tok.part = sel.substr(tok_start, idx - tok_start);
      tok.combinator = next_comb;
      tokens.push_back(tok);
      next_comb = ' ';
   }

   if (tokens.empty()) return false;

   /* Check rightmost token against elem */
   if (!matches_single_part(elem, tokens.back().part)) return false;
   if (tokens.size() == 1) return true;

   /* Walk backwards through ancestors */
   DilloDOMElement *curr = elem;
   for (int t = (int)tokens.size() - 2; t >= 0; t--) {
      char comb = tokens[t + 1].combinator;
      if (comb == '>') {
         curr = curr->parent;
         if (!curr || !matches_single_part(curr, tokens[t].part)) return false;
      } else {
         bool found = false;
         for (DilloDOMElement *p = curr->parent; p; p = p->parent) {
            if (matches_single_part(p, tokens[t].part)) {
               curr = p;
               found = true;
               break;
            }
         }
         if (!found) return false;
      }
   }
   return true;
}

static void query_selector_all_internal(DilloDOMElement *node, const std::string &selector,
                                        std::vector<DilloDOMElement*> &out)
{
   if (!node) return;
   for (size_t i = 0; i < node->children.size(); i++) {
      DilloDOMElement *child = node->children[i];
      if (child) {
         if (matches_compound_selector(child, selector)) {
            out.push_back(child);
         }
         query_selector_all_internal(child, selector, out);
      }
   }
}

static JSValue js_element_query_selector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_NULL;
   const char *sel = JS_ToCString(ctx, argv[0]);
   if (!sel) return JS_NULL;

   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   std::vector<DilloDOMElement*> matches;
   if (elem) query_selector_all_internal(elem, sel, matches);
   JS_FreeCString(ctx, sel);

   if (!matches.empty()) return js_get_or_create_element_object(ctx, matches.front());
   return JS_NULL;
}

static JSValue js_element_query_selector_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_NewArray(ctx);
   const char *sel = JS_ToCString(ctx, argv[0]);
   if (!sel) return JS_NewArray(ctx);

   DilloDOMElement *elem = (DilloDOMElement *)JS_GetOpaque(this_val, js_element_class_id);
   std::vector<DilloDOMElement*> matches;
   if (elem) query_selector_all_internal(elem, sel, matches);
   JS_FreeCString(ctx, sel);

   JSValue arr = JS_NewArray(ctx);
   for (size_t i = 0; i < matches.size(); i++) {
      JS_SetPropertyUint32(ctx, arr, i, js_get_or_create_element_object(ctx, matches[i]));
   }
   return arr;
}

/* -------------------------------------------------------------------------
 * DOM: Element Object Factory
 * ------------------------------------------------------------------------- */

static JSValue js_get_or_create_element_object(JSContext *ctx, DilloDOMElement *elem)
{
   if (!elem) return JS_NULL;
   if (!JS_IsUndefined(elem->js_obj) && !JS_IsNull(elem->js_obj)) {
      return JS_DupValue(ctx, elem->js_obj);
   }

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   JSValue proto = dctx ? dctx->js_element_proto : JS_NULL;
   if (dctx) {
      if (elem->tag_name == "FORM") proto = dctx->js_form_proto;
      else if (elem->tag_name == "INPUT" || elem->tag_name == "BUTTON" ||
               elem->tag_name == "SELECT" || elem->tag_name == "TEXTAREA") proto = dctx->js_input_proto;
      else if (elem->tag_name == "A") proto = dctx->js_anchor_proto;
      else if (elem->tag_name == "IMG") proto = dctx->js_image_proto;
   }

   JSValue obj = JS_NewObjectProtoClass(ctx, proto, js_element_class_id);
   JS_SetOpaque(obj, elem);

   /* Element methods */
   JS_SetPropertyStr(ctx, obj, "getAttribute", JS_NewCFunction(ctx, js_element_get_attribute, "getAttribute", 1));
   JS_SetPropertyStr(ctx, obj, "setAttribute", JS_NewCFunction(ctx, js_element_set_attribute, "setAttribute", 2));
   JS_SetPropertyStr(ctx, obj, "removeAttribute", JS_NewCFunction(ctx, js_element_remove_attribute, "removeAttribute", 1));
   JS_SetPropertyStr(ctx, obj, "hasAttribute", JS_NewCFunction(ctx, js_element_has_attribute, "hasAttribute", 1));
   JS_SetPropertyStr(ctx, obj, "toggleAttribute", JS_NewCFunction(ctx, js_element_toggle_attribute, "toggleAttribute", 1));
   JS_SetPropertyStr(ctx, obj, "querySelector", JS_NewCFunction(ctx, js_element_query_selector, "querySelector", 1));
   JS_SetPropertyStr(ctx, obj, "querySelectorAll", JS_NewCFunction(ctx, js_element_query_selector_all, "querySelectorAll", 1));
   JS_SetPropertyStr(ctx, obj, "click", JS_NewCFunction(ctx, js_element_click, "click", 0));

   /* EventTarget methods */
   JS_SetPropertyStr(ctx, obj, "addEventListener", JS_NewCFunction(ctx, js_event_target_add_event_listener, "addEventListener", 2));
   JS_SetPropertyStr(ctx, obj, "removeEventListener", JS_NewCFunction(ctx, js_event_target_remove_event_listener, "removeEventListener", 2));
   JS_SetPropertyStr(ctx, obj, "dispatchEvent", JS_NewCFunction(ctx, js_event_target_dispatch_event, "dispatchEvent", 1));

   /* Element properties */
   js_define_get_set(ctx, obj, "tagName", (JSCFunction*)js_element_get_tag_name, NULL);
   js_define_get_set(ctx, obj, "id", (JSCFunction*)js_element_get_id, (JSCFunction*)js_element_set_id);
   js_define_get_set(ctx, obj, "className", (JSCFunction*)js_element_get_class_name, (JSCFunction*)js_element_set_class_name);
   js_define_get_set(ctx, obj, "classList", (JSCFunction*)js_element_get_class_list, NULL);
   js_define_get_set(ctx, obj, "style", (JSCFunction*)js_element_get_style, NULL);
   js_define_get_set(ctx, obj, "textContent", (JSCFunction*)js_element_get_text_content, (JSCFunction*)js_element_set_text_content);
   js_define_get_set(ctx, obj, "innerText", (JSCFunction*)js_element_get_text_content, (JSCFunction*)js_element_set_text_content);
   js_define_get_set(ctx, obj, "parentElement", (JSCFunction*)js_element_get_parent_element, NULL);
   js_define_get_set(ctx, obj, "parentNode", (JSCFunction*)js_element_get_parent_element, NULL);
   js_define_get_set(ctx, obj, "children", (JSCFunction*)js_element_get_children, NULL);
   js_define_get_set(ctx, obj, "childNodes", (JSCFunction*)js_element_get_children, NULL);
   js_define_get_set(ctx, obj, "firstElementChild", (JSCFunction*)js_element_get_first_element_child, NULL);
   js_define_get_set(ctx, obj, "lastElementChild", (JSCFunction*)js_element_get_last_element_child, NULL);
   js_define_get_set(ctx, obj, "nextElementSibling", (JSCFunction*)js_element_get_next_element_sibling, NULL);
   js_define_get_set(ctx, obj, "previousElementSibling", (JSCFunction*)js_element_get_prev_element_sibling, NULL);

   /* Type-specific additions */
   if (elem->tag_name == "FORM") {
      JS_SetPropertyStr(ctx, obj, "submit", JS_NewCFunction(ctx, js_form_submit, "submit", 0));
      JS_SetPropertyStr(ctx, obj, "reset", JS_NewCFunction(ctx, js_form_reset, "reset", 0));
      js_define_get_set(ctx, obj, "action", (JSCFunction*)js_form_get_action, NULL);
      js_define_get_set(ctx, obj, "method", (JSCFunction*)js_form_get_method, NULL);
      js_define_get_set(ctx, obj, "length", (JSCFunction*)js_form_get_length, NULL);
      js_define_get_set(ctx, obj, "elements", (JSCFunction*)js_form_get_elements, NULL);
   } else if (elem->tag_name == "INPUT" || elem->tag_name == "BUTTON" ||
              elem->tag_name == "SELECT" || elem->tag_name == "TEXTAREA") {
      js_define_get_set(ctx, obj, "value", (JSCFunction*)js_input_get_value, (JSCFunction*)js_input_set_value);
      js_define_get_set(ctx, obj, "name", (JSCFunction*)js_input_get_name, NULL);
      js_define_get_set(ctx, obj, "type", (JSCFunction*)js_input_get_type, NULL);
      js_define_get_set(ctx, obj, "checked", (JSCFunction*)js_input_get_checked, (JSCFunction*)js_input_set_checked);
   } else if (elem->tag_name == "A") {
      js_define_get_set(ctx, obj, "href", (JSCFunction*)js_anchor_get_href, (JSCFunction*)js_anchor_set_href);
   } else if (elem->tag_name == "IMG") {
      js_define_get_set(ctx, obj, "src", (JSCFunction*)js_image_get_src, NULL);
   }

   /* Cache JS wrapper */
   elem->js_obj = JS_DupValue(ctx, obj);
   return obj;
}

/* -------------------------------------------------------------------------
 * DOM: Document Methods & Properties
 * ------------------------------------------------------------------------- */

static JSValue js_document_get_title(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (dctx) {
      if (!dctx->custom_title.empty()) return JS_NewString(ctx, dctx->custom_title.c_str());
      if (dctx->html) return JS_NewString(ctx, a_Html_get_title(dctx->html));
   }
   return JS_NewString(ctx, "");
}

static JSValue js_document_set_title(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc < 1) return JS_UNDEFINED;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (dctx) {
      const char *t = JS_ToCString(ctx, argv[0]);
      if (t) {
         dctx->custom_title = t;
         if (dctx->html) {
            void *bw = a_Html_get_bw(dctx->html);
            if (bw) a_UIcmd_set_page_title((BrowserWindow*)bw, t);
         }
         JS_FreeCString(ctx, t);
      }
   }
   return JS_UNDEFINED;
}

static JSValue js_document_get_forms(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (!dctx || !dctx->html) return JS_NewArray(ctx);

   JSValue forms_arr = JS_NewArray(ctx);
   uint32_t idx = 0;
   for (size_t i = 0; i < dctx->all_elements.size(); i++) {
      DilloDOMElement *elem = dctx->all_elements[i];
      if (elem && elem->tag_name == "FORM") {
         JSValue fo = js_get_or_create_element_object(ctx, elem);
         JS_SetPropertyUint32(ctx, forms_arr, idx++, fo);
      }
   }
   return forms_arr;
}

static JSValue js_document_get_links(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   JSValue arr = JS_NewArray(ctx);
   if (dctx) {
      uint32_t idx = 0;
      for (size_t i = 0; i < dctx->all_elements.size(); i++) {
         DilloDOMElement *e = dctx->all_elements[i];
         if (e && e->tag_name == "A" && e->has_attribute("href")) {
            JS_SetPropertyUint32(ctx, arr, idx++, js_get_or_create_element_object(ctx, e));
         }
      }
   }
   return arr;
}

static JSValue js_document_get_images(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   JSValue arr = JS_NewArray(ctx);
   if (dctx) {
      uint32_t idx = 0;
      for (size_t i = 0; i < dctx->all_elements.size(); i++) {
         DilloDOMElement *e = dctx->all_elements[i];
         if (e && e->tag_name == "IMG") {
            JS_SetPropertyUint32(ctx, arr, idx++, js_get_or_create_element_object(ctx, e));
         }
      }
   }
   return arr;
}

static JSValue js_document_get_scripts(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   JSValue arr = JS_NewArray(ctx);
   if (dctx) {
      uint32_t idx = 0;
      for (size_t i = 0; i < dctx->all_elements.size(); i++) {
         DilloDOMElement *e = dctx->all_elements[i];
         if (e && e->tag_name == "SCRIPT") {
            JS_SetPropertyUint32(ctx, arr, idx++, js_get_or_create_element_object(ctx, e));
         }
      }
   }
   return arr;
}

static JSValue js_document_get_element_by_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc < 1) return JS_NULL;
   const char *id_str = JS_ToCString(ctx, argv[0]);
   if (!id_str) return JS_NULL;

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (!dctx) {
      JS_FreeCString(ctx, id_str);
      return JS_NULL;
   }

   /* 1. Direct id_map lookup */
   if (dctx->id_map.count(id_str)) {
      DilloDOMElement *e = dctx->id_map[id_str];
      JS_FreeCString(ctx, id_str);
      return js_get_or_create_element_object(ctx, e);
   }

   /* 2. Linear scan by id or name */
   for (size_t i = 0; i < dctx->all_elements.size(); i++) {
      DilloDOMElement *e = dctx->all_elements[i];
      if (e && (e->id == id_str || e->get_attribute("name") && strcmp(e->get_attribute("name"), id_str) == 0)) {
         JS_FreeCString(ctx, id_str);
         return js_get_or_create_element_object(ctx, e);
      }
   }

   JS_FreeCString(ctx, id_str);
   return JS_NULL;
}

static JSValue js_document_get_elements_by_tag_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc < 1) return JS_NewArray(ctx);
   const char *tag = JS_ToCString(ctx, argv[0]);
   if (!tag) return JS_NewArray(ctx);

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   JSValue arr = JS_NewArray(ctx);
   if (dctx) {
      uint32_t idx = 0;
      bool all = (strcmp(tag, "*") == 0);
      for (size_t i = 0; i < dctx->all_elements.size(); i++) {
         DilloDOMElement *e = dctx->all_elements[i];
         if (e && (all || strcasecmp(e->tag_name.c_str(), tag) == 0)) {
            JS_SetPropertyUint32(ctx, arr, idx++, js_get_or_create_element_object(ctx, e));
         }
      }
   }
   JS_FreeCString(ctx, tag);
   return arr;
}

static JSValue js_document_get_elements_by_class_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc < 1) return JS_NewArray(ctx);
   const char *cls = JS_ToCString(ctx, argv[0]);
   if (!cls) return JS_NewArray(ctx);

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   JSValue arr = JS_NewArray(ctx);
   if (dctx) {
      uint32_t idx = 0;
      for (size_t i = 0; i < dctx->all_elements.size(); i++) {
         DilloDOMElement *e = dctx->all_elements[i];
         if (e && e->has_class(cls)) {
            JS_SetPropertyUint32(ctx, arr, idx++, js_get_or_create_element_object(ctx, e));
         }
      }
   }
   JS_FreeCString(ctx, cls);
   return arr;
}

static JSValue js_document_query_selector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc < 1) return JS_NULL;
   const char *sel = JS_ToCString(ctx, argv[0]);
   if (!sel) return JS_NULL;

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (!dctx) { JS_FreeCString(ctx, sel); return JS_NULL; }

   /* Check all elements in document order */
   for (size_t i = 0; i < dctx->all_elements.size(); i++) {
      DilloDOMElement *e = dctx->all_elements[i];
      if (e && matches_compound_selector(e, sel)) {
         JS_FreeCString(ctx, sel);
         return js_get_or_create_element_object(ctx, e);
      }
   }

   JS_FreeCString(ctx, sel);
   return JS_NULL;
}

static JSValue js_document_query_selector_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc < 1) return JS_NewArray(ctx);
   const char *sel = JS_ToCString(ctx, argv[0]);
   if (!sel) return JS_NewArray(ctx);

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   JSValue arr = JS_NewArray(ctx);
   if (dctx) {
      uint32_t idx = 0;
      for (size_t i = 0; i < dctx->all_elements.size(); i++) {
         DilloDOMElement *e = dctx->all_elements[i];
         if (e && matches_compound_selector(e, sel)) {
            JS_SetPropertyUint32(ctx, arr, idx++, js_get_or_create_element_object(ctx, e));
         }
      }
   }
   JS_FreeCString(ctx, sel);
   return arr;
}

static JSValue js_document_create_element(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc < 1) return JS_NULL;
   const char *tag = JS_ToCString(ctx, argv[0]);
   if (!tag) return JS_NULL;

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (!dctx) { JS_FreeCString(ctx, tag); return JS_NULL; }

   DilloDOMElement *elem = new DilloDOMElement();
   elem->id_num = ++dctx->next_element_id;
   std::string t(tag);
   for (size_t i = 0; i < t.size(); i++) t[i] = toupper((unsigned char)t[i]);
   elem->tag_name = t;
   elem->dillo_html = dctx->html;

   dctx->all_elements.push_back(elem);
   JS_FreeCString(ctx, tag);

   return js_get_or_create_element_object(ctx, elem);
}

static JSValue js_document_get_body(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (dctx && dctx->body_element) return js_get_or_create_element_object(ctx, dctx->body_element);
   return JS_NULL;
}

static JSValue js_document_get_head(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (dctx && dctx->head_element) return js_get_or_create_element_object(ctx, dctx->head_element);
   return JS_NULL;
}

static JSValue js_document_get_document_element(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (dctx && dctx->root_element) return js_get_or_create_element_object(ctx, dctx->root_element);
   return JS_NULL;
}

/* -------------------------------------------------------------------------
 * Track 1.5: Network Fetching & AJAX (XMLHttpRequest, Response, fetch)
 * ------------------------------------------------------------------------- */

struct CurlWriteContext {
   std::string data;
   std::map<std::string, std::string> headers;
};

static size_t curl_write_data_cb(void *buffer, size_t size, size_t nmemb, void *userp)
{
   CurlWriteContext *ctx = (CurlWriteContext *)userp;
   size_t total = size * nmemb;
   ctx->data.append((char *)buffer, total);
   return total;
}

static size_t curl_header_cb(void *buffer, size_t size, size_t nmemb, void *userp)
{
   CurlWriteContext *ctx = (CurlWriteContext *)userp;
   size_t total = size * nmemb;
   std::string line((char *)buffer, total);
   size_t colon = line.find(':');
   if (colon != std::string::npos) {
      std::string k = line.substr(0, colon);
      std::string v = line.substr(colon + 1);
      while (!k.empty() && (k.back() == '\r' || k.back() == '\n' || isspace((unsigned char)k.back()))) k.pop_back();
      while (!k.empty() && isspace((unsigned char)k.front())) k.erase(0, 1);
      while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || isspace((unsigned char)v.back()))) v.pop_back();
      while (!v.empty() && isspace((unsigned char)v.front())) v.erase(0, 1);
      for (size_t i = 0; i < k.size(); i++) k[i] = tolower((unsigned char)k[i]);
      if (!k.empty()) ctx->headers[k] = v;
   }
   return total;
}

static void js_drain_microtasks(JSContext *ctx)
{
   if (!ctx) return;
   JSRuntime *rt = JS_GetRuntime(ctx);
   if (!rt) return;
   JSContext *pctx = NULL;
   while (JS_IsJobPending(rt)) {
      int err = JS_ExecutePendingJob(rt, &pctx);
      if (err < 0 && pctx) {
         JSValue exc = JS_GetException(pctx);
         const char *str = JS_ToCString(pctx, exc);
         if (str) {
            MSG_ERR("[Dillo QuickJS Microtask Error] %s\n", str);
            JS_FreeCString(pctx, str);
         }
         JS_FreeValue(pctx, exc);
      }
   }
}

static void js_xhr_fltk_callback(void *data);

static void *xhr_worker_thread(void *arg)
{
   DilloXHR *xhr = (DilloXHR *)arg;
   if (!xhr) return NULL;

   CURL *curl = curl_easy_init();
   if (!curl) {
      xhr->completed = true;
      xhr->status = 0;
      xhr->status_text = "Curl Initialization Failed";
      return NULL;
   }

   CurlWriteContext cctx;
   curl_easy_setopt(curl, CURLOPT_URL, xhr->url.c_str());
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cctx);
   curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
   curl_easy_setopt(curl, CURLOPT_HEADERDATA, &cctx);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
   curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

   struct curl_slist *headers_list = NULL;
   for (std::map<std::string, std::string>::iterator it = xhr->request_headers.begin();
        it != xhr->request_headers.end(); ++it) {
      std::string h = it->first + ": " + it->second;
      headers_list = curl_slist_append(headers_list, h.c_str());
   }
   if (headers_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_list);

   if (strcasecmp(xhr->method.c_str(), "POST") == 0) {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      if (!xhr->request_body.empty()) {
         curl_easy_setopt(curl, CURLOPT_POSTFIELDS, xhr->request_body.c_str());
         curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)xhr->request_body.size());
      }
   } else if (strcasecmp(xhr->method.c_str(), "PUT") == 0) {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
      if (!xhr->request_body.empty()) {
         curl_easy_setopt(curl, CURLOPT_POSTFIELDS, xhr->request_body.c_str());
         curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)xhr->request_body.size());
      }
   } else if (strcasecmp(xhr->method.c_str(), "DELETE") == 0) {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
   } else if (strcasecmp(xhr->method.c_str(), "HEAD") == 0) {
      curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
   }

   CURLcode res = curl_easy_perform(curl);
   if (res == CURLE_OK) {
      long http_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
      xhr->status = (int)http_code;
      xhr->status_text = (http_code >= 200 && http_code < 300) ? "OK" : "Error";
      xhr->response_text = cctx.data;
      xhr->response_headers = cctx.headers;
   } else {
      xhr->status = 0;
      xhr->status_text = curl_easy_strerror(res);
   }

   if (headers_list) curl_slist_free_all(headers_list);
   curl_easy_cleanup(curl);

   xhr->completed = true;
   return NULL;
}

static void js_xhr_fltk_callback(void *data)
{
   DilloXHR *xhr = (DilloXHR *)data;
   if (!xhr) return;

   DilloJSContext *dctx = (DilloJSContext *)xhr->ctx_wrapper;
   if (!dctx || !dctx->ctx) return;

   JSContext *ctx = dctx->ctx;

   if (!xhr->completed) {
      Fl::add_timeout(0.016, js_xhr_fltk_callback, xhr);
      return;
   }

   if (xhr->thread_running) {
      pthread_join(xhr->worker_thread, NULL);
      xhr->thread_running = false;
   }

   if (xhr->aborted) {
      xhr->ready_state = 4;
      if (JS_IsFunction(ctx, xhr->onabort)) {
         JSValue ev = js_create_event_object(ctx, "abort", false, false);
         JSValue ret = JS_Call(ctx, xhr->onabort, xhr->js_self, 1, &ev);
         JS_FreeValue(ctx, ev);
         JS_FreeValue(ctx, ret);
      }
      return;
   }

   /* Advance through readyState transitions */
   xhr->ready_state = 2; /* HEADERS_RECEIVED */
   if (JS_IsFunction(ctx, xhr->onreadystatechange)) {
      JSValue ev = js_create_event_object(ctx, "readystatechange", false, false);
      JSValue ret = JS_Call(ctx, xhr->onreadystatechange, xhr->js_self, 1, &ev);
      JS_FreeValue(ctx, ev);
      JS_FreeValue(ctx, ret);
   }

   xhr->ready_state = 3; /* LOADING */
   if (JS_IsFunction(ctx, xhr->onreadystatechange)) {
      JSValue ev = js_create_event_object(ctx, "readystatechange", false, false);
      JSValue ret = JS_Call(ctx, xhr->onreadystatechange, xhr->js_self, 1, &ev);
      JS_FreeValue(ctx, ev);
      JS_FreeValue(ctx, ret);
   }

   xhr->ready_state = 4; /* DONE */
   if (JS_IsFunction(ctx, xhr->onreadystatechange)) {
      JSValue ev = js_create_event_object(ctx, "readystatechange", false, false);
      JSValue ret = JS_Call(ctx, xhr->onreadystatechange, xhr->js_self, 1, &ev);
      JS_FreeValue(ctx, ev);
      JS_FreeValue(ctx, ret);
   }

   if (xhr->status >= 200 && xhr->status < 400) {
      if (JS_IsFunction(ctx, xhr->onload)) {
         JSValue ev = js_create_event_object(ctx, "load", false, false);
         JSValue ret = JS_Call(ctx, xhr->onload, xhr->js_self, 1, &ev);
         JS_FreeValue(ctx, ev);
         JS_FreeValue(ctx, ret);
      }
   } else {
      if (JS_IsFunction(ctx, xhr->onerror)) {
         JSValue ev = js_create_event_object(ctx, "error", false, false);
         JSValue ret = JS_Call(ctx, xhr->onerror, xhr->js_self, 1, &ev);
         JS_FreeValue(ctx, ev);
         JS_FreeValue(ctx, ret);
      }
   }

   js_drain_microtasks(ctx);
}

static void js_xhr_finalizer(JSRuntime *rt, JSValue val)
{
   (void)rt;
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(val, js_xhr_class_id);
   if (xhr) {
      if (xhr->thread_running) {
         pthread_join(xhr->worker_thread, NULL);
         xhr->thread_running = false;
      }
      Fl::remove_timeout(js_xhr_fltk_callback, xhr);
      delete xhr;
   }
}

static JSValue js_xhr_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (!dctx) return JS_EXCEPTION;

   JSValue obj = JS_NewObjectProtoClass(ctx, dctx->js_xhr_proto, js_xhr_class_id);
   if (JS_IsException(obj)) return obj;

   DilloXHR *xhr = new DilloXHR();
   xhr->ctx_wrapper = dctx;
   xhr->js_self = obj;

   JS_SetOpaque(obj, xhr);
   dctx->active_xhrs.push_back(xhr);
   return obj;
}

static JSValue js_xhr_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr) return JS_EXCEPTION;
   if (argc < 2) return JS_EXCEPTION;

   const char *m = JS_ToCString(ctx, argv[0]);
   const char *u = JS_ToCString(ctx, argv[1]);
   if (!m || !u) {
      if (m) JS_FreeCString(ctx, m);
      if (u) JS_FreeCString(ctx, u);
      return JS_EXCEPTION;
   }

   xhr->method = m;
   for (size_t i = 0; i < xhr->method.size(); i++) xhr->method[i] = toupper((unsigned char)xhr->method[i]);

   DilloJSContext *dctx = (DilloJSContext *)xhr->ctx_wrapper;
   std::string base = (dctx && !dctx->page_url.empty()) ? dctx->page_url : "http://localhost/";

   DilloUrl *parsed = a_Url_new(u, base.c_str());
   if (parsed) {
      char *full = a_Url_str(parsed);
      xhr->url = full ? full : u;
      a_Url_free(parsed);
   } else {
      xhr->url = u;
   }

   JS_FreeCString(ctx, m);
   JS_FreeCString(ctx, u);

   xhr->is_async = true;
   if (argc > 2) xhr->is_async = JS_ToBool(ctx, argv[2]);

   xhr->ready_state = 1; /* OPENED */
   xhr->request_headers.clear();
   xhr->response_headers.clear();
   xhr->response_text.clear();
   xhr->status = 0;
   xhr->status_text.clear();
   xhr->aborted = false;
   xhr->completed = false;

   return JS_UNDEFINED;
}

static JSValue js_xhr_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr) return JS_EXCEPTION;

   xhr->request_body.clear();
   if (argc > 0 && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
      const char *b = JS_ToCString(ctx, argv[0]);
      if (b) {
         xhr->request_body = b;
         JS_FreeCString(ctx, b);
      }
   }

   xhr->completed = false;
   xhr->aborted = false;

   if (xhr->is_async) {
      xhr->thread_running = true;
      if (pthread_create(&xhr->worker_thread, NULL, xhr_worker_thread, xhr) != 0) {
         xhr->thread_running = false;
         xhr->completed = true;
         xhr->status = 0;
         xhr->status_text = "Thread Spawn Failed";
      }
      Fl::add_timeout(0.005, js_xhr_fltk_callback, xhr);
   } else {
      /* Synchronous send */
      xhr_worker_thread(xhr);
      xhr->ready_state = 4;
      if (JS_IsFunction(ctx, xhr->onload)) {
         JSValue ev = js_create_event_object(ctx, "load", false, false);
         JSValue ret = JS_Call(ctx, xhr->onload, xhr->js_self, 1, &ev);
         JS_FreeValue(ctx, ev);
         JS_FreeValue(ctx, ret);
      }
      js_drain_microtasks(ctx);
   }

   return JS_UNDEFINED;
}

static JSValue js_xhr_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr) return JS_EXCEPTION;

   xhr->aborted = true;
   Fl::remove_timeout(js_xhr_fltk_callback, xhr);
   if (xhr->thread_running) {
      pthread_join(xhr->worker_thread, NULL);
      xhr->thread_running = false;
   }
   xhr->ready_state = 0;
   return JS_UNDEFINED;
}

static JSValue js_xhr_set_request_header(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr || argc < 2) return JS_UNDEFINED;

   const char *k = JS_ToCString(ctx, argv[0]);
   const char *v = JS_ToCString(ctx, argv[1]);
   if (k && v) {
      xhr->request_headers[k] = v;
   }
   if (k) JS_FreeCString(ctx, k);
   if (v) JS_FreeCString(ctx, v);
   return JS_UNDEFINED;
}

static JSValue js_xhr_get_response_header(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr || argc < 1) return JS_NULL;

   const char *k = JS_ToCString(ctx, argv[0]);
   if (!k) return JS_NULL;

   std::string lk = k;
   JS_FreeCString(ctx, k);
   for (size_t i = 0; i < lk.size(); i++) lk[i] = tolower((unsigned char)lk[i]);

   if (xhr->response_headers.count(lk)) {
      return JS_NewString(ctx, xhr->response_headers[lk].c_str());
   }
   return JS_NULL;
}

static JSValue js_xhr_get_all_response_headers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr) return JS_NewString(ctx, "");

   std::string res;
   for (std::map<std::string, std::string>::iterator it = xhr->response_headers.begin();
        it != xhr->response_headers.end(); ++it) {
      res += it->first + ": " + it->second + "\r\n";
   }
   return JS_NewString(ctx, res.c_str());
}

static JSValue js_xhr_get_ready_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr) return JS_NewInt32(ctx, 0);
   return JS_NewInt32(ctx, xhr->ready_state);
}

static JSValue js_xhr_get_status(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr) return JS_NewInt32(ctx, 0);
   return JS_NewInt32(ctx, xhr->status);
}

static JSValue js_xhr_get_status_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr) return JS_NewString(ctx, "");
   return JS_NewString(ctx, xhr->status_text.c_str());
}

static JSValue js_xhr_get_response_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr) return JS_NewString(ctx, "");
   return JS_NewString(ctx, xhr->response_text.c_str());
}

static JSValue js_xhr_get_onreadystatechange(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr) return JS_UNDEFINED;
   return JS_DupValue(ctx, xhr->onreadystatechange);
}

static JSValue js_xhr_set_onreadystatechange(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr || argc < 1) return JS_UNDEFINED;
   JS_FreeValue(ctx, xhr->onreadystatechange);
   xhr->onreadystatechange = JS_DupValue(ctx, argv[0]);
   return JS_UNDEFINED;
}

static JSValue js_xhr_get_onload(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr) return JS_UNDEFINED;
   return JS_DupValue(ctx, xhr->onload);
}

static JSValue js_xhr_set_onload(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr || argc < 1) return JS_UNDEFINED;
   JS_FreeValue(ctx, xhr->onload);
   xhr->onload = JS_DupValue(ctx, argv[0]);
   return JS_UNDEFINED;
}

static JSValue js_xhr_get_onerror(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr) return JS_UNDEFINED;
   return JS_DupValue(ctx, xhr->onerror);
}

static JSValue js_xhr_set_onerror(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   DilloXHR *xhr = (DilloXHR *)JS_GetOpaque(this_val, js_xhr_class_id);
   if (!xhr || argc < 1) return JS_UNDEFINED;
   JS_FreeValue(ctx, xhr->onerror);
   xhr->onerror = JS_DupValue(ctx, argv[0]);
   return JS_UNDEFINED;
}

/* -------------------------------------------------------------------------
 * Fetch & Response API
 * ------------------------------------------------------------------------- */

struct DilloFetchData {
   JSContext *ctx;
   JSValue resolve_func;
   JSValue reject_func;
   int status;
   std::string status_text;
   std::string body;
   std::map<std::string, std::string> headers;
   pthread_t thread;
   bool completed;
   std::string url;
   std::string method;
   std::string req_body;
   std::map<std::string, std::string> req_headers;

   DilloFetchData() : ctx(NULL), resolve_func(JS_UNDEFINED), reject_func(JS_UNDEFINED),
                      status(0), status_text(""), completed(false), method("GET") {}
};

static void *fetch_worker_thread(void *arg)
{
   DilloFetchData *fd = (DilloFetchData *)arg;
   if (!fd) return NULL;

   CURL *curl = curl_easy_init();
   if (!curl) {
      fd->status = 0;
      fd->status_text = "Curl Init Failed";
      fd->completed = true;
      return NULL;
   }

   CurlWriteContext cctx;
   curl_easy_setopt(curl, CURLOPT_URL, fd->url.c_str());
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cctx);
   curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
   curl_easy_setopt(curl, CURLOPT_HEADERDATA, &cctx);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
   curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

   struct curl_slist *headers_list = NULL;
   for (std::map<std::string, std::string>::iterator it = fd->req_headers.begin();
        it != fd->req_headers.end(); ++it) {
      std::string h = it->first + ": " + it->second;
      headers_list = curl_slist_append(headers_list, h.c_str());
   }
   if (headers_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_list);

   if (strcasecmp(fd->method.c_str(), "POST") == 0) {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      if (!fd->req_body.empty()) {
         curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fd->req_body.c_str());
         curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)fd->req_body.size());
      }
   } else if (strcasecmp(fd->method.c_str(), "PUT") == 0) {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
      if (!fd->req_body.empty()) {
         curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fd->req_body.c_str());
         curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)fd->req_body.size());
      }
   } else if (strcasecmp(fd->method.c_str(), "DELETE") == 0) {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
   }

   CURLcode res = curl_easy_perform(curl);
   if (res == CURLE_OK) {
      long http_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
      fd->status = (int)http_code;
      fd->status_text = (http_code >= 200 && http_code < 300) ? "OK" : "Error";
      fd->body = cctx.data;
      fd->headers = cctx.headers;
   } else {
      fd->status = 0;
      fd->status_text = curl_easy_strerror(res);
   }

   if (headers_list) curl_slist_free_all(headers_list);
   curl_easy_cleanup(curl);

   fd->completed = true;
   return NULL;
}

struct DilloResponseObj {
   int status;
   std::string status_text;
   std::string body;
   std::map<std::string, std::string> headers;
};

static void js_response_finalizer(JSRuntime *rt, JSValue val)
{
   (void)rt;
   DilloResponseObj *ro = (DilloResponseObj *)JS_GetOpaque(val, js_response_class_id);
   if (ro) delete ro;
}

static JSValue js_response_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloResponseObj *ro = (DilloResponseObj *)JS_GetOpaque(this_val, js_response_class_id);
   std::string text = ro ? ro->body : "";

   JSValue funcs[2];
   JSValue promise = JS_NewPromiseCapability(ctx, funcs);
   JSValue str_val = JS_NewString(ctx, text.c_str());
   JSValue ret = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, &str_val);
   JS_FreeValue(ctx, str_val);
   JS_FreeValue(ctx, ret);
   JS_FreeValue(ctx, funcs[0]);
   JS_FreeValue(ctx, funcs[1]);

   js_drain_microtasks(ctx);
   return promise;
}

static JSValue js_response_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloResponseObj *ro = (DilloResponseObj *)JS_GetOpaque(this_val, js_response_class_id);
   std::string text = ro ? ro->body : "{}";

   JSValue funcs[2];
   JSValue promise = JS_NewPromiseCapability(ctx, funcs);
   JSValue json_val = JS_ParseJSON(ctx, text.c_str(), text.size(), "<json>");
   if (JS_IsException(json_val)) {
      JSValue exc = JS_GetException(ctx);
      JSValue ret = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &exc);
      JS_FreeValue(ctx, exc);
      JS_FreeValue(ctx, ret);
   } else {
      JSValue ret = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, &json_val);
      JS_FreeValue(ctx, json_val);
      JS_FreeValue(ctx, ret);
   }
   JS_FreeValue(ctx, funcs[0]);
   JS_FreeValue(ctx, funcs[1]);

   js_drain_microtasks(ctx);
   return promise;
}

static JSValue js_response_get_status(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloResponseObj *ro = (DilloResponseObj *)JS_GetOpaque(this_val, js_response_class_id);
   return JS_NewInt32(ctx, ro ? ro->status : 0);
}

static JSValue js_response_get_status_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloResponseObj *ro = (DilloResponseObj *)JS_GetOpaque(this_val, js_response_class_id);
   return JS_NewString(ctx, ro ? ro->status_text.c_str() : "");
}

static JSValue js_response_get_ok(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloResponseObj *ro = (DilloResponseObj *)JS_GetOpaque(this_val, js_response_class_id);
   bool is_ok = ro && (ro->status >= 200 && ro->status < 300);
   return JS_NewBool(ctx, is_ok);
}

static void js_fetch_fltk_callback(void *data)
{
   DilloFetchData *fd = (DilloFetchData *)data;
   if (!fd) return;

   if (!fd->completed) {
      Fl::add_timeout(0.016, js_fetch_fltk_callback, fd);
      return;
   }

   pthread_join(fd->thread, NULL);

   JSContext *ctx = fd->ctx;
   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);

   if (fd->status == 0) {
      /* Network or DNS failure -> Reject Promise */
      JSValue err = JS_NewError(ctx);
      JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, fd->status_text.c_str()));
      JSValue ret = JS_Call(ctx, fd->reject_func, JS_UNDEFINED, 1, &err);
      JS_FreeValue(ctx, err);
      JS_FreeValue(ctx, ret);
   } else {
      /* Create Response object -> Resolve Promise */
      JSValue resp = JS_NewObjectProtoClass(ctx, dctx ? dctx->js_response_proto : JS_NULL, js_response_class_id);
      DilloResponseObj *ro = new DilloResponseObj();
      ro->status = fd->status;
      ro->status_text = fd->status_text;
      ro->body = fd->body;
      ro->headers = fd->headers;
      JS_SetOpaque(resp, ro);

      JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, ro->status));
      JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, ro->status_text.c_str()));
      JS_SetPropertyStr(ctx, resp, "ok", JS_NewBool(ctx, ro->status >= 200 && ro->status < 300));

      JSValue ret = JS_Call(ctx, fd->resolve_func, JS_UNDEFINED, 1, &resp);
      JS_FreeValue(ctx, resp);
      JS_FreeValue(ctx, ret);
   }

   JS_FreeValue(ctx, fd->resolve_func);
   JS_FreeValue(ctx, fd->reject_func);
   delete fd;

   js_drain_microtasks(ctx);
}

static JSValue js_window_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc < 1) return JS_EXCEPTION;

   const char *u = JS_ToCString(ctx, argv[0]);
   if (!u) return JS_EXCEPTION;

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   std::string base = (dctx && !dctx->page_url.empty()) ? dctx->page_url : "http://localhost/";

   DilloFetchData *fd = new DilloFetchData();
   fd->ctx = ctx;

   DilloUrl *parsed = a_Url_new(u, base.c_str());
   if (parsed) {
      char *full = a_Url_str(parsed);
      fd->url = full ? full : u;
      a_Url_free(parsed);
   } else {
      fd->url = u;
   }
   JS_FreeCString(ctx, u);

   /* Parse options argument if present */
   if (argc > 1 && JS_IsObject(argv[1])) {
      JSValue method_val = JS_GetPropertyStr(ctx, argv[1], "method");
      if (JS_IsString(method_val)) {
         const char *m = JS_ToCString(ctx, method_val);
         if (m) {
            fd->method = m;
            for (size_t i = 0; i < fd->method.size(); i++) fd->method[i] = toupper((unsigned char)fd->method[i]);
            JS_FreeCString(ctx, m);
         }
      }
      JS_FreeValue(ctx, method_val);

      JSValue body_val = JS_GetPropertyStr(ctx, argv[1], "body");
      if (JS_IsString(body_val)) {
         const char *b = JS_ToCString(ctx, body_val);
         if (b) {
            fd->req_body = b;
            JS_FreeCString(ctx, b);
         }
      }
      JS_FreeValue(ctx, body_val);

      JSValue headers_val = JS_GetPropertyStr(ctx, argv[1], "headers");
      if (JS_IsObject(headers_val)) {
         JSPropertyEnum *ptab = NULL;
         uint32_t plen = 0;
         if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, headers_val, JS_GPN_STRING_MASK) == 0) {
            for (uint32_t i = 0; i < plen; i++) {
               const char *key = JS_AtomToCString(ctx, ptab[i].atom);
               JSValue val = JS_GetProperty(ctx, headers_val, ptab[i].atom);
               const char *val_str = JS_ToCString(ctx, val);
               if (key && val_str) {
                  fd->req_headers[key] = val_str;
               }
               if (key) JS_FreeCString(ctx, key);
               if (val_str) JS_FreeCString(ctx, val_str);
               JS_FreeValue(ctx, val);
               JS_FreeAtom(ctx, ptab[i].atom);
            }
            js_free(ctx, ptab);
         }
      }
      JS_FreeValue(ctx, headers_val);
   }

   JSValue funcs[2];
   JSValue promise = JS_NewPromiseCapability(ctx, funcs);
   fd->resolve_func = funcs[0];
   fd->reject_func = funcs[1];

   if (pthread_create(&fd->thread, NULL, fetch_worker_thread, fd) != 0) {
      fd->completed = true;
      fd->status = 0;
      fd->status_text = "Thread Spawn Failed";
   }

   Fl::add_timeout(0.005, js_fetch_fltk_callback, fd);
   return promise;
}

/* -------------------------------------------------------------------------
 * Web Storage & Cookie Subsystems
 * ------------------------------------------------------------------------- */

enum StorageType {
   STORAGE_TYPE_SESSION = 0,
   STORAGE_TYPE_LOCAL = 1
};

enum StorageMode {
   STORAGE_MODE_NONE = 0,
   STORAGE_MODE_EPHEMERAL = 1,
   STORAGE_MODE_PERMIT = 2
};

struct DilloStorageRule {
   std::string domain;
   int mode; /* 0=DENY, 1=EPHEMERAL, 2=PERMIT */
   int quota_kb;
};

struct DilloStorageData {
   StorageType type;
   StorageMode mode;
   std::string origin;
   std::string domain;
   std::string storage_file;
   int quota_kb;
   std::map<std::string, std::string> items;
   std::vector<std::string> keys_order;

   DilloStorageData() : type(STORAGE_TYPE_SESSION), mode(STORAGE_MODE_NONE), quota_kb(512) {}

   void update_keys_order() {
      keys_order.clear();
      for (std::map<std::string, std::string>::iterator it = items.begin(); it != items.end(); ++it) {
         keys_order.push_back(it->first);
      }
   }

   size_t get_total_bytes() const {
      size_t total = 0;
      for (std::map<std::string, std::string>::const_iterator it = items.begin(); it != items.end(); ++it) {
         total += it->first.size() + it->second.size();
      }
      return total;
   }

   bool save_to_disk();
   bool load_from_disk();
};

static std::map<std::string, std::set<std::string> > g_httponly_cookies;
static std::map<std::string, std::map<std::string, std::string> > g_session_cookies;
static std::map<std::string, DilloStorageData *> g_local_storage_map;
static std::vector<DilloStorageRule> g_storage_rules;
static DilloStorageRule g_default_storage_rule = { "DEFAULT", 1 /* EPHEMERAL */, 512 };
static bool g_storage_rules_loaded = false;

void a_Dillo_JS_register_httponly_cookie(const char *host, const char *cookie_name)
{
   if (!host || !cookie_name) return;
   std::string h = host;
   std::string n = cookie_name;
   for (size_t i = 0; i < h.size(); i++) h[i] = tolower((unsigned char)h[i]);
   g_httponly_cookies[h].insert(n);
}

static void parse_origin_and_domain(const std::string &url_str, std::string &out_origin, std::string &out_domain)
{
   DilloUrl *u = a_Url_new(url_str.c_str(), NULL);
   if (u) {
      const char *scheme = URL_SCHEME(u);
      const char *host = URL_HOST(u);
      int port = URL_PORT(u);
      if (host && host[0]) {
         out_domain = host;
         for (size_t i = 0; i < out_domain.size(); i++) out_domain[i] = tolower((unsigned char)out_domain[i]);
         std::ostringstream ss;
         ss << (scheme ? scheme : "http") << "://" << out_domain;
         if (port > 0 && !(strcmp(scheme ? scheme : "", "http") == 0 && port == 80) &&
                         !(strcmp(scheme ? scheme : "", "https") == 0 && port == 443)) {
            ss << ":" << port;
         }
         out_origin = ss.str();
      } else {
         out_domain = "localhost";
         out_origin = (scheme && strcmp(scheme, "file") == 0) ? "file://" : "null";
      }
      a_Url_free(u);
   } else {
      out_domain = "localhost";
      out_origin = "null";
   }
}

static std::string sanitize_origin_for_filename(const std::string &origin)
{
   std::string s = origin;
   for (size_t i = 0; i < s.size(); i++) {
      char c = s[i];
      if (!isalnum((unsigned char)c) && c != '-' && c != '.') {
         s[i] = '_';
      }
   }
   return s;
}

static std::string get_storage_filepath(const std::string &origin)
{
   char *home = dGethomedir();
   if (!home) return "";
   std::string dir = std::string(home) + "/.dillo/storage";
   mkdir(dir.c_str(), 0700);
   std::string fname = sanitize_origin_for_filename(origin) + ".json";
   return dir + "/" + fname;
}

bool DilloStorageData::save_to_disk()
{
   if (mode != STORAGE_MODE_PERMIT || storage_file.empty()) return false;
   int fd = open(storage_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0) return false;

   std::ostringstream ss;
   ss << "{\n";
   bool first = true;
   for (std::map<std::string, std::string>::iterator it = items.begin(); it != items.end(); ++it) {
      if (!first) ss << ",\n";
      first = false;
      ss << "  \"";
      for (size_t i = 0; i < it->first.size(); i++) {
         char c = it->first[i];
         if (c == '"' || c == '\\') ss << '\\';
         ss << c;
      }
      ss << "\": \"";
      for (size_t i = 0; i < it->second.size(); i++) {
         char c = it->second[i];
         if (c == '"' || c == '\\') ss << '\\';
         else if (c == '\n') { ss << "\\n"; continue; }
         else if (c == '\r') { ss << "\\r"; continue; }
         else if (c == '\t') { ss << "\\t"; continue; }
         ss << c;
      }
      ss << "\"";
   }
   ss << "\n}\n";
   std::string str = ss.str();
   write(fd, str.data(), str.size());
   close(fd);
   return true;
}

bool DilloStorageData::load_from_disk()
{
   if (mode != STORAGE_MODE_PERMIT || storage_file.empty()) return false;
   FILE *fp = fopen(storage_file.c_str(), "r");
   if (!fp) return false;

   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (sz <= 0 || sz > 10 * 1024 * 1024) {
      fclose(fp);
      return false;
   }
   std::vector<char> buf(sz + 1, 0);
   size_t read_bytes = fread(&buf[0], 1, sz, fp);
   fclose(fp);
   (void)read_bytes;

   items.clear();
   keys_order.clear();
   const char *p = &buf[0];
   while (*p) {
      while (*p && *p != '"') p++;
      if (!*p) break;
      p++;
      std::string k;
      while (*p && *p != '"') {
         if (*p == '\\' && *(p+1)) {
            p++;
            if (*p == 'n') k += '\n';
            else if (*p == 'r') k += '\r';
            else if (*p == 't') k += '\t';
            else k += *p;
         } else {
            k += *p;
         }
         p++;
      }
      if (*p == '"') p++;
      while (*p && *p != ':') p++;
      if (!*p) break;
      p++;
      while (*p && *p != '"' && *p != '}' && *p != ',') p++;
      if (*p != '"') break;
      p++;
      std::string v;
      while (*p && *p != '"') {
         if (*p == '\\' && *(p+1)) {
            p++;
            if (*p == 'n') v += '\n';
            else if (*p == 'r') v += '\r';
            else if (*p == 't') v += '\t';
            else v += *p;
         } else {
            v += *p;
         }
         p++;
      }
      if (*p == '"') p++;
      items[k] = v;
      keys_order.push_back(k);
   }
   return true;
}

static bool storage_rule_comparator(const DilloStorageRule &a, const DilloStorageRule &b)
{
   return a.domain.size() > b.domain.size();
}

static void load_storagerc()
{
   if (g_storage_rules_loaded) return;
   g_storage_rules_loaded = true;
   g_storage_rules.clear();
   g_default_storage_rule.domain = "DEFAULT";
   g_default_storage_rule.mode = 1; /* EPHEMERAL */
   g_default_storage_rule.quota_kb = prefs.js_storage_quota_kb > 0 ? prefs.js_storage_quota_kb : 512;

   char *home = dGethomedir();
   if (!home) return;
   std::string path = std::string(home) + "/.dillo/storagerc";
   FILE *fp = fopen(path.c_str(), "r");
   if (!fp) return;

   char line[1024];
   while (fgets(line, sizeof(line), fp)) {
      char *p = line;
      while (*p && isspace((unsigned char)*p)) p++;
      if (*p == '\0' || *p == '#') continue;

      char domain[256] = {0};
      char action[64] = {0};
      int quota = g_default_storage_rule.quota_kb;

      int n = sscanf(p, "%255s %63s %d", domain, action, &quota);
      if (n >= 2) {
         int m = 1;
         if (!strcasecmp(action, "PERMIT")) m = 2;
         else if (!strcasecmp(action, "EPHEMERAL")) m = 1;
         else if (!strcasecmp(action, "DENY")) m = 0;

         if (!strcasecmp(domain, "DEFAULT") || !strcmp(domain, "*")) {
            g_default_storage_rule.mode = m;
            g_default_storage_rule.quota_kb = quota;
         } else {
            DilloStorageRule r;
            r.domain = domain;
            for (size_t i = 0; i < r.domain.size(); i++) r.domain[i] = tolower((unsigned char)r.domain[i]);
            r.mode = m;
            r.quota_kb = quota;
            g_storage_rules.push_back(r);
         }
      }
   }
   fclose(fp);

   std::sort(g_storage_rules.begin(), g_storage_rules.end(), storage_rule_comparator);
}

static DilloStorageRule check_storage_rule(const std::string &domain, int global_localstorage_pref, int default_quota_kb)
{
   DilloStorageRule res;
   res.domain = domain;
   res.mode = 0;
   res.quota_kb = default_quota_kb > 0 ? default_quota_kb : 512;

   if (global_localstorage_pref == JS_LOCALSTORAGE_NO) {
      res.mode = 0;
      return res;
   }
   if (global_localstorage_pref == JS_LOCALSTORAGE_EPHEMERAL) {
      res.mode = 1; // EPHEMERAL
      return res;
   }

   load_storagerc();

   std::string d = domain;
   for (size_t i = 0; i < d.size(); i++) d[i] = tolower((unsigned char)d[i]);

   for (size_t i = 0; i < g_storage_rules.size(); i++) {
      const DilloStorageRule &r = g_storage_rules[i];
      if (r.domain[0] == '.') {
         if (d.size() >= r.domain.size()) {
            if (d.compare(d.size() - r.domain.size(), r.domain.size(), r.domain) == 0) {
               res.mode = r.mode;
               res.quota_kb = r.quota_kb;
               return res;
            }
         }
      } else {
         if (d == r.domain) {
            res.mode = r.mode;
            res.quota_kb = r.quota_kb;
            return res;
         }
      }
   }

   res.mode = (g_default_storage_rule.mode == 2) ? 2 : (g_default_storage_rule.mode == 0 ? 0 : 1);
   res.quota_kb = g_default_storage_rule.quota_kb;
   return res;
}

static DilloStorageData *get_or_create_local_storage(const std::string &origin, const std::string &domain, StorageMode mode, int quota_kb)
{
   if (g_local_storage_map.count(origin)) {
      return g_local_storage_map[origin];
   }
   DilloStorageData *sd = new DilloStorageData();
   sd->type = STORAGE_TYPE_LOCAL;
   sd->mode = mode;
   sd->origin = origin;
   sd->domain = domain;
   sd->quota_kb = quota_kb;
   if (mode == STORAGE_MODE_PERMIT) {
      sd->storage_file = get_storage_filepath(origin);
      sd->load_from_disk();
   }
   g_local_storage_map[origin] = sd;
   return sd;
}

static JSValue js_storage_get_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_NULL;
   DilloStorageData *sd = (DilloStorageData *)JS_GetOpaque(this_val, js_storage_class_id);
   if (!sd) return JS_NULL;

   const char *k = JS_ToCString(ctx, argv[0]);
   if (!k) return JS_NULL;

   std::string sk = k;
   JS_FreeCString(ctx, k);

   if (sd->items.count(sk)) {
      return JS_NewString(ctx, sd->items[sk].c_str());
   }
   return JS_NULL;
}

static JSValue js_storage_set_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 2) return JS_UNDEFINED;
   DilloStorageData *sd = (DilloStorageData *)JS_GetOpaque(this_val, js_storage_class_id);
   if (!sd) return JS_UNDEFINED;

   const char *k = JS_ToCString(ctx, argv[0]);
   const char *v = JS_ToCString(ctx, argv[1]);
   if (!k || !v) {
      if (k) JS_FreeCString(ctx, k);
      if (v) JS_FreeCString(ctx, v);
      return JS_UNDEFINED;
   }

   std::string sk = k;
   std::string sv = v;
   JS_FreeCString(ctx, k);
   JS_FreeCString(ctx, v);

   size_t current_size = sd->get_total_bytes();
   if (sd->items.count(sk)) {
      current_size -= (sk.size() + sd->items[sk].size());
   }
   size_t new_size = current_size + sk.size() + sv.size();
   size_t quota_bytes = (size_t)sd->quota_kb * 1024;

   if (new_size > quota_bytes) {
      JSValue err = JS_NewError(ctx);
      JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, "QuotaExceededError"));
      JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "The quota has been exceeded."));
      return JS_Throw(ctx, err);
   }

   bool is_new = (sd->items.count(sk) == 0);
   sd->items[sk] = sv;
   if (is_new) {
      sd->keys_order.push_back(sk);
   }

   if (sd->mode == STORAGE_MODE_PERMIT) {
      sd->save_to_disk();
   }

   return JS_UNDEFINED;
}

static JSValue js_storage_remove_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_UNDEFINED;
   DilloStorageData *sd = (DilloStorageData *)JS_GetOpaque(this_val, js_storage_class_id);
   if (!sd) return JS_UNDEFINED;

   const char *k = JS_ToCString(ctx, argv[0]);
   if (!k) return JS_UNDEFINED;

   std::string sk = k;
   JS_FreeCString(ctx, k);

   if (sd->items.count(sk)) {
      sd->items.erase(sk);
      sd->update_keys_order();
      if (sd->mode == STORAGE_MODE_PERMIT) {
         sd->save_to_disk();
      }
   }
   return JS_UNDEFINED;
}

static JSValue js_storage_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloStorageData *sd = (DilloStorageData *)JS_GetOpaque(this_val, js_storage_class_id);
   if (!sd) return JS_UNDEFINED;

   sd->items.clear();
   sd->keys_order.clear();
   if (sd->mode == STORAGE_MODE_PERMIT) {
      sd->save_to_disk();
   }
   return JS_UNDEFINED;
}

static JSValue js_storage_key(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   if (argc < 1) return JS_NULL;
   DilloStorageData *sd = (DilloStorageData *)JS_GetOpaque(this_val, js_storage_class_id);
   if (!sd) return JS_NULL;

   int32_t idx = 0;
   if (JS_ToInt32(ctx, &idx, argv[0]) < 0) return JS_NULL;

   if (idx >= 0 && (size_t)idx < sd->keys_order.size()) {
      return JS_NewString(ctx, sd->keys_order[idx].c_str());
   }
   return JS_NULL;
}

static JSValue js_storage_get_length(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)argc; (void)argv;
   DilloStorageData *sd = (DilloStorageData *)JS_GetOpaque(this_val, js_storage_class_id);
   if (!sd) return JS_NewInt32(ctx, 0);
   return JS_NewInt32(ctx, (int32_t)sd->items.size());
}

static JSValue js_document_get_cookie(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   if (prefs.js_document_cookie != JS_COOKIE_RESTRICTED) {
      return JS_NewString(ctx, "");
   }

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (!dctx || dctx->page_url.empty()) return JS_NewString(ctx, "");

   DilloUrl *url = a_Url_new(dctx->page_url.c_str(), NULL);
   if (!url) return JS_NewString(ctx, "");

   if (a_Cookies_control_check(url) == COOKIE_DENY) {
      a_Url_free(url);
      return JS_NewString(ctx, "");
   }

   std::string host = URL_HOST(url) ? URL_HOST(url) : "localhost";
   if (host.empty()) host = "localhost";
   for (size_t i = 0; i < host.size(); i++) host[i] = tolower((unsigned char)host[i]);

   std::map<std::string, std::string> visible_cookies;

   char *dpi_q = a_Cookies_get_query(url, NULL, 1);
   if (dpi_q) {
      char *token = strtok(dpi_q, ";");
      while (token) {
         while (*token && isspace((unsigned char)*token)) token++;
         char *eq = strchr(token, '=');
         if (eq) {
            std::string n(token, eq - token);
            std::string v(eq + 1);
            while (!n.empty() && isspace((unsigned char)n[n.size()-1])) n.erase(n.size()-1);
            while (!v.empty() && isspace((unsigned char)v[v.size()-1])) v.erase(v.size()-1);
            visible_cookies[n] = v;
         }
         token = strtok(NULL, ";");
      }
      dFree(dpi_q);
   }

   for (std::map<std::string, std::map<std::string, std::string> >::iterator it = g_session_cookies.begin();
        it != g_session_cookies.end(); ++it) {
      const std::string &cdom = it->first;
      bool matches = false;
      if (cdom == host) {
         matches = true;
      } else if (!cdom.empty() && cdom[0] == '.' && host.size() >= cdom.size()) {
         if (host.compare(host.size() - cdom.size(), cdom.size(), cdom) == 0) {
            matches = true;
         }
      }
      if (matches) {
         for (std::map<std::string, std::string>::iterator cit = it->second.begin(); cit != it->second.end(); ++cit) {
            visible_cookies[cit->first] = cit->second;
         }
      }
   }

   if (g_httponly_cookies.count(host)) {
      const std::set<std::string> &ho_set = g_httponly_cookies[host];
      for (std::set<std::string>::const_iterator hit = ho_set.begin(); hit != ho_set.end(); ++hit) {
         visible_cookies.erase(*hit);
      }
   }

   a_Url_free(url);

   std::string res;
   for (std::map<std::string, std::string>::iterator it = visible_cookies.begin();
        it != visible_cookies.end(); ++it) {
      if (!res.empty()) res += "; ";
      res += it->first + "=" + it->second;
   }

   return JS_NewString(ctx, res.c_str());
}

static JSValue js_document_set_cookie(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc < 1 || prefs.js_document_cookie != JS_COOKIE_RESTRICTED) {
      return JS_UNDEFINED;
   }

   DilloJSContext *dctx = (DilloJSContext *)JS_GetContextOpaque(ctx);
   if (!dctx || dctx->page_url.empty()) return JS_UNDEFINED;

   DilloUrl *url = a_Url_new(dctx->page_url.c_str(), NULL);
   if (!url) return JS_UNDEFINED;

   if (a_Cookies_control_check(url) == COOKIE_DENY) {
      a_Url_free(url);
      return JS_UNDEFINED;
   }

   const char *c_str = JS_ToCString(ctx, argv[0]);
   if (!c_str) {
      a_Url_free(url);
      return JS_UNDEFINED;
   }

   std::string host = URL_HOST(url) ? URL_HOST(url) : "localhost";
   if (host.empty()) host = "localhost";
   for (size_t i = 0; i < host.size(); i++) host[i] = tolower((unsigned char)host[i]);

   std::string raw = c_str;
   JS_FreeCString(ctx, c_str);

   std::string cookie_name, cookie_val, cookie_domain = host;
   bool is_delete = false;

   std::vector<std::string> parts;
   size_t pos = 0;
   while (pos < raw.size()) {
      size_t semi = raw.find(';', pos);
      std::string part = (semi == std::string::npos) ? raw.substr(pos) : raw.substr(pos, semi - pos);
      while (!part.empty() && isspace((unsigned char)part[0])) part.erase(0, 1);
      while (!part.empty() && isspace((unsigned char)part[part.size()-1])) part.erase(part.size()-1);
      if (!part.empty()) parts.push_back(part);
      if (semi == std::string::npos) break;
      pos = semi + 1;
   }

   if (parts.empty()) {
      a_Url_free(url);
      return JS_UNDEFINED;
   }

   size_t eq = parts[0].find('=');
   if (eq == std::string::npos) {
      a_Url_free(url);
      return JS_UNDEFINED;
   }
   cookie_name = parts[0].substr(0, eq);
   cookie_val = parts[0].substr(eq + 1);

   for (size_t i = 1; i < parts.size(); i++) {
      std::string attr = parts[i];
      size_t aeq = attr.find('=');
      std::string aname = (aeq == std::string::npos) ? attr : attr.substr(0, aeq);
      std::string aval = (aeq == std::string::npos) ? "" : attr.substr(aeq + 1);
      for (size_t j = 0; j < aname.size(); j++) aname[j] = tolower((unsigned char)aname[j]);

      if (aname == "domain" && !aval.empty()) {
         for (size_t j = 0; j < aval.size(); j++) aval[j] = tolower((unsigned char)aval[j]);
         if (aval == host) {
            cookie_domain = aval;
         } else if (aval[0] == '.' && host.size() >= aval.size() &&
                    host.compare(host.size() - aval.size(), aval.size(), aval) == 0) {
            cookie_domain = aval;
         } else if (host.size() > aval.size() && host[host.size() - aval.size() - 1] == '.' &&
                    host.compare(host.size() - aval.size(), aval.size(), aval) == 0) {
            cookie_domain = "." + aval;
         } else {
            a_Url_free(url);
            return JS_UNDEFINED;
         }
      } else if (aname == "max-age" && aval == "0") {
         is_delete = true;
      } else if (aname == "expires") {
         if (aval.find("1970") != std::string::npos || aval.find("1969") != std::string::npos) {
            is_delete = true;
         }
      }
   }

   if (is_delete) {
      g_session_cookies[cookie_domain].erase(cookie_name);
   } else {
      g_session_cookies[cookie_domain][cookie_name] = cookie_val;
   }

   Dlist *clist = dList_new(1);
   dList_append(clist, dStrdup(raw.c_str()));
   a_Cookies_set(clist, url, NULL);
   dFree(dList_nth_data(clist, 0));
   dList_free(clist);

   a_Url_free(url);
   return JS_UNDEFINED;
}

void a_Dillo_JS_init(void)
{
   if (g_js_runtime) return;

   g_js_runtime = JS_NewRuntime();
   if (!g_js_runtime) {
      MSG_ERR("[Dillo QuickJS] Failed to allocate JSRuntime!\n");
      return;
   }

   /* Register element class */
   static JSClassDef elem_class_def = {
      "Element",
      js_element_finalizer,
      NULL, NULL, NULL
   };
   JS_NewClassID(&js_element_class_id);
   JS_NewClass(g_js_runtime, js_element_class_id, &elem_class_def);

   static JSClassDef classlist_class_def = {
      "DOMTokenList",
      js_classlist_finalizer,
      NULL, NULL, NULL
   };
   JS_NewClassID(&js_classlist_class_id);
   JS_NewClass(g_js_runtime, js_classlist_class_id, &classlist_class_def);

   static JSClassDef style_class_def = {
      "CSSStyleDeclaration",
      js_style_finalizer,
      NULL, NULL, NULL
   };
   JS_NewClassID(&js_style_class_id);
   JS_NewClass(g_js_runtime, js_style_class_id, &style_class_def);

   static JSClassDef event_class_def = {
      "Event",
      js_event_finalizer,
      NULL, NULL, NULL
   };
   JS_NewClassID(&js_event_class_id);
   JS_NewClass(g_js_runtime, js_event_class_id, &event_class_def);

   static JSClassDef xhr_class_def = {
      "XMLHttpRequest",
      js_xhr_finalizer,
      NULL, NULL, NULL
   };
   JS_NewClassID(&js_xhr_class_id);
   JS_NewClass(g_js_runtime, js_xhr_class_id, &xhr_class_def);

   static JSClassDef response_class_def = {
      "Response",
      js_response_finalizer,
      NULL, NULL, NULL
   };
   JS_NewClassID(&js_response_class_id);
   JS_NewClass(g_js_runtime, js_response_class_id, &response_class_def);

   static JSClassDef storage_class_def = {
      "Storage",
      NULL,
      NULL, NULL, NULL
   };
   JS_NewClassID(&js_storage_class_id);
   JS_NewClass(g_js_runtime, js_storage_class_id, &storage_class_def);

   MSG("[Dillo QuickJS] Runtime initialized successfully.\n");
}

void a_Dillo_JS_cleanup(void)
{
   for (std::map<std::string, DilloStorageData*>::iterator it = g_local_storage_map.begin();
        it != g_local_storage_map.end(); ++it) {
      delete it->second;
   }
   g_local_storage_map.clear();

   if (g_js_runtime) {
      JS_FreeRuntime(g_js_runtime);
      g_js_runtime = NULL;
      MSG("[Dillo QuickJS] Runtime freed.\n");
   }
}

void *a_Dillo_JS_context_new(void *v_html, const char *url_str)
{
   if (!g_js_runtime) {
      a_Dillo_JS_init();
      if (!g_js_runtime) return NULL;
   }

   JSContext *ctx = JS_NewContext(g_js_runtime);
   if (!ctx) return NULL;

   DilloJSContext *dctx = new DilloJSContext();
   dctx->ctx = ctx;
   dctx->html = v_html;
   dctx->page_url = url_str ? url_str : "";
   dctx->next_timer_id = 0;
   dctx->next_element_id = 0;
   dctx->root_element = NULL;
   dctx->head_element = NULL;
   dctx->body_element = NULL;
   dctx->current_parent = NULL;
   dctx->session_storage = NULL;

   JS_SetContextOpaque(ctx, dctx);

   JSValue global = JS_GetGlobalObject(ctx);
   dctx->window_obj = JS_DupValue(ctx, global);

   /* 1. Global Window Self References */
   JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
   JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));

   /* 2. Window Dialogs */
   JS_SetPropertyStr(ctx, global, "alert", JS_NewCFunction(ctx, js_window_alert, "alert", 1));
   JS_SetPropertyStr(ctx, global, "confirm", JS_NewCFunction(ctx, js_window_confirm, "confirm", 1));
   JS_SetPropertyStr(ctx, global, "prompt", JS_NewCFunction(ctx, js_window_prompt, "prompt", 1));

   /* 3. Window Timers */
   JS_SetPropertyStr(ctx, global, "setTimeout", JS_NewCFunction(ctx, js_window_set_timeout, "setTimeout", 2));
   JS_SetPropertyStr(ctx, global, "clearTimeout", JS_NewCFunction(ctx, js_window_clear_timer, "clearTimeout", 1));
   JS_SetPropertyStr(ctx, global, "setInterval", JS_NewCFunction(ctx, js_window_set_interval, "setInterval", 2));
   JS_SetPropertyStr(ctx, global, "clearInterval", JS_NewCFunction(ctx, js_window_clear_timer, "clearInterval", 1));

   /* 4. Console */
   JSValue console = JS_NewObject(ctx);
   JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_console_log, "log", 1));
   JS_SetPropertyStr(ctx, console, "info", JS_NewCFunction(ctx, js_console_log, "info", 1));
   JS_SetPropertyStr(ctx, console, "warn", JS_NewCFunction(ctx, js_console_warn, "warn", 1));
   JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, js_console_error, "error", 1));
   JS_SetPropertyStr(ctx, global, "console", console);

   /* 5. Navigator */
   JSValue navigator = JS_NewObject(ctx);
   JS_SetPropertyStr(ctx, navigator, "userAgent", JS_NewString(ctx, "Mozilla/5.0 (compatible; Dillo/3.3.0; QuickJS)"));
   JS_SetPropertyStr(ctx, navigator, "appName", JS_NewString(ctx, "Netscape"));
   JS_SetPropertyStr(ctx, navigator, "appVersion", JS_NewString(ctx, "5.0 (Dillo 3.3.0)"));
   JS_SetPropertyStr(ctx, navigator, "platform", JS_NewString(ctx, "MacIntel"));
   JS_SetPropertyStr(ctx, global, "navigator", navigator);

   /* 6. Location */
   JSValue location = JS_NewObject(ctx);
   js_define_get_set(ctx, location, "href", (JSCFunction *)js_location_get_href, (JSCFunction *)js_location_set_href);
   JS_SetPropertyStr(ctx, location, "assign", JS_NewCFunction(ctx, js_location_assign, "assign", 1));
   JS_SetPropertyStr(ctx, location, "replace", JS_NewCFunction(ctx, js_location_replace, "replace", 1));
   JS_SetPropertyStr(ctx, location, "reload", JS_NewCFunction(ctx, js_location_reload, "reload", 0));
   JS_SetPropertyStr(ctx, global, "location", location);

   /* 7. Window EventTarget methods */
   JS_SetPropertyStr(ctx, global, "addEventListener", JS_NewCFunction(ctx, js_event_target_add_event_listener, "addEventListener", 2));
   JS_SetPropertyStr(ctx, global, "removeEventListener", JS_NewCFunction(ctx, js_event_target_remove_event_listener, "removeEventListener", 2));
   JS_SetPropertyStr(ctx, global, "dispatchEvent", JS_NewCFunction(ctx, js_event_target_dispatch_event, "dispatchEvent", 1));

   /* 8. Event Constructors */
   JSValue event_ctor = JS_NewCFunction2(ctx, (JSCFunction*)js_event_constructor, "Event", 1, JS_CFUNC_constructor, 0);
   JS_SetPropertyStr(ctx, global, "Event", JS_DupValue(ctx, event_ctor));
   JS_SetPropertyStr(ctx, global, "CustomEvent", JS_DupValue(ctx, event_ctor));
   JS_SetPropertyStr(ctx, global, "MouseEvent", JS_DupValue(ctx, event_ctor));
   JS_SetPropertyStr(ctx, global, "SubmitEvent", event_ctor);

   /* 9. Prototypes */
   dctx->js_event_proto = JS_NewObject(ctx);
   dctx->js_element_proto = JS_NewObject(ctx);
   dctx->js_form_proto = JS_NewObjectProto(ctx, dctx->js_element_proto);
   dctx->js_input_proto = JS_NewObjectProto(ctx, dctx->js_element_proto);
   dctx->js_anchor_proto = JS_NewObjectProto(ctx, dctx->js_element_proto);
   dctx->js_image_proto = JS_NewObjectProto(ctx, dctx->js_element_proto);
   dctx->js_classlist_proto = JS_NewObject(ctx);
   dctx->js_style_proto = JS_NewObject(ctx);

   /* XHR Prototype & Constructor */
   dctx->js_xhr_proto = JS_NewObject(ctx);
   JS_SetPropertyStr(ctx, dctx->js_xhr_proto, "open", JS_NewCFunction(ctx, js_xhr_open, "open", 3));
   JS_SetPropertyStr(ctx, dctx->js_xhr_proto, "send", JS_NewCFunction(ctx, js_xhr_send, "send", 1));
   JS_SetPropertyStr(ctx, dctx->js_xhr_proto, "abort", JS_NewCFunction(ctx, js_xhr_abort, "abort", 0));
   JS_SetPropertyStr(ctx, dctx->js_xhr_proto, "setRequestHeader", JS_NewCFunction(ctx, js_xhr_set_request_header, "setRequestHeader", 2));
   JS_SetPropertyStr(ctx, dctx->js_xhr_proto, "getResponseHeader", JS_NewCFunction(ctx, js_xhr_get_response_header, "getResponseHeader", 1));
   JS_SetPropertyStr(ctx, dctx->js_xhr_proto, "getAllResponseHeaders", JS_NewCFunction(ctx, js_xhr_get_all_response_headers, "getAllResponseHeaders", 0));
   js_define_get_set(ctx, dctx->js_xhr_proto, "readyState", (JSCFunction *)js_xhr_get_ready_state, NULL);
   js_define_get_set(ctx, dctx->js_xhr_proto, "status", (JSCFunction *)js_xhr_get_status, NULL);
   js_define_get_set(ctx, dctx->js_xhr_proto, "statusText", (JSCFunction *)js_xhr_get_status_text, NULL);
   js_define_get_set(ctx, dctx->js_xhr_proto, "responseText", (JSCFunction *)js_xhr_get_response_text, NULL);
   js_define_get_set(ctx, dctx->js_xhr_proto, "response", (JSCFunction *)js_xhr_get_response_text, NULL);
   js_define_get_set(ctx, dctx->js_xhr_proto, "onreadystatechange", (JSCFunction *)js_xhr_get_onreadystatechange, (JSCFunction *)js_xhr_set_onreadystatechange);
   js_define_get_set(ctx, dctx->js_xhr_proto, "onload", (JSCFunction *)js_xhr_get_onload, (JSCFunction *)js_xhr_set_onload);
   js_define_get_set(ctx, dctx->js_xhr_proto, "onerror", (JSCFunction *)js_xhr_get_onerror, (JSCFunction *)js_xhr_set_onerror);

   JSValue xhr_ctor = JS_NewCFunction2(ctx, (JSCFunction*)js_xhr_constructor, "XMLHttpRequest", 0, JS_CFUNC_constructor, 0);
   JS_SetPropertyStr(ctx, xhr_ctor, "UNSENT", JS_NewInt32(ctx, 0));
   JS_SetPropertyStr(ctx, xhr_ctor, "OPENED", JS_NewInt32(ctx, 1));
   JS_SetPropertyStr(ctx, xhr_ctor, "HEADERS_RECEIVED", JS_NewInt32(ctx, 2));
   JS_SetPropertyStr(ctx, xhr_ctor, "LOADING", JS_NewInt32(ctx, 3));
   JS_SetPropertyStr(ctx, xhr_ctor, "DONE", JS_NewInt32(ctx, 4));
   JS_SetConstructor(ctx, xhr_ctor, dctx->js_xhr_proto);
   JS_SetPropertyStr(ctx, global, "XMLHttpRequest", xhr_ctor);

   /* Response Prototype & Constructor */
   dctx->js_response_proto = JS_NewObject(ctx);
   JS_SetPropertyStr(ctx, dctx->js_response_proto, "text", JS_NewCFunction(ctx, js_response_text, "text", 0));
   JS_SetPropertyStr(ctx, dctx->js_response_proto, "json", JS_NewCFunction(ctx, js_response_json, "json", 0));
   js_define_get_set(ctx, dctx->js_response_proto, "status", (JSCFunction *)js_response_get_status, NULL);
   js_define_get_set(ctx, dctx->js_response_proto, "statusText", (JSCFunction *)js_response_get_status_text, NULL);
   js_define_get_set(ctx, dctx->js_response_proto, "ok", (JSCFunction *)js_response_get_ok, NULL);

   JSValue response_ctor = JS_NewObject(ctx);
   JS_SetConstructor(ctx, response_ctor, dctx->js_response_proto);
   JS_SetPropertyStr(ctx, global, "Response", response_ctor);

   /* Fetch API on Window / Global */
   JS_SetPropertyStr(ctx, global, "fetch", JS_NewCFunction(ctx, js_window_fetch, "fetch", 2));

   /* Web Storage API */
   dctx->js_storage_proto = JS_NewObject(ctx);
   JS_SetPropertyStr(ctx, dctx->js_storage_proto, "getItem", JS_NewCFunction(ctx, js_storage_get_item, "getItem", 1));
   JS_SetPropertyStr(ctx, dctx->js_storage_proto, "setItem", JS_NewCFunction(ctx, js_storage_set_item, "setItem", 2));
   JS_SetPropertyStr(ctx, dctx->js_storage_proto, "removeItem", JS_NewCFunction(ctx, js_storage_remove_item, "removeItem", 1));
   JS_SetPropertyStr(ctx, dctx->js_storage_proto, "clear", JS_NewCFunction(ctx, js_storage_clear, "clear", 0));
   JS_SetPropertyStr(ctx, dctx->js_storage_proto, "key", JS_NewCFunction(ctx, js_storage_key, "key", 1));
   js_define_get_set(ctx, dctx->js_storage_proto, "length", (JSCFunction *)js_storage_get_length, NULL);

   std::string doc_origin, doc_domain;
   parse_origin_and_domain(dctx->page_url, doc_origin, doc_domain);

   if (prefs.js_sessionstorage == JS_STORAGE_PERMIT) {
      DilloStorageData *ssd = new DilloStorageData();
      ssd->type = STORAGE_TYPE_SESSION;
      ssd->mode = STORAGE_MODE_EPHEMERAL;
      ssd->origin = doc_origin;
      ssd->domain = doc_domain;
      ssd->quota_kb = prefs.js_storage_quota_kb > 0 ? prefs.js_storage_quota_kb : 512;
      dctx->session_storage = ssd;

      JSValue sess_obj = JS_NewObjectProtoClass(ctx, dctx->js_storage_proto, js_storage_class_id);
      JS_SetOpaque(sess_obj, ssd);
      JS_SetPropertyStr(ctx, global, "sessionStorage", sess_obj);
   }

   if (prefs.js_localstorage != JS_LOCALSTORAGE_NO) {
      DilloStorageRule rule = check_storage_rule(doc_domain, prefs.js_localstorage, prefs.js_storage_quota_kb);
      if (rule.mode != STORAGE_MODE_NONE) {
         DilloStorageData *lsd = get_or_create_local_storage(doc_origin, doc_domain, (StorageMode)rule.mode, rule.quota_kb);
         JSValue local_obj = JS_NewObjectProtoClass(ctx, dctx->js_storage_proto, js_storage_class_id);
         JS_SetOpaque(local_obj, lsd);
         JS_SetPropertyStr(ctx, global, "localStorage", local_obj);
      }
   }

   /* 10. Document */
   JSValue document = JS_NewObject(ctx);
   dctx->document_obj = JS_DupValue(ctx, document);

   js_define_get_set(ctx, document, "title", (JSCFunction *)js_document_get_title, (JSCFunction *)js_document_set_title);
   js_define_get_set(ctx, document, "cookie", (JSCFunction *)js_document_get_cookie, (JSCFunction *)js_document_set_cookie);
   js_define_get_set(ctx, document, "forms", (JSCFunction *)js_document_get_forms, NULL);
   js_define_get_set(ctx, document, "links", (JSCFunction *)js_document_get_links, NULL);
   js_define_get_set(ctx, document, "images", (JSCFunction *)js_document_get_images, NULL);
   js_define_get_set(ctx, document, "scripts", (JSCFunction *)js_document_get_scripts, NULL);
   js_define_get_set(ctx, document, "body", (JSCFunction *)js_document_get_body, NULL);
   js_define_get_set(ctx, document, "head", (JSCFunction *)js_document_get_head, NULL);
   js_define_get_set(ctx, document, "documentElement", (JSCFunction *)js_document_get_document_element, NULL);

   JS_SetPropertyStr(ctx, document, "location", JS_DupValue(ctx, location));
   JS_SetPropertyStr(ctx, document, "URL", JS_NewString(ctx, dctx->page_url.c_str()));
   JS_SetPropertyStr(ctx, document, "getElementById", JS_NewCFunction(ctx, js_document_get_element_by_id, "getElementById", 1));
   JS_SetPropertyStr(ctx, document, "getElementsByTagName", JS_NewCFunction(ctx, js_document_get_elements_by_tag_name, "getElementsByTagName", 1));
   JS_SetPropertyStr(ctx, document, "getElementsByClassName", JS_NewCFunction(ctx, js_document_get_elements_by_class_name, "getElementsByClassName", 1));
   JS_SetPropertyStr(ctx, document, "querySelector", JS_NewCFunction(ctx, js_document_query_selector, "querySelector", 1));
   JS_SetPropertyStr(ctx, document, "querySelectorAll", JS_NewCFunction(ctx, js_document_query_selector_all, "querySelectorAll", 1));
   JS_SetPropertyStr(ctx, document, "createElement", JS_NewCFunction(ctx, js_document_create_element, "createElement", 1));

   JS_SetPropertyStr(ctx, document, "addEventListener", JS_NewCFunction(ctx, js_event_target_add_event_listener, "addEventListener", 2));
   JS_SetPropertyStr(ctx, document, "removeEventListener", JS_NewCFunction(ctx, js_event_target_remove_event_listener, "removeEventListener", 2));
   JS_SetPropertyStr(ctx, document, "dispatchEvent", JS_NewCFunction(ctx, js_event_target_dispatch_event, "dispatchEvent", 1));

   JS_SetPropertyStr(ctx, global, "document", document);

   JS_FreeValue(ctx, global);

   return dctx;
}

void a_Dillo_JS_context_free(void *v_ctx)
{
   if (!v_ctx) return;
   DilloJSContext *dctx = (DilloJSContext *)v_ctx;

   if (dctx->session_storage) {
      delete (DilloStorageData *)dctx->session_storage;
      dctx->session_storage = NULL;
   }

   for (size_t i = 0; i < dctx->timers.size(); i++) {
      DilloJSTimer *t = dctx->timers[i];
      if (t) {
         if (t->active) {
            Fl::remove_timeout(js_timer_fltk_callback, t);
            t->active = false;
         }
         JS_FreeValue(dctx->ctx, t->func);
         delete t;
      }
   }
   dctx->timers.clear();

   for (size_t i = 0; i < dctx->window_listeners.size(); i++) {
      JS_FreeValue(dctx->ctx, dctx->window_listeners[i].callback);
   }
   for (size_t i = 0; i < dctx->document_listeners.size(); i++) {
      JS_FreeValue(dctx->ctx, dctx->document_listeners[i].callback);
   }

   for (size_t i = 0; i < dctx->all_elements.size(); i++) {
      DilloDOMElement *e = dctx->all_elements[i];
      if (e) {
         for (size_t l = 0; l < e->listeners.size(); l++) {
            JS_FreeValue(dctx->ctx, e->listeners[l].callback);
         }
         for (std::map<std::string, JSValue>::iterator it = e->on_handlers.begin();
              it != e->on_handlers.end(); ++it) {
            JS_FreeValue(dctx->ctx, it->second);
         }
         if (!JS_IsUndefined(e->js_obj) && !JS_IsNull(e->js_obj)) {
            JS_FreeValue(dctx->ctx, e->js_obj);
         }
         delete e;
      }
   }
   dctx->all_elements.clear();

   JS_FreeValue(dctx->ctx, dctx->document_obj);
   JS_FreeValue(dctx->ctx, dctx->window_obj);

   JS_FreeValue(dctx->ctx, dctx->js_event_proto);
   JS_FreeValue(dctx->ctx, dctx->js_element_proto);
   JS_FreeValue(dctx->ctx, dctx->js_form_proto);
   JS_FreeValue(dctx->ctx, dctx->js_input_proto);
   JS_FreeValue(dctx->ctx, dctx->js_anchor_proto);
   JS_FreeValue(dctx->ctx, dctx->js_image_proto);
   JS_FreeValue(dctx->ctx, dctx->js_classlist_proto);
   JS_FreeValue(dctx->ctx, dctx->js_style_proto);
   JS_FreeValue(dctx->ctx, dctx->js_xhr_proto);
   JS_FreeValue(dctx->ctx, dctx->js_response_proto);

   if (dctx->ctx) {
      JS_FreeContext(dctx->ctx);
      dctx->ctx = NULL;
   }
   delete dctx;
}

void a_Dillo_JS_eval(void *v_ctx, const char *code, size_t len, const char *filename)
{
   if (!v_ctx || !code || len == 0) return;
   DilloJSContext *dctx = (DilloJSContext *)v_ctx;
   if (!dctx->ctx) return;

   const char *fn = filename ? filename : "<script>";
   JSValue ret = JS_Eval(dctx->ctx, code, len, fn, JS_EVAL_TYPE_GLOBAL);

   if (JS_IsException(ret)) {
      JSValue exc = JS_GetException(dctx->ctx);
      const char *exc_str = JS_ToCString(dctx->ctx, exc);
      if (exc_str) {
         MSG_ERR("[Dillo QuickJS Error in %s] %s\n", fn, exc_str);
         JS_FreeCString(dctx->ctx, exc_str);
      }
      JS_FreeValue(dctx->ctx, exc);
   }
   JS_FreeValue(dctx->ctx, ret);

   js_drain_microtasks(dctx->ctx);
}

/* -------------------------------------------------------------------------
 * DOM Parser Hooks
 * ------------------------------------------------------------------------- */

void a_Dillo_JS_on_tag_open(void *v_ctx, const char *tag_name, const char *tag, int tagsize,
                            void *v_form, void *v_input, int link_idx, int img_idx)
{
   if (!v_ctx || !tag_name) return;
   DilloJSContext *dctx = (DilloJSContext *)v_ctx;

   DilloDOMElement *elem = new DilloDOMElement();
   elem->id_num = ++dctx->next_element_id;
   std::string t(tag_name);
   for (size_t i = 0; i < t.size(); i++) t[i] = toupper((unsigned char)t[i]);
   elem->tag_name = t;
   elem->dillo_html = dctx->html;
   elem->dillo_form = v_form;
   elem->dillo_input = v_input;
   elem->link_idx = link_idx;
   elem->img_idx = img_idx;

   /* Parse attributes from raw tag string: <tag attr="val" ...> */
   if (tag && tagsize > 2) {
      int i = 1;
      while (i < tagsize && !isspace((unsigned char)tag[i]) && tag[i] != '>') i++;
      while (i < tagsize) {
         while (i < tagsize && (isspace((unsigned char)tag[i]) || tag[i] == '/')) i++;
         if (i >= tagsize || tag[i] == '>') break;

         size_t name_start = i;
         while (i < tagsize && !isspace((unsigned char)tag[i]) && tag[i] != '=' && tag[i] != '>') i++;
         std::string a_name(tag + name_start, i - name_start);

         while (i < tagsize && isspace((unsigned char)tag[i])) i++;
         std::string a_val;
         if (i < tagsize && tag[i] == '=') {
            i++;
            while (i < tagsize && isspace((unsigned char)tag[i])) i++;
            if (i < tagsize && (tag[i] == '"' || tag[i] == '\'')) {
               char q = tag[i++];
               size_t val_start = i;
               while (i < tagsize && tag[i] != q) i++;
               a_val.assign(tag + val_start, i - val_start);
               if (i < tagsize && tag[i] == q) i++;
            } else {
               size_t val_start = i;
               while (i < tagsize && !isspace((unsigned char)tag[i]) && tag[i] != '>') i++;
               a_val.assign(tag + val_start, i - val_start);
            }
         }
         elem->set_attribute(a_name, a_val);

         /* Inline Event Attributes */
         if (a_name.size() > 2 && tolower((unsigned char)a_name[0]) == 'o' && tolower((unsigned char)a_name[1]) == 'n') {
            std::string lower_on = a_name;
            for (size_t c = 0; c < lower_on.size(); c++) lower_on[c] = tolower((unsigned char)lower_on[c]);
            std::string fn_code = "(function(event) {\n" + a_val + "\n})";
            JSValue fn_val = JS_Eval(dctx->ctx, fn_code.c_str(), fn_code.size(), "<inline>", JS_EVAL_TYPE_GLOBAL);
            if (!JS_IsException(fn_val)) {
               elem->on_handlers[lower_on] = fn_val;
               if (elem->tag_name == "BODY" && lower_on == "onload") {
                  dctx->window_on_handlers["onload"] = JS_DupValue(dctx->ctx, fn_val);
               }
            } else {
               JS_FreeValue(dctx->ctx, fn_val);
            }
         }
      }
   }

   /* Register element in context maps */
   dctx->all_elements.push_back(elem);
   if (!elem->id.empty()) dctx->id_map[elem->id] = elem;
   if (v_form) dctx->form_map[v_form] = elem;
   if (v_input) dctx->input_map[v_input] = elem;
   if (link_idx >= 0) dctx->link_map[link_idx] = elem;

   /* Link into DOM tree */
   if (dctx->current_parent) {
      elem->parent = dctx->current_parent;
      dctx->current_parent->children.push_back(elem);
   } else if (!dctx->root_element) {
      dctx->root_element = elem;
   }

   if (elem->tag_name == "BODY") dctx->body_element = elem;
   else if (elem->tag_name == "HEAD") dctx->head_element = elem;

   /* Container tags become current parent */
   if (elem->tag_name != "IMG" && elem->tag_name != "INPUT" &&
       elem->tag_name != "BR" && elem->tag_name != "HR" && elem->tag_name != "META") {
      dctx->current_parent = elem;
   }
}

void a_Dillo_JS_on_tag_close(void *v_ctx, const char *tag_name)
{
   if (!v_ctx || !tag_name) return;
   DilloJSContext *dctx = (DilloJSContext *)v_ctx;
   if (dctx->current_parent && strcasecmp(dctx->current_parent->tag_name.c_str(), tag_name) == 0) {
      dctx->current_parent = dctx->current_parent->parent;
   }
}

void a_Dillo_JS_on_text(void *v_ctx, const char *text, int len)
{
   if (!v_ctx || !text || len <= 0) return;
   DilloJSContext *dctx = (DilloJSContext *)v_ctx;
   if (dctx->current_parent) {
      if (text[0] == ' ' && len == 1) {
         if (!dctx->current_parent->text_content.empty() &&
             dctx->current_parent->text_content[dctx->current_parent->text_content.size() - 1] != ' ') {
            dctx->current_parent->text_content += ' ';
         }
      } else {
         dctx->current_parent->text_content.append(text, len);
      }
   }
}

/* -------------------------------------------------------------------------
 * Event Dispatch Hooks
 * ------------------------------------------------------------------------- */

int a_Dillo_JS_dispatch_page_load(void *v_ctx)
{
   if (!v_ctx) return 1;
   DilloJSContext *dctx = (DilloJSContext *)v_ctx;
   JSContext *ctx = dctx->ctx;

   /* 1. DOMContentLoaded on document */
   JSValue dcl = js_create_event_object(ctx, "DOMContentLoaded", true, false);
   js_dispatch_event_internal(dctx, NULL, dctx->document_obj, dcl);
   JS_FreeValue(ctx, dcl);

   /* 2. load on window */
   JSValue load_ev = js_create_event_object(ctx, "load", false, false);
   js_dispatch_event_internal(dctx, NULL, dctx->window_obj, load_ev);
   JS_FreeValue(ctx, load_ev);

   return 1;
}

int a_Dillo_JS_dispatch_form_submit(void *v_ctx, void *v_form)
{
   if (!v_ctx || !v_form) return 1;
   DilloJSContext *dctx = (DilloJSContext *)v_ctx;
   if (!dctx->form_map.count(v_form)) return 1;

   DilloDOMElement *elem = dctx->form_map[v_form];
   JSValue form_obj = js_get_or_create_element_object(dctx->ctx, elem);
   JSValue submit_ev = js_create_event_object(dctx->ctx, "submit", true, true);

   bool ok = js_dispatch_event_internal(dctx, elem, form_obj, submit_ev);

   JS_FreeValue(dctx->ctx, submit_ev);
   JS_FreeValue(dctx->ctx, form_obj);
   return ok ? 1 : 0;
}

int a_Dillo_JS_dispatch_form_reset(void *v_ctx, void *v_form)
{
   if (!v_ctx || !v_form) return 1;
   DilloJSContext *dctx = (DilloJSContext *)v_ctx;
   if (!dctx->form_map.count(v_form)) return 1;

   DilloDOMElement *elem = dctx->form_map[v_form];
   JSValue form_obj = js_get_or_create_element_object(dctx->ctx, elem);
   JSValue reset_ev = js_create_event_object(dctx->ctx, "reset", true, true);

   bool ok = js_dispatch_event_internal(dctx, elem, form_obj, reset_ev);

   JS_FreeValue(dctx->ctx, reset_ev);
   JS_FreeValue(dctx->ctx, form_obj);
   return ok ? 1 : 0;
}

int a_Dillo_JS_dispatch_input_click(void *v_ctx, void *v_input)
{
   if (!v_ctx || !v_input) return 1;
   DilloJSContext *dctx = (DilloJSContext *)v_ctx;
   if (!dctx->input_map.count(v_input)) return 1;

   DilloDOMElement *elem = dctx->input_map[v_input];
   JSValue inp_obj = js_get_or_create_element_object(dctx->ctx, elem);
   JSValue click_ev = js_create_event_object(dctx->ctx, "click", true, true);

   bool ok = js_dispatch_event_internal(dctx, elem, inp_obj, click_ev);

   JS_FreeValue(dctx->ctx, click_ev);
   JS_FreeValue(dctx->ctx, inp_obj);
   return ok ? 1 : 0;
}

int a_Dillo_JS_dispatch_input_change(void *v_ctx, void *v_input)
{
   if (!v_ctx || !v_input) return 1;
   DilloJSContext *dctx = (DilloJSContext *)v_ctx;
   if (!dctx->input_map.count(v_input)) return 1;

   DilloDOMElement *elem = dctx->input_map[v_input];
   JSValue inp_obj = js_get_or_create_element_object(dctx->ctx, elem);
   JSValue change_ev = js_create_event_object(dctx->ctx, "change", true, false);

   js_dispatch_event_internal(dctx, elem, inp_obj, change_ev);

   JS_FreeValue(dctx->ctx, change_ev);
   JS_FreeValue(dctx->ctx, inp_obj);
   return 1;
}

int a_Dillo_JS_dispatch_link_click(void *v_ctx, int link_idx)
{
   if (!v_ctx || link_idx < 0) return 1;
   DilloJSContext *dctx = (DilloJSContext *)v_ctx;
   if (!dctx->link_map.count(link_idx)) return 1;

   DilloDOMElement *elem = dctx->link_map[link_idx];
   JSValue link_obj = js_get_or_create_element_object(dctx->ctx, elem);
   JSValue click_ev = js_create_event_object(dctx->ctx, "click", true, true);

   bool ok = js_dispatch_event_internal(dctx, elem, link_obj, click_ev);

   JS_FreeValue(dctx->ctx, click_ev);
   JS_FreeValue(dctx->ctx, link_obj);
   return ok ? 1 : 0;
}

#else /* !ENABLE_JS */

/* Stubs when JavaScript support is compiled out */
void a_Dillo_JS_init(void) {}
void a_Dillo_JS_cleanup(void) {}
void *a_Dillo_JS_context_new(void *v_html, const char *url_str) {
   (void)v_html;
   (void)url_str;
   return NULL;
}
void a_Dillo_JS_context_free(void *v_ctx) {
   (void)v_ctx;
}
void a_Dillo_JS_eval(void *v_ctx, const char *code, size_t len, const char *filename) {
   (void)v_ctx;
   (void)code;
   (void)len;
   (void)filename;
}
void a_Dillo_JS_on_tag_open(void *v_ctx, const char *tag_name, const char *tag, int tagsize,
                            void *v_form, void *v_input, int link_idx, int img_idx) {
   (void)v_ctx;
   (void)tag_name;
   (void)tag;
   (void)tagsize;
   (void)v_form;
   (void)v_input;
   (void)link_idx;
   (void)img_idx;
}
void a_Dillo_JS_on_tag_close(void *v_ctx, const char *tag_name) {
   (void)v_ctx;
   (void)tag_name;
}
void a_Dillo_JS_on_text(void *v_ctx, const char *text, int len) {
   (void)v_ctx;
   (void)text;
   (void)len;
}
int a_Dillo_JS_dispatch_page_load(void *v_ctx) {
   (void)v_ctx;
   return 1;
}
int a_Dillo_JS_dispatch_form_submit(void *v_ctx, void *v_form) {
   (void)v_ctx;
   (void)v_form;
   return 1;
}
int a_Dillo_JS_dispatch_form_reset(void *v_ctx, void *v_form) {
   (void)v_ctx;
   (void)v_form;
   return 1;
}
int a_Dillo_JS_dispatch_input_click(void *v_ctx, void *v_input) {
   (void)v_ctx;
   (void)v_input;
   return 1;
}
int a_Dillo_JS_dispatch_input_change(void *v_ctx, void *v_input) {
   (void)v_ctx;
   (void)v_input;
   return 1;
}
int a_Dillo_JS_dispatch_link_click(void *v_ctx, int link_idx) {
   (void)v_ctx;
   (void)link_idx;
   return 1;
}
void a_Dillo_JS_register_httponly_cookie(const char *host, const char *cookie_name) {
   (void)host;
   (void)cookie_name;
}

#endif /* ENABLE_JS */
