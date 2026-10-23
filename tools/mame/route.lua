-- Real CD-i (MAME cdimono1): click PLAY CD-I, press buttons, move the mouse, take snapshots.
-- Environment:
--   PRESSES  button-1 presses in seconds (default "14,42,65": PLAY CD-I in the Philips shell at
--            14 s, then Laser Lords' title skip and Beamdown; set "14" to only start the disc)
--   ROUTE    timed moves "t:dir:secs,..." with dir l/r/u/d (or b1/b2 presses), e.g. "80:r:3"
--   SNAP     snapshot interval in seconds (default 0.5), STOP end time (default 120)
-- The mouse is relative: only the change per frame matters, so the counters start at 20000
-- (MAME clamps negative values). About 1 unit = 1 pixel in the shell; 30 per frame walks.
-- The shell's pointer starts just left of PLAY CD-I; +37 units over 12-12.5 s puts it on it.
local ports = manager.machine.ioport.ports
local mx = ports[":MOUSEX"].fields["Mouse X"]
local my = ports[":MOUSEY"].fields["Mouse Y"]
local b1 = ports[":MOUSEBTN"].fields["Button 1"]
local b2 = ports[":MOUSEBTN"].fields["Button 2"]
local steps = {}
for t, k, a in string.gmatch(os.getenv("ROUTE") or "", "([%d%.]+):(%w+):?([%d%.]*)") do
  steps[#steps + 1] = { t = tonumber(t), k = k, d = tonumber(a) or 0.3 }
end
local presses = {}
for v in string.gmatch(os.getenv("PRESSES") or "14,42,65", "[%d%.]+") do presses[#presses + 1] = tonumber(v) end
local snap = tonumber(os.getenv("SNAP") or "0.5")
local stop = tonumber(os.getenv("STOP") or "120")
local nexts, x, y = 0, 20000, 20000
mx:set_value(x); my:set_value(y)
emu.register_frame_done(function()
  local t = manager.machine.time:as_double()
  if t > 12 and t < 12.5 then x = x + 1.5 end
  local p1, p2 = false, false
  for _, p in ipairs(presses) do if t > p and t < p + 0.3 then p1 = true end end
  for _, s in ipairs(steps) do
    if t > s.t and t < s.t + s.d then
      if s.k == "l" then x = x - 30 elseif s.k == "r" then x = x + 30
      elseif s.k == "u" then y = y - 30 elseif s.k == "d" then y = y + 30
      elseif s.k == "b1" then p1 = true elseif s.k == "b2" then p2 = true end
    end
  end
  mx:set_value(math.floor(x)); my:set_value(math.floor(y))
  b1:set_value(p1 and 1 or 0); b2:set_value(p2 and 1 or 0)
  if t >= nexts then
    manager.machine.video:snapshot()
    print(string.format("snap %.3f", t))
    nexts = nexts + snap
  end
  if t > stop then manager.machine:exit() end
end)
