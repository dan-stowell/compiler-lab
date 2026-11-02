-- binary-trees (benchmarks game shape): table allocation and GC
local function make(d)
  if d == 0 then return {} end
  d = d - 1
  return { make(d), make(d) }
end
local function check(t)
  if t[1] then return 1 + check(t[1]) + check(t[2]) end
  return 1
end
local N = 9
local maxd = N
print("stretch", check(make(maxd + 1)))
local long = make(maxd)
for d = 4, maxd, 2 do
  local iters, c = 1 << (maxd - d + 4), 0
  for _ = 1, iters do c = c + check(make(d)) end
  print(iters, d, c)
end
print("long", check(long))
