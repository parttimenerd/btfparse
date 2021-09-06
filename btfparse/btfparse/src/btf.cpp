//
// Copyright (c) 2021-present, Trail of Bits, Inc.
// All rights reserved.
//
// This source code is licensed in accordance with the terms specified in
// the LICENSE file found in the root directory of this source tree.
//

#include "btf.h"

#include <unordered_map>

namespace btfparse {

namespace {

const std::unordered_map<std::uint8_t, BTFTypeParser> kBTFParserMap{
    {BTFKind_Int, BTF::parseIntData},
    {BTFKind_Ptr, BTF::parsePtrData},
    {BTFKind_Const, BTF::parseConstData},
    {BTFKind_Array, BTF::parseArrayData},
    {BTFKind_Typedef, BTF::parseTypedefData},
    {BTFKind_Enum, BTF::parseEnumData},
    {BTFKind_FuncProto, BTF::parseFuncProtoData},
    {BTFKind_Volatile, BTF::parseVolatileData},
    {BTFKind_Struct, BTF::parseStructData},
    {BTFKind_Union, BTF::parseUnionData},
    {BTFKind_Fwd, BTF::parseFwdData},
    {BTFKind_Func, BTF::parseFuncData}};

/// TODO: Check again how this is encoded; the `kind_flag` value changes how
/// `offset` works
template <typename Type>
std::optional<BTFError>
parseStructOrUnionData(Type &output, const BTFHeader &btf_header,
                       const BTFTypeHeader &btf_type_header,
                       IFileReader &file_reader) noexcept {

  static_assert(std::is_same<Type, StructBPFType>::value ||
                    std::is_same<Type, UnionBPFType>::value,
                "Type must be either StructBPFType or UnionBPFType");

  try {
    output = {};

    output.size = btf_type_header.size_or_type;

    if (btf_type_header.name_off != 0) {
      auto name_offset =
          btf_type_header.name_off + btf_header.hdr_len + btf_header.str_off;

      auto name_res = BTF::parseString(file_reader, name_offset);
      if (name_res.failed()) {
        return name_res.takeError();
      }

      output.opt_name = name_res.takeValue();
    }

    for (std::uint32_t i = 0; i < btf_type_header.vlen; ++i) {
      typename Type::Member member{};

      auto member_name_off = file_reader.u32();
      if (member_name_off != 0) {
        member_name_off += btf_header.hdr_len + btf_header.str_off;

        auto member_name_res = BTF::parseString(file_reader, member_name_off);
        if (member_name_res.failed()) {
          return member_name_res.takeError();
        }

        member.opt_name = member_name_res.takeValue();
      }

      member.type = file_reader.u32();
      member.offset = file_reader.u32();
    }

    return std::nullopt;

  } catch (const FileReaderError &error) {
    return BTF::convertFileReaderError(error);
  }
}

} // namespace

struct BTF::PrivateData final {
  BTFTypeList btf_type_list;
};

BTF::~BTF() {}

BTF::BTF(const std::filesystem::path &path) : d(new PrivateData) {
  auto file_reader_res = IFileReader::open(path);
  if (file_reader_res.failed()) {
    throw convertFileReaderError(file_reader_res.takeError());
  }

  auto file_reader = file_reader_res.takeValue();

  bool little_endian{false};
  auto opt_error = detectEndianness(little_endian, *file_reader.get());
  if (opt_error.has_value()) {
    throw opt_error.value();
  }

  file_reader->setEndianness(little_endian);

  auto btf_header_res = readBTFHeader(*file_reader.get());
  if (btf_header_res.failed()) {
    throw btf_header_res.takeError();
  }

  auto btf_header = btf_header_res.takeValue();

  auto btf_type_list_res = parseTypeSection(btf_header, *file_reader.get());
  if (btf_type_list_res.failed()) {
    throw btf_type_list_res.takeError();
  }

  d->btf_type_list = btf_type_list_res.takeValue();
}

BTFError BTF::convertFileReaderError(const FileReaderError &error) noexcept {
  const auto &file_reader_error_info = error.get();

  BTFErrorInformation::Code error_code{BTFErrorInformation::Code::Unknown};
  switch (file_reader_error_info.code) {
  case FileReaderErrorInformation::Code::Unknown:
    error_code = BTFErrorInformation::Code::Unknown;
    break;

  case FileReaderErrorInformation::Code::MemoryAllocationFailure:
    error_code = BTFErrorInformation::Code::MemoryAllocationFailure;
    break;

  case FileReaderErrorInformation::Code::FileNotFound:
    error_code = BTFErrorInformation::Code::FileNotFound;
    break;

  case FileReaderErrorInformation::Code::IOError:
    error_code = BTFErrorInformation::Code::IOError;
    break;
  }

  std::optional<BTFErrorInformation::FileRange> opt_file_range;
  if (file_reader_error_info.opt_read_operation.has_value()) {
    const auto &read_operation =
        file_reader_error_info.opt_read_operation.value();

    opt_file_range = BTFErrorInformation::FileRange{
        read_operation.offset,
        read_operation.size,
    };
  }

  return BTFError{
      BTFErrorInformation{
          error_code,
          opt_file_range,
      },
  };
}

std::optional<BTFError>
BTF::detectEndianness(bool &little_endian, IFileReader &file_reader) noexcept {
  try {
    file_reader.seek(0);
    file_reader.setEndianness(true);

    auto magic = file_reader.u16();
    if (magic == kLittleEndianMagicValue) {
      little_endian = true;

    } else if (magic == kBigEndianMagicValue) {
      little_endian = false;

    } else {
      return BTFError{
          BTFErrorInformation{
              BTFErrorInformation::Code::InvalidMagicValue,
          },
      };
    }

    return std::nullopt;

  } catch (const FileReaderError &error) {
    return convertFileReaderError(error);
  }
}

Result<BTFHeader, BTFError>
BTF::readBTFHeader(IFileReader &file_reader) noexcept {
  try {
    file_reader.seek(0);

    BTFHeader btf_header{};
    btf_header.magic = file_reader.u16();
    btf_header.version = file_reader.u8();
    btf_header.flags = file_reader.u8();
    btf_header.hdr_len = file_reader.u32();
    btf_header.type_off = file_reader.u32();
    btf_header.type_len = file_reader.u32();
    btf_header.str_off = file_reader.u32();
    btf_header.str_len = file_reader.u32();

    return btf_header;

  } catch (const FileReaderError &error) {
    return convertFileReaderError(error);
  }
}

Result<BTF::BTFTypeList, BTFError>
BTF::parseTypeSection(const BTFHeader &btf_header,
                      IFileReader &file_reader) noexcept {

  try {
    BTFTypeList btf_type_list;

    auto type_section_start_offset = btf_header.hdr_len + btf_header.type_off;
    auto type_section_end_offset =
        type_section_start_offset + btf_header.type_len;

    file_reader.seek(type_section_start_offset);

    for (;;) {
      auto current_offset = file_reader.offset();
      if (current_offset >= type_section_end_offset) {
        break;
      }

      auto btf_type_header_res = parseTypeHeader(file_reader);
      if (btf_type_header_res.failed()) {
        return btf_type_header_res.takeError();
      }

      auto btf_type_header = btf_type_header_res.takeValue();

      auto parser_it = kBTFParserMap.find(btf_type_header.kind);
      if (parser_it == kBTFParserMap.end()) {
        std::cout << "Unsupported entry of kind "
                  << static_cast<int>(btf_type_header.kind) << std::endl;

        BTFErrorInformation::FileRange file_range{current_offset,
                                                  kBTFTypeHeaderSize};

        return BTFError{
            BTFErrorInformation{BTFErrorInformation::Code::InvalidBTFKind,
                                file_range},
        };
      }

      const auto &parser = parser_it->second;

      auto btf_type_res = parser(btf_header, btf_type_header, file_reader);
      if (btf_type_res.failed()) {
        return btf_type_res.takeError();
      }

      btf_type_list.push_back(btf_type_res.takeValue());
    }

    return btf_type_list;

  } catch (const FileReaderError &error) {
    return convertFileReaderError(error);
  }
}

Result<BTFTypeHeader, BTFError>
BTF::parseTypeHeader(IFileReader &file_reader) noexcept {

  try {
    BTFTypeHeader btf_type_common;
    btf_type_common.name_off = file_reader.u32();

    auto info = file_reader.u32();
    btf_type_common.vlen = info & 0xFFFFUL;
    btf_type_common.kind = (info & 0x1F000000UL) >> 24UL;
    btf_type_common.kind_flag = (info & 0x80000000UL) != 0;

    btf_type_common.size_or_type = file_reader.u32();

    return btf_type_common;

  } catch (const FileReaderError &error) {
    return convertFileReaderError(error);
  }
}

Result<BTFType, BTFError>
BTF::parseIntData(const BTFHeader &btf_header,
                  const BTFTypeHeader &btf_type_header,
                  IFileReader &file_reader) noexcept {

  BTFErrorInformation::FileRange file_range{
      file_reader.offset() - kBTFTypeHeaderSize,
      kBTFTypeHeaderSize + kIntBTFTypeSize};

  if (btf_type_header.kind_flag || btf_type_header.vlen != 0) {
    return BTFError{
        BTFErrorInformation{
            BTFErrorInformation::Code::InvalidIntBTFTypeEncoding,
            file_range,
        },
    };
  }

  switch (btf_type_header.size_or_type) {
  case 1:
  case 2:
  case 4:
  case 8:
  case 16:
    break;

  default: {
    return BTFError{
        BTFErrorInformation{
            BTFErrorInformation::Code::InvalidIntBTFTypeEncoding,
            file_range,
        },
    };
  }
  }

  try {
    auto name_offset =
        btf_header.hdr_len + btf_header.str_off + btf_type_header.name_off;

    auto name_res = parseString(file_reader, name_offset);
    if (name_res.failed()) {
      return name_res.takeError();
    }

    IntBTFType output;
    output.name = name_res.takeValue();

    auto integer_info = file_reader.u32();

    auto encoding = (integer_info & 0x0F000000UL) >> 24;
    output.is_signed = (encoding & 1) != 0;
    output.is_char = (encoding & 2) != 0;
    output.is_bool = (encoding & 4) != 0;

    std::size_t encoding_flag_count{0U};
    if (output.is_signed) {
      ++encoding_flag_count;
    }

    if (output.is_char) {
      ++encoding_flag_count;
    }

    if (output.is_bool) {
      ++encoding_flag_count;
    }

    if (encoding_flag_count > 1U) {
      return BTFError{
          BTFErrorInformation{
              BTFErrorInformation::Code::InvalidIntBTFTypeEncoding,
              file_range,
          },
      };
    }

    output.bits = integer_info & 0x000000ff;
    if (output.bits > 128 || output.bits > btf_type_header.size_or_type * 8) {

      return BTFError{
          BTFErrorInformation{
              BTFErrorInformation::Code::InvalidIntBTFTypeEncoding,
              file_range,
          },
      };
    }

    output.offset = (integer_info & 0x00ff0000) >> 16;
    if (output.offset + output.bits > btf_type_header.size_or_type * 8) {

      return BTFError{
          BTFErrorInformation{
              BTFErrorInformation::Code::InvalidIntBTFTypeEncoding,
              file_range,
          },
      };
    }

    return BTFType{output};

  } catch (const FileReaderError &error) {
    return convertFileReaderError(error);
  }
}

Result<BTFType, BTFError>
BTF::parsePtrData(const BTFHeader &btf_header,
                  const BTFTypeHeader &btf_type_header,
                  IFileReader &file_reader) noexcept {

  BTFErrorInformation::FileRange file_range{
      file_reader.offset() - kBTFTypeHeaderSize,
      kBTFTypeHeaderSize + kIntBTFTypeSize};

  if (btf_type_header.name_off != 0 || btf_type_header.kind_flag ||
      btf_type_header.vlen != 0) {

    return BTFError{
        BTFErrorInformation{
            BTFErrorInformation::Code::InvalidPtrBTFTypeEncoding,
            file_range,
        },
    };
  }

  PtrBTFType output;
  output.type = btf_type_header.size_or_type;

  return BTFType{output};
}

Result<BTFType, BTFError>
BTF::parseConstData(const BTFHeader &btf_header,
                    const BTFTypeHeader &btf_type_header,
                    IFileReader &file_reader) noexcept {

  BTFErrorInformation::FileRange file_range{
      file_reader.offset() - kBTFTypeHeaderSize,
      kBTFTypeHeaderSize + kIntBTFTypeSize};

  if (btf_type_header.name_off != 0 || btf_type_header.kind_flag ||
      btf_type_header.vlen != 0) {

    return BTFError{
        BTFErrorInformation{
            BTFErrorInformation::Code::InvalidPtrBTFTypeEncoding,
            file_range,
        },
    };
  }

  ConstBTFType output;
  output.type = btf_type_header.size_or_type;

  return BTFType{output};
}

Result<BTFType, BTFError>
BTF::parseArrayData(const BTFHeader &btf_header,
                    const BTFTypeHeader &btf_type_header,
                    IFileReader &file_reader) noexcept {

  BTFErrorInformation::FileRange file_range{
      file_reader.offset() - kBTFTypeHeaderSize,
      kBTFTypeHeaderSize + kIntBTFTypeSize};

  if (btf_type_header.name_off != 0 || btf_type_header.kind_flag ||
      btf_type_header.vlen != 0 || btf_type_header.size_or_type != 0) {

    return BTFError{
        BTFErrorInformation{
            BTFErrorInformation::Code::InvalidArrayBTFTypeEncoding,
            file_range,
        },
    };
  }

  try {
    ArrayBTFType output;
    output.type = file_reader.u32();
    output.index_type = file_reader.u32();
    output.nelems = file_reader.u32();

    return BTFType{output};

  } catch (const FileReaderError &error) {
    return convertFileReaderError(error);
  }
}

Result<BTFType, BTFError>
BTF::parseTypedefData(const BTFHeader &btf_header,
                      const BTFTypeHeader &btf_type_header,
                      IFileReader &file_reader) noexcept {

  BTFErrorInformation::FileRange file_range{
      file_reader.offset() - kBTFTypeHeaderSize,
      kBTFTypeHeaderSize + kIntBTFTypeSize};

  if (btf_type_header.name_off == 0 || btf_type_header.kind_flag ||
      btf_type_header.vlen != 0) {

    return BTFError{
        BTFErrorInformation{
            BTFErrorInformation::Code::InvalidTypedefBTFTypeEncoding,
            file_range,
        },
    };
  }

  auto name_offset =
      btf_header.hdr_len + btf_header.str_off + btf_type_header.name_off;

  auto name_res = parseString(file_reader, name_offset);
  if (name_res.failed()) {
    return name_res.takeError();
  }

  TypedefBTFType output;
  output.name = name_res.takeValue();

  return BTFType{output};
}

Result<BTFType, BTFError>
BTF::parseEnumData(const BTFHeader &btf_header,
                   const BTFTypeHeader &btf_type_header,
                   IFileReader &file_reader) noexcept {

  BTFErrorInformation::FileRange file_range{
      file_reader.offset() - kBTFTypeHeaderSize,
      kBTFTypeHeaderSize + kIntBTFTypeSize};

  if (btf_type_header.kind_flag || btf_type_header.vlen == 0) {
    return BTFError{
        BTFErrorInformation{
            BTFErrorInformation::Code::InvalidEnumBTFTypeEncoding,
            file_range,
        },
    };
  }

  switch (btf_type_header.size_or_type) {
  case 1:
  case 2:
  case 4:
  case 8:
    break;

  default:
    return BTFError{
        BTFErrorInformation{
            BTFErrorInformation::Code::InvalidEnumBTFTypeEncoding,
            file_range,
        },
    };
  }

  try {
    EnumBTFType output;

    if (btf_type_header.name_off != 0) {
      auto name_offset =
          btf_header.hdr_len + btf_header.str_off + btf_type_header.name_off;

      auto name_res = parseString(file_reader, name_offset);
      if (name_res.failed()) {
        return name_res.takeError();
      }

      output.opt_name = name_res.takeValue();
    }

    for (std::uint32_t i = 0; i < btf_type_header.vlen; ++i) {
      auto value_name_off = file_reader.u32();
      if (value_name_off == 0) {
        return BTFError{
            BTFErrorInformation{
                BTFErrorInformation::Code::InvalidEnumBTFTypeEncoding,
                file_range,
            },
        };
      }

      auto value_name_res =
          parseString(file_reader,
                      btf_header.hdr_len + btf_header.str_off + value_name_off);

      if (value_name_res.failed()) {
        return value_name_res.takeError();
      }

      EnumBTFType::Value enum_value{};
      enum_value.name = value_name_res.takeValue();
      enum_value.val = static_cast<std::int32_t>(file_reader.u32());

      output.value_list.push_back(std::move(enum_value));
    }

    return BTFType{output};

  } catch (const FileReaderError &error) {
    return convertFileReaderError(error);
  }
}

// TODO: Check the documentation for `BTF_KIND_FUNC_PROTO` for the necessary
// post-parsing validation steps
Result<BTFType, BTFError>
BTF::parseFuncProtoData(const BTFHeader &btf_header,
                        const BTFTypeHeader &btf_type_header,
                        IFileReader &file_reader) noexcept {

  BTFErrorInformation::FileRange file_range{
      file_reader.offset() - kBTFTypeHeaderSize,
      kBTFTypeHeaderSize + kIntBTFTypeSize};

  if (btf_type_header.name_off != 0 || btf_type_header.kind_flag) {
    return BTFError{
        BTFErrorInformation{
            BTFErrorInformation::Code::InvalidFuncProtoBTFTypeEncoding,
            file_range,
        },
    };
  }

  try {
    FuncProtoBTFType output;

    for (std::uint32_t i = 0; i < btf_type_header.vlen; ++i) {
      FuncProtoBTFType::Param param{};

      auto param_name_off = file_reader.u32();
      if (param_name_off != 0) {
        param_name_off += btf_header.hdr_len + btf_header.str_off;

        auto param_name_res = parseString(file_reader, param_name_off);
        if (param_name_res.failed()) {
          return param_name_res.takeError();
        }

        param.opt_name = param_name_res.takeValue();
      }

      param.type = file_reader.u32();

      output.param_list.push_back(std::move(param));
    }

    if (!output.param_list.empty()) {
      const auto &last_element = output.param_list.back();

      if (!last_element.opt_name.has_value() && last_element.type == 0) {
        output.param_list.pop_back();
        output.variadic = true;
      }
    }

    return BTFType{output};

  } catch (const FileReaderError &error) {
    return convertFileReaderError(error);
  }
}

Result<BTFType, BTFError>
BTF::parseVolatileData(const BTFHeader &btf_header,
                       const BTFTypeHeader &btf_type_header,
                       IFileReader &file_reader) noexcept {

  BTFErrorInformation::FileRange file_range{
      file_reader.offset() - kBTFTypeHeaderSize,
      kBTFTypeHeaderSize + kIntBTFTypeSize};

  if (btf_type_header.name_off != 0 || btf_type_header.kind_flag ||
      btf_type_header.vlen != 0) {

    return BTFError{
        BTFErrorInformation{
            BTFErrorInformation::Code::InvalidVolatileBTFTypeEncoding,
            file_range,
        },
    };
  }

  VolatileBTFType output;
  output.type = btf_type_header.size_or_type;

  return BTFType{output};
}

Result<BTFType, BTFError>
BTF::parseStructData(const BTFHeader &btf_header,
                     const BTFTypeHeader &btf_type_header,
                     IFileReader &file_reader) noexcept {

  StructBPFType output;
  auto opt_error =
      parseStructOrUnionData(output, btf_header, btf_type_header, file_reader);

  if (opt_error.has_value()) {
    return opt_error.value();
  }

  return BTFType{output};
}

Result<BTFType, BTFError>
BTF::parseUnionData(const BTFHeader &btf_header,
                    const BTFTypeHeader &btf_type_header,
                    IFileReader &file_reader) noexcept {

  UnionBPFType output;
  auto opt_error =
      parseStructOrUnionData(output, btf_header, btf_type_header, file_reader);

  if (opt_error.has_value()) {
    return opt_error.value();
  }

  return BTFType{output};
}

Result<BTFType, BTFError>
BTF::parseFwdData(const BTFHeader &btf_header,
                  const BTFTypeHeader &btf_type_header,
                  IFileReader &file_reader) noexcept {

  BTFErrorInformation::FileRange file_range{
      file_reader.offset() - kBTFTypeHeaderSize,
      kBTFTypeHeaderSize + kIntBTFTypeSize};

  if (btf_type_header.name_off == 0 || btf_type_header.vlen != 0 ||
      btf_type_header.size_or_type != 0) {

    return BTFError{
        BTFErrorInformation{
            BTFErrorInformation::Code::InvalidFwdBTFTypeEncoding,
            file_range,
        },
    };
  }

  auto name_offset =
      btf_type_header.name_off + btf_header.hdr_len + btf_header.str_off;

  auto name_res = parseString(file_reader, name_offset);
  if (name_res.failed()) {
    return name_res.takeError();
  }

  FwdBTFType output;
  output.name = name_res.takeValue();
  output.is_union = btf_type_header.kind_flag;

  return BTFType{output};
}

Result<BTFType, BTFError>
BTF::parseFuncData(const BTFHeader &btf_header,
                   const BTFTypeHeader &btf_type_header,
                   IFileReader &file_reader) noexcept {

  BTFErrorInformation::FileRange file_range{
      file_reader.offset() - kBTFTypeHeaderSize,
      kBTFTypeHeaderSize + kIntBTFTypeSize};

  if (btf_type_header.name_off == 0 || btf_type_header.kind_flag != 0 ||
      btf_type_header.vlen != 0) {

    return BTFError{
        BTFErrorInformation{
            BTFErrorInformation::Code::InvalidFuncBTFTypeEncoding,
            file_range,
        },
    };
  }

  auto name_offset =
      btf_type_header.name_off + btf_header.hdr_len + btf_header.str_off;

  auto name_res = parseString(file_reader, name_offset);
  if (name_res.failed()) {
    return name_res.takeError();
  }

  FuncBTFType output;
  output.name = name_res.takeValue();
  output.type = btf_type_header.size_or_type;

  return BTFType{output};
}

Result<std::string, BTFError> BTF::parseString(IFileReader &file_reader,
                                               std::uint64_t offset) noexcept {

  Result<std::string, BTFError> output;

  auto original_offset{file_reader.offset()};

  try {
    file_reader.seek(offset);
    std::string buffer;

    for (;;) {
      auto ch = static_cast<char>(file_reader.u8());
      if (ch == 0) {
        break;
      }

      buffer.push_back(ch);
    }

    output = buffer;

  } catch (const FileReaderError &error) {
    output = convertFileReaderError(error);
  }

  file_reader.seek(original_offset);
  return output;
}

} // namespace btfparse
