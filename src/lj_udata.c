/*
** Userdata handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_udata_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_udata.h"

GCudata *lj_udata_new(lua_State *L, MSize sz, GCtab *env)
{
  GCudata *ud = lj_mem_newt(L, sizeof(GCudata) + sz, GCudata);
  global_State *g = G(L);
  newwhite(g, ud);  /* Not finalized. */
  ud->gct = ~LJ_TUDATA;
  ud->udtype = UDTYPE_USERDATA;
  ud->len = sz;
  /* NOBARRIER: The GCudata is new (marked white). */
  setgcrefnull(ud->metatable);
  setgcref(ud->env, obj2gco(env));
  /* Chain to userdata list (after main thread). */
  setgcrefr(ud->nextgc, mainthread(g)->nextgc);
  setgcref(mainthread(g)->nextgc, obj2gco(ud));
  return ud;
}

/* OGSR: userdata whose __gc is never called (lua_newuserdata_nogc). It's
** moved to the main GC list, so the atomic phase doesn't walk it when
** separating userdata to be finalized, and it's simply freed by the sweep.
*/
GCudata *lj_udata_new_nogc(lua_State *L, MSize sz, GCtab *env)
{
  GCudata *ud = lj_udata_new(L, sz, env);
  global_State *g = G(L);
  setgcrefr(mainthread(g)->nextgc, ud->nextgc);  /* Unchain from the userdata list, it's the head. */
  setgcrefr(ud->nextgc, g->gc.root);  /* Chain to the main GC list, like any other new object. */
  setgcref(g->gc.root, obj2gco(ud));
  markfinalized(obj2gco(ud));  /* Never finalize it. */
  return ud;
}

void LJ_FASTCALL lj_udata_free(global_State *g, GCudata *ud)
{
  lj_mem_free(g, ud, sizeudata(ud));
}

#if LJ_64
void *lj_lightud_intern(lua_State *L, void *p)
{
  global_State *g = G(L);
  uint64_t u = (uint64_t)p;
  uint32_t up = lightudup(u);
  uint32_t *segmap = mref(g->gc.lightudseg, uint32_t);
  MSize segnum = g->gc.lightudnum;
  if (segmap) {
    MSize seg;
    for (seg = 0; seg <= segnum; seg++)
      if (segmap[seg] == up)  /* Fast path. */
	return (void *)(((uint64_t)seg << LJ_LIGHTUD_BITS_LO) | lightudlo(u));
    segnum++;
    /* Leave last segment unused to avoid clash with ITERN key. */
    if (segnum >= (1 << LJ_LIGHTUD_BITS_SEG)-1) lj_err_msg(L, LJ_ERR_BADLU);
  }
  if (!((segnum-1) & segnum) && segnum != 1) {
    lj_mem_reallocvec(L, segmap, segnum, segnum ? 2*segnum : 2u, uint32_t);
    setmref(g->gc.lightudseg, segmap);
  }
  g->gc.lightudnum = segnum;
  segmap[segnum] = up;
  return (void *)(((uint64_t)segnum << LJ_LIGHTUD_BITS_LO) | lightudlo(u));
}
#endif

