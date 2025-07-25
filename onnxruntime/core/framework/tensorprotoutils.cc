// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/framework/tensorprotoutils.h"

#include <memory>
#include <algorithm>
#include <limits>
#include <string>
#include <filesystem>
#if defined(__wasm__)
#include <emscripten.h>
#endif

#include <gsl/gsl>
#include "core/common/logging/logging.h"
#include "core/common/narrow.h"
#include "core/common/span_utils.h"
#include "core/graph/onnx_protobuf.h"
#include "core/framework/endian_utils.h"
#include "core/framework/op_kernel.h"
#include "core/framework/tensor.h"
#include "core/framework/ort_value_pattern_planner.h"
#include "core/framework/allocator.h"
#include "core/framework/data_types.h"
#include "core/platform/path_lib.h"
#include "core/framework/to_tensor_proto_element_type.h"
#include "core/session/ort_apis.h"
#include "onnx/defs/tensor_proto_util.h"

using namespace ONNX_NAMESPACE;
using namespace ::onnxruntime::common;
using namespace ::onnxruntime::utils;

TensorProto ToTensorInitialize(TensorProto_DataType datatype) {
  TensorProto t;
  t.clear_int32_data();
  t.set_data_type(datatype);
  return t;
}

TensorProto ToScalarTensor(TensorProto_DataType datatype, int32_t value) {
  TensorProto t = ToTensorInitialize(datatype);
  t.add_int32_data(value);
  return t;
}

#define TO_TENSOR_ORT_TYPE(TYPE)                                                          \
  template <>                                                                             \
  TensorProto ToTensor<onnxruntime::TYPE>(const onnxruntime::TYPE& value) {               \
    return ToScalarTensor(ToTensorProtoElementType<onnxruntime::TYPE>(), value.val);      \
  }                                                                                       \
  template <>                                                                             \
  TensorProto ToTensor<onnxruntime::TYPE>(const std::vector<onnxruntime::TYPE>& values) { \
    TensorProto t = ToTensorInitialize(ToTensorProtoElementType<onnxruntime::TYPE>());    \
    for (const onnxruntime::TYPE& val : values) {                                         \
      t.add_int32_data(val.val);                                                          \
    }                                                                                     \
    return t;                                                                             \
  }

#define TO_TENSOR_ORT_TYPE_INT4(TYPE)                                                                           \
  template <>                                                                                                   \
  TensorProto ToTensor<onnxruntime::TYPE>(const onnxruntime::TYPE& value) {                                     \
    return ToScalarTensor(ToTensorProtoElementType<onnxruntime::TYPE>(), static_cast<int32_t>(value.ToBits())); \
  }                                                                                                             \
  template <>                                                                                                   \
  TensorProto ToTensor<onnxruntime::TYPE>(const std::vector<onnxruntime::TYPE>& values) {                       \
    TensorProto t = ToTensorInitialize(ToTensorProtoElementType<onnxruntime::TYPE>());                          \
    for (const onnxruntime::TYPE& val : values) {                                                               \
      t.add_int32_data(static_cast<int32_t>(val.ToBits()));                                                     \
    }                                                                                                           \
    return t;                                                                                                   \
  }

namespace ONNX_NAMESPACE {

// Provide template specializations for onnxruntime-specific types.
TO_TENSOR_ORT_TYPE(MLFloat16)
TO_TENSOR_ORT_TYPE(BFloat16)
#if !defined(DISABLE_FLOAT8_TYPES)
TO_TENSOR_ORT_TYPE(Float8E4M3FN)
TO_TENSOR_ORT_TYPE(Float8E4M3FNUZ)
TO_TENSOR_ORT_TYPE(Float8E5M2)
TO_TENSOR_ORT_TYPE(Float8E5M2FNUZ)
#endif
TO_TENSOR_ORT_TYPE_INT4(Int4x2)
TO_TENSOR_ORT_TYPE_INT4(UInt4x2)

bool operator==(const ONNX_NAMESPACE::TensorShapeProto_Dimension& l,
                const ONNX_NAMESPACE::TensorShapeProto_Dimension& r) {
  if (l.has_dim_value()) {
    return r.has_dim_value() && l.dim_value() == r.dim_value();
  } else if (l.has_dim_param()) {
    return r.has_dim_param() && l.dim_param() == r.dim_param() && !l.dim_param().empty();
  } else {
    // l is unknown - has neither dim_value nor dim_param
  }

  return false;
}

bool operator!=(const ONNX_NAMESPACE::TensorShapeProto_Dimension& l,
                const ONNX_NAMESPACE::TensorShapeProto_Dimension& r) {
  return !(l == r);
}

}  // namespace ONNX_NAMESPACE

namespace onnxruntime {
namespace {

// This function doesn't support string tensors
static Status UnpackTensorWithRawDataImpl(const void* raw_data, size_t raw_data_len, size_t expected_num_elements,
                                          size_t element_size,
                                          /*out*/ unsigned char* p_data) {
  auto src = gsl::make_span<const unsigned char>(static_cast<const unsigned char*>(raw_data), raw_data_len);
  auto dst = gsl::make_span<unsigned char>(p_data, expected_num_elements * element_size);

  size_t expected_size_in_bytes;
  if (!onnxruntime::IAllocator::CalcMemSizeForArray(expected_num_elements, element_size, &expected_size_in_bytes)) {
    return Status(onnxruntime::common::ONNXRUNTIME, onnxruntime::common::INVALID_ARGUMENT, "size overflow");
  }

  if (dst.size_bytes() != expected_size_in_bytes) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "UnpackTensor: the pre-allocated size does not match the raw data size, expected ",
                           expected_size_in_bytes, ", got ", dst.size_bytes());
  }

  // ReadLittleEndian checks src and dst buffers are the same size
  return onnxruntime::utils::ReadLittleEndian(element_size, src, dst);
}

template <typename T>
Status UnpackTensorWithRawData(const void* raw_data, size_t raw_data_len, size_t expected_num_elements,
                               /*out*/ T* p_data) {
  static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");

  return UnpackTensorWithRawDataImpl(raw_data, raw_data_len, expected_num_elements, sizeof(T),
                                     reinterpret_cast<unsigned char*>(p_data));
}

#define DEFINE_INT4_UNPACK_TENSOR_WITH_RAW_DATA_IMPL(INT4_TYPE)                                                      \
  template <>                                                                                                        \
  Status UnpackTensorWithRawData<INT4_TYPE>(const void* raw_data, size_t raw_data_len, size_t expected_num_elements, \
                                            /*out*/ INT4_TYPE* p_data) {                                             \
    static_assert(std::is_trivially_copyable<INT4_TYPE>::value, "T must be trivially copyable");                     \
                                                                                                                     \
    ORT_RETURN_IF(nullptr == p_data, "nullptr == p_data");                                                           \
                                                                                                                     \
    size_t num_packed_pairs = INT4_TYPE::CalcNumInt4Pairs(expected_num_elements);                                    \
    ORT_RETURN_IF_NOT(num_packed_pairs == raw_data_len, "Unexpected number of packed int4 pairs");                   \
                                                                                                                     \
    gsl::span<const INT4_TYPE> src_span =                                                                            \
        gsl::make_span(reinterpret_cast<const INT4_TYPE*>(raw_data), num_packed_pairs);                              \
    gsl::span<INT4_TYPE> dst_span = gsl::make_span(p_data, num_packed_pairs);                                        \
                                                                                                                     \
    std::memcpy(dst_span.data(), src_span.data(), num_packed_pairs);                                                 \
                                                                                                                     \
    return Status::OK();                                                                                             \
  }

DEFINE_INT4_UNPACK_TENSOR_WITH_RAW_DATA_IMPL(Int4x2)
DEFINE_INT4_UNPACK_TENSOR_WITH_RAW_DATA_IMPL(UInt4x2)

// Read external data for tensor in unint8_t* form and return Status::OK() if the data is read successfully.
// Uses the tensor_proto_dir to construct the full path for external data. If tensor_proto_dir == nullptr
// then uses the current directory instead.
// This function does not unpack string_data of an initializer tensor
Status ReadExternalDataForTensor(const ONNX_NAMESPACE::TensorProto& tensor_proto,
                                 const std::filesystem::path& tensor_proto_dir,
                                 std::vector<uint8_t>& unpacked_tensor) {
  PathString external_file_path;
  onnxruntime::FileOffsetType file_offset;
  SafeInt<size_t> tensor_byte_size;
  ORT_RETURN_IF_ERROR(
      GetExternalDataInfo(tensor_proto, tensor_proto_dir, external_file_path, file_offset, tensor_byte_size));

  unpacked_tensor.resize(tensor_byte_size);

  if (external_file_path == kTensorProtoMemoryAddressTag) {
    // The external data is in the same memory as the tensor proto.
    // The offset is the address of the data.
    std::memcpy(unpacked_tensor.data(), reinterpret_cast<const void*>(file_offset), tensor_byte_size);
    return Status::OK();
  }

  ORT_RETURN_IF_ERROR(onnxruntime::Env::Default().ReadFileIntoBuffer(
      external_file_path.c_str(),
      file_offset,
      tensor_byte_size,
      gsl::make_span(reinterpret_cast<char*>(unpacked_tensor.data()), tensor_byte_size)));

  return Status::OK();
}

Status TensorProtoToOrtValueImpl(const Env& env, const std::filesystem::path& model_path,
                                 const ONNX_NAMESPACE::TensorProto& tensor_proto, const MemBuffer* m,
                                 AllocatorPtr alloc, OrtValue& value) {
  if (m && m->GetBuffer() == nullptr) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "MemBuffer has not been allocated.");
  }

  // to construct a Tensor with std::string we need to pass an allocator to the Tensor ctor
  // as the contents of each string needs to be allocated and freed separately.
  ONNXTensorElementDataType ele_type = utils::GetTensorElementType(tensor_proto);
  if (ele_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING && (m || !alloc)) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "string tensor requires allocator to be provided.");
  }

  // Note: We permit an empty tensor_shape_vec, and treat it as a scalar (a tensor of size 1).
  TensorShape tensor_shape = GetTensorShapeFromTensorProto(tensor_proto);
  const DataTypeImpl* const type = DataTypeImpl::TensorTypeFromONNXEnum(tensor_proto.data_type())->GetElementType();

  std::unique_ptr<Tensor> tensor;

  if (m) {
    tensor = std::make_unique<Tensor>(type, tensor_shape, m->GetBuffer(), m->GetAllocInfo());

    if (tensor->SizeInBytes() > m->GetLen()) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "The preallocated buffer is too small. Requires ",
                             tensor->SizeInBytes(), ", Got ", m->GetLen());
    }
  } else {
    tensor = std::make_unique<Tensor>(type, tensor_shape, std::move(alloc));
  }

  ORT_RETURN_IF_ERROR(TensorProtoToTensor(env, model_path, tensor_proto, *tensor));

  auto ml_tensor = DataTypeImpl::GetType<Tensor>();
  value.Init(tensor.release(), ml_tensor, ml_tensor->GetDeleteFunc());
  return Status::OK();
}

}  // namespace

namespace utils {

bool HasExternalDataInMemory(const ONNX_NAMESPACE::TensorProto& ten_proto) {
  if (HasExternalData(ten_proto)) {
    // Retrieve the external data info
    for (const auto& entry : ten_proto.external_data()) {
      if (entry.key() == "location") {
        PathString location = ToWideString(entry.value());
        return location == kTensorProtoMemoryAddressTag;
      }
    }
  }

  return false;  // No external data in memory
}

Status TensorProtoWithExternalDataToTensorProto(
    const ONNX_NAMESPACE::TensorProto& ten_proto,
    const std::filesystem::path& model_path,
    ONNX_NAMESPACE::TensorProto& new_tensor_proto) {
  // Check if the input tensor has external data
  ORT_RETURN_IF_NOT(HasExternalData(ten_proto), "Input tensor does not have external data.");

  // Copy the metadata from the source tensor to the new tensor
  ONNX_NAMESPACE::TensorProto result;
  result.set_name(ten_proto.name());
  result.set_data_type(ten_proto.data_type());
  result.mutable_dims()->CopyFrom(ten_proto.dims());

  // Load the external data into memory
  std::vector<uint8_t> unpacked_data;
  ORT_RETURN_IF_ERROR(ReadExternalDataForTensor(ten_proto, model_path, unpacked_data));

  // Set the raw data in the new tensor
  result.set_raw_data(unpacked_data.data(), unpacked_data.size());

  new_tensor_proto = std::move(result);

  return Status::OK();
}

Status GetExternalDataInfo(const ONNX_NAMESPACE::TensorProto& tensor_proto,
                           const std::filesystem::path& tensor_proto_dir,
                           std::basic_string<ORTCHAR_T>& external_file_path,
                           onnxruntime::FileOffsetType& file_offset,
                           SafeInt<size_t>& tensor_byte_size,
                           ExternalDataInfo::PrepackedInfos* prepacked_infos) {
  ORT_RETURN_IF_NOT(HasExternalData(tensor_proto),
                    "Tensor does not have external data to read from.");

  ORT_RETURN_IF(!HasDataType(tensor_proto) || HasString(tensor_proto),
                "External data type cannot be UNDEFINED or STRING.");

  std::unique_ptr<onnxruntime::ExternalDataInfo> external_data_info;
  ORT_RETURN_IF_ERROR(onnxruntime::ExternalDataInfo::Create(tensor_proto.external_data(), external_data_info));

  const auto& location = external_data_info->GetRelPath();

  external_file_path = location == kTensorProtoMemoryAddressTag ? std::filesystem::path(location)
                                                                : (tensor_proto_dir / location);

  ORT_RETURN_IF_ERROR(GetSizeInBytesFromTensorProto<0>(tensor_proto, &tensor_byte_size));
  const size_t external_data_length = external_data_info->GetLength();
  ORT_RETURN_IF_NOT(external_data_length == 0 || external_data_length == tensor_byte_size,
                    "TensorProto: ", tensor_proto.name(),
                    " external data size mismatch. Computed size: ", *&tensor_byte_size,
                    ", external_data.length: ", external_data_length);

  file_offset = external_data_info->GetOffset();

  if (prepacked_infos != nullptr && external_data_info->HasPrepackedInfo()) {
    *prepacked_infos = external_data_info->TakePrepackedInfos();
  }

  return Status::OK();
}

void SetRawDataInTensorProto(ONNX_NAMESPACE::TensorProto& tensor_proto, std::string&& param) {
  tensor_proto.set_raw_data(std::move(param));
}

void ConvertRawDataInTensorProto(TensorProto& tensor) {
  size_t element_size = 1;
  void* bytes = NULL;
  size_t num_elements = 0;

  switch (tensor.data_type()) {
    case TensorProto_DataType_FLOAT:
      bytes = tensor.mutable_float_data()->mutable_data();
      num_elements = tensor.float_data_size();
      element_size = sizeof(float);
      break;

    case TensorProto_DataType_UINT8:
    case TensorProto_DataType_INT8:
      bytes = tensor.mutable_int32_data()->mutable_data();
      num_elements = tensor.int32_data_size();
      element_size = sizeof(uint8_t);
      break;

    case TensorProto_DataType_UINT16:
    case TensorProto_DataType_INT16:
    case TensorProto_DataType_FLOAT16:
    case TensorProto_DataType_BFLOAT16:
    case TensorProto_DataType_INT32:
      bytes = tensor.mutable_int32_data()->mutable_data();
      num_elements = tensor.int32_data_size();
      // We are setting this to int32_t size because we need to swap all 4 bytes
      // to represent 16 bits within 32 bits correctly on a LE/BE system.
      element_size = sizeof(int32_t);
      break;

    // uint32_t is stored in uint64_t
    case TensorProto_DataType_UINT32:
    case TensorProto_DataType_UINT64:
      bytes = tensor.mutable_uint64_data()->mutable_data();
      num_elements = tensor.uint64_data_size();
      element_size = sizeof(uint64_t);
      break;

    case TensorProto_DataType_INT64:
      bytes = tensor.mutable_int64_data()->mutable_data();
      num_elements = tensor.int64_data_size();
      element_size = sizeof(int64_t);
      break;

    case TensorProto_DataType_DOUBLE:
      bytes = tensor.mutable_double_data()->mutable_data();
      num_elements = tensor.double_data_size();
      element_size = sizeof(double);
      break;

    case TensorProto_DataType_COMPLEX64:
      bytes = tensor.mutable_float_data()->mutable_data();
      num_elements = tensor.float_data_size();
      element_size = sizeof(float);
      break;
  }

  if (element_size == 1) {
    return;
  }

  if (tensor.has_raw_data()) {
    num_elements = tensor.raw_data().size() / element_size;
    bytes = tensor.mutable_raw_data()->data();
  }

  gsl::span<std::byte> span = gsl::make_span(reinterpret_cast<std::byte*>(bytes), num_elements * element_size);
  SwapByteOrderInplace(element_size, span);
}

#if !defined(ORT_MINIMAL_BUILD)

static Status UnpackTensorWithExternalDataImpl(const ONNX_NAMESPACE::TensorProto& tensor,
                                               const std::filesystem::path& tensor_proto_dir,
                                               size_t expected_num_elements, size_t element_size,
                                               /*out*/ unsigned char* p_data) {
  ORT_RETURN_IF(nullptr == p_data, "nullptr == p_data");
  std::vector<uint8_t> unpacked_tensor;
  ORT_RETURN_IF_ERROR(ReadExternalDataForTensor(tensor, tensor_proto_dir, unpacked_tensor));

  // ReadLittleEndian checks src and dst buffers are the same size
  auto src_span = gsl::make_span(unpacked_tensor.data(), unpacked_tensor.size());
  auto dst_span = gsl::make_span(p_data, expected_num_elements * element_size);

  return onnxruntime::utils::ReadLittleEndian(element_size, src_span, dst_span);
}

template <typename T>
Status UnpackTensorWithExternalData(const ONNX_NAMESPACE::TensorProto& tensor,
                                    const std::filesystem::path& tensor_proto_dir, size_t expected_num_elements,
                                    /*out*/ T* p_data) {
  static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");

  return UnpackTensorWithExternalDataImpl(tensor, tensor_proto_dir, expected_num_elements, sizeof(T),
                                          reinterpret_cast<unsigned char*>(p_data));
}

#define DEFINE_INT4_UNPACK_TENSOR_WITH_EXT_DATA_IMPL(INT4_TYPE)                                              \
  template <>                                                                                                \
  Status UnpackTensorWithExternalData<INT4_TYPE>(const ONNX_NAMESPACE::TensorProto& tensor,                  \
                                                 const std::filesystem::path& tensor_proto_dir,              \
                                                 size_t expected_num_elements, /*out*/ INT4_TYPE* p_data) {  \
    static_assert(std::is_trivially_copyable<INT4_TYPE>::value, "T must be trivially copyable");             \
                                                                                                             \
    ORT_RETURN_IF(nullptr == p_data, "nullptr == p_data");                                                   \
    std::vector<uint8_t> unpacked_tensor;                                                                    \
    ORT_RETURN_IF_ERROR(ReadExternalDataForTensor(tensor, tensor_proto_dir, unpacked_tensor));               \
                                                                                                             \
    size_t num_packed_pairs = INT4_TYPE::CalcNumInt4Pairs(expected_num_elements);                            \
    ORT_RETURN_IF_NOT(num_packed_pairs == unpacked_tensor.size(), "Unexpected number of packed int4 pairs"); \
                                                                                                             \
    gsl::span<const INT4_TYPE> src_span =                                                                    \
        gsl::make_span(reinterpret_cast<const INT4_TYPE*>(unpacked_tensor.data()), num_packed_pairs);        \
    gsl::span<INT4_TYPE> dst_span = gsl::make_span(p_data, expected_num_elements);                           \
                                                                                                             \
    std::memcpy(dst_span.data(), src_span.data(), num_packed_pairs);                                         \
                                                                                                             \
    return Status::OK();                                                                                     \
  }

DEFINE_INT4_UNPACK_TENSOR_WITH_EXT_DATA_IMPL(Int4x2)
DEFINE_INT4_UNPACK_TENSOR_WITH_EXT_DATA_IMPL(UInt4x2)

#define INSTANTIATE_UNPACK_EXTERNAL_TENSOR(type)                                                                 \
  template Status UnpackTensorWithExternalData(const ONNX_NAMESPACE::TensorProto&, const std::filesystem::path&, \
                                               size_t, type*);

INSTANTIATE_UNPACK_EXTERNAL_TENSOR(float)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(double)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(uint8_t)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(int8_t)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(int16_t)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(uint16_t)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(int32_t)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(int64_t)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(uint64_t)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(uint32_t)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(bool)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(MLFloat16)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(BFloat16)

#if !defined(DISABLE_FLOAT8_TYPES)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(Float8E4M3FN)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(Float8E4M3FNUZ)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(Float8E5M2)
INSTANTIATE_UNPACK_EXTERNAL_TENSOR(Float8E5M2FNUZ)
#endif

template <>
Status UnpackTensorWithExternalData(const ONNX_NAMESPACE::TensorProto& /*tensor*/,
                                    const std::filesystem::path& /*tensor_proto_dir*/, size_t /*expected_num_elements*/,
                                    /*out*/ std::string* /*p_data*/) {
  return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "External data type cannot be STRING.");
}
#endif  //! defined(ORT_MINIMAL_BUILD)

// implementation of type specific unpack of data contained within the TensorProto
template <typename T>
Status UnpackTensor(const ONNX_NAMESPACE::TensorProto& tensor, const void* raw_data, size_t raw_data_len,
                    /*out*/ T* p_data, size_t expected_num_elements);

#define DEFINE_UNPACK_TENSOR_IMPL(T, Type, field_name, field_size)                                          \
  template <>                                                                                               \
  Status UnpackTensor(const ONNX_NAMESPACE::TensorProto& tensor, const void* raw_data, size_t raw_data_len, \
                      /*out*/ T* p_data, size_t expected_num_elements) {                                    \
    if (nullptr == p_data) {                                                                                \
      const size_t size = raw_data != nullptr ? raw_data_len : tensor.field_size();                         \
      if (size == 0)                                                                                        \
        return Status::OK();                                                                                \
      return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);                                         \
    }                                                                                                       \
    if (nullptr == p_data || Type != tensor.data_type()) {                                                  \
      return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);                                         \
    }                                                                                                       \
    if (raw_data != nullptr) {                                                                              \
      return UnpackTensorWithRawData(raw_data, raw_data_len, expected_num_elements, p_data);                \
    }                                                                                                       \
    if (static_cast<size_t>(tensor.field_size()) != expected_num_elements)                                  \
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "corrupted protobuf data: tensor shape size(",  \
                             expected_num_elements, ") does not match the data size(", tensor.field_size(), \
                             ") in proto");                                                                 \
    auto& data = tensor.field_name();                                                                       \
    for (auto data_iter = data.cbegin(); data_iter != data.cend(); ++data_iter)                             \
      *p_data++ = static_cast<T>(*data_iter);                                                               \
    return Status::OK();                                                                                    \
  }

// TODO: complex64 complex128
DEFINE_UNPACK_TENSOR_IMPL(float, ONNX_NAMESPACE::TensorProto_DataType_FLOAT, float_data, float_data_size)
DEFINE_UNPACK_TENSOR_IMPL(double, ONNX_NAMESPACE::TensorProto_DataType_DOUBLE, double_data, double_data_size);
DEFINE_UNPACK_TENSOR_IMPL(uint8_t, ONNX_NAMESPACE::TensorProto_DataType_UINT8, int32_data, int32_data_size)
DEFINE_UNPACK_TENSOR_IMPL(int8_t, ONNX_NAMESPACE::TensorProto_DataType_INT8, int32_data, int32_data_size)
DEFINE_UNPACK_TENSOR_IMPL(int16_t, ONNX_NAMESPACE::TensorProto_DataType_INT16, int32_data, int32_data_size)
DEFINE_UNPACK_TENSOR_IMPL(uint16_t, ONNX_NAMESPACE::TensorProto_DataType_UINT16, int32_data, int32_data_size)
DEFINE_UNPACK_TENSOR_IMPL(int32_t, ONNX_NAMESPACE::TensorProto_DataType_INT32, int32_data, int32_data_size)
DEFINE_UNPACK_TENSOR_IMPL(int64_t, ONNX_NAMESPACE::TensorProto_DataType_INT64, int64_data, int64_data_size)
DEFINE_UNPACK_TENSOR_IMPL(uint64_t, ONNX_NAMESPACE::TensorProto_DataType_UINT64, uint64_data, uint64_data_size)
DEFINE_UNPACK_TENSOR_IMPL(uint32_t, ONNX_NAMESPACE::TensorProto_DataType_UINT32, uint64_data, uint64_data_size)

//
// Specializations of UnpackTensor that need custom handling for the input type
//

// UnpackTensor<std::string>. Note: doesn't support raw data
template <>
Status UnpackTensor(const ONNX_NAMESPACE::TensorProto& tensor, const void* /*raw_data*/, size_t /*raw_data_len*/,
                    /*out*/ std::string* p_data, size_t expected_size) {
  if (nullptr == p_data) {
    if (tensor.string_data_size() == 0)
      return Status::OK();
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }
  if (ONNX_NAMESPACE::TensorProto_DataType_STRING != tensor.data_type()) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }

  if (static_cast<size_t>(tensor.string_data_size()) != expected_size)
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                  "UnpackTensor: the pre-allocate size does not match the size in proto");

  auto& string_data = tensor.string_data();
  for (const auto& iter : string_data) {
    *p_data++ = iter;
  }

  return Status::OK();
}

// UnpackTensor<bool>
template <>
Status UnpackTensor(const ONNX_NAMESPACE::TensorProto& tensor, const void* raw_data, size_t raw_data_len,
                    /*out*/ bool* p_data, size_t expected_size) {
  if (nullptr == p_data) {
    const size_t size = raw_data != nullptr ? raw_data_len : tensor.int32_data_size();
    if (size == 0)
      return Status::OK();
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }
  if (ONNX_NAMESPACE::TensorProto_DataType_BOOL != tensor.data_type()) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }

  if (raw_data != nullptr) {
    return UnpackTensorWithRawData(raw_data, raw_data_len, expected_size, p_data);
  }

  if (static_cast<size_t>(tensor.int32_data_size()) != expected_size)
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                  "UnpackTensor: the pre-allocate size does not match the size in proto");
  for (int iter : tensor.int32_data()) {
    *p_data++ = static_cast<bool>(iter);
  }

  return Status::OK();
}

// UnpackTensor<MLFloat16>
template <>
Status UnpackTensor(const ONNX_NAMESPACE::TensorProto& tensor, const void* raw_data, size_t raw_data_len,
                    /*out*/ MLFloat16* p_data, size_t expected_size) {
  if (nullptr == p_data) {
    const size_t size = raw_data != nullptr ? raw_data_len : tensor.int32_data_size();
    if (size == 0)
      return Status::OK();
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }
  if (ONNX_NAMESPACE::TensorProto_DataType_FLOAT16 != tensor.data_type()) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }

  if (raw_data != nullptr) {
    return UnpackTensorWithRawData(raw_data, raw_data_len, expected_size, p_data);
  }

  if (static_cast<size_t>(tensor.int32_data_size()) != expected_size)
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                  "UnpackTensor: the pre-allocate size does not match the size in proto");

  constexpr int max_value = std::numeric_limits<uint16_t>::max();
  for (int i = 0; i < static_cast<int>(expected_size); i++) {
    int v = tensor.int32_data()[i];
    if (v < 0 || v > max_value) {
      return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "data overflow");
    }
    p_data[i] = MLFloat16::FromBits(static_cast<uint16_t>(v));
  }

  return Status::OK();
}

// UnpackTensor<BFloat16>
template <>
Status UnpackTensor(const ONNX_NAMESPACE::TensorProto& tensor, const void* raw_data, size_t raw_data_len,
                    /*out*/ BFloat16* p_data, size_t expected_size) {
  if (nullptr == p_data) {
    const size_t size = raw_data != nullptr ? raw_data_len : tensor.int32_data_size();
    if (size == 0)
      return Status::OK();

    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }
  if (ONNX_NAMESPACE::TensorProto_DataType_BFLOAT16 != tensor.data_type()) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }

  if (raw_data != nullptr) {
    return UnpackTensorWithRawData(raw_data, raw_data_len, expected_size, p_data);
  }

  if (static_cast<size_t>(tensor.int32_data_size()) != expected_size)
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                  "UnpackTensor: the pre-allocate size does not match the size in proto");

  constexpr int max_value = std::numeric_limits<uint16_t>::max();
  for (int i = 0; i < static_cast<int>(expected_size); i++) {
    int v = tensor.int32_data()[i];
    if (v < 0 || v > max_value) {
      return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "data overflow");
    }
    p_data[i] = BFloat16(static_cast<uint16_t>(v), BFloat16::FromBits());
  }

  return Status::OK();
}

#if !defined(DISABLE_FLOAT8_TYPES)

// UnpackTensor<Float8E4M3FN>
template <>
Status UnpackTensor(const ONNX_NAMESPACE::TensorProto& tensor, const void* raw_data, size_t raw_data_len,
                    /*out*/ Float8E4M3FN* p_data, size_t expected_size) {
  if (nullptr == p_data) {
    const size_t size = raw_data != nullptr ? raw_data_len : tensor.int32_data_size();
    if (size == 0)
      return Status::OK();

    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }
  if (ONNX_NAMESPACE::TensorProto_DataType_FLOAT8E4M3FN != tensor.data_type()) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }

  if (raw_data != nullptr) {
    return UnpackTensorWithRawData(raw_data, raw_data_len, expected_size, p_data);
  }

  if (static_cast<size_t>(tensor.int32_data_size()) != expected_size)
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                  "UnpackTensor: the pre-allocate size does not match the size in proto");

  constexpr int max_value = std::numeric_limits<uint8_t>::max();
  for (int i = 0; i < static_cast<int>(expected_size); i++) {
    int v = tensor.int32_data()[i];
    if (v < 0 || v > max_value) {
      return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "data overflow");
    }
    p_data[i] = Float8E4M3FN(static_cast<uint8_t>(v), Float8E4M3FN::FromBits());
  }

  return Status::OK();
}

// UnpackTensor<Float8E4M3FNUZ>
template <>
Status UnpackTensor(const ONNX_NAMESPACE::TensorProto& tensor, const void* raw_data, size_t raw_data_len,
                    /*out*/ Float8E4M3FNUZ* p_data, size_t expected_size) {
  if (nullptr == p_data) {
    const size_t size = raw_data != nullptr ? raw_data_len : tensor.int32_data_size();
    if (size == 0)
      return Status::OK();

    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }
  if (ONNX_NAMESPACE::TensorProto_DataType_FLOAT8E4M3FNUZ != tensor.data_type()) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }

  if (raw_data != nullptr) {
    return UnpackTensorWithRawData(raw_data, raw_data_len, expected_size, p_data);
  }

  if (static_cast<size_t>(tensor.int32_data_size()) != expected_size)
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                  "UnpackTensor: the pre-allocate size does not match the size in proto");

  constexpr int max_value = std::numeric_limits<uint8_t>::max();
  for (int i = 0; i < static_cast<int>(expected_size); i++) {
    int v = tensor.int32_data()[i];
    if (v < 0 || v > max_value) {
      return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "data overflow");
    }
    p_data[i] = Float8E4M3FNUZ(static_cast<uint8_t>(v), Float8E4M3FNUZ::FromBits());
  }

  return Status::OK();
}

// UnpackTensor<Float8E5M2>
template <>
Status UnpackTensor(const ONNX_NAMESPACE::TensorProto& tensor, const void* raw_data, size_t raw_data_len,
                    /*out*/ Float8E5M2* p_data, size_t expected_size) {
  if (nullptr == p_data) {
    const size_t size = raw_data != nullptr ? raw_data_len : tensor.int32_data_size();
    if (size == 0)
      return Status::OK();

    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }
  if (ONNX_NAMESPACE::TensorProto_DataType_FLOAT8E5M2 != tensor.data_type()) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }

  if (raw_data != nullptr) {
    return UnpackTensorWithRawData(raw_data, raw_data_len, expected_size, p_data);
  }

  if (static_cast<size_t>(tensor.int32_data_size()) != expected_size)
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                  "UnpackTensor: the pre-allocate size does not match the size in proto");

  constexpr int max_value = std::numeric_limits<uint8_t>::max();
  for (int i = 0; i < static_cast<int>(expected_size); i++) {
    int v = tensor.int32_data()[i];
    if (v < 0 || v > max_value) {
      return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "data overflow");
    }
    p_data[i] = Float8E5M2(static_cast<uint8_t>(v), Float8E5M2::FromBits());
  }

  return Status::OK();
}

// UnpackTensor<Float8E5M2FNUZ>
template <>
Status UnpackTensor(const ONNX_NAMESPACE::TensorProto& tensor, const void* raw_data, size_t raw_data_len,
                    /*out*/ Float8E5M2FNUZ* p_data, size_t expected_size) {
  if (nullptr == p_data) {
    const size_t size = raw_data != nullptr ? raw_data_len : tensor.int32_data_size();
    if (size == 0)
      return Status::OK();

    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }
  if (ONNX_NAMESPACE::TensorProto_DataType_FLOAT8E5M2FNUZ != tensor.data_type()) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);
  }

  if (raw_data != nullptr) {
    return UnpackTensorWithRawData(raw_data, raw_data_len, expected_size, p_data);
  }

  if (static_cast<size_t>(tensor.int32_data_size()) != expected_size)
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                  "UnpackTensor: the pre-allocate size does not match the size in proto");

  constexpr int max_value = std::numeric_limits<uint8_t>::max();
  for (int i = 0; i < static_cast<int>(expected_size); i++) {
    int v = tensor.int32_data()[i];
    if (v < 0 || v > max_value) {
      return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "data overflow");
    }
    p_data[i] = Float8E5M2FNUZ(static_cast<uint8_t>(v), Float8E5M2FNUZ::FromBits());
  }

  return Status::OK();
}

#endif

#define DEFINE_INT4_UNPACK_TENSOR_IMPL(INT4_TYPE, ONNX_INT4_TYPE)                                           \
  template <>                                                                                               \
  Status UnpackTensor(const ONNX_NAMESPACE::TensorProto& tensor, const void* raw_data, size_t raw_data_len, \
                      /*out*/ INT4_TYPE* p_data, size_t expected_num_elems) {                               \
    if (nullptr == p_data) {                                                                                \
      const size_t size = raw_data != nullptr ? raw_data_len : tensor.int32_data_size();                    \
      return size == 0 ? Status::OK() : Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);              \
    }                                                                                                       \
    if (ONNX_NAMESPACE::ONNX_INT4_TYPE != tensor.data_type()) {                                             \
      return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT);                                         \
    }                                                                                                       \
                                                                                                            \
    size_t expected_int4_pairs = INT4_TYPE::CalcNumInt4Pairs(expected_num_elems);                           \
                                                                                                            \
    if (raw_data != nullptr) {                                                                              \
      return UnpackTensorWithRawData(raw_data, raw_data_len, expected_num_elems, p_data);                   \
    }                                                                                                       \
                                                                                                            \
    ORT_RETURN_IF_NOT(static_cast<size_t>(tensor.int32_data_size()) == expected_int4_pairs,                 \
                      "UnpackTensor: the pre-allocated size does not match the size in proto");             \
                                                                                                            \
    for (int i = 0; i < static_cast<int>(tensor.int32_data_size()); i++) {                                  \
      p_data[i] = INT4_TYPE(static_cast<std::byte>(tensor.int32_data()[i]));                                \
    }                                                                                                       \
                                                                                                            \
    return Status::OK();                                                                                    \
  }

// UnpackTensor<Int4x2>
DEFINE_INT4_UNPACK_TENSOR_IMPL(Int4x2, TensorProto_DataType_INT4)

// UnpackTensor<UInt4x2>
DEFINE_INT4_UNPACK_TENSOR_IMPL(UInt4x2, TensorProto_DataType_UINT4)

// UnpackTensor from raw data, external data or the type specific data field.
// Uses the model path to construct the full path for loading external data. In case when model_path is empty
// it uses current directory.
template <typename T>
Status UnpackTensor(const ONNX_NAMESPACE::TensorProto& tensor, const std::filesystem::path& model_path,
                    /*out*/ T* p_data, size_t expected_num_elements) {
#if !defined(ORT_MINIMAL_BUILD)
  if (HasExternalData(tensor)) {
    return UnpackTensorWithExternalData(tensor, model_path.parent_path(),
                                        expected_num_elements, p_data);
  }
#else
  ORT_UNUSED_PARAMETER(model_path);
  ORT_RETURN_IF(HasExternalData(tensor), "TensorProto with external data is not supported in ORT minimal build.");
#endif

  return HasRawData(tensor)
             ? UnpackTensor(tensor, tensor.raw_data().data(), tensor.raw_data().size(), p_data, expected_num_elements)
             : UnpackTensor(tensor, nullptr, 0, p_data, expected_num_elements);
}

// instantiate the UnpackTensor variant that supports external data
#define INSTANTIATE_UNPACK_TENSOR(type) \
  template Status UnpackTensor(const ONNX_NAMESPACE::TensorProto&, const std::filesystem::path&, type* p_data, size_t);

INSTANTIATE_UNPACK_TENSOR(float)
INSTANTIATE_UNPACK_TENSOR(double)
INSTANTIATE_UNPACK_TENSOR(uint8_t)
INSTANTIATE_UNPACK_TENSOR(int8_t)
INSTANTIATE_UNPACK_TENSOR(int16_t)
INSTANTIATE_UNPACK_TENSOR(uint16_t)
INSTANTIATE_UNPACK_TENSOR(int32_t)
INSTANTIATE_UNPACK_TENSOR(int64_t)
INSTANTIATE_UNPACK_TENSOR(uint64_t)
INSTANTIATE_UNPACK_TENSOR(uint32_t)
INSTANTIATE_UNPACK_TENSOR(bool)
INSTANTIATE_UNPACK_TENSOR(MLFloat16)
INSTANTIATE_UNPACK_TENSOR(BFloat16)
INSTANTIATE_UNPACK_TENSOR(std::string)

#if !defined(DISABLE_FLOAT8_TYPES)
INSTANTIATE_UNPACK_TENSOR(Float8E4M3FN)
INSTANTIATE_UNPACK_TENSOR(Float8E4M3FNUZ)
INSTANTIATE_UNPACK_TENSOR(Float8E5M2)
INSTANTIATE_UNPACK_TENSOR(Float8E5M2FNUZ)
#endif
INSTANTIATE_UNPACK_TENSOR(Int4x2)
INSTANTIATE_UNPACK_TENSOR(UInt4x2)

#define CASE_PROTO_TRACE(X, Y)                                                                     \
  case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_##X:                             \
    if (!IAllocator::CalcMemSizeForArrayWithAlignment<alignment>(size, sizeof(Y), out)) {          \
      return common::Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "Invalid TensorProto"); \
    }                                                                                              \
    break;

#define CASE_PROTO_TRACE_INT4(X, Y)                                                                            \
  case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_##X:                                         \
    if (!IAllocator::CalcMemSizeForArrayWithAlignment<alignment>(Y::CalcNumInt4Pairs(size), sizeof(Y), out)) { \
      return common::Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "Invalid TensorProto");             \
    }                                                                                                          \
    break;

template <size_t alignment>
common::Status GetSizeInBytesFromTensorShapeAndType(const TensorShape& shape, int32_t element_type, size_t* out) {
  const auto size = narrow<size_t>(shape.Size());
  switch (element_type) {
    CASE_PROTO_TRACE(FLOAT, float);
    CASE_PROTO_TRACE(DOUBLE, double);
    CASE_PROTO_TRACE(BOOL, bool);
    CASE_PROTO_TRACE(INT8, int8_t);
    CASE_PROTO_TRACE(INT16, int16_t);
    CASE_PROTO_TRACE(INT32, int32_t);
    CASE_PROTO_TRACE(INT64, int64_t);
    CASE_PROTO_TRACE(UINT8, uint8_t);
    CASE_PROTO_TRACE(UINT16, uint16_t);
    CASE_PROTO_TRACE(UINT32, uint32_t);
    CASE_PROTO_TRACE(UINT64, uint64_t);
    CASE_PROTO_TRACE(FLOAT16, MLFloat16);
    CASE_PROTO_TRACE(BFLOAT16, BFloat16);
    CASE_PROTO_TRACE(STRING, std::string);
#if !defined(DISABLE_FLOAT8_TYPES)
    CASE_PROTO_TRACE(FLOAT8E4M3FN, Float8E4M3FN);
    CASE_PROTO_TRACE(FLOAT8E4M3FNUZ, Float8E4M3FNUZ);
    CASE_PROTO_TRACE(FLOAT8E5M2, Float8E5M2);
    CASE_PROTO_TRACE(FLOAT8E5M2FNUZ, Float8E5M2FNUZ);
#endif
    CASE_PROTO_TRACE_INT4(UINT4, UInt4x2);
    CASE_PROTO_TRACE_INT4(INT4, Int4x2);
    default:
      return common::Status(common::ONNXRUNTIME, common::NOT_IMPLEMENTED);
  }
  return Status::OK();
}

template <size_t alignment>
common::Status GetSizeInBytesFromTensorProto(const ONNX_NAMESPACE::TensorProto& tensor_proto, size_t* out) {
  TensorShape tensor_shape = GetTensorShapeFromTensorProto(tensor_proto);

  bool any_out_of_bounds = std::any_of(tensor_shape.GetDims().begin(), tensor_shape.GetDims().end(),
                                       [](int64_t dim) {
                                         if (dim < 0 ||
                                             static_cast<uint64_t>(dim) >= std::numeric_limits<size_t>::max()) {
                                           return true;
                                         }
                                         return false;
                                       });

  ORT_RETURN_IF(any_out_of_bounds, "Out of bounds dimensions in TypeProto_Tensor");

  return GetSizeInBytesFromTensorShapeAndType<alignment>(tensor_shape, tensor_proto.data_type(), out);
}

template <size_t alignment>
common::Status GetSizeInBytesFromTensorTypeProto(const ONNX_NAMESPACE::TypeProto_Tensor& tensor_proto, size_t* out) {
  ORT_RETURN_IF_NOT(HasShape(tensor_proto), "TypeProto_Tensor does not have shape");
  ORT_RETURN_IF_NOT(HasElemType(tensor_proto), "TypeProto_Tensor does not have element type");

  TensorShape tensor_shape = GetTensorShapeFromTensorShapeProto(tensor_proto.shape());

  bool any_out_of_bounds = std::any_of(tensor_shape.GetDims().begin(), tensor_shape.GetDims().end(),
                                       [](int64_t dim) {
                                         return dim < 0 ||
                                                static_cast<uint64_t>(dim) >= std::numeric_limits<size_t>::max();
                                       });
  ORT_RETURN_IF(any_out_of_bounds, "Out of bounds dimensions in TypeProto_Tensor");

  return GetSizeInBytesFromTensorShapeAndType<alignment>(tensor_shape, tensor_proto.elem_type(), out);
}

template Status GetSizeInBytesFromTensorTypeProto<0>(const ONNX_NAMESPACE::TypeProto_Tensor& tensor_proto, size_t* out);

TensorShape GetTensorShapeFromTensorShapeProto(const ONNX_NAMESPACE::TensorShapeProto& tensor_shape_proto) {
  const auto& dims = tensor_shape_proto.dim();
  TensorShapeVector tensor_shape_vec(static_cast<size_t>(dims.size()));
  for (int i = 0; i < dims.size(); ++i) {
    tensor_shape_vec[i] =
        HasDimValue(dims[i]) ? dims[i].dim_value() : -1; /* symbolic dimensions are represented as -1 in onnxruntime*/
  }
  return TensorShape(tensor_shape_vec);
}

TensorShape GetTensorShapeFromTensorProto(const ONNX_NAMESPACE::TensorProto& tensor_proto) {
  const auto& dims = tensor_proto.dims();
  TensorShapeVector tensor_shape_vec(static_cast<size_t>(dims.size()));
  for (int i = 0; i < dims.size(); ++i) {
    tensor_shape_vec[i] = dims[i];
  }

  return TensorShape(tensor_shape_vec);
}

struct UnInitializeParam {
  void* preallocated;
  size_t preallocated_size;
  ONNXTensorElementDataType ele_type;
};

ORT_API_STATUS_IMPL(OrtInitializeBufferForTensor, _In_opt_ void* input, size_t input_len,
                    enum ONNXTensorElementDataType type) {
  OrtStatus* status = nullptr;
  ORT_TRY {
    if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING || input == nullptr)
      return nullptr;
    size_t tensor_size = input_len / sizeof(std::string);
    std::string* ptr = reinterpret_cast<std::string*>(input);
    for (size_t i = 0, n = tensor_size; i < n; ++i) {
      new (ptr + i) std::string();
    }
  }
  ORT_CATCH(const std::exception& ex) {
    ORT_HANDLE_EXCEPTION([&]() { status = OrtApis::CreateStatus(ORT_RUNTIME_EXCEPTION, ex.what()); });
  }

  return status;
}

ORT_API(void, OrtUninitializeBuffer, _In_opt_ void* input, size_t input_len, enum ONNXTensorElementDataType type) {
  if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING || input == nullptr)
    return;
  size_t tensor_size = input_len / sizeof(std::string);
  std::string* ptr = reinterpret_cast<std::string*>(input);
  using std::string;
  for (size_t i = 0, n = tensor_size; i < n; ++i) {
    ptr[i].~string();
  }
}
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(disable : 26409)
#endif

#if !defined(__wasm__)
static Status GetFileContent(const Env& env, const std::filesystem::path& file_path, FileOffsetType offset,
                             size_t length, IAllocatorUniquePtr<void>& external_data) {
  // query length if it is 0
  if (length == 0) {
    // The return type of std::filesystem::file_size is uintmax_t which could be bigger than size_t
    length = narrow<size_t>(std::filesystem::file_size(file_path));
  }

  // first, try to map into memory
  {
    Env::MappedMemoryPtr mapped_memory{};
    auto status = env.MapFileIntoMemory(file_path.native().c_str(), offset, length, mapped_memory);
    if (status.IsOK()) {
      IAllocatorUniquePtr<void> raw_buffer(mapped_memory.release(),
                                           mapped_memory.get_deleter());
      external_data.swap(raw_buffer);
      return Status::OK();
    }
  }

  // if that fails, try to copy
  auto buffer = std::make_unique<char[]>(length);
  ORT_RETURN_IF_ERROR(
      env.ReadFileIntoBuffer(file_path.native().c_str(), offset, length, gsl::make_span(buffer.get(), length)));

  IAllocatorUniquePtr<void> raw_buffer(buffer.release(), [](void* p) { delete[] reinterpret_cast<char*>(p); });
  external_data.swap(raw_buffer);
  return Status::OK();
}
#endif

Status GetExtDataFromTensorProto(const Env& env,
                                 const std::filesystem::path& model_path,
                                 const ONNX_NAMESPACE::TensorProto& tensor_proto,
                                 OrtValue& ort_value, PrepackedWeightsForGraph* prepacked_info) {
  ORT_ENFORCE(HasExternalData(tensor_proto), "TensorProto for: ",
              tensor_proto.name(), "Expected to have external data");

  std::basic_string<ORTCHAR_T> tensor_proto_dir;
  if (!model_path.empty()) {
    ORT_RETURN_IF_ERROR(GetDirNameFromFilePath(model_path, tensor_proto_dir));
  }

  std::basic_string<ORTCHAR_T> external_data_file_path;
  FileOffsetType file_offset;
  SafeInt<size_t> raw_data_safe_len = 0;
  std::optional<ExternalDataInfo::PrepackedInfos> prepacked_infos;
  if (prepacked_info != nullptr) {
    prepacked_infos.emplace();
  }

  ORT_RETURN_IF_ERROR(
      GetExternalDataInfo(tensor_proto, tensor_proto_dir, external_data_file_path, file_offset,
                          raw_data_safe_len, (prepacked_info != nullptr) ? &*prepacked_infos : nullptr));

  TensorShape tensor_shape = utils::GetTensorShapeFromTensorProto(tensor_proto);
  const DataTypeImpl* const type = DataTypeImpl::TensorTypeFromONNXEnum(tensor_proto.data_type())->GetElementType();
  MLDataType ml_tensor_type = DataTypeImpl::GetType<Tensor>();
  const auto& name = tensor_proto.name();

  if (external_data_file_path == onnxruntime::utils::kTensorProtoMemoryAddressTag) {
    // the value in location is the memory address of the data
    void* ext_data_buf = reinterpret_cast<void*>(file_offset);
    auto tensor = Tensor{type, tensor_shape, ext_data_buf, OrtMemoryInfo(CPU, OrtAllocatorType::OrtDeviceAllocator)};
    ORT_RETURN_IF(raw_data_safe_len != tensor.SizeInBytes(), "Weight: ", name,
                  " kTensorProtoMemoryAddressTag address points to length: ", static_cast<size_t>(raw_data_safe_len),
                  " while shape has bytes size: ", tensor.SizeInBytes());
    Tensor::InitOrtValue(std::move(tensor), ort_value);
  } else {
#if defined(__wasm__)
    ORT_RETURN_IF(file_offset < 0 || file_offset + raw_data_safe_len >= 4294967296,
                  "External initializer: ", tensor_proto.name(), " offset: ", file_offset,
                  " size to read: ", static_cast<size_t>(raw_data_safe_len),
                  " are out of bounds or can not be read in full (>4GB).");

    auto buffer = std::make_unique<char[]>(raw_data_safe_len);
    ORT_RETURN_IF_ERROR(LoadWebAssemblyExternalData(env,
                                                    external_data_file_path,
                                                    file_offset,
                                                    raw_data_safe_len,
                                                    ExternalDataLoadType::CPU,
                                                    buffer.get()));

    auto p_tensor = std::make_unique<Tensor>(type, tensor_shape, buffer.get(),
                                             OrtMemoryInfo(CPU, OrtAllocatorType::OrtDeviceAllocator));

    std::function<void(void*)> deleter = [ext_data = buffer.get()](void* t) {
      delete reinterpret_cast<Tensor*>(t);
      delete[] ext_data;
    };

    ort_value.Init(p_tensor.release(), ml_tensor_type, std::move(deleter));
    buffer.release();

#else
    //  The GetFileContent function doesn't report error if the requested data range is invalid. Therefore we need to
    //  manually check file size first.
    std::uintmax_t file_length = std::filesystem::file_size(external_data_file_path);

    SafeInt<FileOffsetType> end_of_read(file_offset);
    end_of_read += raw_data_safe_len;
    ORT_RETURN_IF(file_offset < 0 || static_cast<std::uintmax_t>(end_of_read) > file_length,
                  "External initializer: ", tensor_proto.name(), " offset: ", file_offset,
                  " size to read: ", static_cast<size_t>(raw_data_safe_len), " given file_length: ", file_length,
                  " are out of bounds or can not be read in full.");

    IAllocatorUniquePtr<void> ext_data_buf;
    ORT_RETURN_IF_ERROR(GetFileContent(env, external_data_file_path, file_offset, raw_data_safe_len,
                                       ext_data_buf));

    // Data on disk is little endian
    if constexpr (endian::native != endian::little) {
      if (type->Size() > 1) {
        gsl::span<std::byte> data_span{reinterpret_cast<std::byte*>(ext_data_buf.get()), raw_data_safe_len};
        SwapByteOrderInplace(type->Size(), data_span);
      }
    }

    auto p_tensor = std::make_unique<Tensor>(type, tensor_shape, ext_data_buf.get(),
                                             OrtMemoryInfo(CPU, OrtAllocatorType::OrtDeviceAllocator));
    ORT_RETURN_IF(raw_data_safe_len != p_tensor->SizeInBytes(), "Weight: ", name,
                  " External file content has length: ", static_cast<size_t>(raw_data_safe_len),
                  " while shape has bytes size: ", p_tensor->SizeInBytes());

    // Will destroy ext_data as a member of the functor
    // can not move the unique_ptr as it is not copyable
    std::function<void(void*)> deleter = [ext_data = ext_data_buf.get(),
                                          d = ext_data_buf.get_deleter()](void* t) {
      delete reinterpret_cast<Tensor*>(t);
      d(ext_data);
    };

    ort_value.Init(p_tensor.release(), ml_tensor_type, std::move(deleter));
    ext_data_buf.release();

    if (prepacked_info != nullptr && !prepacked_infos->empty()) {
      for (const auto& [key, blobs] : *prepacked_infos) {
        PrePackedWeights prepacked_weights;
        prepacked_weights.buffers_.reserve(blobs.size());
        prepacked_weights.buffer_sizes_.reserve(blobs.size());
        for (const auto& blob : blobs) {
          const auto blob_offset = std::get<0>(blob);
          const auto blob_length = std::get<1>(blob);
          SafeInt<FileOffsetType> end_of_blob{blob_offset};
          end_of_blob += blob_length;
          ORT_RETURN_IF(blob_offset < 0 || static_cast<uintmax_t>(end_of_blob) > file_length,
                        "Pre-packed blob: ", key, " offset: ", blob_offset, " file_length: ", file_length,
                        " is out of bounds and can not read in full");

          IAllocatorUniquePtr<void> data_ptr;
          ORT_RETURN_IF_ERROR(GetFileContent(env, external_data_file_path, blob_offset, blob_length,
                                             data_ptr));
          prepacked_weights.buffers_.push_back(std::move(data_ptr));
          prepacked_weights.buffer_sizes_.push_back(blob_length);
        }
        if (!blobs.empty()) {
          prepacked_info->InsertPrepackedWeights(key, std::move(prepacked_weights));
        }
      }
    }
#endif
  }

  return Status::OK();
}

Status LoadExtDataToTensorFromTensorProto(const Env& env, const std::filesystem::path& model_path,
                                          const ONNX_NAMESPACE::TensorProto& tensor_proto,
                                          const IExternalDataLoader& ext_data_loader,
                                          Tensor& tensor) {
  ORT_ENFORCE(HasExternalData(tensor_proto));
  std::basic_string<ORTCHAR_T> tensor_proto_dir;
  if (!model_path.empty()) {
    ORT_RETURN_IF_ERROR(GetDirNameFromFilePath(model_path, tensor_proto_dir));
  }
  std::basic_string<ORTCHAR_T> external_data_file_path;
  FileOffsetType file_offset;
  SafeInt<size_t> raw_data_safe_len = 0;
  ORT_RETURN_IF_ERROR(
      GetExternalDataInfo(tensor_proto, tensor_proto_dir, external_data_file_path, file_offset, raw_data_safe_len));

  ORT_RETURN_IF(file_offset < 0 || raw_data_safe_len != tensor.SizeInBytes(),
                "External initializer: ", tensor_proto.name(), " offset: ", file_offset,
                " size to read: ", static_cast<size_t>(raw_data_safe_len),
                " does not match the tensor size: ", tensor.SizeInBytes());
  ORT_RETURN_IF(external_data_file_path == onnxruntime::utils::kTensorProtoMemoryAddressTag,
                "Memory address tag is not supported by custom external data loader.");

  return ext_data_loader.LoadTensor(env, external_data_file_path, file_offset, raw_data_safe_len, tensor);
}

#define CASE_PROTO(X, Y)                                                                                            \
  case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_##X:                                              \
    ORT_RETURN_IF_ERROR(                                                                                            \
        UnpackTensor<Y>(tensor_proto, raw_data, raw_data_len, (Y*)preallocated, static_cast<size_t>(tensor_size))); \
    break;

/**
 * @brief Convert tensor_proto to tensor format and store it to pre-allocated tensor
 * @param env
 * @param model_path
 * @param tensor_proto  tensor data in protobuf format
 * @param tensor        pre-allocated tensor object, where we store the data
 * @return
 */
Status TensorProtoToTensor(const Env& env, const std::filesystem::path& model_path,
                           const ONNX_NAMESPACE::TensorProto& tensor_proto, Tensor& tensor) {
  // Validate tensor compatibility
  TensorShape tensor_shape = GetTensorShapeFromTensorProto(tensor_proto);

  if (std::any_of(tensor_shape.GetDims().begin(), tensor_shape.GetDims().end(),
                  [](int64_t dim) {
                    return dim < 0;
                  })) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "tensor can't contain negative dims");
  }

  if (HasExternalData(tensor_proto)) {
    OrtValue ort_value;
    ORT_RETURN_IF_ERROR(GetExtDataFromTensorProto(env, model_path, tensor_proto, ort_value));
    const auto& ext_tensor = ort_value.Get<Tensor>();
    MakeCpuTensorCopy(ext_tensor, tensor);
    return Status::OK();
  }

  if (tensor_shape != tensor.Shape()) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "TensorProtoToTensor() tensor shape mismatch!");
  }

  const DataTypeImpl* const source_type =
      DataTypeImpl::TensorTypeFromONNXEnum(tensor_proto.data_type())->GetElementType();
  if (source_type->Size() > tensor.DataType()->Size()) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "TensorProto type ", DataTypeImpl::ToString(source_type),
                           " can not be written into Tensor type ", DataTypeImpl::ToString(tensor.DataType()));
  }

  //  Below we handle the case where TensorProto contains data in itself

  // find raw data in proto buf
  void* raw_data = nullptr;
  SafeInt<size_t> raw_data_len = 0;
  if (utils::HasRawData(tensor_proto)) {
    raw_data = const_cast<char*>(tensor_proto.raw_data().data());
    // TODO The line above has const-correctness issues. Below is a possible fix which copies the tensor_proto data
    //      into a writeable buffer. However, it requires extra memory which may exceed the limit for certain tests.
    // auto buffer = std::make_unique<char[]>(tensor_proto.raw_data().size());
    // std::memcpy(buffer.get(), tensor_proto.raw_data().data(), tensor_proto.raw_data().size());
    // deleter_for_file_data.d = OrtCallback{DeleteCharArray, buffer.get()};
    // raw_data = buffer.release();
    raw_data_len = tensor_proto.raw_data().size();
  }

  if (nullptr != raw_data && utils::HasString(tensor_proto)) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "string tensor can not have raw data");
  }

  // unpacking tensor_proto data to preallocated tensor
  void* preallocated = tensor.MutableDataRaw();
  const int64_t tensor_size = tensor_shape.Size();

  // tensor_size could be zero. see test_slice_start_out_of_bounds\test_data_set_0\output_0.pb
  if (narrow<uint64_t>(tensor_size) > SIZE_MAX) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "size overflow");
  }

  switch (tensor_proto.data_type()) {
    CASE_PROTO(FLOAT, float);
    CASE_PROTO(DOUBLE, double);
    CASE_PROTO(BOOL, bool);
    CASE_PROTO(INT8, int8_t);
    CASE_PROTO(INT16, int16_t);
    CASE_PROTO(INT32, int32_t);
    CASE_PROTO(INT64, int64_t);
    CASE_PROTO(UINT8, uint8_t);
    CASE_PROTO(UINT16, uint16_t);
    CASE_PROTO(UINT32, uint32_t);
    CASE_PROTO(UINT64, uint64_t);
    CASE_PROTO(FLOAT16, MLFloat16);
    CASE_PROTO(BFLOAT16, BFloat16);
#if !defined(DISABLE_FLOAT8_TYPES)
    CASE_PROTO(FLOAT8E4M3FN, Float8E4M3FN);
    CASE_PROTO(FLOAT8E4M3FNUZ, Float8E4M3FNUZ);
    CASE_PROTO(FLOAT8E5M2, Float8E5M2);
    CASE_PROTO(FLOAT8E5M2FNUZ, Float8E5M2FNUZ);
#endif
    CASE_PROTO(INT4, Int4x2);
    CASE_PROTO(UINT4, UInt4x2);
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_STRING:
      ORT_RETURN_IF_ERROR(UnpackTensor<std::string>(tensor_proto, raw_data, raw_data_len,
                                                    static_cast<std::string*>(preallocated),
                                                    static_cast<size_t>(tensor_size)));
      break;
    default: {
      std::ostringstream ostr;
      ostr << "Initialized tensor with unexpected type: " << tensor_proto.data_type();
      return common::Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, ostr.str());
    }
  }

  return Status::OK();
}

common::Status CreateTensorFromTensorProto(const Env& env, const std::filesystem::path& model_path,
                                           const ONNX_NAMESPACE::TensorProto& tensor_proto, Tensor& tensor) {
  ORT_RETURN_IF_NOT(utils::HasDataType(tensor_proto), "Initializer must have a datatype");
  auto proto_data_type = tensor_proto.data_type();

  auto proto_shape = utils::GetTensorShapeFromTensorProto(tensor_proto);
  Tensor w(DataTypeImpl::TensorTypeFromONNXEnum(proto_data_type)->GetElementType(), proto_shape,
           CPUAllocator::DefaultInstance());
  ORT_RETURN_IF_ERROR(utils::TensorProtoToTensor(env, model_path, tensor_proto, w));

  tensor = std::move(w);
  return Status::OK();
}

Status GetTensorProtoWithDataIfInMemory(
    const ONNX_NAMESPACE::TensorProto& tensor_proto, std::unique_ptr<ONNX_NAMESPACE::TensorProto>& result) {
  if (HasExternalDataInMemory(tensor_proto)) {
    result = std::make_unique<ONNX_NAMESPACE::TensorProto>();
    return TensorProtoWithExternalDataToTensorProto(tensor_proto, {}, *result);
  }

  result.reset();
  return Status::OK();
}

Status TensorProtoToOrtValue(const Env& env, const std::filesystem::path& model_path,
                             const ONNX_NAMESPACE::TensorProto& tensor_proto, const MemBuffer& m, OrtValue& value) {
  return TensorProtoToOrtValueImpl(env, model_path, tensor_proto, &m, nullptr, value);
}

Status TensorProtoToOrtValue(const Env& env, const std::filesystem::path& model_path,
                             const ONNX_NAMESPACE::TensorProto& tensor_proto, AllocatorPtr alloc, OrtValue& value) {
  return TensorProtoToOrtValueImpl(env, model_path, tensor_proto, nullptr, alloc, value);
}

#define CASE_TYPE(X)                             \
  case ONNX_NAMESPACE::TensorProto_DataType_##X: \
    return ONNX_TENSOR_ELEMENT_DATA_TYPE_##X;

ONNXTensorElementDataType CApiElementTypeFromProtoType(int type) {
  switch (type) {
    CASE_TYPE(FLOAT)
    CASE_TYPE(UINT8)
    CASE_TYPE(INT8)
    CASE_TYPE(UINT16)
    CASE_TYPE(INT16)
    CASE_TYPE(INT32)
    CASE_TYPE(INT64)
    CASE_TYPE(STRING)
    CASE_TYPE(BOOL)
    CASE_TYPE(FLOAT16)
    CASE_TYPE(DOUBLE)
    CASE_TYPE(UINT32)
    CASE_TYPE(UINT64)
    CASE_TYPE(COMPLEX64)
    CASE_TYPE(COMPLEX128)
    CASE_TYPE(BFLOAT16)
#if !defined(DISABLE_FLOAT8_TYPES)
    CASE_TYPE(FLOAT8E4M3FN)
    CASE_TYPE(FLOAT8E4M3FNUZ)
    CASE_TYPE(FLOAT8E5M2)
    CASE_TYPE(FLOAT8E5M2FNUZ)
#endif
    CASE_TYPE(UINT4)
    CASE_TYPE(INT4)
    default:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  }
}

ONNXTensorElementDataType GetTensorElementType(const ONNX_NAMESPACE::TensorProto& tensor_proto) {
  return CApiElementTypeFromProtoType(tensor_proto.data_type());
}

ONNX_NAMESPACE::TensorProto TensorToTensorProto(const Tensor& tensor,
                                                const std::string& tensor_proto_name,
                                                bool use_tensor_buffer) {
  // Set name, dimensions, type, and data of the TensorProto.
  ONNX_NAMESPACE::TensorProto tensor_proto;

  tensor_proto.set_name(tensor_proto_name);

  for (auto& dim : tensor.Shape().GetDims()) {
    tensor_proto.add_dims(dim);
  }

  tensor_proto.set_data_type(tensor.GetElementType());
  if (use_tensor_buffer && tensor.SizeInBytes() > kSmallTensorExternalDataThreshold) {
    // https://github.com/microsoft/onnxruntime/blob/main/onnxruntime/core/graph/graph_flatbuffers_utils.cc#L302
    const auto* raw_data = tensor.DataRaw();
    ORT_ENFORCE(raw_data, "Missing raw data for tensor proto. Invalid tensor.");
    static_assert(sizeof(void*) <= sizeof(ExternalDataInfo::OFFSET_TYPE));

    // we reinterpret_cast this back to void* in tensorprotoutils.cc:GetExtDataFromTensorProto.
    // use intptr_t as OFFSET_TYPE is signed. in theory you could get a weird looking value if the address uses the
    // high bit, but that should be unlikely in a scenario where we care about memory usage enough to use this path.
    auto offset = narrow<ExternalDataInfo::OFFSET_TYPE>(reinterpret_cast<intptr_t>(raw_data));

    ExternalDataInfo::SetExternalLocationToProto(onnxruntime::utils::kTensorProtoMemoryAddressTag,
                                                 offset, tensor.SizeInBytes(), tensor_proto);

  } else {
    if (tensor.IsDataTypeString()) {
      auto* mutable_string_data = tensor_proto.mutable_string_data();
      auto f = tensor.Data<std::string>();
      auto end = f + tensor.Shape().Size();
      for (; f < end; ++f) {
        *mutable_string_data->Add() = *f;
      }
    } else {
      SetRawDataInTensorProto(tensor_proto, tensor.DataRaw(), tensor.SizeInBytes());
    }
  }

  return tensor_proto;
}

ONNX_NAMESPACE::TypeProto TypeProtoFromTensorProto(const ONNX_NAMESPACE::TensorProto& tensor_proto) {
  TypeProto type_proto;

  type_proto.mutable_tensor_type()->set_elem_type(tensor_proto.data_type());
  auto shape = type_proto.mutable_tensor_type()->mutable_shape();

  for (auto dim : tensor_proto.dims()) {
    shape->add_dim()->set_dim_value(dim);
  }

  return type_proto;
}

common::Status ConstantNodeProtoToTensorProto(const ONNX_NAMESPACE::NodeProto& node,
                                              const std::filesystem::path& model_path,
                                              ONNX_NAMESPACE::TensorProto& tensor, const std::string& tensor_name) {
  ORT_RETURN_IF_NOT(node.attribute_size() > 0, "Constant node: ", node.name(), " has no data attributes");

  const AttributeProto& constant_attribute = node.attribute(0);

  switch (constant_attribute.type()) {
    case AttributeProto_AttributeType_TENSOR:
      tensor = constant_attribute.t();
      break;
    case AttributeProto_AttributeType_FLOAT:
      tensor.set_data_type(TensorProto_DataType_FLOAT);
      tensor.add_float_data(constant_attribute.f());
      break;
    case AttributeProto_AttributeType_FLOATS:
      tensor.set_data_type(TensorProto_DataType_FLOAT);
      *tensor.mutable_float_data() = constant_attribute.floats();
      tensor.add_dims(constant_attribute.floats().size());
      break;
    case AttributeProto_AttributeType_INT:
      tensor.set_data_type(TensorProto_DataType_INT64);
      tensor.add_int64_data(constant_attribute.i());
      break;
    case AttributeProto_AttributeType_INTS:
      tensor.set_data_type(TensorProto_DataType_INT64);
      *tensor.mutable_int64_data() = constant_attribute.ints();
      tensor.add_dims(constant_attribute.ints().size());
      break;
    case AttributeProto_AttributeType_STRING:
      tensor.set_data_type(TensorProto_DataType_STRING);
      tensor.add_string_data(constant_attribute.s());
      break;
    case AttributeProto_AttributeType_STRINGS: {
      tensor.set_data_type(TensorProto_DataType_STRING);
      *tensor.mutable_string_data() = constant_attribute.strings();
      tensor.add_dims(constant_attribute.strings().size());
      break;
    }
#if !defined(DISABLE_SPARSE_TENSORS)
    case AttributeProto_AttributeType_SPARSE_TENSOR: {
      auto& s = constant_attribute.sparse_tensor();
      ORT_RETURN_IF_ERROR(SparseTensorProtoToDenseTensorProto(s, model_path, tensor));
      break;
    }
#else
      ORT_UNUSED_PARAMETER(model_path);
#endif
    default:
      ORT_THROW("Unsupported attribute value type of ", constant_attribute.type(), " in 'Constant' node '", node.name(),
                "'");
  }

  // set name last in case attribute type was tensor (would copy over name)
  *(tensor.mutable_name()) = tensor_name;

  return Status::OK();
}

common::Status ConstantNodeProtoToTensorProto(const ONNX_NAMESPACE::NodeProto& node,
                                              const std::filesystem::path& model_path,
                                              ONNX_NAMESPACE::TensorProto& tensor) {
  ORT_ENFORCE(node.output_size() == 1, "NodeProto for Constant should have 1 output. Got:", node.output_size());
  return ConstantNodeProtoToTensorProto(node, model_path, tensor, node.output(0));
}

void MakeCpuTensorCopy(const Tensor& src_tensor, Tensor& dst_tensor) {
  if (src_tensor.IsDataTypeString()) {
    auto src_span = src_tensor.DataAsSpan<std::string>();
    std::copy(src_span.begin(), src_span.end(), dst_tensor.MutableDataAsSpan<std::string>().begin());
  } else {
    std::memcpy(dst_tensor.MutableDataRaw(), src_tensor.DataRaw(), src_tensor.SizeInBytes());
  }
}

#if !defined(DISABLE_SPARSE_TENSORS)
static Status CopySparseData(size_t n_sparse_elements,
                             const ONNX_NAMESPACE::TensorProto& indices,
                             const std::filesystem::path& model_path,
                             gsl::span<const int64_t>
                                 dims,
                             std::function<void(size_t from_idx, size_t to_idx)>
                                 copier) {
  Status status = Status::OK();
  TensorShape indices_shape(indices.dims().data(), indices.dims().size());
  const auto elements = narrow<size_t>(indices_shape.Size());

  std::vector<int64_t> indices_values;  // used for conversion of smaller size indices
  std::vector<uint8_t> unpack_buffer;
  gsl::span<const int64_t> indices_data;
  const bool has_raw_data = indices.has_raw_data();
  switch (indices.data_type()) {
    case ONNX_NAMESPACE::TensorProto_DataType_INT64:
      if (has_raw_data) {
        ORT_RETURN_IF_NOT(indices.raw_data().size() == (elements * sizeof(int64_t)),
                          "Sparse Indices raw data size does not match expected.");
        ORT_RETURN_IF_ERROR(UnpackInitializerData(indices, model_path, unpack_buffer));
        indices_data = ReinterpretAsSpan<const int64_t>(gsl::make_span(unpack_buffer));
      } else {
        ORT_RETURN_IF_NOT(indices.int64_data_size() == static_cast<int64_t>(elements),
                          "Sparse indices int64 data size does not match expected");
        indices_data = gsl::make_span(indices.int64_data().data(), elements);
      }
      break;
    case ONNX_NAMESPACE::TensorProto_DataType_INT32: {
      if (has_raw_data) {
        ORT_RETURN_IF_NOT(indices.raw_data().size() == (elements * sizeof(int32_t)),
                          "Sparse Indices raw data size does not match expected.");
        ORT_RETURN_IF_ERROR(UnpackInitializerData(indices, model_path, unpack_buffer));
        auto int32_span = ReinterpretAsSpan<const int32_t>(gsl::make_span(unpack_buffer));
        indices_values.insert(indices_values.cend(), int32_span.begin(), int32_span.end());
        unpack_buffer.clear();
        unpack_buffer.shrink_to_fit();
      } else {
        ORT_RETURN_IF_NOT(indices.int32_data_size() == static_cast<int64_t>(elements),
                          "Sparse indices int32 data size does not match expected");
        indices_values.insert(indices_values.cend(), indices.int32_data().cbegin(), indices.int32_data().cend());
      }
      indices_data = gsl::make_span(indices_values);
      break;
    }
    case ONNX_NAMESPACE::TensorProto_DataType_INT16: {
      if (has_raw_data) {
        ORT_RETURN_IF_NOT(indices.raw_data().size() == (elements * sizeof(int16_t)),
                          "Sparse Indices raw data size does not match expected.");
        ORT_RETURN_IF_ERROR(UnpackInitializerData(indices, model_path, unpack_buffer));
        auto int16_span = ReinterpretAsSpan<const int16_t>(gsl::make_span(unpack_buffer));
        indices_values.insert(indices_values.cend(), int16_span.begin(), int16_span.end());
        indices_data = gsl::make_span(indices_values);
        unpack_buffer.clear();
        unpack_buffer.shrink_to_fit();
      } else {
        return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_GRAPH,
                               "Invalid SparseTensor indices. INT16 indices must be in the raw data of indices tensor");
      }
      break;
    }
    case ONNX_NAMESPACE::TensorProto_DataType_INT8: {
      if (has_raw_data) {
        ORT_RETURN_IF_NOT(indices.raw_data().size() == elements,
                          "Sparse Indices raw data size does not match expected.");
        ORT_RETURN_IF_ERROR(UnpackInitializerData(indices, model_path, unpack_buffer));
        auto int8_span = ReinterpretAsSpan<const int8_t>(gsl::make_span(unpack_buffer));
        indices_values.insert(indices_values.cend(), int8_span.begin(), int8_span.end());
        indices_data = gsl::make_span(indices_values);
        unpack_buffer.clear();
        unpack_buffer.shrink_to_fit();
      } else {
        return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_GRAPH,
                               "Invalid SparseTensor indices. INT8 indices must be in the raw data of indices tensor");
      }
      break;
    }
    default:
      return ORT_MAKE_STATUS(
          ONNXRUNTIME, INVALID_GRAPH,
          "Invalid SparseTensor indices. Should one of the following types: int8, int16, int32 or int64");
  }

  if (indices_shape.NumDimensions() == 1) {
    // flattened indexes
    for (size_t i = 0; i < n_sparse_elements; ++i) {
      copier(i, narrow<size_t>(indices_data[i]));
    }
  } else if (indices_shape.NumDimensions() == 2) {
    // entries in format {NNZ, rank}
    ORT_ENFORCE(indices_shape[1] > 0 && static_cast<size_t>(indices_shape[1]) == dims.size());
    auto rank = static_cast<size_t>(indices_shape[1]);
    auto cur_index = indices_data.begin();
    std::vector<size_t> multipliers;
    multipliers.resize(rank);

    // calculate sum of inner dimension elements for each dimension.
    // e.g. if shape {2,3,4}, the result should be {3*4, 4, 1}
    multipliers[rank - 1] = 1;
    for (auto r = rank - 1; r > 0; --r) {
      multipliers[r - 1] = SafeInt<size_t>(dims[r]) * multipliers[r];
    }

    // calculate the offset for the entry
    // e.g. if shape was {2,3,4} and entry was (1, 0, 2) the offset is 14
    // as there are 2 rows, each with 12 entries per row
    for (size_t i = 0; i < n_sparse_elements; ++i) {
      SafeInt<int64_t> idx = 0;
      for (size_t j = 0; j < rank; ++j) {
        idx += SafeInt<int64_t>(cur_index[j]) * multipliers[j];
      }

      copier(i, static_cast<size_t>(idx));
      cur_index += rank;
    }

    ORT_ENFORCE(cur_index == indices_data.end());
  } else {
    status = ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_GRAPH,
                             "Invalid SparseTensor indices. Should be rank 0 or 1. Got:", indices_shape);
  }

  return status;
}

common::Status SparseTensorProtoToDenseTensorProto(const ONNX_NAMESPACE::SparseTensorProto& sparse,
                                                   const std::filesystem::path& model_path,
                                                   ONNX_NAMESPACE::TensorProto& dense) {
  Status status = Status::OK();

  const auto& sparse_values = sparse.values();
  auto type = sparse_values.data_type();
  dense.set_data_type(type);
  *dense.mutable_name() = sparse_values.name();

  SafeInt<size_t> n_sparse_elements = 1;
  for (auto dim : sparse_values.dims()) {
    n_sparse_elements *= dim;
  }

  SafeInt<size_t> n_dense_elements = 1;
  for (auto dim : sparse.dims()) {
    n_dense_elements *= dim;
    dense.add_dims(dim);
  }

  const auto& indices = sparse.indices();
  auto dims = gsl::make_span<const int64_t>(dense.dims().data(), dense.dims().size());

  if (type != TensorProto_DataType_STRING) {
    auto ml_data = DataTypeImpl::TensorTypeFromONNXEnum(type)->GetElementType();
    size_t element_size = ml_data->Size();

    // need to read in sparse data first as it could be in a type specific field, in raw data, or in external data
    std::vector<uint8_t> sparse_data_storage;
    ORT_RETURN_IF_ERROR(UnpackInitializerData(sparse_values, model_path, sparse_data_storage));
    void* sparse_data = sparse_data_storage.data();

    // by putting the data into a std::string we can avoid a copy as set_raw_data can do a std::move
    // into the TensorProto.
    std::string dense_data_storage(n_dense_elements * element_size, 0);
    if (n_sparse_elements > 0) {
      void* dense_data = dense_data_storage.data();

      switch (element_size) {
        case 1: {
          status = CopySparseData(
              n_sparse_elements, indices, model_path, dims, [sparse_data, dense_data](size_t from_idx, size_t to_idx) {
                static_cast<uint8_t*>(dense_data)[to_idx] = static_cast<const uint8_t*>(sparse_data)[from_idx];
              });

          break;
        }
        case 2: {
          status = CopySparseData(n_sparse_elements, indices, model_path, dims,
                                  [sparse_data, dense_data](size_t from_idx, size_t to_idx) {
                                    const auto* src = static_cast<const uint16_t*>(sparse_data) + from_idx;
                                    auto* dst = static_cast<uint16_t*>(dense_data) + to_idx;
                                    memcpy(dst, src, sizeof(uint16_t));
                                  });

          break;
        }
        case 4: {
          status = CopySparseData(n_sparse_elements, indices, model_path, dims,
                                  [sparse_data, dense_data](size_t from_idx, size_t to_idx) {
                                    const auto* src = static_cast<const uint32_t*>(sparse_data) + from_idx;
                                    auto* dst = static_cast<uint32_t*>(dense_data) + to_idx;
                                    memcpy(dst, src, sizeof(uint32_t));
                                  });

          break;
        }
        case 8: {
          status = CopySparseData(n_sparse_elements, indices, model_path, dims,
                                  [sparse_data, dense_data](size_t from_idx, size_t to_idx) {
                                    const auto* src = static_cast<const uint64_t*>(sparse_data) + from_idx;
                                    auto* dst = static_cast<uint64_t*>(dense_data) + to_idx;
                                    memcpy(dst, src, sizeof(uint64_t));
                                  });
          break;
        }

        default:
          return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Element_size of: ", element_size, " is not supported.",
                                 " type: ", type);
      }

      ORT_RETURN_IF_ERROR(status);
    }
    utils::SetRawDataInTensorProto(dense, std::move(dense_data_storage));
  } else {
    // No request for std::string
    status = ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Unsupported sparse tensor data type of ",
                             ONNX_NAMESPACE::TensorProto_DataType_STRING);
  }
  return status;
}

#if !defined(ORT_MINIMAL_BUILD)
// Determines if this is a type specific zero
using IsZeroFunc = bool (*)(const void*);
// Copy element
using CopyElementFunc = void (*)(void* dest, const void* src, int64_t dest_index, int64_t src_index);

// Here we are not using tolerance for FP types since these dense tensors were
// created from sparse initializers where zeros were absolute
template <typename T>
inline bool IsZero(const void* p) {
  return (static_cast<T>(0) == *reinterpret_cast<const T*>(p));
}

template <typename T>
inline void CopyElement(void* dst, const void* src, int64_t dst_index, int64_t src_index) {
  const auto* src_p = reinterpret_cast<const T*>(src) + src_index;
  auto* dst_p = reinterpret_cast<T*>(dst) + dst_index;
  memcpy(dst_p, src_p, sizeof(T));
}

template <>
inline void CopyElement<uint8_t>(void* dst, const void* src, int64_t dst_index, int64_t src_index) {
  reinterpret_cast<uint8_t*>(dst)[dst_index] = reinterpret_cast<const uint8_t*>(src)[src_index];
}

template <typename T>
static void SetIndices(gsl::span<int64_t> gathered_indices, std::string& raw_indices, TensorProto& indices) {
  raw_indices.resize(gathered_indices.size() * sizeof(T));
  auto* ind_dest = reinterpret_cast<T*>(raw_indices.data());
  size_t dest_index = 0;
  for (auto src_index : gathered_indices) {
    if constexpr (sizeof(T) == sizeof(int8_t)) {
      ind_dest[dest_index] = static_cast<T>(src_index);
    } else {
      auto* dst = ind_dest + dest_index;
      T v = static_cast<T>(src_index);
      if constexpr (endian::native != endian::little) {
        auto src = gsl::make_span<const unsigned char>(static_cast<const unsigned char*>(
                                                           reinterpret_cast<const unsigned char*>(&v)),
                                                       sizeof(T));
        auto dest = gsl::make_span<unsigned char>(static_cast<unsigned char*>(
                                                      reinterpret_cast<unsigned char*>(dst)),
                                                  sizeof(T));
        onnxruntime::utils::SwapByteOrderCopy(sizeof(T), src, dest);
      } else {
        memcpy(dst, &v, sizeof(T));
      }
    }
    ++dest_index;
  }
  indices.set_data_type(utils::ToTensorProtoElementType<T>());
}

static void SparsifyGeneric(const void* dense_raw_data, size_t n_dense_elements, size_t element_size,
                            IsZeroFunc is_zero, CopyElementFunc copy, TensorProto& values, TensorProto& indices,
                            size_t& nnz) {
  auto advance = [element_size](const void* start, size_t elements) -> const void* {
    return (reinterpret_cast<const uint8_t*>(start) + elements * element_size);
  };

  const auto* cbegin = dense_raw_data;
  const auto* const cend = advance(cbegin, n_dense_elements);
  std::vector<int64_t> gathered_indices;
  int64_t index = 0;
  while (cbegin != cend) {
    if (!is_zero(cbegin)) {
      gathered_indices.push_back(index);
    }
    ++index;
    cbegin = advance(cbegin, 1U);
  }

  if (!gathered_indices.empty()) {
    auto& raw_data = *values.mutable_raw_data();
    raw_data.resize(gathered_indices.size() * element_size);
    void* data_dest = raw_data.data();

    int64_t dest_index = 0;
    for (auto src_index : gathered_indices) {
      copy(data_dest, dense_raw_data, dest_index, src_index);
      ++dest_index;
    }

    auto gathered_span = gsl::make_span(gathered_indices);
    auto& raw_indices = *indices.mutable_raw_data();
    const auto max_index = gathered_indices.back();
    if (max_index <= std::numeric_limits<int8_t>::max()) {
      SetIndices<int8_t>(gathered_span, raw_indices, indices);
    } else if (max_index <= std::numeric_limits<int16_t>::max()) {
      SetIndices<int16_t>(gathered_span, raw_indices, indices);
    } else if (max_index <= std::numeric_limits<int32_t>::max()) {
      SetIndices<int32_t>(gathered_span, raw_indices, indices);
    } else {
      SetIndices<int64_t>(gathered_span, raw_indices, indices);
    }
  } else {
    indices.set_data_type(ONNX_NAMESPACE::TensorProto_DataType_INT8);
    utils::SetRawDataInTensorProto(indices, std::string());
  }
  nnz = gathered_indices.size();
}

common::Status DenseTensorToSparseTensorProto(const ONNX_NAMESPACE::TensorProto& dense_proto,
                                              const std::filesystem::path& model_path,
                                              ONNX_NAMESPACE::SparseTensorProto& result) {
  ORT_ENFORCE(HasDataType(dense_proto), "Must have a valid data type");

  if (dense_proto.data_type() == ONNX_NAMESPACE::TensorProto_DataType_STRING) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Unsupported sparse tensor data type of ",
                           ONNX_NAMESPACE::TensorProto_DataType_STRING);
  }

  const auto data_type = dense_proto.data_type();
  SparseTensorProto sparse_proto;
  auto& values = *sparse_proto.mutable_values();
  values.set_name(dense_proto.name());
  values.set_data_type(data_type);

  auto& indices = *sparse_proto.mutable_indices();

  SafeInt<size_t> n_dense_elements = 1;
  for (auto dim : dense_proto.dims()) {
    n_dense_elements *= dim;
  }

  auto ml_data = DataTypeImpl::TensorTypeFromONNXEnum(data_type)->GetElementType();
  size_t element_size = ml_data->Size();

  std::vector<uint8_t> dense_raw_data;
  ORT_RETURN_IF_ERROR(UnpackInitializerData(dense_proto, model_path, dense_raw_data));

  size_t nnz = 0;
  void* dense_data = dense_raw_data.data();
  switch (element_size) {
    case 1: {
      SparsifyGeneric(dense_data, n_dense_elements, element_size, IsZero<uint8_t>, CopyElement<uint8_t>, values,
                      indices, nnz);
      break;
    }
    case 2: {
      SparsifyGeneric(dense_data, n_dense_elements, element_size, IsZero<uint16_t>, CopyElement<uint16_t>, values,
                      indices, nnz);
      break;
    }
    case 4: {
      SparsifyGeneric(dense_data, n_dense_elements, element_size, IsZero<uint32_t>, CopyElement<uint32_t>, values,
                      indices, nnz);
      break;
    }
    case 8: {
      SparsifyGeneric(dense_data, n_dense_elements, element_size, IsZero<uint64_t>, CopyElement<uint64_t>, values,
                      indices, nnz);
      break;
    }
    default:
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Element_size of: ", element_size, " is not supported.",
                             " data_type: ", data_type);
  }

  // Fix up shapes
  values.add_dims(nnz);
  indices.add_dims(nnz);

  // Save dense shape
  *sparse_proto.mutable_dims() = dense_proto.dims();
  swap(result, sparse_proto);
  return Status::OK();
}

#endif  // !ORT_MINIMAL_BUILD
#endif  // !defined(DISABLE_SPARSE_TENSORS)

template common::Status GetSizeInBytesFromTensorProto<kAllocAlignment>(const ONNX_NAMESPACE::TensorProto& tensor_proto,
                                                                       size_t* out);
template common::Status GetSizeInBytesFromTensorProto<0>(const ONNX_NAMESPACE::TensorProto& tensor_proto, size_t* out);

#define CASE_UNPACK(TYPE, ELEMENT_TYPE, DATA_SIZE)                                                                   \
  case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_##TYPE: {                                          \
    SafeInt<size_t> tensor_byte_size;                                                                                \
    size_t element_count = 0;                                                                                        \
    if (initializer.has_raw_data()) {                                                                                \
      tensor_byte_size = initializer.raw_data().size();                                                              \
      element_count = tensor_byte_size / sizeof(ELEMENT_TYPE);                                                       \
    } else {                                                                                                         \
      element_count = initializer.DATA_SIZE();                                                                       \
      tensor_byte_size = element_count * sizeof(ELEMENT_TYPE);                                                       \
    }                                                                                                                \
    unpacked_tensor.resize(tensor_byte_size);                                                                        \
    return onnxruntime::utils::UnpackTensor(initializer,                                                             \
                                            initializer.has_raw_data() ? initializer.raw_data().data() : nullptr,    \
                                            initializer.has_raw_data() ? initializer.raw_data().size() : 0,          \
                                            reinterpret_cast<ELEMENT_TYPE*>(unpacked_tensor.data()), element_count); \
    break;                                                                                                           \
  }

#define CASE_UNPACK_INT4(TYPE, ELEMENT_TYPE, DATA_SIZE)                                                              \
  case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_##TYPE: {                                          \
    TensorShape tensor_shape = GetTensorShapeFromTensorProto(initializer);                                           \
    size_t element_count = static_cast<size_t>(tensor_shape.Size());                                                 \
    size_t packed_element_count = ELEMENT_TYPE::CalcNumInt4Pairs(element_count);                                     \
    unpacked_tensor.resize(packed_element_count * sizeof(ELEMENT_TYPE));                                             \
    return onnxruntime::utils::UnpackTensor(initializer,                                                             \
                                            initializer.has_raw_data() ? initializer.raw_data().data() : nullptr,    \
                                            initializer.has_raw_data() ? initializer.raw_data().size() : 0,          \
                                            reinterpret_cast<ELEMENT_TYPE*>(unpacked_tensor.data()), element_count); \
    break;                                                                                                           \
  }

Status UnpackInitializerData(const onnx::TensorProto& initializer,
                             const std::filesystem::path& model_path,
                             std::vector<uint8_t>& unpacked_tensor) {
  // TODO, if std::vector does not use a custom allocator, the default std::allocator will
  // allocation the memory aligned to std::max_align_t, need look into allocating
  // forced aligned memory (align as 16 or larger)for unpacked_tensor
  if (HasExternalData(initializer)) {
    ORT_RETURN_IF_ERROR(ReadExternalDataForTensor(
        initializer,
        model_path.parent_path(),
        unpacked_tensor));
    return Status::OK();
  }

  switch (initializer.data_type()) {
    CASE_UNPACK(FLOAT, float, float_data_size);
    CASE_UNPACK(DOUBLE, double, double_data_size);
    CASE_UNPACK(BOOL, bool, int32_data_size);
    CASE_UNPACK(INT8, int8_t, int32_data_size);
    CASE_UNPACK(INT16, int16_t, int32_data_size);
    CASE_UNPACK(INT32, int32_t, int32_data_size);
    CASE_UNPACK(INT64, int64_t, int64_data_size);
    CASE_UNPACK(UINT8, uint8_t, int32_data_size);
    CASE_UNPACK(UINT16, uint16_t, int32_data_size);
    CASE_UNPACK(UINT32, uint32_t, uint64_data_size);
    CASE_UNPACK(UINT64, uint64_t, uint64_data_size);
    CASE_UNPACK(FLOAT16, onnxruntime::MLFloat16, int32_data_size);
    CASE_UNPACK(BFLOAT16, onnxruntime::BFloat16, int32_data_size);
#if !defined(DISABLE_FLOAT8_TYPES)
    CASE_UNPACK(FLOAT8E4M3FN, onnxruntime::Float8E4M3FN, int32_data_size);
    CASE_UNPACK(FLOAT8E4M3FNUZ, onnxruntime::Float8E4M3FNUZ, int32_data_size);
    CASE_UNPACK(FLOAT8E5M2, onnxruntime::Float8E5M2, int32_data_size);
    CASE_UNPACK(FLOAT8E5M2FNUZ, onnxruntime::Float8E5M2FNUZ, int32_data_size);
#endif
    CASE_UNPACK_INT4(INT4, Int4x2, int32_data_size);
    CASE_UNPACK_INT4(UINT4, UInt4x2, int32_data_size);
    default:
      break;
  }
  return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Unsupported type: ", initializer.data_type());
}
#undef CASE_UNPACK

Status UnpackInitializerData(const ONNX_NAMESPACE::TensorProto& initializer, std::vector<uint8_t>& unpacked_tensor) {
  ORT_RETURN_IF(initializer.data_location() == TensorProto_DataLocation_EXTERNAL,
                "The given initializer contains external data");
  return UnpackInitializerData(initializer, std::filesystem::path(), unpacked_tensor);
}

}  // namespace utils
}  // namespace onnxruntime
