// CoreBluetooth shim for the Lean 4 RENPHO tape client.
//
// One central, one connected peripheral. CoreBluetooth runs on a private serial
// queue; every Lean-facing call blocks on a condition variable until the
// matching delegate callback arrives or the timeout expires. No Lean object is
// ever touched on the CoreBluetooth queue: callbacks only fill Objective-C
// state, and the calling Lean thread converts it.

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include <lean/lean.h>
#include <string.h>

// ---------------------------------------------------------------- state

@interface RTScanHit : NSObject
@property(strong) CBPeripheral *peripheral;
@property(copy) NSString *name;
@property(strong) NSNumber *rssi;
@property(strong) NSArray<CBUUID *> *services;
@property(strong) NSData *mfg;
@end
@implementation RTScanHit
@end

@interface RTNote : NSObject
@property(copy) NSString *uuid;
@property(strong) NSData *data;
@end
@implementation RTNote
@end

@interface RTCentral : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(strong) CBCentralManager *central;
@property(strong) dispatch_queue_t queue;
@property(strong) NSCondition *cond;
@property(strong) NSMutableDictionary<NSString *, RTScanHit *> *hits;
@property(strong) NSMutableArray<NSString *> *hitOrder;
@property(strong) CBPeripheral *peripheral;
@property(assign) BOOL connected;
@property(assign) BOOL connectDone;
@property(copy) NSString *lastError;
@property(assign) NSInteger pendingDiscovery;
@property(assign) BOOL discoveryDone;
@property(strong) NSMutableArray<RTNote *> *notes;
@property(assign) BOOL opDone;       // write / subscribe / read completion
@property(strong) NSData *readValue; // result of an explicit read
@property(strong) CBCharacteristic *readTarget;
@end

@implementation RTCentral

- (instancetype)init {
  if ((self = [super init])) {
    _cond = [NSCondition new];
    _hits = [NSMutableDictionary new];
    _hitOrder = [NSMutableArray new];
    _notes = [NSMutableArray new];
    _queue = dispatch_queue_create("renpho.tape.ble", DISPATCH_QUEUE_SERIAL);
    _central = [[CBCentralManager alloc] initWithDelegate:self queue:_queue options:nil];
  }
  return self;
}

- (void)signal:(void (^)(void))mutate {
  [self.cond lock];
  mutate();
  [self.cond broadcast];
  [self.cond unlock];
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
  [self signal:^{
  }];
}

- (void)centralManager:(CBCentralManager *)central
    didDiscoverPeripheral:(CBPeripheral *)peripheral
        advertisementData:(NSDictionary<NSString *, id> *)adv
                     RSSI:(NSNumber *)rssi {
  NSString *ident = peripheral.identifier.UUIDString;
  NSString *name = adv[CBAdvertisementDataLocalNameKey] ?: peripheral.name ?: @"";
  [self signal:^{
    RTScanHit *hit = self.hits[ident];
    if (!hit) {
      hit = [RTScanHit new];
      self.hits[ident] = hit;
      [self.hitOrder addObject:ident];
      hit.name = @"";
      hit.services = @[];
      hit.mfg = [NSData data];
    }
    hit.peripheral = peripheral;
    hit.rssi = rssi;
    if (name.length) hit.name = name;
    NSArray *svcs = adv[CBAdvertisementDataServiceUUIDsKey];
    if (svcs.count) hit.services = svcs;
    NSData *mfg = adv[CBAdvertisementDataManufacturerDataKey];
    if (mfg.length) hit.mfg = mfg;
  }];
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)p {
  [self signal:^{
    self.connected = YES;
    self.connectDone = YES;
  }];
}

- (void)centralManager:(CBCentralManager *)central
    didFailToConnectPeripheral:(CBPeripheral *)p
                         error:(NSError *)error {
  [self signal:^{
    self.connected = NO;
    self.connectDone = YES;
    self.lastError = error.localizedDescription ?: @"connect failed";
  }];
}

- (void)centralManager:(CBCentralManager *)central
    didDisconnectPeripheral:(CBPeripheral *)p
                      error:(NSError *)error {
  [self signal:^{
    self.connected = NO;
    self.connectDone = YES;
    self.discoveryDone = YES;
    self.opDone = YES;
    self.lastError = error.localizedDescription ?: @"disconnected";
  }];
}

- (void)peripheral:(CBPeripheral *)p didDiscoverServices:(NSError *)error {
  if (error || p.services.count == 0) {
    [self signal:^{
      if (error) self.lastError = error.localizedDescription;
      self.discoveryDone = YES;
    }];
    return;
  }
  [self signal:^{
    self.pendingDiscovery = (NSInteger)p.services.count;
  }];
  for (CBService *s in p.services) [p discoverCharacteristics:nil forService:s];
}

- (void)peripheral:(CBPeripheral *)p
    didDiscoverCharacteristicsForService:(CBService *)service
                                   error:(NSError *)error {
  [self signal:^{
    if (error) self.lastError = error.localizedDescription;
    self.pendingDiscovery -= 1;
    if (self.pendingDiscovery <= 0) self.discoveryDone = YES;
  }];
}

- (void)peripheral:(CBPeripheral *)p
    didUpdateValueForCharacteristic:(CBCharacteristic *)c
                              error:(NSError *)error {
  NSData *value = [c.value copy] ?: [NSData data];
  NSString *uuid = c.UUID.UUIDString;
  [self signal:^{
    if (self.readTarget == c) {
      // Answer to an explicit read.
      self.readTarget = nil;
      if (error) self.lastError = error.localizedDescription;
      self.readValue = value;
      self.opDone = YES;
    } else if (!error) {
      RTNote *n = [RTNote new];
      n.uuid = uuid;
      n.data = value;
      [self.notes addObject:n];
    }
  }];
}

- (void)peripheral:(CBPeripheral *)p
    didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)c
                                          error:(NSError *)error {
  [self signal:^{
    if (error) self.lastError = error.localizedDescription;
    self.opDone = YES;
  }];
}

- (void)peripheral:(CBPeripheral *)p
    didWriteValueForCharacteristic:(CBCharacteristic *)c
                             error:(NSError *)error {
  [self signal:^{
    if (error) self.lastError = error.localizedDescription;
    self.opDone = YES;
  }];
}

@end

static RTCentral *gC = nil;

// ---------------------------------------------------------------- helpers

static lean_obj_res io_err(NSString *msg) {
  return lean_io_result_mk_error(lean_mk_io_user_error(lean_mk_string(msg.UTF8String)));
}

static lean_obj_res io_unit(void) { return lean_io_result_mk_ok(lean_box(0)); }

static lean_object *mk_bytes(NSData *d) {
  lean_object *arr = lean_alloc_sarray(1, d.length, d.length);
  if (d.length) memcpy(lean_sarray_cptr(arr), d.bytes, d.length);
  return arr;
}

static lean_object *mk_str(NSString *s) { return lean_mk_string(s ? s.UTF8String : ""); }

static NSString *state_name(CBManagerState s) {
  switch (s) {
    case CBManagerStatePoweredOn: return @"powered on";
    case CBManagerStatePoweredOff: return @"Bluetooth is powered off";
    case CBManagerStateUnauthorized:
      return @"Bluetooth permission denied: allow your terminal app in System Settings > "
             @"Privacy & Security > Bluetooth";
    case CBManagerStateUnsupported: return @"Bluetooth LE unsupported on this machine";
    case CBManagerStateResetting: return @"Bluetooth is resetting";
    default: return @"Bluetooth state unknown (timed out waiting for the adapter)";
  }
}

// Wait under gC.cond until `pred` holds or `ms` elapsed. Caller holds the lock.
static BOOL wait_until(uint32_t ms, BOOL (^pred)(void)) {
  NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:ms / 1000.0];
  while (!pred()) {
    if (![gC.cond waitUntilDate:deadline]) return pred();
  }
  return YES;
}

static BOOL uuid_eq(CBUUID *u, NSString *want) {
  if (want.length == 0) return YES;
  @try {
    return [u isEqual:[CBUUID UUIDWithString:want]];
  } @catch (NSException *e) {
    return NO;
  }
}

// Find a characteristic; an empty service string matches any service.
static CBCharacteristic *find_char(NSString *svc, NSString *chr) {
  for (CBService *s in gC.peripheral.services) {
    if (!uuid_eq(s.UUID, svc)) continue;
    for (CBCharacteristic *c in s.characteristics) {
      if (uuid_eq(c.UUID, chr)) return c;
    }
  }
  return nil;
}

#define REQUIRE_INIT() \
  if (!gC) return io_err(@"BLE not initialised: call Ble.init first")
#define REQUIRE_CONN()                                                   \
  REQUIRE_INIT();                                                        \
  if (!gC.peripheral || !gC.connected) return io_err(@"not connected")

// ---------------------------------------------------------------- API

// Ble.init : UInt32 → IO Unit
lean_obj_res lean_ble_init(uint32_t timeout_ms, lean_obj_arg w) {
  @autoreleasepool {
    if (!gC) gC = [RTCentral new];
    [gC.cond lock];
    BOOL ok = wait_until(timeout_ms, ^BOOL {
      CBManagerState s = gC.central.state;
      return s != CBManagerStateUnknown && s != CBManagerStateResetting;
    });
    CBManagerState s = gC.central.state;
    [gC.cond unlock];
    if (!ok || s != CBManagerStatePoweredOn) return io_err(state_name(s));
    return io_unit();
  }
}

// Ble.scanRaw : UInt32 → IO (Array ScanHit)
// ScanHit = { id name : String, rssi : Int, services : Array String, mfg : ByteArray }
lean_obj_res lean_ble_scan(uint32_t ms, lean_obj_arg w) {
  @autoreleasepool {
    REQUIRE_INIT();
    [gC.cond lock];
    [gC.hits removeAllObjects];
    [gC.hitOrder removeAllObjects];
    [gC.cond unlock];
    [gC.central scanForPeripheralsWithServices:nil
                                       options:@{CBCentralManagerScanOptionAllowDuplicatesKey : @YES}];
    [NSThread sleepForTimeInterval:ms / 1000.0];
    [gC.central stopScan];

    [gC.cond lock];
    lean_object *out = lean_mk_empty_array();
    for (NSString *ident in gC.hitOrder) {
      RTScanHit *h = gC.hits[ident];
      lean_object *svcs = lean_mk_empty_array();
      for (CBUUID *u in h.services) svcs = lean_array_push(svcs, mk_str(u.UUIDString));
      lean_object *o = lean_alloc_ctor(0, 5, 0);
      lean_ctor_set(o, 0, mk_str(ident));
      lean_ctor_set(o, 1, mk_str(h.name));
      lean_ctor_set(o, 2, lean_int64_to_int(h.rssi.longLongValue));
      lean_ctor_set(o, 3, svcs);
      lean_ctor_set(o, 4, mk_bytes(h.mfg));
      out = lean_array_push(out, o);
    }
    [gC.cond unlock];
    return lean_io_result_mk_ok(out);
  }
}

// Ble.connect : @& String → UInt32 → IO Unit
lean_obj_res lean_ble_connect(b_lean_obj_arg ident, uint32_t timeout_ms, lean_obj_arg w) {
  @autoreleasepool {
    REQUIRE_INIT();
    NSString *want = [NSString stringWithUTF8String:lean_string_cstr(ident)];
    [gC.cond lock];
    CBPeripheral *p = gC.hits[want.uppercaseString].peripheral;
    [gC.cond unlock];
    if (!p) {
      NSUUID *u = [[NSUUID alloc] initWithUUIDString:want];
      if (u) p = [gC.central retrievePeripheralsWithIdentifiers:@[ u ]].firstObject;
    }
    if (!p) return io_err([NSString stringWithFormat:@"unknown peripheral %@ (scan first)", want]);

    [gC.cond lock];
    gC.peripheral = p;
    p.delegate = gC;
    gC.connected = NO;
    gC.connectDone = NO;
    gC.lastError = nil;
    [gC.notes removeAllObjects];
    [gC.cond unlock];

    [gC.central connectPeripheral:p options:nil];

    [gC.cond lock];
    BOOL done = wait_until(timeout_ms, ^BOOL { return gC.connectDone; });
    BOOL ok = done && gC.connected;
    NSString *err = gC.lastError;
    [gC.cond unlock];
    if (!ok) {
      [gC.central cancelPeripheralConnection:p];
      return io_err(done ? (err ?: @"connect failed") : @"connect timed out");
    }
    return io_unit();
  }
}

// Ble.discoverRaw : UInt32 → IO (Array Characteristic)
// Characteristic = { service uuid : String, props : Nat }
lean_obj_res lean_ble_discover(uint32_t timeout_ms, lean_obj_arg w) {
  @autoreleasepool {
    REQUIRE_CONN();
    [gC.cond lock];
    gC.discoveryDone = NO;
    gC.pendingDiscovery = 0;
    gC.lastError = nil;
    [gC.cond unlock];

    [gC.peripheral discoverServices:nil];

    [gC.cond lock];
    BOOL done = wait_until(timeout_ms, ^BOOL { return gC.discoveryDone; });
    BOOL conn = gC.connected;
    NSString *err = gC.lastError;
    [gC.cond unlock];
    if (!done) return io_err(@"service discovery timed out");
    if (!conn) return io_err(err ?: @"disconnected during discovery");

    lean_object *out = lean_mk_empty_array();
    for (CBService *s in gC.peripheral.services) {
      for (CBCharacteristic *c in s.characteristics) {
        lean_object *o = lean_alloc_ctor(0, 3, 0);
        lean_ctor_set(o, 0, mk_str(s.UUID.UUIDString));
        lean_ctor_set(o, 1, mk_str(c.UUID.UUIDString));
        lean_ctor_set(o, 2, lean_box((size_t)c.properties & 0xff));
        out = lean_array_push(out, o);
      }
    }
    return lean_io_result_mk_ok(out);
  }
}

static NSString *lean_ns(b_lean_obj_arg s) {
  return [NSString stringWithUTF8String:lean_string_cstr(s)];
}

// Ble.subscribe : @& String → @& String → UInt32 → IO Unit
lean_obj_res lean_ble_subscribe(b_lean_obj_arg svc, b_lean_obj_arg chr, uint32_t timeout_ms,
                                lean_obj_arg w) {
  @autoreleasepool {
    REQUIRE_CONN();
    CBCharacteristic *c = find_char(lean_ns(svc), lean_ns(chr));
    if (!c) return io_err(@"characteristic not found (run discover first)");
    [gC.cond lock];
    gC.opDone = NO;
    gC.lastError = nil;
    [gC.cond unlock];
    [gC.peripheral setNotifyValue:YES forCharacteristic:c];
    [gC.cond lock];
    BOOL done = wait_until(timeout_ms, ^BOOL { return gC.opDone; });
    NSString *err = gC.lastError;
    [gC.cond unlock];
    if (!done) return io_err(@"subscribe timed out");
    if (err) return io_err([@"subscribe failed: " stringByAppendingString:err]);
    return io_unit();
  }
}

// Ble.write : @& String → @& String → @& ByteArray → Bool → UInt32 → IO Unit
lean_obj_res lean_ble_write(b_lean_obj_arg svc, b_lean_obj_arg chr, b_lean_obj_arg data,
                            uint8_t with_response, uint32_t timeout_ms, lean_obj_arg w) {
  @autoreleasepool {
    REQUIRE_CONN();
    CBCharacteristic *c = find_char(lean_ns(svc), lean_ns(chr));
    if (!c) return io_err(@"characteristic not found (run discover first)");
    NSData *d = [NSData dataWithBytes:lean_sarray_cptr(data) length:lean_sarray_size(data)];
    if (!with_response) {
      [gC.peripheral writeValue:d forCharacteristic:c type:CBCharacteristicWriteWithoutResponse];
      return io_unit();
    }
    [gC.cond lock];
    gC.opDone = NO;
    gC.lastError = nil;
    [gC.cond unlock];
    [gC.peripheral writeValue:d forCharacteristic:c type:CBCharacteristicWriteWithResponse];
    [gC.cond lock];
    BOOL done = wait_until(timeout_ms, ^BOOL { return gC.opDone; });
    NSString *err = gC.lastError;
    [gC.cond unlock];
    if (!done) return io_err(@"write timed out");
    if (err) return io_err([@"write failed: " stringByAppendingString:err]);
    return io_unit();
  }
}

// Ble.read : @& String → @& String → UInt32 → IO ByteArray
lean_obj_res lean_ble_read(b_lean_obj_arg svc, b_lean_obj_arg chr, uint32_t timeout_ms,
                           lean_obj_arg w) {
  @autoreleasepool {
    REQUIRE_CONN();
    CBCharacteristic *c = find_char(lean_ns(svc), lean_ns(chr));
    if (!c) return io_err(@"characteristic not found (run discover first)");
    [gC.cond lock];
    gC.opDone = NO;
    gC.lastError = nil;
    gC.readValue = nil;
    gC.readTarget = c;
    [gC.cond unlock];
    [gC.peripheral readValueForCharacteristic:c];
    [gC.cond lock];
    BOOL done = wait_until(timeout_ms, ^BOOL { return gC.opDone; });
    NSString *err = gC.lastError;
    NSData *v = gC.readValue;
    gC.readTarget = nil;
    [gC.cond unlock];
    if (!done) return io_err(@"read timed out");
    if (err) return io_err([@"read failed: " stringByAppendingString:err]);
    return lean_io_result_mk_ok(mk_bytes(v ?: [NSData data]));
  }
}

// Ble.nextNotification : UInt32 → IO (Option (String × ByteArray))
// `none` on timeout; throws once the link is down and the queue is drained.
lean_obj_res lean_ble_next_notification(uint32_t timeout_ms, lean_obj_arg w) {
  @autoreleasepool {
    REQUIRE_INIT();
    [gC.cond lock];
    wait_until(timeout_ms, ^BOOL { return gC.notes.count > 0 || !gC.connected; });
    RTNote *n = gC.notes.firstObject;
    if (n) [gC.notes removeObjectAtIndex:0];
    BOOL conn = gC.connected;
    NSString *err = gC.lastError;
    [gC.cond unlock];
    if (!n) {
      if (!conn) return io_err(err ?: @"disconnected");
      return lean_io_result_mk_ok(lean_box(0)); // none
    }
    lean_object *pair = lean_alloc_ctor(0, 2, 0);
    lean_ctor_set(pair, 0, mk_str(n.uuid));
    lean_ctor_set(pair, 1, mk_bytes(n.data));
    lean_object *some = lean_alloc_ctor(1, 1, 0);
    lean_ctor_set(some, 0, pair);
    return lean_io_result_mk_ok(some);
  }
}

// Ble.disconnect : IO Unit
lean_obj_res lean_ble_disconnect(lean_obj_arg w) {
  @autoreleasepool {
    if (gC && gC.peripheral) {
      [gC.central cancelPeripheralConnection:gC.peripheral];
      [gC.cond lock];
      wait_until(2000, ^BOOL { return !gC.connected; });
      [gC.cond unlock];
    }
    return io_unit();
  }
}
