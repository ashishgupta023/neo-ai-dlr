#include "dlr_data_transform.h"

using namespace dlr;

bool DataTransform::HasInputTransform(const nlohmann::json& metadata) const {
  try {
    if (metadata.at("DataTransform").at("Input").count("ColumnTransform")) {
      return true;
    }
  } catch (nlohmann::json::out_of_range& e) {
    // ignore
  }
  return false;
}

bool DataTransform::HasOutputTransform(const nlohmann::json& metadata, int index) const {
  auto index_str = std::to_string(index);
  return metadata.count("DataTransform") && metadata["DataTransform"].count("Output") &&
         metadata["DataTransform"]["Output"].count(index_str) &&
         metadata["DataTransform"]["Output"][index_str].count("CategoricalString");
}

void DataTransform::TransformInput(const nlohmann::json& metadata, const int64_t* shape,
                                   const void* input, int dim,
                                   const std::vector<DLDataType>& dtypes, DLContext ctx,
                                   std::vector<tvm::runtime::NDArray>* tvm_inputs) const {
  nlohmann::json input_json = GetAsJson(shape, input, dim);
  const auto& transforms = metadata["DataTransform"]["Input"]["ColumnTransform"];
  CHECK_LE(tvm_inputs->size(), transforms.size());
  for (int i = 0; i < tvm_inputs->size(); i++) {
    const std::string& transformer_type = transforms[i]["Type"].get_ref<const std::string&>();
    auto it = GetTransformerMap()->find(transformer_type);
    CHECK(it != GetTransformerMap()->end())
        << transformer_type << " is not a valid DataTransform type.";
    const auto transformer = it->second;

    tvm_inputs->at(i) = transformer->InitNDArray(input_json, dtypes[i], ctx);
    transformer->MapToNDArray(input_json, transforms[i], tvm_inputs->at(i));
  }
}

nlohmann::json DataTransform::GetAsJson(const int64_t* shape, const void* input, int dim) const {
  CHECK_EQ(dim, 1) << "String input must be 1-D vector.";
  // Interpret input as json
  const char* input_str = static_cast<const char*>(input);
  nlohmann::json input_json;
  try {
    input_json = nlohmann::json::parse(input_str, input_str + shape[0]);
  } catch (nlohmann::json::parse_error& e) {
    LOG(ERROR) << "Invalid JSON input: " << e.what();
  }
  CHECK(input_json.is_array() && input_json.size() > 0 && input_json[0].is_array())
      << "Invalid JSON input: Must be 2-D array.";
  return input_json;
}

tvm::runtime::NDArray Transformer::InitNDArray(const nlohmann::json& input_json, DLDataType dtype,
                                               DLContext ctx) const {
  // Create NDArray for transformed input which will be passed to TVM.
  std::vector<int64_t> arr_shape = {static_cast<int64_t>(input_json.size()),
                                    static_cast<int64_t>(input_json[0].size())};
  CHECK(dtype.code == kDLFloat && dtype.bits == 32 && dtype.lanes == 1)
      << "DataTransform CategoricalString is only supported for float32 inputs.";
  return tvm::runtime::NDArray::Empty(arr_shape, dtype, ctx);
}

void FloatTransformer::MapToNDArray(const nlohmann::json& input_json,
                                    const nlohmann::json& transform,
                                    tvm::runtime::NDArray& input_array) const {
  DLTensor* input_tensor = const_cast<DLTensor*>(input_array.operator->());
  CHECK_EQ(input_tensor->ctx.device_type, DLDeviceType::kDLCPU)
      << "DataTransform is only supported for CPU.";
  float* data = static_cast<float*>(input_tensor->data);
  for (size_t r = 0; r < input_json.size(); ++r) {
    CHECK_EQ(input_json[r].size(), input_json[0].size()) << "Inconsistent number of columns";
    for (size_t c = 0; c < input_json[r].size(); ++c) {
      const int out_index = r * input_json[r].size() + c;
      // Data is numeric, pass through. Attempt to convert string to float.
      try {
        data[out_index] = input_json[r][c].is_number()
                              ? input_json[r][c].get<float>()
                              : std::stof(input_json[r][c].get_ref<const std::string&>());
      } catch (const std::exception& ex) {
        // Any error will fallback safely to kBadValue.
        data[out_index] = kBadValue;
      }
    }
  }
}

void CategoricalStringTransformer::MapToNDArray(const nlohmann::json& input_json,
                                                const nlohmann::json& transform,
                                                tvm::runtime::NDArray& input_array) const {
  const nlohmann::json& mapping = transform["Map"];
  DLTensor* input_tensor = const_cast<DLTensor*>(input_array.operator->());
  // Writing directly to the DLTensor will only work for CPU context. For other contexts, we would
  // need to create an intermediate buffer on CPU and copy that to the context.
  CHECK_EQ(input_tensor->ctx.device_type, DLDeviceType::kDLCPU)
      << "DataTransform CategoricalString is only supported for CPU.";
  CHECK_EQ(input_json[0].size(), mapping.size())
      << "Input has " << input_json[0].size() << " columns, but model requires " << mapping.size();
  float* data = static_cast<float*>(input_tensor->data);
  // Copy data into data, mapping strings to float along the way.
  for (size_t r = 0; r < input_json.size(); ++r) {
    CHECK_EQ(input_json[r].size(), input_json[0].size()) << "Inconsistent number of columns";
    for (size_t c = 0; c < input_json[r].size(); ++c) {
      const int out_index = r * input_json[r].size() + c;
      // Look up in map. If not found, use kMissingValue.
      try {
        // If there is no items in map, try to pass forward as float.
        if (mapping[c].empty()) {
          data[out_index] = input_json[r][c].is_number()
                                ? input_json[r][c].get<float>()
                                : std::stof(input_json[r][c].get_ref<const std::string&>());
          continue;
        }
        const std::string& data_str = input_json[r][c].get_ref<const std::string&>();
        auto it = mapping[c].find(data_str);
        data[out_index] = it != mapping[c].end() ? it->operator float() : kMissingValue;
      } catch (const std::exception& ex) {
        // Any error will fallback safely to kMissingValue.
        data[out_index] = kMissingValue;
      }
    }
  }
}

tvm::runtime::NDArray DateTimeTransformer::InitNDArray(const nlohmann::json& input_json,
                                                       DLDataType dtype, DLContext ctx) const {
  // Create NDArray for transformed input which will be passed to TVM. NUM_COL
  // fixed to 7
  std::vector<int64_t> arr_shape = {static_cast<int64_t>(input_json.size()),
                                    static_cast<int64_t>(kNumDateTimeCols)};
  CHECK(dtype.code == kDLFloat && dtype.bits == 32 && dtype.lanes == 1)
      << "DataTransform DateTimeTransformer is only supported for float32 "
         "inputs.";
  return tvm::runtime::NDArray::Empty(arr_shape, dtype, ctx);
}

int64_t DateTimeTransformer::GetWeekDay(int64_t year, int64_t month, int64_t day) const {
  int64_t century = year / 100;
  int64_t month_ = month == 2 ? 12 : postive_modulo((month - 2), 12);
  int64_t year_ = month <= 2 ? year % 100 - 1 : year % 100;
  int64_t weekday = postive_modulo((day + (int64_t)std::floor(2.6 * month_ - 0.2) - 2 * century +
                                    year_ + year_ / 4 + century / 4),
                                   7);
  weekday = weekday == 0 ? 7 : weekday;
  return weekday;
}

void DateTimeTransformer::DigitizeDateTime(std::string& input_string,
                                           std::vector<int64_t>& datetime_digits) const {
  struct tm tm = {};

  char* strptime_success;
  for (const auto datetime_template : datetime_templates) {
    strptime_success = strptime(input_string.c_str(), datetime_template.c_str(), &tm);
    if (strptime_success) {
      break;
    }
  }

  int64_t week_offset =
      GetWeekDay(1900 + tm.tm_year, 1, 1) == 0 ? 0 : (7 - GetWeekDay(1900 + tm.tm_year, 1, 1)) + 1;
  int64_t week_of_year = (tm.tm_yday) / 7;
  if (week_offset > 0) week_of_year += 1;

  datetime_digits[0] = tm.tm_wday;
  datetime_digits[1] = 1900 + tm.tm_year;
  datetime_digits[2] = tm.tm_hour;
  datetime_digits[3] = tm.tm_min;
  datetime_digits[4] = tm.tm_sec;
  datetime_digits[5] = 1 + tm.tm_mon;
  datetime_digits[6] = week_of_year;
}

void DateTimeTransformer::MapToNDArray(const nlohmann::json& input_json,
                                       const nlohmann::json& transform,
                                       tvm::runtime::NDArray& input_array) const {
  DLTensor* input_tensor = const_cast<DLTensor*>(input_array.operator->());
  CHECK_EQ(input_tensor->ctx.device_type, DLDeviceType::kDLCPU)
      << "DataTransform DateTimeVectorizer is only supported for CPU.";
  float* data = static_cast<float*>(input_tensor->data);

  std::vector<int64_t> datetime_digits = std::vector<int64_t>(kNumDateTimeCols, 0);
  for (size_t r = 0; r < input_json.size(); ++r) {
    CHECK(input_json[r].size() > 0)
        << "Input must contains a string of format [Date Month, Year, Time].";
    auto date_col = transform["DateCol"].get_ref<const nlohmann::json::number_integer_t&>();
    std::string entry = input_json[r][date_col].get_ref<const std::string&>();
    DigitizeDateTime(entry, datetime_digits);
    for (size_t c = 0; c < kNumDateTimeCols; ++c) {
      const int out_index = r * kNumDateTimeCols + c;
      data[out_index] = static_cast<float>(datetime_digits[c]);
    }
  }
}

const std::shared_ptr<std::unordered_map<std::string, std::shared_ptr<Transformer>>>
DataTransform::GetTransformerMap() const {
  static auto map =
      std::make_shared<std::unordered_map<std::string, std::shared_ptr<Transformer>>>();
  if (!map->empty()) return map;
  map->emplace("Float", std::make_shared<FloatTransformer>());
  map->emplace("CategoricalString", std::make_shared<CategoricalStringTransformer>());
  map->emplace("DateTime", std::make_shared<DateTimeTransformer>());
  return map;
}

template <typename T>
nlohmann::json DataTransform::TransformOutputHelper1D(const nlohmann::json& transform,
                                                      const T* data,
                                                      const std::vector<int64_t>& shape) const {
  const nlohmann::json& mapping = transform["CategoricalString"];
  CHECK_EQ(shape.size(), 1);
  nlohmann::json output_json = nlohmann::json::array();
  for (int64_t i = 0; i < shape[0]; ++i) {
    auto it = mapping.find(std::to_string(data[i]));
    if (it == mapping.end()) {
      output_json.push_back(transform.count("UnseenLabel") ? transform["UnseenLabel"]
                                                           : kUnknownLabel);
    } else {
      output_json.push_back(*it);
    }
  }
  return output_json;
}

template <typename T>
nlohmann::json DataTransform::TransformOutputHelper2D(const nlohmann::json& transform,
                                                      const T* data,
                                                      const std::vector<int64_t>& shape) const {
  CHECK_EQ(shape.size(), 2);
  nlohmann::json output_json = nlohmann::json::array();
  for (int64_t i = 0; i < shape[0]; ++i) {
    output_json.push_back(TransformOutputHelper1D<T>(transform, data + i * shape[1], {shape[1]}));
  }
  return output_json;
}

void DataTransform::TransformOutput(const nlohmann::json& metadata, int index,
                                    const tvm::runtime::NDArray& output_array) {
  auto& transform = metadata["DataTransform"]["Output"][std::to_string(index)];
  const DLTensor* tensor = output_array.operator->();
  CHECK_EQ(tensor->ctx.device_type, DLDeviceType::kDLCPU)
      << "DataTransform CategoricalString is only supported for CPU.";
  CHECK(tensor->dtype.code == kDLInt && tensor->dtype.bits == 32 && tensor->dtype.lanes == 1)
      << "DataTransform CategoricalString is only supported for int32 outputs.";

  std::vector<int64_t> shape(output_array->shape, output_array->shape + output_array->ndim);
  nlohmann::json output_json;
  if (shape.size() == 1) {
    output_json = TransformOutputHelper1D<int>(transform, static_cast<int*>(tensor->data), shape);
  } else if (shape.size() == 2) {
    output_json = TransformOutputHelper2D<int>(transform, static_cast<int*>(tensor->data), shape);
  } else {
    throw dmlc::Error("DataTransform CategoricalString is only supported for 1-D or 2-D inputs.");
  }
  transformed_outputs_[index] = output_json.dump();
}

void DataTransform::GetOutputShape(int index, int64_t* shape) const {
  auto it = transformed_outputs_.find(index);
  shape[0] = it == transformed_outputs_.end() ? -1 : it->second.size();
}

void DataTransform::GetOutputSizeDim(int index, int64_t* size, int* dim) const {
  auto it = transformed_outputs_.find(index);
  if (it == transformed_outputs_.end()) {
    *size = -1;
    *dim = 1;
    return;
  }
  *size = it->second.size();
  *dim = 1;
}

void DataTransform::GetOutput(int index, void* output) const {
  auto it = transformed_outputs_.find(index);
  CHECK(it != transformed_outputs_.end()) << "Inference has not been run or output does not exist.";
  std::copy(it->second.begin(), it->second.end(), static_cast<char*>(output));
}

const void* DataTransform::GetOutputPtr(int index) const {
  auto it = transformed_outputs_.find(index);
  CHECK(it != transformed_outputs_.end()) << "Inference has not been run or output does not exist.";
  return static_cast<const void*>(it->second.data());
}
