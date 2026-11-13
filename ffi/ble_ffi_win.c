// Lean-facing BLE shim for Windows (MinGW gcc).
//
// Same C ABI as ble_ffi.m. The WinRT work lives in renpho_ble_win.dll (MSVC,
// C++/WinRT); this file loads it next to the executable and adapts its plain-C
// functions to Lean objects. Lean objects are built only on the calling Lean
// thread: the DLL hands data to callbacks that run on that same thread.

#include <lean/lean.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "win/renpho_ble_win.h"

#define ERRCAP 256

static HMODULE dll = NULL;

#define FN(name) static __typeof__(&name) p_##name
FN(rb_init);
FN(rb_scan);
FN(rb_connect);
FN(rb_discover);
FN(rb_subscribe);
FN(rb_write);
FN(rb_read);
FN(rb_next);
FN(rb_disconnect);

static lean_obj_res io_err(const char *msg) {
  return lean_io_result_mk_error(lean_mk_io_user_error(lean_mk_string(msg)));
}
static lean_obj_res io_unit(void) { return lean_io_result_mk_ok(lean_box(0)); }

#define LOAD(name)                                                       \
  p_##name = (__typeof__(&name))(void *)GetProcAddress(dll, #name);      \
  if (!p_##name) return io_err("renpho_ble_win.dll is missing " #name)

static lean_obj_res load_dll(void) {
  if (dll) return NULL;
  char path[MAX_PATH];
  DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
  while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') n--;
  path[n] = 0;
  strncat(path, "renpho_ble_win.dll", sizeof path - strlen(path) - 1);
  dll = LoadLibraryA(path);
  if (!dll) {
    char m[MAX_PATH + 96];
    snprintf(m, sizeof m, "cannot load %s (keep renpho_ble_win.dll beside the executable)", path);
    return io_err(m);
  }
  LOAD(rb_init);
  LOAD(rb_scan);
  LOAD(rb_connect);
  LOAD(rb_discover);
  LOAD(rb_subscribe);
  LOAD(rb_write);
  LOAD(rb_read);
  LOAD(rb_next);
  LOAD(rb_disconnect);
  return NULL;
}

#define REQUIRE_DLL()                  \
  if (!dll) return io_err("BLE not initialised: call Ble.init first")

lean_obj_res lean_ble_init(uint32_t timeout_ms, lean_obj_arg w) {
  lean_obj_res e = load_dll();
  if (e) return e;
  char err[ERRCAP] = "";
  if (p_rb_init(timeout_ms, err, sizeof err)) return io_err(err);
  return io_unit();
}

typedef struct {
  lean_object *out;
} ScanCtx;

static void scan_cb(void *ctx, const char *id, const char *name, int rssi, const char *csv,
                    const uint8_t *mfg, size_t mfg_len) {
  ScanCtx *c = ctx;
  lean_object *svcs = lean_mk_empty_array();
  const char *p = csv;
  while (*p) {
    const char *q = strchr(p, ',');
    size_t n = q ? (size_t)(q - p) : strlen(p);
    char *s = malloc(n + 1);
    memcpy(s, p, n);
    s[n] = 0;
    svcs = lean_array_push(svcs, lean_mk_string(s));
    free(s);
    p = q ? q + 1 : p + n;
  }
  lean_object *m = lean_alloc_sarray(1, mfg_len, mfg_len);
  if (mfg_len) memcpy(lean_sarray_cptr(m), mfg, mfg_len);
  lean_object *o = lean_alloc_ctor(0, 5, 0);
  lean_ctor_set(o, 0, lean_mk_string(id));
  lean_ctor_set(o, 1, lean_mk_string(name));
  lean_ctor_set(o, 2, lean_int64_to_int(rssi));
  lean_ctor_set(o, 3, svcs);
  lean_ctor_set(o, 4, m);
  c->out = lean_array_push(c->out, o);
}

lean_obj_res lean_ble_scan(uint32_t ms, lean_obj_arg w) {
  REQUIRE_DLL();
  ScanCtx c = {lean_mk_empty_array()};
  char err[ERRCAP] = "";
  if (p_rb_scan(ms, scan_cb, &c, err, sizeof err)) {
    lean_dec(c.out);
    return io_err(err);
  }
  return lean_io_result_mk_ok(c.out);
}

lean_obj_res lean_ble_connect(b_lean_obj_arg id, uint32_t timeout_ms, lean_obj_arg w) {
  REQUIRE_DLL();
  char err[ERRCAP] = "";
  if (p_rb_connect(lean_string_cstr(id), timeout_ms, err, sizeof err)) return io_err(err);
  return io_unit();
}

static void char_cb(void *ctx, const char *service, const char *uuid, uint32_t props) {
  lean_object **out = ctx;
  lean_object *o = lean_alloc_ctor(0, 3, 0);
  lean_ctor_set(o, 0, lean_mk_string(service));
  lean_ctor_set(o, 1, lean_mk_string(uuid));
  lean_ctor_set(o, 2, lean_box(props));
  *out = lean_array_push(*out, o);
}

lean_obj_res lean_ble_discover(uint32_t timeout_ms, lean_obj_arg w) {
  REQUIRE_DLL();
  lean_object *out = lean_mk_empty_array();
  char err[ERRCAP] = "";
  if (p_rb_discover(timeout_ms, char_cb, &out, err, sizeof err)) {
    lean_dec(out);
    return io_err(err);
  }
  return lean_io_result_mk_ok(out);
}

lean_obj_res lean_ble_subscribe(b_lean_obj_arg svc, b_lean_obj_arg chr, uint32_t timeout_ms,
                                lean_obj_arg w) {
  REQUIRE_DLL();
  char err[ERRCAP] = "";
  if (p_rb_subscribe(lean_string_cstr(svc), lean_string_cstr(chr), timeout_ms, err, sizeof err))
    return io_err(err);
  return io_unit();
}

lean_obj_res lean_ble_write(b_lean_obj_arg svc, b_lean_obj_arg chr, b_lean_obj_arg data,
                            uint8_t with_response, uint32_t timeout_ms, lean_obj_arg w) {
  REQUIRE_DLL();
  char err[ERRCAP] = "";
  if (p_rb_write(lean_string_cstr(svc), lean_string_cstr(chr), lean_sarray_cptr(data),
                 lean_sarray_size(data), with_response, timeout_ms, err, sizeof err))
    return io_err(err);
  return io_unit();
}

lean_obj_res lean_ble_read(b_lean_obj_arg svc, b_lean_obj_arg chr, uint32_t timeout_ms,
                           lean_obj_arg w) {
  REQUIRE_DLL();
  uint8_t buf[512];
  size_t len = 0;
  char err[ERRCAP] = "";
  if (p_rb_read(lean_string_cstr(svc), lean_string_cstr(chr), timeout_ms, buf, sizeof buf, &len, err,
                sizeof err))
    return io_err(err);
  lean_object *a = lean_alloc_sarray(1, len, len);
  if (len) memcpy(lean_sarray_cptr(a), buf, len);
  return lean_io_result_mk_ok(a);
}

lean_obj_res lean_ble_next_notification(uint32_t timeout_ms, lean_obj_arg w) {
  REQUIRE_DLL();
  char uuid[64];
  uint8_t buf[512];
  size_t len = 0;
  char err[ERRCAP] = "";
  int r = p_rb_next(timeout_ms, uuid, sizeof uuid, buf, sizeof buf, &len, err, sizeof err);
  if (r < 0) return io_err(err);
  if (r == 0) return lean_io_result_mk_ok(lean_box(0));
  lean_object *a = lean_alloc_sarray(1, len, len);
  if (len) memcpy(lean_sarray_cptr(a), buf, len);
  lean_object *pair = lean_alloc_ctor(0, 2, 0);
  lean_ctor_set(pair, 0, lean_mk_string(uuid));
  lean_ctor_set(pair, 1, a);
  lean_object *some = lean_alloc_ctor(1, 1, 0);
  lean_ctor_set(some, 0, pair);
  return lean_io_result_mk_ok(some);
}

lean_obj_res lean_ble_disconnect(lean_obj_arg w) {
  if (dll) p_rb_disconnect();
  return io_unit();
}
