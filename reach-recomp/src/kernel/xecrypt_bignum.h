// reach - minimal fixed-width bignum for XeCrypt RSA public operations.
// Header-only and dependency-free so it can be unit tested on the host.

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace reach::xecrypt {

// Little-endian limb order: limb[0] is the least significant 64 bits. This is
// also the digit order XeCrypt uses ("Qw" = qword digits, least significant
// first); each digit is stored big-endian ("Ne" = native endian on Xenon).
using BigNum = std::vector<uint64_t>;

inline uint64_t LoadBE64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return __builtin_bswap64(v);
}

inline void StoreBE64(uint8_t* p, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, sizeof(v));
}

inline bool GreaterOrEqual(const BigNum& a, const BigNum& b) {
  for (size_t i = a.size(); i-- > 0;) {
    if (a[i] != b[i]) return a[i] > b[i];
  }
  return true;
}

// a -= b (mod 2^(64*k)). Wraparound is intended: callers subtract n from a
// value whose true magnitude may have carried out of the top limb.
inline void SubInPlace(BigNum& a, const BigNum& b) {
  uint64_t borrow = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    unsigned __int128 d = static_cast<unsigned __int128>(a[i]) - b[i] - borrow;
    a[i] = static_cast<uint64_t>(d);
    borrow = static_cast<uint64_t>(d >> 64) & 1;
  }
}

// r = 2r mod n, for r < n.
inline void DoubleMod(BigNum& r, const BigNum& n) {
  uint64_t carry = 0;
  for (auto& limb : r) {
    uint64_t next = limb >> 63;
    limb = (limb << 1) | carry;
    carry = next;
  }
  if (carry || GreaterOrEqual(r, n)) SubInPlace(r, n);
}

// r = r + a mod n, for r, a < n.
inline void AddMod(BigNum& r, const BigNum& a, const BigNum& n) {
  uint64_t carry = 0;
  for (size_t i = 0; i < r.size(); ++i) {
    unsigned __int128 s = static_cast<unsigned __int128>(r[i]) + a[i] + carry;
    r[i] = static_cast<uint64_t>(s);
    carry = static_cast<uint64_t>(s >> 64);
  }
  if (carry || GreaterOrEqual(r, n)) SubInPlace(r, n);
}

inline bool TestBit(const BigNum& x, size_t bit) {
  return (x[bit / 64] >> (bit % 64)) & 1;
}

// a * b mod n via double-and-add over the bits of b (a < n).
inline BigNum MulMod(const BigNum& a, const BigNum& b, const BigNum& n) {
  BigNum r(n.size(), 0);
  for (size_t bit = n.size() * 64; bit-- > 0;) {
    DoubleMod(r, n);
    if (TestBit(b, bit)) AddMod(r, a, n);
  }
  return r;
}

// x mod n for arbitrary x of the same width.
inline BigNum Reduce(const BigNum& x, const BigNum& n) {
  BigNum one(n.size(), 0);
  one[0] = 1;
  if (!GreaterOrEqual(one, n)) {
    // n > 1, so 1 < n and MulMod's precondition holds.
    return MulMod(one, x, n);
  }
  return BigNum(n.size(), 0);
}

inline BigNum PowMod(const BigNum& base, uint32_t exponent, const BigNum& n) {
  BigNum result(n.size(), 0);
  result[0] = 1;
  result = Reduce(result, n);
  BigNum b = Reduce(base, n);
  int top = 31;
  while (top > 0 && !((exponent >> top) & 1)) --top;
  for (int bit = top; bit >= 0; --bit) {
    result = MulMod(result, result, n);
    if ((exponent >> bit) & 1) result = MulMod(result, b, n);
  }
  return result;
}

}  // namespace reach::xecrypt
