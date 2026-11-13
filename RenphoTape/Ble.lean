/-!
# Bluetooth LE central (CoreBluetooth shim)

Thin bindings over `ffi/ble_ffi.m`. One central, one connected peripheral.
Every call blocks until its CoreBluetooth callback arrives or the timeout
(milliseconds) expires; failures surface as `IO` errors.
-/

namespace RenphoTape

/-- One advertising peripheral seen during a scan. `id` is the CoreBluetooth
identifier (a per-host UUID; macOS hides the MAC address). -/
structure ScanHit where
  id : String
  name : String
  rssi : Int
  services : Array String
  mfg : ByteArray

/-- A GATT characteristic. `props` is the GATT property bit set. -/
structure Characteristic where
  service : String
  uuid : String
  props : Nat

namespace Characteristic

def canRead (c : Characteristic) : Bool := c.props &&& 0x02 != 0
def canWriteNoResponse (c : Characteristic) : Bool := c.props &&& 0x04 != 0
def canWrite (c : Characteristic) : Bool := c.props &&& 0x08 != 0
def canNotify (c : Characteristic) : Bool := c.props &&& 0x10 != 0
def canIndicate (c : Characteristic) : Bool := c.props &&& 0x20 != 0

def propNames (c : Characteristic) : String :=
  let names := [(c.canRead, "read"), (c.canWriteNoResponse, "write-no-response"),
    (c.canWrite, "write"), (c.canNotify, "notify"), (c.canIndicate, "indicate")]
  ", ".intercalate (names.filterMap fun (on, n) => if on then some n else none)

end Characteristic

namespace Ble

/-- Start the central and wait for the adapter to power on. -/
@[extern "lean_ble_init"]
opaque init (timeoutMs : UInt32) : IO Unit

/-- Scan for `ms` milliseconds and return every peripheral seen. -/
@[extern "lean_ble_scan"]
opaque scan (ms : UInt32) : IO (Array ScanHit)

/-- Connect to a peripheral by the `id` of a scan hit. -/
@[extern "lean_ble_connect"]
opaque connect (id : @& String) (timeoutMs : UInt32) : IO Unit

/-- Discover every service and characteristic of the connected peripheral. -/
@[extern "lean_ble_discover"]
opaque discover (timeoutMs : UInt32) : IO (Array Characteristic)

/-- Enable notifications (or indications). An empty `service` matches any. -/
@[extern "lean_ble_subscribe"]
opaque subscribe (service uuid : @& String) (timeoutMs : UInt32) : IO Unit

@[extern "lean_ble_write"]
opaque write (service uuid : @& String) (data : @& ByteArray) (withResponse : Bool)
    (timeoutMs : UInt32) : IO Unit

@[extern "lean_ble_read"]
opaque read (service uuid : @& String) (timeoutMs : UInt32) : IO ByteArray

/-- Next queued notification as `(characteristic uuid, payload)`; `none` on
timeout. Throws when the link is down and the queue is empty. -/
@[extern "lean_ble_next_notification"]
opaque nextNotification (timeoutMs : UInt32) : IO (Option (String × ByteArray))

@[extern "lean_ble_disconnect"]
opaque disconnect : IO Unit

end Ble
end RenphoTape
