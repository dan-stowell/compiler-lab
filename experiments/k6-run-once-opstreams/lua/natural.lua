-- K6 stream 2 (natural): run the main chunk (module init) of every file in a list of real Lua
-- modules (Neovim runtime + LuaJIT jit/*), each once, under a permissive stub for `vim` and
-- `require` so editor/JIT APIs resolve to a proxy. Only PCs that execute exactly once are
-- kept by the converter; everything else (stub metamethods, loops) collapses to effects.
collectgarbage("stop")
local P, mt = nil, {}
local function ret() return P end
mt.__index = ret; mt.__call = ret; mt.__newindex = function() end
for _, e in ipairs{"__add","__sub","__mul","__div","__mod","__pow","__unm","__idiv","__band",
  "__bor","__bxor","__shl","__shr","__bnot"} do mt[e] = ret end
mt.__concat = function() return "" end; mt.__len = function() return 0 end
mt.__eq = function() return false end; mt.__lt = function() return false end; mt.__le = function() return false end
P = setmetatable({}, mt)
vim = P; jit = P; bit = P; ffi = P
require = function() return P end
local ok_n, n = 0, 0
for path in io.lines(arg[1]) do
  local f = loadfile(path)
  if f then n = n + 1; if pcall(f, "mod") then ok_n = ok_n + 1 end end
end
print("modules", n, "completed", ok_n)
