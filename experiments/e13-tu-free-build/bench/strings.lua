-- string ops: concatenation, format, find/gsub, interning, table.concat
local parts, total = {}, 0
for i = 1, 240000 do
  local s = string.format("item-%d:%x", i, i * 7)
  local k = s:gsub("%d", "#")
  if k:find("item", 1, true) then total = total + #k end
  parts[#parts + 1] = s:upper():sub(2, 8)
  if i % 1000 == 0 then
    local joined = table.concat(parts, ",")
    total = total + #joined
    parts = {}
  end
end
local acc = ""
for i = 1, 3000 do acc = acc .. tostring(i % 10) end
print(total, #acc, acc:sub(1, 20))
