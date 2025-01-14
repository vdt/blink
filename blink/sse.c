/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2022 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include <string.h>

#include "blink/case.h"
#include "blink/endian.h"
#include "blink/likely.h"
#include "blink/machine.h"
#include "blink/macros.h"
#include "blink/modrm.h"
#include "blink/sse.h"

static void MmxPaddusb(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 8; ++i) {
    x[i] = MIN(255, x[i] + y[i]);
  }
}

static void MmxPsubusb(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 8; ++i) {
    x[i] = MIN(255, MAX(0, x[i] - y[i]));
  }
}

static void MmxPsubsb(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 8; ++i) {
    x[i] = MAX(-128, MIN(127, (i8)x[i] - (i8)y[i]));
  }
}

static void MmxPaddsb(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 8; ++i) {
    x[i] = MAX(-128, MIN(127, (i8)x[i] + (i8)y[i]));
  }
}

static void MmxPmulhrsw(u8 x[8], const u8 y[8]) {
  i16 a, b;
  unsigned i;
  for (i = 0; i < 4; ++i) {
    a = Get16(x + i * 2);
    b = Get16(y + i * 2);
    Put16(x + i * 2, (((a * b) >> 14) + 1) >> 1);
  }
}

static void MmxPmaddubsw(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 4; ++i) {
    Put16(x + i * 2,
          MAX(-32768, MIN(32767, (x[i * 2 + 0] * (i8)y[i * 2 + 0] +
                                  x[i * 2 + 1] * (i8)y[i * 2 + 1]))));
  }
}

static void MmxPsraw(u8 x[8], unsigned k) {
  if (k > 15) k = 15;
  for (unsigned i = 0; i < 4; ++i) {
    Put16(x + i * 2, (i16)Get16(x + i * 2) >> k);
  }
}

static void MmxPsrad(u8 x[8], unsigned k) {
  if (k > 31) k = 31;
  for (unsigned i = 0; i < 2; ++i) {
    Put32(x + i * 4, (i32)Get32(x + i * 4) >> k);
  }
}

static void MmxPsrlw(u8 x[8], unsigned k) {
  unsigned i;
  if (k < 16) {
    for (i = 0; i < 4; ++i) {
      Put16(x + i * 2, Get16(x + i * 2) >> k);
    }
  } else {
    memset(x, 0, 8);
  }
}

static void MmxPsllw(u8 x[8], unsigned k) {
  unsigned i;
  if (k <= 15) {
    for (i = 0; i < 4; ++i) {
      Put16(x + i * 2, Get16(x + i * 2) << k);
    }
  } else {
    memset(x, 0, 8);
  }
}

static void MmxPsrld(u8 x[8], unsigned k) {
  unsigned i;
  if (k <= 31) {
    for (i = 0; i < 2; ++i) {
      Put32(x + i * 4, Get32(x + i * 4) >> k);
    }
  } else {
    memset(x, 0, 8);
  }
}

static void MmxPslld(u8 x[8], unsigned k) {
  unsigned i;
  if (k <= 31) {
    for (i = 0; i < 2; ++i) {
      Put32(x + i * 4, Get32(x + i * 4) << k);
    }
  } else {
    memset(x, 0, 8);
  }
}

static void MmxPsrlq(u8 x[8], unsigned k) {
  if (k <= 63) {
    Put64(x, Get64(x) >> k);
  } else {
    memset(x, 0, 8);
  }
}

static void MmxPsllq(u8 x[8], unsigned k) {
  if (k <= 63) {
    Put64(x, Get64(x) << k);
  } else {
    memset(x, 0, 8);
  }
}

static void MmxPslldq(u8 x[8], unsigned k) {
  unsigned i;
  u8 t[8];
  if (k > 8) k = 8;
  for (i = 0; i < k; ++i) t[i] = 0;
  for (i = 0; i < 8 - k; ++i) t[k + i] = x[i];
  memcpy(x, t, 8);
}

static void MmxPsrldq(u8 x[8], unsigned k) {
  u8 t[8];
  if (k > 8) k = 8;
  memcpy(t, x + k, 8 - k);
  memset(t + (8 - k), 0, k);
  memcpy(x, t, 8);
}

static void MmxPalignr(u8 x[8], const u8 y[8], unsigned k) {
  u8 t[24];
  memcpy(t, y, 8);
  memcpy(t + 8, x, 8);
  memset(t + 16, 0, 8);
  memcpy(x, t + MIN(k, 16), 8);
}

static void MmxPsubd(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 2; ++i) {
    Put32(x + i * 4, Get32(x + i * 4) - Get32(y + i * 4));
  }
}

static void MmxPaddd(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 2; ++i) {
    Put32(x + i * 4, Get32(x + i * 4) + Get32(y + i * 4));
  }
}

static void MmxPaddq(u8 x[8], const u8 y[8]) {
  Put64(x, Get64(x) + Get64(y));
}

static void MmxPsubq(u8 x[8], const u8 y[8]) {
  Put64(x, Get64(x) - Get64(y));
}

static void MmxPsubusw(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 4; ++i) {
    Put16(x + i * 2, MIN(65535, MAX(0, Get16(x + i * 2) - Get16(y + i * 2))));
  }
}

static void MmxPminsw(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 4; ++i) {
    Put16(x + i * 2, MIN((i16)Get16(x + i * 2), (i16)Get16(y + i * 2)));
  }
}

static void MmxPmaxsw(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 4; ++i) {
    Put16(x + i * 2, MAX((i16)Get16(x + i * 2), (i16)Get16(y + i * 2)));
  }
}

static void MmxPackuswb(u8 x[8], const u8 y[8]) {
  unsigned i;
  u8 t[8];
  for (i = 0; i < 4; ++i) {
    t[i + 0] = MIN(255, MAX(0, (i16)Get16(x + i * 2)));
  }
  for (i = 0; i < 4; ++i) {
    t[i + 4] = MIN(255, MAX(0, (i16)Get16(y + i * 2)));
  }
  memcpy(x, t, 8);
}

static void MmxPacksswb(u8 x[8], const u8 y[8]) {
  unsigned i;
  u8 t[8];
  for (i = 0; i < 4; ++i) {
    t[i + 0] = MAX(-128, MIN(127, (i16)Get16(x + i * 2)));
  }
  for (i = 0; i < 4; ++i) {
    t[i + 4] = MAX(-128, MIN(127, (i16)Get16(y + i * 2)));
  }
  memcpy(x, t, 8);
}

static void MmxPackssdw(u8 x[8], const u8 y[8]) {
  unsigned i;
  u8 t[8];
  for (i = 0; i < 2; ++i) {
    Put16(t + i * 2 + 0, MAX(-32768, MIN(32767, (i32)Get32(x + i * 4))));
  }
  for (i = 0; i < 2; ++i) {
    Put16(t + i * 2 + 4, MAX(-32768, MIN(32767, (i32)Get32(y + i * 4))));
  }
  memcpy(x, t, 8);
}

static void MmxPcmpgtd(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 2; ++i) {
    Put32(x + i * 4, -((i32)Get32(x + i * 4) > (i32)Get32(y + i * 4)));
  }
}

static void MmxPcmpeqd(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 2; ++i) {
    Put32(x + i * 4, -(Get32(x + i * 4) == Get32(y + i * 4)));
  }
}

static void MmxPsrawv(u8 x[8], const u8 y[8]) {
  u64 k;
  unsigned i;
  k = Get64(y);
  if (k > 15) k = 15;
  for (i = 0; i < 4; ++i) {
    Put16(x + i * 2, (i16)Get16(x + i * 2) >> k);
  }
}

static void MmxPsradv(u8 x[8], const u8 y[8]) {
  u64 k;
  unsigned i;
  k = Get64(y);
  if (k > 31) k = 31;
  for (i = 0; i < 2; ++i) {
    Put32(x + i * 4, (i32)Get32(x + i * 4) >> k);
  }
}

static void MmxPsrlwv(u8 x[8], const u8 y[8]) {
  u64 k;
  unsigned i;
  k = Get64(y);
  if (k < 16) {
    for (i = 0; i < 4; ++i) {
      Put16(x + i * 2, Get16(x + i * 2) >> k);
    }
  } else {
    memset(x, 0, 8);
  }
}

static void MmxPsllwv(u8 x[8], const u8 y[8]) {
  u64 k;
  unsigned i;
  k = Get64(y);
  if (k < 16) {
    for (i = 0; i < 4; ++i) {
      Put16(x + i * 2, Get16(x + i * 2) << k);
    }
  } else {
    memset(x, 0, 8);
  }
}

static void MmxPsrldv(u8 x[8], const u8 y[8]) {
  u64 k;
  unsigned i;
  k = Get64(y);
  if (k < 32) {
    for (i = 0; i < 2; ++i) {
      Put32(x + i * 4, Get32(x + i * 4) >> k);
    }
  } else {
    memset(x, 0, 8);
  }
}

static void MmxPslldv(u8 x[8], const u8 y[8]) {
  u64 k;
  unsigned i;
  k = Get64(y);
  if (k < 32) {
    for (i = 0; i < 2; ++i) {
      Put32(x + i * 4, Get32(x + i * 4) << k);
    }
  } else {
    memset(x, 0, 8);
  }
}

static void MmxPsrlqv(u8 x[8], const u8 y[8]) {
  u64 k;
  k = Get64(y);
  if (k < 64) {
    Put64(x, Get64(x) >> k);
  } else {
    memset(x, 0, 8);
  }
}

static void MmxPsllqv(u8 x[8], const u8 y[8]) {
  u64 k;
  k = Get64(y);
  if (k < 64) {
    Put64(x, Get64(x) << k);
  } else {
    memset(x, 0, 8);
  }
}

static void MmxPsadbw(u8 x[8], const u8 y[8]) {
  unsigned i, s, t;
  for (s = i = 0; i < 4; ++i) {
    s += ABS(x[i] - y[i]);
  }
  for (t = 0; i < 8; ++i) {
    t += ABS(x[i] - y[i]);
  }
  Put32(x + 0, s);
  Put32(x + 4, t);
}

static void MmxPmaddwd(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 2; ++i) {
    Put32(x + i * 4, ((i16)Get16(x + i * 4 + 0) * (i16)Get16(y + i * 4 + 0) +
                      (i16)Get16(x + i * 4 + 2) * (i16)Get16(y + i * 4 + 2)));
  }
}

static void MmxPmulhuw(u8 x[8], const u8 y[8]) {
  u32 v;
  for (unsigned i = 0; i < 4; ++i) {
    v = Get16(x + i * 2);
    v *= Get16(y + i * 2);
    v >>= 16;
    Put16(x + i * 2, v);
  }
}

static void MmxPmuludq(u8 x[8], const u8 y[8]) {
  Put64(x, (u64)Get32(x) * Get32(y));
}

static void MmxPmulld(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 2; ++i) {
    Put32(x + i * 4, Get32(x + i * 4) * Get32(y + i * 4));
  }
}

static void MmxPshufb(u8 x[8], const u8 y[8]) {
  unsigned i;
  u8 t[8];
  for (i = 0; i < 8; ++i) {
    t[i] = (y[i] & 128) ? 0 : x[y[i] & 7];
  }
  memcpy(x, t, 8);
}

static void MmxPsignb(u8 x[8], const u8 y[8]) {
  int v;
  for (unsigned i = 0; i < 8; ++i) {
    v = (i8)y[i];
    if (!v) {
      x[i] = 0;
    } else if (v < 0) {
      x[i] = -(i8)x[i];
    }
  }
}

static void MmxPsignw(u8 x[8], const u8 y[8]) {
  int v;
  for (unsigned i = 0; i < 4; ++i) {
    v = (i16)Get16(y + i * 2);
    if (!v) {
      Put16(x + i * 2, 0);
    } else if (v < 0) {
      Put16(x + i * 2, -(i16)Get16(x + i * 2));
    }
  }
}

static void MmxPsignd(u8 x[8], const u8 y[8]) {
  i32 v;
  for (unsigned i = 0; i < 2; ++i) {
    v = Get32(y + i * 4);
    if (!v) {
      Put32(x + i * 4, 0);
    } else if (v < 0) {
      Put32(x + i * 4, -Get32(x + i * 4));
    }
  }
}

static void MmxPabsw(u8 x[8], const u8 y[8]) {
  for (unsigned i = 0; i < 4; ++i) {
    Put16(x + i * 2, ABS((i16)Get16(y + i * 2)));
  }
}

static void MmxPabsd(u8 x[8], const u8 y[8]) {
  i32 v;
  for (unsigned i = 0; i < 2; ++i) {
    v = Get32(y + i * 4);
    Put32(x + i * 4, v >= 0 ? v : -(u32)v);
  }
}

static void MmxPhaddw(u8 x[8], const u8 y[8]) {
  u8 t[8];
  Put16(t + 0 * 2, Get16(x + 0 * 2) + Get16(x + 1 * 2));
  Put16(t + 1 * 2, Get16(x + 2 * 2) + Get16(x + 3 * 2));
  Put16(t + 2 * 2, Get16(y + 0 * 2) + Get16(y + 1 * 2));
  Put16(t + 3 * 2, Get16(y + 2 * 2) + Get16(y + 3 * 2));
  memcpy(x, t, 8);
}

static void MmxPhsubw(u8 x[8], const u8 y[8]) {
  u8 t[8];
  Put16(t + 0 * 2, Get16(x + 0 * 2) - Get16(x + 1 * 2));
  Put16(t + 1 * 2, Get16(x + 2 * 2) - Get16(x + 3 * 2));
  Put16(t + 2 * 2, Get16(y + 0 * 2) - Get16(y + 1 * 2));
  Put16(t + 3 * 2, Get16(y + 2 * 2) - Get16(y + 3 * 2));
  memcpy(x, t, 8);
}

static void MmxPhaddd(u8 x[8], const u8 y[8]) {
  u8 t[8];
  Put32(t + 0 * 4, Get32(x + 0 * 4) + Get32(x + 1 * 4));
  Put32(t + 1 * 4, Get32(y + 0 * 4) + Get32(y + 1 * 4));
  memcpy(x, t, 8);
}

static void MmxPhsubd(u8 x[8], const u8 y[8]) {
  u8 t[8];
  Put32(t + 0 * 4, Get32(x + 0 * 4) - Get32(x + 1 * 4));
  Put32(t + 1 * 4, Get32(y + 0 * 4) - Get32(y + 1 * 4));
  memcpy(x, t, 8);
}

static void MmxPunpcklbw(u8 x[8], const u8 y[8]) {
  x[7] = y[3];
  x[6] = x[3];
  x[5] = y[2];
  x[4] = x[2];
  x[3] = y[1];
  x[2] = x[1];
  x[1] = y[0];
  x[0] = x[0];
}

static void MmxPunpckhbw(u8 x[8], const u8 y[8]) {
  x[0] = x[4];
  x[1] = y[4];
  x[2] = x[5];
  x[3] = y[5];
  x[4] = x[6];
  x[5] = y[6];
  x[6] = x[7];
  x[7] = y[7];
}

static void MmxPunpcklwd(u8 x[8], const u8 y[8]) {
  x[6] = y[2];
  x[7] = y[3];
  x[4] = x[2];
  x[5] = x[3];
  x[2] = y[0];
  x[3] = y[1];
  x[0] = x[0];
  x[1] = x[1];
}

static void MmxPunpckldq(u8 x[8], const u8 y[8]) {
  x[4] = y[0];
  x[5] = y[1];
  x[6] = y[2];
  x[7] = y[3];
  x[0] = x[0];
  x[1] = x[1];
  x[2] = x[2];
  x[3] = x[3];
}

static void MmxPunpckhwd(u8 x[8], const u8 y[8]) {
  x[0] = x[4];
  x[1] = x[5];
  x[2] = y[4];
  x[3] = y[5];
  x[4] = x[6];
  x[5] = x[7];
  x[6] = y[6];
  x[7] = y[7];
}

static void MmxPunpckhdq(u8 x[8], const u8 y[8]) {
  x[0] = x[4];
  x[1] = x[5];
  x[2] = x[6];
  x[3] = x[7];
  x[4] = y[4];
  x[5] = y[5];
  x[6] = y[6];
  x[7] = y[7];
}

static void MmxPunpcklqdq(u8 x[8], const u8 y[8]) {
}

static void MmxPunpckhqdq(u8 x[8], const u8 y[8]) {
}

static void SsePslldq(u8 x[16], unsigned k) {
  unsigned i;
  u8 t[16];
  if (k > 16) k = 16;
  for (i = 0; i < k; ++i) t[i] = 0;
  for (i = 0; i < 16 - k; ++i) t[k + i] = x[i];
  memcpy(x, t, 16);
}

static void SsePsrldq(u8 x[16], unsigned k) {
  u8 t[16];
  if (k > 16) k = 16;
  memcpy(t, x + k, 16 - k);
  memset(t + (16 - k), 0, k);
  memcpy(x, t, 16);
}

static void SsePalignr(u8 x[16], const u8 y[16], unsigned k) {
  u8 t[48];
  memcpy(t, y, 16);
  memcpy(t + 16, x, 16);
  memset(t + 32, 0, 16);
  memcpy(x, t + MIN(k, 32), 16);
}

static void SsePsubd(u8 x[16], const u8 y[16]) {
  for (unsigned i = 0; i < 4; ++i) {
    Put32(x + i * 4, Get32(x + i * 4) - Get32(y + i * 4));
  }
}

static void SsePaddd(u8 x[16], const u8 y[16]) {
  for (unsigned i = 0; i < 4; ++i) {
    Put32(x + i * 4, Get32(x + i * 4) + Get32(y + i * 4));
  }
}

static void SsePaddq(u8 x[16], const u8 y[16]) {
  for (unsigned i = 0; i < 2; ++i) {
    Put64(x + i * 8, Get64(x + i * 8) + Get64(y + i * 8));
  }
}

static void SsePsubq(u8 x[16], const u8 y[16]) {
  for (unsigned i = 0; i < 2; ++i) {
    Put64(x + i * 8, Get64(x + i * 8) - Get64(y + i * 8));
  }
}

static void SsePackuswb(u8 x[16], const u8 y[16]) {
  unsigned i;
  u8 t[16];
  for (i = 0; i < 8; ++i) {
    t[i + 0] = MIN(255, MAX(0, (i16)Get16(x + i * 2)));
  }
  for (i = 0; i < 8; ++i) {
    t[i + 8] = MIN(255, MAX(0, (i16)Get16(y + i * 2)));
  }
  memcpy(x, t, 16);
}

static void SsePacksswb(u8 x[16], const u8 y[16]) {
  unsigned i;
  u8 t[16];
  for (i = 0; i < 8; ++i) {
    t[i + 0] = MAX(-128, MIN(127, (i16)Get16(x + i * 2)));
  }
  for (i = 0; i < 8; ++i) {
    t[i + 8] = MAX(-128, MIN(127, (i16)Get16(y + i * 2)));
  }
  memcpy(x, t, 16);
}

static void SsePackssdw(u8 x[16], const u8 y[16]) {
  unsigned i;
  u8 t[16];
  for (i = 0; i < 4; ++i) {
    Put16(t + i * 2 + 0, MAX(-32768, MIN(32767, (i32)Get32(x + i * 4))));
  }
  for (i = 0; i < 4; ++i) {
    Put16(t + i * 2 + 8, MAX(-32768, MIN(32767, (i32)Get32(y + i * 4))));
  }
  memcpy(x, t, 16);
}

static void SsePsadbw(u8 x[16], const u8 y[16]) {
  unsigned i, s, t;
  for (s = i = 0; i < 8; ++i) s += ABS(x[i] - y[i]);
  for (t = 0; i < 16; ++i) t += ABS(x[i] - y[i]);
  Put64(x + 0, s);
  Put64(x + 8, t);
}

static void SsePmuludq(u8 x[16], const u8 y[16]) {
  for (unsigned i = 0; i < 2; ++i) {
    Put64(x + i * 8, (u64)Get32(x + i * 8) * Get32(y + i * 8));
  }
}

static void SsePshufb(u8 x[16], const u8 y[16]) {
  u8 t[16];
  for (unsigned i = 0; i < 16; ++i) {
    t[i] = (y[i] & 128) ? 0 : x[y[i] & 15];
  }
  memcpy(x, t, 16);
}

static void SsePhaddd(u8 x[16], const u8 y[16]) {
  u8 t[16];
  Put32(t + 0 * 4, Get32(x + 0 * 4) + Get32(x + 1 * 4));
  Put32(t + 1 * 4, Get32(x + 2 * 4) + Get32(x + 3 * 4));
  Put32(t + 2 * 4, Get32(y + 0 * 4) + Get32(y + 1 * 4));
  Put32(t + 3 * 4, Get32(y + 2 * 4) + Get32(y + 3 * 4));
  memcpy(x, t, 16);
}

static void SsePhsubd(u8 x[16], const u8 y[16]) {
  u8 t[16];
  Put32(t + 0 * 4, Get32(x + 0 * 4) - Get32(x + 1 * 4));
  Put32(t + 1 * 4, Get32(x + 2 * 4) - Get32(x + 3 * 4));
  Put32(t + 2 * 4, Get32(y + 0 * 4) - Get32(y + 1 * 4));
  Put32(t + 3 * 4, Get32(y + 2 * 4) - Get32(y + 3 * 4));
  memcpy(x, t, 16);
}

static void SsePhaddw(u8 x[16], const u8 y[16]) {
  u8 t[16];
  Put16(t + 0 * 2, Get16(x + 0 * 2) + Get16(x + 1 * 2));
  Put16(t + 1 * 2, Get16(x + 2 * 2) + Get16(x + 3 * 2));
  Put16(t + 2 * 2, Get16(x + 4 * 2) + Get16(x + 5 * 2));
  Put16(t + 3 * 2, Get16(x + 6 * 2) + Get16(x + 7 * 2));
  Put16(t + 4 * 2, Get16(y + 0 * 2) + Get16(y + 1 * 2));
  Put16(t + 5 * 2, Get16(y + 2 * 2) + Get16(y + 3 * 2));
  Put16(t + 6 * 2, Get16(y + 4 * 2) + Get16(y + 5 * 2));
  Put16(t + 7 * 2, Get16(y + 6 * 2) + Get16(y + 7 * 2));
  memcpy(x, t, 16);
}

static void SsePhsubw(u8 x[16], const u8 y[16]) {
  u8 t[16];
  Put16(t + 0 * 2, Get16(x + 0 * 2) - Get16(x + 1 * 2));
  Put16(t + 1 * 2, Get16(x + 2 * 2) - Get16(x + 3 * 2));
  Put16(t + 2 * 2, Get16(x + 4 * 2) - Get16(x + 5 * 2));
  Put16(t + 3 * 2, Get16(x + 6 * 2) - Get16(x + 7 * 2));
  Put16(t + 4 * 2, Get16(y + 0 * 2) - Get16(y + 1 * 2));
  Put16(t + 5 * 2, Get16(y + 2 * 2) - Get16(y + 3 * 2));
  Put16(t + 6 * 2, Get16(y + 4 * 2) - Get16(y + 5 * 2));
  Put16(t + 7 * 2, Get16(y + 6 * 2) - Get16(y + 7 * 2));
  memcpy(x, t, 16);
}

static void SsePunpcklbw(u8 x[16], const u8 y[16]) {
  x[0xf] = y[0x7];
  x[0xe] = x[0x7];
  x[0xd] = y[0x6];
  x[0xc] = x[0x6];
  x[0xb] = y[0x5];
  x[0xa] = x[0x5];
  x[0x9] = y[0x4];
  x[0x8] = x[0x4];
  x[0x7] = y[0x3];
  x[0x6] = x[0x3];
  x[0x5] = y[0x2];
  x[0x4] = x[0x2];
  x[0x3] = y[0x1];
  x[0x2] = x[0x1];
  x[0x1] = y[0x0];
  x[0x0] = x[0x0];
}

static void SsePunpckhbw(u8 x[16], const u8 y[16]) {
  x[0x0] = x[0x8];
  x[0x1] = y[0x8];
  x[0x2] = x[0x9];
  x[0x3] = y[0x9];
  x[0x4] = x[0xa];
  x[0x5] = y[0xa];
  x[0x6] = x[0xb];
  x[0x7] = y[0xb];
  x[0x8] = x[0xc];
  x[0x9] = y[0xc];
  x[0xa] = x[0xd];
  x[0xb] = y[0xd];
  x[0xc] = x[0xe];
  x[0xd] = y[0xe];
  x[0xe] = x[0xf];
  x[0xf] = y[0xf];
}

static void SsePunpcklwd(u8 x[16], const u8 y[16]) {
  x[0xe] = y[0x6];
  x[0xf] = y[0x7];
  x[0xc] = x[0x6];
  x[0xd] = x[0x7];
  x[0xa] = y[0x4];
  x[0xb] = y[0x5];
  x[0x8] = x[0x4];
  x[0x9] = x[0x5];
  x[0x6] = y[0x2];
  x[0x7] = y[0x3];
  x[0x4] = x[0x2];
  x[0x5] = x[0x3];
  x[0x2] = y[0x0];
  x[0x3] = y[0x1];
  x[0x0] = x[0x0];
  x[0x1] = x[0x1];
}

static void SsePunpckldq(u8 x[16], const u8 y[16]) {
  x[0xc] = y[0x4];
  x[0xd] = y[0x5];
  x[0xe] = y[0x6];
  x[0xf] = y[0x7];
  x[0x8] = x[0x4];
  x[0x9] = x[0x5];
  x[0xa] = x[0x6];
  x[0xb] = x[0x7];
  x[0x4] = y[0x0];
  x[0x5] = y[0x1];
  x[0x6] = y[0x2];
  x[0x7] = y[0x3];
  x[0x0] = x[0x0];
  x[0x1] = x[0x1];
  x[0x2] = x[0x2];
  x[0x3] = x[0x3];
}

static void SsePunpckhwd(u8 x[16], const u8 y[16]) {
  x[0x0] = x[0x8];
  x[0x1] = x[0x9];
  x[0x2] = y[0x8];
  x[0x3] = y[0x9];
  x[0x4] = x[0xa];
  x[0x5] = x[0xb];
  x[0x6] = y[0xa];
  x[0x7] = y[0xb];
  x[0x8] = x[0xc];
  x[0x9] = x[0xd];
  x[0xa] = y[0xc];
  x[0xb] = y[0xd];
  x[0xc] = x[0xe];
  x[0xd] = x[0xf];
  x[0xe] = y[0xe];
  x[0xf] = y[0xf];
}

static void SsePunpckhdq(u8 x[16], const u8 y[16]) {
  x[0x0] = x[0x8];
  x[0x1] = x[0x9];
  x[0x2] = x[0xa];
  x[0x3] = x[0xb];
  x[0x4] = y[0x8];
  x[0x5] = y[0x9];
  x[0x6] = y[0xa];
  x[0x7] = y[0xb];
  x[0x8] = x[0xc];
  x[0x9] = x[0xd];
  x[0xa] = x[0xe];
  x[0xb] = x[0xf];
  x[0xc] = y[0xc];
  x[0xd] = y[0xd];
  x[0xe] = y[0xe];
  x[0xf] = y[0xf];
}

static void SsePunpcklqdq(u8 x[16], const u8 y[16]) {
  x[0x8] = y[0x0];
  x[0x9] = y[0x1];
  x[0xa] = y[0x2];
  x[0xb] = y[0x3];
  x[0xc] = y[0x4];
  x[0xd] = y[0x5];
  x[0xe] = y[0x6];
  x[0xf] = y[0x7];
  x[0x0] = x[0x0];
  x[0x1] = x[0x1];
  x[0x2] = x[0x2];
  x[0x3] = x[0x3];
  x[0x4] = x[0x4];
  x[0x5] = x[0x5];
  x[0x6] = x[0x6];
  x[0x7] = x[0x7];
}

static void SsePunpckhqdq(u8 x[16], const u8 y[16]) {
  x[0x0] = x[0x8];
  x[0x1] = x[0x9];
  x[0x2] = x[0xa];
  x[0x3] = x[0xb];
  x[0x4] = x[0xc];
  x[0x5] = x[0xd];
  x[0x6] = x[0xe];
  x[0x7] = x[0xf];
  x[0x8] = y[0x8];
  x[0x9] = y[0x9];
  x[0xa] = y[0xa];
  x[0xb] = y[0xb];
  x[0xc] = y[0xc];
  x[0xd] = y[0xd];
  x[0xe] = y[0xe];
  x[0xf] = y[0xf];
}

static void SsePsrlw(u8 x[16], unsigned k) {
  MmxPsrlw(x + 0, k);
  MmxPsrlw(x + 8, k);
}

static void SsePsraw(u8 x[16], unsigned k) {
  MmxPsraw(x + 0, k);
  MmxPsraw(x + 8, k);
}

static void SsePsllw(u8 x[16], unsigned k) {
  MmxPsllw(x + 0, k);
  MmxPsllw(x + 8, k);
}

static void SsePsrld(u8 x[16], unsigned k) {
  MmxPsrld(x + 0, k);
  MmxPsrld(x + 8, k);
}

static void SsePsrad(u8 x[16], unsigned k) {
  MmxPsrad(x + 0, k);
  MmxPsrad(x + 8, k);
}

static void SsePslld(u8 x[16], unsigned k) {
  MmxPslld(x + 0, k);
  MmxPslld(x + 8, k);
}

static void SsePsrlq(u8 x[16], unsigned k) {
  MmxPsrlq(x + 0, k);
  MmxPsrlq(x + 8, k);
}

static void SsePsllq(u8 x[16], unsigned k) {
  MmxPsllq(x + 0, k);
  MmxPsllq(x + 8, k);
}

static void SsePsubsb(u8 x[16], const u8 y[16]) {
  i8 X[16], Y[16];
  memcpy(X, x, 16);
  memcpy(Y, y, 16);
  for (unsigned i = 0; i < 16; ++i) {
    X[i] = MAX(-128, MIN(127, X[i] - Y[i]));
  }
  memcpy(x, X, 16);
}

static void SsePaddsb(u8 x[16], const u8 y[16]) {
  i8 X[16], Y[16];
  memcpy(X, x, 16);
  memcpy(Y, y, 16);
  for (unsigned i = 0; i < 16; ++i) {
    X[i] = MAX(-128, MIN(127, X[i] + Y[i]));
  }
  memcpy(x, X, 16);
}

static void SsePaddusb(u8 x[16], const u8 y[16]) {
  u8 X[16], Y[16];
  memcpy(X, x, 16);
  memcpy(Y, y, 16);
  for (unsigned i = 0; i < 16; ++i) {
    X[i] = MIN(255, X[i] + Y[i]);
  }
  memcpy(x, X, 16);
}

static void SsePsubusb(u8 x[16], const u8 y[16]) {
  u8 X[16], Y[16];
  memcpy(X, x, 16);
  memcpy(Y, y, 16);
  for (unsigned i = 0; i < 16; ++i) {
    X[i] = MIN(255, MAX(0, X[i] - Y[i]));
  }
  memcpy(x, X, 16);
}

static void SsePsubusw(u8 x[16], const u8 y[16]) {
  MmxPsubusw(x + 0, y + 0);
  MmxPsubusw(x + 8, y + 8);
}

static void SsePminsw(u8 x[16], const u8 y[16]) {
  MmxPminsw(x + 0, y + 0);
  MmxPminsw(x + 8, y + 8);
}

static void SsePmaxsw(u8 x[16], const u8 y[16]) {
  MmxPmaxsw(x + 0, y + 0);
  MmxPmaxsw(x + 8, y + 8);
}

static void SsePsignb(u8 x[16], const u8 y[16]) {
  MmxPsignb(x + 0, y + 0);
  MmxPsignb(x + 8, y + 8);
}

static void SsePsignw(u8 x[16], const u8 y[16]) {
  MmxPsignw(x + 0, y + 0);
  MmxPsignw(x + 8, y + 8);
}

static void SsePsignd(u8 x[16], const u8 y[16]) {
  MmxPsignd(x + 0, y + 0);
  MmxPsignd(x + 8, y + 8);
}

static void SsePmulhrsw(u8 x[16], const u8 y[16]) {
  MmxPmulhrsw(x + 0, y + 0);
  MmxPmulhrsw(x + 8, y + 8);
}

static void SsePabsw(u8 x[16], const u8 y[16]) {
  MmxPabsw(x + 0, y + 0);
  MmxPabsw(x + 8, y + 8);
}

static void SsePabsd(u8 x[16], const u8 y[16]) {
  MmxPabsd(x + 0, y + 0);
  MmxPabsd(x + 8, y + 8);
}

static void SsePcmpgtd(u8 x[16], const u8 y[16]) {
  MmxPcmpgtd(x + 0, y + 0);
  MmxPcmpgtd(x + 8, y + 8);
}

static void SsePcmpeqd(u8 x[16], const u8 y[16]) {
  MmxPcmpeqd(x + 0, y + 0);
  MmxPcmpeqd(x + 8, y + 8);
}

static void SsePsrawv(u8 x[16], const u8 y[16]) {
  MmxPsrawv(x + 0, y);
  MmxPsrawv(x + 8, y);
}

static void SsePsradv(u8 x[16], const u8 y[16]) {
  MmxPsradv(x + 0, y);
  MmxPsradv(x + 8, y);
}

static void SsePsrlwv(u8 x[16], const u8 y[16]) {
  MmxPsrlwv(x + 0, y);
  MmxPsrlwv(x + 8, y);
}

static void SsePsllwv(u8 x[16], const u8 y[16]) {
  MmxPsllwv(x + 0, y);
  MmxPsllwv(x + 8, y);
}

static void SsePsrldv(u8 x[16], const u8 y[16]) {
  MmxPsrldv(x + 0, y);
  MmxPsrldv(x + 8, y);
}

static void SsePslldv(u8 x[16], const u8 y[16]) {
  MmxPslldv(x + 0, y);
  MmxPslldv(x + 8, y);
}

static void SsePsrlqv(u8 x[16], const u8 y[16]) {
  MmxPsrlqv(x + 0, y);
  MmxPsrlqv(x + 8, y);
}

static void SsePsllqv(u8 x[16], const u8 y[16]) {
  MmxPsllqv(x + 0, y);
  MmxPsllqv(x + 8, y);
}

static void SsePmaddwd(u8 x[16], const u8 y[16]) {
  MmxPmaddwd(x + 0, y + 0);
  MmxPmaddwd(x + 8, y + 8);
}

static void SsePmulhuw(u8 x[16], const u8 y[16]) {
  MmxPmulhuw(x + 0, y + 0);
  MmxPmulhuw(x + 8, y + 8);
}

static void SsePmulld(u8 x[16], const u8 y[16]) {
  int i;
  u32 X[4] = {Get32(x), Get32(x + 4), Get32(x + 8), Get32(x + 12)};
  u32 Y[4] = {Get32(y), Get32(y + 4), Get32(y + 8), Get32(y + 12)};
  for (i = 0; i < 4; ++i) {
    X[i] *= Y[i];
  }
  for (i = 0; i < 4; ++i) {
    Put32(x + i * 4, X[i]);
  }
}

static void SsePmaddubsw(u8 x[16], const u8 y[16]) {
  MmxPmaddubsw(x + 0, y + 0);
  MmxPmaddubsw(x + 8, y + 8);
}

static void OpPsb(P, void MmxKernel(u8[8], unsigned),
                  void SseKernel(u8[16], unsigned)) {
  if (Osz(rde)) {
    SseKernel(XmmRexbRm(m, rde), uimm0);
  } else {
    MmxKernel(XmmRexbRm(m, rde), uimm0);
  }
}

optimizesize void Op171(P) {
  switch (ModrmReg(rde)) {
    case 2:
      OpPsb(A, MmxPsrlw, SsePsrlw);
      break;
    case 4:
      OpPsb(A, MmxPsraw, SsePsraw);
      break;
    case 6:
      OpPsb(A, MmxPsllw, SsePsllw);
      break;
    default:
      OpUdImpl(m);
  }
}

optimizesize void Op172(P) {
  switch (ModrmReg(rde)) {
    case 2:
      OpPsb(A, MmxPsrld, SsePsrld);
      break;
    case 4:
      OpPsb(A, MmxPsrad, SsePsrad);
      break;
    case 6:
      OpPsb(A, MmxPslld, SsePslld);
      break;
    default:
      OpUdImpl(m);
  }
}

optimizesize void Op173(P) {
  switch (ModrmReg(rde)) {
    case 2:
      OpPsb(A, MmxPsrlq, SsePsrlq);
      break;
    case 3:
      OpPsb(A, MmxPsrldq, SsePsrldq);
      break;
    case 6:
      OpPsb(A, MmxPsllq, SsePsllq);
      break;
    case 7:
      OpPsb(A, MmxPslldq, SsePslldq);
      break;
    default:
      OpUdImpl(m);
  }
}

void OpSsePalignr(P) {
  if (Osz(rde)) {
    SsePalignr(XmmRexrReg(m, rde), GetModrmRegisterXmmPointerRead16(A), uimm0);
  } else {
    MmxPalignr(XmmRexrReg(m, rde), GetModrmRegisterXmmPointerRead8(A), uimm0);
  }
}

// clang-format off
void OpSsePunpcklbw(P) { OpSse(A, MmxPunpcklbw, SsePunpcklbw); }
void OpSsePunpcklwd(P) { OpSse(A, MmxPunpcklwd, SsePunpcklwd); }
void OpSsePunpckldq(P) { OpSse(A, MmxPunpckldq, SsePunpckldq); }
void OpSsePacksswb(P) { OpSse(A, MmxPacksswb, SsePacksswb); }
void OpSsePcmpgtd(P) { OpSse(A, MmxPcmpgtd, SsePcmpgtd); }
void OpSsePackuswb(P) { OpSse(A, MmxPackuswb, SsePackuswb); }
void OpSsePunpckhbw(P) { OpSse(A, MmxPunpckhbw, SsePunpckhbw); }
void OpSsePunpckhwd(P) { OpSse(A, MmxPunpckhwd, SsePunpckhwd); }
void OpSsePunpckhdq(P) { OpSse(A, MmxPunpckhdq, SsePunpckhdq); }
void OpSsePackssdw(P) { OpSse(A, MmxPackssdw, SsePackssdw); }
void OpSsePunpcklqdq(P) { OpSse(A, MmxPunpcklqdq, SsePunpcklqdq); }
void OpSsePunpckhqdq(P) { OpSse(A, MmxPunpckhqdq, SsePunpckhqdq); }
void OpSsePcmpeqd(P) { OpSse(A, MmxPcmpeqd, SsePcmpeqd); }
void OpSsePsrlwv(P) { OpSse(A, MmxPsrlwv, SsePsrlwv); }
void OpSsePsrldv(P) { OpSse(A, MmxPsrldv, SsePsrldv); }
void OpSsePsrlqv(P) { OpSse(A, MmxPsrlqv, SsePsrlqv); }
void OpSsePaddq(P) { OpSse(A, MmxPaddq, SsePaddq); }
void OpSsePsubusb(P) { OpSse(A, MmxPsubusb, SsePsubusb); }
void OpSsePsubusw(P) { OpSse(A, MmxPsubusw, SsePsubusw); }
void OpSsePaddusb(P) { OpSse(A, MmxPaddusb, SsePaddusb); }
void OpSsePsrawv(P) { OpSse(A, MmxPsrawv, SsePsrawv); }
void OpSsePsradv(P) { OpSse(A, MmxPsradv, SsePsradv); }
void OpSsePmulhuw(P) { OpSse(A, MmxPmulhuw, SsePmulhuw); }
void OpSsePsubsb(P) { OpSse(A, MmxPsubsb, SsePsubsb); }
void OpSsePminsw(P) { OpSse(A, MmxPminsw, SsePminsw); }
void OpSsePaddsb(P) { OpSse(A, MmxPaddsb, SsePaddsb); }
void OpSsePmaxsw(P) { OpSse(A, MmxPmaxsw, SsePmaxsw); }
void OpSsePsllwv(P) { OpSse(A, MmxPsllwv, SsePsllwv); }
void OpSsePslldv(P) { OpSse(A, MmxPslldv, SsePslldv); }
void OpSsePsllqv(P) { OpSse(A, MmxPsllqv, SsePsllqv); }
void OpSsePmuludq(P) { OpSse(A, MmxPmuludq, SsePmuludq); }
void OpSsePmaddwd(P) { OpSse(A, MmxPmaddwd, SsePmaddwd); }
void OpSsePsadbw(P) { OpSse(A, MmxPsadbw, SsePsadbw); }
void OpSsePsubd(P) { OpSse(A, MmxPsubd, SsePsubd); }
void OpSsePsubq(P) { OpSse(A, MmxPsubq, SsePsubq); }
void OpSsePaddd(P) { OpSse(A, MmxPaddd, SsePaddd); }
void OpSsePshufb(P) { OpSse(A, MmxPshufb, SsePshufb); }
void OpSsePhaddw(P) { OpSse(A, MmxPhaddw, SsePhaddw); }
void OpSsePhaddd(P) { OpSse(A, MmxPhaddd, SsePhaddd); }
void OpSsePmaddubsw(P) { OpSse(A, MmxPmaddubsw, SsePmaddubsw); }
void OpSsePhsubw(P) { OpSse(A, MmxPhsubw, SsePhsubw); }
void OpSsePhsubd(P) { OpSse(A, MmxPhsubd, SsePhsubd); }
void OpSsePsignb(P) { OpSse(A, MmxPsignb, SsePsignb); }
void OpSsePsignw(P) { OpSse(A, MmxPsignw, SsePsignw); }
void OpSsePsignd(P) { OpSse(A, MmxPsignd, SsePsignd); }
void OpSsePmulhrsw(P) { OpSse(A, MmxPmulhrsw, SsePmulhrsw); }
void OpSsePabsw(P) { OpSse(A, MmxPabsw, SsePabsw); }
void OpSsePabsd(P) { OpSse(A, MmxPabsd, SsePabsd); }
void OpSsePmulld(P) { OpSse(A, MmxPmulld, SsePmulld); }
