// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_UNITTESTS_INTERPRETER_BYTECODE_UTILS_H_
#define V8_UNITTESTS_INTERPRETER_BYTECODE_UTILS_H_

#include "src/frames.h"

#if V8_TARGET_LITTLE_ENDIAN

#define EXTRACT(x, n) static_cast<uint8_t>((x) >> (8 * n))
#define U16(i) EXTRACT(i, 0), EXTRACT(i, 1)
#define U32(i) EXTRACT(i, 0), EXTRACT(i, 1), EXTRACT(i, 2), EXTRACT(i, 3)

#elif V8_TARGET_BIG_ENDIAN

#define EXTRACT(x, n) static_cast<uint8_t>((x) >> (8 * n))

#define U16(i) EXTRACT(i, 1), EXTRACT(i, 0)
#define U32(i) EXTRACT(i, 3), EXTRACT(i, 2), EXTRACT(i, 1), EXTRACT(i, 0)

#else

#error "Unknown Architecture"

#endif

#define U8(i) static_cast<uint8_t>(i)
#define REG_OPERAND(i) \
  (InterpreterFrameConstants::kRegisterFileFromFp / kPointerSize - (i))
#define R8(i) static_cast<uint8_t>(REG_OPERAND(i))
#define R16(i) U16(REG_OPERAND(i))
#define R32(i) U32(REG_OPERAND(i))

#endif  // V8_UNITTESTS_INTERPRETER_BYTECODE_UTILS_H_
