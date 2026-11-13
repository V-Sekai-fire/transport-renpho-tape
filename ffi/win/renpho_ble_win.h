// C ABI between the Lean-facing stub (ble_ffi_win.c, MinGW) and the WinRT
// implementation (renpho_ble_win.cpp, MSVC DLL). Plain integers and bytes only,
// so the two sides share no C++ or CRT state. Every function returns 0 on
// success and fills `err` (NUL-terminated UTF-8, `errcap` bytes) on failure.

#ifndef RENPHO_BLE_WIN_H
#define RENPHO_BLE_WIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rb_scan_cb)(void *ctx, const char *id, const char *name, int rssi,
                           const char *services_csv, const uint8_t *mfg, size_t mfg_len);
typedef void (*rb_char_cb)(void *ctx, const char *service, const char *uuid, uint32_t props);

int rb_init(uint32_t timeout_ms, char *err, size_t errcap);
int rb_scan(uint32_t ms, rb_scan_cb cb, void *ctx, char *err, size_t errcap);
int rb_connect(const char *id, uint32_t timeout_ms, char *err, size_t errcap);
int rb_discover(uint32_t timeout_ms, rb_char_cb cb, void *ctx, char *err, size_t errcap);
int rb_subscribe(const char *service, const char *uuid, uint32_t timeout_ms, char *err, size_t errcap);
int rb_write(const char *service, const char *uuid, const uint8_t *data, size_t len,
             int with_response, uint32_t timeout_ms, char *err, size_t errcap);
int rb_read(const char *service, const char *uuid, uint32_t timeout_ms, uint8_t *buf, size_t cap,
            size_t *len, char *err, size_t errcap);
// 1 = notification delivered, 0 = timeout, -1 = link down and queue empty.
int rb_next(uint32_t timeout_ms, char *uuid, size_t uuidcap, uint8_t *buf, size_t cap, size_t *len,
            char *err, size_t errcap);
int rb_disconnect(void);

#ifdef __cplusplus
}
#endif
#endif
