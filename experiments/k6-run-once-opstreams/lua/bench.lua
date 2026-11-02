-- K6 stream 3 (predictable control): the project's Lua benchmark set (e13 bench/*.lua),
-- sizes reduced so the full trace stays near 10M instructions; run in one process.
collectgarbage("stop")  -- keeps prototypes alive so pc identities are never reused
local dir = arg[1]
for _, b in ipairs{"fib", "binary_trees", "nbody", "spectral_norm", "strings"} do
  dofile(dir .. "/" .. b .. ".lua")
end
