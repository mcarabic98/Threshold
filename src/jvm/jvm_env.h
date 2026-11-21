#pragma once
#include <cstddef>
#include <cstdint>

namespace jvm {

// Version-independent JVM object model, decoded from VMStructs at runtime.
struct JvmEnv {
  bool valid = false;

  // compressed data oops (Universe::_narrow_oop_*); base/shift are the live
  // values read from the target process. Base 0 + shift 0 is a valid
  // "zero-based" compressed encoding, not "uncompressed".
  bool compressedOops = true;
  uint64_t narrowOopBase = 0;
  int narrowOopShift = 0;

  // compressed class pointers (Universe::_narrow_klass_*)
  bool compressedKlasses = true;
  uint64_t narrowKlassBase = 0;
  int narrowKlassShift = 0;

  // Klass / Symbol offsets
  int64_t offKlassName = -1;         // Klass::_name -> Symbol*
  int64_t offKlassLayoutHelper = -1; // Klass::_layout_helper (jint)
  int64_t offSymbolLength = -1;      // Symbol::_length (u2)
  int64_t offSymbolBody = -1;        // Symbol::_body (bytes)

  // Layout-helper decoding (VM int constants; JDK 21: shift30/8/16 encodings)
  int lhInstanceSlowPathBit = 0;
  int lhLog2ElemShift = 0;
  int lhLog2ElemMask = 0;
  int lhHeaderSizeShift = 0;
  int lhHeaderSizeMask = 0;
  int lhArrayTagShift = 0;
  int lhArrayTagTypeVal = 0;
  int lhArrayTagOopVal = 0;

  // Instance-size decoding mode, calibrated on the live heap:
  //   1 = (lh & ~slow) >> 3        2 = (lh & ~slow)
  //   3 = (lh & ~slow) * 8         4 = (lh & ~slow) >> 1   (bytes<<1 encoding)
  //   5 = ((lh & ~slow) >> 1) * 8                          (words<<1 encoding)
  int instanceSizeMode = 1;

  size_t instanceSize(uint32_t lh, int modeOverride = 0) const {
    uint32_t v = lh & ~(uint32_t)lhInstanceSlowPathBit;
    int mode = modeOverride ? modeOverride : instanceSizeMode;
    switch (mode) {
      case 2: return (size_t)v;
      case 3: return (size_t)v * 8;
      case 4: return (size_t)v >> 1;
      case 5: return ((size_t)v >> 1) * 8;
      default: return (size_t)v >> 3;
    }
  }

  bool lhIsArray(uint32_t lh) const { return (int32_t)lh < 0; }

  size_t arrayElemSize(uint32_t lh) const {
    return (size_t)1u << ((lh >> lhLog2ElemShift) & lhLog2ElemMask);
  }

  // Array header (elements offset) straight from the layout helper when the
  // JDK encodes it; otherwise derived from the compressed-pointer layout.
  size_t arrayHeaderSize(uint32_t lh) const {
    if (lhHeaderSizeMask > 0) {
      size_t h = (size_t)((lh >> lhHeaderSizeShift) & lhHeaderSizeMask);
      if (h > 0) return h;
    }
    return headerSize();
  }

  // object header / array length offset
  size_t headerSize() const { return compressedKlasses ? 16 : 24; }
  size_t arrayLengthOffset() const { return compressedKlasses ? 12 : 16; }
};

} // namespace jvm
