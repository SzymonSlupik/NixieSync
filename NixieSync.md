# NixieSync — firmware and commissioning

NTP time-set module for a nixie clock built around an ST62T65C6. The module
watches for clock resets, fetches time over WiFi, and reproduces button presses
on the clock's own set switch to put it back to the right time.

Hardware is in **NixieSync-PCB-layout.pdf** — schematic, board, netlist, BOM.
This file covers the firmware, the timing algorithm, and bringing it up.

---

## 1. The clock, as measured

| | |
|---|---|
| Controller | ST62T65C6, 3.579545 MHz crystal, VDD 5 V |
| Reset supervisor | MC33064-5, open collector, ~4.6 V threshold |
| Display | 24 h, hours/minutes/seconds tubes |
| After reset | always `00:00:00`, repeatable — confirmed for both a power cycle and an external RESET pulse |
| Set control | 3-position momentary toggle, common at ground |
| HOUR and MIN lines | pulled up to VDD through 1 kΩ; PC1 has a further 1 kΩ in series |
| Hour advance | +1 hour. Disturbs neither minutes nor seconds |
| Minute advance | +1 minute, **and zeroes the seconds** |

Three properties of this clock do most of the work in the algorithm below:

- **Reset gives a known zero.** The module has no way to read the display, so
  everything starts from a forced `00:00:00`.
- **Hours are decoupled from everything else.** Hour presses are pure +1 with
  no side effects, so they need no timing precision at all.
- **Minute presses freeze the clock's minute.** Each one restarts the 60-second
  seconds window, so a minute train that lands on a true minute boundary sets
  minutes and seconds simultaneously and exactly.

---

## 2. Timing algorithm

### 2.1 Full set — after any clock reset

The clock reads `00:00:00` the moment RESET is released. Pick a future minute
boundary `T_end`; let `H = hour(T_end)`, `M = minute(T_end)`.

- `pulseH = H` — hour presses are pure +1 from zero.
- `pulseM = M` — the clock's minute is still `00` when the first minute press
  lands, because the hour train is at most 23 × 300 ms = 6.9 s.

**If M > 0:** one contiguous train, hours then minutes, scheduled *backwards* so
the last minute press lands exactly on `T_end`. That press zeroes the seconds at
the boundary, so minutes and seconds both come right in the same instant.

**If M == 0:** there is no minute press to zero the seconds, so instead release
RESET exactly on `T_end` and run the hour train afterwards. This is only safe
because hour presses don't perturb minutes or seconds — it is the one piece of
`planFullSet()` that isn't obvious on reading.

No wrap is possible anywhere: hours run 0→H ≤ 23 and minutes 0→M ≤ 59, so there
is no carry into hours and no dependence on the clock's carry behaviour.

### 2.2 DST — hour presses only

Because hour presses touch neither minutes nor seconds, a DST correction is just
`Δ mod 24` hour presses fired whenever. No boundary alignment, no minute
correction, no reset, no fixed-point solving. Spring forward is 1 press; fall
back is 23 presses over about 7 s.

### 2.3 Scheduling precision

Every edge is scheduled against an absolute wall-clock microsecond deadline
derived from the SNTP-disciplined system clock, never against relative delays,
so error cannot accumulate across an 82-pulse train. SNTP runs in smooth
(adjtime) mode after the first sync so it cannot step the clock out from under
an in-flight train.

---

## 3. What triggers a sync

| Trigger | Action |
|---|---|
| Boot, once WiFi and NTP are up | full set |
| RESET line seen asserted by something other than us | full set |
| UTC offset changes (DST) | hour presses only, unless `dstUseReset` |
| Short press on SW1, or `/sync` | full set |
| Daily at a configured time | full set — off by default |

The RESET-line watch is what closes the awkward case: a mains dip deep enough to
trip the clock's MC33064 but not the module would otherwise leave the display
stuck at `00:00` until the next reboot. Q4 buffers that line so the sense path
loads it with ~4.8 MΩ of gate pull-down rather than a divider — this is the one
net whose loading could hold the clock in reset permanently, so the sense path
is deliberately kept out of its DC operating point.

### Coming back from an outage

The ESP32 is up in about two seconds; the router is not. Three things cover the
gap, and they cover different parts of it:

1. **Association.** `connectSTA()` retries for a full five minutes, 20 s per
   attempt with a 3 s backoff, before giving up.
2. **Late NTP.** Association is not the same as having time. The AP can be up in
   a minute while the router's WAN link takes several more, so `startNtp()`
   waiting 60 s is not enough on its own. SNTP keeps trying in the background,
   and `bootSyncPending` makes the main loop set the clock the moment time
   becomes valid — however long that takes. Without this the clock would sit at
   `00:xx` until the next button press or DST change.
3. **No AP fallback.** If association fails outright the module keeps retrying
   every 30 s indefinitely and never raises an AP. Reset sensing carries on
   throughout, so a clock reset during the outage is still noticed and acted on
   once the network returns.

---

## 4. Configuration

With nothing stored, the module raises an open AP named `NixieSync-XXXX` and the
captive portal should pop up on connecting. Once credentials are saved the AP is
**never raised automatically** — see below. The same page is then served on the
LAN at `http://nixie.local/` or the DHCP address.

### The button

Three actions, chosen by how long SW1 is held. The LED shows which one is armed
while you hold it, so you can release before committing to the wrong one.

| Hold | Action | LED while held |
|---|---|---|
| < 1.5 s | re-sync the clock now | unchanged |
| 1.5–8 s | open the configuration portal | slow blink |
| > 8 s | erase credentials and reboot | fast blink |

Both edges are debounced over 25 ms. Committing on a single sample meant that
contact chatter part-way through a hold read as a release and fired whichever
action was armed at that instant — a hold meant for the wipe would open the
portal instead. If SW1 still chatters, tack a 100 nF capacitor across its
terminals: the internal pull-up is only ~45 kΩ, which is a high-impedance node
to run inside a chassis containing a 200 V boost converter.

### What the LED means

One function owns it, so it always reflects the current state rather than
whatever the last blink left behind.

| Pattern | Meaning |
|---|---|
| solid | associated and idle |
| slow blink, ~0.7 s | not associated; retrying |
| blink, ~0.12 s | busy setting the clock |
| blink, ~0.15 s | configuration portal is open |
| while the button is held | which action is armed (see above) |

### Why the portal needs a physical press

An AP that appears whenever association fails is an attack surface that shows up
exactly when the network is least healthy — and a deauth from within radio range
would be enough to summon it on demand. So once credentials are stored the
module retries association indefinitely instead, and the portal is reachable
only by holding the button.

The on-demand portal then closes itself: after ten minutes, provided nobody has
loaded a page for three minutes, the module reboots and rejoins the network. It
will not restart out from under you mid-configuration.

The AP is left open rather than WPA2-protected, because a passworded AP breaks
the captive-portal popup on both iOS and Android and would make setup
considerably more awkward. With physical presence required and a ten-minute
life, that trade seems right — but `AP_PASSWORD` at the top of the sketch takes
a string if you disagree.

### WPA2-Enterprise (PEAP / MSCHAPv2)

Select it under Security and fill in the username and password. The anonymous
identity is optional and defaults to the username. Association is attempted with
`WiFi.begin(ssid)` after the EAP client is configured; everything else in the
firmware is unchanged.

**Paste the RADIUS server's CA certificate.** Without it the server is not
validated at all, so anything advertising your SSID can complete a PEAP
handshake with the module. MSCHAPv2's challenge/response is offline-crackable,
so a rogue AP within radio range recovers the password and the NT hash — and on
most networks those are *domain* credentials, not just WiFi ones. The field
accepts a PEM blob stored separately in NVS.

Two consequences of using enterprise credentials on a device like this, worth
weighing before you do:

- They sit in plaintext flash inside a clock on a shelf, readable by anyone who
  opens the case and attaches a programmer. NVS encryption exists but requires
  flash encryption, which complicates reflashing considerably.
- The module calls `EAP_NO_TIME_CHECK(true)` because it has no battery-backed
  RTC: after a power cut it boots at 1970 and cannot learn the time until it
  joins the network it is trying to join. Certificate expiry is therefore not
  checked. The CA identity check still is, which is what stops the rogue-AP
  attack — but an expired-but-genuine server certificate will be accepted.

If your network offers an IoT SSID with a PSK, or a MAC exception onto an
isolated VLAN, that is the better answer for an appliance like this.

**Residual exposure worth knowing about:** while joined to your LAN the config
page has no authentication, so anyone on the network can re-sync the clock, fire
a test pulse, or change settings. For a clock on a home network that is probably
fine; if it isn't, HTTP basic auth on `/save`, `/sync` and `/test` is a small
addition.

| Field | Default | Notes |
|---|---|---|
| SSID | — | scan list, or type a hidden SSID |
| Security | WPA2-PSK | or WPA2-Enterprise, PEAP/MSCHAPv2 |
| PSK passphrase | — | WPA2-PSK only |
| Anonymous identity | — | outer identity; blank uses the username |
| Username / password | — | inner MSCHAPv2 credentials |
| RADIUS CA certificate | — | PEM; see the warning below |
| NTP servers | `pool.ntp.org`, `time.google.com` | |
| Timezone | `CET-1CEST,M3.5.0,M10.5.0/3` | POSIX TZ; this one is Poland |
| `resetHoldMs` | 50 | the ST6 adds its own delay to the pin |
| `powerOnLagMs` | 0 | RESET releases this early; see §5 |
| `pulseOnMs` / `pulseOffMs` | 120 / 180 | |
| `postResetMs` | 300 | settle before the first pulse |
| `senseEnable` | on | watch the clock's RESET line |
| `senseInverted` | on | Q4 buffer: pin HIGH means in reset |
| `dstUseReset` | off | hour presses are less disruptive |
| `dailyEnable` | off | belt and braces; blanks nothing, but resets the clock |
| `dryRun` | **on** | ships safe — turn off after bench testing |

---

## 5. Bringing it up

1. **Flash with `dryRun` on.** The firmware logs the complete plan over serial
   at 115200 — pulse counts, every scheduled timestamp, and achieved versus
   planned end time — while driving no outputs. Check the arithmetic at a few
   times of day, particularly just before the top of an hour, which exercises
   the `M == 0` branch.
2. **Check the outputs before connecting the clock.** With `dryRun` off, use
   `/test?r=h` and `/test?r=m` to fire single pulses and scope the drains.
3. **Connect and run a full sync.** Expect the display to blink, then count up
   hours and minutes over roughly 25 s, landing on the correct time with the
   seconds tubes reading `00`.
4. **Calibrate `powerOnLagMs`** only if the clock lands consistently late. The
   ST6's post-reset startup delay is 2048 or 32768 oscillator cycles depending
   on the DELAY option bit — 0.57 ms or 9.2 ms at 3.579545 MHz, both small
   enough to start at zero.
5. **LED1 must be red or amber**, Vf around 1.9 V. R9 = 470 ohm then gives
   about 3 mA, which reads clearly inside the case. A green or blue part at Vf ~3.0 V has only a couple of hundred
   millivolts of headroom against the 3.3 V GPIO, so it is barely lit and the
   current swings with tolerance and temperature. If the indicator looks faint,
   measure across LED1 while lit: ~1.9 V means red and R9 is the whole story;
   ~3.0 V means the LED itself is wrong.
6. **Pulse width.** 120 / 180 ms is conservative. There is little to gain from
   shortening it, since the worst-case train is already under 25 s.

## 6. Powering the module

Two options; the board supports either.

**USB-C alone.** Leave J2-5 unconnected. Simplest, and it decouples the module
from the clock entirely.

**From the clock's 78M05.** Wire its 5 V output to J2-5. D1 on the module blocks
back-feed into a host's VBUS, so a programming lead and the clock's rail can be
connected at the same time — whichever is higher wins, and with USB absent the
node sits about 0.3 V down, which the XIAO's LDO absorbs.

Sheet 3 of the PDF has the block diagram and the checks. The one worth
repeating: **the 5 V rail is what the MC33064 supervises.** It holds the ST62 in
reset below ~4.6 V, and WiFi transmit peaks of a few hundred mA are drawn
straight through the XIAO's linear LDO from that same rail. If a peak dips it
past the threshold the clock resets — and the module then sees a foreign reset
and re-syncs it, so the fault self-heals and presents as the clock occasionally
resetting for no reason.

The diagnostic is already built in. Watch the serial log for

    [sense] foreign reset seen, scheduling a full set

for a few days after wiring it up. If it appears when nothing else explains it,
the rail is dipping.

Mitigations, in order of effort:

- Fit C1 at 100 uF rather than 47 uF, so transmit peaks come from local bulk
  instead of down the feed wire. The 1210 footprint takes either.
- `WiFi.setSleep(false)` in `connectSTA()` forces the radio fully awake.
  Changing it to `true` enables modem sleep and roughly halves the average
  draw, at some latency cost on the config page.
- Keep the 5 V feed short and thick.

Also check the 78M05's dissipation, (Vin - 5) x I: at 11 V input and 90 mA that
is 0.5 W, but a lightly loaded 9 V wall unit can give 15 V DC and 0.9 W, and the
ambient inside a sealed chassis warmed by six tubes is not 25 C. Measure the tab
temperature before and after.

## 7. Testing the reset sense

The status page shows both the interpretation and the raw pin, which is what
makes a polarity or wiring fault obvious:

    Clock RESET line released (running)  (D1 pin reads LOW, foreign resets since boot 0)

Expected states, with Q4 as an inverter (clock RESET high -> Q4 on -> D1 low):

| Clock RESET net | D1 pin | Page says |
|---|---|---|
| ~5 V, running | LOW | released (running) |
| pulled low, in reset | HIGH | asserted (in reset) |

**End-to-end test:** click `test reset pulse` on the status page, or fetch
`/test?r=r`. That fires Q3 for `resetHoldMs`, which is deliberately *not* masked
by the syncing flag, so the sense path sees it too. The clock should blink to
`00:00:00`, and the serial log should show:

    [sense] RESET asserted
    [sense] RESET released - foreign reset #1, scheduling a full set
    [sync] RESET+SET target ...

That exercises Q3, Q4 and both wires in one click. If the clock resets but no
`[sense]` line appears, the drive works and the sense does not.

**Manual test:** touch TP3 to ground through 1 kohm for a second. Same result,
without involving Q3.

Fault-finding, in order:

- **Page always says "asserted", D1 always HIGH.** Q4 is never turning on. Most
  likely J2-2 is not actually on the ST62's RESET pin, so the gate sits at 0 V
  through R8. Measure Q4's gate: it should be ~4.9 V with the clock running
  (5 V divided by R7 and R8). Also check R7 is 100 k and R8 is 4M7, not
  swapped — reversed, the gate would sit near zero.
- **Page always says "released", D1 always LOW.** Q4 is stuck on, or R10 is
  missing so the drain has nothing to pull it up. Check 3V3 is present at R10.
- **D1 reads correctly but nothing is logged.** `senseEnable` is off in the
  config, or `senseInverted` has been switched to the bare-divider setting.

Note that `foreign resets since boot` is also the counter to watch if you are
running the module from the clock's 78M05 — see section 6.

## 8. Known unknown

**Is there a capacitor to ground near PC1?** With the 1 kΩ already in series
there, one would form an RC filter, and its time constant sets the shortest
pulse the clock will reliably register. It is the one thing that could force
`pulseOnMs` above 120 ms. Because the minute train is what lands on the
boundary, a filter slower than expected would show up as a consistent offset
rather than an obvious failure — so if the clock ends up reproducibly a minute
or two out, look here before anything else.

---

## 9. Build

Arduino IDE with the Espressif ESP32 core 3.x, board **XIAO_ESP32C3**. No
third-party libraries — WiFi, WebServer, DNSServer, Preferences, ESPmDNS and
esp_sntp all come from the core.

Note that `struct Config` and `struct Plan` are both declared above the first
function definition. The IDE injects generated prototypes at exactly that point,
so a struct defined further down the file produces `does not name a type` on its
own prototype.
