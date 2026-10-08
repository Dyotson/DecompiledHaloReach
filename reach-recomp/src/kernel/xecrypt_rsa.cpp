// reach - native replacements for xboxkrnl crypto exports the SDK leaves
// unimplemented on Linux.
//
// Generated code calls kernel imports through `__imp__<Name>` symbols that
// librexruntime provides. A strong definition here wins at link time, so the
// game's import thunks land in this file instead of the SDK stub.

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "xecrypt_bignum.h"

#include <cstdint>
#include <cstring>

using namespace reach::xecrypt;

// BOOL XeCryptBnQwNeRsaPubCrypt(const QWORD* a, QWORD* b, const XECRYPT_RSA* rsa)
//   b = a ^ rsa->public_exponent mod rsa->modulus
// XECRYPT_RSA: be32 cqw; be32 public_exponent; be64 reserved; qword modulus[cqw].
extern "C" REX_FUNC(__imp__XeCryptBnQwNeRsaPubCrypt) {
  const uint32_t a_addr = ctx.r3.u32;
  const uint32_t b_addr = ctx.r4.u32;
  const uint32_t rsa_addr = ctx.r5.u32;

  const uint8_t* rsa = base + rsa_addr;
  uint32_t cqw;
  uint32_t exponent;
  std::memcpy(&cqw, rsa, 4);
  std::memcpy(&exponent, rsa + 4, 4);
  cqw = __builtin_bswap32(cqw);
  exponent = __builtin_bswap32(exponent);

  if (cqw == 0 || cqw > 64 || exponent == 0) {
    REXKRNL_WARN("XeCryptBnQwNeRsaPubCrypt: rejecting key (cqw={}, e={:#x})",
                 cqw, exponent);
    ctx.r3.u64 = 0;
    return;
  }

  BigNum n(cqw), a(cqw);
  const uint8_t* mod_ptr = rsa + 16;
  const uint8_t* a_ptr = base + a_addr;
  for (uint32_t i = 0; i < cqw; ++i) {
    n[i] = LoadBE64(mod_ptr + i * 8);
    a[i] = LoadBE64(a_ptr + i * 8);
  }

  bool n_is_zero = true;
  for (uint64_t limb : n) n_is_zero &= (limb == 0);
  if (n_is_zero) {
    ctx.r3.u64 = 0;
    return;
  }

  BigNum out = PowMod(a, exponent, n);
  uint8_t* b_ptr = base + b_addr;
  for (uint32_t i = 0; i < cqw; ++i) {
    StoreBE64(b_ptr + i * 8, out[i]);
  }
  ctx.r3.u64 = 1;
}
