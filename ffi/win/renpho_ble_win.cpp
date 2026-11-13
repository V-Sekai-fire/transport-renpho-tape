// WinRT implementation of the BLE central for the RENPHO tape client (Windows).
//
// Built with MSVC as renpho_ble_win.dll (C++/WinRT, C++20). It is a DLL, not a
// static library, so its C++ runtime and exception model stay out of the
// MinGW-built Lean executable. The id of a peripheral is its Bluetooth address
// as "AA:BB:CC:DD:EE:FF".

#include "renpho_ble_win.h"

#include <windows.h>

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;

namespace {

struct Hit {
  std::string name;
  int rssi = 0;
  std::vector<std::string> services;
  std::vector<uint8_t> mfg;
};

struct Note {
  std::string uuid;
  std::vector<uint8_t> data;
};

std::mutex mu;
std::condition_variable cv;
bool inited = false;
BluetoothLEDevice device{nullptr};
GattSession session{nullptr};
std::vector<GattDeviceService> services;
bool connected = false;
std::deque<Note> notes;
std::vector<std::pair<GattCharacteristic, event_token>> subs;
event_token status_token{};

std::string guid_str(guid const &g) {
  char b[40];
  std::snprintf(b, sizeof b, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2,
                g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
                g.Data4[6], g.Data4[7]);
  return b;
}

std::string addr_str(uint64_t a) {
  char b[24];
  std::snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)((a >> 40) & 0xff),
                (unsigned)((a >> 32) & 0xff), (unsigned)((a >> 24) & 0xff),
                (unsigned)((a >> 16) & 0xff), (unsigned)((a >> 8) & 0xff), (unsigned)(a & 0xff));
  return b;
}

bool parse_addr(const char *s, uint64_t &out) {
  unsigned b[6];
  if (std::sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
  out = 0;
  for (int i = 0; i < 6; i++) out = (out << 8) | (b[i] & 0xff);
  return true;
}

bool uuid_eq(const std::string &a, const char *b) {
  return _stricmp(a.c_str(), b) == 0;
}

void put_err(char *err, size_t cap, const std::string &m) {
  if (!err || !cap) return;
  std::snprintf(err, cap, "%s", m.c_str());
}

std::vector<uint8_t> buf_bytes(IBuffer const &b) {
  std::vector<uint8_t> v(b.Length());
  if (!v.empty()) DataReader::FromBuffer(b).ReadBytes(v);
  return v;
}

template <class Op>
auto wait_for(Op const &op, uint32_t ms) -> decltype(op.GetResults()) {
  if (op.wait_for(std::chrono::milliseconds(ms)) != Windows::Foundation::AsyncStatus::Completed)
    throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"operation timed out");
  return op.GetResults();
}

// Find a characteristic by UUID (and service UUID when `svc` is non-empty).
GattCharacteristic find_char(const char *svc, const char *chr, uint32_t ms) {
  for (auto const &s : services) {
    if (*svc && !uuid_eq(guid_str(s.Uuid()), svc)) continue;
    auto res = wait_for(s.GetCharacteristicsAsync(BluetoothCacheMode::Uncached), ms);
    for (auto const &c : res.Characteristics())
      if (uuid_eq(guid_str(c.Uuid()), chr)) return c;
  }
  throw hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), L"characteristic not found (run discover first)");
}

#define GUARD(...)                                                 \
  try {                                                            \
    __VA_ARGS__                                                    \
  } catch (hresult_error const &e) {                               \
    put_err(err, errcap, to_string(e.message()));                  \
    return 1;                                                      \
  } catch (std::exception const &e) {                              \
    put_err(err, errcap, e.what());                                \
    return 1;                                                      \
  }

} // namespace

extern "C" {

int rb_init(uint32_t, char *err, size_t errcap) {
  GUARD({
    if (!inited) {
      init_apartment(apartment_type::multi_threaded);
      inited = true;
    }
    auto adapter = wait_for(BluetoothAdapter::GetDefaultAsync(), 5000);
    if (!adapter) {
      put_err(err, errcap, "no Bluetooth adapter found");
      return 1;
    }
    if (!adapter.IsLowEnergySupported()) {
      put_err(err, errcap, "Bluetooth LE unsupported on this machine");
      return 1;
    }
    return 0;
  })
}

int rb_scan(uint32_t ms, rb_scan_cb cb, void *ctx, char *err, size_t errcap) {
  GUARD({
    std::map<uint64_t, Hit> hits;
    std::vector<uint64_t> order;
    std::mutex hmu;
    BluetoothLEAdvertisementWatcher w;
    w.ScanningMode(BluetoothLEScanningMode::Active);
    auto tok = w.Received([&](auto &&, BluetoothLEAdvertisementReceivedEventArgs const &a) {
      std::lock_guard<std::mutex> g(hmu);
      auto addr = a.BluetoothAddress();
      if (!hits.count(addr)) order.push_back(addr);
      Hit &h = hits[addr];
      h.rssi = a.RawSignalStrengthInDBm();
      auto adv = a.Advertisement();
      auto n = to_string(adv.LocalName());
      if (!n.empty()) h.name = n;
      for (auto const &u : adv.ServiceUuids()) {
        auto s = guid_str(u);
        bool have = false;
        for (auto const &x : h.services) have |= x == s;
        if (!have) h.services.push_back(s);
      }
      for (auto const &m : adv.ManufacturerData()) {
        h.mfg.clear();
        h.mfg.push_back(m.CompanyId() & 0xff);
        h.mfg.push_back(m.CompanyId() >> 8);
        auto d = buf_bytes(m.Data());
        h.mfg.insert(h.mfg.end(), d.begin(), d.end());
      }
    });
    w.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    w.Stop();
    w.Received(tok);
    std::lock_guard<std::mutex> g(hmu);
    for (auto addr : order) {
      Hit &h = hits[addr];
      std::string csv;
      for (auto const &s : h.services) csv += (csv.empty() ? "" : ",") + s;
      cb(ctx, addr_str(addr).c_str(), h.name.c_str(), h.rssi, csv.c_str(), h.mfg.data(), h.mfg.size());
    }
    return 0;
  })
}

int rb_connect(const char *id, uint32_t timeout_ms, char *err, size_t errcap) {
  GUARD({
    uint64_t addr;
    if (!parse_addr(id, addr)) {
      put_err(err, errcap, std::string("bad peripheral id ") + id);
      return 1;
    }
    rb_disconnect();
    auto dev = wait_for(BluetoothLEDevice::FromBluetoothAddressAsync(addr), timeout_ms);
    if (!dev) {
      put_err(err, errcap, std::string("unknown peripheral ") + id + " (scan first)");
      return 1;
    }
    // Keep the link up while we hold the device; a GATT service query makes the connection.
    session = wait_for(GattSession::FromDeviceIdAsync(dev.BluetoothDeviceId()), timeout_ms);
    session.MaintainConnection(true);
    auto r = wait_for(dev.GetGattServicesAsync(BluetoothCacheMode::Uncached), timeout_ms);
    if (r.Status() != GattCommunicationStatus::Success) {
      put_err(err, errcap, "connect failed (GATT status " + std::to_string((int)r.Status()) + ")");
      return 1;
    }
    std::lock_guard<std::mutex> g(mu);
    device = dev;
    services.clear();
    for (auto const &s : r.Services()) services.push_back(s);
    notes.clear();
    connected = true;
    status_token = dev.ConnectionStatusChanged([](BluetoothLEDevice const &d, auto &&) {
      std::lock_guard<std::mutex> g2(mu);
      connected = d.ConnectionStatus() == BluetoothConnectionStatus::Connected;
      cv.notify_all();
    });
    return 0;
  })
}

int rb_discover(uint32_t timeout_ms, rb_char_cb cb, void *ctx, char *err, size_t errcap) {
  GUARD({
    if (!connected) {
      put_err(err, errcap, "not connected");
      return 1;
    }
    for (auto const &s : services) {
      auto res = wait_for(s.GetCharacteristicsAsync(BluetoothCacheMode::Uncached), timeout_ms);
      for (auto const &c : res.Characteristics())
        cb(ctx, guid_str(s.Uuid()).c_str(), guid_str(c.Uuid()).c_str(),
           (uint32_t)c.CharacteristicProperties() & 0xff);
    }
    return 0;
  })
}

int rb_subscribe(const char *svc, const char *chr, uint32_t timeout_ms, char *err, size_t errcap) {
  GUARD({
    if (!connected) {
      put_err(err, errcap, "not connected");
      return 1;
    }
    auto c = find_char(svc, chr, timeout_ms);
    std::string uuid = guid_str(c.Uuid());
    auto tok = c.ValueChanged([uuid](GattCharacteristic const &, GattValueChangedEventArgs const &a) {
      Note n{uuid, buf_bytes(a.CharacteristicValue())};
      std::lock_guard<std::mutex> g(mu);
      notes.push_back(std::move(n));
      cv.notify_all();
    });
    subs.emplace_back(c, tok);
    bool indicate = ((uint32_t)c.CharacteristicProperties() & 0x10) == 0;
    auto st = wait_for(c.WriteClientCharacteristicConfigurationDescriptorAsync(
                           indicate ? GattClientCharacteristicConfigurationDescriptorValue::Indicate
                                    : GattClientCharacteristicConfigurationDescriptorValue::Notify),
                       timeout_ms);
    if (st != GattCommunicationStatus::Success) {
      put_err(err, errcap, "subscribe failed (GATT status " + std::to_string((int)st) + ")");
      return 1;
    }
    return 0;
  })
}

int rb_write(const char *svc, const char *chr, const uint8_t *data, size_t len, int with_response,
             uint32_t timeout_ms, char *err, size_t errcap) {
  GUARD({
    if (!connected) {
      put_err(err, errcap, "not connected");
      return 1;
    }
    auto c = find_char(svc, chr, timeout_ms);
    DataWriter dw;
    dw.WriteBytes(array_view<const uint8_t>(data, data + len));
    auto st = wait_for(c.WriteValueAsync(dw.DetachBuffer(), with_response ? GattWriteOption::WriteWithResponse
                                                                          : GattWriteOption::WriteWithoutResponse),
                       timeout_ms);
    if (st != GattCommunicationStatus::Success) {
      put_err(err, errcap, "write failed (GATT status " + std::to_string((int)st) + ")");
      return 1;
    }
    return 0;
  })
}

int rb_read(const char *svc, const char *chr, uint32_t timeout_ms, uint8_t *buf, size_t cap, size_t *len,
            char *err, size_t errcap) {
  GUARD({
    if (!connected) {
      put_err(err, errcap, "not connected");
      return 1;
    }
    auto c = find_char(svc, chr, timeout_ms);
    auto r = wait_for(c.ReadValueAsync(BluetoothCacheMode::Uncached), timeout_ms);
    if (r.Status() != GattCommunicationStatus::Success) {
      put_err(err, errcap, "read failed (GATT status " + std::to_string((int)r.Status()) + ")");
      return 1;
    }
    auto v = buf_bytes(r.Value());
    *len = v.size() < cap ? v.size() : cap;
    std::memcpy(buf, v.data(), *len);
    return 0;
  })
}

int rb_next(uint32_t timeout_ms, char *uuid, size_t uuidcap, uint8_t *buf, size_t cap, size_t *len,
            char *err, size_t errcap) {
  std::unique_lock<std::mutex> lk(mu);
  cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] { return !notes.empty() || !connected; });
  if (notes.empty()) {
    if (!connected) {
      put_err(err, errcap, "disconnected");
      return -1;
    }
    return 0;
  }
  Note n = std::move(notes.front());
  notes.pop_front();
  std::snprintf(uuid, uuidcap, "%s", n.uuid.c_str());
  *len = n.data.size() < cap ? n.data.size() : cap;
  std::memcpy(buf, n.data.data(), *len);
  return 1;
}

int rb_disconnect(void) {
  try {
    for (auto &s : subs) s.first.ValueChanged(s.second);
    subs.clear();
    if (device) device.ConnectionStatusChanged(status_token);
    for (auto const &s : services) s.Close();
    services.clear();
    if (session) session.Close();
    if (device) device.Close();
    session = nullptr;
    device = nullptr;
  } catch (...) {
  }
  std::lock_guard<std::mutex> g(mu);
  connected = false;
  cv.notify_all();
  return 0;
}

} // extern "C"
