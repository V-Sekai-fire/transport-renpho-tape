// BlueZ shim for the Lean 4 RENPHO tape client (Linux).
//
// Same C ABI as ble_ffi.m. Talks to bluetoothd over the system D-Bus with GIO.
// A private thread runs a GMainLoop so signals (notifications, disconnects) are
// delivered while a Lean thread blocks inside a call. Callbacks only fill C
// state under `mu`; no Lean object is touched off the calling Lean thread.
//
// The "id" of a peripheral is its Bluetooth address (BlueZ exposes it).

#include <gio/gio.h>
#include <lean/lean.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

typedef struct Note {
  char *uuid;
  GBytes *data;
} Note;

static GDBusConnection *bus = NULL;
static GMainLoop *loop = NULL;
static char *adapter = NULL;     // object path, e.g. /org/bluez/hci0
static char *dev_path = NULL;    // connected peripheral
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static gboolean connected = FALSE;
static GPtrArray *notes = NULL;  // of Note*
static GHashTable *subs = NULL;  // char path -> subscription id
static guint dev_sub = 0;
static char *last_err = NULL;

#define BLUEZ "org.bluez"

// ------------------------------------------------------------ helpers

static lean_obj_res io_err(const char *msg) {
  return lean_io_result_mk_error(lean_mk_io_user_error(lean_mk_string(msg)));
}
static lean_obj_res io_unit(void) { return lean_io_result_mk_ok(lean_box(0)); }

static lean_object *mk_bytes(const void *p, size_t n) {
  lean_object *a = lean_alloc_sarray(1, n, n);
  if (n) memcpy(lean_sarray_cptr(a), p, n);
  return a;
}

// io_err with the GError text; frees the error.
static lean_obj_res gerr(const char *prefix, GError *e) {
  char *m = g_strdup_printf("%s: %s", prefix, e ? e->message : "unknown error");
  lean_obj_res r = io_err(m);
  g_free(m);
  if (e) g_error_free(e);
  return r;
}

static void deadline_in(struct timespec *ts, uint32_t ms) {
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_sec += ms / 1000;
  ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec += 1;
    ts->tv_nsec -= 1000000000L;
  }
}

static void sleep_ms(uint32_t ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static void *loop_main(void *arg) {
  g_main_loop_run(loop);
  return NULL;
}

static GVariant *call(const char *path, const char *iface, const char *method, GVariant *params,
                      int timeout_ms, GError **err) {
  return g_dbus_connection_call_sync(bus, BLUEZ, path, iface, method, params, NULL,
                                     G_DBUS_CALL_FLAGS_NONE, timeout_ms, NULL, err);
}

static GVariant *managed_objects(GError **err) {
  GVariant *r = call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL, 5000, err);
  if (!r) return NULL;
  GVariant *dict = g_variant_get_child_value(r, 0);
  g_variant_unref(r);
  return dict; // a{oa{sa{sv}}}
}

// Property of an interface of a managed object; caller unrefs. NULL if absent.
static GVariant *prop(GVariant *ifaces, const char *iface, const char *name) {
  GVariant *props = g_variant_lookup_value(ifaces, iface, G_VARIANT_TYPE("a{sv}"));
  if (!props) return NULL;
  GVariant *v = g_variant_lookup_value(props, name, NULL);
  g_variant_unref(props);
  return v;
}

static char *prop_str(GVariant *ifaces, const char *iface, const char *name) {
  GVariant *v = prop(ifaces, iface, name);
  if (!v) return NULL;
  char *s = NULL;
  if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ||
      g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH))
    s = g_strdup(g_variant_get_string(v, NULL));
  g_variant_unref(v);
  return s;
}

static gboolean prop_bool(GVariant *ifaces, const char *iface, const char *name) {
  GVariant *v = prop(ifaces, iface, name);
  gboolean b = FALSE;
  if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) b = g_variant_get_boolean(v);
  if (v) g_variant_unref(v);
  return b;
}

// Path of the first Device1 whose Address (any case) equals `addr`.
static char *find_device(const char *addr) {
  GError *e = NULL;
  GVariant *objs = managed_objects(&e);
  if (!objs) {
    if (e) g_error_free(e);
    return NULL;
  }
  char *found = NULL;
  GVariantIter it;
  const char *path;
  GVariant *ifaces;
  g_variant_iter_init(&it, objs);
  while (!found && g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
    char *a = prop_str(ifaces, "org.bluez.Device1", "Address");
    if (a && g_ascii_strcasecmp(a, addr) == 0) found = g_strdup(path);
    g_free(a);
    g_variant_unref(ifaces);
  }
  g_variant_unref(objs);
  return found;
}

// ------------------------------------------------------------ signals

static void on_value(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                     const gchar *signal, GVariant *params, gpointer user) {
  const char *changed_iface;
  GVariant *changed;
  g_variant_get(params, "(&s@a{sv}as)", &changed_iface, &changed, NULL);
  GVariant *v = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
  if (v) {
    Note *n = g_new0(Note, 1);
    n->uuid = g_strdup((const char *)user);
    n->data = g_variant_get_data_as_bytes(v);
    pthread_mutex_lock(&mu);
    g_ptr_array_add(notes, n);
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    g_variant_unref(v);
  }
  g_variant_unref(changed);
}

static void on_device(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                      const gchar *signal, GVariant *params, gpointer user) {
  GVariant *changed;
  g_variant_get(params, "(&s@a{sv}as)", NULL, &changed, NULL);
  GVariant *v = g_variant_lookup_value(changed, "Connected", G_VARIANT_TYPE_BOOLEAN);
  if (v) {
    pthread_mutex_lock(&mu);
    connected = g_variant_get_boolean(v);
    if (!connected) {
      g_free(last_err);
      last_err = g_strdup("disconnected");
    }
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    g_variant_unref(v);
  }
  g_variant_unref(changed);
}

// ------------------------------------------------------------ API

#define REQUIRE_INIT() \
  if (!bus || !adapter) return io_err("BLE not initialised: call Ble.init first")
#define REQUIRE_CONN() \
  REQUIRE_INIT();      \
  if (!dev_path || !connected) return io_err("not connected")

lean_obj_res lean_ble_init(uint32_t timeout_ms, lean_obj_arg w) {
  if (!bus) {
    GError *e = NULL;
    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &e);
    if (!bus) return gerr("cannot reach the system D-Bus", e);
    notes = g_ptr_array_new();
    subs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    loop = g_main_loop_new(NULL, FALSE);
    pthread_t t;
    pthread_create(&t, NULL, loop_main, NULL);
    pthread_detach(t);
  }
  GError *e = NULL;
  GVariant *objs = managed_objects(&e);
  if (!objs) return gerr("bluetoothd not reachable (is the bluetooth service running?)", e);
  g_free(adapter);
  adapter = NULL;
  gboolean powered = FALSE;
  GVariantIter it;
  const char *path;
  GVariant *ifaces;
  g_variant_iter_init(&it, objs);
  while (!adapter && g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
    if (prop(ifaces, "org.bluez.Adapter1", "Address")) {
      adapter = g_strdup(path);
      powered = prop_bool(ifaces, "org.bluez.Adapter1", "Powered");
    }
    g_variant_unref(ifaces);
  }
  g_variant_unref(objs);
  if (!adapter) return io_err("no Bluetooth adapter found");
  if (!powered) {
    GVariant *r = call(adapter, "org.freedesktop.DBus.Properties", "Set",
                       g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(TRUE)),
                       5000, &e);
    if (!r) return gerr("Bluetooth is powered off and could not be turned on", e);
    g_variant_unref(r);
    sleep_ms(500);
  }
  return io_unit();
}

lean_obj_res lean_ble_scan(uint32_t ms, lean_obj_arg w) {
  REQUIRE_INIT();
  GError *e = NULL;
  GVariantBuilder f;
  g_variant_builder_init(&f, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&f, "{sv}", "Transport", g_variant_new_string("le"));
  g_variant_builder_add(&f, "{sv}", "DuplicateData", g_variant_new_boolean(TRUE));
  GVariant *r = call(adapter, "org.bluez.Adapter1", "SetDiscoveryFilter", g_variant_new("(a{sv})", &f), 5000, &e);
  if (r) g_variant_unref(r);
  else if (e) { g_error_free(e); e = NULL; }
  r = call(adapter, "org.bluez.Adapter1", "StartDiscovery", NULL, 5000, &e);
  if (r) g_variant_unref(r);
  else if (e && !strstr(e->message, "InProgress")) return gerr("StartDiscovery", e);
  else if (e) { g_error_free(e); e = NULL; }
  sleep_ms(ms);
  r = call(adapter, "org.bluez.Adapter1", "StopDiscovery", NULL, 5000, &e);
  if (r) g_variant_unref(r);
  else if (e) { g_error_free(e); e = NULL; }

  GVariant *objs = managed_objects(&e);
  if (!objs) return gerr("GetManagedObjects", e);
  lean_object *out = lean_mk_empty_array();
  GVariantIter it;
  const char *path;
  GVariant *ifaces;
  g_variant_iter_init(&it, objs);
  while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
    char *addr = prop_str(ifaces, "org.bluez.Device1", "Address");
    if (addr) {
      char *name = prop_str(ifaces, "org.bluez.Device1", "Name");
      if (!name) name = prop_str(ifaces, "org.bluez.Device1", "Alias");
      if (name && g_str_equal(name, addr)) { g_free(name); name = NULL; } // Alias defaults to the address
      GVariant *rssi = prop(ifaces, "org.bluez.Device1", "RSSI");
      lean_object *svcs = lean_mk_empty_array();
      GVariant *uu = prop(ifaces, "org.bluez.Device1", "UUIDs");
      if (uu) {
        GVariantIter ui;
        const char *u;
        g_variant_iter_init(&ui, uu);
        while (g_variant_iter_next(&ui, "&s", &u)) svcs = lean_array_push(svcs, lean_mk_string(u));
        g_variant_unref(uu);
      }
      lean_object *mfg = NULL;
      GVariant *md = prop(ifaces, "org.bluez.Device1", "ManufacturerData");
      if (md) {
        // a{qv}: take the first entry; the Lean side expects company id (LE) then data.
        GVariantIter mi;
        guint16 company;
        GVariant *val;
        g_variant_iter_init(&mi, md);
        if (g_variant_iter_next(&mi, "{qv}", &company, &val)) {
          gsize n = 0;
          const guint8 *p = g_variant_get_fixed_array(val, &n, 1);
          lean_object *a = lean_alloc_sarray(1, n + 2, n + 2);
          lean_sarray_cptr(a)[0] = company & 0xff;
          lean_sarray_cptr(a)[1] = company >> 8;
          if (n) memcpy(lean_sarray_cptr(a) + 2, p, n);
          mfg = a;
          g_variant_unref(val);
        }
        g_variant_unref(md);
      }
      if (!mfg) mfg = lean_alloc_sarray(1, 0, 0);
      lean_object *o = lean_alloc_ctor(0, 5, 0);
      lean_ctor_set(o, 0, lean_mk_string(addr));
      lean_ctor_set(o, 1, lean_mk_string(name ? name : ""));
      lean_ctor_set(o, 2, lean_int64_to_int(rssi && g_variant_is_of_type(rssi, G_VARIANT_TYPE_INT16)
                                                ? g_variant_get_int16(rssi)
                                                : 0));
      lean_ctor_set(o, 3, svcs);
      lean_ctor_set(o, 4, mfg);
      out = lean_array_push(out, o);
      if (rssi) g_variant_unref(rssi);
      g_free(name);
      g_free(addr);
    }
    g_variant_unref(ifaces);
  }
  g_variant_unref(objs);
  return lean_io_result_mk_ok(out);
}

lean_obj_res lean_ble_connect(b_lean_obj_arg id, uint32_t timeout_ms, lean_obj_arg w) {
  REQUIRE_INIT();
  const char *addr = lean_string_cstr(id);
  char *path = find_device(addr);
  if (!path) {
    char *m = g_strdup_printf("unknown peripheral %s (scan first)", addr);
    lean_obj_res r = io_err(m);
    g_free(m);
    return r;
  }
  pthread_mutex_lock(&mu);
  g_free(dev_path);
  dev_path = path;
  connected = FALSE;
  for (guint i = 0; i < notes->len; i++) {
    Note *n = g_ptr_array_index(notes, i);
    g_free(n->uuid);
    g_bytes_unref(n->data);
    g_free(n);
  }
  g_ptr_array_set_size(notes, 0);
  pthread_mutex_unlock(&mu);
  if (dev_sub) g_dbus_connection_signal_unsubscribe(bus, dev_sub);
  dev_sub = g_dbus_connection_signal_subscribe(bus, BLUEZ, "org.freedesktop.DBus.Properties",
                                               "PropertiesChanged", path, "org.bluez.Device1",
                                               G_DBUS_SIGNAL_FLAGS_NONE, on_device, NULL, NULL);
  GError *e = NULL;
  GVariant *r = call(path, "org.bluez.Device1", "Connect", NULL, (int)timeout_ms, &e);
  if (!r) return gerr("connect", e);
  g_variant_unref(r);
  pthread_mutex_lock(&mu);
  connected = TRUE;
  pthread_mutex_unlock(&mu);
  return io_unit();
}

lean_obj_res lean_ble_discover(uint32_t timeout_ms, lean_obj_arg w) {
  REQUIRE_CONN();
  // Wait for BlueZ to finish resolving the GATT database.
  struct timespec dl;
  deadline_in(&dl, timeout_ms);
  for (;;) {
    GError *e = NULL;
    GVariant *r = call(dev_path, "org.freedesktop.DBus.Properties", "Get",
                       g_variant_new("(ss)", "org.bluez.Device1", "ServicesResolved"), 2000, &e);
    gboolean ok = FALSE;
    if (r) {
      GVariant *inner;
      g_variant_get(r, "(v)", &inner);
      ok = g_variant_get_boolean(inner);
      g_variant_unref(inner);
      g_variant_unref(r);
    } else if (e) g_error_free(e);
    if (ok) break;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec > dl.tv_sec || (now.tv_sec == dl.tv_sec && now.tv_nsec > dl.tv_nsec))
      return io_err("service discovery timed out");
    pthread_mutex_lock(&mu);
    gboolean up = connected;
    pthread_mutex_unlock(&mu);
    if (!up) return io_err("disconnected during discovery");
    sleep_ms(100);
  }
  GError *e = NULL;
  GVariant *objs = managed_objects(&e);
  if (!objs) return gerr("GetManagedObjects", e);
  GHashTable *svc_uuid = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  GVariantIter it;
  const char *path;
  GVariant *ifaces;
  size_t plen = strlen(dev_path);
  g_variant_iter_init(&it, objs);
  while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
    if (strncmp(path, dev_path, plen) == 0) {
      char *u = prop_str(ifaces, "org.bluez.GattService1", "UUID");
      if (u) g_hash_table_insert(svc_uuid, g_strdup(path), u);
    }
    g_variant_unref(ifaces);
  }
  lean_object *out = lean_mk_empty_array();
  g_variant_iter_init(&it, objs);
  while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
    if (strncmp(path, dev_path, plen) == 0) {
      char *cu = prop_str(ifaces, "org.bluez.GattCharacteristic1", "UUID");
      char *sp = prop_str(ifaces, "org.bluez.GattCharacteristic1", "Service");
      if (cu && sp) {
        size_t props = 0;
        GVariant *fl = prop(ifaces, "org.bluez.GattCharacteristic1", "Flags");
        if (fl) {
          GVariantIter fi;
          const char *f;
          g_variant_iter_init(&fi, fl);
          while (g_variant_iter_next(&fi, "&s", &f)) {
            if (g_str_equal(f, "read")) props |= 0x02;
            else if (g_str_equal(f, "write-without-response")) props |= 0x04;
            else if (g_str_equal(f, "write")) props |= 0x08;
            else if (g_str_equal(f, "notify")) props |= 0x10;
            else if (g_str_equal(f, "indicate")) props |= 0x20;
          }
          g_variant_unref(fl);
        }
        const char *su = g_hash_table_lookup(svc_uuid, sp);
        lean_object *o = lean_alloc_ctor(0, 3, 0);
        lean_ctor_set(o, 0, lean_mk_string(su ? su : ""));
        lean_ctor_set(o, 1, lean_mk_string(cu));
        lean_ctor_set(o, 2, lean_box(props));
        out = lean_array_push(out, o);
      }
      g_free(cu);
      g_free(sp);
    }
    g_variant_unref(ifaces);
  }
  g_hash_table_destroy(svc_uuid);
  g_variant_unref(objs);
  return lean_io_result_mk_ok(out);
}

// Object path of the characteristic with this UUID (and service UUID, when given).
static char *find_char(const char *svc, const char *chr) {
  GError *e = NULL;
  GVariant *objs = managed_objects(&e);
  if (!objs) {
    if (e) g_error_free(e);
    return NULL;
  }
  GHashTable *svc_uuid = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  GVariantIter it;
  const char *path;
  GVariant *ifaces;
  g_variant_iter_init(&it, objs);
  while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
    char *u = prop_str(ifaces, "org.bluez.GattService1", "UUID");
    if (u) g_hash_table_insert(svc_uuid, g_strdup(path), u);
    g_variant_unref(ifaces);
  }
  char *found = NULL;
  size_t plen = strlen(dev_path);
  g_variant_iter_init(&it, objs);
  while (!found && g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
    if (strncmp(path, dev_path, plen) == 0) {
      char *cu = prop_str(ifaces, "org.bluez.GattCharacteristic1", "UUID");
      char *sp = prop_str(ifaces, "org.bluez.GattCharacteristic1", "Service");
      if (cu && g_ascii_strcasecmp(cu, chr) == 0) {
        const char *su = sp ? g_hash_table_lookup(svc_uuid, sp) : NULL;
        if (!*svc || (su && g_ascii_strcasecmp(su, svc) == 0)) found = g_strdup(path);
      }
      g_free(cu);
      g_free(sp);
    }
    g_variant_unref(ifaces);
  }
  g_hash_table_destroy(svc_uuid);
  g_variant_unref(objs);
  return found;
}

lean_obj_res lean_ble_subscribe(b_lean_obj_arg svc, b_lean_obj_arg chr, uint32_t timeout_ms,
                                lean_obj_arg w) {
  REQUIRE_CONN();
  char *cp = find_char(lean_string_cstr(svc), lean_string_cstr(chr));
  if (!cp) return io_err("characteristic not found (run discover first)");
  if (!g_hash_table_contains(subs, cp)) {
    guint id = g_dbus_connection_signal_subscribe(
        bus, BLUEZ, "org.freedesktop.DBus.Properties", "PropertiesChanged", cp,
        "org.bluez.GattCharacteristic1", G_DBUS_SIGNAL_FLAGS_NONE, on_value,
        g_strdup(lean_string_cstr(chr)), NULL);
    g_hash_table_insert(subs, g_strdup(cp), GUINT_TO_POINTER(id));
  }
  GError *e = NULL;
  GVariant *r = call(cp, "org.bluez.GattCharacteristic1", "StartNotify", NULL, (int)timeout_ms, &e);
  g_free(cp);
  if (!r) {
    if (e && strstr(e->message, "Already notifying")) { g_error_free(e); return io_unit(); }
    return gerr("subscribe failed", e);
  }
  g_variant_unref(r);
  return io_unit();
}

lean_obj_res lean_ble_write(b_lean_obj_arg svc, b_lean_obj_arg chr, b_lean_obj_arg data,
                            uint8_t with_response, uint32_t timeout_ms, lean_obj_arg w) {
  REQUIRE_CONN();
  char *cp = find_char(lean_string_cstr(svc), lean_string_cstr(chr));
  if (!cp) return io_err("characteristic not found (run discover first)");
  GVariantBuilder opts;
  g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&opts, "{sv}", "type",
                        g_variant_new_string(with_response ? "request" : "command"));
  GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, lean_sarray_cptr(data),
                                              lean_sarray_size(data), 1);
  GError *e = NULL;
  GVariant *r = call(cp, "org.bluez.GattCharacteristic1", "WriteValue",
                     g_variant_new("(@aya{sv})", bytes, &opts), (int)timeout_ms, &e);
  g_free(cp);
  if (!r) return gerr("write failed", e);
  g_variant_unref(r);
  return io_unit();
}

lean_obj_res lean_ble_read(b_lean_obj_arg svc, b_lean_obj_arg chr, uint32_t timeout_ms,
                           lean_obj_arg w) {
  REQUIRE_CONN();
  char *cp = find_char(lean_string_cstr(svc), lean_string_cstr(chr));
  if (!cp) return io_err("characteristic not found (run discover first)");
  GVariantBuilder opts;
  g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
  GError *e = NULL;
  GVariant *r = call(cp, "org.bluez.GattCharacteristic1", "ReadValue", g_variant_new("(a{sv})", &opts),
                     (int)timeout_ms, &e);
  g_free(cp);
  if (!r) return gerr("read failed", e);
  GVariant *arr = g_variant_get_child_value(r, 0);
  gsize n = 0;
  const guint8 *p = g_variant_get_fixed_array(arr, &n, 1);
  lean_object *out = mk_bytes(p, n);
  g_variant_unref(arr);
  g_variant_unref(r);
  return lean_io_result_mk_ok(out);
}

lean_obj_res lean_ble_next_notification(uint32_t timeout_ms, lean_obj_arg w) {
  REQUIRE_INIT();
  struct timespec dl;
  deadline_in(&dl, timeout_ms);
  pthread_mutex_lock(&mu);
  while (notes->len == 0 && connected) {
    if (pthread_cond_timedwait(&cv, &mu, &dl) != 0) break;
  }
  Note *n = notes->len ? g_ptr_array_remove_index(notes, 0) : NULL;
  gboolean conn = connected;
  char *err = last_err ? g_strdup(last_err) : NULL;
  pthread_mutex_unlock(&mu);
  if (!n) {
    lean_obj_res r = conn ? lean_io_result_mk_ok(lean_box(0)) : io_err(err ? err : "disconnected");
    g_free(err);
    return r;
  }
  g_free(err);
  gsize len = 0;
  const void *p = g_bytes_get_data(n->data, &len);
  lean_object *pair = lean_alloc_ctor(0, 2, 0);
  lean_ctor_set(pair, 0, lean_mk_string(n->uuid));
  lean_ctor_set(pair, 1, mk_bytes(p, len));
  lean_object *some = lean_alloc_ctor(1, 1, 0);
  lean_ctor_set(some, 0, pair);
  g_free(n->uuid);
  g_bytes_unref(n->data);
  g_free(n);
  return lean_io_result_mk_ok(some);
}

lean_obj_res lean_ble_disconnect(lean_obj_arg w) {
  if (bus && dev_path) {
    GError *e = NULL;
    GVariant *r = call(dev_path, "org.bluez.Device1", "Disconnect", NULL, 3000, &e);
    if (r) g_variant_unref(r);
    else if (e) g_error_free(e);
    pthread_mutex_lock(&mu);
    connected = FALSE;
    pthread_mutex_unlock(&mu);
  }
  return io_unit();
}
