-- Replay a player's runner session on the real CD-i (MAME). Make the segment file with
-- tools/llrec_to_route.py; it holds "t dir button duration" lines in runner seconds.
-- Environment: SEGS (file), OFFSET (seconds MAME runs behind the runner, default 19: the
-- BIOS boot plus the PLAY CD-I click), FROM/SNAP/STOP for snapshots.
-- Movement is approximate (direction only), but the game steps in whole moves every few
-- fields, so a route usually follows the player's path closely enough to reach the same spot.
local ports = manager.machine.ioport.ports
local mx = ports[":MOUSEX"].fields["Mouse X"]
local my = ports[":MOUSEY"].fields["Mouse Y"]
local b1 = ports[":MOUSEBTN"].fields["Button 1"]
local off = tonumber(os.getenv("OFFSET") or "19")
local snap = tonumber(os.getenv("SNAP") or "1")
local from = tonumber(os.getenv("FROM") or "0")
local stop = tonumber(os.getenv("STOP") or "120")
local segs = {}
for line in io.lines(os.getenv("SEGS")) do
  local t, d, b, len = line:match("([%d%.]+) (%S+) (%d) ([%d%.]+)")
  if t then segs[#segs + 1] = { t = tonumber(t) + off, d = d, b = tonumber(b), len = tonumber(len) } end
end
local x, y, nexts, i = 20000, 20000, from, 1
mx:set_value(x); my:set_value(y)
emu.register_frame_done(function()
  local t = manager.machine.time:as_double()
  if t > 12 and t < 12.5 then x = x + 1.5 end
  local press = t > 14 and t < 14.3
  while i < #segs and t >= segs[i + 1].t do i = i + 1 end
  local s = segs[i]
  if s and t >= s.t then
    if s.b == 1 then press = true end
    if s.d:find("l") then x = x - 30 end
    if s.d:find("r") then x = x + 30 end
    if s.d:find("u") then y = y - 30 end
    if s.d:find("d") then y = y + 30 end
  end
  mx:set_value(math.floor(x)); my:set_value(math.floor(y))
  b1:set_value(press and 1 or 0)
  if t >= nexts then manager.machine.video:snapshot(); print(string.format("snap %.2f", t)); nexts = nexts + snap end
  if t > stop then manager.machine:exit() end
end)
