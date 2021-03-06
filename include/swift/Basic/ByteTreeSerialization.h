//===--- ByteTreeSerialization.h - ByteTree serialization -------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
/// \file
/// \brief Provides an interface for serializing an object tree to a custom
/// binary format called ByteTree.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_BYTETREESERIALIZATION_H
#define SWIFT_BASIC_BYTETREESERIALIZATION_H

#include "llvm/Support/BinaryStreamError.h"
#include "llvm/Support/BinaryStreamWriter.h"

namespace {
// Only used by compiler if both template types are the same
template <typename T, T>
struct SameType;
} // anonymous namespace

namespace swift {
namespace byteTree {
class ByteTreeWriter;

/// Add a template specialization of \c ObjectTraits for any that type
/// serializes as an object consisting of multiple fields.
template <class T>
struct ObjectTraits {
  // Must provide:

  /// Return the number of fields that will be written in \c write when
  /// \p Object gets serialized.
  // static unsigned numFields(const T &Object);

  /// Serialize \p Object by calling \c Writer.write for all the fields of
  /// \p Object.
  // static void write(BinaryTreeWriter &Writer, const T &Object);
};

/// Add a template specialization of \c ScalarTraits for any that type
/// serializes into a raw set of bytes.
template <class T>
struct ScalarTraits {
  // Must provide:

  /// Return the number of bytes the serialized format of \p Value will take up.
  // static unsigned size(const T &Value);

  /// Serialize \p Value by writing its binary format into \p Writer. Any errors
  /// that may be returned by \p Writer can be returned by this function and
  /// will be handled on the call-side.
  // static llvm::Error write(llvm::BinaryStreamWriter &Writer, const T &Value);
};

/// Add a template specialization of \c WrapperTypeTraits for any that type
/// serializes as a type that already has a specialization of \c ScalarTypes.
/// This will typically be useful for types like enums that have a 1-to-1
/// mapping to e.g. an integer.
template <class T>
struct WrapperTypeTraits {
  // Must provide:

  /// Write the serializable representation of \p Value to \p Writer. This will
  /// typically take the form \c Writer.write(convertedValue(Value), Index)
  /// where \c convertedValue has to be defined.
  // static void write(ByteTreeWriter &Writer, const T &Value, unsigned Index);
};

// Test if ObjectTraits<T> is defined on type T.
template <class T>
struct has_ObjectTraits {
  using Signature_numFields = unsigned (*)(const T &);
  using Signature_write = void (*)(ByteTreeWriter &Writer, const T &Object);

  template <typename U>
  static char test(SameType<Signature_numFields, &U::numFields> *,
                   SameType<Signature_write, &U::write> *);

  template <typename U>
  static double test(...);

public:
  static bool const value =
      (sizeof(test<ObjectTraits<T>>(nullptr, nullptr)) == 1);
};

// Test if ScalarTraits<T> is defined on type T.
template <class T>
struct has_ScalarTraits {
  using Signature_size = unsigned (*)(const T &Object);
  using Signature_write = llvm::Error (*)(llvm::BinaryStreamWriter &Writer,
                                          const T &Object);

  template <typename U>
  static char test(SameType<Signature_size, &U::size> *,
                   SameType<Signature_write, &U::write> *);

  template <typename U>
  static double test(...);

public:
  static bool const value =
      (sizeof(test<ScalarTraits<T>>(nullptr, nullptr)) == 1);
};

// Test if WrapperTypeTraits<T> is defined on type T.
template <class T>
struct has_WrapperTypeTraits {
  using Signature_write = void (*)(ByteTreeWriter &Writer, const T &Object,
                                   unsigned Index);

  template <typename U>
  static char test(SameType<Signature_write, &U::write> *);

  template <typename U>
  static double test(...);

public:
  static bool const value = (sizeof(test<WrapperTypeTraits<T>>(nullptr)) == 1);
};

class ByteTreeWriter {
private:
  /// The writer to which the binary data is written.
  llvm::BinaryStreamWriter &StreamWriter;

  /// The number of fields this object contains. \c UINT_MAX if it has not been
  /// set yet. No member may be written to the object if expected number of
  /// fields has not been set yet.
  unsigned NumFields = UINT_MAX;

  /// The index of the next field to write. Used in assertion builds to keep
  /// track that no indicies are jumped and that the object contains the
  /// expected number of fields.
  unsigned CurrentFieldIndex = 0;

  /// The \c ByteTreeWriter can only be constructed internally. Use
  /// \c ByteTreeWriter.write to serialize a new object.
  ByteTreeWriter(llvm::BinaryStreamWriter &StreamWriter)
      : StreamWriter(StreamWriter) {}

  /// Set the expected number of fields the object written by this writer is
  /// expected to have.
  void setNumFields(uint32_t NumFields) {
    assert(NumFields != UINT_MAX &&
           "NumFields may not be reset since it has already been written to "
           "the byte stream");
    assert((this->NumFields == UINT_MAX) && "NumFields has already been set");

    auto Error = StreamWriter.writeInteger(NumFields);
    (void)Error;
    assert(!Error);

    this->NumFields = NumFields;
  }

  /// Validate that \p Index is the next field that is expected to be written,
  /// does not exceed the number of fields in this object and that
  /// \c setNumFields has already been called.
  void validateAndIncreaseFieldIndex(unsigned Index) {
    assert((NumFields != UINT_MAX) &&
           "setNumFields must be called before writing any value");
    assert(Index == CurrentFieldIndex && "Writing index out of order");
    assert(Index < NumFields &&
           "Writing more fields than object is expected to have");

    CurrentFieldIndex++;
  }

  ~ByteTreeWriter() {
    assert(CurrentFieldIndex == NumFields &&
           "Object had more or less elements than specified");
  }

public:
  /// Write a binary serialization of \p Object to \p StreamWriter, prefixing
  /// the stream by the specified ProtocolVersion.
  template <typename T>
  typename std::enable_if<has_ObjectTraits<T>::value, void>::type
  static write(uint32_t ProtocolVersion, llvm::BinaryStreamWriter &StreamWriter,
              const T &Object) {
    ByteTreeWriter Writer(StreamWriter);

    auto Error = Writer.StreamWriter.writeInteger(ProtocolVersion);
    (void)Error;
    assert(!Error);

    // There always is one root. We need to set NumFields so that index
    // validation succeeds, but we don't want to serialize this.
    Writer.NumFields = 1;
    Writer.write(Object, /*Index=*/0);
  }

  template <typename T>
  typename std::enable_if<has_ObjectTraits<T>::value, void>::type
  write(const T &Object, unsigned Index) {
    validateAndIncreaseFieldIndex(Index);

    auto ObjectWriter = ByteTreeWriter(StreamWriter);
    ObjectWriter.setNumFields(ObjectTraits<T>::numFields(Object));

    ObjectTraits<T>::write(ObjectWriter, Object);
  }

  template <typename T>
  typename std::enable_if<has_ScalarTraits<T>::value, void>::type
  write(const T &Value, unsigned Index) {
    validateAndIncreaseFieldIndex(Index);

    uint32_t ValueSize = ScalarTraits<T>::size(Value);
    auto SizeError = StreamWriter.writeInteger(ValueSize);
    (void)SizeError;
    assert(!SizeError);

    auto StartOffset = StreamWriter.getOffset();
    auto ContentError = ScalarTraits<T>::write(StreamWriter, Value);
    (void)ContentError;
    assert(!ContentError);
    (void)StartOffset;
    assert((StreamWriter.getOffset() - StartOffset == ValueSize) &&
           "Number of written bytes does not match size returned by "
           "ScalarTraits<T>::size");
  }

  template <typename T>
  typename std::enable_if<has_WrapperTypeTraits<T>::value, void>::type
  write(const T &Value, unsigned Index) {
    auto LengthBeforeWrite = CurrentFieldIndex;
    WrapperTypeTraits<T>::write(*this, Value, Index);
    (void)LengthBeforeWrite;
    assert(CurrentFieldIndex == LengthBeforeWrite + 1 &&
           "WrapperTypeTraits did not call BinaryWriter.write");
  }
};

// Define serialization schemes for common types

template <>
struct ScalarTraits<uint8_t> {
  static unsigned size(const uint8_t &Value) { return 1; }
  static llvm::Error write(llvm::BinaryStreamWriter &Writer,
                           const uint8_t &Value) {
    return Writer.writeInteger(Value);
  }
};

template <>
struct ScalarTraits<uint16_t> {
  static unsigned size(const uint16_t &Value) { return 2; }
  static llvm::Error write(llvm::BinaryStreamWriter &Writer,
                           const uint16_t &Value) {
    return Writer.writeInteger(Value);
  }
};

template <>
struct ScalarTraits<uint32_t> {
  static unsigned size(const uint32_t &Value) { return 4; }
  static llvm::Error write(llvm::BinaryStreamWriter &Writer,
                           const uint32_t &Value) {
    return Writer.writeInteger(Value);
  }
};

template <>
struct WrapperTypeTraits<bool> {
  static void write(ByteTreeWriter &Writer, const bool &Value,
                    unsigned Index) {
    Writer.write(static_cast<uint8_t>(Value), Index);
  }
};

template <>
struct ScalarTraits<llvm::StringRef> {
  static unsigned size(const llvm::StringRef &Str) { return Str.size(); }
  static llvm::Error write(llvm::BinaryStreamWriter &Writer,
                           const llvm::StringRef &Str) {
    return Writer.writeFixedString(Str);
  }
};

template <>
struct ScalarTraits<llvm::NoneType> {
  // Serialize llvm::None as a value with 0 length
  static unsigned size(const llvm::NoneType &None) { return 0; }
  static llvm::Error write(llvm::BinaryStreamWriter &Writer,
                           const llvm::NoneType &None) {
    // Nothing to write
    return llvm::ErrorSuccess();
  }
};

} // end namespace byteTree
} // end namespace swift

#endif
