#include "s5_frame.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <setupapi.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kIoctlWriteBulk = 0x80892880;
constexpr size_t kMaxFrameSize = 4u * 1024u * 1024u;
// The reader stream index (MF_SOURCE_READER_FIRST_VIDEO_STREAM as DWORD).
constexpr DWORD kVideoStream =
    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

// This is the interface GUID published by the ARM64 driver in src/s5usb.c.
constexpr GUID kS5DisplayInterface = {
    0xc757f7b0,
    0xa56e,
    0x4d3d,
    {0xb8, 0xd4, 0x95, 0x16, 0xe9, 0x32, 0xca, 0xf7}};

using Pixels = s5::Pixels;
using s5::encode_frame;
constexpr UINT kWidth = s5::kWidth;
constexpr UINT kHeight = s5::kHeight;

std::string hr_text(HRESULT hr) {
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08lX",
                static_cast<unsigned long>(hr));
  return buffer;
}

void check_hr(HRESULT hr, const char *operation) {
  if (FAILED(hr)) {
    throw std::runtime_error(std::string(operation) +
                             " failed: " + hr_text(hr));
  }
}

std::string win32_text(DWORD error) {
  char buffer[64]{};
  std::snprintf(buffer, sizeof(buffer), "Win32 error %lu (0x%08lX)",
                static_cast<unsigned long>(error),
                static_cast<unsigned long>(error));
  return buffer;
}

std::wstring lower(std::wstring value) {
  for (wchar_t &character : value) {
    character = static_cast<wchar_t>(std::towlower(character));
  }
  return value;
}

bool is_image_path(const std::wstring &path) {
  const size_t dot = path.find_last_of(L'.');
  const size_t separator = path.find_last_of(L"/\\");
  if (dot == std::wstring::npos ||
      (separator != std::wstring::npos && dot < separator)) {
    return false;
  }
  const std::wstring extension = lower(path.substr(dot));
  return extension == L".jpg" || extension == L".jpeg" ||
         extension == L".png" || extension == L".bmp" || extension == L".gif" ||
         extension == L".tif" || extension == L".tiff";
}

class S5Transport {
public:
  S5Transport() = default;
  S5Transport(const S5Transport &) = delete;
  S5Transport &operator=(const S5Transport &) = delete;

  ~S5Transport() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }

  void open() {
    HDEVINFO devices =
        SetupDiGetClassDevsW(&kS5DisplayInterface, nullptr, nullptr,
                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("SetupDiGetClassDevsW failed: " +
                               win32_text(GetLastError()));
    }

    DWORD open_error = ERROR_NOT_FOUND;
    for (DWORD index = 0;; ++index) {
      SP_DEVICE_INTERFACE_DATA interface_data{};
      interface_data.cbSize = sizeof(interface_data);
      if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &kS5DisplayInterface,
                                       index, &interface_data)) {
        if (GetLastError() == ERROR_NO_MORE_ITEMS) {
          break;
        }
        open_error = GetLastError();
        break;
      }

      DWORD required = 0;
      SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, nullptr, 0,
                                       &required, nullptr);
      if (required == 0) {
        open_error = GetLastError();
        continue;
      }

      std::vector<std::uint8_t> storage(required);
      auto *detail =
          reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
      detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
      if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail,
                                            required, nullptr, nullptr)) {
        open_error = GetLastError();
        continue;
      }

      const std::wstring path(detail->DevicePath);
      const std::wstring path_lower = lower(path);
      if (path_lower.find(L"vid_17cc&pid_1420&mi_04#") == std::wstring::npos) {
        continue;
      }

      HANDLE handle =
          CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (handle != INVALID_HANDLE_VALUE) {
        handle_ = handle;
        std::wcout << L"Opened S5 display interface: " << path << L"\n";
        break;
      }
      open_error = GetLastError();
    }

    SetupDiDestroyDeviceInfoList(devices);
    if (handle_ == INVALID_HANDLE_VALUE) {
      throw std::runtime_error(
          "Could not open the S5 display interface: " + win32_text(open_error) +
          ". Check that s5usb.sys is installed on MI_04.");
    }
  }

  void write_frame(const std::vector<std::uint8_t> &frame,
                   std::uint8_t screen) {
    if (frame.size() < 20 || frame.size() > kMaxFrameSize) {
      throw std::runtime_error("encoded frame has an invalid size");
    }

    std::lock_guard<std::mutex> lock(write_mutex_);
    DWORD returned = 0;
    if (!DeviceIoControl(
            handle_, kIoctlWriteBulk, const_cast<std::uint8_t *>(frame.data()),
            static_cast<DWORD>(frame.size()), nullptr, 0, &returned, nullptr)) {
      throw std::runtime_error("0x80892880 write for screen " +
                               std::to_string(screen) +
                               " failed: " + win32_text(GetLastError()));
    }
  }

private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  std::mutex write_mutex_;
};

std::uint16_t to_rgb565(std::uint8_t blue, std::uint8_t green,
                        std::uint8_t red) {
  const std::uint16_t r = static_cast<std::uint16_t>((red * 31u + 127u) / 255u);
  const std::uint16_t g =
      static_cast<std::uint16_t>((green * 63u + 127u) / 255u);
  const std::uint16_t b =
      static_cast<std::uint16_t>((blue * 31u + 127u) / 255u);
  return static_cast<std::uint16_t>((r << 11) | (g << 5) | b);
}

Pixels bgra_to_pixels(const std::uint8_t *data, LONG stride, UINT width,
                      UINT height) {
  if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
      std::abs(static_cast<long long>(stride)) <
          static_cast<long long>(width) * 4) {
    throw std::runtime_error("invalid RGB32 dimensions or stride");
  }
  Pixels pixels(static_cast<size_t>(width) * height);
  for (UINT y = 0; y < height; ++y) {
    // data points to the top scanline. Lock2D may return negative pitch;
    // subsequent logical rows then lie at lower memory addresses.
    const std::uint8_t *row = data + static_cast<std::ptrdiff_t>(y) * stride;
    for (UINT x = 0; x < width; ++x) {
      const std::uint8_t *pixel = row + static_cast<size_t>(x) * 4;
      pixels[static_cast<size_t>(y) * width + x] =
          to_rgb565(pixel[0], pixel[1], pixel[2]);
    }
  }
  return pixels;
}

ComPtr<IWICImagingFactory> create_wic_factory() {
  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    check_hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
             "CoCreateInstance(CLSID_WICImagingFactory)");
  }
  return factory;
}

Pixels load_image(const std::wstring &path) {
  ComPtr<IWICImagingFactory> factory = create_wic_factory();
  ComPtr<IWICBitmapDecoder> decoder;
  check_hr(factory->CreateDecoderFromFilename(
               path.c_str(), nullptr, GENERIC_READ,
               WICDecodeMetadataCacheOnLoad, &decoder),
           "CreateDecoderFromFilename");

  ComPtr<IWICBitmapFrameDecode> frame;
  check_hr(decoder->GetFrame(0, &frame), "IWICBitmapDecoder::GetFrame");

  ComPtr<IWICBitmapScaler> scaler;
  check_hr(factory->CreateBitmapScaler(&scaler), "CreateBitmapScaler");
  check_hr(scaler->Initialize(frame.Get(), kWidth, kHeight,
                              WICBitmapInterpolationModeFant),
           "IWICBitmapScaler::Initialize");

  ComPtr<IWICFormatConverter> converter;
  check_hr(factory->CreateFormatConverter(&converter), "CreateFormatConverter");
  check_hr(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom),
           "IWICFormatConverter::Initialize");

  const UINT stride = kWidth * 4;
  std::vector<std::uint8_t> bgra(static_cast<size_t>(stride) * kHeight);
  check_hr(converter->CopyPixels(nullptr, stride,
                                 static_cast<UINT>(bgra.size()), bgra.data()),
           "IWICFormatConverter::CopyPixels");
  return bgra_to_pixels(bgra.data(), static_cast<LONG>(stride), kWidth,
                        kHeight);
}

Pixels sample_to_pixels(IMFSample *sample, UINT width, UINT height, LONG stride,
                        UINT offset_x = 0, UINT offset_y = 0) {
  ComPtr<IMFMediaBuffer> buffer;
  check_hr(sample->ConvertToContiguousBuffer(&buffer),
           "IMFSample::ConvertToContiguousBuffer");

  ComPtr<IMF2DBuffer> buffer_2d;
  if (SUCCEEDED(buffer.As(&buffer_2d))) {
    BYTE *scanline = nullptr;
    LONG pitch = 0;
    check_hr(buffer_2d->Lock2D(&scanline, &pitch), "IMF2DBuffer::Lock2D");
    try {
      const std::ptrdiff_t origin =
          static_cast<std::ptrdiff_t>(offset_y) * pitch +
          static_cast<std::ptrdiff_t>(offset_x) * 4;
      Pixels pixels = bgra_to_pixels(scanline + origin, pitch, width, height);
      buffer_2d->Unlock2D();
      return pixels;
    } catch (...) {
      buffer_2d->Unlock2D();
      throw;
    }
  }

  BYTE *data = nullptr;
  DWORD max_length = 0;
  DWORD current_length = 0;
  check_hr(buffer->Lock(&data, &max_length, &current_length),
           "IMFMediaBuffer::Lock");
  try {
    const auto row_size =
        static_cast<size_t>(std::abs(static_cast<long long>(stride)));
    if (row_size * (static_cast<size_t>(offset_y) + height) > current_length ||
        row_size < (static_cast<size_t>(offset_x) + width) * 4 ||
        current_length > max_length) {
      throw std::runtime_error("truncated RGB32 video buffer");
    }
    // Bottom-up buffers are assumed to cover exactly the display region;
    // vertical aperture offsets are not applied to them (not needed for the
    // S5 inputs seen so far).
    const BYTE *top =
        stride < 0
            ? data + row_size * (height - 1) + static_cast<size_t>(offset_x) * 4
            : data + static_cast<size_t>(offset_y) * row_size +
                  static_cast<size_t>(offset_x) * 4;
    Pixels pixels = bgra_to_pixels(top, stride, width, height);
    buffer->Unlock();
    return pixels;
  } catch (...) {
    buffer->Unlock();
    throw;
  }
}

// The reader can report a frame that is larger than the visible picture (for
// example H.264 pads 480x360 to 480x368) and marks the display area with
// aperture attributes. Prefer the aperture; fall back to the full frame.
void display_area_from_type(IMFMediaType *type, UINT32 &width, UINT32 &height,
                            UINT32 &offset_x, UINT32 &offset_y) {
  UINT32 frame_width = 0;
  UINT32 frame_height = 0;
  check_hr(
      MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &frame_width, &frame_height),
      "MFGetAttributeSize");
  if (frame_width == 0 || frame_height == 0 || frame_width > 16384 ||
      frame_height > 16384) {
    throw std::runtime_error("video has no valid frame size");
  }
  width = frame_width;
  height = frame_height;
  offset_x = 0;
  offset_y = 0;

  MFVideoArea aperture = {};
  UINT32 aperture_size = 0;
  bool have_aperture = SUCCEEDED(type->GetBlob(
      MF_MT_GEOMETRIC_APERTURE, reinterpret_cast<UINT8 *>(&aperture),
      sizeof(aperture), &aperture_size));
  if (!have_aperture) {
    have_aperture = SUCCEEDED(type->GetBlob(
        MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8 *>(&aperture),
        sizeof(aperture), &aperture_size));
  }
  if (!have_aperture || aperture.Area.cx <= 0 || aperture.Area.cy <= 0 ||
      static_cast<UINT32>(aperture.Area.cx) > frame_width ||
      static_cast<UINT32>(aperture.Area.cy) > frame_height) {
    return;
  }
  width = static_cast<UINT32>(aperture.Area.cx);
  height = static_cast<UINT32>(aperture.Area.cy);
  if (aperture.OffsetX.value > 0) {
    offset_x = static_cast<UINT32>(aperture.OffsetX.value);
  }
  if (aperture.OffsetY.value > 0) {
    offset_y = static_cast<UINT32>(aperture.OffsetY.value);
  }
  if (offset_x + width > frame_width) {
    offset_x = frame_width - width;
  }
  if (offset_y + height > frame_height) {
    offset_y = frame_height - height;
  }
}

// Buffer stride of the decoded RGB32 frames. Prefer the attribute the
// reader reports; otherwise derive it from the frame width (the display
// width can be smaller when the frame is padded or cropped).
LONG stride_from_type(IMFMediaType *type) {
  UINT32 stride_bits = 0;
  if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_bits))) {
    return static_cast<LONG>(stride_bits);
  }
  UINT32 frame_width = 0;
  UINT32 frame_height = 0;
  check_hr(
      MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &frame_width, &frame_height),
      "MFGetAttributeSize (stride fallback)");
  if (frame_width == 0 || frame_height == 0) {
    throw std::runtime_error("video has no valid frame size");
  }
  LONG stride = 0;
  check_hr(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,
                                          frame_width, &stride),
           "MFGetStrideForBitmapInfoHeader");
  return stride;
}

void send_to_screens(S5Transport &transport, std::uint8_t screen_mask,
                     const Pixels &pixels) {
  for (std::uint8_t screen = 0; screen < 2; ++screen) {
    if ((screen_mask & (1u << screen)) == 0) {
      continue;
    }
    const std::vector<std::uint8_t> frame = encode_frame(screen, pixels);
    transport.write_frame(frame, screen);
    std::wcout << L"screen " << static_cast<unsigned>(screen) << L": wrote "
               << frame.size() << L" bytes\n";
  }
}

void play_image(S5Transport &transport, std::uint8_t screen_mask,
                const std::wstring &path) {
  const Pixels pixels = load_image(path);
  send_to_screens(transport, screen_mask, pixels);
}

void play_video(S5Transport &transport, std::uint8_t screen_mask,
                const std::wstring &path) {
  ComPtr<IMFAttributes> attributes;
  check_hr(MFCreateAttributes(&attributes, 2), "MFCreateAttributes");
  check_hr(
      attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE),
      "IMFAttributes::SetUINT32");

  ComPtr<IMFSourceReader> reader;
  check_hr(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader),
           "MFCreateSourceReaderFromURL");

  ComPtr<IMFMediaType> output_type;
  check_hr(MFCreateMediaType(&output_type), "MFCreateMediaType");
  check_hr(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video),
           "Set video major type");
  check_hr(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32),
           "Set RGB32 subtype");
  check_hr(
      reader->SetCurrentMediaType(kVideoStream, nullptr, output_type.Get()),
      "IMFSourceReader::SetCurrentMediaType");

  ComPtr<IMFMediaType> current_type;
  check_hr(reader->GetCurrentMediaType(kVideoStream, &current_type),
           "IMFSourceReader::GetCurrentMediaType");
  UINT32 width = 0;
  UINT32 height = 0;
  UINT32 sample_offset_x = 0;
  UINT32 sample_offset_y = 0;
  display_area_from_type(current_type.Get(), width, height, sample_offset_x,
                         sample_offset_y);
  LONG stride = stride_from_type(current_type.Get());

  UINT32 frame_rate_num = 0;
  UINT32 frame_rate_den = 1;
  MFGetAttributeRatio(current_type.Get(), MF_MT_FRAME_RATE, &frame_rate_num,
                      &frame_rate_den);
  std::wcout << L"Playing " << path << L" (" << width << L"x" << height << L", "
             << frame_rate_num << L"/" << frame_rate_den << L" fps)\n";

  LONGLONG first_timestamp = -1;
  const auto wall_start = std::chrono::steady_clock::now();
  for (;;) {
    DWORD stream_index = 0;
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> sample;
    check_hr(reader->ReadSample(kVideoStream, 0, &stream_index, &flags,
                                &timestamp, &sample),
             "IMFSourceReader::ReadSample");
    if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
      break;
    }
    if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
      // Fatal stream error reported by the source reader.
      throw std::runtime_error(
          "source reader reported an error during playback");
    }
    if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
      // The reader can flag a format change even when the frames we
      // consume are unaffected (post-decode renegotiation, stride/crop
      // normalization, ...). Re-query the current type and adapt in
      // place instead of bailing out.
      ComPtr<IMFMediaType> changed_type;
      check_hr(reader->GetCurrentMediaType(kVideoStream, &changed_type),
               "GetCurrentMediaType (format change)");

      GUID subtype = {};
      check_hr(changed_type->GetGUID(MF_MT_SUBTYPE, &subtype),
               "IMFMediaType::GetGUID(MF_MT_SUBTYPE)");
      if (subtype != MFVideoFormat_RGB32) {
        // Try to renegotiate the RGB32 output we can handle; if this
        // fails, the file is genuinely unsupported from here on.
        check_hr(reader->SetCurrentMediaType(kVideoStream, nullptr,
                                             output_type.Get()),
                 "renegotiate RGB32 output");
        check_hr(reader->GetCurrentMediaType(kVideoStream, &changed_type),
                 "GetCurrentMediaType (after renegotiate)");
      }

      UINT32 new_width = 0;
      UINT32 new_height = 0;
      UINT32 new_offset_x = 0;
      UINT32 new_offset_y = 0;
      display_area_from_type(changed_type.Get(), new_width, new_height,
                             new_offset_x, new_offset_y);
      if (new_width != width || new_height != height ||
          new_offset_x != sample_offset_x || new_offset_y != sample_offset_y) {
        std::wcout << path << L": display area changed: " << width << L"x"
                   << height << L"[+" << sample_offset_x << L","
                   << sample_offset_y << L"] -> " << new_width << L"x"
                   << new_height << L"[+" << new_offset_x << L","
                   << new_offset_y << L"]\n";
        width = new_width;
        height = new_height;
        sample_offset_x = new_offset_x;
        sample_offset_y = new_offset_y;
      }
      // Only the non-2D fallback path uses this cached stride, but keep
      // it in sync anyway.
      stride = stride_from_type(changed_type.Get());
    }
    if (!sample) {
      continue;
    }

    if (first_timestamp < 0) {
      first_timestamp = timestamp;
    }
    if (timestamp >= first_timestamp) {
      const auto target =
          wall_start +
          std::chrono::milliseconds((timestamp - first_timestamp) / 10000);
      std::this_thread::sleep_until(target);
    }

    const Pixels pixels = sample_to_pixels(sample.Get(), width, height, stride,
                                           sample_offset_x, sample_offset_y);
    Pixels scaled;
    if (width == kWidth && height == kHeight) {
      send_to_screens(transport, screen_mask, pixels);
    } else {
      // Nearest-neighbor scaling from the decoded video to the panel.
      scaled.resize(static_cast<size_t>(kWidth) * kHeight);
      for (UINT y = 0; y < kHeight; ++y) {
        const UINT source_y = (y * height) / kHeight;
        for (UINT x = 0; x < kWidth; ++x) {
          const UINT source_x = (x * width) / kWidth;
          scaled[static_cast<size_t>(y) * kWidth + x] =
              pixels[static_cast<size_t>(source_y) * width + source_x];
        }
      }
      send_to_screens(transport, screen_mask, scaled);
    }
  }
}

struct Source {
  std::uint8_t screen_mask;
  std::wstring path;
};

void print_usage() {
  std::wcout
      << L"S5 Screen Player\n\n"
      << L"Usage:\n"
      << L"  s5-screen-player.exe --left <image-or-video>\n"
      << L"  s5-screen-player.exe --right <image-or-video>\n"
      << L"  s5-screen-player.exe --all <image-or-video>\n"
      << L"  s5-screen-player.exe --left <file> --right <file>\n\n"
      << L"Images use WIC; MP4/video uses Media Foundation.\n"
      << L"The Traktor/NIHardwareService display path should be stopped while\n"
      << L"using this standalone player so both writers do not compete.\n";
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc == 1 || (argc == 2 && std::wstring(argv[1]) == L"--help")) {
    print_usage();
    return argc == 1 ? 1 : 0;
  }

  std::vector<Source> sources;
  for (int index = 1; index < argc;) {
    if (index + 1 >= argc) {
      print_usage();
      return 2;
    }
    const std::wstring option = argv[index];
    std::uint8_t screen_mask = 0;
    if (option == L"--left") {
      screen_mask = 0x01;
    } else if (option == L"--right") {
      screen_mask = 0x02;
    } else if (option == L"--all") {
      screen_mask = 0x03;
    } else {
      std::wcerr << L"Unknown option: " << option << L"\n";
      print_usage();
      return 2;
    }
    sources.push_back({screen_mask, argv[index + 1]});
    index += 2;
  }

  HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
    std::cerr << "CoInitializeEx failed: " << hr_text(com_result) << "\n";
    return 3;
  }
  const bool com_initialized = SUCCEEDED(com_result);

  HRESULT mf_result = MFStartup(MF_VERSION);
  if (FAILED(mf_result)) {
    std::cerr << "MFStartup failed: " << hr_text(mf_result) << "\n";
    if (com_initialized) {
      CoUninitialize();
    }
    return 3;
  }

  int result = 0;
  try {
    S5Transport transport;
    transport.open();

    std::mutex error_mutex;
    std::exception_ptr first_error;
    std::vector<std::thread> workers;
    for (const Source &source : sources) {
      workers.emplace_back([&transport, &error_mutex, &first_error, source]() {
        const HRESULT worker_com_result =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool worker_com_initialized = SUCCEEDED(worker_com_result);
        try {
          check_hr(worker_com_result, "worker CoInitializeEx");
          if (is_image_path(source.path)) {
            play_image(transport, source.screen_mask, source.path);
          } else {
            play_video(transport, source.screen_mask, source.path);
          }
        } catch (...) {
          std::lock_guard<std::mutex> lock(error_mutex);
          if (!first_error) {
            first_error = std::current_exception();
          }
        }
        if (worker_com_initialized) {
          CoUninitialize();
        }
      });
    }
    for (std::thread &worker : workers) {
      worker.join();
    }
    if (first_error) {
      std::rethrow_exception(first_error);
    }
  } catch (const std::exception &error) {
    std::cerr << "S5 Screen Player: " << error.what() << "\n";
    result = 4;
  }

  MFShutdown();
  if (com_initialized) {
    CoUninitialize();
  }
  return result;
}
