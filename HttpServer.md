# HttpServer Plugin API (v22, server only)

The `IPluginHttpServer` interface lets plugins serve HTTP content directly through the game
server's built-in HTTP listener. It is available from interface version 22 onwards and is
server-only -- `hooks->HttpServer` is `nullptr` on client and generic builds.

---

## Table of Contents

- [Overview](#overview)
- [URL Scheme](#url-scheme)
- [Request Processing Order](#request-processing-order)
- [MIME Type Support](#mime-type-support)
- [Static File Routes](#static-file-routes)
- [Raw Response Routes (JSON APIs)](#raw-response-routes-json-apis)
- [Raw Request Filters](#raw-request-filters)
- [Thread Safety](#thread-safety)
- [PluginHttpRequest Reference](#pluginhttprequest-reference)
- [PluginHttpResponse Reference](#pluginhttpresponse-reference)
- [Full Example Plugin](#full-example-plugin)
- [Common Mistakes](#common-mistakes)

---

## Overview

Every incoming HTTP request is routed through a three-stage pipeline:

```
Request
|
  v
[1] Raw-request filters    --- first Deny -> 403, stop
  |
  v
[2] Raw-response routes    --- first prefix match -> plugin callback, stop
  |
  v
[3] Static-file routes     --- first prefix match -> file served from disk, stop
  |
  v
[4] Engine pass-through    --- original engine handler (404 for unknown paths)
```

---

## URL Scheme

All plugin-owned URLs follow this pattern (always case-insensitive):

```
/<pluginName>/<routeName>/[optional/sub/path]
```

- `<pluginName>` -- the exact value of `self->name` as returned by `GetPluginInfo()`.
- `<routeName>`  -- the `folderName` (static) or `urlPrefix` (raw route) passed at registration.

Examples for a plugin named `MyPlugin`:

| Route type   | Registration call              | Matched URLs       |
|--------------|--------------------------------|---------------------------------------------------|
| Static files | `AddRoute(self, "ui")` | `/myplugin/ui/index.html`, `/myplugin/ui/app.js`  |
| Raw route    | `AddRawRoute(self, "api", cb)` | `/myplugin/api/`, `/myplugin/api/status`         |

URL matching is case-insensitive. The plugin name and route name are lowercased for the
prefix comparison, but the original casing is preserved in `req->url`.

---

## Request Processing Order

1. **Raw-request filters** -- run first, for every request regardless of URL. A filter
   returning `HttpRequestAction::Deny` immediately sends a `403 Forbidden` and stops all
   further processing. Use filters for authentication, rate-limiting, or blanket blocking.

2. **Raw-response routes** -- prefix-matched against the URL. The first match wins; the
   matched plugin callback is called and its response is sent. No other handler runs.

3. **Static-file routes** -- prefix-matched against the URL. The file path is resolved on
   disk, validated to prevent path traversal, and served with an inferred MIME type. For
   directory requests (no file extension in the last path segment) the server probes
   `index.html` then `index.htm` before giving up.

4. **Engine pass-through** -- the original game HTTP handler. Produces `404 Not Found` for
   paths it does not recognise.

---

## MIME Type Support

The following file extensions are recognised automatically for static file routes:

| Extension        | MIME type          |
|--------------------|---------------------------|
| `.html`, `.htm`    | `text/html`|
| `.css`        | `text/css`            |
| `.js`         | `application/javascript`  |
| `.json`  | `application/json`  |
| `.txt`             | `text/plain`       |
| `.xml`  | `text/xml`             |
| `.svg`   | `image/svg+xml`           |
| `.png`  | `image/png`               |
| `.jpg`, `.jpeg`    | `image/jpeg`      |
| `.gif`   | `image/gif`               |
| `.ico`  | `image/x-icon`  |
| `.wasm`    | `application/wasm`        |
| (anything else)    | `application/octet-stream`|

Text types (`text/*`, `application/javascript`, `application/json`, `image/svg+xml`) are
sent through the engine's UTF-16 string path. All other types are sent as raw bytes --
binary files such as PNG and JPEG are served without any encoding conversion and arrive
at the browser intact.

---

## Static File Routes

Serve a folder of files from disk.

### Registration

```cpp
// In PluginInit:
if (self->hooks->HttpServer)
{
    if (self->hooks->HttpServer->AddRoute(self, "ui"))
        LOG_INFO("Static route registered: /%s/ui/", self->name);
    else
        LOG_WARN("Static route registration failed (already registered?)");
}
```

Files are served from:

```
<exe_dir>\Plugins\<pluginName>\<folderName>\
```

For example, if your plugin is `MyPlugin` and your folder is `ui`, the server looks for
files under:

```
StarRupture\Binaries\Win64\Plugins\MyPlugin\ui\
```

### Directory requests

A request whose last URL segment has no file extension is treated as a directory request.
The server probes:

1. `index.html`
2. `index.htm`

If neither exists the engine's 404 handler responds.

### Filename URLs with trailing slashes

The game server normalises all URLs by appending a trailing slash before routing. The
static file handler strips it before path resolution, so a request for `index.html/` is
correctly treated as a request for `index.html`.

### Unregistration

Always call `RemoveRoute` during `PluginShutdown`:

```cpp
// In PluginShutdown:
if (g_self && g_self->hooks->HttpServer)
    g_self->hooks->HttpServer->RemoveRoute(g_self, "ui");
```

---

## Raw Response Routes (JSON APIs)

Serve dynamic content -- JSON, computed HTML, or anything else -- from a callback.

### Registration

```cpp
static void OnApiRequest(const PluginHttpRequest* req, PluginHttpResponse* resp)
{
    static char s_buf[256];
    int len = snprintf(s_buf, sizeof(s_buf), "{\"status\":\"ok\"}");

    resp->statusCode  = 200;
    resp->contentType = "application/json";
    resp->body        = s_buf;
    resp->bodyLen     = (len > 0) ? static_cast<size_t>(len) : 0;
}

// In PluginInit:
if (self->hooks->HttpServer)
{
    if (self->hooks->HttpServer->AddRawRoute(self, "api", OnApiRequest))
        LOG_INFO("Raw route registered: /%s/api/", self->name);
}
```

The callback is invoked for any URL that starts with `/<pluginName>/<urlPrefix>/`
(case-insensitive). The full original URL is available in `req->url` so you can dispatch
sub-paths manually.

### Response fields

| Field         | Type         | Description     |
|---------------|----------------|----------------------------------------------------------|
| `statusCode`  | `int`       | HTTP status code. Default `200`.   |
| `contentType` | `const char*`  | MIME type string. Default `"text/plain"`.     |
| `body`        | `const char*`  | Response body bytes. May be `nullptr` if `bodyLen == 0`. |
| `bodyLen`     | `size_t`   | Byte length of `body`. `0` produces an empty body.       |

All pointers in `PluginHttpResponse` must remain valid until the callback returns. The
modloader copies them immediately after. Using a `static` buffer (as shown above) is the
simplest approach.

### Unregistration

```cpp
// In PluginShutdown:
if (g_self && g_self->hooks->HttpServer)
    g_self->hooks->HttpServer->RemoveRawRoute(g_self, "api");
```

---

## Raw Request Filters

Inspect or block every incoming request before any route handler runs.

```cpp
static HttpRequestAction OnFilter(const PluginHttpRequest* req)
{
    // Only block non-GET requests
    if (req->method != HttpMethod::Get)
   return HttpRequestAction::Deny;   // sends 403

    return HttpRequestAction::Approve;    // continue normally
}

// In PluginInit:
if (self->hooks->HttpServer)
    self->hooks->HttpServer->RegisterOnRawRequest(OnFilter);

// In PluginShutdown:
if (g_self && g_self->hooks->HttpServer)
    g_self->hooks->HttpServer->UnregisterOnRawRequest(OnFilter);
```

Filters are global -- they fire for all requests, not just your plugin's routes. Use them
sparingly and return quickly.

---

## Thread Safety

- HTTP callbacks are called from Unreal's **HTTP connection thread** -- the thread that
  FHttpConnection::ProcessRequest runs on. This is entirely separate from the game thread
  that UGameEngine::Tick (and all plugin tick callbacks) run on.
- Do not access UObjects, game state, or any data written by the game thread without
  proper synchronisation.
- The recommended pattern is to write shared state on the game thread under a mutex, and
  read it inside the HTTP callback under the same mutex. This is exactly what
  NetChannelTest does for s_worldName:

```cpp
// Written on the game thread (world-begin-play callback):
static void OnWorldBeginPlay(SDK::UWorld*, const char* worldName)
{
    std::lock_guard lock(s_mutex);   // game thread writer
    strncpy_s(s_worldName, worldName ? worldName : "<unknown>", _TRUNCATE);
}

// Read on the HTTP connection thread (route callback):
static void OnApiRoute(const PluginHttpRequest* req, PluginHttpResponse* resp)
{
    char worldName[256];
    {
        std::lock_guard lock(s_mutex);   // HTTP thread reader
        strncpy_s(worldName, s_worldName, _TRUNCATE);
    }
    // ... build response from worldName ...
}
```

- Plain reads of naturally-aligned 32-bit values (uint32_t, int, float) are atomic on
  x86-64 and do not require a mutex when only one thread writes them and the read does
  not need to be consistent with other values. NetChannelTest uses this for the tick/send
  counters in OnApiRoute, but protects the world name string with a mutex because a
  char[256] copy is not atomic.
- Do not call PostToGameThread from inside an HTTP callback and then block waiting for the
  result -- that will deadlock because the game thread is not blocked waiting for you.
