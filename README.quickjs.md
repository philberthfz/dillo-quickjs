# Dillo with QuickJS JavaScript Engine

This repository is an enhanced fork of the [Dillo web browser](https://dillo-browser.org/) that embeds Fabrice Bellard's lightweight [QuickJS](https://bellard.org/quickjs/) JavaScript engine (ES2023).

It brings pragmatic modern client-side scripting to Dillo's ultra-fast, minimal-footprint architecture while preserving Dillo's core values of speed, efficiency, and user privacy.

---

## Key Features

### 1. Embedded QuickJS Runtime (ES2023)
- Context isolation per document lifecycle (`DilloHtml`).
- Full modern ECMAScript standard support (ES2023) with minimal binary and memory overhead.
- Safe execution with strict opt-in controls.

### 2. Browser Object Model (BOM) & Dialogs
- **Dialogs**: `window.alert()`, `window.confirm()`, and `window.prompt()` bridged directly to native FLTK dialogs.
- **Timers**: `setTimeout()`, `setInterval()`, `clearTimeout()`, and `clearInterval()` scheduled via FLTK event loops.
- **Console**: `console.log()`, `console.info()`, `console.warn()`, and `console.error()`.
- **Location & Navigation**: `location.href` (getter/setter), `location.reload()`, `location.replace()`, and `location.assign()`.
- **Navigator**: `navigator.userAgent` and standard platform attributes.

### 3. Document Object Model (DOM) & Forms
- DOM querying: `document.getElementById()`, `document.querySelector()`, and `document.querySelectorAll()`.
- Document metadata: `document.title` dynamic read/write.
- Form controls: `document.forms[]`, input field access (`value`, `checked`), programmatic `submit()`, and `reset()`.
- Event handling: Inline handlers (`onclick`, `onsubmit`, `onchange`), `addEventListener()`, `removeEventListener()`, and `event.preventDefault()`.
- Style manipulation: Element attributes (`setAttribute`, `getAttribute`, `classList`) and inline style mutators.

### 4. Networking & Storage
- **Fetch & XHR**: Lightweight `fetch()` and `XMLHttpRequest` implementations layered on Dillo's internal HTTP cache / libcurl.
- **Web Storage**: `window.localStorage` and `window.sessionStorage` with configurable persistence in `~/.dillo/storage/`.
- **Cookies**: Origin-restricted `document.cookie` bridge synchronized with Dillo's native cookie jar and security policies in `~/.dillo/cookiesrc`.

---

## Configuration (`~/.dillorc`)

JavaScript execution is controlled via options in `~/.dillorc`:

```ini
# Enable JavaScript execution (QuickJS)
# Default is NO (strictly opt-in for backward compatibility and security)
enable_js=YES

# Web Storage persistence (localStorage / sessionStorage)
# NO: sessionStorage and localStorage disabled
# MEMORY: stored in RAM for current session only
# PERMIT: persistent storage for domains listed in ~/.dillo/storagerc
js_storage=PERMIT

# Document cookie scripting access
# NO: document.cookie returns empty string and writes are no-ops
# RESTRICTED: enables same-origin cookies with HttpOnly protections
js_document_cookie=RESTRICTED
```

---

## Building from Source

### Prerequisites
- FLTK 1.3 or FLTK 1.4
- QuickJS (`brew install quickjs` on macOS, or `libquickjs-dev` on Debian/Ubuntu)
- OpenSSL (optional, for TLS/HTTPS)
- libcurl (for network fetch)
- libpng, libjpeg, libwebp, zlib

### Build Instructions

```bash
./autogen.sh
./configure --prefix=/usr/local \
    --enable-tls \
    --enable-cookies \
    --enable-ipv6

make -j$(nproc)
sudo make install
```

On macOS with Homebrew:
```bash
export OPENSSL_PREFIX="$(brew --prefix openssl@3)"
export QUICKJS_PREFIX="$(brew --prefix quickjs)"

CPPFLAGS="-I$OPENSSL_PREFIX/include -I$QUICKJS_PREFIX/include" \
LDFLAGS="-L$OPENSSL_PREFIX/lib -L$QUICKJS_PREFIX/lib/quickjs -L$QUICKJS_PREFIX/lib" \
./configure --prefix=/usr/local --enable-tls --enable-cookies --enable-ipv6 --enable-experimental-fltk

make -j$(sysctl -n hw.ncpu)
sudo make install
```

---

## Upstream Compatibility & Bugfixes Included
This repository also incorporates several portability and robustness improvements over upstream Dillo:
- **macOS EOF Handling**: Prevents `EOF` (`-1`) from being cast to unsigned char in `dlib/dlib.h`, avoiding infinite loops and crashes in character classification macros.
- **CSS Parser Safety**: Bounds-checking on CSS token streams in `src/cssparser.cc`.
- **Remote Control IPC**: Extended command set for `dillo` control socket (`src/IO/control.c`) adding `back`, `forward`, `reload`, `stop`, and `home`.
- **XEmbed Portability**: Cleaned XEmbed platform guards in `src/xembed.cc`.
