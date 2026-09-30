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

## Engine side (OGSR Engine repo)

- `xrGame/Level.cpp` `CLevel::script_gc`: pacing (pause, budget from allocation rate, ramp up when behind), `[script_gc]` log of slow calls (`lua_gc_log_ms`).
- Luabind: `lua_newuserdata_nogc` for non-owning references and inline values of trivially destructible classes.
- `lua_udata_stats` console command: live userdata and `__gc` calls by class.

## Build

`luajit2.vcxproj` builds `msvcbuild.bat` only when `src/LuaJIT.lib` is missing: delete it after changing the sources.
