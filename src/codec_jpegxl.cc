// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/codec_jpegxl.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "src/base.h"
#include "src/task.h"
#include "src/timer.h"

#if defined(HAS_WEBP2)
#include "third_party/libwebp2/src/wp2/base.h"
#endif

#if defined(HAS_JPEGXL)
#include "third_party/libjxl/lib/include/jxl/codestream_header.h"
#include "third_party/libjxl/lib/include/jxl/color_encoding.h"
#include "third_party/libjxl/lib/include/jxl/decode.h"
#include "third_party/libjxl/lib/include/jxl/decode_cxx.h"
#include "third_party/libjxl/lib/include/jxl/encode.h"
#include "third_party/libjxl/lib/include/jxl/encode_cxx.h"
#include "third_party/libjxl/lib/include/jxl/types.h"
#endif

namespace codec_compare_gen {

std::string JpegXLVersion() {
#if defined(HAS_JPEGXL)
  return std::to_string(JxlEncoderVersion() / 1000000) + "." +
         std::to_string(JxlEncoderVersion() % 1000000 / 1000) + "." +
         std::to_string(JxlEncoderVersion() % 1000);
#else
  return "n/a";
#endif
}

std::vector<int> JpegXLLossyQualities() {
  std::vector<int> qualities(100);
  std::iota(qualities.begin(), qualities.end(), 0);
  return qualities;  // [0:99] because 100 is lossless.
}

#if defined(HAS_WEBP2)

#if defined(HAS_JPEGXL)

namespace {

JxlPixelFormat ArgbBufferToJxlPixelFormat(const WP2::ArgbBuffer& image) {
  JxlPixelFormat pixel_format;
  const uint32_t bytes_per_channel = (WP2Formatbpc(image.format()) + 7) / 8;
  pixel_format.num_channels = WP2FormatBpp(image.format()) / bytes_per_channel;
  pixel_format.data_type =
      WP2Formatbpc(image.format()) == 8 ? JXL_TYPE_UINT8 : JXL_TYPE_UINT16;
  // TODO(yguyon): Fix endianness TODO in libwebp2/public/src/wp2/decode.h
  pixel_format.endianness = JXL_NATIVE_ENDIAN;
  pixel_format.align = image.stride();
  return pixel_format;
}

size_t ArgbBufferSize(const WP2::ArgbBuffer& image) {
  return static_cast<size_t>(image.height() - 1) * image.stride() +
         static_cast<size_t>(image.width()) * WP2FormatBpp(image.format());
}

}  // namespace

StatusOr<WP2::Data> EncodeJxl(const TaskInput& input,
                              const WP2::ArgbBuffer& original_image,
                              bool quiet) {
  CHECK_OR_RETURN(original_image.format() == WP2_RGBA_32 ||
                      original_image.format() == WP2_RGB_24,
                  quiet)
      << "libjxl requires RGB(A)";

  const JxlEncoderPtr encoder = JxlEncoderMake(nullptr);
  CHECK_OR_RETURN(encoder != nullptr, quiet) << "JxlEncoderMake() failed";
  // Single-threaded by default, no need to call JxlEncoderSetParallelRunner().

  JxlBasicInfo basic_info;
  JxlEncoderInitBasicInfo(&basic_info);
  basic_info.xsize = original_image.width();
  basic_info.ysize = original_image.height();
  basic_info.bits_per_sample = WP2Formatbpc(original_image.format());
  basic_info.uses_original_profile =
      input.codec_settings.quality == kQualityLossless ? JXL_TRUE : JXL_FALSE;
  basic_info.num_color_channels = 3;
  if (WP2FormatHasAlpha(original_image.format())) {
    basic_info.num_extra_channels =
        WP2FormatHasAlpha(original_image.format()) ? 1 : 0;
    basic_info.alpha_bits = basic_info.bits_per_sample;
    basic_info.alpha_premultiplied =
        WP2IsPremultiplied(original_image.format());
    // JxlEncoderSetExtraChannelInfo() does not need to be called for alpha
    // apparently.
  }
  JxlEncoderStatus status = JxlEncoderSetBasicInfo(encoder.get(), &basic_info);
  CHECK_OR_RETURN(status == JXL_ENC_SUCCESS, quiet)
      << "JxlEncoderSetBasicInfo() failed with error code "
      << JxlEncoderGetError(encoder.get()) << " when encoding "
      << input.image_path;

  JxlColorEncoding color_encoding = {};
  JxlColorEncodingSetToSRGB(&color_encoding, /*is_gray=*/JXL_FALSE);
  // Match cjxl output (according to jxlinfo).
  color_encoding.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
  status = JxlEncoderSetColorEncoding(encoder.get(), &color_encoding);
  CHECK_OR_RETURN(status == JXL_ENC_SUCCESS, quiet)
      << "JxlEncoderSetColorEncoding() failed with error code "
      << JxlEncoderGetError(encoder.get()) << " when encoding "
      << input.image_path;

  JxlEncoderFrameSettings* frame_settings =
      JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
  CHECK_OR_RETURN(frame_settings != nullptr, quiet)
      << "JxlEncoderFrameSettingsCreate() returned null when encoding "
      << input.image_path;

  if (input.codec_settings.quality == kQualityLossless) {
    status = JxlEncoderSetFrameLossless(frame_settings, JXL_TRUE);
    CHECK_OR_RETURN(status == JXL_ENC_SUCCESS, quiet)
        << "JxlEncoderSetFrameLossless() failed with error code "
        << JxlEncoderGetError(encoder.get()) << " when encoding "
        << input.image_path;
    // JXL_ENC_FRAME_SETTING_KEEP_INVISIBLE should be ON by default if lossless.
  } else {
    const double distance =
        JxlEncoderDistanceFromQuality(input.codec_settings.quality);
    status = JxlEncoderSetFrameDistance(frame_settings, distance);
    CHECK_OR_RETURN(status == JXL_ENC_SUCCESS, quiet)
        << "JxlEncoderSetFrameDistance() failed with error code "
        << JxlEncoderGetError(encoder.get()) << " when encoding "
        << input.image_path << " with distance " << distance << " (quality "
        << input.codec_settings.quality << ")";
  }
  status = JxlEncoderFrameSettingsSetOption(frame_settings,
                                            JXL_ENC_FRAME_SETTING_EFFORT,
                                            input.codec_settings.effort);
  CHECK_OR_RETURN(status == JXL_ENC_SUCCESS, quiet)
      << "JxlEncoderFrameSettingsSetOption(/*effort=*/"
      << input.codec_settings.effort << ") failed with error code "
      << JxlEncoderGetError(encoder.get()) << " when encoding "
      << input.image_path;

  const JxlPixelFormat pixel_format =
      ArgbBufferToJxlPixelFormat(original_image);
  status = JxlEncoderAddImageFrame(frame_settings, &pixel_format,
                                   original_image.GetRow(0),
                                   ArgbBufferSize(original_image));
  CHECK_OR_RETURN(status == JXL_ENC_SUCCESS, quiet)
      << "JxlEncoderAddImageFrame() failed with error code "
      << JxlEncoderGetError(encoder.get()) << " when encoding "
      << input.image_path;
  JxlEncoderCloseInput(encoder.get());

  WP2::Data data;
  CHECK_OR_RETURN(data.Resize(64, /*keep_bytes=*/false) == WP2_STATUS_OK,
                  quiet);

  uint8_t* next_out = data.bytes;
  size_t avail_out = data.size - (next_out - data.bytes);
  do {
    status = JxlEncoderProcessOutput(encoder.get(), &next_out, &avail_out);
    if (status == JXL_ENC_NEED_MORE_OUTPUT) {
      size_t offset = next_out - data.bytes;
      CHECK_OR_RETURN(
          data.Resize(data.size * 2, /*keep_bytes=*/true) == WP2_STATUS_OK,
          quiet);
      next_out = data.bytes + offset;
      avail_out = data.size - offset;
    }
  } while (status == JXL_ENC_NEED_MORE_OUTPUT);
  CHECK_OR_RETURN(
      data.Resize(next_out - data.bytes, /*keep_bytes=*/true) == WP2_STATUS_OK,
      quiet);
  CHECK_OR_RETURN(status == JXL_ENC_SUCCESS, quiet)
      << "JxlEncoderProcessOutput() failed with error code "
      << JxlEncoderGetError(encoder.get()) << " when encoding "
      << input.image_path;
  return data;
}

StatusOr<std::pair<WP2::ArgbBuffer, double>> DecodeJxl(
    const TaskInput& input, const WP2::Data& encoded_image, bool quiet) {
  const JxlDecoderPtr decoder = JxlDecoderMake(nullptr);
  CHECK_OR_RETURN(decoder != nullptr, quiet) << "JxlDecoderMake() failed";

  JxlDecoderStatus status = JxlDecoderSubscribeEvents(
      decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
  CHECK_OR_RETURN(status == JXL_DEC_SUCCESS, quiet)
      << "JxlDecoderSubscribeEvents() failed with error code " << status
      << " when decoding " << input.image_path;

  status = JxlDecoderSetInput(decoder.get(), encoded_image.bytes,
                              encoded_image.size);
  CHECK_OR_RETURN(status == JXL_DEC_SUCCESS, quiet)
      << "JxlDecoderSetInput() failed with error code " << status
      << " when decoding " << input.image_path;
  JxlDecoderCloseInput(decoder.get());

  status = JxlDecoderProcessInput(decoder.get());
  CHECK_OR_RETURN(status == JXL_DEC_BASIC_INFO, quiet)
      << "First call to JxlDecoderProcessInput() unexpectedly returned "
      << status << " when decoding " << input.image_path;

  JxlBasicInfo info;
  status = JxlDecoderGetBasicInfo(decoder.get(), &info);
  CHECK_OR_RETURN(status == JXL_DEC_SUCCESS, quiet)
      << "JxlDecoderGetBasicInfo() failed with error code " << status
      << " when decoding " << input.image_path;

  status = JxlDecoderProcessInput(decoder.get());
  CHECK_OR_RETURN(status == JXL_DEC_NEED_IMAGE_OUT_BUFFER, quiet)
      << "Second call to JxlDecoderProcessInput() unexpectedly returned "
      << status << " when decoding " << input.image_path;

  std::pair<WP2::ArgbBuffer, double> image_and_color_conversion_duration(
      info.alpha_bits > 0 ? WP2_RGBA_32 : WP2_RGB_24, 0.);
  WP2::ArgbBuffer& image = image_and_color_conversion_duration.first;
  CHECK_OR_RETURN(image.Resize(info.xsize, info.ysize) == WP2_STATUS_OK, quiet);
  const JxlPixelFormat pixel_format = ArgbBufferToJxlPixelFormat(image);
  status = JxlDecoderSetImageOutBuffer(decoder.get(), &pixel_format,
                                       image.GetRow(0), ArgbBufferSize(image));
  CHECK_OR_RETURN(status == JXL_DEC_SUCCESS, quiet)
      << "JxlDecoderSetImageOutBuffer() failed with error code " << status
      << " when decoding " << input.image_path;

  status = JxlDecoderProcessInput(decoder.get());
  CHECK_OR_RETURN(status == JXL_DEC_FULL_IMAGE, quiet)
      << "Third call to JxlDecoderProcessInput() unexpectedly returned "
      << status << " when decoding " << input.image_path;

  // Not sure when the color conversion is actually performed.
  const Timer color_conversion_duration;

  status = JxlDecoderProcessInput(decoder.get());
  CHECK_OR_RETURN(status == JXL_DEC_SUCCESS, quiet)
      << "Third call to JxlDecoderProcessInput() failed with error code "
      << status << " when decoding " << input.image_path;

  image_and_color_conversion_duration.second =
      color_conversion_duration.seconds();
  return image_and_color_conversion_duration;
}

#else
StatusOr<WP2::Data> EncodeJxl(const TaskInput&, const WP2::ArgbBuffer&,
                              bool quiet) {
  CHECK_OR_RETURN(false, quiet) << "Encoding images requires HAS_JPEGXL";
}
StatusOr<std::pair<WP2::ArgbBuffer, double>> DecodeJxl(const TaskInput&,
                                                       const WP2::Data&,
                                                       bool quiet) {
  CHECK_OR_RETURN(false, quiet) << "Decoding images requires HAS_JPEGXL";
}
#endif  // HAS_JPEGXL

#endif  // HAS_WEBP2

}  // namespace codec_compare_gen
