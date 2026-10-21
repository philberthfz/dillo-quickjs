/*
 * File: dillo_js.hh
 *
 * Lightweight QuickJS Embedded JavaScript Engine Integration for Dillo.
 */

#ifndef __DILLO_JS_HH__
#define __DILLO_JS_HH__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize global JS runtime state */
void a_Dillo_JS_init(void);

/* Free global JS runtime state */
void a_Dillo_JS_cleanup(void);

/* Create a JS execution context for a DilloHtml document instance */
void *a_Dillo_JS_context_new(void *v_html, const char *url_str);

/* Free JS execution context and cancel any pending timers */
void a_Dillo_JS_context_free(void *v_ctx);

/* Evaluate JavaScript source code */
void a_Dillo_JS_eval(void *v_ctx, const char *code, size_t len, const char *filename);

/* Tag parser hooks */
void a_Dillo_JS_on_tag_open(void *v_ctx, const char *tag_name, const char *tag, int tagsize,
                            void *v_form, void *v_input, int link_idx, int img_idx);
void a_Dillo_JS_on_tag_close(void *v_ctx, const char *tag_name);
void a_Dillo_JS_on_text(void *v_ctx, const char *text, int len);

/* Event dispatch hooks (return 1 to proceed with default browser action, 0 to cancel) */
int a_Dillo_JS_dispatch_page_load(void *v_ctx);
int a_Dillo_JS_dispatch_form_submit(void *v_ctx, void *v_form);
int a_Dillo_JS_dispatch_form_reset(void *v_ctx, void *v_form);
int a_Dillo_JS_dispatch_input_click(void *v_ctx, void *v_input);
int a_Dillo_JS_dispatch_input_change(void *v_ctx, void *v_input);
int a_Dillo_JS_dispatch_link_click(void *v_ctx, int link_idx);

/* Cookie HttpOnly registration hook */
void a_Dillo_JS_register_httponly_cookie(const char *host, const char *cookie_name);

#ifdef __cplusplus
}
#endif

#endif /* __DILLO_JS_HH__ */
