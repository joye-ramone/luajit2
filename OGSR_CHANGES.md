# OGSR changes to LuaJIT GC

Fork changes on top of `xray` (openresty luajit2 + solbjorn `gc-timeout`). They keep the Lua semantics and are needed by OGSR Engine, which drives the GC itself once per frame (`CLevel::script_gc`, time budget) and creates lots of short-lived luabind userdata.

Every change is marked with an `OGSR` comment; `grep -n OGSR src/*` lists them all. Upstream lines are not modified, only added to.

## Why

The GC atomic phase (not incremental) walked every userdata to find those to finalize: one cache miss each, 40-140 ms spikes with ~300k-500k userdata. Most of them were luabind temporaries with a no-op `__gc`.

## Changes

| Change | Where | What |
|---|---|---|
| Time-budgeted GC | `lj_gc.c` `lj_gc_step_timeout`, `lj_api.c` `LUA_GCTIMEOUT` | Replaces solbjorn's timeout loop: runs `gc_onestep` until the deadline, reads the clock every 16 steps (every step while finalizing). The atomic phase is deferred to the start of the next call if it doesn't fit the remaining budget (last atomic duration is measured). Doesn't start a new cycle after finishing one, the caller decides (pause). Returns 1 when a cycle finished. |
| Userdata pre-scan | `lj_gc.c` `gc_udscan_*`, hooks in `gc_mark_start`, `gc_onestep` (propagate), `lj_gc_separateudata`, `lj_gc_fullgc` | At the end of the mark phase, userdata that are already black or finalized are moved incrementally (64 per step) to a side list, so the atomic `separateudata` only walks the rest. The side list keeps the original order and is spliced back at the end of the main list right after that walk (or before any full walk: `lua_close`, full GC in the middle of a cycle, debug walk), so the newest-first finalization order is kept (luabind `lua_close` relies on it). |
| `lua_newuserdata_nogc` | `lj_udata.c` `lj_udata_new_nogc`, `lj_api.c`, `lua.h` | Userdata that is never finalized (`__gc` is ignored): marked finalized and chained to the main GC list, so the atomic phase never walks it. Freed by the sweep. Used by luabind for non-owning references and for trivially destructible values stored inline. |
| GC stats | `lua_GCStats` in `lua.h`, `GCState.stats`, `lua_gcstats` | Per `LUA_GCTIMEOUT` call: total time, longest step run, atomic phase split by part (remark, roots + weak, grayagain, userdata, clear weak), userdata walked/pre-scanned. |
| Finalizer counter | `GCState.finalized_total`, `lua_gcfinalizedtotal` | Total `__gc` calls (userdata and cdata), for per-class finalization stats in the engine. |
| Heap walk | `lj_gc_foreach_udata`, `lua_gcforeachudata` | Debug: visits every userdata (live count by class). Run a full GC first, pending finalization userdata aren't visited. |

`GCState` gets new fields at its end (`udscan*`, `atomic_ns`, `finalized_total`, `stats`). The VM code only uses offsets of existing fields, which don't change.

## Other fixes

| Fix | Where | What |
|---|---|---|
| `jit.prngstate` round trip | `lib_jit.c` `jit_prngstate`, `jit_prngstate_word` | OpenResty's getter returned the 32-bit words as signed integers (`setintV`), which its setter rejected, and the setter converted words with `numberVint`: words >= 2^31 saturated to `INT_MIN` and the sign extension corrupted the high word (`{4000000000, 1}` read back as `{-2147483648, -1}`). Now the getter returns unsigned words, the setter takes integers in `-2^31 .. 2^32-1` (negative ones as their two's complement bits, so states saved by the old getter still work) and rejects anything else. Upstream lines changed here (exception to "only added to"), marked `OGSR`. Tested by the engine's autotest `tools/autotest/engine/luajit_openresty.script`. |
| Host-triggered profiler samples | `lj_profile.c` `ProfileState.external`, `profile_timer_start` / `_stop`, `luaJIT_profile_request`; `luajit_rolling.h` | `luaJIT_profile_start` mode `x` (Windows timer build): no timer thread, only the profile lock; `luaJIT_profile_request(L)` does what the timer thread does (`profile_trigger`: the profile hook for the next VM check) from any thread and returns 1 while the profiler runs on that VM. The engine's sampler thread requests samples only while Lua runs a call it records, so engine time is never sampled (OGSR Engine `xrGame/lua_profiler.cpp`, `agent/game_PLAN_devtools.md` P2). `profile_timer_stop` waits only for a thread it has (upstream line changed, marked `OGSR`). |
| Trace event helpers | `lib_jit.c` `luaJIT_trace_errmsg`, `luaJIT_func_pcinfo`; `luajit_rolling.h` | A static host has no `jit.vmdef` (buildvm output, read by `jit/v.lua`): `luaJIT_trace_errmsg(L, erridx, infoidx)` makes the text of a trace abort reason from the `abort` event's error and info as `jit/v.lua`'s `fmterr` does (formats from `lj_traceerr.h`, bytecode names from `BCDEF`, fast function names from `lj_ffdef.h`); `luaJIT_func_pcinfo(L, idx, pc, &blacklisted)` gives the line of a bytecode and whether it is the `I` variant of a loop / function header (blacklisted or JIT off). OGSR Engine's JIT report (`xrGame/lua_profiler.cpp`, `agent/game_PLAN_devtools.md` P3). |

## Engine side (OGSR Engine repo)

- `xrGame/Level.cpp` `CLevel::script_gc`: pacing (pause, budget from allocation rate, ramp up when behind), `[script_gc]` log of slow calls (`lua_gc_log_ms`).
- Luabind: `lua_newuserdata_nogc` for non-owning references and inline values of trivially destructible classes.
- `lua_udata_stats` console command: live userdata and `__gc` calls by class.
- `xrGame/lua_profiler.cpp`: sampling profiler on `luaJIT_profile_request` (mode `fx`); JIT report on `jit.attach(f, "trace")` with `luaJIT_trace_errmsg` / `luaJIT_func_pcinfo`.

## Build

`luajit2.vcxproj` builds `msvcbuild.bat` only when `src/LuaJIT.lib` is missing: delete it after changing the sources.
