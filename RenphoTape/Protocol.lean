/-!
# RENPHO tape frame decoding

Pure, no I/O. The only publicly documented RENPHO tape protocol is the ASCII
frame of the older `ES_TAPE` (RF-BMF01), notified on characteristic
`0783B03E-8535-B5A0-7140-A304D2495CB8`:

    *LLLLL;00000;0000<P|S><M|I>\n

`LLLLL` is the length in hundredths of a centimetre, `P` marks a live update
and `S` a reading committed with the OK button. The last letter is the display
unit, `M` metric or `I` inches; the digits stay in hundredths of a centimetre
either way (a 10 cm pull sent `01000` in both modes; the display read 3.93 in). The R-Y001 uses the same service and frame (checked on
a real unit, 2026-11-13, which sent `*00000;00000;0000PI`). Frames that do not
match are left undecoded and the client prints them raw.
-/

namespace RenphoTape

/-- `ES_TAPE` service and its notify characteristic. -/
def esTapeService : String := "0783B03E-8535-B5A0-7140-A304D2495CB7"
def esTapeNotify : String := "0783B03E-8535-B5A0-7140-A304D2495CB8"

/-- A decoded tape reading. -/
structure Reading where
  /-- Length in hundredths of a centimetre (0.1 mm). -/
  centiCm : Nat
  /-- `true` when the reading was committed with the OK button (`S` frame). -/
  committed : Bool
  /-- Trailing flag letters as sent, for example `"PM"` or `"SM"`. -/
  flags : String
  deriving Repr, BEq, DecidableEq

namespace Reading

def cm (r : Reading) : Float := r.centiCm.toFloat / 100.0
def inches (r : Reading) : Float := r.cm / 2.54

/-- Plain decimal centimetres, for example `10.00`. -/
def cmText (r : Reading) : String :=
  let frac := r.centiCm % 100
  s!"{r.centiCm / 100}.{if frac < 10 then "0" else ""}{frac}"

/-- `true` when the unit letter is `I` (tape display switched to inches). -/
def imperial (r : Reading) : Bool := r.flags.toList.contains 'I'

private def fixed2 (n : Nat) : String :=
  s!"{n / 100}.{if n % 100 < 10 then "0" else ""}{n % 100}"

/-- Fixed-point rendering, for example `10.00 cm`. The digits are hundredths of
a centimetre in both unit modes; in inch mode the inch value is appended,
truncated to two places as the tape display does (`10.00 cm (3.93 in)`). -/
def render (r : Reading) : String :=
  let cm := s!"{fixed2 r.centiCm} cm"
  if r.imperial then s!"{cm} ({fixed2 (r.centiCm * 100 / 254)} in)" else cm

end Reading

private def isDigit (b : UInt8) : Bool := 0x30 ≤ b && b ≤ 0x39
private def isUpper (b : UInt8) : Bool := 0x41 ≤ b && b ≤ 0x5A

/-- Decode one ASCII tape frame. `none` when the bytes are not such a frame. -/
def decodeFrame (bytes : List UInt8) : Option Reading :=
  match bytes with
  | 0x2A :: rest =>
    let digits := rest.takeWhile isDigit
    match rest.dropWhile isDigit with
    | 0x3B :: tail =>
      if digits.length != 5 then none
      else
        let flags := tail.filter isUpper
        some {
          centiCm := digits.foldl (fun acc d => acc * 10 + (d.toNat - 0x30)) 0
          committed := flags.contains 0x53
          flags := String.ofList (flags.map fun b => Char.ofNat b.toNat) }
    | _ => none
  | _ => none

def decode (payload : ByteArray) : Option Reading := decodeFrame payload.toList

/-- Lowercase hex, bytes separated by spaces. -/
def hex (payload : ByteArray) : String :=
  let digit (n : Nat) : Char := (Nat.toDigits 16 n).headD '0'
  " ".intercalate (payload.toList.map fun b =>
    String.ofList [digit (b.toNat / 16), digit (b.toNat % 16)])

/-- Printable ASCII view; other bytes become `.`. -/
def ascii (payload : ByteArray) : String :=
  String.ofList (payload.toList.map fun b =>
    if 0x20 ≤ b && b < 0x7F then Char.ofNat b.toNat else '.')

/-- Parse hex such as `"a5 01"`, `"A501"` or `"0xa5,0x01"`. -/
def parseHex (s : String) : Option ByteArray :=
  let val (c : Char) : Option Nat :=
    if '0' ≤ c && c ≤ '9' then some (c.toNat - '0'.toNat)
    else if 'a' ≤ c && c ≤ 'f' then some (c.toNat - 'a'.toNat + 10)
    else if 'A' ≤ c && c ≤ 'F' then some (c.toNat - 'A'.toNat + 10)
    else none
  let rec go : List Char → ByteArray → Option ByteArray
    | [], acc => some acc
    | '0' :: 'x' :: rest, acc => go rest acc
    | c :: rest, acc =>
      if c == ' ' || c == ',' || c == ':' then go rest acc
      else match rest with
        | d :: rest' =>
          match val c, val d with
          | some hi, some lo => go rest' (acc.push (UInt8.ofNat (hi * 16 + lo)))
          | _, _ => none
        | [] => none
  go s.toList ByteArray.empty

/-! Captured `ES_TAPE` frames, checked at build time. -/

private def frame (s : String) : List UInt8 := s.toUTF8.toList

#guard decodeFrame (frame "*00000;00000;0000PM\n") == some ⟨0, false, "PM"⟩
#guard decodeFrame (frame "*00510;00000;0000PM\n") == some ⟨510, false, "PM"⟩
#guard decodeFrame (frame "*01240;00000;0000PM\n") == some ⟨1240, false, "PM"⟩
#guard decodeFrame (frame "*11510;00000;0000PM\n") == some ⟨11510, false, "PM"⟩
#guard decodeFrame (frame "*00650;00000;0000SM\n") == some ⟨650, true, "SM"⟩
#guard decodeFrame (frame "00650;00000;0000SM\n") == none
#guard decodeFrame (frame "*0065;0") == none
#guard decodeFrame (frame "*00000;00000;0000PI\n") == some ⟨0, false, "PI"⟩
#guard (decodeFrame (frame "*01000;00000;0000PI\n")).map (·.render) == some "10.00 cm (3.93 in)"
#guard decodeFrame (frame "*01000;00000;0000SM\n") == some ⟨1000, true, "SM"⟩
#guard (parseHex "0xa5, 01 FF").map hex == some "a5 01 ff"
#guard (decodeFrame (frame "*01240;00000;0000PM\n")).map (·.render) == some "12.40 cm"

end RenphoTape
