// reach - conditional tail calls out of `bdz`/`bdzf` switch chains.
//
// MSVC lowers small switches to `mtctr rX; bdz case0; bdz case1; ...`. When a
// case is `return f(...)`, the bdz branches straight into f, which lives in a
// different function. ReXGlue 0.10.0 translates a conditional branch outside
// the current function as a bare `return`, silently skipping f (and whatever
// it would have returned in r3). hints/switch_tailcalls.toml places a mid-asm
// hook before each such branch; the hook evaluates the branch condition, makes
// the tail call itself and returns true so the recompiled caller returns.
//
// Hooks only receive registers. They are passed ctx.r3, and r3 is the first
// member of PPCContext, so its address is the caller's context.

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <cstddef>
#include <cstdint>

static_assert(offsetof(PPCContext, r3) == 0, "hooks recover ctx from &ctx.r3");

REX_EXTERN(sub_820D8F9C);
REX_EXTERN(sub_8244B364);
REX_EXTERN(sub_82479CE8);
REX_EXTERN(sub_825E09CC);
REX_EXTERN(sub_82631D14);

namespace {

enum class Kind {
  kBdz,         // branch if --ctr == 0
  kBdzfCr6Eq,   // branch if --ctr == 0 && !cr6.eq
};

bool TailCallIfTaken(PPCRegister& r3, Kind kind, PPCFunc* target) {
  PPCContext& ctx = *reinterpret_cast<PPCContext*>(&r3);
  if (static_cast<uint32_t>(ctx.ctr.u32 - 1) != 0) return false;
  if (kind == Kind::kBdzfCr6Eq && ctx.cr6.eq) return false;
  // Taken: apply the instruction's own side effect, then branch.
  --ctx.ctr.u64;
  target(ctx, rex::system::kernel_state()->memory()->virtual_membase());
  return true;
}

}  // namespace

#define REACH_TAILCALL_HOOK(site, kind, target) \
  bool ReachTailCall_##site(PPCRegister& r3) { return TailCallIfTaken(r3, kind, target); }

REACH_TAILCALL_HOOK(820D8DC0, Kind::kBdzfCr6Eq, sub_820D8F9C)
REACH_TAILCALL_HOOK(8244B2C8, Kind::kBdz, sub_8244B364)
REACH_TAILCALL_HOOK(8244B2D0, Kind::kBdz, sub_8244B364)
REACH_TAILCALL_HOOK(8244B2D4, Kind::kBdz, sub_8244B364)
REACH_TAILCALL_HOOK(82479A88, Kind::kBdzfCr6Eq, sub_82479CE8)
REACH_TAILCALL_HOOK(825E08C0, Kind::kBdzfCr6Eq, sub_825E09CC)
REACH_TAILCALL_HOOK(825E08C8, Kind::kBdzfCr6Eq, sub_825E09CC)
REACH_TAILCALL_HOOK(82631854, Kind::kBdzfCr6Eq, sub_82631D14)
REACH_TAILCALL_HOOK(82631858, Kind::kBdzfCr6Eq, sub_82631D14)
REACH_TAILCALL_HOOK(8263185C, Kind::kBdzfCr6Eq, sub_82631D14)
