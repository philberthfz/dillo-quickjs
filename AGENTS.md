# AGENTS.md: Developer & Agent Guide for `dillo-quickjs`

Welcome to `dillo-quickjs`. This file serves as an authoritative guide for AI agents and human contributors working on this codebase. It documents the repository architecture, origins, subsystem implementations, development workflows, and coding rules.

---

## 1. Project Overview & Identity

* **Repository**: [`philberthfz/dillo-quickjs`](https://github.com/philberthfz/dillo-quickjs)
* **Description**: A standalone fork of the [Dillo web browser](https://dillo-browser.org/) that embeds Fabrice Bellard's [QuickJS](https://bellard.org/quickjs/) ES2023 engine, providing client-side scripting, DOM/BOM manipulation, timers, storage, and networking under strict opt-in user control.
* **Sister Project**: [Progman](https://github.com/philberthfz/progman) (a Windows 3.1-style desktop environment and window manager). In Progman, Dillo serves as the optional **Tier 3 (lightweight)** backend for Internet Explorer (`iexplore`) embedded via XEmbed and controlled via socket IPC.

### Git Remotes Policy
This repository maintains two distinct remotes:
* **`origin`** (`https://github.com/philberthfz/dillo-quickjs.git`):
  Your primary working and publishing remote on GitHub. New features, documentation, and QuickJS improvements go here.
* **`upstream`** (`https://git.dillo-browser.org/dillo`):
  Official upstream Dillo git repository. Allows tracking new upstream releases, pulling stability patches, or preparing patches to submit to upstream maintainers.

> [!IMPORTANT]
> **Separation of Upstream Fixes vs. QuickJS Features**:
> When making changes that are generic Dillo bugfixes (such as platform portability, parser fixes, or FLTK compatibility), keep them in distinct, atomic commits isolated from the QuickJS subsystems. This ensures they can be cleanly cherry-picked or exported with `git format-patch` for upstream contribution.

---

## 2. Key Subsystems & File Layout

### A. QuickJS Engine Integration & DOM Bridge
All JavaScript execution, DOM/BOM bridges, and event loops are implemented in:
* **`src/dillo_js.hh`**: Public C/C++ interface for the JavaScript subsystem.
  - `a_Dillo_JS_init()` / `a_Dillo_JS_cleanup()`: Lifecycle management for global `JSRuntime`.
  - `a_Dillo_JS_context_new()` / `a_Dillo_JS_context_free()`: Per-document `JSContext` isolation.
  - Event dispatchers: `a_Dillo_JS_dispatch_event()`, `a_Dillo_JS_eval_inline()`.
* **`src/dillo_js.cc`**: Complete QuickJS integration implementation (~4,300 lines):
  - **BOM Dialogs**: Bridges `window.alert()`, `window.confirm()`, and `window.prompt()` directly to native FLTK modal dialogs (`fl_alert`, `fl_choice`, `fl_input`).
  - **BOM Timers**: Implements `setTimeout()`, `setInterval()`, `clearTimeout()`, and `clearInterval()` scheduled via FLTK timer callbacks (`Fl::add_timeout()`, `Fl::repeat_timeout()`).
  - **BOM Console & Navigator**: `console.log/info/warn/error`, `navigator.userAgent`, `navigator.appName`, `navigator.platform`.
  - **BOM Location & Navigation**: `location.href` getter/setter, `location.reload()`, `replace()`, and `assign()`.
  - **DOM Core**: Bindings for `document.title`, `document.getElementById()`, `document.querySelector()`, and `document.querySelectorAll()`.
  - **DOM Forms & Controls**: Direct bindings for `document.forms[]`, input fields (`value`, `checked`, `type`), `form.submit()`, and `form.reset()`.
  - **Event Subsystem**: Event bubbling, inline handlers (`onclick`, `onsubmit`, `onchange`), `addEventListener()`, `removeEventListener()`, and `event.preventDefault()`.
  - **Web Storage**: `window.localStorage` and `window.sessionStorage` with domain-based persistence in `~/.dillo/storage/<origin>.json` (gated by `~/.dillo/storagerc`).
  - **Document Cookies**: Restricted `document.cookie` bridge synchronized with Dillo's native cookie jar (`a_Cookies_set` / `a_Cookies_get_query`) with strict Same-Origin and `HttpOnly` protection.
  - **Networking**: Lightweight `fetch()` and `XMLHttpRequest` over Dillo's cache and libcurl.

### B. Core Browser Hooks
* **`src/dillo.cc`**: Initializes `a_Dillo_JS_init()` at startup and cleans up with `a_Dillo_JS_cleanup()`.
* **`src/html.cc` / `src/html.hh`**: Hooks into the HTML parser to instantiate `JSContext` on document creation, parses `<script>` tags, and binds inline event attributes (`onclick`, `onsubmit`, etc.).
* **`src/form.cc` / `src/form.hh`**: Provides hooks for JavaScript access to form widgets and input values.
* **`src/cookies.c` / `src/cookies.h`**: Cookie management and security policies (`~/.dillo/cookiesrc`).
* **`src/prefs.c` / `src/prefs.h` / `src/prefsparser.cc` / `dillorc`**: Configuration flags (`enable_js`, `js_storage`, `js_document_cookie`).

### C. Portability & Upstream Fixes
* **`dlib/dlib.h`**: Explicitly handles `EOF` (`-1`) in character classification functions (`dIsalnum`, `dIsalpha`, `dIsascii`, etc.) so it never casts to unsigned char `255` (preventing infinite loops and `SIGBUS` crashes on macOS/BSD).
* **`src/cssparser.cc`**: Adds bounds checks to `CssParser::ungetChar` and checks for `EOF` in token streaming.
* **`src/version.cc`**: Undefines conflicting `zlibVersion` macros.
* **`src/xembed.cc`**: Permits XEmbed support on macOS when targeting X11 backends.

### D. Remote Control Socket IPC
* **`src/IO/control.c`**: Implements Dillo's UNIX domain socket control interface (`~/.dillo/ctl/<pid>`):
  - Supported commands: `open <url>`, `back`, `forward`, `reload`, `stop`, `home`, and `ready`.
  - Allows an external wrapper shell (like Progman's `iexplore`) to control navigation without process restarts or window repainting flickers.

---

## 3. Build & Development Instructions

### Dependencies
* **C/C++ Compiler**: GCC or Clang supporting C23 / C++17.
* **GUI Toolkit**: FLTK 1.3 or FLTK 1.4.
* **JavaScript Engine**: QuickJS (`libquickjs`).
* **Libraries**: OpenSSL, libcurl, libpng, libjpeg, libwebp, zlib, libiconv.

### Standard Build (Linux)
```bash
./autogen.sh
./configure --prefix=/usr/local --enable-tls --enable-cookies --enable-ipv6
make -j$(nproc)
sudo make install
```

### macOS Build (Homebrew with X11 FLTK)
```bash
# If using Homebrew dependencies:
export OPENSSL_PREFIX="$(brew --prefix openssl@3 2>/dev/null || echo /opt/homebrew/opt/openssl@3)"
export QUICKJS_PREFIX="$(brew --prefix quickjs 2>/dev/null || echo /opt/homebrew/opt/quickjs)"

# Point to X11 FLTK build if building for X11:
export PATH="$HOME/.cache/progman/dillo-build/fltk-x11-dist/bin:$PATH"

CPPFLAGS="-I$OPENSSL_PREFIX/include" \
LDFLAGS="-L$OPENSSL_PREFIX/lib" \
./configure --prefix="$HOME/.local" \
    --enable-tls \
    --enable-cookies \
    --enable-ipv6 \
    --enable-experimental-fltk

make -j$(sysctl -n hw.ncpu)
```

> [!NOTE]
> `configure.ac` automatically inspects `/opt/homebrew/opt/quickjs`, `/usr/local`, and `/usr` for QuickJS headers (`quickjs.h`) and libraries (`libquickjs.a` / `.so`), so explicit QuickJS flags are generally unnecessary.

---

## 4. Testing & Verification

1. **Check Binary Version & Enabled Features**:
   ```bash
   ./src/dillo -v
   # Output should indicate: +XEMBED +TLS +IPV6 +CTL
   ```
2. **Verify QuickJS Symbols**:
   ```bash
   nm ./src/dillo | grep JS_NewRuntime
   ```
3. **Run DOM & Event Test**:
   ```bash
   # Enable JS with -j or configure enable_js=YES in ~/.dillorc
   ./src/dillo -j /path/to/dillo-js-test.html
   ```
4. **Test Remote Control IPC**:
   ```bash
   ./src/dillo &
   DILLO_PID=$!
   # Wait for socket in ~/.dillo/ctl/<pid>
   echo "open https://example.com" | nc -U ~/.dillo/ctl/$DILLO_PID
   echo "reload" | nc -U ~/.dillo/ctl/$DILLO_PID
   ```

---

## 5. Coding Conventions & Memory Safety Rules

### QuickJS Memory Management
QuickJS uses reference counting for `JSValue`. Any agent modifying `src/dillo_js.cc` **must** adhere strictly to:
* **Duplicate References**: Call `JS_DupValue(ctx, val)` when keeping a reference or sharing values across calls.
* **Free References**: Call `JS_FreeValue(ctx, val)` on every created, returned, or duplicated `JSValue` once it is no longer needed.
* **Exception Handling**: Always test with `JS_IsException(val)`. When an exception occurs, retrieve the exception using `JS_GetException(ctx)` and free both values.
* **Context Teardown**: When a document is torn down or navigated (`a_Dillo_JS_context_free`), clear all pending FLTK timer callbacks associated with that context immediately to prevent callbacks from firing on freed memory.

### Security & Opt-in Policy
* JavaScript execution **must default to disabled** (`enable_js=NO` in `dillorc`).
* Document cookies must respect Same-Origin and drop `HttpOnly` cookies from script access.
* Local storage writes must be gated by `storagerc` permissions.

---

## 6. History & Provenance

* **Original Upstream**: Dillo Web Browser developed by Jorge Arellano Cid and the Dillo community ([dillo-browser.org](https://dillo-browser.org/)).
* **September 2026**: Integrated Fabrice Bellard's QuickJS engine and BOM/DOM subsystem into Dillo as part of the Progman Internet Explorer project.
* **October 2026**: Fully severed and established as an independent project at [`philberthfz/dillo-quickjs`](https://github.com/philberthfz/dillo-quickjs) with dedicated build system, automatic QuickJS discovery, and full git history preserved.
