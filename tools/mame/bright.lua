-- Print the mean brightness of the screen every frame ("B time value"), from FROM to STOP
-- seconds, after clicking PLAY CD-I. Cheap way to time fades and image changes on the real
-- CD-i without writing thousands of snapshots: grep '^B' and compare with the runner.
local scr = manager.machine.screens[":screen"]
local stop = tonumber(os.getenv("STOP") or "80")
local from = tonumber(os.getenv("FROM") or "35")
local ports = manager.machine.ioport.ports
local mx = ports[":MOUSEX"].fields["Mouse X"]
local b1 = ports[":MOUSEBTN"].fields["Button 1"]
local x = 0
emu.register_frame_done(function()
  local t = manager.machine.time:as_double()
  if t > 12 and t < 12.5 then x = x + 1.5; mx:set_value(math.floor(x)) end
  b1:set_value((t > 14 and t < 14.3) and 1 or 0)
  if t > from then
    local px, w, h = scr:pixels(), scr.width, scr.height
    local s, n = 0, 0
    for yy = 100, h - 60, 16 do
      for xx = 40, w - 40, 16 do
        local o = (yy * w + xx) * 4 + 1
        local b, g, r = string.byte(px, o, o + 2)
        s = s + r + g + b; n = n + 1
      end
    end
    print(string.format("B %.3f %.1f", t, s / n / 3))
  end
  if t > stop then manager.machine:exit() end
end)
