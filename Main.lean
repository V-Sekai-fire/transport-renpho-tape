import RenphoTape
import Std.Time

open RenphoTape

def usage : String := "renpho-tape: Bluetooth LE client for the RENPHO smart tape (R-Y001)

  renpho-tape scan [seconds]            list nearby BLE peripherals (default 6 s)
  renpho-tape dump [device]             connect and list services and characteristics
  renpho-tape listen [device] [opts]    subscribe to every notify characteristic and
                                        print each frame (hex, ASCII, decoded length)
    --write <char-uuid>=<hex>           write bytes after subscribing (repeatable)
    --seconds <n>                       stop after n seconds (default: until disconnect)
    --all                               print repeated frames too (default: changes only)
    --json                              one JSON object per line
  renpho-tape serve [device]            listen --json forever, for a service: events
                                        {event: reading | link_lost}

<device> is a CoreBluetooth id from `scan`, or part of the advertised name.
Without it, the first peripheral whose name contains tape, renpho or r-y001 is used.
Press the tape's power button first: it advertises only while it is on."

def contains (hay needle : String) : Bool :=
  (hay.toLower.splitOn needle.toLower).length > 1

def defaultNames : List String := ["tape", "renpho", "r-y001", "ry001"]

def matchesDevice (want : Option String) (h : ScanHit) : Bool :=
  match want with
  | some w => h.id.toLower == w.toLower || (h.name != "" && contains h.name w)
  | none => defaultNames.any (contains h.name)

def showHit (h : ScanHit) : String :=
  let name := if h.name == "" then "(no name)" else h.name
  let svcs := if h.services.isEmpty then "" else s!"  services={h.services.toList}"
  let mfg := if h.mfg.isEmpty then "" else s!"  mfg={hex h.mfg}"
  s!"{h.id}  rssi={h.rssi}  {name}{svcs}{mfg}"

/-- Scan in 2 s rounds until a matching peripheral shows up. -/
def findDevice (want : Option String) (rounds : Nat := 15) : IO ScanHit := do
  IO.eprintln "scanning (turn the tape on now)..."
  for _ in [0:rounds] do
    let hits ← Ble.scan 2000
    if let some h := hits.find? (matchesDevice want) then
      IO.eprintln s!"found {showHit h}"
      return h
  throw <| IO.userError "tape not found; run `renpho-tape scan` to see what is advertising"

def connectTo (want : Option String) : IO (Array Characteristic) := do
  let h ← findDevice want
  Ble.connect h.id 10000
  IO.eprintln "connected"
  Ble.discover 10000

def showChars (chars : Array Characteristic) : IO Unit := do
  let mut service := ""
  for c in chars do
    if c.service != service then
      service := c.service
      IO.println s!"service {service}"
    IO.println s!"  characteristic {c.uuid}  [{c.propNames}]"

def cmdScan (seconds : Nat) : IO Unit := do
  let hits ← Ble.scan (UInt32.ofNat (seconds * 1000))
  for h in hits do IO.println (showHit h)
  IO.eprintln s!"{hits.size} peripherals"

def cmdDump (want : Option String) : IO Unit := do
  let chars ← connectTo want
  showChars chars
  for c in chars do
    if c.canRead then
      try
        let v ← Ble.read c.service c.uuid 3000
        IO.println s!"read {c.uuid}: {hex v}  |{ascii v}|"
      catch e => IO.println s!"read {c.uuid}: {e}"
  Ble.disconnect

structure ListenOpts where
  device : Option String := none
  writes : List (String × ByteArray) := []
  seconds : Option Nat := none
  all : Bool := false
  /-- One JSON object per line (for services and log shippers). -/
  json : Bool := false

def parseListen : List String → ListenOpts → Except String ListenOpts
  | [], o => .ok o
  | "--write" :: spec :: rest, o =>
    match spec.splitOn "=" with
    | [uuid, h] =>
      match parseHex h with
      | some bytes => parseListen rest { o with writes := o.writes ++ [(uuid, bytes)] }
      | none => .error s!"bad hex in --write {spec}"
    | _ => .error "--write expects <char-uuid>=<hex>"
  | "--seconds" :: n :: rest, o =>
    match n.toNat? with
    | some n => parseListen rest { o with seconds := some n }
    | none => .error "--seconds expects a number"
  | "--all" :: rest, o => parseListen rest { o with all := true }
  | "--json" :: rest, o => parseListen rest { o with json := true }
  | arg :: rest, o =>
    if arg.startsWith "--" then .error s!"unknown option {arg}"
    else parseListen rest { o with device := some arg }

/-- Connect, subscribe to every notify characteristic and replay `--write`s.
Returns the number of subscriptions. -/
def openSession (o : ListenOpts) (verbose : Bool) : IO Unit := do
  let chars ← connectTo o.device
  if verbose then showChars chars
  let mut subscribed := 0
  for c in chars do
    if c.canNotify || c.canIndicate then
      try
        Ble.subscribe c.service c.uuid 5000
        subscribed := subscribed + 1
        if verbose then IO.eprintln s!"subscribed {c.uuid}"
      catch e => IO.eprintln s!"subscribe {c.uuid}: {e}"
  if subscribed == 0 then
    throw <| IO.userError "no notify or indicate characteristic on this device"
  for (uuid, bytes) in o.writes do
    let withResponse := chars.any fun c => c.uuid.toLower == uuid.toLower && c.canWrite
    Ble.write "" uuid bytes withResponse 5000
    IO.eprintln s!"wrote {uuid}: {hex bytes}"

/-- The tape powers itself off after a short idle period and drops the link
(the vendor app sends nothing to prevent this; it reconnects). So a lost link
is normal: wait for the tape to advertise again and resume. -/
def cmdListen (o : ListenOpts) : IO Unit := do
  let start ← IO.monoMsNow
  let expired : IO Bool := do
    match o.seconds with
    | some s => return (← IO.monoMsNow) - start ≥ s * 1000
    | none => return false
  openSession o true
  IO.eprintln "listening; pull the tape (ctrl-c to stop)"
  let out ← IO.getStdout
  let mut last : Option ByteArray := none
  repeat
    if ← expired then break
    let next ← try
        pure (Except.ok (← Ble.nextNotification 500))
      catch e => pure (Except.error e)
    match next with
    | .error e =>
      if o.json then
        let ts := (← Std.Time.Timestamp.now).toMillisecondsSinceUnixEpoch.val
        out.putStrLn s!"\{\"event\":\"link_lost\",\"ts_ms\":{ts}}"
        out.flush
      IO.eprintln s!"link lost ({e}); tape asleep, press its power button to resume"
      last := none
      let mut up := false
      while !up do
        if ← expired then break
        try
          openSession o false
          up := true
        catch _ => pure ()
      if !up then break
    | .ok none => pure ()
    | .ok (some (uuid, payload)) =>
      -- The tape repeats its frame about ten times a second: print changes only.
      if !o.all && last == some payload then continue
      last := some payload
      if o.json then
        let ts := (← Std.Time.Timestamp.now).toMillisecondsSinceUnixEpoch.val
        let body := match decode payload with
          | some r => s!"\"centi_cm\":{r.centiCm},\"cm\":\"{r.cmText}\",\"committed\":{r.committed},\"display_unit\":\"{if r.imperial then "in" else "cm"}\""
          | none => s!"\"undecoded\":true"
        out.putStrLn s!"\{\"event\":\"reading\",\"ts_ms\":{ts},{body},\"raw\":\"{hex payload}\"}"
        out.flush
        continue
      let decoded := match decode payload with
        | some r => s!"  => {r.render}{if r.committed then " (saved)" else ""}"
        | none => ""
      IO.println s!"{(← IO.monoMsNow) - start} ms  {uuid}  {hex payload}  |{ascii payload}|{decoded}"
      out.flush
  Ble.disconnect

def run : List String → IO UInt32
  | "scan" :: rest => do
    let seconds := (rest.head? >>= String.toNat?).getD 6
    Ble.init 5000
    cmdScan seconds
    return 0
  | "dump" :: rest => do
    Ble.init 5000
    cmdDump rest.head?
    return 0
  | "serve" :: rest => do
    match parseListen rest { json := true } with
    | .error e => IO.eprintln e; return 2
    | .ok o =>
      Ble.init 5000
      -- A service never gives up: if the adapter or scan fails, retry later.
      repeat
        try cmdListen o
        catch e => IO.eprintln s!"serve: {e}; retrying in 5 s"
        IO.sleep 5000
      return 0
  | "listen" :: rest => do
    match parseListen rest {} with
    | .error e => IO.eprintln e; return 2
    | .ok o =>
      Ble.init 5000
      cmdListen o
      return 0
  | _ => do
    IO.println usage
    return 2

def main (args : List String) : IO UInt32 := do
  try run args
  catch e =>
    IO.eprintln s!"error: {e}"
    return 1
