// Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "src/backends/backend/examples/backend_utils.h"
#include "src/backends/backend/tensorflow/model_instance.h"
#include "src/backends/backend/tensorflow/tf_utils.h"
#include "src/backends/tensorflow/tensorflow_backend_tf.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>

#ifdef TRITON_ENABLE_GPU
#include <cuda_runtime_api.h>
#endif  // TRITON_ENABLE_GPU

namespace ni = nvidia::inferenceserver;
namespace nib = nvidia::inferenceserver::backend;

//
// TF Backend that implements the TRITONBACKEND API.
//

namespace {

#ifndef TRITON_ENABLE_GPU
using cudaStream_t = void*;
#endif  // !TRITON_ENABLE_GPU

using IONameMap = std::unordered_map<std::string, std::string>;
using TRTISTFModelHandle =
    std::unique_ptr<TRTISTF_Model, decltype(&TRTISTF_ModelDelete)>;

TRITONSERVER_Error*
ParseLongLongParameter(
    const std::string& key, const std::string& value, int64_t* parsed_value)
{
  try {
    *parsed_value = std::stoll(value);
  }
  catch (const std::invalid_argument& ia) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INVALID_ARG,
        (std::string("failed to convert ") + key + " '" + value +
         "' to integral number")
            .c_str());
  }

  return nullptr;  // success
}

void
RequestsRespondIfError(
    TRITONBACKEND_Request** requests, const uint32_t request_count,
    TRITONSERVER_Error* response_err)
{
  for (size_t i = 0; i < request_count; i++) {
    TRITONBACKEND_Response* response;
    auto err = TRITONBACKEND_ResponseNew(&response, requests[i]);
    if (err != nullptr) {
      LOG_MESSAGE(TRITONSERVER_LOG_ERROR, "Fail to create response");
      TRITONSERVER_ErrorDelete(err);
    } else {
      std::unique_ptr<
          TRITONBACKEND_Response, decltype(&TRITONBACKEND_ResponseDelete)>
          response_handle(response, TRITONBACKEND_ResponseDelete);
      err = TRITONBACKEND_ResponseSend(
          response, TRITONSERVER_RESPONSE_COMPLETE_FINAL, response_err);
      if (err != nullptr) {
        LOG_MESSAGE(TRITONSERVER_LOG_ERROR, "Fail to send response");
        TRITONSERVER_ErrorDelete(err);
      }
    }
    err = TRITONBACKEND_RequestRelease(
        requests[i], TRITONSERVER_REQUEST_RELEASE_ALL);
    if (err != nullptr) {
      LOG_MESSAGE(TRITONSERVER_LOG_ERROR, "Fail to release request");
      TRITONSERVER_ErrorDelete(err);
    }
  }
  TRITONSERVER_ErrorDelete(response_err);
}

namespace GraphDef {

TRITONSERVER_Error*
ValidateSequenceControl(
    const std::string& model_name, ni::TritonJson::Value& model_config,
    const std::string& control_kind, const TRTISTF_IOList* inputs,
    bool required, bool is_boolean)
{
  ni::TritonJson::Value sequence_batching;
  RETURN_IF_ERROR(
      model_config.MemberAsObject("sequence_batching", &sequence_batching));
  std::string tensor_name;
  if (is_boolean) {
    RETURN_IF_ERROR(nib::GetBooleanSequenceControlProperties(
        sequence_batching, model_name, control_kind, required, &tensor_name,
        nullptr, nullptr, nullptr, nullptr, nullptr));
  } else {
    RETURN_IF_ERROR(nib::GetTypedSequenceControlProperties(
        sequence_batching, model_name, control_kind, required, &tensor_name,
        nullptr));
  }
  if (!tensor_name.empty()) {
    const TRTISTF_IO* input = nib::FindIOByName(inputs, tensor_name);
    if (input == nullptr) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          (std::string(
               "configuration specified sequence control '" + tensor_name +
               "', but model does not provide that input")
               .c_str()));
    }
  }

  return nullptr;  // success
}

TRITONSERVER_Error*
CreateTRTISTFModel(
    ni::TritonJson::Value& backend_config, ni::TritonJson::Value& model_config,
    const int device_id, const bool has_graph_level, const int graph_level,
    const std::string& model_name, const std::string& model_path,
    TRTISTFModelHandle* trtistf_model, IONameMap* input_name_map,
    IONameMap* output_name_map, const TRTISTF_TFTRTConfig* tftrt_config,
    const bool auto_mixed_precision)
{
  TRTISTF_Model* model = nullptr;
  RETURN_IF_TRTISTF_ERROR(TRTISTF_ModelCreateFromGraphDef(
      &model, model_name.c_str(), model_path.c_str(), device_id,
      has_graph_level, graph_level, true, 0, true,
      std::map<int, std::vector<float>>(), tftrt_config, auto_mixed_precision));

  trtistf_model->reset(model);

  // For graphdef the model inputs and outputs are just "potential"
  // inputs and outputs since graphdef doesn't explicitly list the
  // inputs and outputs. Also, only the name is available, shape and
  // datatype are not.
  const TRTISTF_IOList* inputs = TRTISTF_ModelInputs(model);
  const TRTISTF_IOList* outputs = TRTISTF_ModelOutputs(model);

  std::set<std::string> potential_inputs, potential_outputs;
  for (const TRTISTF_IOList* itr = inputs; itr != nullptr; itr = itr->next_) {
    potential_inputs.insert(itr->io_->name_);
  }
  for (const TRTISTF_IOList* itr = outputs; itr != nullptr; itr = itr->next_) {
    potential_outputs.insert(itr->io_->name_);
  }

  ni::TritonJson::Value config_inputs;
  RETURN_IF_ERROR(model_config.MemberAsArray("input", &config_inputs));
  if (potential_inputs.size() < config_inputs.ArraySize()) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INVALID_ARG,
        std::string(
            "unable to load model '" + model_name +
            "', configuration expects " +
            std::to_string(config_inputs.ArraySize()) +
            " inputs, model provides at most " +
            std::to_string(potential_inputs.size()))
            .c_str());
  }

  // If this is a sequence model then make sure that the required
  // inputs are present in the model
  ni::TritonJson::Value sequence_batching;
  if (model_config.Find("sequence_batching", &sequence_batching)) {
    RETURN_IF_ERROR(ValidateSequenceControl(
        model_name, model_config, "CONTROL_SEQUENCE_START", inputs,
        false /* required */, true /* is_boolean */));
    RETURN_IF_ERROR(ValidateSequenceControl(
        model_name, model_config, "CONTROL_SEQUENCE_END", inputs,
        false /* required */, true /* is_boolean */));
    RETURN_IF_ERROR(ValidateSequenceControl(
        model_name, model_config, "CONTROL_SEQUENCE_READY", inputs,
        false /* required */, true /* is_boolean */));
    RETURN_IF_ERROR(ValidateSequenceControl(
        model_name, model_config, "CONTROL_SEQUENCE_CORRID", inputs,
        false /* required */, false /* is_boolean */));
  }

  for (size_t i = 0; i < config_inputs.ArraySize(); i++) {
    ni::TritonJson::Value io;
    RETURN_IF_ERROR(config_inputs.IndexAsObject(i, &io));
    RETURN_IF_ERROR(nib::CheckAllowedModelInput(io, potential_inputs));
  }

  ni::TritonJson::Value config_outputs;
  RETURN_IF_ERROR(model_config.MemberAsArray("output", &config_outputs));
  for (size_t i = 0; i < config_outputs.ArraySize(); i++) {
    ni::TritonJson::Value io;
    RETURN_IF_ERROR(config_outputs.IndexAsObject(i, &io));
    RETURN_IF_ERROR(nib::CheckAllowedModelOutput(io, potential_outputs));
  }

  return nullptr;  // success
}

}  // namespace GraphDef

namespace SavedModel {

TRITONSERVER_Error*
ValidateSequenceControl(
    const std::string& model_name, ni::TritonJson::Value& model_config,
    const std::string& control_kind, const TRTISTF_IOList* inputs,
    bool required, bool is_boolean, bool* have_control)
{
  ni::TritonJson::Value sequence_batching;
  RETURN_IF_ERROR(
      model_config.MemberAsObject("sequence_batching", &sequence_batching));
  std::string tensor_name;
  std::string tensor_datatype;
  if (is_boolean) {
    RETURN_IF_ERROR(nib::GetBooleanSequenceControlProperties(
        sequence_batching, model_name, control_kind, required, &tensor_name,
        &tensor_datatype, nullptr, nullptr, nullptr, nullptr));
  } else {
    RETURN_IF_ERROR(nib::GetTypedSequenceControlProperties(
        sequence_batching, model_name, control_kind, required, &tensor_name,
        &tensor_datatype));
  }

  *have_control = !tensor_name.empty();
  if (*have_control) {
    const TRTISTF_IO* input = nib::FindIOByName(inputs, tensor_name);
    if (input == nullptr) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          (std::string(
               "configuration specified sequence control '" + tensor_name +
               "', but model does not provide that input")
               .c_str()));
    }

    // Control tensors must have shape [1].
    std::vector<int64_t> dims{1};

    int64_t max_batch_size;
    RETURN_IF_ERROR(
        model_config.MemberAsInt("max_batch_size", &max_batch_size));

    auto err = nib::CompareDims(
        model_name, tensor_name, input->shape_, dims, max_batch_size > 0,
        true /* compare_exact */);
    if (err != nullptr) {
      auto detailed_err = TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INVALID_ARG,
          std::string(
              "unable to load model '" + model_name + "', sequence control '" +
              tensor_name + "': " + TRITONSERVER_ErrorMessage(err))
              .c_str());
      TRITONSERVER_ErrorDelete(err);
      return detailed_err;
    }

    if (!nib::CompareDataType(input->data_type_, tensor_datatype)) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INVALID_ARG,
          std::string(
              "unable to load model '" + model_name + "', sequence control '" +
              tensor_name + "': the model expects data-type " +
              TRITONSERVER_DataTypeString(
                  nib::ConvertDataType(input->data_type_)) +
              " but the model configuration specifies data-type " +
              tensor_datatype)
              .c_str());
    }
  }

  return nullptr;  // success
}

TRITONSERVER_Error*
CreateTRTISTFModel(
    ni::TritonJson::Value& backend_config, ni::TritonJson::Value& model_config,
    const int device_id, const bool has_graph_level, const int graph_level,
    const std::string& model_name, const std::string& model_path,
    TRTISTFModelHandle* trtistf_model, IONameMap* input_name_map,
    IONameMap* output_name_map, const TRTISTF_TFTRTConfig* tftrt_config,
    const bool auto_mixed_precision)
{
  TRTISTF_Model* model = nullptr;

  // Set default backend values. Note that those values should be set during
  // TRITONBACKEND_Initialize, but for TensorFlow they are set while creating
  // the model.
  bool allow_gpu_memory_growth = true;
  float per_process_gpu_memory_fraction = 0.0;
  bool allow_soft_placement = true;
  // FIXME: vGPU only flag, should be removed
  std::map<int, std::vector<float>> memory_limit_mb;
  {
    ni::TritonJson::Value cmdline;
    if (backend_config.Find("cmdline", &cmdline)) {
      ni::TritonJson::Value value;
      if (cmdline.Find("allow-soft-placement", &value)) {
        RETURN_IF_ERROR(value.AsBool(&allow_soft_placement));
      }
      if (cmdline.Find("gpu-memory-fraction", &value)) {
        double lvalue;
        RETURN_IF_ERROR(value.AsDouble(&lvalue));
        per_process_gpu_memory_fraction = lvalue;
        allow_gpu_memory_growth = (lvalue == 0.0);
      }
    }
  }
  RETURN_IF_TRTISTF_ERROR(TRTISTF_ModelCreateFromSavedModel(
      &model, model_name.c_str(), model_path.c_str(), device_id,
      has_graph_level, graph_level, allow_gpu_memory_growth,
      per_process_gpu_memory_fraction, allow_soft_placement,
      std::map<int, std::vector<float>>(), tftrt_config, auto_mixed_precision));

  trtistf_model->reset(model);

  // The model inputs are the expected inputs and the outputs are
  // the allowed outputs. Saved-model gives these explicitly so we can
  // check precisely if the model configuration matches.
  const TRTISTF_IOList* inputs = TRTISTF_ModelInputs(model);
  const TRTISTF_IOList* outputs = TRTISTF_ModelOutputs(model);

  std::set<std::string> expected_inputs, allowed_outputs;
  for (const TRTISTF_IOList* itr = inputs; itr != nullptr; itr = itr->next_) {
    expected_inputs.insert(itr->io_->name_);
    input_name_map->insert({itr->io_->name_, itr->io_->inmodel_name_});
  }
  for (const TRTISTF_IOList* itr = outputs; itr != nullptr; itr = itr->next_) {
    allowed_outputs.insert(itr->io_->name_);
    output_name_map->insert({itr->io_->name_, itr->io_->inmodel_name_});
  }

  ni::TritonJson::Value config_inputs;
  RETURN_IF_ERROR(model_config.MemberAsArray("input", &config_inputs));
  size_t expected_input_cnt = config_inputs.ArraySize();

  // If this is a sequence model then make sure that the required
  // inputs are present in the model and have the correct shape and
  // datatype.
  ni::TritonJson::Value sequence_batching;
  if (model_config.Find("sequence_batching", &sequence_batching)) {
    bool have_start, have_end, have_ready, have_corrid;
    RETURN_IF_ERROR(ValidateSequenceControl(
        model_name, model_config, "CONTROL_SEQUENCE_START", inputs,
        false /* required */, true /* is_boolean */, &have_start));
    RETURN_IF_ERROR(ValidateSequenceControl(
        model_name, model_config, "CONTROL_SEQUENCE_END", inputs,
        false /* required */, true /* is_boolean */, &have_end));
    RETURN_IF_ERROR(ValidateSequenceControl(
        model_name, model_config, "CONTROL_SEQUENCE_READY", inputs,
        false /* required */, true /* is_boolean */, &have_ready));
    RETURN_IF_ERROR(ValidateSequenceControl(
        model_name, model_config, "CONTROL_SEQUENCE_CORRID", inputs,
        false /* required */, false /* is_boolean */, &have_corrid));
    if (have_start) {
      expected_input_cnt += 1;
    }
    if (have_end) {
      expected_input_cnt += 1;
    }
    if (have_ready) {
      expected_input_cnt += 1;
    }
    if (have_corrid) {
      expected_input_cnt += 1;
    }
  }

  // Verify that the model configuration input and outputs match what
  // is expected by the model.
  if (expected_inputs.size() != expected_input_cnt) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INVALID_ARG,
        std::string(
            "unable to load model '" + model_name +
            "', configuration expects " +
            std::to_string(config_inputs.ArraySize()) +
            " inputs, model provides " + std::to_string(expected_inputs.size()))
            .c_str());
  }

  int64_t max_batch_size;
  RETURN_IF_ERROR(model_config.MemberAsInt("max_batch_size", &max_batch_size));

  for (size_t i = 0; i < config_inputs.ArraySize(); i++) {
    ni::TritonJson::Value io;
    RETURN_IF_ERROR(config_inputs.IndexAsObject(i, &io));
    RETURN_IF_ERROR(nib::CheckAllowedModelInput(io, expected_inputs));

    std::string io_name;
    RETURN_IF_ERROR(io.MemberAsString("name", &io_name));
    const TRTISTF_IO* input = nib::FindIOByName(inputs, io_name);
    if (input == nullptr) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          std::string("unexpected inference input '" + io_name + "'").c_str());
    }

    // If a reshape is provided for the input then use that when
    // validating that the TF model matches what is expected.
    std::vector<int64_t> dims;
    ni::TritonJson::Value reshape;
    if (io.Find("reshape", &reshape)) {
      RETURN_IF_ERROR(nib::ParseShape(reshape, "shape", &dims));
    } else {
      RETURN_IF_ERROR(nib::ParseShape(io, "dims", &dims));
    }
    if (input->shape_->rank_ != 0) {
      RETURN_IF_ERROR(nib::CompareDims(
          model_name, io_name, input->shape_, dims, max_batch_size > 0,
          false /* compare_exact */));
    } else {
      // The savedmodel doesn't specify a shape for the input so use the shape
      // from the model configuration
      bool supports_batching = max_batch_size > 0;
      input->shape_->rank_ =
          (size_t)(dims.size() + (supports_batching ? 1 : 0));
      input->shape_->dims_ =
          (int64_t*)malloc(input->shape_->rank_ * sizeof(int64_t));
      for (size_t i = 0; i < dims.size(); ++i) {
        input->shape_->dims_[i + (supports_batching ? 1 : 0)] = dims[i];
      }
    }

    std::string io_data_type;
    RETURN_IF_ERROR(io.MemberAsString("data_type", &io_data_type));
    if (!nib::CompareDataType(input->data_type_, io_data_type)) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INVALID_ARG,
          std::string(
              "unable to load model '" + model_name + "', input '" + io_name +
              "' data-type " +
              TRITONSERVER_DataTypeString(
                  nib::ConvertDataType(input->data_type_)) +
              " doesn't match configuration data-type " + io_data_type)
              .c_str());
    }
  }

  ni::TritonJson::Value config_outputs;
  RETURN_IF_ERROR(model_config.MemberAsArray("output", &config_outputs));
  for (size_t i = 0; i < config_outputs.ArraySize(); i++) {
    ni::TritonJson::Value io;
    RETURN_IF_ERROR(config_outputs.IndexAsObject(i, &io));
    RETURN_IF_ERROR(nib::CheckAllowedModelOutput(io, allowed_outputs));

    std::string io_name;
    RETURN_IF_ERROR(io.MemberAsString("name", &io_name));
    const TRTISTF_IO* output = nib::FindIOByName(outputs, io_name);
    if (output == nullptr) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          std::string("unexpected inference output '" + io_name + "'").c_str());
    }

    // If a reshape is provided for the input then use that when
    // validating that the TF model matches what is expected.
    std::vector<int64_t> dims;
    ni::TritonJson::Value reshape;
    if (io.Find("reshape", &reshape)) {
      RETURN_IF_ERROR(nib::ParseShape(reshape, "shape", &dims));
    } else {
      RETURN_IF_ERROR(nib::ParseShape(io, "dims", &dims));
    }

    if (output->shape_->rank_ != 0) {
      RETURN_IF_ERROR(nib::CompareDims(
          model_name, io_name, output->shape_, dims, max_batch_size > 0,
          true /* compare_exact */));
    } else {
      // The savedmodel doesn't specify a shape for the output so use the shape
      // from the model configuration
      bool supports_batching = max_batch_size > 0;
      output->shape_->rank_ =
          (size_t)(dims.size() + (supports_batching ? 1 : 0));
      output->shape_->dims_ =
          (int64_t*)malloc(output->shape_->rank_ * sizeof(int64_t));
      for (size_t i = 0; i < dims.size(); ++i) {
        output->shape_->dims_[i + (supports_batching ? 1 : 0)] = dims[i];
      }
    }

    std::string io_data_type;
    RETURN_IF_ERROR(io.MemberAsString("data_type", &io_data_type));
    if (!nib::CompareDataType(output->data_type_, io_data_type)) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INVALID_ARG,
          std::string(
              "unable to load model '" + model_name + "', output '" + io_name +
              "' data-type " +
              TRITONSERVER_DataTypeString(
                  nib::ConvertDataType(output->data_type_)) +
              " doesn't match configuration data-type " + io_data_type)
              .c_str());
    }
  }

  return nullptr;  // success
}

}  // namespace SavedModel

// This function will return a tensor's contents as a contiguous
// chunk in system memory. In some cases this will require copying the data.
// If that  happens, 'contiguous_buffer' will be set to hold the contiguous
// chunk and 'cuda_copy' will be set to indicate whether CUDA copy is
// conducted.  The data copy can be avoided if the input is already in
// a contiguous chunk and the input is located in memory type and id
// specified.
TRITONSERVER_Error*
GetContiguousInputContent(
    TRITONBACKEND_Input* rinput, const uint32_t buffer_count,
    const char** content, size_t* content_byte_size, char** contiguous_buffer,
    cudaStream_t stream, bool* cuda_copy)
{
  *cuda_copy = false;
  *contiguous_buffer = nullptr;

  // Check input buffers to see if data copy is necessary
  size_t chunk_count = 0;
  bool type_mismatch = false;
  uint64_t total_byte_size = 0;
  for (size_t idx = 0; idx < buffer_count; ++idx) {
    TRITONSERVER_MemoryType src_memory_type;
    int64_t src_memory_type_id;
    size_t src_byte_size;
    const void* src_ptr;

    RETURN_IF_ERROR(TRITONBACKEND_InputBuffer(
        rinput, idx, &src_ptr, &src_byte_size, &src_memory_type,
        &src_memory_type_id));

    if (src_ptr != nullptr) {
      chunk_count++;
      total_byte_size += src_byte_size;
      type_mismatch |= (src_memory_type == TRITONSERVER_MEMORY_GPU);
    }
  }

  if (chunk_count == 0) {
    *content = nullptr;
    *content_byte_size = 0;
  } else if ((chunk_count == 1) && !type_mismatch) {
    TRITONSERVER_MemoryType src_memory_type;
    int64_t src_memory_type_id;
    RETURN_IF_ERROR(TRITONBACKEND_InputBuffer(
        rinput, 0, (const void**)content, content_byte_size, &src_memory_type,
        &src_memory_type_id));
  } else {
    *contiguous_buffer = (char*)malloc(total_byte_size);

    size_t offset = 0;
    for (size_t i = 0; i < chunk_count; i++) {
      bool cuda_used;
      TRITONSERVER_MemoryType src_memory_type;
      int64_t src_memory_type_id;
      size_t src_byte_size;
      const void* src_ptr;

      RETURN_IF_ERROR(TRITONBACKEND_InputBuffer(
          rinput, i, &src_ptr, &src_byte_size, &src_memory_type,
          &src_memory_type_id));
      RETURN_IF_ERROR(nib::CopyBuffer(
          "Contiguous input", src_memory_type, src_memory_type_id,
          TRITONSERVER_MEMORY_CPU, 0, src_byte_size, src_ptr,
          *contiguous_buffer + offset, stream, &cuda_used));
      *cuda_copy |= cuda_used;
      offset += src_byte_size;
    }

    *content = *contiguous_buffer;
    *content_byte_size = total_byte_size;
  }

  return nullptr;  // success
}

void
FillStringTensor(TRTISTF_Tensor* tensor, const size_t idx, const size_t cnt)
{
  for (size_t c = 0; c < cnt; ++c) {
    TRTISTF_TensorSetString(tensor, idx + c, nullptr, 0);
  }
}

bool
SetStringInputTensor(
    TRTISTF_Tensor* tensor, TRITONBACKEND_Input* input, const char* name,
    const uint32_t buffer_count, const size_t request_element_cnt,
    const size_t tensor_offset, TRITONBACKEND_Response** response,
    cudaStream_t stream)
{
  bool cuda_copy = false;
  size_t element_idx = 0;

  // For string data type, we always need to have the data on CPU so
  // that we can read string length and construct the string
  // properly. So if the request's input tensor is not in CPU need to
  // copy it there.
  const char* content = nullptr;
  size_t content_byte_size = 0;

  char* contiguous_buffer = nullptr;
  auto err = GetContiguousInputContent(
      input, buffer_count, &content, &content_byte_size, &contiguous_buffer,
      stream, &cuda_copy);
  if (err != nullptr) {
    RESPOND_AND_SET_NULL_IF_ERROR(response, err);
    FillStringTensor(
        tensor, tensor_offset + element_idx, request_element_cnt - element_idx);
    free(contiguous_buffer);
    return cuda_copy;
  }

#ifdef TRITON_ENABLE_GPU
  if (cuda_copy) {
    cudaStreamSynchronize(stream);
    cuda_copy = false;
  }
#endif  // TRITON_ENABLE_GPU

  // Parse content and assign to 'tensor'. Each string in 'content'
  // is a 4-byte length followed by the string itself with no
  // null-terminator.
  while (content_byte_size >= sizeof(uint32_t)) {
    if (element_idx >= request_element_cnt) {
      RESPOND_AND_SET_NULL_IF_ERROR(
          response,
          TRITONSERVER_ErrorNew(
              TRITONSERVER_ERROR_INVALID_ARG,
              std::string(
                  "unexpected number of string elements " +
                  std::to_string(element_idx + 1) + " for inference input '" +
                  name + "', expecting " + std::to_string(request_element_cnt))
                  .c_str()));
      FillStringTensor(
          tensor, tensor_offset + element_idx,
          request_element_cnt - element_idx);
      free(contiguous_buffer);
      return cuda_copy;
    }

    const uint32_t len = *(reinterpret_cast<const uint32_t*>(content));
    content += sizeof(uint32_t);
    content_byte_size -= sizeof(uint32_t);

    if (content_byte_size < len) {
      RESPOND_AND_SET_NULL_IF_ERROR(
          response,
          TRITONSERVER_ErrorNew(
              TRITONSERVER_ERROR_INVALID_ARG,
              std::string(
                  "incomplete string data for inference input '" +
                  std::string(name) + "', expecting string of length " +
                  std::to_string(len) + " but only " +
                  std::to_string(content_byte_size) + " bytes available")
                  .c_str()));
      FillStringTensor(
          tensor, tensor_offset + element_idx,
          request_element_cnt - element_idx);
      free(contiguous_buffer);
      return cuda_copy;
    }

    TRTISTF_TensorSetString(tensor, tensor_offset + element_idx, content, len);
    content += len;
    content_byte_size -= len;
    element_idx++;
  }

  if ((*response != nullptr) && (element_idx != request_element_cnt)) {
    RESPOND_AND_SET_NULL_IF_ERROR(
        response, TRITONSERVER_ErrorNew(
                      TRITONSERVER_ERROR_INTERNAL,
                      std::string(
                          "expected " + std::to_string(request_element_cnt) +
                          " strings for inference input '" + name + "', got " +
                          std::to_string(element_idx))
                          .c_str()));
    FillStringTensor(
        tensor, tensor_offset + element_idx, request_element_cnt - element_idx);
  }

  free(contiguous_buffer);
  return cuda_copy;
}

bool
SetStringOutputBuffer(
    TRTISTF_Tensor* tensor, TRITONBACKEND_Response** response,
    TRITONBACKEND_Output* response_output, const size_t tensor_element_count,
    const size_t tensor_offset, cudaStream_t stream, std::string* serialized)
{
  bool cuda_copy = false;

  // Serialize the output tensor strings. Each string is serialized as
  // a 4-byte length followed by the string itself with no
  // null-terminator.
  serialized->clear();
  for (size_t e = 0; e < tensor_element_count; ++e) {
    size_t len;
    const char* cstr = TRTISTF_TensorString(tensor, tensor_offset + e, &len);
    serialized->append(reinterpret_cast<const char*>(&len), sizeof(uint32_t));
    if (len > 0) {
      serialized->append(cstr, len);
    }
  }

  // Allocate a buffer large enough to hold the serialized tensor.
  TRITONSERVER_MemoryType actual_memory_type = TRITONSERVER_MEMORY_CPU;
  int64_t actual_memory_type_id = 0;

  void* buffer;
  auto err = TRITONBACKEND_OutputBuffer(
      response_output, &buffer, serialized->size(), &actual_memory_type,
      &actual_memory_type_id);
  if (err != nullptr) {
    RESPOND_AND_SET_NULL_IF_ERROR(response, err);
    return cuda_copy;
  }

  // Copy the serialized tensor into the allocated buffer.
  bool cuda_used = false;
  err = nib::CopyBuffer(
      "String output", TRITONSERVER_MEMORY_CPU /* src_memory_type */,
      0 /* src_memory_type_id */, actual_memory_type, actual_memory_type_id,
      serialized->size(), reinterpret_cast<const void*>(serialized->c_str()),
      buffer, stream, &cuda_used);
  cuda_copy |= cuda_used;

  if (err != nullptr) {
    RESPOND_AND_SET_NULL_IF_ERROR(response, err);
    return cuda_copy;
  }

  return cuda_copy;
}

//
// ModelState
//
// State associated with a model that is using this backend. An object
// of this class is created and associated with each
// TRITONBACKEND_Model.
//
class ModelState {
 public:
  static TRITONSERVER_Error* Create(
      TRITONBACKEND_Model* triton_model, ModelState** state);

  // Validate that model configuration is supported by this backend.
  TRITONSERVER_Error* ValidateModelConfig();

  const std::string& Name() { return name_; }
  TRITONBACKEND_Model* TritonModel() { return triton_model_; }
  ni::TritonJson::Value& BackendConfig() { return backend_config_; }
  ni::TritonJson::Value& ModelConfig() { return model_config_; }
  const std::unordered_map<std::string, std::string>& ModelPaths() const
  {
    return model_paths_;
  }
  bool IsGraphdef() const { return is_graphdef_; }

 private:
  ModelState(
      TRITONBACKEND_Model* triton_model, const std::string& name,
      ni::TritonJson::Value&& backend_config,
      ni::TritonJson::Value&& model_config,
      std::unordered_map<std::string, std::string>&& model_paths,
      const bool is_graphdef);

  TRITONBACKEND_Model* triton_model_;
  const std::string name_;
  ni::TritonJson::Value backend_config_;
  ni::TritonJson::Value model_config_;
  const std::unordered_map<std::string, std::string> model_paths_;
  const bool is_graphdef_;
};

TRITONSERVER_Error*
ModelState::Create(TRITONBACKEND_Model* triton_model, ModelState** state)
{
  // Obtain backend config as JSON object
  TRITONBACKEND_Backend* backend;
  RETURN_IF_ERROR(TRITONBACKEND_ModelBackend(triton_model, &backend));
  TRITONSERVER_Message* config_message;
  RETURN_IF_ERROR(TRITONBACKEND_BackendConfig(backend, &config_message));
  const char* buffer;
  size_t byte_size;
  RETURN_IF_ERROR(
      TRITONSERVER_MessageSerializeToJson(config_message, &buffer, &byte_size));
  ni::TritonJson::Value backend_config;
  if (byte_size != 0) {
    RETURN_IF_ERROR(backend_config.Parse(buffer, byte_size));
  }

  RETURN_IF_ERROR(TRITONBACKEND_ModelConfig(
      triton_model, 1 /* config_version */, &config_message));

  // We can get the model configuration as a json string from
  // config_message, parse it with our favorite json parser to create
  // DOM that we can access when we need to example the
  // configuration. We use TritonJson, which is a wrapper that returns
  // nice errors (currently the underlying implementation is
  // rapidjson... but others could be added). You can use any json
  // parser you prefer.
  RETURN_IF_ERROR(
      TRITONSERVER_MessageSerializeToJson(config_message, &buffer, &byte_size));

  ni::TritonJson::Value model_config;
  TRITONSERVER_Error* err = model_config.Parse(buffer, byte_size);
  RETURN_IF_ERROR(TRITONSERVER_MessageDelete(config_message));
  RETURN_IF_ERROR(err);

  const char* name;
  RETURN_IF_ERROR(TRITONBACKEND_ModelName(triton_model, &name));

  std::string platform;
  RETURN_IF_ERROR(model_config.MemberAsString("platform", &platform));
  bool is_graphdef;
  if (platform == "tensorflow_graphdef") {
    is_graphdef = true;
  } else if (platform == "tensorflow_savedmodel") {
    is_graphdef = false;
  } else {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INVALID_ARG, (std::string("platform ") + platform +
                                         " not supported for TensorFlow "
                                         "model '" +
                                         name + "'")
                                            .c_str());
  }

  const char* path = nullptr;
  TRITONBACKEND_ModelArtifactType artifact_type;
  RETURN_IF_ERROR(
      TRITONBACKEND_ModelRepository(triton_model, &artifact_type, &path));
  if (artifact_type != TRITONBACKEND_ARTIFACT_FILESYSTEM) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_UNSUPPORTED,
        (std::string("unsupported artifact type for model '") + name + "'")
            .c_str());
  }
  uint64_t version;
  RETURN_IF_ERROR(TRITONBACKEND_ModelVersion(triton_model, &version));
  std::unordered_map<std::string, std::string> model_paths;
  RETURN_IF_ERROR(
      nib::ModelPaths(path, version, is_graphdef, !is_graphdef, &model_paths));

  std::unique_ptr<ModelState> local_state(new ModelState(
      triton_model, name, std::move(backend_config), std::move(model_config),
      std::move(model_paths), is_graphdef));
  RETURN_IF_ERROR(local_state->ValidateModelConfig());

  *state = local_state.release();
  return nullptr;  // success
}

ModelState::ModelState(
    TRITONBACKEND_Model* triton_model, const std::string& name,
    ni::TritonJson::Value&& backend_config,
    ni::TritonJson::Value&& model_config,
    std::unordered_map<std::string, std::string>&& model_paths,
    const bool is_graphdef)
    : triton_model_(triton_model), name_(name),
      backend_config_(std::move(backend_config)),
      model_config_(std::move(model_config)),
      model_paths_(std::move(model_paths)), is_graphdef_(is_graphdef)
{
}

TRITONSERVER_Error*
ModelState::ValidateModelConfig()
{
  // We have the json DOM for the model configuration...
  ni::TritonJson::WriteBuffer buffer;
  RETURN_IF_ERROR(model_config_.PrettyWrite(&buffer));
  LOG_MESSAGE(
      TRITONSERVER_LOG_VERBOSE,
      (std::string("model configuration:\n") + buffer.Contents()).c_str());

  ni::TritonJson::Value ios;
  RETURN_IF_ERROR(model_config_.MemberAsArray("input", &ios));
  for (size_t i = 0; i < ios.ArraySize(); i++) {
    ni::TritonJson::Value io;
    RETURN_IF_ERROR(ios.IndexAsObject(i, &io));
    std::string io_name;
    RETURN_IF_ERROR(io.MemberAsString("name", &io_name));
    // Check datatypes
    std::string io_dtype;
    RETURN_IF_ERROR(io.MemberAsString("data_type", &io_dtype));
    RETURN_ERROR_IF_TRUE(
        nib::ConvertDataType(io_dtype) ==
            TRTISTF_DataType::TRTISTF_TYPE_INVALID,
        TRITONSERVER_ERROR_INVALID_ARG,
        std::string("unsupported datatype '") + io_dtype + "' for tensor '" +
            io_name + "' for model '" + name_ + "'");
  }
  RETURN_IF_ERROR(model_config_.MemberAsArray("output", &ios));
  for (size_t i = 0; i < ios.ArraySize(); i++) {
    ni::TritonJson::Value io;
    RETURN_IF_ERROR(ios.IndexAsObject(i, &io));
    std::string io_name;
    RETURN_IF_ERROR(io.MemberAsString("name", &io_name));
    // Check datatypes
    std::string io_dtype;
    RETURN_IF_ERROR(io.MemberAsString("data_type", &io_dtype));
    RETURN_ERROR_IF_TRUE(
        nib::ConvertDataType(io_dtype) ==
            TRTISTF_DataType::TRTISTF_TYPE_INVALID,
        TRITONSERVER_ERROR_INVALID_ARG,
        std::string("unsupported datatype '") + io_dtype + "' for tensor '" +
            io_name + "' for model '" + name_ + "'");
  }

  return nullptr;  // success
}

//
// ModelInstanceState
//
// State associated with a model instance. An object of this class is
// created and associated with each TRITONBACKEND_ModelInstance.
//
class ModelInstanceState : public nib::ModelInstance {
 public:
  // GPU device number that indicates model will be loaded on GPUs
  // as specified in model graph
  static constexpr int MODEL_DEVICE = -2;

  void ProcessRequests(
      TRITONBACKEND_Request** requests, const uint32_t request_count);

  static TRITONSERVER_Error* Create(
      ModelState* model_state,
      TRITONBACKEND_ModelInstance* triton_model_instance,
      ModelInstanceState** state);

  // Get the name, device ID of the instance.
  const std::string& Name() const { return name_; }
  int32_t DeviceId() const { return gpu_device_; }

  // Get the state of the model that corresponds to this instance.
  ModelState* StateForModel() const { return model_state_; }

 private:
  ModelInstanceState(
      ModelState* model_state,
      TRITONBACKEND_ModelInstance* triton_model_instance,
      const std::string& name, const int gpu_device, const int max_batch_size,
      const bool enable_pinned_input, const bool enable_pinned_output)
      : nib::ModelInstance(
            name, gpu_device, max_batch_size, enable_pinned_input,
            enable_pinned_output),
        model_state_(model_state),
        triton_model_instance_(triton_model_instance),
        trtistf_model_(nullptr, TRTISTF_ModelDelete),
        input_device_id_(MODEL_DEVICE)
  {
  }

  ModelState* model_state_;
  TRITONBACKEND_ModelInstance* triton_model_instance_;

  // Map from configuration name for an input to tensor name for
  // that input in the model.
  IONameMap input_name_map_;

  // Map from configuration name for an output to tensor name for
  // that output in the model.
  IONameMap output_name_map_;

  // TRTISTFModel for this context.
  TRTISTFModelHandle trtistf_model_;

  // use for GPU allocator
  int input_device_id_;
};

TRITONSERVER_Error*
ModelInstanceState::Create(
    ModelState* model_state, TRITONBACKEND_ModelInstance* triton_model_instance,
    ModelInstanceState** state)
{
  *state = nullptr;

  const char* instance_name;
  TRITONSERVER_InstanceGroupKind kind;
  int32_t device_id;
  RETURN_IF_ERROR(
      TRITONBACKEND_ModelInstanceName(triton_model_instance, &instance_name));
  RETURN_IF_ERROR(
      TRITONBACKEND_ModelInstanceKind(triton_model_instance, &kind));
  RETURN_IF_ERROR(
      TRITONBACKEND_ModelInstanceDeviceId(triton_model_instance, &device_id));

  auto& model_config = model_state->ModelConfig();
  // For a GPU context, determine the model file to use for device
  // compute capability. CPU always uses the default model file.
  std::string cc_model_filename;
  model_config.MemberAsString("default_model_filename", &cc_model_filename);
  // FIXME this should be part of model config normalization / autofill
  if (cc_model_filename.empty()) {
    if (model_state->IsGraphdef()) {
      cc_model_filename = "model.graphdef";
    } else {
      cc_model_filename = "model.savedmodel";
    }
  }
  int gpu_device;

  switch (kind) {
    case TRITONSERVER_INSTANCEGROUPKIND_CPU: {
      LOG_MESSAGE(
          TRITONSERVER_LOG_VERBOSE,
          (std::string("Creating instance ") + instance_name +
           " on CPU using " + cc_model_filename)
              .c_str());
      gpu_device = ModelInstanceState::NO_GPU_DEVICE;
      break;
    }
    case TRITONSERVER_INSTANCEGROUPKIND_MODEL: {
      LOG_MESSAGE(
          TRITONSERVER_LOG_VERBOSE,
          (std::string("Creating instance ") + instance_name +
           " on devices using " + cc_model_filename)
              .c_str());
      gpu_device = ModelInstanceState::MODEL_DEVICE;
      break;
    }
    default: {
#ifdef TRITON_ENABLE_GPU
      cudaDeviceProp cuprops;
      cudaError_t cuerr = cudaGetDeviceProperties(&cuprops, device_id);
      if (cuerr != cudaSuccess) {
        RETURN_ERROR_IF_FALSE(
            false, TRITONSERVER_ERROR_INTERNAL,
            std::string("unable to get CUDA device properties for ") +
                instance_name + ": " + cudaGetErrorString(cuerr));
      }

      const std::string cc =
          std::to_string(cuprops.major) + "." + std::to_string(cuprops.minor);
      ni::TritonJson::Value cc_names;
      ni::TritonJson::Value cc_name;
      if ((model_config.Find("cc_model_filenames", &cc_names)) &&
          (cc_names.Find(cc.c_str(), &cc_name))) {
        cc_name.AsString(&cc_model_filename);
      }

      gpu_device = device_id;
      LOG_MESSAGE(
          TRITONSERVER_LOG_VERBOSE,
          (std::string("Creating instance ") + instance_name + " on GPU " +
           std::to_string(gpu_device) + " (" + cc + ") using " +
           cc_model_filename)
              .c_str());
#else
      RETURN_ERROR_IF_FALSE(
          false, TRITONSERVER_ERROR_INTERNAL,
          std::string("GPU instances not supported"));
#endif  // TRITON_ENABLE_GPU
      break;
    }
  }

  const auto& gdp_itr = model_state->ModelPaths().find(cc_model_filename);
  if (gdp_itr == model_state->ModelPaths().end()) {
    RETURN_ERROR_IF_FALSE(
        false, TRITONSERVER_ERROR_INTERNAL,
        (std::string("unable to find model '") + cc_model_filename + "' for " +
         instance_name));
  }

  // Max batch size. A value of 0 in the config becomes NO_BATCHING.
  int64_t max_batch_size;
  RETURN_IF_ERROR(model_config.MemberAsInt("max_batch_size", &max_batch_size));
  const int mbs =
      (max_batch_size <= 0) ? ModelInstanceState::NO_BATCHING : max_batch_size;

  // TODO put the model config related code as backend_utils
  bool pinned_input, pinned_output;
  {
    ni::TritonJson::Value optimization;
    if (model_config.Find("optimization", &optimization)) {
      ni::TritonJson::Value pinned_memory;
      if (model_config.Find("input_pinned_memory", &pinned_memory)) {
        RETURN_IF_ERROR(pinned_memory.MemberAsBool("enable", &pinned_input));
      }
      if (model_config.Find("output_pinned_memory", &pinned_memory)) {
        RETURN_IF_ERROR(pinned_memory.MemberAsBool("enable", &pinned_output));
      }
    }
  }

  std::unique_ptr<ModelInstanceState> lstate(new ModelInstanceState(
      model_state, triton_model_instance, instance_name, gpu_device, mbs,
      pinned_input, pinned_output));
  auto instance = lstate.get();

  RETURN_IF_ERROR(instance->CreateCudaStream());

  TRTISTF_TFTRTConfig* tftrt_config_ptr = nullptr;
  TRTISTF_TFTRTConfig tftrt_config;
  bool auto_mixed_precision = false;
  bool has_graph_level = false;
  int64_t graph_level = 0;
  // [TODO] this can be moved one level above
  {
    ni::TritonJson::Value optimization;
    if (model_config.Find("optimization", &optimization)) {
      {
        ni::TritonJson::Value graph;
        if ((has_graph_level = optimization.Find("graph", &graph))) {
          RETURN_IF_ERROR(graph.MemberAsInt("level", &graph_level));
        }
      }
      ni::TritonJson::Value eas;
      if (optimization.Find("execution_accelerators", &eas)) {
        // Set default values. is_dynamic_op is always true for online
        // TF-TRT.
        tftrt_config.minimum_segment_size_ = 3;
        tftrt_config.max_workspace_size_bytes_ = 1 << 30;
        tftrt_config.max_cached_engines_ = 100;
        tftrt_config.max_batch_size_ = std::max(mbs, 1);
        tftrt_config.precision_mode_ = TRTISTF_MODE_FP32;
        tftrt_config.is_dynamic_op_ = true;

        ni::TritonJson::Value cpu_eas;
        RETURN_ERROR_IF_TRUE(
            eas.Find("cpu_execution_accelerator", &cpu_eas) &&
                (cpu_eas.ArraySize() != 0),
            TRITONSERVER_ERROR_INVALID_ARG,
            std::string("CPU Execution Accelerator is not supported in "
                        "TensorFlow backend"));

        RETURN_ERROR_IF_TRUE(
            gpu_device == ModelInstanceState::NO_GPU_DEVICE,
            TRITONSERVER_ERROR_INVALID_ARG,
            std::string(
                "GPU Execution Accelerator can only be set on non-CPU backend "
                "context"));

        ni::TritonJson::Value gpu_eas;
        if (eas.Find("gpu_execution_accelerator", &gpu_eas)) {
          for (size_t ea_idx = 0; ea_idx < gpu_eas.ArraySize(); ea_idx++) {
            ni::TritonJson::Value ea;
            RETURN_IF_ERROR(gpu_eas.IndexAsObject(ea_idx, &ea));
            std::string name;
            RETURN_IF_ERROR(ea.MemberAsString("name", &name));
            if (name == nib::kTensorRTExecutionAccelerator) {
              // Validate and set parameters
              ni::TritonJson::Value params;
              if (ea.Find("parameters", &params)) {
                std::vector<std::string> param_keys;
                RETURN_IF_ERROR(params.Members(&param_keys));
                for (const auto& param_key : param_keys) {
                  std::string value_string;
                  if (param_key == "precision_mode") {
                    RETURN_IF_ERROR(params.MemberAsString(
                        param_key.c_str(), &value_string));
                    if (value_string == "FP32") {
                      tftrt_config.precision_mode_ = TRTISTF_MODE_FP32;
                    } else if (value_string == "FP16") {
                      tftrt_config.precision_mode_ = TRTISTF_MODE_FP16;
                    } else {
                      RETURN_ERROR_IF_FALSE(
                          false, TRITONSERVER_ERROR_INVALID_ARG,
                          std::string("unsupported precision mode '") +
                              value_string + "' is requested");
                    }
                  } else if (param_key == "minimum_segment_size") {
                    RETURN_IF_ERROR(params.MemberAsString(
                        param_key.c_str(), &value_string));
                    RETURN_IF_ERROR(ParseLongLongParameter(
                        "minimum_segment_size", value_string,
                        &tftrt_config.minimum_segment_size_));
                  } else if (param_key == "max_workspace_size_bytes") {
                    RETURN_IF_ERROR(params.MemberAsString(
                        param_key.c_str(), &value_string));
                    RETURN_IF_ERROR(ParseLongLongParameter(
                        "max_workspace_size_bytes", value_string,
                        &tftrt_config.max_workspace_size_bytes_));
                  } else if (param_key == "max_cached_engines") {
                    RETURN_IF_ERROR(params.MemberAsString(
                        param_key.c_str(), &value_string));
                    RETURN_IF_ERROR(ParseLongLongParameter(
                        "max_cached_engines", value_string,
                        &tftrt_config.max_cached_engines_));
                  } else {
                    return TRITONSERVER_ErrorNew(
                        TRITONSERVER_ERROR_INVALID_ARG,
                        std::string(
                            "unknown parameter '" + param_key +
                            "' is provided for TensorRT Execution Accelerator")
                            .c_str());
                  }
                }
              }
              tftrt_config_ptr = &tftrt_config;
              LOG_MESSAGE(
                  TRITONSERVER_LOG_VERBOSE,
                  (std::string("TensorRT Execution Accelerator is set for ") +
                   instance_name)
                      .c_str());
            } else if (name == nib::kGPUIOExecutionAccelerator) {
              // GPU I/O can be set, set hint
              if ((gpu_device != ModelInstanceState::NO_GPU_DEVICE) &&
                  (gpu_device != ModelInstanceState::MODEL_DEVICE)) {
                instance->input_device_id_ = gpu_device;
              }
            } else if (name == nib::kAutoMixedPrecisionExecutionAccelerator) {
              auto_mixed_precision = true;
            } else {
              return TRITONSERVER_ErrorNew(
                  TRITONSERVER_ERROR_INVALID_ARG,
                  (std::string("unknown Execution Accelerator '") + name +
                   "' is requested")
                      .c_str());
            }
          }
        }
      }
    }
  }

  if (auto_mixed_precision && (tftrt_config_ptr != nullptr)) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INVALID_ARG,
        "Auto mixed precision can not be set with TFTRT optimization");
  }

  if (model_state->IsGraphdef()) {
    // TODO better if passing instance name?
    RETURN_IF_ERROR(GraphDef::CreateTRTISTFModel(
        model_state->BackendConfig(), model_config, gpu_device, has_graph_level,
        graph_level, model_state->Name(), gdp_itr->second,
        &instance->trtistf_model_, &instance->input_name_map_,
        &instance->output_name_map_, tftrt_config_ptr, auto_mixed_precision));
  } else {
    RETURN_IF_ERROR(SavedModel::CreateTRTISTFModel(
        model_state->BackendConfig(), model_config, gpu_device, has_graph_level,
        graph_level, model_state->Name(), gdp_itr->second,
        &instance->trtistf_model_, &instance->input_name_map_,
        &instance->output_name_map_, tftrt_config_ptr, auto_mixed_precision));
  }

  if (instance->input_device_id_ != ModelInstanceState::MODEL_DEVICE) {
    std::vector<const char*> input_names, output_names;
    std::vector<TRTISTF_DataType> input_types, output_types;
    std::deque<std::string> io_names;

    ni::TritonJson::Value config_inputs;
    RETURN_IF_ERROR(model_config.MemberAsArray("input", &config_inputs));
    for (size_t i = 0; i < config_inputs.ArraySize(); i++) {
      ni::TritonJson::Value io;
      RETURN_IF_ERROR(config_inputs.IndexAsObject(i, &io));
      io_names.emplace_back();
      RETURN_IF_ERROR(io.MemberAsString("name", &io_names.back()));
      std::string io_data_type;
      RETURN_IF_ERROR(io.MemberAsString("data_type", &io_data_type));

      input_names.push_back(io_names.back().c_str());
      input_types.push_back(nib::ConvertDataType(io_data_type));
    }

    ni::TritonJson::Value config_outputs;
    RETURN_IF_ERROR(model_config.MemberAsArray("output", &config_outputs));
    for (size_t i = 0; i < config_outputs.ArraySize(); i++) {
      ni::TritonJson::Value io;
      RETURN_IF_ERROR(config_outputs.IndexAsObject(i, &io));
      io_names.emplace_back();
      RETURN_IF_ERROR(io.MemberAsString("name", &io_names.back()));
      std::string io_data_type;
      RETURN_IF_ERROR(io.MemberAsString("data_type", &io_data_type));

      output_names.push_back(io_names.back().c_str());
      output_types.push_back(nib::ConvertDataType(io_data_type));
    }
    RETURN_IF_TRTISTF_ERROR(TRTISTF_ModelMakeCallable(
        instance->trtistf_model_.get(), input_names.data(), input_types.data(),
        config_inputs.ArraySize(), output_names.data(), output_types.data(),
        config_outputs.ArraySize()));
  }

  *state = lstate.release();
  return nullptr;  // success
}

void
ModelInstanceState::ProcessRequests(
    TRITONBACKEND_Request** requests, const uint32_t request_count)
{
  LOG_MESSAGE(
      TRITONSERVER_LOG_VERBOSE,
      (std::string("TRITONBACKEND_ModelExecute: Running ") + name_ + " with " +
       std::to_string(request_count) + " requests")
          .c_str());

  uint64_t exec_start_ns = 0;
  SET_TIMESTAMP(exec_start_ns);

  // For each request collect the total batch size for this inference
  // execution. The batch-size, number of inputs, and size of each
  // input has already been checked so don't need to do that here.
  size_t total_batch_size = 0;
  for (size_t i = 0; i < request_count; i++) {
    // If we get a nullptr request then something is badly wrong. Fail
    // and release all requests.
    if (requests[i] == nullptr) {
      RequestsRespondIfError(
          requests, request_count,
          TRITONSERVER_ErrorNew(
              TRITONSERVER_ERROR_INTERNAL,
              std::string(
                  "null request given to TensorFlow runner for '" + name_ + "'")
                  .c_str()));
      return;
    }

    if (max_batch_size_ > 0) {
      // Retrieve the batch size from one of the inputs,
      // if the model support batching, the first dimension size is batch size
      const char* name;
      auto err = TRITONBACKEND_RequestInputName(requests[i], 0, &name);
      TRITONBACKEND_Input* input;
      if (err == nullptr) {
        err = TRITONBACKEND_RequestInput(requests[i], name, &input);
      }
      if (err == nullptr) {
        const int64_t* shape;
        err = TRITONBACKEND_InputProperties(
            input, nullptr, nullptr, &shape, nullptr, nullptr, nullptr);
        total_batch_size += shape[0];
      }
      if (err != nullptr) {
        RequestsRespondIfError(requests, request_count, err);
        return;
      }
    } else {
      total_batch_size += 1;
    }
  }

  // If there are no valid requests then no need to run the
  // inference. This should never happen unless called with an empty
  // 'requests' for some reason.
  if (total_batch_size == 0) {
    return;
  }

  // Make sure the maximum batch size is not exceeded. The
  // total_batch_size must be 1 for models that don't support batching
  // (i.e. max_batch_size_ == 0). If max_batch_size is exceeded then
  // scheduler has done something badly wrong so fail and release all
  // requests.
  if ((total_batch_size != 1) && (total_batch_size > (size_t)max_batch_size_)) {
    RequestsRespondIfError(
        requests, request_count,
        TRITONSERVER_ErrorNew(
            TRITONSERVER_ERROR_INTERNAL,
            std::string(
                "dynamic batch size " + std::to_string(total_batch_size) +
                " for '" + name_ + "', max allowed is " +
                std::to_string(max_batch_size_))
                .c_str()));
    return;
  }

  // At this point we are committed to running inference with all
  // 'requests'. Create a response for each request. During input
  // processing if there is an error with any request that error will
  // be sent immediately with the corresponding response (and the
  // response pointer will then be nullptr). The request object
  // itself will not be released until after all inferencing is done
  // (below) as we may need to access the request object when
  // determine how to process outputs (for example, even if we don't
  // need the outputs for a request that has an error, we do need to
  // know the size of those outputs associated with the request so we
  // can skip them in the output tensors).
  std::vector<TRITONBACKEND_Response*> responses;
  responses.reserve(request_count);

  for (size_t i = 0; i < request_count; i++) {
    TRITONBACKEND_Response* response;
    auto err = TRITONBACKEND_ResponseNew(&response, requests[i]);
    if (err == nullptr) {
      responses.emplace_back(response);
    } else {
      responses.emplace_back(nullptr);
      LOG_MESSAGE(TRITONSERVER_LOG_ERROR, "Fail to create response");
      TRITONSERVER_ErrorDelete(err);
    }
  }

  // Create a tensor for each input sized correctly for the total
  // batch size. Concatenate input values from each request into the
  // corresponding tensor.

  // Unique pointer is TensorList** as the pointer to input head
  // (TensorList*) will be updated in SetInput()
  TRTISTF_TensorList* input_head_ptr = nullptr;
  static auto input_deleter = [](TRTISTF_TensorList** list) {
    if (list != nullptr) {
      TRTISTF_TensorListDelete(*list);
    }
  };
  std::unique_ptr<TRTISTF_TensorList*, decltype(input_deleter)> input_tensors(
      &input_head_ptr, input_deleter);

  // Collect the request inputs into contiguous input tensors. For
  // tensors with string data type we must handle ourselves since we
  // must use TF-specific string tensor APIs.
  bool cuda_copy = false;

  nib::ModelInputCollector collector(
      requests, request_count, &responses, enable_pinned_input_, stream_);
  {
    // All requests must have equally-sized input tensors so use the first
    // request as the representative for the input tensors.
    uint32_t input_count;
    TRITONBACKEND_RequestInputCount(requests[0], &input_count);
    for (uint32_t input_idx = 0; input_idx < input_count; input_idx++) {
      const char* name;
      TRITONBACKEND_RequestInputName(requests[0], input_idx, &name);
      TRITONBACKEND_Input* input;
      TRITONBACKEND_RequestInput(requests[0], name, &input);
      TRITONSERVER_DataType datatype;
      const int64_t* shape;
      uint32_t dims_count;
      uint32_t buffer_count;
      TRITONBACKEND_InputProperties(
          input, nullptr, &datatype, &shape, &dims_count, nullptr,
          &buffer_count);

      // The shape for the entire input patch, [total_batch_size, ...]
      std::vector<int64_t> batchn_shape(shape, shape + dims_count);
      if (max_batch_size_ != NO_BATCHING) {
        batchn_shape[0] = total_batch_size;
      }

      // The name of the input in the model can be different...
      const char* input_tensor_name = name;
      const auto& tn_itr = input_name_map_.find(input_tensor_name);
      if (tn_itr != input_name_map_.end()) {
        input_tensor_name = tn_itr->second.c_str();
      }

      // Create a TF tensor to hold the entire input batch. Only try
      // to create a tensor on a specific device if 'input_device_id_'
      // is set. If unable to create the tensor then fail all
      // requests.
      TRTISTF_Tensor* tensor = TRTISTF_TensorNew(
          input_tensor_name, nib::ConvertDataType(datatype),
          batchn_shape.size(),
          (batchn_shape.size() == 0) ? nullptr : &batchn_shape[0],
          input_device_id_);
      if (tensor == nullptr) {
        auto err = TRITONSERVER_ErrorNew(
            TRITONSERVER_ERROR_INTERNAL,
            (std::string("failed to create input tensor '") + name +
             "' with shape " + nib::ShapeToString(batchn_shape) +
             " and data type " + TRITONSERVER_DataTypeString(datatype) +
             " for '" + name_ + "'")
                .c_str());
        // Send remaining responses and returned
        for (uint32_t r = 0; r < request_count; ++r) {
          if (responses[r] != nullptr) {
            LOG_IF_ERROR(
                TRITONBACKEND_ResponseSend(
                    responses[r], TRITONSERVER_RESPONSE_COMPLETE_FINAL, err),
                "failed to send TensorFlow backend response");
          }

          LOG_IF_ERROR(
              TRITONBACKEND_RequestRelease(
                  requests[r], TRITONSERVER_REQUEST_RELEASE_ALL),
              "failed releasing request");
        }
        TRITONSERVER_ErrorDelete(err);
        return;
      }

      // Add the new TF tensor to the list of TF inputs.
      TRTISTF_TensorList* tlink = TRTISTF_TensorListNew(tensor, *input_tensors);
      *input_tensors = tlink;

      // Custom handling for string/bytes tensor...
      if (datatype == TRITONSERVER_TYPE_BYTES) {
        size_t tensor_offset = 0;

        for (size_t idx = 0; idx < request_count; idx++) {
          TRITONBACKEND_Input* input;
          RESPOND_AND_SET_NULL_IF_ERROR(
              &responses[idx],
              TRITONBACKEND_RequestInput(requests[idx], name, &input));
          const int64_t* shape;
          uint32_t dims_count;
          uint32_t buffer_count;
          RESPOND_AND_SET_NULL_IF_ERROR(
              &responses[idx], TRITONBACKEND_InputProperties(
                                   input, nullptr, nullptr, &shape, &dims_count,
                                   nullptr, &buffer_count));

          const int64_t batch_element_cnt =
              nib::GetElementCount(shape, dims_count);

          cuda_copy |= SetStringInputTensor(
              tensor, input, name, buffer_count, batch_element_cnt,
              tensor_offset, &responses[idx], stream_);
          tensor_offset += batch_element_cnt;
        }
      }
      // Use the collector for non-STRING datatype...
      else {  // datatype != DataType::TYPE_STRING
        collector.ProcessTensor(
            name, TRTISTF_TensorData(tensor),
            TRTISTF_TensorDataByteSize(tensor),
            (TRTISTF_TensorIsGPUTensor(tensor)) ? TRITONSERVER_MEMORY_GPU
                                                : TRITONSERVER_MEMORY_CPU,
            (TRTISTF_TensorIsGPUTensor(tensor)) ? gpu_device_ : 0);
      }

      LOG_MESSAGE(
          TRITONSERVER_LOG_VERBOSE,
          (std::string("TRITONBACKEND_ModelExecute: input '") + name +
           "' is GPU tensor: " +
           std::to_string(TRTISTF_TensorIsGPUTensor(tensor)))
              .c_str());
    }

    // Finalize...
    cuda_copy |= collector.Finalize();
  }

  // Collect the names of requested outputs. Do not include outputs
  // for requests that have already responded with an error.
  std::set<std::string> required_outputs;
  std::vector<std::set<std::string>> request_required_outputs(request_count);
  for (size_t idx = 0; idx < request_count; idx++) {
    const auto& request = requests[idx];
    auto& response = responses[idx];
    if (response != nullptr) {
      uint32_t output_count;
      RESPOND_AND_SET_NULL_IF_ERROR(
          &response, TRITONBACKEND_RequestOutputCount(request, &output_count));
      if (response != nullptr) {
        for (uint32_t output_idx = 0; output_idx < output_count; output_idx++) {
          const char* output_name;
          RESPOND_AND_SET_NULL_IF_ERROR(
              &response, TRITONBACKEND_RequestOutputName(
                             request, output_idx, &output_name));
          if (response != nullptr) {
            required_outputs.insert(output_name);
            request_required_outputs[idx].insert(output_name);
          }
        }
      }
    }
  }

  // Create the vector of required output names using the names
  // expected by the model.
  std::vector<std::string> model_output_names;
  const char* output_names_cstr[required_outputs.size()];
  {
    size_t oidx = 0;
    for (const auto& name : required_outputs) {
      model_output_names.push_back(name);
      const auto& tn_itr = output_name_map_.find(name);
      if (tn_itr == output_name_map_.end()) {
        output_names_cstr[oidx] = name.c_str();
      } else {
        output_names_cstr[oidx] = tn_itr->second.c_str();
      }
      oidx++;
    }
  }

  // Wait for any in-flight input tensor copies to complete.
#ifdef TRITON_ENABLE_GPU
  if (cuda_copy) {
    cudaStreamSynchronize(stream_);
  }
#endif

  uint64_t compute_start_ns = 0;
  SET_TIMESTAMP(compute_start_ns);

  // Run. Session will update the 'output_tensors'.
  std::unique_ptr<TRTISTF_TensorList, decltype(&TRTISTF_TensorListDelete)>
      output_tensors(nullptr, TRTISTF_TensorListDelete);

  {
    TRTISTF_TensorList* rtl = nullptr;

    TRTISTF_Error* tf_err = TRTISTF_ModelRun(
        trtistf_model_.get(), *(input_tensors.release()),
        required_outputs.size(), output_names_cstr, &rtl);
    if (tf_err != nullptr) {
      auto err =
          TRITONSERVER_ErrorNew(TRITONSERVER_ERROR_INTERNAL, tf_err->msg_);
      TRTISTF_ErrorDelete(tf_err);
      // Send remaining responses and returned
      for (uint32_t r = 0; r < request_count; ++r) {
        if (responses[r] != nullptr) {
          LOG_IF_ERROR(
              TRITONBACKEND_ResponseSend(
                  responses[r], TRITONSERVER_RESPONSE_COMPLETE_FINAL, err),
              "failed to send TensorFlow backend response");
        }

        LOG_IF_ERROR(
            TRITONBACKEND_RequestRelease(
                requests[r], TRITONSERVER_REQUEST_RELEASE_ALL),
            "failed releasing request");
      }
      TRITONSERVER_ErrorDelete(err);
      return;
    }

    output_tensors.reset(rtl);
  }

  uint64_t compute_end_ns = 0;
  SET_TIMESTAMP(compute_end_ns);

  // Create the response tensors and copy the appropriate tensor data
  // into each. For tensors with string data type we must handle
  // ourselves since we must use TF-specific string tensor APIs.
  cuda_copy = false;
  // The serialized string buffer must be valid until output copies are done
  std::vector<std::unique_ptr<std::string>> string_buffer;
  nib::ModelResponder responder(
      requests, request_count, &responses, max_batch_size_,
      enable_pinned_output_, stream_);
  {
    TRTISTF_TensorList* output_tensor_itr = output_tensors.get();
    for (const auto& name : model_output_names) {
      TRTISTF_Tensor* output_tensor = output_tensor_itr->tensor_;

      TRTISTF_DataType tf_datatype = TRTISTF_TensorDataType(output_tensor);
      TRTISTF_Shape* tf_shape = TRTISTF_TensorShape(output_tensor);

      const TRITONSERVER_DataType datatype = nib::ConvertDataType(tf_datatype);

      // batchn_shape holds the shape of the entire tensor batch, but
      // is overwritten below and used as the shape for each response
      // output.
      std::vector<int64_t> batchn_shape;
      batchn_shape.reserve(tf_shape->rank_);
      for (size_t itr = 0; itr < tf_shape->rank_; itr++) {
        const int64_t dim = tf_shape->dims_[itr];
        batchn_shape.push_back(dim);
      }

      // Custom handling for string/bytes tensor...
      if (datatype == TRITONSERVER_TYPE_BYTES) {
        size_t tensor_offset = 0;

        for (size_t idx = 0; idx < responses.size(); idx++) {
          auto& request = requests[idx];
          auto& response = responses[idx];

          if (max_batch_size_ != NO_BATCHING) {
            // [TODO] remember some input properties on the first call
            const char* name;
            TRITONBACKEND_RequestInputName(request, 0, &name);
            TRITONBACKEND_Input* input;
            TRITONBACKEND_RequestInput(request, name, &input);
            const int64_t* shape;
            TRITONBACKEND_InputProperties(
                input, nullptr, nullptr, &shape, nullptr, nullptr, nullptr);
            batchn_shape[0] = shape[0];
          }

          const size_t tensor_element_cnt = nib::GetElementCount(batchn_shape);

          // Only need an response tensor for requested outputs.
          if ((response != nullptr) &&
              (request_required_outputs[idx].find(name) !=
               request_required_outputs[idx].end())) {
            TRITONBACKEND_Output* response_output;
            RESPOND_AND_SET_NULL_IF_ERROR(
                &response,
                TRITONBACKEND_ResponseOutput(
                    response, &response_output, name.c_str(), datatype,
                    batchn_shape.data(), batchn_shape.size()));
            string_buffer.emplace_back(new std::string());
            cuda_copy |= SetStringOutputBuffer(
                output_tensor, &response, response_output, tensor_element_cnt,
                tensor_offset, stream_, string_buffer.back().get());
          }

          tensor_offset += tensor_element_cnt;
        }
      }
      // Use the responder for non-STRING datatype...
      else {  // datatype != DataType::TYPE_STRING
        responder.ProcessTensor(
            name, datatype, batchn_shape, TRTISTF_TensorData(output_tensor),
            (TRTISTF_TensorIsGPUTensor(output_tensor))
                ? TRITONSERVER_MEMORY_GPU
                : TRITONSERVER_MEMORY_CPU,
            (TRTISTF_TensorIsGPUTensor(output_tensor)) ? gpu_device_ : 0);
      }

      LOG_MESSAGE(
          TRITONSERVER_LOG_VERBOSE,
          (std::string("TRITONBACKEND_ModelExecute: output '") + name +
           "' is GPU tensor: " +
           std::to_string(TRTISTF_TensorIsGPUTensor(output_tensor)))
              .c_str());

      output_tensor_itr = output_tensor_itr->next_;
    }

    // Finalize and wait for any pending buffer copies.
    cuda_copy |= responder.Finalize();
  }

#ifdef TRITON_ENABLE_GPU
  if (cuda_copy) {
    cudaStreamSynchronize(stream_);
  }
#endif  // TRITON_ENABLE_GPU

  uint64_t exec_end_ns = 0;
  SET_TIMESTAMP(exec_end_ns);

  // Send all the responses that haven't already been sent because of
  // an earlier error. Note that the responses are not set to nullptr
  // for later checking.
  for (auto& response : responses) {
    if (response != nullptr) {
      LOG_IF_ERROR(
          TRITONBACKEND_ResponseSend(
              response, TRITONSERVER_RESPONSE_COMPLETE_FINAL, nullptr),
          "failed to send TensorFlow backend response");
    }
  }

  for (uint32_t r = 0; r < request_count; ++r) {
    auto& request = requests[r];
    // Report statistics for the request. Note that there could
    // still be responses that have not yet been sent but those
    // cannot be captured in the statistics as they reflect only the
    // request object. We use the execution start/end time for
    // compute also so that the entire execution time is associated
    // with the inference computation.
    LOG_IF_ERROR(
        TRITONBACKEND_ModelInstanceReportStatistics(
            triton_model_instance_, request,
            (responses[r] != nullptr) /* success */, exec_start_ns,
            compute_start_ns, compute_end_ns, exec_end_ns),
        "failed reporting request statistics");

    LOG_IF_ERROR(
        TRITONBACKEND_RequestRelease(request, TRITONSERVER_REQUEST_RELEASE_ALL),
        "failed releasing request");
  }

  // Report the entire batch statistics. This backend does not support
  // batching so the total batch size is always 1.
  LOG_IF_ERROR(
      TRITONBACKEND_ModelInstanceReportBatchStatistics(
          triton_model_instance_, total_batch_size, exec_start_ns,
          compute_start_ns, compute_end_ns, exec_end_ns),
      "failed reporting batch request statistics");

  LOG_MESSAGE(
      TRITONSERVER_LOG_VERBOSE,
      (std::string("TRITONBACKEND_ModelExecute: model ") + name_ +
       " released " + std::to_string(request_count) + " requests")
          .c_str());
}

}  // namespace

/////////////

extern "C" {

// Implementing TRITONBACKEND_Initialize is optional. The backend
// should initialize any global state that is intended to be shared
// across all models and model instances that use the backend. But here
// it simply verify the backend API version is compatible
TRITONSERVER_Error*
TRITONBACKEND_Initialize(TRITONBACKEND_Backend* backend)
{
  const char* cname;
  RETURN_IF_ERROR(TRITONBACKEND_BackendName(backend, &cname));
  std::string name(cname);

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("TRITONBACKEND_Initialize: ") + name).c_str());

  // We should check the backend API version that Triton supports
  // vs. what this backend was compiled against.
  uint32_t api_version_major, api_version_minor;
  RETURN_IF_ERROR(
      TRITONBACKEND_ApiVersion(&api_version_major, &api_version_minor));

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("Triton TRITONBACKEND API version: ") +
       std::to_string(api_version_major) + "." +
       std::to_string(api_version_minor))
          .c_str());
  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("'") + name + "' TRITONBACKEND API version: " +
       std::to_string(TRITONBACKEND_API_VERSION_MAJOR) + "." +
       std::to_string(TRITONBACKEND_API_VERSION_MINOR))
          .c_str());

  if ((api_version_major != TRITONBACKEND_API_VERSION_MAJOR) ||
      (api_version_minor < TRITONBACKEND_API_VERSION_MINOR)) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_UNSUPPORTED,
        "triton backend API version does not support this backend");
  }

  return nullptr;  // success
}

// Implementing TRITONBACKEND_ModelInitialize is optional. The backend
// should initialize any state that is intended to be shared across
// all instances of the model.
TRITONSERVER_Error*
TRITONBACKEND_ModelInitialize(TRITONBACKEND_Model* model)
{
  const char* cname;
  RETURN_IF_ERROR(TRITONBACKEND_ModelName(model, &cname));
  std::string name(cname);

  uint64_t version;
  RETURN_IF_ERROR(TRITONBACKEND_ModelVersion(model, &version));

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("TRITONBACKEND_ModelInitialize: ") + name + " (version " +
       std::to_string(version) + ")")
          .c_str());

  // With each model we create a ModelState object and associate it
  // with the TRITONBACKEND_Model.
  ModelState* model_state;
  RETURN_IF_ERROR(ModelState::Create(model, &model_state));
  RETURN_IF_ERROR(
      TRITONBACKEND_ModelSetState(model, reinterpret_cast<void*>(model_state)));

  return nullptr;  // success
}

// Implementing TRITONBACKEND_ModelFinalize is optional unless state
// is set using TRITONBACKEND_ModelSetState. The backend must free
// this state and perform any other cleanup.
TRITONSERVER_Error*
TRITONBACKEND_ModelFinalize(TRITONBACKEND_Model* model)
{
  void* vstate;
  RETURN_IF_ERROR(TRITONBACKEND_ModelState(model, &vstate));
  ModelState* model_state = reinterpret_cast<ModelState*>(vstate);

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO, "TRITONBACKEND_ModelFinalize: delete model state");

  delete model_state;

  return nullptr;  // success
}

// Implementing TRITONBACKEND_ModelInstanceInitialize is optional. The
// backend should initialize any state that is required for a model
// instance.
TRITONSERVER_Error*
TRITONBACKEND_ModelInstanceInitialize(TRITONBACKEND_ModelInstance* instance)
{
  const char* cname;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceName(instance, &cname));
  std::string name(cname);

  int32_t device_id;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceDeviceId(instance, &device_id));

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("TRITONBACKEND_ModelInstanceInitialize: ") + name +
       " (device " + std::to_string(device_id) + ")")
          .c_str());

  // The instance can access the corresponding model as well... here
  // we get the model and from that get the model's state.
  TRITONBACKEND_Model* model;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceModel(instance, &model));

  void* vmodelstate;
  RETURN_IF_ERROR(TRITONBACKEND_ModelState(model, &vmodelstate));
  ModelState* model_state = reinterpret_cast<ModelState*>(vmodelstate);

  // With each instance we create a ModelInstanceState object and
  // associate it with the TRITONBACKEND_ModelInstance.
  ModelInstanceState* instance_state;
  RETURN_IF_ERROR(
      ModelInstanceState::Create(model_state, instance, &instance_state));
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceSetState(
      instance, reinterpret_cast<void*>(instance_state)));

  return nullptr;  // success
}

// Implementing TRITONBACKEND_ModelInstanceFinalize is optional unless
// state is set using TRITONBACKEND_ModelInstanceSetState. The backend
// must free this state and perform any other cleanup.
TRITONSERVER_Error*
TRITONBACKEND_ModelInstanceFinalize(TRITONBACKEND_ModelInstance* instance)
{
  void* vstate;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceState(instance, &vstate));
  ModelInstanceState* instance_state =
      reinterpret_cast<ModelInstanceState*>(vstate);

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      "TRITONBACKEND_ModelInstanceFinalize: delete instance state");

  delete instance_state;

  return nullptr;  // success
}

// Implementing TRITONBACKEND_ModelInstanceExecute is required.
TRITONSERVER_Error*
TRITONBACKEND_ModelInstanceExecute(
    TRITONBACKEND_ModelInstance* instance, TRITONBACKEND_Request** requests,
    const uint32_t request_count)
{
  // Triton will not call this function simultaneously for the same
  // 'instance'. But since this backend could be used by multiple
  // instances from multiple models the implementation needs to handle
  // multiple calls to this function at the same time (with different
  // 'instance' objects). Suggested practice for this is to use only
  // function-local and model-instance-specific state (obtained from
  // 'instance'), which is what we do here.
  ModelInstanceState* instance_state;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceState(
      instance, reinterpret_cast<void**>(&instance_state)));
  ModelState* model_state = instance_state->StateForModel();

  // This backend specifies BLOCKING execution policy. That means that
  // we should not return from this function until execution is
  // complete. Triton will automatically release 'instance' on return
  // from this function so that it is again available to be used for
  // another call to TRITONBACKEND_ModelInstanceExecute.

  LOG_MESSAGE(
      TRITONSERVER_LOG_VERBOSE,
      (std::string("model ") + model_state->Name() + ", instance " +
       instance_state->Name() + ", executing " + std::to_string(request_count) +
       " requests")
          .c_str());

  // At this point we accept ownership of 'requests', which means that
  // even if something goes wrong we must still return success from
  // this function. If something does go wrong in processing a
  // particular request then we send an error response just for the
  // specific request.

  // Note that access to 'requests' will be invalidated once
  // TRITONBACKEND_ModelInstanceExecute returns. If requests need to be
  // referred after TRITONBACKEND_ModelInstanceExecute returns, i.e. in
  // non-blocking model, the request array must be copied to a instance
  // owned buffer.
  instance_state->ProcessRequests(requests, request_count);

  return nullptr;  // success
}

}  // extern "C"
