#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <csignal>
#include <chrono>
#include <new>
#include <sys/stat.h>
#include <thread>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <limits>
#include <numeric>
#include <cmath>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <histedit.h>
#include <functional>
#include <deque>
#include <initializer_list>
#include <arpa/inet.h>
#include <clocale>
#include <optional>
#include <cerrno>
#include <linux/input.h>

#include "CRSDK/CameraRemote_SDK.h"
#include "CRSDK/ICrCameraObjectInfo.h"
#include "CRSDK/IDeviceCallback.h"
#include "CRSDK/CrDeviceProperty.h"
#include "CRSDK/CrTypes.h"
#include "CRSDK/CrImageDataBlock.h"
#include "CRSDK/CrDefines.h"

#ifndef SONSHELL_HEADLESS
#include <opencv2/opencv.hpp>
#include <opencv2/highgui/highgui_c.h>
#endif

#include "prop_names_generated.h"
#include "error_names_generated.h"

namespace SDK = SCRSDK;

// ----------------------------
// Globals
// ----------------------------
static std::vector<std::thread> g_downloadThreads;
static std::string g_download_dir;
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_shutting_down{false};
static std::atomic<bool> g_reconnect{false};
static std::string g_post_cmd; // optional executable/script to call after each photo
static std::chrono::milliseconds g_keepalive{0};
static std::thread inputThread;
static int g_wake_pipe[2] = {-1, -1};
static std::atomic<bool> g_repl_active{false};
static std::atomic<bool> g_wake_pending{false};
static std::atomic<int>  g_sync_tokens{0};   // how many callbacks are marked as boot-spawned
static std::atomic<int>  g_sync_active{0};   // how many boot-spawned workers are still running
static std::atomic<bool> g_sync_all{false};
static std::atomic<bool> g_sync_abort{false};
static std::atomic<bool> g_sync_running{false};
static std::atomic<bool> g_auto_sync_enabled{false};
static std::atomic<bool> g_sigint_requested{false};
static std::atomic<bool> g_shutdown_requested{false};
static std::atomic<bool> g_force_close_requested{false};
static std::atomic<bool> g_force_close_logged{false};
static std::atomic<int>  g_sigint_count{0};

static std::mutex        g_monitor_mtx;
static std::thread       g_monitor_thread;
static std::atomic<bool> g_monitor_running{false};
static std::atomic<bool> g_monitor_stop_flag{false};
static constexpr const char* kMonitorWindowName = "sonshell-monitor";
static std::atomic<std::uint64_t> g_last_contents_update_slot1{0};
static std::atomic<std::uint64_t> g_last_contents_update_slot2{0};
static std::mutex g_rating_mtx;
static std::unordered_map<std::uint64_t, int> g_last_known_ratings;

struct InputMapDevice {
  std::string path;
  std::unordered_map<int, std::vector<std::string>> key_to_command;
};
static std::vector<std::thread> g_input_device_threads;
static std::mutex g_command_runner_mutex;
static std::condition_variable g_command_runner_cv;
static std::function<int(const std::vector<std::string>&)> g_command_runner;
static std::mutex g_command_exec_mutex;

static const char* camera_power_status_to_string(SDK::CrCameraPowerStatus status) {
  switch (status) {
    case SDK::CrCameraPowerStatus_Off: return "Off";
    case SDK::CrCameraPowerStatus_Standby: return "Standby";
    case SDK::CrCameraPowerStatus_PowerOn: return "PowerOn";
    case SDK::CrCameraPowerStatus_TransitioningFromPowerOnToStandby: return "Transitioning (On → Standby)";
    case SDK::CrCameraPowerStatus_TransitioningFromStandbyToPowerOn: return "Transitioning (Standby → On)";
    default: return "Unknown";
  }
}

static bool fetch_camera_power_status(SDK::CrDeviceHandle handle,
                                      SDK::CrCameraPowerStatus& status_out,
                                      SDK::CrError& err_out) {
  CrInt32u prop = SDK::CrDevicePropertyCode::CrDeviceProperty_CameraPowerStatus;
  SDK::CrDeviceProperty* props = nullptr;
  CrInt32 count = 0;
  err_out = SDK::GetSelectDeviceProperties(handle, 1, &prop, &props, &count);
  if (err_out != SDK::CrError_None) {
    return false;
  }
  bool ok = (count > 0);
  if (ok) {
    status_out = static_cast<SDK::CrCameraPowerStatus>(props[0].GetCurrentValue());
  }
  SDK::ReleaseDeviceProperties(handle, props);
  err_out = SDK::CrError_None;
  return ok;
}

static const char* movie_recording_state_to_string(SDK::CrMovie_Recording_State state) {
  switch (state) {
    case SDK::CrMovie_Recording_State_Not_Recording: return "NotRecording";
    case SDK::CrMovie_Recording_State_Recording: return "Recording";
    case SDK::CrMovie_Recording_State_Recording_Failed: return "RecordingFailed";
    case SDK::CrMovie_Recording_State_IntervalRec_Waiting_Record: return "IntervalWaiting";
    default: return "Unknown";
  }
}

static const char* camera_button_function_status_to_string(CrInt16u status) {
  switch (status) {
    case SDK::CrCameraButtonFunctionStatus_Idle: return "Idle";
    case SDK::CrCameraButtonFunctionStatus_AnyKeyOn: return "AnyKeyOn";
    default: return "Unknown";
  }
}

static int contents_rating_to_int(SDK::CrContentsInfo_Rating rating) {
  switch (rating) {
    case SDK::CrContentsInfo_Rating_NotRequired: return -1;
    case SDK::CrContentsInfo_Rating_Nothing: return 0;
    case SDK::CrContentsInfo_Rating_1: return 1;
    case SDK::CrContentsInfo_Rating_2: return 2;
    case SDK::CrContentsInfo_Rating_3: return 3;
    case SDK::CrContentsInfo_Rating_4: return 4;
    case SDK::CrContentsInfo_Rating_5: return 5;
    default: return -1;
  }
}

static std::string contents_rating_to_string(SDK::CrContentsInfo_Rating rating) {
  switch (rating) {
    case SDK::CrContentsInfo_Rating_NotRequired: return "NotRequired";
    case SDK::CrContentsInfo_Rating_Nothing: return "0";
    case SDK::CrContentsInfo_Rating_1: return "1";
    case SDK::CrContentsInfo_Rating_2: return "2";
    case SDK::CrContentsInfo_Rating_3: return "3";
    case SDK::CrContentsInfo_Rating_4: return "4";
    case SDK::CrContentsInfo_Rating_5: return "5";
    default: return "Unknown";
  }
}

static bool capture_date_equal(const SDK::CrCaptureDate &a,
                               const SDK::CrCaptureDate &b) {
  return a.year == b.year && a.month == b.month && a.day == b.day &&
         a.hour == b.hour && a.minute == b.minute && a.sec == b.sec &&
         a.msec == b.msec;
}

static bool capture_date_newer(const SDK::CrCaptureDate &a,
                               const SDK::CrCaptureDate &b) {
  if (a.year != b.year) return a.year > b.year;
  if (a.month != b.month) return a.month > b.month;
  if (a.day != b.day) return a.day > b.day;
  if (a.hour != b.hour) return a.hour > b.hour;
  if (a.minute != b.minute) return a.minute > b.minute;
  if (a.sec != b.sec) return a.sec > b.sec;
  return a.msec > b.msec;
}

static std::string camera_mode_to_string(int mode_value) {
  switch (mode_value) {
    case SDK::CrCameraOperatingMode_Record: return "record";
    case SDK::CrCameraOperatingMode_Playback: return "playback";
    default: {
      std::ostringstream oss;
      oss << "mode_0x" << std::hex << std::uppercase << mode_value;
      return oss.str();
    }
  }
}

static std::string movie_mode_to_string(CrInt64u raw) {
  auto mode = static_cast<SDK::CrMovieShootingMode>(static_cast<CrInt16u>(raw));
  switch (mode) {
    case SDK::CrMovieShootingMode_Off: return "photo";
    case SDK::CrMovieShootingMode_CineEI: return "cine_ei";
    case SDK::CrMovieShootingMode_CineEIQuick: return "cine_ei_quick";
    case SDK::CrMovieShootingMode_Custom: return "custom";
    case SDK::CrMovieShootingMode_FlexibleISO: return "flexible_iso";
    default: {
      std::ostringstream oss;
      oss << "movie_0x" << std::hex << std::uppercase << static_cast<CrInt16u>(raw);
      return oss.str();
    }
  }
}

static std::string sq_mode_token(CrInt64u raw) {
  auto value = static_cast<SDK::CrSQModeSetting>(static_cast<CrInt8u>(raw & 0xFF));
  switch (value) {
    case SDK::CrSQModeSetting_On: return "sq";
    case SDK::CrSQModeSetting_Off:
    default:
      return {};
  }
}

static std::string exposure_program_code(CrInt64u raw) {
  auto mode = static_cast<SDK::CrExposureProgram>(static_cast<CrInt32u>(raw));
  switch (mode) {
    case SDK::CrExposure_M_Manual: return "m";
    case SDK::CrExposure_P_Auto: return "p";
    case SDK::CrExposure_A_AperturePriority: return "a";
    case SDK::CrExposure_S_ShutterSpeedPriority: return "s";
    case SDK::CrExposure_Auto: return "auto";
    case SDK::CrExposure_Auto_Plus: return "auto_plus";
    case SDK::CrExposure_Program_Creative: return "creative";
    case SDK::CrExposure_Program_Action: return "action";
    case SDK::CrExposure_Portrait: return "portrait";
    case SDK::CrExposure_Landscape: return "landscape";
    case SDK::CrExposure_Sports_Action: return "sports";
    case SDK::CrExposure_Sunset: return "sunset";
    case SDK::CrExposure_Night: return "night";
    case SDK::CrExposure_NightPortrait: return "night_portrait";
    case SDK::CrExposure_Macro: return "macro";
    case SDK::CrExposure_HandheldTwilight: return "handheld_twilight";
    case SDK::CrExposure_Pet: return "pet";
    case SDK::CrExposure_Gourmet: return "gourmet";
    case SDK::CrExposure_AntiMotionBlur: return "anti_motion_blur";
    default: {
      std::ostringstream oss;
      oss << "exp_0x" << std::hex << std::uppercase << static_cast<CrInt32u>(raw);
      return oss.str();
    }
  }
}

static bool is_movie_file(const SDK::CrContentsFile &file) {
  switch (file.fileFormat) {
    case SDK::CrContentsFile_FileFormat_Mp4:
      return true;
    default:
      return false;
  }
}


static std::string hex_code(CrInt64u value) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::uppercase << value;
  return oss.str();
}

static std::string decode_cr_string(const CrInt16u* raw) {
  if (!raw) return {};
  CrInt16u length = *raw;
  if (length <= 1) return {};
  std::string out;
  out.reserve(length);
  const CrInt16u* p = raw + 1;
  for (int i = 0; i < length - 1; ++i, ++p) {
    // SDK strings are UTF-16; best effort conversion assuming ASCII subset.
    out.push_back(static_cast<char>(*p & 0xFF));
  }
  return out;
}

static std::string format_f_number(CrInt64u raw) {
  CrInt32u val = static_cast<CrInt32u>(raw & 0xFFFF);
  if (val == 0 || val == SDK::CrFnumber_Unknown || val == SDK::CrFnumber_Nothing) return "f/--";
  double f = static_cast<double>(val) / 100.0;
  std::ostringstream oss;
  oss << "f/";
  if (std::fabs(f - std::round(f)) < 0.05) {
    oss << std::fixed << std::setprecision(0) << f;
  } else if (f < 10.0) {
    oss << std::fixed << std::setprecision(1) << f;
  } else {
    oss << std::fixed << std::setprecision(0) << f;
  }
  return oss.str();
}

static std::string format_shutter_speed(CrInt64u raw) {
  CrInt32u val = static_cast<CrInt32u>(raw);
  if (val == SDK::CrShutterSpeed_Bulb) return "Bulb";
  if (val == SDK::CrShutterSpeed_Nothing || val == 0) return "--";
  CrInt16u numerator = static_cast<CrInt16u>(val >> 16);
  CrInt16u denominator = static_cast<CrInt16u>(val & 0xFFFFu);
  if (denominator == 0) return hex_code(val);
  std::ostringstream oss;
  if (numerator == 1) {
    oss << "1/" << denominator;
  } else if (numerator % denominator == 0) {
    oss << (numerator / denominator) << '\"';
  } else {
    double seconds = static_cast<double>(numerator) / static_cast<double>(denominator);
    if (seconds < 10.0) {
      oss << std::fixed << std::setprecision(2) << seconds << '\"';
    } else {
      oss << std::fixed << std::setprecision(1) << seconds << '\"';
    }
  }
  return oss.str();
}

static std::string format_iso_value(CrInt64u raw) {
  CrInt32u iso = static_cast<CrInt32u>(raw);
  CrInt32u iso_mode = (iso >> 24) & 0x0F;
  CrInt32u iso_value = (iso & 0x00FFFFFFu);
  std::ostringstream oss;
  if (iso_mode == SDK::CrISO_MultiFrameNR) oss << "Multi NR ";
  else if (iso_mode == SDK::CrISO_MultiFrameNR_High) oss << "Multi NR High ";
  if (iso_value == SDK::CrISO_AUTO) oss << "ISO AUTO";
  else oss << "ISO " << iso_value;
  return oss.str();
}

static std::string format_iso_current(CrInt64u raw) {
  CrInt32u iso = static_cast<CrInt32u>(raw);
  if (iso == 0 || iso == SDK::CrISO_AUTO) return {};
  std::ostringstream oss;
  oss << "ISO " << iso;
  return oss.str();
}

static std::string format_exposure_compensation(CrInt64u raw) {
  CrInt16 val = static_cast<CrInt16>(static_cast<CrInt16u>(raw & 0xFFFFu));
  double ev = static_cast<double>(val) / 1000.0;
  if (std::fabs(ev) < 0.001) return "0";
  std::ostringstream oss;
  oss << std::showpos;
  if (std::fabs(ev - std::round(ev)) < 0.05) {
    oss << std::fixed << std::setprecision(0) << ev;
  } else if (std::fabs(ev * 2.0 - std::round(ev * 2.0)) < 0.05) {
    oss << std::fixed << std::setprecision(1) << ev;
  } else {
    oss << std::fixed << std::setprecision(2) << ev;
  }
  return oss.str();
}

static std::string trim_copy(std::string s) {
  auto begin = std::find_if_not(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
  auto end = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (begin >= end) return {};
  return std::string(begin, end);
}

static std::string join_args(const std::vector<std::string>& args, size_t start) {
  if (start >= args.size()) return {};
  std::string out = args[start];
  for (size_t i = start + 1; i < args.size(); ++i) {
    out.push_back(' ');
    out.append(args[i]);
  }
  return out;
}

static std::string to_lower_ascii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static std::string normalize_identifier(std::string s) {
  std::string trimmed = trim_copy(std::move(s));
  std::string out;
  out.reserve(trimmed.size());
  for (char ch : trimmed) {
    unsigned char c = static_cast<unsigned char>(ch);
    if (std::isspace(c) || ch == '-' || ch == '_' || ch == '+') continue;
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

static bool parse_exposure_mode_token(const std::string& raw, SDK::CrExposureProgram& out) {
  std::string key = normalize_identifier(raw);
  if (key.empty()) return false;
  static const std::pair<const char*, SDK::CrExposureProgram> kModeMap[] = {
    {"manual", SDK::CrExposure_M_Manual},
    {"m", SDK::CrExposure_M_Manual},
    {"program", SDK::CrExposure_P_Auto},
    {"p", SDK::CrExposure_P_Auto},
    {"creative", SDK::CrExposure_Program_Creative},
    {"action", SDK::CrExposure_Program_Action},
    {"aperturepriority", SDK::CrExposure_A_AperturePriority},
    {"aperture", SDK::CrExposure_A_AperturePriority},
    {"a", SDK::CrExposure_A_AperturePriority},
    {"shutterpriority", SDK::CrExposure_S_ShutterSpeedPriority},
    {"shutter", SDK::CrExposure_S_ShutterSpeedPriority},
    {"s", SDK::CrExposure_S_ShutterSpeedPriority},
    {"auto", SDK::CrExposure_Auto},
    {"autoplus", SDK::CrExposure_Auto_Plus},
    {"sports", SDK::CrExposure_Sports_Action},
    {"sportsaction", SDK::CrExposure_Sports_Action},
    {"sunset", SDK::CrExposure_Sunset},
    {"night", SDK::CrExposure_Night},
    {"landscape", SDK::CrExposure_Landscape},
    {"portrait", SDK::CrExposure_Portrait},
    {"macro", SDK::CrExposure_Macro},
    {"handheldtwilight", SDK::CrExposure_HandheldTwilight},
    {"nightportrait", SDK::CrExposure_NightPortrait},
    {"antimotionblur", SDK::CrExposure_AntiMotionBlur},
    {"pet", SDK::CrExposure_Pet},
    {"gourmet", SDK::CrExposure_Gourmet},
    {"moviep", SDK::CrExposure_Movie_P},
    {"moviea", SDK::CrExposure_Movie_A},
    {"movies", SDK::CrExposure_Movie_S},
    {"moviem", SDK::CrExposure_Movie_M},
    {"movieauto", SDK::CrExposure_Movie_Auto}
  };
  for (auto const& entry : kModeMap) {
    if (key == entry.first) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

static bool parse_iso_token(const std::string& raw, CrInt32u& out) {
  std::string token = trim_copy(raw);
  if (token.empty()) return false;
  std::string lower = to_lower_ascii(token);
  if (lower.rfind("iso", 0) == 0) {
    lower.erase(0, 3);
    lower = trim_copy(lower);
  }
  if (lower.empty()) return false;
  if (lower == "auto" || lower == "a") {
    out = SDK::CrISO_AUTO;
    return true;
  }
  char* endptr = nullptr;
  long val = std::strtol(lower.c_str(), &endptr, 10);
  if (!lower.empty() && endptr && *endptr == '\0' && val > 0 && val <= 0xFFFFFF) {
    out = static_cast<CrInt32u>(val) & 0x00FFFFFFu;
    return true;
  }
  return false;
}

static bool parse_fnumber_token(const std::string& raw, CrInt16u& out) {
  std::string token = trim_copy(raw);
  if (token.empty()) return false;
  if (!token.empty() && (token[0] == 'f' || token[0] == 'F')) {
    token.erase(0, 1);
    if (!token.empty() && (token[0] == '/' || token[0] == '\\')) {
      token.erase(0, 1);
    }
  }
  token = trim_copy(token);
  if (token.empty()) return false;
  try {
    double value = std::stod(token);
    if (!(value > 0.0)) return false;
    double scaled = std::round(value * 100.0);
    if (scaled <= 0.0 || scaled > 0xFFFF) return false;
    out = static_cast<CrInt16u>(scaled);
    return true;
  } catch (...) {
    return false;
  }
}

static bool encode_shutter_seconds(double seconds, CrInt32u& out) {
  if (!(seconds > 0.0)) return false;
  const int kMaxDen = 1000;
  int numerator = 0;
  int denominator = 0;
  bool found = false;
  for (int d = 1; d <= kMaxDen; ++d) {
    double n = seconds * static_cast<double>(d);
    double rounded = std::round(n);
    if (std::fabs(n - rounded) < 1e-4) {
      numerator = static_cast<int>(rounded);
      denominator = d;
      found = true;
      break;
    }
  }
  if (!found) {
    denominator = kMaxDen;
    numerator = static_cast<int>(std::round(seconds * static_cast<double>(denominator)));
  }
  if (numerator <= 0 || denominator <= 0) return false;
  int g = std::gcd(numerator, denominator);
  numerator /= g;
  denominator /= g;
  if (numerator <= 0 || numerator > 0xFFFF || denominator <= 0 || denominator > 0xFFFF) return false;
  out = (static_cast<CrInt32u>(numerator) << 16) | static_cast<CrInt32u>(denominator);
  return true;
}

static bool parse_shutter_token(const std::string& raw, CrInt32u& out) {
  std::string token = trim_copy(raw);
  if (token.empty()) return false;
  std::string lower = to_lower_ascii(token);
  if (lower == "bulb") {
    out = SDK::CrShutterSpeed_Bulb;
    return true;
  }
  auto slash = lower.find('/');
  if (slash != std::string::npos) {
    std::string num_str = trim_copy(lower.substr(0, slash));
    std::string den_str = trim_copy(lower.substr(slash + 1));
    if (num_str.empty() || den_str.empty()) return false;
    try {
      int numerator = std::stoi(num_str);
      int denominator = std::stoi(den_str);
      if (numerator <= 0 || denominator <= 0 || numerator > 0xFFFF || denominator > 0xFFFF) return false;
      out = (static_cast<CrInt32u>(numerator) << 16) | static_cast<CrInt32u>(denominator);
      return true;
    } catch (...) {
      return false;
    }
  }

  while (!lower.empty() && (lower.back() == '"' || std::isspace(static_cast<unsigned char>(lower.back())))) {
    lower.pop_back();
  }
  if (!lower.empty() && lower.back() == 's') {
    lower.pop_back();
  }
  if (lower.size() >= 3 && lower.substr(lower.size() - 3) == "sec") {
    lower.erase(lower.size() - 3);
  }
  lower = trim_copy(lower);
  if (lower.empty()) return false;

  try {
    double seconds = std::stod(lower);
    return encode_shutter_seconds(seconds, out);
  } catch (...) {
    return false;
  }
}

static bool parse_exposure_comp_token(const std::string& raw, CrInt16& out) {
  std::string token = trim_copy(raw);
  if (token.empty()) return false;
  std::string lower = to_lower_ascii(token);
  if (lower.size() > 2 && lower.substr(lower.size() - 2) == "ev") {
    lower.erase(lower.size() - 2);
    lower = trim_copy(lower);
  }
  if (lower == "reset" || lower == "0" || lower == "+0" || lower == "-0") {
    out = 0;
    return true;
  }
  auto slash = lower.find('/');
  double value = 0.0;
  if (slash != std::string::npos) {
    std::string num_str = trim_copy(lower.substr(0, slash));
    std::string den_str = trim_copy(lower.substr(slash + 1));
    if (den_str.empty()) return false;
    try {
      double numerator = std::stod(num_str);
      double denominator = std::stod(den_str);
      if (denominator == 0.0) return false;
      value = numerator / denominator;
    } catch (...) {
      return false;
    }
  } else {
    try {
      value = std::stod(lower);
    } catch (...) {
      return false;
    }
  }
  if (!std::isfinite(value)) return false;
  double scaled = std::round(value * 1000.0);
  if (scaled < std::numeric_limits<CrInt16>::min() || scaled > std::numeric_limits<CrInt16>::max()) return false;
  out = static_cast<CrInt16>(scaled);
  return true;
}

using ExposureHandler = int(*)(SDK::CrDeviceHandle, bool, const std::vector<std::string>&, size_t);

struct ExposureSubcommand {
  const char* name;
  const char* usage;
  size_t min_args;
  size_t max_args;
  ExposureHandler handler;
};

static constexpr size_t kExposureUnlimitedArgs = std::numeric_limits<size_t>::max();

static int exposure_show_handler(SDK::CrDeviceHandle handle, bool verbose,
                                 const std::vector<std::string>& args, size_t start_index);
static int exposure_mode_handler(SDK::CrDeviceHandle handle, bool verbose,
                                 const std::vector<std::string>& args, size_t start_index);
static int exposure_iso_handler(SDK::CrDeviceHandle handle, bool verbose,
                                const std::vector<std::string>& args, size_t start_index);
static int exposure_aperture_handler(SDK::CrDeviceHandle handle, bool verbose,
                                     const std::vector<std::string>& args, size_t start_index);
static int exposure_shutter_handler(SDK::CrDeviceHandle handle, bool verbose,
                                    const std::vector<std::string>& args, size_t start_index);
static int exposure_comp_handler(SDK::CrDeviceHandle handle, bool verbose,
                                 const std::vector<std::string>& args, size_t start_index);
static void log_exposure_usage();

static constexpr const char* kExposureUsageShow = "usage: exposure show";
static constexpr const char* kExposureUsageMode =
    "usage: exposure mode [manual|program|aperture|shutter|auto|autoplus|sports|sunset|... ]";
static constexpr const char* kExposureUsageIso = "usage: exposure iso [value]";
static constexpr const char* kExposureUsageAperture = "usage: exposure aperture [f-number]";
static constexpr const char* kExposureUsageShutter = "usage: exposure shutter [value]";
static constexpr const char* kExposureUsageComp = "usage: exposure comp [value]";

static const std::array<ExposureSubcommand, 12> kExposureSubcommands = {{
  {"show", kExposureUsageShow, 0, 0, &exposure_show_handler},
  {"mode", kExposureUsageMode, 0, kExposureUnlimitedArgs, &exposure_mode_handler},
  {"iso", kExposureUsageIso, 0, kExposureUnlimitedArgs, &exposure_iso_handler},
  {"sensitivity", kExposureUsageIso, 0, kExposureUnlimitedArgs, &exposure_iso_handler},
  {"aperture", kExposureUsageAperture, 0, kExposureUnlimitedArgs, &exposure_aperture_handler},
  {"f", kExposureUsageAperture, 0, kExposureUnlimitedArgs, &exposure_aperture_handler},
  {"fnumber", kExposureUsageAperture, 0, kExposureUnlimitedArgs, &exposure_aperture_handler},
  {"shutter", kExposureUsageShutter, 0, kExposureUnlimitedArgs, &exposure_shutter_handler},
  {"speed", kExposureUsageShutter, 0, kExposureUnlimitedArgs, &exposure_shutter_handler},
  {"comp", kExposureUsageComp, 0, kExposureUnlimitedArgs, &exposure_comp_handler},
  {"compensation", kExposureUsageComp, 0, kExposureUnlimitedArgs, &exposure_comp_handler},
  {"ev", kExposureUsageComp, 0, kExposureUnlimitedArgs, &exposure_comp_handler}
}};

static const ExposureSubcommand* find_exposure_subcommand(const std::string& key) {
  for (auto const& entry : kExposureSubcommands) {
    if (key == entry.name) {
      return &entry;
    }
  }
  return nullptr;
}

static std::string exposure_program_to_string(CrInt64u raw) {
  auto mode = static_cast<SDK::CrExposureProgram>(static_cast<CrInt32u>(raw));
  switch (mode) {
    case SDK::CrExposure_M_Manual: return "Manual";
    case SDK::CrExposure_P_Auto: return "Program";
    case SDK::CrExposure_A_AperturePriority: return "Aperture Priority";
    case SDK::CrExposure_S_ShutterSpeedPriority: return "Shutter Priority";
    case SDK::CrExposure_Program_Creative: return "Creative";
    case SDK::CrExposure_Program_Action: return "Action";
    case SDK::CrExposure_Portrait: return "Portrait";
    case SDK::CrExposure_Auto: return "Auto";
    case SDK::CrExposure_Auto_Plus: return "Auto+";
    case SDK::CrExposure_Sports_Action: return "Sports";
    case SDK::CrExposure_Sunset: return "Sunset";
    case SDK::CrExposure_Night: return "Night";
    case SDK::CrExposure_Landscape: return "Landscape";
    case SDK::CrExposure_Macro: return "Macro";
    case SDK::CrExposure_HandheldTwilight: return "Handheld Twilight";
    case SDK::CrExposure_NightPortrait: return "Night Portrait";
    case SDK::CrExposure_AntiMotionBlur: return "Anti Motion Blur";
    case SDK::CrExposure_Pet: return "Pet";
    case SDK::CrExposure_Gourmet: return "Gourmet";
    default: break;
  }
  return hex_code(static_cast<CrInt32u>(raw));
}

static std::string exposure_program_label_with_token(SDK::CrExposureProgram mode) {
  switch (mode) {
    case SDK::CrExposure_M_Manual: return "Manual (M)";
    case SDK::CrExposure_P_Auto: return "Program (P)";
    case SDK::CrExposure_A_AperturePriority: return "Aperture Priority (A)";
    case SDK::CrExposure_S_ShutterSpeedPriority: return "Shutter Priority (S)";
    case SDK::CrExposure_Auto: return "Auto";
    case SDK::CrExposure_Auto_Plus: return "Auto+";
    default:
      break;
  }
  return exposure_program_to_string(static_cast<CrInt64u>(mode));
}

static std::string format_mode_requirement_list(std::initializer_list<SDK::CrExposureProgram> modes) {
  std::vector<std::string> labels;
  labels.reserve(modes.size());
  for (auto mode : modes) {
    labels.push_back(exposure_program_label_with_token(mode));
  }
  if (labels.empty()) return {};
  if (labels.size() == 1) return labels.front();
  if (labels.size() == 2) return labels[0] + " or " + labels[1];
  std::string out;
  for (size_t i = 0; i < labels.size(); ++i) {
    if (i > 0) {
      out += (i + 1 == labels.size()) ? ", or " : ", ";
    }
    out += labels[i];
  }
  return out;
}

static std::string drive_mode_to_string(CrInt64u raw) {
  auto mode = static_cast<SDK::CrDriveMode>(static_cast<CrInt32u>(raw));
  switch (mode) {
    case SDK::CrDrive_Single: return "Single";
    case SDK::CrDrive_Continuous_Hi: return "Cont High";
    case SDK::CrDrive_Continuous_Hi_Plus: return "Cont High+";
    case SDK::CrDrive_Continuous_Lo: return "Cont Low";
    case SDK::CrDrive_Continuous: return "Continuous";
    case SDK::CrDrive_Continuous_SpeedPriority: return "Cont Speed Priority";
    case SDK::CrDrive_Continuous_Mid: return "Cont Mid";
    case SDK::CrDrive_Continuous_Lo_Live: return "Cont Low Live";
    case SDK::CrDrive_SingleBurstShooting_lo: return "Single Burst (Lo)";
    case SDK::CrDrive_SingleBurstShooting_mid: return "Single Burst (Mid)";
    case SDK::CrDrive_SingleBurstShooting_hi: return "Single Burst (Hi)";
    case SDK::CrDrive_FocusBracket: return "Focus Bracket";
    case SDK::CrDrive_Timelapse: return "Timelapse";
    case SDK::CrDrive_Timer_2s: return "Self Timer 2s";
    case SDK::CrDrive_Timer_5s: return "Self Timer 5s";
    case SDK::CrDrive_Timer_10s: return "Self Timer 10s";
    default: break;
  }
  return hex_code(static_cast<CrInt32u>(raw));
}

static std::string focus_mode_to_string(CrInt64u raw) {
  auto mode = static_cast<SDK::CrFocusMode>(static_cast<CrInt16u>(raw));
  switch (mode) {
    case SDK::CrFocus_MF: return "MF";
    case SDK::CrFocus_AF_S: return "AF-S";
    case SDK::CrFocus_AF_C: return "AF-C";
    case SDK::CrFocus_AF_A: return "AF-A";
    case SDK::CrFocus_AF_D: return "AF-D";
    case SDK::CrFocus_DMF: return "DMF";
    case SDK::CrFocus_PF: return "PF";
    default: break;
  }
  return hex_code(static_cast<CrInt16u>(raw));
}

static std::string focus_area_to_string(CrInt64u raw) {
  auto area = static_cast<SDK::CrFocusArea>(static_cast<CrInt16u>(raw));
  switch (area) {
    case SDK::CrFocusArea_Wide: return "Wide";
    case SDK::CrFocusArea_Zone: return "Zone";
    case SDK::CrFocusArea_Center: return "Center";
    case SDK::CrFocusArea_Flexible_Spot_S: return "Flexible Spot (S)";
    case SDK::CrFocusArea_Flexible_Spot_M: return "Flexible Spot (M)";
    case SDK::CrFocusArea_Flexible_Spot_L: return "Flexible Spot (L)";
    case SDK::CrFocusArea_Expand_Flexible_Spot: return "Expand Flexible Spot";
    case SDK::CrFocusArea_Flexible_Spot: return "Flexible Spot";
    case SDK::CrFocusArea_Tracking_Wide: return "Tracking Wide";
    case SDK::CrFocusArea_Tracking_Zone: return "Tracking Zone";
    case SDK::CrFocusArea_Tracking_Center: return "Tracking Center";
    case SDK::CrFocusArea_Tracking_Flexible_Spot_S: return "Tracking Flex (S)";
    case SDK::CrFocusArea_Tracking_Flexible_Spot_M: return "Tracking Flex (M)";
    case SDK::CrFocusArea_Tracking_Flexible_Spot_L: return "Tracking Flex (L)";
    case SDK::CrFocusArea_Tracking_Expand_Flexible_Spot: return "Tracking Expand Flex";
    default: break;
  }
  return hex_code(static_cast<CrInt16u>(raw));
}

static std::string white_balance_to_string(CrInt64u raw) {
  auto wb = static_cast<SDK::CrWhiteBalanceSetting>(static_cast<CrInt16u>(raw));
  switch (wb) {
    case SDK::CrWhiteBalance_AWB: return "Auto";
    case SDK::CrWhiteBalance_Underwater_Auto: return "Underwater Auto";
    case SDK::CrWhiteBalance_Daylight: return "Daylight";
    case SDK::CrWhiteBalance_Shadow: return "Shade";
    case SDK::CrWhiteBalance_Cloudy: return "Cloudy";
    case SDK::CrWhiteBalance_Tungsten: return "Tungsten";
    case SDK::CrWhiteBalance_Fluorescent: return "Fluorescent";
    case SDK::CrWhiteBalance_Fluorescent_WarmWhite: return "Fluorescent Warm";
    case SDK::CrWhiteBalance_Fluorescent_CoolWhite: return "Fluorescent Cool";
    case SDK::CrWhiteBalance_Fluorescent_DayWhite: return "Fluorescent Day";
    case SDK::CrWhiteBalance_Fluorescent_Daylight: return "Fluorescent Daylight";
    case SDK::CrWhiteBalance_Flush: return "Flash";
    case SDK::CrWhiteBalance_ColorTemp: return "Color Temp";
    case SDK::CrWhiteBalance_Custom_1: return "Custom 1";
    case SDK::CrWhiteBalance_Custom_2: return "Custom 2";
    case SDK::CrWhiteBalance_Custom_3: return "Custom 3";
    case SDK::CrWhiteBalance_Custom: return "Custom";
    default: break;
  }
  return hex_code(static_cast<CrInt16u>(raw));
}

static std::string steady_shot_to_string(CrInt64u raw) {
  auto mode = static_cast<SDK::CrImageStabilizationSteadyShot>(static_cast<CrInt8u>(raw));
  switch (mode) {
    case SDK::CrImageStabilizationSteadyShot_Off: return "Off";
    case SDK::CrImageStabilizationSteadyShot_On: return "On";
    default: break;
  }
  return hex_code(static_cast<CrInt8u>(raw));
}

static std::string steady_shot_movie_to_string(CrInt64u raw) {
  auto mode = static_cast<SDK::CrImageStabilizationSteadyShotMovie>(static_cast<CrInt8u>(raw));
  switch (mode) {
    case SDK::CrImageStabilizationSteadyShotMovie_Off: return "Off";
    case SDK::CrImageStabilizationSteadyShotMovie_Standard: return "Standard";
    case SDK::CrImageStabilizationSteadyShotMovie_Active: return "Active";
    case SDK::CrImageStabilizationSteadyShotMovie_DynamicActive: return "Dynamic Active";
    default: break;
  }
  return hex_code(static_cast<CrInt8u>(raw));
}

static std::string silent_mode_to_string(CrInt64u raw) {
  auto mode = static_cast<SDK::CrSilentMode>(static_cast<CrInt8u>(raw));
  switch (mode) {
    case SDK::CrSilentMode_Off: return "Off";
    case SDK::CrSilentMode_On: return "On";
    default: break;
  }
  return hex_code(static_cast<CrInt8u>(raw));
}

static std::string shutter_type_to_string(CrInt64u raw) {
  auto type = static_cast<SDK::CrShutterType>(static_cast<CrInt8u>(raw));
  switch (type) {
    case SDK::CrShutterType_Auto: return "Auto";
    case SDK::CrShutterType_MechanicalShutter: return "Mechanical";
    case SDK::CrShutterType_ElectronicShutter: return "Electronic";
    default: break;
  }
  return hex_code(static_cast<CrInt8u>(raw));
}

static std::string movie_mode_to_display_string(CrInt64u raw) {
  auto mode = static_cast<SDK::CrMovieShootingMode>(static_cast<CrInt16u>(raw));
  switch (mode) {
    case SDK::CrMovieShootingMode_Off: return "Off";
    case SDK::CrMovieShootingMode_CineEI: return "Cine EI";
    case SDK::CrMovieShootingMode_CineEIQuick: return "Cine EI Quick";
    case SDK::CrMovieShootingMode_Custom: return "Custom";
    case SDK::CrMovieShootingMode_FlexibleISO: return "Flexible ISO";
    default: break;
  }
  return hex_code(static_cast<CrInt16u>(raw));
}

static std::string movie_media_to_string(CrInt64u raw) {
  auto media = static_cast<SDK::CrRecordingMediaMovie>(static_cast<CrInt16u>(raw));
  switch (media) {
    case SDK::CrRecordingMediaMovie_Slot1: return "Slot 1";
    case SDK::CrRecordingMediaMovie_Slot2: return "Slot 2";
    case SDK::CrRecordingMediaMovie_SimultaneousRecording: return "Simul";
    default: break;
  }
  return hex_code(static_cast<CrInt16u>(raw));
}

static std::string movie_recording_setting_to_string(CrInt64u raw) {
  auto setting = static_cast<SDK::CrRecordingSettingMovie>(static_cast<CrInt16u>(raw));
  switch (setting) {
    case SDK::CrRecordingSettingMovie_60p_50M: return "60p 50M XAVC S";
    case SDK::CrRecordingSettingMovie_30p_50M: return "30p 50M XAVC S";
    case SDK::CrRecordingSettingMovie_24p_50M: return "24p 50M XAVC S";
    case SDK::CrRecordingSettingMovie_50p_50M: return "50p 50M XAVC S";
    case SDK::CrRecordingSettingMovie_25p_50M: return "25p 50M XAVC S";
    case SDK::CrRecordingSettingMovie_60i_24M: return "60i 24M AVCHD";
    case SDK::CrRecordingSettingMovie_50i_24M_FX: return "50i 24M AVCHD";
    case SDK::CrRecordingSettingMovie_60i_17M_FH: return "60i 17M AVCHD";
    case SDK::CrRecordingSettingMovie_50i_17M_FH: return "50i 17M AVCHD";
    case SDK::CrRecordingSettingMovie_60p_28M_PS: return "60p 28M AVCHD";
    case SDK::CrRecordingSettingMovie_50p_28M_PS: return "50p 28M AVCHD";
    case SDK::CrRecordingSettingMovie_24p_24M_FX: return "24p 24M AVCHD";
    case SDK::CrRecordingSettingMovie_25p_24M_FX: return "25p 24M AVCHD";
    case SDK::CrRecordingSettingMovie_24p_17M_FH: return "24p 17M AVCHD";
    case SDK::CrRecordingSettingMovie_25p_17M_FH: return "25p 17M AVCHD";
    case SDK::CrRecordingSettingMovie_120p_50M_1280x720: return "120p 50M 720p XAVC S";
    case SDK::CrRecordingSettingMovie_100p_50M_1280x720: return "100p 50M 720p XAVC S";
    case SDK::CrRecordingSettingMovie_1920x1080_30p_16M: return "1080 30p 16M MP4";
    case SDK::CrRecordingSettingMovie_1920x1080_25p_16M: return "1080 25p 16M MP4";
    case SDK::CrRecordingSettingMovie_1280x720_30p_6M: return "720 30p 6M MP4";
    case SDK::CrRecordingSettingMovie_1280x720_25p_6M: return "720 25p 6M MP4";
    case SDK::CrRecordingSettingMovie_1920x1080_60p_28M: return "1080 60p 28M MP4";
    case SDK::CrRecordingSettingMovie_1920x1080_50p_28M: return "1080 50p 28M MP4";
    default: break;
  }
  return hex_code(static_cast<CrInt16u>(raw));
}

static std::string focus_bracket_shots_to_string(CrInt64u raw) {
  CrInt32u val = static_cast<CrInt32u>(raw);
  if (val == 0) return {};
  std::ostringstream oss;
  oss << val;
  return oss.str();
}

static std::string focus_bracket_range_to_string(CrInt64u raw) {
  CrInt32u val = static_cast<CrInt32u>(raw);
  if (val == 0) return {};
  std::ostringstream oss;
  oss << val;
  return oss.str();
}

struct PropertyValue {
  bool supported = false;
  CrInt64u value = 0;
  std::string text;
};

static PropertyValue fetch_property(SDK::CrDeviceHandle handle, CrInt32u code) {
  PropertyValue out;
  SDK::CrDeviceProperty* props = nullptr;
  CrInt32 count = 0;
  auto err = SDK::GetSelectDeviceProperties(handle, 1, &code, &props, &count);
  if (err != SDK::CrError_None || count <= 0 || !props) {
    return out;
  }
  const auto flag = props[0].GetPropertyEnableFlag();
  if (flag != SDK::CrEnableValue_NotSupported && flag != SDK::CrEnableValue_False) {
    out.supported = true;
    out.value = props[0].GetCurrentValue();
    if (props[0].GetValueType() == SDK::CrDataType::CrDataType_STR) {
      out.text = decode_cr_string(props[0].GetCurrentStr());
    }
  }
  SDK::ReleaseDeviceProperties(handle, props);
  return out;
}

static std::string capture_mode_string(SDK::CrDeviceHandle handle,
                                       const SDK::CrContentsInfo &info,
                                       const SDK::CrContentsFile &file) {
  (void)info;
  auto mode_prop = fetch_property(handle, SDK::CrDeviceProperty_CameraOperatingMode);
  int mode_value = mode_prop.supported
                       ? static_cast<int>(mode_prop.value & 0xFFFF)
                       : static_cast<int>(SDK::CrCameraOperatingMode_Record);
  std::string base = camera_mode_to_string(mode_value);

  if (is_movie_file(file)) {
    std::string detail = "movie";
    auto movie_mode = fetch_property(handle, SDK::CrDeviceProperty_MovieShootingMode);
    if (movie_mode.supported) {
      std::string movie_token = movie_mode_to_string(movie_mode.value);
      if (!movie_token.empty() && movie_token != "photo") {
        detail += "/" + movie_token;
      }
    }
    auto sq_mode = fetch_property(handle, SDK::CrDeviceProperty_SQModeSetting);
    if (sq_mode.supported) {
      std::string sq = sq_mode_token(sq_mode.value);
      if (!sq.empty()) detail += "/" + sq;
    }
    return base + "/" + detail;
  }

  std::string detail = "still";
  auto exposure_mode = fetch_property(handle, SDK::CrDeviceProperty_ExposureProgramMode);
  if (exposure_mode.supported) {
    std::string exp_code = exposure_program_code(exposure_mode.value);
    if (!exp_code.empty()) detail += "/" + exp_code;
  }
  return base + "/" + detail;
}

static std::string current_mode_string(SDK::CrDeviceHandle handle) {
  auto mode_prop = fetch_property(handle, SDK::CrDeviceProperty_CameraOperatingMode);
  int mode_value = mode_prop.supported ? static_cast<int>(mode_prop.value & 0xFFFF) : -1;
  std::string base = camera_mode_to_string(mode_value);

  if (mode_value == SDK::CrCameraOperatingMode_Record) {
    std::string detail;
    auto movie_mode = fetch_property(handle, SDK::CrDeviceProperty_MovieShootingMode);
    if (movie_mode.supported) {
      std::string movie_token = movie_mode_to_string(movie_mode.value);
      if (!movie_token.empty() && movie_token != "photo") {
        detail = movie_token;
      }
    }

    auto sq_mode = fetch_property(handle, SDK::CrDeviceProperty_SQModeSetting);
    if (sq_mode.supported) {
      std::string sq = sq_mode_token(sq_mode.value);
      if (!sq.empty()) {
        if (!detail.empty()) detail += "/" + sq;
        else detail = sq;
      }
    }

    if (!detail.empty() && detail != base) {
      return base + "/" + detail;
    }
  }

  return base;
}

struct StatusSnapshot {
  std::string model = "--";
  std::string lens = "--";
  std::string serial = "--";
  std::string f_number = "--";
  std::string shutter = "--";
  std::string iso = "--";
  std::string iso_actual;
  std::string exposure_program = "--";
  std::string drive_mode = "--";
  std::string focus_mode = "--";
  std::string focus_area = "--";
  std::string focus_bracket_shots;
  std::string focus_bracket_range;
  std::string white_balance = "--";
  std::string steady_still = "--";
  std::string steady_movie = "--";
  std::string silent_mode = "--";
  std::string shutter_type = "--";
  std::string movie_mode = "--";
  std::string movie_setting = "--";
  std::string movie_media = "--";
  std::string recording_state = "--";
};

static bool collect_status_snapshot(SDK::CrDeviceHandle handle, StatusSnapshot& snap, bool verbose) {
  bool any = false;

  auto assign_formatted = [&](CrInt32u code, auto formatter, std::string& target) {
    PropertyValue p = fetch_property(handle, code);
    if (!p.supported) return;
    target = formatter(p.value);
    any = true;
  };

  PropertyValue model = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_ModelName);
  if (model.supported && !model.text.empty()) { snap.model = model.text; any = true; }

  PropertyValue lens = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_LensModelName);
  if (lens.supported && !lens.text.empty()) { snap.lens = lens.text; any = true; }

  PropertyValue serial = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_BodySerialNumber);
  if (serial.supported && !serial.text.empty()) { snap.serial = serial.text; any = true; }

  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_FNumber, format_f_number, snap.f_number);
  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_ShutterSpeed, format_shutter_speed, snap.shutter);
  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_IsoSensitivity, format_iso_value, snap.iso);

  PropertyValue iso_actual = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_IsoCurrentSensitivity);
  if (iso_actual.supported) {
    snap.iso_actual = format_iso_current(iso_actual.value);
    any = true;
  }

  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_ExposureProgramMode, exposure_program_to_string, snap.exposure_program);
  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_DriveMode, drive_mode_to_string, snap.drive_mode);
  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_FocusMode, focus_mode_to_string, snap.focus_mode);
  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_FocusArea, focus_area_to_string, snap.focus_area);
  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_WhiteBalance, white_balance_to_string, snap.white_balance);
  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_ImageStabilizationSteadyShot, steady_shot_to_string, snap.steady_still);
  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_Movie_ImageStabilizationSteadyShot, steady_shot_movie_to_string, snap.steady_movie);
  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_SilentMode, silent_mode_to_string, snap.silent_mode);
  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_ShutterType, shutter_type_to_string, snap.shutter_type);
  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_MovieShootingMode, movie_mode_to_display_string, snap.movie_mode);
  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_Movie_Recording_Setting, movie_recording_setting_to_string, snap.movie_setting);
  assign_formatted(SDK::CrDevicePropertyCode::CrDeviceProperty_Movie_RecordingMedia, movie_media_to_string, snap.movie_media);

  PropertyValue rec_state = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_RecordingState);
  if (rec_state.supported) {
    snap.recording_state = movie_recording_state_to_string(static_cast<SDK::CrMovie_Recording_State>(static_cast<CrInt16u>(rec_state.value)));
    any = true;
  }

  PropertyValue bracket_shots = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_FocusBracketShotNumber);
  if (bracket_shots.supported) {
    snap.focus_bracket_shots = focus_bracket_shots_to_string(bracket_shots.value);
    any = true;
  }
  PropertyValue bracket_range = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_FocusBracketFocusRange);
  if (bracket_range.supported) {
    snap.focus_bracket_range = focus_bracket_range_to_string(bracket_range.value);
    any = true;
  }

  return any;
}

static bool fetch_movie_recording_state(SDK::CrDeviceHandle handle,
                                        SDK::CrMovie_Recording_State& state_out,
                                        SDK::CrError& err_out) {
  CrInt32u prop = SDK::CrDevicePropertyCode::CrDeviceProperty_RecordingState;
  SDK::CrDeviceProperty* props = nullptr;
  CrInt32 count = 0;
  err_out = SDK::GetSelectDeviceProperties(handle, 1, &prop, &props, &count);
  if (err_out != SDK::CrError_None) {
    return false;
  }
  bool ok = (count > 0);
  if (ok) {
    state_out = static_cast<SDK::CrMovie_Recording_State>(props[0].GetCurrentValue());
  }
  SDK::ReleaseDeviceProperties(handle, props);
  err_out = SDK::CrError_None;
  return ok;
}

#ifndef SONSHELL_HEADLESS
static bool monitor_window_is_alive() {
  try {
    void* handle = cvGetWindowHandle(kMonitorWindowName);
    if (handle) return true;
  } catch (const cv::Exception&) {
  }
  try {
    double visible = cv::getWindowProperty(kMonitorWindowName, cv::WND_PROP_VISIBLE);
    return visible > 0.0;
  } catch (const cv::Exception&) {
  }
  return false;
}

static void monitor_join_stale_thread() {
  std::thread stale;
  {
    std::lock_guard<std::mutex> lk(g_monitor_mtx);
    if (g_monitor_running.load(std::memory_order_acquire)) return;
    if (!g_monitor_thread.joinable()) return;
    stale = std::move(g_monitor_thread);
  }
  if (stale.joinable()) {
    stale.join();
  }
}
#else
static bool monitor_window_is_alive() {
  return false;
}

static void monitor_join_stale_thread() {
  std::thread stale;
  {
    std::lock_guard<std::mutex> lk(g_monitor_mtx);
    if (g_monitor_running.load(std::memory_order_acquire)) return;
    if (!g_monitor_thread.joinable()) return;
    stale = std::move(g_monitor_thread);
  }
  if (stale.joinable()) {
    stale.join();
  }
}
#endif

static unsigned char repl_trigger_shoot(EditLine* el, int) {
  el_push(el, "shoot\n");
  return CC_REFRESH;
}

// ----------------------------
// Logging
// ----------------------------
enum class LogLevel { Info, Warn, Error, Debug };

struct LogItem {
  LogLevel level;
  std::string text;
};
static bool g_stdout_is_tty = isatty(STDOUT_FILENO);
static bool g_stderr_is_tty = isatty(STDERR_FILENO);

static const char* log_color(LogLevel lvl) {
  switch (lvl) {
    case LogLevel::Info:  return "\033[36m";  // cyan
    case LogLevel::Warn:  return "\033[33m";  // yellow
    case LogLevel::Error: return "\033[31m";  // red
    case LogLevel::Debug: return "\033[90m";  // dim gray
  }
  return "\033[0m";
}

static const char* log_label(LogLevel lvl) {
  switch (lvl) {
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Debug: return "DEBUG";
  }
  return "LOG";
}

static inline bool use_color_for(std::ostream& os) {
  return (&os == &std::cerr) ? g_stderr_is_tty : g_stdout_is_tty;
}

static inline void write_log_line(LogLevel lvl, const std::string& text, std::ostream& os) {
  if (text.empty()) return;

  std::string content = text;
  if (content.empty() || content.front() != '[') {
    content = std::string("[") + log_label(lvl) + "] " + content;
  }

  std::ostringstream line;
  line << std::left << std::setw(5) << log_label(lvl) << " | " << content;
  if (use_color_for(os)) {
    os << log_color(lvl) << line.str() << "\033[0m" << '\n';
  } else {
    os << line.str() << '\n';
  }
}

static std::mutex        g_log_mtx;
static std::condition_variable g_log_cv;
static std::deque<LogItem> g_log_q;

// Enqueue a message from any thread.
static inline void log_enqueue(LogLevel lvl, std::string msg) {
  if (!g_repl_active.load(std::memory_order_relaxed)) {
    std::ostream& os = (lvl == LogLevel::Error) ? std::cerr : std::cout;
    write_log_line(lvl, msg, os);
    os.flush();
    return;
  }
  {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    g_log_q.push_back({lvl, std::move(msg)});
  }
  g_log_cv.notify_one();

  // Write exactly one wake byte while a wake is pending
  if (g_wake_pipe[1] != -1) {
    bool expected = false;
    if (g_wake_pending.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
      char x = 0;
      (void)!write(g_wake_pipe[1], &x, 1);
    }
  }
}

// Stream-friendly macros: use like LOGI("foo " << x << " bar");
#define LOGI(expr) do { std::ostringstream _oss; _oss << expr; log_enqueue(LogLevel::Info,  _oss.str()); } while(0)
#define LOGW(expr) do { std::ostringstream _oss; _oss << expr; log_enqueue(LogLevel::Warn,  _oss.str()); } while(0)
#define LOGE(expr) do { std::ostringstream _oss; _oss << expr; log_enqueue(LogLevel::Error, _oss.str()); } while(0)
#define LOGD(expr) do { std::ostringstream _oss; _oss << expr; log_enqueue(LogLevel::Debug, _oss.str()); } while(0)

static inline void wake_repl_loop() {
  if (g_wake_pipe[1] != -1) {
    char x = 0;
    (void)!write(g_wake_pipe[1], &x, 1);
  }
}

static inline void request_shutdown(bool force) {
  g_stop.store(true, std::memory_order_release);
  g_shutting_down.store(true, std::memory_order_release);
  if (force) g_force_close_requested.store(true, std::memory_order_release);
  wake_repl_loop();
}

static inline void maybe_log_force_close() {
  if (g_force_close_requested.load(std::memory_order_acquire) &&
      !g_force_close_logged.exchange(true, std::memory_order_acq_rel)) {
    LOGW("Force-close requested; skipping remaining waits.");
  }
}

static void log_exposure_mode_hint(SDK::CrDeviceHandle handle,
                                   const char* subcommand,
                                   std::initializer_list<SDK::CrExposureProgram> required_modes) {
  std::string requirement = format_mode_requirement_list(required_modes);
  if (requirement.empty()) {
    return;
  }

  PropertyValue current_mode = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_ExposureProgramMode);
  if (current_mode.supported) {
    std::string mode_name = exposure_program_to_string(current_mode.value);
    LOGI("Hint: exposure " << subcommand << " requires " << requirement
         << "; camera is currently in " << mode_name << " mode.");
  } else {
    LOGI("Hint: exposure " << subcommand << " requires " << requirement << '.');
  }
}

static bool exposure_error_suggests_mode_change(SDK::CrError err) {
  switch (err) {
    case SDK::CrError_Api_InvalidCalled:
    case SDK::CrError_Generic_NotSupported:
    case SDK::CrError_Generic_InvalidParameter:
    case SDK::CrError_Adaptor_InvalidProperty:
      return true;
    default:
      break;
  }
  return false;
}

static bool tap_camera_button(SDK::CrDeviceHandle handle,
                              SDK::CrCameraButtonFunction button,
                              const char* label) {
  if (!handle) {
    LOGE("button: camera handle unavailable");
    return false;
  }
  auto send_edge = [&](SDK::CrCameraButtonFunctionValue edge,
                       const char* edge_name) -> bool {
    SDK::CrDeviceProperty prop;
    prop.SetCode(SDK::CrDevicePropertyCode::CrDeviceProperty_CameraButtonFunction);
    prop.SetValueType(SDK::CrDataType::CrDataType_UInt32Range);
    CrInt32u payload = static_cast<CrInt32u>(button) +
                       static_cast<CrInt32u>(edge);
    prop.SetCurrentValue(payload);
    auto err = SDK::SetDeviceProperty(handle, &prop);
    if (err != SDK::CrError_None) {
      unsigned code = static_cast<unsigned>(err);
      LOGE("button: failed to send " << label << ' ' << edge_name
           << " event: " << crsdk_err::error_to_name(err)
           << " (0x" << std::hex << code << std::dec << ")");
      return false;
    }
    return true;
  };

  if (!send_edge(SDK::CrCameraButtonFunctionValue_Down, "down")) {
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if (!send_edge(SDK::CrCameraButtonFunctionValue_Up, "up")) {
    return false;
  }
  return true;
}

static void log_command_overview() {
  LOGI("SonShell commands:");
  LOGI("  help                 Show this command overview");
  LOGI("  status               Dump a snapshot of camera settings (mode, ISO, lens, etc.)");
  LOGI("  exposure ...         Inspect or set exposure options; run 'exposure' for subcommands");
  LOGI("  shoot | trigger      Fire the shutter immediately (full press)");
  LOGI("  focus                Half-press + release to autofocus");
  LOGI("  sync [N|all|on|off]  Pull the latest files or mirror all contents; 'sync stop' aborts");
#ifdef SONSHELL_HEADLESS
  LOGI("  monitor start|stop   (disabled in headless builds)");
#else
  LOGI("  monitor start|stop   Start/stop the live-view window (requires OpenCV deps)");
#endif
  LOGI("  record start|stop    Toggle movie recording");
  LOGI("  button dpad ...      Tap the rear d-pad (left/right/up/down/center) buttons");
  LOGI("  button playback      Toggle the camera playback button");
  LOGI("  button menu          Open the camera menu (rear Menu button)");
  LOGI("  button shutter|movie Tap the top shutter or movie buttons");
  LOGI("  power off            Ask the camera to power down (half-pressing the shutter will wake it up)");
  LOGI("  quit | exit          Leave SonShell");
  LOGI("Shortcuts:");
  LOGI("  Ctrl+C               Cancel the current line and show a fresh prompt");
  LOGI("  Ctrl+D               Quit the shell (same as 'exit')");
  LOGI("  F1 / 'trigger'       Fire shutter (mapped by the REPL)");
  LOGI("Notes:");
  LOGI("  Use '--sync-dir' at launch to pick a download folder; '--cmd' runs a hook per file");
  LOGI("  Auto-download can be toggled at runtime with 'sync on' / 'sync off'");
  LOGI("  Some exposure controls require the camera's physical mode dial to match");
}

static void log_exposure_usage() {
  LOGI("usage: exposure <show|mode|iso|aperture|shutter|comp>");
  LOGI("  show                 Display current exposure metrics");
  LOGI("  mode [value]         Get or set exposure mode (manual, program, aperture, shutter, auto, ...)");
  LOGI("  iso [value]          Get or set ISO (e.g. auto, 100, 6400)");
  LOGI("  aperture [value]     Get or set aperture (e.g. f/2.8, 5.6)");
  LOGI("  shutter [value]      Get or set shutter speed (e.g. 1/125, 0.5s, bulb)");
  LOGI("  comp [value]         Get or set exposure compensation (e.g. +1.0, -0.3, reset)");
}

static int exposure_show_handler(SDK::CrDeviceHandle handle, bool verbose,
                                 const std::vector<std::string>& args, size_t start_index) {
  (void)args;
  (void)start_index;
  StatusSnapshot snap;
  bool got_any = collect_status_snapshot(handle, snap, verbose);
  if (!got_any) {
    LOGW("exposure show: camera did not report detailed properties; showing defaults.");
  }
  std::string iso_display = snap.iso;
  if (!snap.iso_actual.empty() && snap.iso_actual != snap.iso) {
    if (!iso_display.empty() && iso_display != "--") {
      iso_display += " [" + snap.iso_actual + "]";
    } else {
      iso_display = snap.iso_actual;
    }
  }
  PropertyValue comp = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_ExposureBiasCompensation);
  std::string comp_display = comp.supported ? format_exposure_compensation(comp.value) : "--";

  LOGI("Exposure:");
  LOGI("  Mode: " << snap.exposure_program);
  LOGI("  Aperture: " << snap.f_number << "  Shutter: " << snap.shutter);
  LOGI("  ISO: " << (iso_display.empty() ? std::string("--") : iso_display)
       << "  EV: " << comp_display);
  return 0;
}

static int exposure_mode_handler(SDK::CrDeviceHandle handle, bool /*verbose*/,
                                 const std::vector<std::string>& args, size_t start_index) {
  PropertyValue current = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_ExposureProgramMode);
  if (start_index >= args.size()) {
    if (!current.supported) {
      LOGW("exposure mode: camera did not report the current mode.");
      return 2;
    }
    LOGI("Exposure mode: " << exposure_program_to_string(current.value));
    return 0;
  }

  std::string input = join_args(args, start_index);
  SDK::CrExposureProgram parsed{};
  if (!parse_exposure_mode_token(input, parsed)) {
    LOGE("exposure mode: unknown mode '" << input << "'.");
    LOGI("Examples: manual, program, aperture, shutter, auto, autoplus, sports");
    return 2;
  }
  if (!current.supported) {
    LOGW("exposure mode: camera did not report support; attempting to set anyway.");
  }

  SDK::CrDeviceProperty prop;
  prop.SetCode(SDK::CrDevicePropertyCode::CrDeviceProperty_ExposureProgramMode);
  prop.SetValueType(SDK::CrDataType::CrDataType_UInt32);
  prop.SetCurrentValue(static_cast<CrInt32u>(parsed));
  auto err = SDK::SetDeviceProperty(handle, &prop);
  if (err != SDK::CrError_None) {
    unsigned code = static_cast<unsigned>(err);
    LOGE("exposure mode: failed to set value: " << crsdk_err::error_to_name(err)
         << " (0x" << std::hex << code << std::dec << ")");
    return 2;
  }

  PropertyValue confirm = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_ExposureProgramMode);
  if (confirm.supported) {
    if (confirm.value != static_cast<CrInt64u>(parsed)) {
      LOGW("exposure mode: camera is still reporting "
           << exposure_program_to_string(confirm.value)
           << " mode. Many bodies require changing the physical mode dial.");
      return 1;
    }
  } else {
    LOGW("exposure mode: camera did not report the updated value; verify on the body if it changed.");
  }

  LOGI("Exposure mode set to " << exposure_program_to_string(static_cast<CrInt64u>(parsed)));
  return 0;
}

static int exposure_iso_handler(SDK::CrDeviceHandle handle, bool /*verbose*/,
                                const std::vector<std::string>& args, size_t start_index) {
  PropertyValue current = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_IsoSensitivity);
  if (start_index >= args.size()) {
    if (!current.supported) {
      LOGW("exposure iso: camera did not report ISO sensitivity.");
      log_exposure_mode_hint(handle, "iso",
                             {SDK::CrExposure_M_Manual,
                              SDK::CrExposure_P_Auto,
                              SDK::CrExposure_A_AperturePriority,
                              SDK::CrExposure_S_ShutterSpeedPriority});
      return 2;
    }
    std::string iso_display = format_iso_value(current.value);
    PropertyValue iso_actual = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_IsoCurrentSensitivity);
    if (iso_actual.supported) {
      std::string actual = format_iso_current(iso_actual.value);
      if (!actual.empty() && actual != iso_display) {
        if (!iso_display.empty() && iso_display != "--") iso_display += " [" + actual + "]";
        else iso_display = actual;
      }
    }
    LOGI("ISO: " << (iso_display.empty() ? std::string("--") : iso_display));
    return 0;
  }

  std::string input = join_args(args, start_index);
  CrInt32u encoded = 0;
  if (!parse_iso_token(input, encoded)) {
    LOGE("exposure iso: invalid value '" << input << "'.");
    LOGI("Examples: exposure iso auto | exposure iso 100 | exposure iso 6400");
    return 2;
  }

  if (!current.supported) {
    log_exposure_mode_hint(handle, "iso",
                           {SDK::CrExposure_M_Manual,
                            SDK::CrExposure_P_Auto,
                            SDK::CrExposure_A_AperturePriority,
                            SDK::CrExposure_S_ShutterSpeedPriority});
  }

  SDK::CrDeviceProperty prop;
  prop.SetCode(SDK::CrDevicePropertyCode::CrDeviceProperty_IsoSensitivity);
  prop.SetValueType(SDK::CrDataType::CrDataType_UInt32);
  prop.SetCurrentValue(encoded);
  auto err = SDK::SetDeviceProperty(handle, &prop);
  if (err != SDK::CrError_None) {
    unsigned code = static_cast<unsigned>(err);
    LOGE("exposure iso: failed to set value: " << crsdk_err::error_to_name(err)
         << " (0x" << std::hex << code << std::dec << ")");
    if (exposure_error_suggests_mode_change(err)) {
      log_exposure_mode_hint(handle, "iso",
                             {SDK::CrExposure_M_Manual,
                              SDK::CrExposure_P_Auto,
                              SDK::CrExposure_A_AperturePriority,
                              SDK::CrExposure_S_ShutterSpeedPriority});
    }
    return 2;
  }
  LOGI("ISO sensitivity set to " << format_iso_value(static_cast<CrInt64u>(encoded)));
  return 0;
}

static int exposure_aperture_handler(SDK::CrDeviceHandle handle, bool /*verbose*/,
                                     const std::vector<std::string>& args, size_t start_index) {
  PropertyValue current = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_FNumber);
  if (start_index >= args.size()) {
    if (!current.supported) {
      LOGW("exposure aperture: camera did not report aperture value.");
      log_exposure_mode_hint(handle, "aperture",
                             {SDK::CrExposure_M_Manual,
                              SDK::CrExposure_A_AperturePriority});
      return 2;
    }
    LOGI("Aperture: " << format_f_number(current.value));
    return 0;
  }

  std::string input = join_args(args, start_index);
  CrInt16u encoded = 0;
  if (!parse_fnumber_token(input, encoded)) {
    LOGE("exposure aperture: invalid value '" << input << "'.");
    LOGI("Examples: exposure aperture f/4 | exposure aperture 2.8");
    return 2;
  }

  if (!current.supported) {
    log_exposure_mode_hint(handle, "aperture",
                           {SDK::CrExposure_M_Manual,
                            SDK::CrExposure_A_AperturePriority});
  }

  SDK::CrDeviceProperty prop;
  prop.SetCode(SDK::CrDevicePropertyCode::CrDeviceProperty_FNumber);
  prop.SetValueType(SDK::CrDataType::CrDataType_UInt16);
  prop.SetCurrentValue(encoded);
  auto err = SDK::SetDeviceProperty(handle, &prop);
  if (err != SDK::CrError_None) {
    unsigned code = static_cast<unsigned>(err);
    LOGE("exposure aperture: failed to set value: " << crsdk_err::error_to_name(err)
         << " (0x" << std::hex << code << std::dec << ")");
    if (exposure_error_suggests_mode_change(err)) {
      log_exposure_mode_hint(handle, "aperture",
                             {SDK::CrExposure_M_Manual,
                              SDK::CrExposure_A_AperturePriority});
    }
    return 2;
  }
  LOGI("Aperture set to " << format_f_number(static_cast<CrInt64u>(encoded)));
  return 0;
}

static int exposure_shutter_handler(SDK::CrDeviceHandle handle, bool /*verbose*/,
                                    const std::vector<std::string>& args, size_t start_index) {
  PropertyValue current = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_ShutterSpeed);
  if (start_index >= args.size()) {
    if (!current.supported) {
      LOGW("exposure shutter: camera did not report shutter speed.");
      log_exposure_mode_hint(handle, "shutter",
                             {SDK::CrExposure_M_Manual,
                              SDK::CrExposure_S_ShutterSpeedPriority});
      return 2;
    }
    LOGI("Shutter: " << format_shutter_speed(current.value));
    return 0;
  }

  std::string input = join_args(args, start_index);
  CrInt32u encoded = 0;
  if (!parse_shutter_token(input, encoded)) {
    LOGE("exposure shutter: invalid value '" << input << "'.");
    LOGI("Examples: exposure shutter 1/125 | exposure shutter 0.5s | exposure shutter bulb");
    return 2;
  }

  if (!current.supported) {
    log_exposure_mode_hint(handle, "shutter",
                           {SDK::CrExposure_M_Manual,
                            SDK::CrExposure_S_ShutterSpeedPriority});
  }

  SDK::CrDeviceProperty prop;
  prop.SetCode(SDK::CrDevicePropertyCode::CrDeviceProperty_ShutterSpeed);
  prop.SetValueType(SDK::CrDataType::CrDataType_UInt32);
  prop.SetCurrentValue(encoded);
  auto err = SDK::SetDeviceProperty(handle, &prop);
  if (err != SDK::CrError_None) {
    unsigned code = static_cast<unsigned>(err);
    LOGE("exposure shutter: failed to set value: " << crsdk_err::error_to_name(err)
         << " (0x" << std::hex << code << std::dec << ")");
    if (exposure_error_suggests_mode_change(err)) {
      log_exposure_mode_hint(handle, "shutter",
                             {SDK::CrExposure_M_Manual,
                              SDK::CrExposure_S_ShutterSpeedPriority});
    }
    return 2;
  }
  LOGI("Shutter speed set to " << format_shutter_speed(static_cast<CrInt64u>(encoded)));
  return 0;
}

static int exposure_comp_handler(SDK::CrDeviceHandle handle, bool /*verbose*/,
                                 const std::vector<std::string>& args, size_t start_index) {
  PropertyValue current = fetch_property(handle, SDK::CrDevicePropertyCode::CrDeviceProperty_ExposureBiasCompensation);
  if (start_index >= args.size()) {
    if (!current.supported) {
      LOGW("exposure comp: camera did not report exposure compensation.");
      log_exposure_mode_hint(handle, "comp",
                             {SDK::CrExposure_P_Auto,
                              SDK::CrExposure_A_AperturePriority,
                              SDK::CrExposure_S_ShutterSpeedPriority});
      return 2;
    }
    LOGI("Exposure compensation: " << format_exposure_compensation(current.value));
    return 0;
  }

  std::string input = join_args(args, start_index);
  CrInt16 encoded = 0;
  if (!parse_exposure_comp_token(input, encoded)) {
    LOGE("exposure comp: invalid value '" << input << "'.");
    LOGI("Examples: exposure comp +1.0 | exposure comp -0.3 | exposure comp reset");
    return 2;
  }

  if (!current.supported) {
    log_exposure_mode_hint(handle, "comp",
                           {SDK::CrExposure_P_Auto,
                            SDK::CrExposure_A_AperturePriority,
                            SDK::CrExposure_S_ShutterSpeedPriority});
  }

  SDK::CrDeviceProperty prop;
  prop.SetCode(SDK::CrDevicePropertyCode::CrDeviceProperty_ExposureBiasCompensation);
  prop.SetValueType(SDK::CrDataType::CrDataType_Int16);
  prop.SetCurrentValue(static_cast<CrInt32>(encoded));
  auto err = SDK::SetDeviceProperty(handle, &prop);
  if (err != SDK::CrError_None) {
    unsigned code = static_cast<unsigned>(err);
    LOGE("exposure comp: failed to set value: " << crsdk_err::error_to_name(err)
         << " (0x" << std::hex << code << std::dec << ")");
    if (exposure_error_suggests_mode_change(err)) {
      log_exposure_mode_hint(handle, "comp",
                             {SDK::CrExposure_P_Auto,
                              SDK::CrExposure_A_AperturePriority,
                              SDK::CrExposure_S_ShutterSpeedPriority});
    }
    return 2;
  }
  CrInt16u raw = static_cast<CrInt16u>(encoded);
  LOGI("Exposure compensation set to " << format_exposure_compensation(static_cast<CrInt64u>(raw)));
  return 0;
}

static bool send_movie_record_button_press(SDK::CrDeviceHandle handle,
                                           std::chrono::milliseconds hold_duration,
                                           bool verbose_logs) {
  if (verbose_logs) LOGI("Record: button down");
  auto down_err = SDK::SendCommand(handle,
                                   SDK::CrCommandId::CrCommandId_MovieRecord,
                                   SDK::CrCommandParam_Down);
  if (down_err != SDK::CrError_None) {
    unsigned code = static_cast<unsigned>(down_err);
    LOGE("Record: button down failed: " << crsdk_err::error_to_name(down_err)
         << " (0x" << std::hex << code << std::dec << ")");
    return false;
  }

  std::this_thread::sleep_for(hold_duration);

  if (verbose_logs) LOGI("Record: button up");
  auto up_err = SDK::SendCommand(handle,
                                 SDK::CrCommandId::CrCommandId_MovieRecord,
                                 SDK::CrCommandParam_Up);
  if (up_err != SDK::CrError_None) {
    unsigned code = static_cast<unsigned>(up_err);
    LOGE("Record: button up failed: " << crsdk_err::error_to_name(up_err)
         << " (0x" << std::hex << code << std::dec << ")");
    return false;
  }

  return true;
}

static bool trigger_full_shutter_press(SDK::CrDeviceHandle handle,
                                       bool verbose_logs,
                                       const char* label) {
  const char* tag = (label && *label) ? label : "shutter";
  if (!handle) {
    LOGE(tag << ": camera handle unavailable");
    return false;
  }

  if (verbose_logs) LOGI(tag << ": capture image...");

  SDK::CrDeviceProperty s1;
  s1.SetCode(SDK::CrDevicePropertyCode::CrDeviceProperty_S1);
  s1.SetValueType(SDK::CrDataType::CrDataType_UInt16);
  s1.SetCurrentValue(SDK::CrLockIndicator::CrLockIndicator_Locked);
  auto s1_lock_err = SDK::SetDeviceProperty(handle, &s1);
  if (s1_lock_err != SDK::CrError_None) {
    unsigned code = static_cast<unsigned>(s1_lock_err);
    LOGE(tag << ": failed to half-press shutter: " << crsdk_err::error_to_name(s1_lock_err)
         << " (0x" << std::hex << code << std::dec << ")");
    return false;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  if (verbose_logs) LOGI(tag << ": shutter down");
  auto down_err = SDK::SendCommand(handle, SDK::CrCommandId::CrCommandId_Release, SDK::CrCommandParam_Down);
  if (down_err != SDK::CrError_None) {
    unsigned code = static_cast<unsigned>(down_err);
    LOGE(tag << ": shutter down failed: " << crsdk_err::error_to_name(down_err)
         << " (0x" << std::hex << code << std::dec << ")");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(35));

  if (verbose_logs) LOGI(tag << ": shutter up");
  auto up_err = SDK::SendCommand(handle, SDK::CrCommandId::CrCommandId_Release, SDK::CrCommandParam_Up);
  if (up_err != SDK::CrError_None) {
    unsigned code = static_cast<unsigned>(up_err);
    LOGE(tag << ": shutter up failed: " << crsdk_err::error_to_name(up_err)
         << " (0x" << std::hex << code << std::dec << ")");
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  s1.SetCurrentValue(SDK::CrLockIndicator::CrLockIndicator_Unlocked);
  auto s1_unlock_err = SDK::SetDeviceProperty(handle, &s1);
  if (s1_unlock_err != SDK::CrError_None) {
    unsigned code = static_cast<unsigned>(s1_unlock_err);
    LOGE(tag << ": failed to release half-press: " << crsdk_err::error_to_name(s1_unlock_err)
         << " (0x" << std::hex << code << std::dec << ")");
  }

  return true;
}

// Drain everything that's queued and repaint the prompt.
// Call ONLY from the input thread that owns `el`.
static inline bool drain_logs_and_refresh(EditLine* el_or_null) {
  std::deque<LogItem> local;
  {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    local.swap(g_log_q);
  }

  if (local.empty()) return false;

  // clear current line once so the prompt vanishes while we print logs
  std::fputs("\r\033[K", stdout);  // CR + clear-to-end-of-line

  for (auto& it : local) {
    std::ostream& os = (it.level == LogLevel::Error) ? std::cerr : std::cout;
    write_log_line(it.level, it.text, os);
  }
  std::cout.flush();
  std::cerr.flush();

  if (el_or_null) {
    el_set(el_or_null, EL_REFRESH, 0);
    // …and NOW clear any leftover characters to the right of the cursor.
    std::fputs("\033[K", stdout); // prevents log output from slicing through a partially typed line
    std::fflush(stdout);
  }
  
  return true;
}


// Optional: blocking wait-with-timeout to reduce busy looping
static inline void wait_for_logs_or_timeout(int ms) {
  std::unique_lock<std::mutex> lk(g_log_mtx);
  if (g_log_q.empty()) {
    g_log_cv.wait_for(lk, std::chrono::milliseconds(ms));
  }
}

#ifndef SONSHELL_HEADLESS
// ----------------------------
// Live-view monitor helpers
// ----------------------------

static void monitor_thread_main(SDK::CrDeviceHandle handle, bool verbose) {

  bool window_created = false;
  bool window_resizable = false;
  bool window_size_initialized = false;
  try {
    cv::namedWindow(kMonitorWindowName, cv::WINDOW_NORMAL);
    cv::resizeWindow(kMonitorWindowName, 200, 120);
    try {
      cv::setWindowProperty(kMonitorWindowName, cv::WND_PROP_ASPECT_RATIO, cv::WINDOW_KEEPRATIO);
    } catch (const cv::Exception&) {
    }
    window_created = true;
    window_resizable = true;
  } catch (const cv::Exception& ex) {
    LOGW("[monitor] WINDOW_NORMAL unavailable (" << ex.what() << "); falling back to autosize window");
  }

  if (!window_created) {
    try {
      cv::namedWindow(kMonitorWindowName, cv::WINDOW_AUTOSIZE);
      window_created = true;
    } catch (const cv::Exception& ex) {
      LOGE("[monitor] Failed to create OpenCV window: " << ex.what());
      g_monitor_running.store(false, std::memory_order_release);
      g_monitor_stop_flag.store(false, std::memory_order_release);
      return;
    }
  }

  std::unique_ptr<SDK::CrImageDataBlock> image_block(new (std::nothrow) SDK::CrImageDataBlock());
  if (!image_block) {
    LOGE("[monitor] Unable to allocate image block");
    if (window_created) cv::destroyWindow(kMonitorWindowName);
    g_monitor_running.store(false, std::memory_order_release);
    g_monitor_stop_flag.store(false, std::memory_order_release);
    return;
  }

  std::vector<CrInt8u> buffer;
  CrInt32u last_buf_capacity = 0;

  bool window_visible_once = false;
  bool frame_displayed_once = false;
  void* window_handle = cvGetWindowHandle(kMonitorWindowName);

  while (!g_monitor_stop_flag.load(std::memory_order_acquire) &&
         !g_stop.load(std::memory_order_acquire)) {

    if (!handle) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    if (frame_displayed_once) {
      void* current_handle = cvGetWindowHandle(kMonitorWindowName);
      if (!current_handle) {
        LOGI("[monitor] window closed (no handle); stopping live view");
        g_monitor_stop_flag.store(true, std::memory_order_release);
        break;
      }
      if (current_handle != window_handle) window_handle = current_handle;
    }

    SDK::CrImageInfo info{};
    auto info_res = SDK::GetLiveViewImageInfo(handle, &info);
    if (info_res != SDK::CrError_None) {
      if (info_res != SDK::CrWarning_Frame_NotUpdated && verbose) {
        LOGE("[monitor] GetLiveViewImageInfo failed: " << crsdk_err::error_to_name(info_res)
              << " (0x" << std::hex << static_cast<unsigned>(info_res) << std::dec << ")");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    CrInt32u capacity = info.GetBufferSize();
    if (capacity == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      continue;
    }

    if (capacity != last_buf_capacity) {
      buffer.assign(capacity, 0);
      image_block->SetSize(capacity);
      image_block->SetData(buffer.data());
      last_buf_capacity = capacity;
    }

    auto lv_res = SDK::GetLiveViewImage(handle, image_block.get());
    if (lv_res == SDK::CrWarning_Frame_NotUpdated) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    if (lv_res == SDK::CrError_Memory_Insufficient) {
      if (verbose) {
        LOGE("[monitor] Live view memory insufficient");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    if (lv_res != SDK::CrError_None) {
      if (!(lv_res == SDK::CrError_Generic && !frame_displayed_once)) {
        LOGE("[monitor] Live view fetch failed: " << crsdk_err::error_to_name(lv_res)
              << " (0x" << std::hex << static_cast<unsigned>(lv_res) << std::dec << ")");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }

    CrInt32u actual = image_block->GetImageSize();
    if (actual == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    cv::Mat raw(1, static_cast<int>(actual), CV_8UC1, image_block->GetImageData());
    cv::Mat frame;
    try {
      frame = cv::imdecode(raw, cv::IMREAD_COLOR);
    } catch (const cv::Exception& ex) {
      LOGE("[monitor] imdecode error: " << ex.what());
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    if (frame.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    if (window_resizable && !window_size_initialized) {
      try {
        cv::resizeWindow(kMonitorWindowName, frame.cols, frame.rows);
        window_size_initialized = true;
      } catch (const cv::Exception&) {
      }
    }

    try {
      cv::imshow(kMonitorWindowName, frame);
      window_visible_once = true;
      frame_displayed_once = true;
      window_handle = cvGetWindowHandle(kMonitorWindowName);
    } catch (const cv::Exception& ex) {
      LOGE("[monitor] imshow error: " << ex.what());
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    int key = cv::waitKey(1);
    (void)key;  // reserved for future hotkeys; keep calling waitKey to pump events

  }

  if (window_created && monitor_window_is_alive()) {
    try {
      cv::destroyWindow(kMonitorWindowName);
    } catch (const cv::Exception& ex) {
      LOGE("[monitor] destroyWindow error: " << ex.what());
    }
  }

  g_monitor_running.store(false, std::memory_order_release);
  g_monitor_stop_flag.store(false, std::memory_order_release);
  LOGI("[monitor] stopped");
}

static bool monitor_start(SDK::CrDeviceHandle handle, bool verbose) {
  monitor_join_stale_thread();

  if (!handle) {
    LOGE("Camera handle not available; cannot start monitor.");
    return false;
  }

  {
    std::lock_guard<std::mutex> lk(g_monitor_mtx);
    if (g_monitor_running.load(std::memory_order_acquire)) {
      LOGI("[monitor] already running");
      return true;
    }
    g_monitor_stop_flag.store(false, std::memory_order_release);
  }

  CrInt32u live_view_enabled = 0;
  auto get_res = SDK::GetDeviceSetting(handle, SDK::Setting_Key_EnableLiveView, &live_view_enabled);
  if (get_res != SDK::CrError_None || live_view_enabled == 0) {
    auto set_res = SDK::SetDeviceSetting(handle, SDK::Setting_Key_EnableLiveView, 1);
    if (set_res != SDK::CrError_None) {
      LOGE("[monitor] Failed to enable live view: " << crsdk_err::error_to_name(set_res)
           << " (0x" << std::hex << static_cast<unsigned>(set_res) << std::dec << ")");
      return false;
    }
  }

  try {
    std::lock_guard<std::mutex> lk(g_monitor_mtx);
    g_monitor_running.store(true, std::memory_order_release);
    g_monitor_thread = std::thread(monitor_thread_main, handle, verbose);
  } catch (const std::exception& ex) {
    LOGE("[monitor] Failed to launch thread: " << ex.what());
    g_monitor_running.store(false, std::memory_order_release);
    g_monitor_stop_flag.store(false, std::memory_order_release);
    return false;
  }

  LOGI("[monitor] started (run 'monitor stop' to close the window)");
  return true;
}

static void monitor_stop() {
  std::thread local;
  {
    std::lock_guard<std::mutex> lk(g_monitor_mtx);
    if (!g_monitor_thread.joinable()) {
      g_monitor_running.store(false, std::memory_order_release);
      g_monitor_stop_flag.store(false, std::memory_order_release);
      return;
    }
    g_monitor_stop_flag.store(true, std::memory_order_release);
    if (monitor_window_is_alive()) {
      try {
        cv::destroyWindow(kMonitorWindowName);
      } catch (const cv::Exception& ex) {
        LOGE("[monitor] destroyWindow error: " << ex.what());
      }
    }
    local = std::move(g_monitor_thread);
  }

  if (local.joinable()) {
    local.join();
  }
  g_monitor_running.store(false, std::memory_order_release);
  g_monitor_stop_flag.store(false, std::memory_order_release);
}
#else
// Headless build stubs -------------------------------------------------------
static bool monitor_start(SDK::CrDeviceHandle handle, bool verbose) {
  (void)handle;
  (void)verbose;
  LOGW("[monitor] Live view disabled in headless build (rebuild without -DSONSHELL_HEADLESS=ON)");
  return false;
}

static void monitor_stop() {
  monitor_join_stale_thread();
  g_monitor_running.store(false, std::memory_order_release);
  g_monitor_stop_flag.store(false, std::memory_order_release);
}
#endif

// poll()-based getchar so logs can wake the REPL
static int my_getc(EditLine* el, char* c) {
  if (!el || !c) return 0;

  struct pollfd fds[2];
  int nfds = 1;
  fds[0].fd = STDIN_FILENO;   fds[0].events = POLLIN; fds[0].revents = 0;
  fds[1].fd = -1;             fds[1].events = POLLIN; fds[1].revents = 0;
  if (g_wake_pipe[0] != -1) { fds[1].fd = g_wake_pipe[0]; nfds = 2; }

  for (;;) {
    if (g_sigint_requested.exchange(false, std::memory_order_relaxed)) {
      tcflush(STDIN_FILENO, TCIFLUSH);
      el_reset(el);
      *c = '\n';
      return 1;
    }

    int r = poll(fds, nfds, -1);
    if (r < 0) {
      if (errno == EINTR) {
	if (g_stop.load(std::memory_order_relaxed) || g_reconnect.load(std::memory_order_relaxed)) return 0; // make el_gets() exit
	continue; // spurious EINTR while not stopping
      }
      return -1;
    }

    // Wake pipe: drain, repaint prompt, and continue waiting for real input
    if (nfds == 2 && (fds[1].revents & POLLIN)) {
      char buf[256];
      while (true) {
	ssize_t n = read(g_wake_pipe[0], buf, sizeof(buf));
	if (n <= 0) break;
      }
      g_wake_pending.store(false, std::memory_order_relaxed);

      (void)drain_logs_and_refresh(el);

      // Terminate read now if we're stopping or reconnecting
      if (g_stop.load(std::memory_order_relaxed) || g_reconnect.load(std::memory_order_relaxed)) return 0;

      continue;
    }

    // Real input
    if (fds[0].revents & POLLIN) {
      ssize_t n = read(STDIN_FILENO, c, 1);
      if (n == 1) {
	if (*c == 4) { // Ctrl-D -> EOF
	  g_stop.store(true, std::memory_order_relaxed);
	  return 0; // el_gets() sees EOF
	}
	if (*c == 3) { // Ctrl-C -> cancel current input line
	  tcflush(STDIN_FILENO, TCIFLUSH);
	  el_reset(el);
	  *c = '\n';
	  return 1;
	}
	return 1;
      }
      if (n == 0) {
	g_stop.store(true, std::memory_order_relaxed);
	return 0;          // EOF
      }
      if (errno == EINTR) continue;
      return -1;                      // error
    }
  }
}

// ----------------------------
// Signals
// ----------------------------
static void signal_handler(int sig) {
  if (sig == SIGINT) {
    if (g_repl_active.load(std::memory_order_relaxed) &&
        !g_shutdown_requested.load(std::memory_order_relaxed)) {
      g_sigint_requested.store(true, std::memory_order_relaxed);
      return;
    }
    int count = g_sigint_count.fetch_add(1, std::memory_order_relaxed) + 1;
    bool force = (count >= 2);
    g_shutdown_requested.store(true, std::memory_order_release);
    request_shutdown(force);
    if (force && count >= 3) {
      _exit(130);
    }
    return;
  }
  g_shutdown_requested.store(true, std::memory_order_release);
  request_shutdown(true);
}

static void install_signal_handlers() {
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // Avoid zombies from post-cmd children
  struct sigaction sa_chld{};
  sa_chld.sa_handler = SIG_IGN;
  sigemptyset(&sa_chld.sa_mask);
  sa_chld.sa_flags = 0;
  sigaction(SIGCHLD, &sa_chld, nullptr);
}

// call this early in main, before starting inputThread:
static void block_sigint_in_this_thread() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

// in inputThread start (first lines inside the lambda), unblock:
static void unblock_sigint_in_this_thread() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
}

// ----------------------------
// FS helpers
// ----------------------------
static std::string join_path(const std::string &d, const std::string &n) {
  char sep = '/';
  if (d.empty()) return n;
  if (d.back() == sep) return d + n;
  return d + sep + n;
}

static std::string basename_from_path(const char *p) {
  if (!p) return {};
  std::string s(p);
  size_t pos = s.find_last_of("/\\");
  return (pos == std::string::npos) ? s : s.substr(pos + 1);
}

static std::string unique_name(const std::string &dir, const std::string &base) {
  std::string target = join_path(dir, base);
  if (!std::filesystem::exists(target)) return base;
  std::string name = base, ext;
  size_t dot = base.find_last_of('.');
  if (dot != std::string::npos && dot > 0 && dot + 1 < base.size()) {
    name = base.substr(0, dot);
    ext = base.substr(dot);
  }
  for (int i = 1; i < 1000000; ++i) {
    std::ostringstream o; o << name << "_" << i << ext;
    if (!std::filesystem::exists(join_path(dir, o.str()))) return o.str();
  }
  return base;
}

static std::string get_cache_dir() {
  const char *h = std::getenv("HOME");
  if (h) return std::string(h) + "/.cache/sonshell";
  return ".";
}

static std::string expand_user_path(const std::string& path) {
  if (path.empty() || path[0] != '~') return path;
  const char* home = std::getenv("HOME");
  if (!home || !*home) return path;
  if (path.size() == 1) return std::string(home);
  if (path[1] == '/' || path[1] == '\\') {
    return std::string(home) + path.substr(1);
  }
  // We don't support ~otheruser; return original string.
  return path;
}

static std::string default_input_map_path() {
  const char* home = std::getenv("HOME");
  if (!home || !*home) return {};
  return std::string(home) + "/.config/sonshell/input-map.yaml";
}

static bool ensure_sync_directory_configured(const char* context) {
  if (!g_download_dir.empty()) return true;
  const char* tag = (context && *context) ? context : "sync";
  LOGE(tag << ": no sync directory configured; restart with '--sync-dir <path>' to enable transfers.");
  return false;
}

static bool load_fingerprint(const std::string &path, std::vector<char> &buf) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) return false;
  buf.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  return true;
}

static bool save_fingerprint(const std::string &path, const char *data, size_t len) {
  std::ofstream ofs(path, std::ios::binary);
  if (!ofs) return false;
  ofs.write(data, static_cast<std::streamsize>(len));
  return ofs.good();
}

static std::string dirname_from_path(const char *p) {
  if (!p) return {};
  std::string s(p);
  // strip trailing slash(es)
  while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
  size_t pos = s.find_last_of("/\\");
  if (pos == std::string::npos || pos == 0) return {};  // empty or root-like
  // drop any leading slash to keep it relative under g_download_dir
  std::string d = s.substr(0, pos);
  if (!d.empty() && (d[0] == '/' || d[0] == '\\')) d.erase(0, 1);
  return d;
}

// ----------------------------
// Net helpers
// ----------------------------
static uint32_t ip_to_uint32(const std::string &ip) {
  unsigned a, b, c, d; char dot;
  std::istringstream iss(ip);
  if (!(iss >> a >> dot >> b >> dot >> c >> dot >> d)) return 0;
  if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
  return (a << 24) | (b << 16) | (c << 8) | d;
}

static bool parse_mac(const std::string &mac_str, unsigned char mac[6]) {
  unsigned int v[6]; char col;
  std::istringstream iss(mac_str);
  if (!(iss >> std::hex >> v[0] >> col >> v[1] >> col >> v[2] >> col >> v[3] >> col >> v[4] >> col >> v[5])) return false;
  for (int i = 0; i < 6; ++i) {
    if (v[i] > 0xff) return false;
    mac[i] = static_cast<unsigned char>(v[i]);
  }
  return true;
}

// ----------------------------
// Misc helpers
// ----------------------------

static std::string ascii_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static std::string ascii_upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return s;
}

static std::string collapse_delims(std::string s) {
  s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) {
    return c == '-' || c == '_' || c == ' ';
  }), s.end());
  return s;
}

static std::string normalize_model_key(const std::string &value) {
  return collapse_delims(ascii_lower(value));
}

struct CameraModelChoice {
  std::string canonical;
  std::optional<SDK::CrCameraDeviceModelList> direct_value;
};

struct CameraModelInfo {
  const char *canonical;
  std::optional<SDK::CrCameraDeviceModelList> direct_value;
  std::initializer_list<const char *> extra_aliases;
};

static std::vector<std::string> heuristic_aliases(const std::string &canonical) {
  std::vector<std::string> aliases;
  std::string norm = normalize_model_key(canonical);
  aliases.push_back(norm);

  auto add_unique = [&](std::string alias) {
    if (alias.empty()) return;
    if (std::find(aliases.begin(), aliases.end(), alias) == aliases.end()) {
      aliases.push_back(std::move(alias));
    }
  };

  auto add_strip_prefix = [&](const std::string &prefix) {
    if (norm.rfind(prefix, 0) == 0 && norm.size() > prefix.size()) {
      add_unique(norm.substr(prefix.size()));
    }
  };

  // ILCE-* bodies → add a/N variants (drops the "M" before numbers too).
  if (norm.rfind("ilce", 0) == 0 && norm.size() > 4) {
    std::string rest = norm.substr(4);
    add_unique("a" + rest);

    std::string trimmed = "a" + rest;
    bool changed = false;
    for (size_t i = 0; i + 1 < trimmed.size(); ++i) {
      if (trimmed[i] == 'm' && std::isdigit(static_cast<unsigned char>(trimmed[i + 1]))) {
        trimmed.erase(i, 1);
        --i;
        changed = true;
      }
    }
    if (changed) add_unique(trimmed);
  }

  add_strip_prefix("ilme"); // cinema/FX bodies → allow "fx3" style
  add_strip_prefix("dsc");  // compact bodies → allow "rx0m2"
  add_strip_prefix("zv");   // vlog cameras → allow "e1", "e10m2"
  add_strip_prefix("ilx");  // industrial bodies → allow "lr1"
  add_strip_prefix("mpc");
  add_strip_prefix("pxw");
  add_strip_prefix("hxr");
  add_strip_prefix("brc");

  return aliases;
}

static const std::vector<CameraModelInfo> &camera_model_infos() {
  static const std::vector<CameraModelInfo> infos = {
    {"ILCE-7RM4", SDK::CrCameraDeviceModel_ILCE_7RM4, {}},
    {"ILCE-9M2", SDK::CrCameraDeviceModel_ILCE_9M2, {}},
    {"ILCE-7C", SDK::CrCameraDeviceModel_ILCE_7C, {}},
    {"ILCE-7SM3", SDK::CrCameraDeviceModel_ILCE_7SM3, {}},
    {"ILCE-1", SDK::CrCameraDeviceModel_ILCE_1, {}},
    {"ILCE-7RM4A", SDK::CrCameraDeviceModel_ILCE_7RM4A, {}},
    {"DSC-RX0M2", SDK::CrCameraDeviceModel_DSC_RX0M2, {}},
    {"ILCE-7M4", SDK::CrCameraDeviceModel_ILCE_7M4, {}},
    {"ILME-FX3", SDK::CrCameraDeviceModel_ILME_FX3, {}},
    {"ILME-FX30", SDK::CrCameraDeviceModel_ILME_FX30, {}},
    {"ILME-FX6", SDK::CrCameraDeviceModel_ILME_FX6, {}},
    {"ILCE-7RM5", SDK::CrCameraDeviceModel_ILCE_7RM5, {}},
    {"ZV-E1", SDK::CrCameraDeviceModel_ZV_E1, {}},
    {"ILCE-6700", SDK::CrCameraDeviceModel_ILCE_6700, {}},
    {"ILCE-7CM2", SDK::CrCameraDeviceModel_ILCE_7CM2, {}},
    {"ILCE-7CR", SDK::CrCameraDeviceModel_ILCE_7CR, {}},
    {"ILX-LR1", SDK::CrCameraDeviceModel_ILX_LR1, {}},
    {"MPC-2610", SDK::CrCameraDeviceModel_MPC_2610, {}},
    {"ILCE-9M3", SDK::CrCameraDeviceModel_ILCE_9M3, {}},
    {"ZV-E10M2", SDK::CrCameraDeviceModel_ZV_E10M2, {}},
    {"PXW-Z200", SDK::CrCameraDeviceModel_PXW_Z200, {}},
    {"HXR-NX800", SDK::CrCameraDeviceModel_HXR_NX800, {}},
    {"ILCE-1M2", SDK::CrCameraDeviceModel_ILCE_1M2, {}},
    {"ILME-FX3A", SDK::CrCameraDeviceModel_ILME_FX3A, {}},
    {"BRC-AM7", SDK::CrCameraDeviceModel_BRC_AM7, {}},
    {"ILME-FR7", SDK::CrCameraDeviceModel_ILME_FR7, {}},
    {"ILME-FX2", SDK::CrCameraDeviceModel_ILME_FX2, {}},
    // Legacy/extra entries that lack enums in this SDK version
    {"ILCE-6000", std::nullopt, {"a6000"}},
  };
  return infos;
}

static CameraModelChoice resolve_camera_model_choice(const std::string &keyword) {
  if (keyword.empty()) {
    return {std::string{}, std::nullopt};
  }

  static std::unordered_map<std::string, const CameraModelInfo*> lookup;
  if (lookup.empty()) {
    for (auto const &info : camera_model_infos()) {
      auto aliases = heuristic_aliases(info.canonical);
      for (auto const *extra : info.extra_aliases) {
        aliases.push_back(normalize_model_key(extra));
      }
      for (auto &alias : aliases) {
        if (alias.empty()) continue;
        lookup.emplace(alias, &info);
      }
    }
  }

  std::string key = normalize_model_key(keyword);
  auto it = lookup.find(key);
  if (it != lookup.end()) {
    const CameraModelInfo *info = it->second;
    return {info->canonical, info->direct_value};
  }

  // Fall back to using the caller-provided string as canonical.
  return {ascii_upper(keyword), std::nullopt};
}

static bool model_name_matches(const SDK::ICrCameraObjectInfo *info,
                               const std::string &expected) {
  if (!info) return false;
  if (expected.empty()) return true;
  const char *model_ptr = reinterpret_cast<const char*>(info->GetModel());
  if (!model_ptr) return false;
  std::string actual(model_ptr);
  return ascii_upper(actual) == ascii_upper(expected);
}

static inline void interruptible_sleep(std::chrono::milliseconds total) {
  using namespace std::chrono;
  auto deadline = steady_clock::now() + total;
  while (!g_stop.load(std::memory_order_relaxed)) {
    auto now = steady_clock::now();
    if (now >= deadline) break;
    auto chunk = std::min(milliseconds(100), duration_cast<milliseconds>(deadline - now));
    std::this_thread::sleep_for(chunk);
  }
}

// ----------------------------
// Post-download command
// ----------------------------
static void run_post_cmd_args(const std::string &path,
                              const std::vector<std::string> &args) {
  if (path.empty()) return;

  pid_t pid = fork();
  if (pid == 0) {
    std::vector<char *> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char *>(path.c_str()));
    for (const auto &arg : args) {
      argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);

    execv(path.c_str(), argv.data());
    _exit(127);
  }
  // parent: we don't wait (SIGCHLD is ignored)
}

static void run_post_cmd(const std::string &path,
                         const std::string &file,
                         const std::string &mode,
                         const std::string &command,
                         const std::string &old_value = {},
                         const std::string &new_value = {}) {
  std::vector<std::string> args{file, mode, command};
  if (!old_value.empty() || !new_value.empty()) {
    args.push_back(old_value);
    if (!new_value.empty()) {
      args.push_back(new_value);
    }
  }
  run_post_cmd_args(path, args);
}

class QuietCallback : public SDK::IDeviceCallback {
public:
  SDK::CrDeviceHandle device_handle = 0;
  bool verbose = false;

  // handshake
  std::mutex mtx;
  std::condition_variable conn_cv;
  bool connected = false;
  bool conn_finished = false;
  CrInt32u last_error_code = 0;

  // download sync
  std::mutex dl_mtx;
  std::condition_variable dl_cv;
  bool dl_waiting = false;
  CrInt32u dl_notify_code = 0;
  CrInt32u dl_progress = 0;
  std::string last_downloaded_file;
  // inside class QuietCallback (near other dl_* fields)
  CrInt32u dl_last_log_per = 101; // 0..100; 101 = "unset"
  std::chrono::steady_clock::time_point dl_last_log_tp{};
  std::string dl_current_label; // e.g., "PRIVATE/M4ROOT/CLIP/DSC01234.MP4"
  std::chrono::steady_clock::time_point dl_start_tp{};
  bool dl_any_progress = false;
  std::string dl_current_mode;
  std::string dl_current_operation;
  
  void OnConnected(SDK::DeviceConnectionVersioin v) override {
    if (g_shutting_down.load()) return;
    if (verbose) LOGI( "[CB] OnConnected v=" << v );
    {
      std::lock_guard<std::mutex> lk(mtx); connected = true; conn_finished = true;
    }
    conn_cv.notify_all();
  }

  void OnDisconnected(CrInt32u error) override {
    if (g_shutting_down.load()) return;
    if (verbose) {
      LOGI( "[CB] OnDisconnected: 0x" << std::hex << error << std::dec << " (" << crsdk_err::error_to_name(error) << ")" );
    }
    {
      std::lock_guard<std::mutex> lk(mtx); last_error_code = error; conn_finished = true;
    }
    conn_cv.notify_all();
    {
      std::lock_guard<std::mutex> lk(dl_mtx); dl_waiting = false;
    }
    dl_cv.notify_all();
    g_reconnect.store(true);
  }

  void OnWarning(CrInt32u w) override {
    if (g_shutting_down.load()) return;
    if (verbose) {
      LOGI( "[CB] OnWarning: " << crsdk_err::warning_to_name(w) << " (0x" << std::hex << w << std::dec << ")" );
    }
  }

  void OnWarningExt(CrInt32u warning, CrInt32 param1, CrInt32 param2, CrInt32 param3) override {
    if (g_shutting_down.load()) return;
    LOGI("[CB] OnWarningExt: " << crsdk_err::warning_to_name(warning)
         << " (0x" << std::hex << warning << std::dec << ")"
         << " p1=0x" << std::hex << param1
         << " p2=0x" << param2
         << " p3=0x" << param3 << std::dec
         << " | p1=" << param1 << ", p2=" << param2 << ", p3=" << param3);
  }

  void OnError(CrInt32u e) override {
    if (g_shutting_down.load()) return;
    LOGI( "[CB] OnError: " << crsdk_err::error_to_name(e) << " (0x" << std::hex << e << std::dec << ")" );
    {
      std::lock_guard<std::mutex> lk(mtx); last_error_code = e; conn_finished = true;
    }
    conn_cv.notify_all();
  }

private:
  void log_changed_properties_(const char *tag) {
    if (!device_handle) return;
    SDK::CrDeviceProperty *props = nullptr; CrInt32 nprop = 0;
    auto er = SDK::GetDeviceProperties(device_handle, &props, &nprop);
    if (er != SDK::CrError_None || nprop <= 0 || !props) return;
    for (int i = 0; i < nprop; ++i) {
      CrInt32u code = props[i].GetCode();
      CrInt64u val = props[i].GetCurrentValue();
      auto it = last_prop_vals.find(code);
      bool had_prev = (it != last_prop_vals.end());
      CrInt64u prev = had_prev ? it->second : 0;
      if (!had_prev || prev != val) {
      last_prop_vals[code] = val;
        if (verbose) {
          const char *name = crsdk_util::prop_code_to_name(code);
          std::ostringstream msg;
          msg << tag << ": " << name << " (0x" << std::hex << code << std::dec << ") -> "
              << (long long)val;
          if (had_prev) {
            msg << " (prev=" << (long long)prev << ')';
          }
          LOGI(msg.str());
        }
        if (code == SDK::CrDeviceProperty_CameraButtonFunctionStatus) {
          auto status = static_cast<CrInt16u>(val & 0xFFFF);
          if (status == SDK::CrCameraButtonFunctionStatus_AnyKeyOn) {
            schedule_playback_button_job();
          }
        }
      }
    }
    SDK::ReleaseDeviceProperties(device_handle, props);
  }

public:
  
  void OnPropertyChanged() override {
    if (!g_shutting_down.load()) log_changed_properties_("[CB] OnPropertyChanged");
  }
  
  void OnLvPropertyChanged() override {
    if (!g_shutting_down.load()) log_changed_properties_("[CB] OnLvPropertyChanged");
  }

  void schedule_playback_button_job() {
    if (g_shutting_down.load()) return;
    g_downloadThreads.emplace_back([this]() {
      this->process_playback_button_job();
    });
  }

  bool download_single_content_file(SDK::CrSlotNumber slot,
                                    const SDK::CrContentsInfo &info,
                                    const SDK::CrContentsFile &file,
                                    std::string &local_path,
                                    bool skip_existing) {
    std::string orig = basename_from_path(file.filePath);
    if (orig.empty()) {
      std::ostringstream o;
      o << "content_" << static_cast<unsigned long long>(info.contentId)
        << "_file_" << file.fileId;
      orig = o.str();
    }

    std::string relDir = dirname_from_path(file.filePath);
    std::string destDir = g_download_dir;
    if (!relDir.empty()) {
      destDir = destDir.empty() ? relDir : join_path(destDir, relDir);
    }

    std::error_code ec;
    if (!destDir.empty()) {
      std::filesystem::create_directories(destDir, ec);
    }

    std::string finalName = orig;
    std::string candidatePath = join_path(destDir, finalName);
    local_path = candidatePath;

    if (skip_existing) {
      std::error_code exists_ec;
      if (std::filesystem::exists(candidatePath, exists_ec) && !exists_ec) {
        return true;
      }
    }

    dl_waiting = true;
    dl_current_label = join_path(relDir, finalName);
    dl_last_log_per = 101;
    dl_last_log_tp = std::chrono::steady_clock::now();
    dl_start_tp = dl_last_log_tp;
    dl_any_progress = false;

    CrChar *saveDir = destDir.empty()
                        ? nullptr
                        : const_cast<CrChar *>(reinterpret_cast<const CrChar *>(destDir.c_str()));
    CrChar *fileName = const_cast<CrChar *>(reinterpret_cast<const CrChar *>(finalName.c_str()));

    SDK::CrError err = SDK::GetRemoteTransferContentsDataFile(
        device_handle, slot, info.contentId, file.fileId, 0x1000000,
        saveDir, fileName);
    if (err != SDK::CrError_None) {
      dl_waiting = false;
      LOGE("GetRemoteTransferContentsDataFile failed: "
           << crsdk_err::error_to_name(err) << " (0x" << std::hex << err << std::dec << ")");
      return false;
    }

    {
      std::unique_lock<std::mutex> lk(dl_mtx);
      dl_cv.wait(lk, [&] { return !dl_waiting || g_stop.load(); });
    }

    if (g_stop.load(std::memory_order_relaxed)) return false;

    if (!last_downloaded_file.empty()) {
      local_path = last_downloaded_file;
    }
    return true;
  }

  void process_playback_button_job() {
    if (g_shutting_down.load()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (g_shutting_down.load()) return;
    if (!device_handle) return;

    auto playback_mode = fetch_property(device_handle, SDK::CrDeviceProperty_CameraOperatingMode);
    auto playback_media = fetch_property(device_handle, SDK::CrDeviceProperty_PlaybackMedia);
    SDK::CrSlotNumber slot = SDK::CrSlotNumber_Slot1;
    if (playback_media.supported &&
        playback_media.value == SDK::CrPlaybackMedia_Slot2) {
      slot = SDK::CrSlotNumber_Slot2;
    }

    CrInt32u update_code = (slot == SDK::CrSlotNumber_Slot2)
                               ? SDK::CrDeviceProperty_MediaSLOT2_ContentsInfoListUpdateTime
                               : SDK::CrDeviceProperty_MediaSLOT1_ContentsInfoListUpdateTime;

    auto &last_update = (slot == SDK::CrSlotNumber_Slot2) ? g_last_contents_update_slot2
                                                         : g_last_contents_update_slot1;
    const int kMaxAttempts = 8;
    const std::chrono::milliseconds kRetryDelay(200);
    bool have_initial = false;
    int initial_rating = 0;
    std::string local_path;

    SDK::CrContentsInfo *list = nullptr;

    auto release_list = [&](SDK::CrContentsInfo *&ptr) {
      if (ptr) {
        SDK::ReleaseRemoteTransferContentsInfoList(device_handle, ptr);
        ptr = nullptr;
      }
    };

    auto fetch_list = [&](SDK::CrContentsInfo *&out_list, CrInt32u &out_count) -> bool {
      release_list(out_list);
      SDK::CrCaptureDate dummy{};
      SDK::CrError err = SDK::GetRemoteTransferContentsInfoList(
          device_handle, slot, SDK::CrGetContentsInfoListType_All, &dummy, 0,
          &out_list, &out_count);
      if (err != SDK::CrError_None || !out_list || out_count == 0) return false;
      return true;
    };

    auto match_file_by_path = [&](const SDK::CrContentsInfo *info,
                                  const std::string &path) -> const SDK::CrContentsFile * {
      if (!info) return nullptr;
      if (path.empty()) return nullptr;
      for (CrInt32u fi = 0; fi < info->filesNum; ++fi) {
        const char *fp = reinterpret_cast<const char *>(info->files[fi].filePath);
        if (!fp) continue;
        if (path == fp) {
          return &info->files[fi];
        }
      }
      return nullptr;
    };

    bool handled = false;
    for (int attempt = 0; attempt < kMaxAttempts && !g_shutting_down.load(); ++attempt) {
      auto playback_name = fetch_property(device_handle, SDK::CrDeviceProperty_PlaybackContentsName);
      std::string playback_path = playback_name.text;

      auto update_prop = fetch_property(device_handle, update_code);
      if (!update_prop.supported || update_prop.value == 0) {
        release_list(list);
        if (attempt + 1 < kMaxAttempts) {
          std::this_thread::sleep_for(kRetryDelay);
          continue;
        }
        return;
      }

      std::uint64_t update_time = static_cast<std::uint64_t>(update_prop.value);
      if (last_update.load(std::memory_order_relaxed) == update_time) {
        release_list(list);
        if (attempt + 1 < kMaxAttempts) {
          std::this_thread::sleep_for(kRetryDelay);
          continue;
        }
        return;
      }

      CrInt32u count = 0;
      if (!fetch_list(list, count)) {
        release_list(list);
        if (attempt + 1 < kMaxAttempts) {
          std::this_thread::sleep_for(kRetryDelay);
          continue;
        }
        return;
      }

      SDK::CrCaptureDate target_stamp(update_time);
      const SDK::CrContentsInfo *update_info = nullptr;
      const SDK::CrContentsFile *update_file = nullptr;
      const SDK::CrContentsInfo *path_info = nullptr;
      const SDK::CrContentsFile *path_file = nullptr;
      const SDK::CrContentsInfo *latest_info = nullptr;

      for (CrInt32u i = 0; i < count; ++i) {
        const SDK::CrContentsInfo &info = list[i];
        if (!latest_info || capture_date_newer(info.modificationDatetimeUTC,
                                               latest_info->modificationDatetimeUTC)) {
          latest_info = &info;
        }

        const SDK::CrContentsFile *matched = match_file_by_path(&info, playback_path);
        if (matched && !path_info) {
          path_info = &info;
          path_file = matched;
        }

        if (update_time != 0 && !update_info &&
            capture_date_equal(info.modificationDatetimeUTC, target_stamp)) {
          update_info = &info;
          update_file = matched;
          if (!update_file && info.filesNum > 0) {
            update_file = &info.files[0];
          }
        }
      }

      const SDK::CrContentsInfo *target_info = nullptr;
      const SDK::CrContentsFile *target_file = nullptr;

      if (path_info) {
        target_info = path_info;
        target_file = path_file;
      }
      if (update_info) {
        target_info = update_info;
        target_file = update_file;
      }
      if (!target_info && latest_info) {
        target_info = latest_info;
        if (latest_info->filesNum > 0) {
          target_file = &latest_info->files[0];
        }
      }

      if (!target_info || !target_file) {
        if (attempt + 1 < kMaxAttempts) {
          std::this_thread::sleep_for(kRetryDelay);
          continue;
        }
        release_list(list);
        return;
      }

      std::string match_reason;
      if (target_info == path_info && path_info && target_info != update_info) {
        match_reason = "Matched by playback path.";
      } else if (target_info == update_info && update_info) {
        match_reason = "Matched by update timestamp.";
      } else if (target_info == latest_info && latest_info) {
        match_reason = "Falling back to most recently modified content.";
      }

      std::string candidate_remote_path;
      if (target_file->filePath) {
        candidate_remote_path = reinterpret_cast<const char *>(target_file->filePath);
      }

      if (!playback_path.empty() && !candidate_remote_path.empty() &&
          candidate_remote_path != playback_path) {
        release_list(list);
        if (attempt + 1 < kMaxAttempts) {
          std::this_thread::sleep_for(kRetryDelay);
          continue;
        }
        return;
      }

      int rating_value = contents_rating_to_int(target_info->rating);
      if (!have_initial) {
        initial_rating = rating_value;
        have_initial = true;
      }

      uint64_t rating_key = (static_cast<uint64_t>(slot) << 32) | target_info->contentId;
      int prev_rating = 0;
      bool prev_known = false;
      {
        std::lock_guard<std::mutex> lk(g_rating_mtx);
        auto it = g_last_known_ratings.find(rating_key);
        if (it != g_last_known_ratings.end()) {
          prev_rating = it->second;
          prev_known = true;
        }
      }

      bool rating_changed = false;
      if (prev_known && rating_value != prev_rating) {
        rating_changed = true;
      }
      if (!rating_changed && have_initial && rating_value != initial_rating) {
        rating_changed = true;
      }

      if (!rating_changed) {
        release_list(list);
        if (attempt + 1 < kMaxAttempts) {
          std::this_thread::sleep_for(kRetryDelay);
          continue;
        }
        {
          std::lock_guard<std::mutex> lk(g_rating_mtx);
          g_last_known_ratings[rating_key] = rating_value;
        }
        last_update.store(update_time, std::memory_order_relaxed);
        return;
      }

      bool have_local = download_single_content_file(slot, *target_info, *target_file,
                                                     local_path, /*skip_existing=*/true);
      if (!have_local) {
        std::error_code exists_ec;
        if (!local_path.empty() && std::filesystem::exists(local_path, exists_ec) && !exists_ec) {
          have_local = true;
        }
      }

      if (!have_local) {
        release_list(list);
        return;
      }

      {
        std::lock_guard<std::mutex> lk(g_rating_mtx);
        g_last_known_ratings[rating_key] = rating_value;
      }

      if (update_time != 0) {
        last_update.store(update_time, std::memory_order_relaxed);
      }

      int mode_value = playback_mode.supported ? static_cast<int>(playback_mode.value & 0xFFFF) : -1;
      std::string mode_text = camera_mode_to_string(mode_value);

      if (!g_post_cmd.empty() && !local_path.empty()) {
        std::vector<std::string> args{local_path, mode_text, "rating",
                                      std::to_string(rating_value), std::to_string(prev_rating)};
        run_post_cmd_args(g_post_cmd, args);
      }

      release_list(list);
      handled = true;
      break;
    }

    if (!handled) {
      release_list(list);
    }
  }

  void OnNotifyRemoteTransferContentsListChanged(CrInt32u notify, CrInt32u slotNumber, CrInt32u addSize) override {
    if (g_shutting_down.load()) return;
    if (g_stop.load(std::memory_order_relaxed)) return;
    
    if (verbose) {
      LOGI( "[CB] ContentsListChanged: notify=0x" << std::hex << notify << std::dec << " slot=" << slotNumber << " add=" << addSize );
    }
    if (notify != SDK::CrNotify_RemoteTransfer_Changed_Add) return;

    // Was this invocation triggered by a manual sync?
    bool is_sync = false;
    for (int tok = g_sync_tokens.load(std::memory_order_relaxed); tok > 0; ) {
      if (g_sync_tokens.compare_exchange_weak(tok, tok - 1, std::memory_order_relaxed)) {
	is_sync = true;
	break;
      }
    }

    bool sync_all = is_sync && g_sync_all.load(std::memory_order_relaxed);

    if (!is_sync && !g_auto_sync_enabled.load(std::memory_order_acquire)) {
      if (verbose) {
        LOGI("[CB] Auto-sync disabled; ignoring contents update (slot=" << slotNumber << ")");
      }
      return;
    }

    try {
      g_downloadThreads.emplace_back([this, slotNumber, addSize, is_sync, sync_all]() {
	if (is_sync) g_sync_active.fetch_add(1, std::memory_order_relaxed);
	auto _boot_guard = std::unique_ptr<void, void(*)(void*)>{nullptr, [](void*) {
	  if (g_sync_active.load(std::memory_order_relaxed) > 0)
	    g_sync_active.fetch_sub(1, std::memory_order_relaxed);
	}};
	SDK::CrDeviceHandle handle = this->device_handle;
	if (!handle) return;
	SDK::CrSlotNumber slot = (slotNumber == SDK::CrSlotNumber_Slot2) ? SDK::CrSlotNumber_Slot2 : SDK::CrSlotNumber_Slot1;

	auto process_list = [&](SDK::CrContentsInfo *list, CrInt32u count, CrInt32u want_hint) {
	  if (!list || count == 0) return;

	  if (is_sync && g_sync_abort.load(std::memory_order_acquire)) {
	    if (verbose) LOGI("Sync: stopped (slot " << (int)slot << ").");
	    return; // bail out before planning/logging
	  }
	  
	  if (verbose) LOGI("[SYNC] slot " << (int)slot << ": planning " << count << " item(s)"
	       << (sync_all ? " (all days)" : ""));
	  
	  // For "sync all", we want everything; otherwise keep your newest-first N logic
	  CrInt32u want = (sync_all ? count : (want_hint > 0 ? want_hint : 1));

	  std::vector<CrInt32u> idx(count);
	  for (CrInt32u i = 0; i < count; ++i) idx[i] = i;

	  if (!sync_all) {
	    std::sort(idx.begin(), idx.end(), [&](CrInt32u a, CrInt32u b) {
	      const auto &A = list[a].modificationDatetimeUTC;
	      const auto &B = list[b].modificationDatetimeUTC;
	      if (A.year != B.year) return A.year > B.year;
	      if (A.month != B.month) return A.month > B.month;
	      if (A.day != B.day) return A.day > B.day;
	      if (A.hour != B.hour) return A.hour > B.hour;
	      if (A.minute != B.minute) return A.minute > B.minute;
	      if (A.sec != B.sec) return A.sec > B.sec;
	      return A.msec > B.msec;
	    });
	    if (want > idx.size()) want = static_cast<CrInt32u>(idx.size());
	    idx.resize(want);
	  } else {
	    // "all": process in natural order
	    idx.resize(count);
	  }

	  for (CrInt32u k = 0; k < idx.size(); ++k) {

	    if (is_sync && g_sync_abort.load(std::memory_order_acquire)) {
	      dl_waiting = false;             // nothing in-flight yet for this file
	      break;
	    }
	    
	    if (g_stop.load(std::memory_order_relaxed)) break;
	    const SDK::CrContentsInfo &target = list[idx[k]];
	    if (target.contentId == 0) continue;

	    for (CrInt32u fi = 0; fi < target.filesNum; ++fi) {

	      if (is_sync && g_sync_abort.load(std::memory_order_acquire)) {
		dl_waiting = false;             // nothing in-flight yet for this file
		break;
	      }
	      
	      if (g_stop.load(std::memory_order_relaxed)) break;
      dl_waiting = true;
      CrInt32u fileId = target.files[fi].fileId;

      // determine original filename
      std::string orig = basename_from_path(target.files[fi].filePath);
	      if (orig.empty()) {
		std::ostringstream o; o << "content_" << (unsigned long long)target.contentId << "_file_" << fileId;
		orig = o.str();
	      }

	      // derive relative directory from remote file path (e.g. "PRIVATE/M4ROOT/CLIP")
	      std::string relDir = dirname_from_path(target.files[fi].filePath);

	      // compute full local directory and ensure it exists
	      std::string destDir = g_download_dir;
	      if (!relDir.empty()) destDir = join_path(destDir, relDir);
	      std::error_code ec;
	      std::filesystem::create_directories(destDir, ec);

	      // choose final filename (sync/boot: keep name & skip existing; else: uniquify)
	      std::string finalName;
	      std::string candidatePath = join_path(destDir, orig);
	      if (is_sync) {
		if (std::filesystem::exists(candidatePath)) {
		  if (verbose) LOGI("[SKIP] already present: " << join_path(relDir, orig));
		  dl_waiting = false;
		  continue;
		}
		finalName = orig;
	      } else {
		finalName = unique_name(destDir, orig);
	      }

	      CrChar *saveDir = destDir.empty() ? nullptr
		: const_cast<CrChar *>(reinterpret_cast<const CrChar *>(destDir.c_str()));
	      if (g_stop.load(std::memory_order_relaxed)) { dl_waiting = false; break; }
	      
	      // progress label before SDK gives us the final path
	      dl_current_label = join_path(relDir, finalName);
      dl_last_log_per = 101; // reset throttling so we log 0% immediately
      dl_last_log_tp = std::chrono::steady_clock::now();
      dl_start_tp = dl_last_log_tp;
      dl_any_progress = false;
      dl_current_operation = is_sync ? "sync" : "new";
      dl_current_mode = capture_mode_string(device_handle, target, target.files[fi]);

      // kick off download
      SDK::CrError dres = SDK::GetRemoteTransferContentsDataFile(
								 handle, slot, target.contentId, fileId, 0x1000000,
								 saveDir, const_cast<CrChar *>(finalName.c_str()));

	      {
		std::unique_lock<std::mutex> lk(dl_mtx);
		dl_cv.wait(lk, [&] { return !dl_waiting || g_stop.load(); });

	if (is_sync && g_sync_abort.load(std::memory_order_acquire)) {
	  // We just finished a file; exit early.
	  break;
	}
	      }

	      if (g_stop.load(std::memory_order_relaxed)) break;
	    }
	  }
	};

	// -------- fetch & process --------
	SDK::CrError resList = SDK::CrError_None;

	if (sync_all) {
	  // Walk ALL dates
	  SDK::CrCaptureDate *dateList = nullptr; CrInt32u dateNums = 0;
	  resList = SDK::GetRemoteTransferCapturedDateList(handle, slot, &dateList, &dateNums);
	  if (resList != SDK::CrError_None || !dateList || dateNums == 0) {
	    if (dateList) SDK::ReleaseRemoteTransferCapturedDateList(handle, dateList);
	    if (verbose) LOGI("[INFO] No contents found (slot=" << (int)slot << ")");
	    return;
	  }

	  // Process each date (newest-first is fine but not required when syncing *all*)
	  auto newer = [](const SDK::CrCaptureDate& A, const SDK::CrCaptureDate& B){
	    if (A.year != B.year) return A.year > B.year;
	    if (A.month != B.month) return A.month > B.month;
	    return A.day > B.day;
	  };
	  std::vector<SDK::CrCaptureDate> days(dateList, dateList + dateNums);
	  std::sort(days.begin(), days.end(), newer);

	  if (is_sync && g_sync_abort.load(std::memory_order_acquire)) {
	    LOGI("Sync: stopped (slot " << (int)slot << ").");
	    SDK::ReleaseRemoteTransferCapturedDateList(handle, dateList);
	    return;
	  }
	  
	  for (auto const& day : days) {

	    if (is_sync && g_sync_abort.load(std::memory_order_acquire)) {
	      LOGI("Sync: stopped (slot " << (int)slot << ").");
	      break;
	    }
	    
	    if (g_stop.load(std::memory_order_relaxed)) break;
	    SDK::CrContentsInfo *list = nullptr; CrInt32u count = 0;
	    resList = SDK::GetRemoteTransferContentsInfoList(handle, slot,
							     SDK::CrGetContentsInfoListType_Range_Day, const_cast<SDK::CrCaptureDate*>(&day),
							     0, &list, &count);
	    if (resList == SDK::CrError_None && list && count > 0) {
	      process_list(list, count, /*want_hint*/0);
	    }
	    if (list) SDK::ReleaseRemoteTransferContentsInfoList(handle, list);
	  }
	  SDK::ReleaseRemoteTransferCapturedDateList(handle, dateList);
	} else {
	  // EXISTING behavior: newest day & newest N items
	  SDK::CrCaptureDate *dateList = nullptr; CrInt32u dateNums = 0;
	  SDK::CrError derr = SDK::GetRemoteTransferCapturedDateList(handle, slot, &dateList, &dateNums);
	  if (derr == SDK::CrError_None && dateList && dateNums > 0) {
	    // pick latest by Y/M/D
	    SDK::CrCaptureDate latest = dateList[0];
	    for (CrInt32u i = 1; i < dateNums; ++i) {
	      const auto &D = dateList[i];
	      if ((D.year > latest.year) ||
		  (D.year == latest.year && D.month > latest.month) ||
		  (D.year == latest.year && D.month == latest.month && D.day > latest.day)) {
		latest = D;
	      }
	    }
	    SDK::CrContentsInfo *list = nullptr; CrInt32u count = 0;
	    resList = SDK::GetRemoteTransferContentsInfoList(handle, slot,
							     SDK::CrGetContentsInfoListType_Range_Day, &latest, 0, &list, &count);
	    SDK::ReleaseRemoteTransferCapturedDateList(handle, dateList);
	    if (resList != SDK::CrError_None || !list || count == 0) {
	      if (list) { SDK::ReleaseRemoteTransferContentsInfoList(handle, list); }
	      if (verbose) LOGI("[INFO] No contents found for latest day (slot=" << (int)slot << ")");
	      return;
	    }

	    process_list(list, count, /*want_hint*/(addSize > 0 ? addSize : 1));
	    SDK::ReleaseRemoteTransferContentsInfoList(handle, list);
	  } else {
	    if (dateList) SDK::ReleaseRemoteTransferCapturedDateList(handle, dateList);
	    if (verbose) LOGI("[INFO] No contents found (slot=" << (int)slot << ")");
	    return;
	  }
	}
      });
    } catch (...) {
      LOGE("[ERROR] Failed to create download thread");
    }

  }

  void OnNotifyContentsTransfer(CrInt32u, SDK::CrContentHandle, CrChar *) override {}

  void OnNotifyRemoteTransferResult(CrInt32u notify, CrInt32u per, CrChar *filename) override
  {
    std::unique_lock<std::mutex> lk(dl_mtx);

    if (filename) last_downloaded_file = filename; else last_downloaded_file.clear();
    dl_notify_code = notify; dl_progress = per;

    bool sync_aborted = g_sync_abort.load(std::memory_order_acquire);

    // Choose a human label: prefer filename from SDK, else the precomputed label.
    std::string label = last_downloaded_file.empty() ? dl_current_label : last_downloaded_file;
    // For nicer logs, strip to a relative path if possible (optional).
    if (!label.empty()) {
      // leave label as-is; it already is relative (relDir/finalName) in our pre-label
    }

    if (notify == SDK::CrNotify_RemoteTransfer_InProgress) {
      if (sync_aborted) {
        // Suppress noise while waiting for the device to acknowledge cancellation.
        return;
      }
      // Throttle: log when +5% or +1s since last log (and always at 0%)
      auto now = std::chrono::steady_clock::now();
      bool time_ok = (dl_last_log_tp.time_since_epoch().count() == 0) ||
	(now - dl_last_log_tp) >= std::chrono::seconds(1);
      bool perc_ok = (dl_last_log_per == 101) || (per >= dl_last_log_per + 5);

      if (time_ok || perc_ok) {
	  if (verbose) LOGI("[DL] " << (label.empty() ? "(unknown file)" : label) << " — " << per << "%");
	dl_last_log_per = per;
	dl_last_log_tp = now;
	dl_any_progress = true;
      }
      // stay waiting; do NOT signal cv yet
      return;
    }

    // Non-progress notifications: finish/abort/etc.
    dl_waiting = false;
    dl_cv.notify_all();

    if (sync_aborted) {
      if (notify == SDK::CrNotify_RemoteTransfer_Result_OK) {
        LOGI("[DL] Completed before cancel request took effect: "
             << (label.empty() ? "(unknown file)" : label));
      } else {
        LOGI("[DL] Canceled: " << (label.empty() ? "(unknown file)" : label)
             << " (notify=0x" << std::hex << notify << std::dec << ")");
      }
      return;
    }

    if (notify == SDK::CrNotify_RemoteTransfer_Result_OK) {
      // No extra "100%" line; keep output compact.
      std::string saved = last_downloaded_file;
      std::string base = saved;
      size_t pos = base.find_last_of("/\\"); if (pos != std::string::npos) base = base.substr(pos + 1);
      long long sizeB = 0; struct stat st{}; if (::stat(saved.c_str(), &st) == 0) sizeB = (long long)st.st_size;

      // Optional: include elapsed time (kept simple; remove if you prefer)
      auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
									      std::chrono::steady_clock::now() - dl_start_tp).count();

      // For small files (no progress logs), this is the ONLY line.
      LOGI("[FILE] " << base << " (" << sizeB << " bytes"
           << ", " << elapsed_ms << " ms)");

      if (!g_post_cmd.empty() && !saved.empty()) {
        std::string mode_text = dl_current_mode.empty()
                                  ? current_mode_string(device_handle)
                                  : dl_current_mode;
        std::string operation = dl_current_operation.empty() ? "new" : dl_current_operation;
        std::string new_value = dl_current_label.empty() ? base : dl_current_label;
        run_post_cmd(g_post_cmd, saved, mode_text, operation, "", new_value);
      }

      dl_current_mode.clear();
      dl_current_operation.clear();

    } else {
      LOGE("[DL] Failed: " << (label.empty() ? "(unknown file)" : label)
	   << " (notify=0x" << std::hex << notify << std::dec << ")");
      dl_current_mode.clear();
      dl_current_operation.clear();
    }
  }


  void OnNotifyRemoteTransferResult(CrInt32u, CrInt32u, CrInt8u *, CrInt64u) override {}
  void OnNotifyFTPTransferResult(CrInt32u, CrInt32u, CrInt32u) override {}
  void OnNotifyRemoteFirmwareUpdateResult(CrInt32u, const void *) override {}
  void OnReceivePlaybackTimeCode(CrInt32u) override {}
  void OnNotifyMonitorUpdated(CrInt32u, CrInt32u) override {}

private:
  std::unordered_map<CrInt32u, CrInt64u> last_prop_vals;
};

// Attempt a single connect (by direct IP or enumeration). Returns true on success.
static bool try_connect_once(const std::string &explicit_host,
                             const std::string &explicit_mac,
                             const std::string &explicit_model,
                             const std::string &download_dir,
                             bool verbose,
                             const std::string &auth_user,
                             const std::string &auth_pass,
                             QuietCallback &cb,
                             SDK::CrDeviceHandle &handle,
                             const SDK::ICrCameraObjectInfo *&selected,
                             SDK::ICrEnumCameraObjectInfo *&enum_list,
                             SDK::ICrCameraObjectInfo *&created)
{
  cb.verbose = verbose;
  SDK::CrError err = SDK::CrError_None;
  selected = nullptr; enum_list = nullptr; created = nullptr; handle = 0;
  auto model_choice = resolve_camera_model_choice(explicit_model);
  bool have_canonical = !model_choice.canonical.empty();

  SDK::CrCameraDeviceModelList direct_ip_model = model_choice.direct_value.value_or(SDK::CrCameraDeviceModel_ILCE_6700);
  bool direct_model_is_guess = have_canonical && !model_choice.direct_value.has_value();

  // Used by the direct-IP path and (maybe) a retry:
  unsigned char direct_ip_mac[6] = {0,0,0,0,0,0};
  
  // Parse explicit_host ("x.y.z.w") into CrInt32u as required by the SDK
  CrInt32u direct_ip_addr_num = 0;
  if (!explicit_host.empty()) {
    struct in_addr ina{};
    if (inet_pton(AF_INET, explicit_host.c_str(), &ina) == 1) {
      // Keep network byte order (big-endian) as CrInt32u
      direct_ip_addr_num = static_cast<CrInt32u>(ina.s_addr);
    } else {
      LOGE("Invalid IPv4 address: " << explicit_host);
      return false;
    }
  }
  
  // -------- Discover or create camera object --------
  const bool using_direct_ip = !explicit_host.empty();
  if (!using_direct_ip) {
    if (verbose) LOGI("Searching for cameras...");
    err = SDK::EnumCameraObjects(&enum_list, 1);
    if (err != SDK::CrError_None || !enum_list || enum_list->GetCount() == 0) {
      LOGE("No cameras found (EnumCameraObjects)");
      return false;
    }
    selected = enum_list->GetCameraObjectInfo(0);
    if (!selected) {
      LOGE("Enumeration returned an empty camera list");
      return false;
    }
    if (have_canonical && !model_name_matches(selected, model_choice.canonical)) {
      const char *model_ptr = reinterpret_cast<const char*>(selected->GetModel());
      LOGW("--model='" << model_choice.canonical
           << "' ignored; proceeding with first enumerated camera"
           << (model_ptr ? std::string(" ('") + model_ptr + "')" : std::string("")));
    }
  } else {
    // --- Direct IP path ---
    bool mac_filter_set = false;
    if (!explicit_mac.empty()) {
      if (!parse_mac(explicit_mac, direct_ip_mac)) {
        LOGE("WARN bad MAC, ignoring: " << explicit_mac);
      } else {
        mac_filter_set = true;
      }
    }

    // Attempt to reuse an enumerated object so we avoid guessing the model enum.
    SDK::ICrEnumCameraObjectInfo *direct_enum = nullptr;
    err = SDK::EnumCameraObjects(&direct_enum, 1);
    if (err == SDK::CrError_None && direct_enum && direct_enum->GetCount() > 0) {
      for (CrInt32u i = 0; i < direct_enum->GetCount(); ++i) {
        auto info = direct_enum->GetCameraObjectInfo(i);
        if (!info) continue;

        bool ip_match = false;
        const char *ip_char = reinterpret_cast<const char*>(info->GetIPAddressChar());
        if (ip_char && *ip_char) {
          ip_match = (explicit_host == std::string(ip_char));
        }
        if (!ip_match && direct_ip_addr_num != 0) {
          ip_match = (info->GetIPAddress() == direct_ip_addr_num);
        }
        if (!ip_match) continue;

        bool mac_match = true;
        if (mac_filter_set) {
          auto cam_mac = info->GetMACAddress();
          auto cam_mac_len = info->GetMACAddressSize();
          mac_match = (cam_mac && cam_mac_len >= 6 && std::memcmp(cam_mac, direct_ip_mac, 6) == 0);
        }
        if (!mac_match) continue;

        selected = info;
        enum_list = direct_enum;
        direct_enum = nullptr;
        if (verbose) {
          LOGI("Using enumerated camera object for IP " << explicit_host);
        }
        break;
      }
    }
    if (direct_enum) {
      direct_enum->Release();
      direct_enum = nullptr;
    }

    if (!selected) {
      if (direct_model_is_guess) {
        LOGI("Model enum for '" << model_choice.canonical << "' unavailable; attempting direct connect with default enum.");
      }

      // If the user provided credentials, default to SSH ON for first attempt.
      CrInt32u sshSupportFlag = (!auth_user.empty() || !auth_pass.empty())
        ? SDK::CrSSHsupport_ON
        : SDK::CrSSHsupport_OFF;

      // Initial direct-IP object
      err = SDK::CreateCameraObjectInfoEthernetConnection(&created, direct_ip_model, direct_ip_addr_num, direct_ip_mac, sshSupportFlag);
      if (err != SDK::CrError_None || !created) {
        LOGE("CreateCameraObjectInfoEthernetConnection failed");
        return false;
      }
      selected = created;
    }
  }

  // -------- Credentials --------
  const CrChar* user_ptr = auth_user.empty() ? nullptr : (const CrChar*)auth_user.c_str();
  const CrChar* pass_ptr = auth_pass.empty() ? nullptr : (const CrChar*)auth_pass.c_str();

  // -------- Fingerprint (cache) --------
  std::string fp_path = get_cache_dir() + "/fp_enumerated.bin";
  {
    std::error_code ec;
    std::filesystem::create_directories(get_cache_dir(), ec);
    if (ec) {
      LOGE("[FP] failed to create cache dir: " << get_cache_dir() << " (" << ec.message() << ")");
    }
  }
  std::vector<char> fp_cache;                // cached fp buffer
  if (load_fingerprint(fp_path, fp_cache)) {
    // ok if empty; we’ll decide below
  }

  // -------- Fingerprint (from discovered object) --------
  // Per SDK sample, obtain a fingerprint from the *discovered* object and prefer it.
  char fp_from_obj[512] = {0};
  CrInt32u fp_from_obj_len = 0;
  SDK::GetFingerprint(const_cast<SDK::ICrCameraObjectInfo *>(selected), fp_from_obj, &fp_from_obj_len);

  const CrChar* fp_ptr = nullptr;
  CrInt32u      fp_len = 0;
  if (fp_from_obj_len > 0 && fp_from_obj_len <= sizeof(fp_from_obj)) {
    fp_ptr = fp_from_obj;
    fp_len = fp_from_obj_len;
    if (verbose) LOGI("[FP] using fingerprint from discovered camera (" << fp_len << " bytes)");
  } else if (!fp_cache.empty()) {
    fp_ptr = (CrChar*)fp_cache.data();
    fp_len = (CrInt32u)fp_cache.size();
    if (verbose) LOGI("[FP] using cached fingerprint (" << fp_len << " bytes)");
  } else {
    if (verbose) LOGI("[FP] no fingerprint available for initial connect");
  }

  // -------- SSH/Access-Auth support & reconnecting flag --------
  bool ssh_on = (selected && selected->GetSSHsupport() == SDK::CrSSHsupport_ON);
  if (verbose) LOGI(std::string("[AUTH] Authenticating (ssh support is ") + (ssh_on ? "on" : "off") + ")");
  if (user_ptr && verbose) LOGI("[AUTH] Using username to connect");

  bool is_direct_ip = using_direct_ip;
  
  auto reconnecting = SDK::CrReconnecting_ON;
  // First contact over direct IP should not be "reconnecting".
  if (is_direct_ip) {
    reconnecting = SDK::CrReconnecting_OFF;
  }
  // Also, if SSH is ON and there's no fingerprint, force a full handshake.
  if (ssh_on && fp_len == 0) {
    reconnecting = SDK::CrReconnecting_OFF;
  }
  
 // -------- Connect --------
  std::string target_desc = explicit_host.empty() ? "camera" : ("camera at " + explicit_host);
  LOGI("Connecting to " << target_desc << "...");
  err = SDK::Connect(const_cast<SDK::ICrCameraObjectInfo *>(selected), &cb, &handle,
                     SDK::CrSdkControlMode_RemoteTransfer, reconnecting,
                     const_cast<CrChar*>(user_ptr),
                     const_cast<CrChar*>(pass_ptr),
                     const_cast<CrChar*>(fp_ptr), fp_len);
  
  if (err != SDK::CrError_None && is_direct_ip && (unsigned)err == 0x8202) {
    // Transport refused the path we chose. Retry once with the opposite SSH flag.
    if (verbose) LOGE("Connect failed 0x8202; retrying once with opposite SSH support flag...");
    
    // Recreate the IP object with flipped SSH support.
    CrInt32u flipped = (selected->GetSSHsupport() == SDK::CrSSHsupport_ON)
      ? SDK::CrSSHsupport_OFF : SDK::CrSSHsupport_ON;

    SDK::ICrCameraObjectInfo* retry_obj = nullptr;
    SDK::CreateCameraObjectInfoEthernetConnection(&retry_obj, direct_ip_model, direct_ip_addr_num, direct_ip_mac, flipped);
    if (retry_obj) {
      // Keep first-contact semantics: not reconnecting.
      auto retry_reconnecting = SDK::CrReconnecting_OFF;
      
      err = SDK::Connect(retry_obj, &cb, &handle,
			 SDK::CrSdkControlMode_RemoteTransfer, retry_reconnecting,
			 const_cast<CrChar*>(user_ptr),
			 const_cast<CrChar*>(pass_ptr),
			 const_cast<CrChar*>(fp_ptr), fp_len);
      
      if (err == SDK::CrError_None) {
	// Use the successful object as the selected one going forward
	if (created) { created->Release(); created = nullptr; }
	selected = retry_obj;
      } else {
	retry_obj->Release();
      }
    }
  }

  if (err != SDK::CrError_None) {
    unsigned u = (unsigned)err;
    const char* name = crsdk_err::error_to_name(err);
    
    // Only log if we didn’t already explain above
    if (u != 0x8213) {
      LOGE("Connect failed: 0x" << std::hex << u << std::dec << " (" << name << ")");
    }
    return false;
  }

  // -------- Wait for handshake completion --------
  {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    std::unique_lock<std::mutex> lk(cb.mtx);
    while (!cb.connected && !cb.conn_finished && !g_stop.load(std::memory_order_relaxed)) {
      if (cb.conn_cv.wait_for(lk, std::chrono::milliseconds(100)) == std::cv_status::timeout) {
        if (std::chrono::steady_clock::now() >= deadline) break;
      }
    }
  }
  if (!cb.connected) {
    std::ostringstream _m;
    _m << "Camera not available";
    if (cb.last_error_code) _m << " error=0x" << std::hex << cb.last_error_code << std::dec;
    LOGE(_m.str());
    return false;
  }

  cb.device_handle = handle;
    if (verbose) {
      LOGI("Connected. Ctrl+D to stop.");
    } else {
      LOGI("Connected. Ctrl+D to stop.");
    }

  // -------- Persist (updated) fingerprint for next run --------
  {
    char newfp[512] = {0}; CrInt32u nlen = 0;
    SDK::GetFingerprint(const_cast<SDK::ICrCameraObjectInfo *>(selected), newfp, &nlen);
    if (verbose) LOGI("[FP] GetFingerprint(ICrCameraObjectInfo*): " << nlen << " bytes");

    if (nlen > 0 && nlen <= sizeof(newfp)) {
      if (save_fingerprint(fp_path, newfp, nlen)) {
        if (verbose) LOGI("[FP] saved " << nlen << " bytes to " << fp_path);
      } else {
        LOGE("[FP] failed to save fingerprint to " << fp_path);
      }
    } else {
      if (verbose) LOGI("[FP] no fingerprint to save (nlen=0)");
    }
  }

  (void)download_dir; // already assigned globally elsewhere
  return true;
}

static void disconnect_and_release(SDK::CrDeviceHandle &handle,
				   SDK::ICrCameraObjectInfo *&created,
				   SDK::ICrEnumCameraObjectInfo *&enum_list) {
  if (handle) { SDK::Disconnect(handle); SDK::ReleaseDevice(handle); handle = 0; }
  if (created) { created->Release(); created = nullptr; }
  if (enum_list) { enum_list->Release(); enum_list = nullptr; }
}

// simple word list
static const std::vector<std::string> commands = {
  "shoot", "trigger", "focus", "sync", "monitor", "record", "button", "status", "exposure", "power", "quit", "exit"
};

char* prompt(EditLine*) {
  return const_cast<char*>("sonshell> ");
}

// completion callback
unsigned char complete(EditLine* el, int ch) {
  const LineInfo* li = el_line(el);
  std::string buf(li->buffer, li->lastchar - li->buffer);

  // very simple: complete whole line against commands
  for (auto& cmd : commands) {
    if (cmd.rfind(buf, 0) == 0) {
      el_insertstr(el, cmd.c_str() + buf.size());
      return CC_REFRESH;
    }
  }
  return CC_REFRESH;
}

// tiny tokenizer: splits on spaces, respects "quotes"
static std::vector<std::string> tokenize(std::string const& s) {
  std::vector<std::string> out; std::string cur; bool q=false;
  for (size_t i=0;i<s.size();++i) {
    char c=s[i];
    if (c=='"') { q=!q; continue; }
    if (!q && std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

static std::string join_tokens(const std::vector<std::string>& tokens) {
  if (tokens.empty()) return {};
  return join_args(tokens, 0);
}

static bool split_key_value_line(const std::string& line,
                                 std::string& key,
                                 std::string& value) {
  auto pos = line.find(':');
  if (pos == std::string::npos) return false;
  key = trim_copy(line.substr(0, pos));
  value = trim_copy(line.substr(pos + 1));
  if (!value.empty() && value.front() == '"' && value.back() == '"' && value.size() >= 2) {
    value = value.substr(1, value.size() - 2);
  }
  return !key.empty();
}

static bool parse_key_code_token(const std::string& raw, int& code_out) {
  if (raw.empty()) return false;
  bool numeric = std::isdigit(static_cast<unsigned char>(raw.front()));
  if (!numeric && raw.size() > 1 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X')) {
    numeric = true;
  }
  if (numeric) {
    try {
      code_out = std::stoi(raw, nullptr, 0);
      return true;
    } catch (...) {
      return false;
    }
  }

  std::string key = raw;
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });

#define KEY_ENTRY(name) {#name, name}
  static const std::unordered_map<std::string, int> kKeyCodeMap = {
    KEY_ENTRY(KEY_LEFT), KEY_ENTRY(KEY_RIGHT), KEY_ENTRY(KEY_UP), KEY_ENTRY(KEY_DOWN),
    KEY_ENTRY(KEY_ENTER), KEY_ENTRY(KEY_OK), KEY_ENTRY(KEY_ESC), KEY_ENTRY(KEY_BACKSPACE),
    KEY_ENTRY(KEY_SPACE), KEY_ENTRY(KEY_TAB), KEY_ENTRY(KEY_HOME), KEY_ENTRY(KEY_END),
    KEY_ENTRY(KEY_PAGEUP), KEY_ENTRY(KEY_PAGEDOWN), KEY_ENTRY(KEY_MENU), KEY_ENTRY(KEY_INFO),
    KEY_ENTRY(KEY_CAMERA), KEY_ENTRY(KEY_RECORD), KEY_ENTRY(KEY_PLAYPAUSE),
    KEY_ENTRY(KEY_STOP), KEY_ENTRY(KEY_VOLUMEUP), KEY_ENTRY(KEY_VOLUMEDOWN),
    KEY_ENTRY(KEY_MUTE), KEY_ENTRY(KEY_MEDIA), KEY_ENTRY(KEY_F1), KEY_ENTRY(KEY_F2),
    KEY_ENTRY(KEY_F3), KEY_ENTRY(KEY_F4), KEY_ENTRY(KEY_F5), KEY_ENTRY(KEY_F6),
    KEY_ENTRY(KEY_F7), KEY_ENTRY(KEY_F8), KEY_ENTRY(KEY_F9), KEY_ENTRY(KEY_F10),
    KEY_ENTRY(KEY_F11), KEY_ENTRY(KEY_F12), KEY_ENTRY(KEY_1), KEY_ENTRY(KEY_2),
    KEY_ENTRY(KEY_3), KEY_ENTRY(KEY_4), KEY_ENTRY(KEY_5), KEY_ENTRY(KEY_6),
    KEY_ENTRY(KEY_7), KEY_ENTRY(KEY_8), KEY_ENTRY(KEY_9), KEY_ENTRY(KEY_0),
    KEY_ENTRY(KEY_A), KEY_ENTRY(KEY_B), KEY_ENTRY(KEY_C), KEY_ENTRY(KEY_D),
    KEY_ENTRY(KEY_E), KEY_ENTRY(KEY_F), KEY_ENTRY(KEY_G), KEY_ENTRY(KEY_H),
    KEY_ENTRY(KEY_I), KEY_ENTRY(KEY_J), KEY_ENTRY(KEY_K), KEY_ENTRY(KEY_L),
    KEY_ENTRY(KEY_M), KEY_ENTRY(KEY_N), KEY_ENTRY(KEY_O), KEY_ENTRY(KEY_P),
    KEY_ENTRY(KEY_Q), KEY_ENTRY(KEY_R), KEY_ENTRY(KEY_S), KEY_ENTRY(KEY_T),
    KEY_ENTRY(KEY_U), KEY_ENTRY(KEY_V), KEY_ENTRY(KEY_W), KEY_ENTRY(KEY_X),
    KEY_ENTRY(KEY_Y), KEY_ENTRY(KEY_Z),
    KEY_ENTRY(BTN_LEFT), KEY_ENTRY(BTN_RIGHT), KEY_ENTRY(BTN_MIDDLE),
    KEY_ENTRY(BTN_SOUTH), KEY_ENTRY(BTN_EAST), KEY_ENTRY(BTN_NORTH),
    KEY_ENTRY(BTN_WEST)
  };
#undef KEY_ENTRY

  auto it = kKeyCodeMap.find(key);
  if (it == kKeyCodeMap.end()) return false;
  code_out = it->second;
  return true;
}

static bool parse_input_map_config(const std::string& path,
                                   std::vector<InputMapDevice>& out,
                                   std::string& error) {
  std::ifstream in(path);
  if (!in) {
    error = "input-map: unable to open " + path;
    return false;
  }

  InputMapDevice current;
  bool device_active = false;
  bool in_bindings = false;
  int line_no = 0;

  auto finalize_device = [&]() -> bool {
    if (!device_active) return true;
    if (current.path.empty()) {
      error = "input-map: device missing 'path:' entry";
      return false;
    }
    out.push_back(current);
    current = InputMapDevice{};
    device_active = false;
    in_bindings = false;
    return true;
  };

  while (in.good()) {
    std::string raw_line;
    std::getline(in, raw_line);
    if (!in && raw_line.empty()) break;
    ++line_no;
    std::string line = raw_line;
    auto comment = line.find('#');
    if (comment != std::string::npos) line.erase(comment);

    int indent = 0;
    while (indent < static_cast<int>(line.size()) && line[indent] == ' ') ++indent;

    bool list_item = false;
    int content_start = indent;
    if (content_start < static_cast<int>(line.size()) && line[content_start] == '-') {
      list_item = true;
      ++content_start;
      if (content_start < static_cast<int>(line.size()) && line[content_start] == ' ') ++content_start;
    }

    std::string content = trim_copy(line.substr(content_start));
    if (content.empty()) continue;

    if (indent == 0 && content == "input:") {
      continue;
    }
    if (indent == 2 && content == "devices:") {
      continue;
    }
    if (indent == 4 && list_item) {
      if (!finalize_device()) return false;
      device_active = true;
      in_bindings = false;
      if (!content.empty()) {
        std::string key, value;
        if (!split_key_value_line(content, key, value)) {
          error = "input-map: expected 'path: ...' after '-' on line " + std::to_string(line_no);
          return false;
        }
        if (key != "path") {
          error = "input-map: only 'path' may follow '-' directly (line " + std::to_string(line_no) + ")";
          return false;
        }
        current.path = expand_user_path(value);
      }
      continue;
    }
    if (!device_active) {
      error = "input-map: device attributes appeared before '- path:' (line " + std::to_string(line_no) + ")";
      return false;
    }

    if (indent >= 6 && indent < 8) {
      std::string key, value;
      if (!split_key_value_line(content, key, value)) {
        error = "input-map: expected 'key: value' on line " + std::to_string(line_no);
        return false;
      }
      if (key == "path") {
        current.path = expand_user_path(value);
      } else if (key == "bindings") {
        if (!value.empty()) {
          error = "input-map: 'bindings' must not have an inline value (line " + std::to_string(line_no) + ")";
          return false;
        }
        in_bindings = true;
      } else {
        error = "input-map: unknown device key '" + key + "' (line " + std::to_string(line_no) + ")";
        return false;
      }
      continue;
    }

    if (indent >= 8 && in_bindings) {
      std::string key, value;
      if (!split_key_value_line(content, key, value)) {
        error = "input-map: expected 'KEY_NAME: command' (line " + std::to_string(line_no) + ")";
        return false;
      }
      if (value.empty()) {
        error = "input-map: command for '" + key + "' is empty (line " + std::to_string(line_no) + ")";
        return false;
      }
      int key_code = 0;
      if (!parse_key_code_token(key, key_code)) {
        error = "input-map: unknown key '" + key + "' (line " + std::to_string(line_no) + ")";
        return false;
      }
      auto tokens = tokenize(value);
      if (tokens.empty()) {
        error = "input-map: command for '" + key + "' parses to zero tokens (line " + std::to_string(line_no) + ")";
        return false;
      }
      current.key_to_command[key_code] = tokens;
      continue;
    }

    error = "input-map: unsupported indentation on line " + std::to_string(line_no);
    return false;
  }

  if (!finalize_device()) return false;
  if (out.empty()) {
    error = "input-map: no devices defined in " + path;
    return false;
  }
  return true;
}

static std::function<int(const std::vector<std::string>&)> wait_for_command_runner() {
  std::unique_lock<std::mutex> lk(g_command_runner_mutex);
  g_command_runner_cv.wait(lk, [] {
    return g_command_runner || g_stop.load(std::memory_order_relaxed);
  });
  if (!g_command_runner) return {};
  return g_command_runner;
}

static int run_input_mapped_command(const std::vector<std::string>& tokens) {
  if (tokens.empty()) return 0;
  auto runner = wait_for_command_runner();
  if (!runner) return 1;
  return runner(tokens);
}

static void input_device_thread_main(InputMapDevice device) {
  const std::string label = device.path.empty() ? "<unknown>" : device.path;
  constexpr std::chrono::milliseconds kRetryDelay{1000};

  while (!g_stop.load(std::memory_order_relaxed)) {
    int fd = open(device.path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      int err = errno;
      LOGE("input-map: failed to open " << label << ": " << std::strerror(err));
      std::this_thread::sleep_for(kRetryDelay);
      continue;
    }
    LOGI("input-map: listening on " << label << " (" << device.key_to_command.size() << " bindings)");

    while (!g_stop.load(std::memory_order_relaxed)) {
      struct pollfd pfd { fd, POLLIN, 0 };
      int pr = poll(&pfd, 1, 250);
      if (pr < 0) {
        if (errno == EINTR) continue;
        LOGE("input-map: poll failed on " << label << ": " << std::strerror(errno));
        break;
      }
      if (pr == 0) continue;
      if (!(pfd.revents & POLLIN)) continue;

      struct input_event ev {};
      ssize_t n = read(fd, &ev, sizeof(ev));
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        LOGE("input-map: read failed on " << label << ": " << std::strerror(errno));
        break;
      }
      if (n != sizeof(ev)) continue;
      if (ev.type != EV_KEY) continue;
      if (ev.value != 1) continue;

      auto it = device.key_to_command.find(ev.code);
      if (it == device.key_to_command.end()) continue;

      int rc = run_input_mapped_command(it->second);
      if (rc != 0) {
        LOGW("input-map: command '" << join_tokens(it->second) << "' returned " << rc);
      }
    }

    close(fd);
    if (g_stop.load(std::memory_order_relaxed)) break;
    LOGW("input-map: device " << label << " disconnected; retrying in " << kRetryDelay.count() << " ms");
    std::this_thread::sleep_for(kRetryDelay);
  }
}

static void start_input_map_threads(const std::vector<InputMapDevice>& devices,
                                    const std::string& source_path) {
  if (devices.empty()) return;
  if (!source_path.empty()) {
    LOGI("input-map: loaded " << devices.size() << " device(s) from " << source_path);
  }
  for (const auto& dev : devices) {
    if (dev.path.empty() || dev.key_to_command.empty()) {
      LOGW("input-map: skipped device with no path or bindings");
      continue;
    }
    g_input_device_threads.emplace_back(input_device_thread_main, dev);
  }
}

static void join_input_map_threads() {
  for (auto& t : g_input_device_threads) {
    if (!t.joinable()) continue;
    if (g_force_close_requested.load(std::memory_order_relaxed)) {
      t.detach();
    } else {
      t.join();
    }
  }
  g_input_device_threads.clear();
}

struct Context {
  SDK::CrDeviceHandle handle = 0;
  // add whatever shared state you need
};

int main(int argc, char **argv) {
  std::setlocale(LC_CTYPE, "");

  install_signal_handlers();
  block_sigint_in_this_thread();

  std::string explicit_host, explicit_mac, explicit_model, download_dir;
  bool verbose = false;
  std::string auth_user, auth_pass;
  std::string input_map_path;
  bool input_map_explicit = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--host" && i + 1 < argc) explicit_host = argv[++i];
    else if (a == "--mac" && i + 1 < argc) explicit_mac = argv[++i];
    else if (a == "--sync-dir" && i + 1 < argc) download_dir = argv[++i];
    else if (a == "--verbose" || a == "-v") verbose = true;
    else if (a == "--cmd" && i + 1 < argc) g_post_cmd = argv[++i];
    else if (a == "--model" && i + 1 < argc) explicit_model = argv[++i];
    else if (a == "--keepalive" && i + 1 < argc) {
      long long ms = std::atoll(argv[++i]);
      if (ms < 0) ms = 0;
      g_keepalive = std::chrono::milliseconds(ms);
    }
    else if (a == "--user" && i + 1 < argc) auth_user = argv[++i];
    else if (a == "--pass" && i + 1 < argc) auth_pass = argv[++i];
    else if (a == "--input-map" && i + 1 < argc) {
      input_map_path = argv[++i];
      input_map_explicit = true;
    }
  }

  const bool sync_dir_configured = !download_dir.empty();

#ifdef SONSHELL_HEADLESS
  LOGW("Live view disabled: SonShell built with -DSONSHELL_HEADLESS=ON (OpenCV omitted)");
#endif

  std::string resolved_input_map;
  if (!input_map_path.empty()) {
    resolved_input_map = expand_user_path(input_map_path);
  } else {
    std::string fallback = default_input_map_path();
    if (!fallback.empty() && std::filesystem::exists(fallback)) {
      resolved_input_map = fallback;
    }
  }

  if (!resolved_input_map.empty()) {
    if (!std::filesystem::exists(resolved_input_map)) {
      if (input_map_explicit) {
        LOGW("input-map: file not found: " << resolved_input_map);
      }
    } else {
      std::vector<InputMapDevice> devices;
      std::string parse_error;
      if (parse_input_map_config(resolved_input_map, devices, parse_error)) {
        start_input_map_threads(devices, resolved_input_map);
      } else {
        LOGE(parse_error);
      }
    }
  }

  if (!SDK::Init()) {
    LOGE( "Init failed" );
    g_stop.store(true, std::memory_order_relaxed);
    join_input_map_threads();
    return 1;
  }
  g_download_dir = download_dir;
  g_auto_sync_enabled.store(sync_dir_configured, std::memory_order_relaxed);

  auto cleanup_sdk = []() {
    g_shutting_down.store(true);
    SDK::Release();
  };

  QuietCallback cb;

  // Main connect loop (keepalive-aware)
  while (!g_stop.load()) {
    maybe_log_force_close();
    SDK::CrDeviceHandle handle = 0;
    const SDK::ICrCameraObjectInfo *selected = nullptr;
    SDK::ICrEnumCameraObjectInfo *enum_list = nullptr;
    SDK::ICrCameraObjectInfo *created = nullptr;
    cb.connected = false;
    cb.conn_finished = false;
    cb.last_error_code = 0;
    g_reconnect.store(false);

    bool ok = try_connect_once(explicit_host, explicit_mac, explicit_model, download_dir, verbose, auth_user, auth_pass, cb, handle, selected, enum_list, created);
    
    if (!ok) {
      disconnect_and_release(handle, created, enum_list);
      if (g_keepalive.count() == 0) {
	LOGE( "Exiting (no keepalive)" );
        g_stop.store(true, std::memory_order_relaxed);
        join_input_map_threads();
	cleanup_sdk();
	return 2;
      }
      if (verbose) LOGI( "Retrying in " << g_keepalive.count() << " ms..." );

      // Allow Ctrl-C to work while we wait to retry.
      unblock_sigint_in_this_thread();
      interruptible_sleep(g_keepalive);
      block_sigint_in_this_thread();
      
      if (g_stop.load(std::memory_order_relaxed) || g_force_close_requested.load(std::memory_order_relaxed)) {
        break;
      }
      continue;
    }

    // Create wake pipe once per connection (or do it once at program start)
    if (g_wake_pipe[0] == -1) {
      if (pipe(g_wake_pipe) == 0) {
	fcntl(g_wake_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(g_wake_pipe[1], F_SETFL, O_NONBLOCK);
      } else {
	// Pipe creation failed; logs just won’t wake the REPL (still works).
      }
    }

    inputThread = std::thread([handle, &cb, verbose]() {
      
      unblock_sigint_in_this_thread();
      
      // history setup
      History* hist = history_init();
      HistEvent ev{};
      history(hist, &ev, H_SETSIZE, 1000);
      // Pick a history file path
      std::filesystem::create_directories(get_cache_dir());
      const std::string histfile = join_path(get_cache_dir(), "history");
      // Load previous session history (ignore failure if file doesn't exist yet)
      history(hist, &ev, H_LOAD, histfile.c_str());
      // Optional niceties
      history(hist, &ev, H_SETUNIQUE, 1);   // no duplicate consecutive entries

      // line editor setup
      EditLine* el = el_init("sonshell", stdin, stdout, stderr);
      // Replace EL_SIGNAL with our getchar (signal handling is fine to keep too)
      el_set(el, EL_GETCFN, my_getc);
      el_set(el, EL_PROMPT, &prompt);
      el_set(el, EL_EDITOR, "emacs");  // or "vi"
      el_set(el, EL_HIST, history, hist);
      el_set(el, EL_SIGNAL, 0); // our SIGINT handler controls shutdown

      // bind tab to our completion function
      el_set(el, EL_ADDFN, "my-complete", "Complete commands", &complete);
      el_set(el, EL_BIND, "\t", "my-complete", NULL);

      el_set(el, EL_ADDFN, "trigger-shoot", "Trigger shutter release", &repl_trigger_shoot);
      el_set(el, EL_BIND, "\eOP", "trigger-shoot", NULL);    // xterm/VT100 F1
      el_set(el, EL_BIND, "\e[11~", "trigger-shoot", NULL);  // linux console F1
      el_set(el, EL_BIND, "\e[[A", "trigger-shoot", NULL);   // some terminals F1

      g_repl_active.store(true, std::memory_order_relaxed);

      // First flush of any queued messages that arrived between connect and REPL start (no refresh)
      (void)drain_logs_and_refresh(nullptr);

      // bind commands to code
      Context ctx;
      using Handler = std::function<int(const std::vector<std::string>&)>;
      std::unordered_map<std::string, Handler> cmd{
	{"help", [&](auto const& args)->int {
	  (void)args;
	  log_command_overview();
	  return 0;
	}},
	{"?",   [&](auto const& args)->int {
	  return cmd.at("help")(args);
	}},
	{"shoot", [&](auto const& args)->int {
	  (void)args;
	  if (!trigger_full_shutter_press(handle, verbose, "shoot")) {
	    return 2;
	  }
	  return 0;
	}},
	{"trigger", [&](auto const& args)->int {
	  return cmd.at("shoot")(args);
	}},
	{"focus", [&](auto const& args)->int {
	  //if (args.size() < 2) { std::cerr << "usage: capture start|stop\n"; return 2; }
	  //if (args[1] == "start") { std::cout << "Capture started\n"; }
	  //else if (args[1] == "stop") { std::cout << "Capture stopped\n"; }
	  //else { std::cerr << "unknown capture subcommand\n"; return 2; }

	  ctx.handle = handle;
	  if (verbose) LOGI("S1 shooting...");
	  if (verbose) LOGI("Shutter Half Press down");
	  SDK::CrDeviceProperty prop;
	  prop.SetCode(SDK::CrDevicePropertyCode::CrDeviceProperty_S1);
	  prop.SetValueType(SDK::CrDataType::CrDataType_UInt16);
	  prop.SetCurrentValue(SDK::CrLockIndicator::CrLockIndicator_Locked);
	  auto s1_lock_err = SDK::SetDeviceProperty(handle, &prop);
	  if (s1_lock_err != SDK::CrError_None) {
	    unsigned code = static_cast<unsigned>(s1_lock_err);
	    LOGE("Failed to half-press shutter: " << crsdk_err::error_to_name(s1_lock_err) << " (0x" << std::hex << code << std::dec << ")");
	    return 2;
	  }

	  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

	  if (verbose) LOGI("Shutter Half Press up");
	  prop.SetCurrentValue(SDK::CrLockIndicator::CrLockIndicator_Unlocked);
	  auto s1_unlock_err = SDK::SetDeviceProperty(handle, &prop);
	  if (s1_unlock_err != SDK::CrError_None) {
	    unsigned code = static_cast<unsigned>(s1_unlock_err);
	    LOGE("Failed to release half-press: " << crsdk_err::error_to_name(s1_unlock_err) << " (0x" << std::hex << code << std::dec << ")");
	    return 2;
	  }

	  if (verbose) LOGI("Focus complete.");
	  
	  return 0;
	}},
	{"sync", [&](auto const& args)->int {
	  // usage: sync [N | all | stop]  (default = 1)
	  int n = 1;
	  bool all = false;
	  if (args.size() >= 2) {
	    std::string a = args[1];
	    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c){ return std::tolower(c); });
	    if (a == "on") {
	      if (!ensure_sync_directory_configured("sync on")) return 2;
	      bool was = g_auto_sync_enabled.exchange(true, std::memory_order_acq_rel);
	      LOGI((was ? "Auto-sync already enabled." : "Auto-sync enabled."));
	      return 0;
	    }
	    else if (a == "off") {
	      bool was = g_auto_sync_enabled.exchange(false, std::memory_order_acq_rel);
	      LOGI((was ? "Auto-sync disabled." : "Auto-sync already disabled."));
	      return 0;
	    }
	    else if (a == "all") {
	      all = true;
	    }
	    else if (a == "stop") {
	      if (!g_sync_running.load(std::memory_order_acquire)) {
	        LOGI("Sync: nothing to stop.");
	        return 0;
	      }
	      g_sync_abort.store(true, std::memory_order_release);
	      g_sync_tokens.store(0, std::memory_order_release); // disarm new sync workers

	      bool cancel_sent = false;
	      if (handle) {
	        auto cancel_err = SDK::SendCommand(handle,
	                                           SDK::CrCommandId::CrCommandId_CancelContentsTransfer,
	                                           SDK::CrCommandParam_Down);
	        if (cancel_err == SDK::CrError_None) {
	          // Mirror other button-like commands by issuing an "up" event too.
	          (void)SDK::SendCommand(handle,
	                                 SDK::CrCommandId::CrCommandId_CancelContentsTransfer,
	                                 SDK::CrCommandParam_Up);
	          cancel_sent = true;
	        } else if (cancel_err == SDK::CrError_Api_Insufficient ||
	                   cancel_err == SDK::CrError_Generic_NotSupported ||
	                   cancel_err == SDK::CrError_Genric_NotSupported ||
	                   cancel_err == SDK::CrError_Connect_ContentsTransfer_NotSupported) {
	          LOGI("Sync: camera does not support immediate cancel (" << crsdk_err::error_to_name(cancel_err)
	               << "); finishing current file.");
	        } else {
	          unsigned code = static_cast<unsigned>(cancel_err);
	          LOGW("Sync: cancel command failed: " << crsdk_err::error_to_name(cancel_err)
	               << " (0x" << std::hex << code << std::dec << ")");
	        }
	      }

	      {
	        std::lock_guard<std::mutex> lk(cb.dl_mtx);
	        cb.dl_waiting = false;
	      }
	      cb.dl_cv.notify_all();

	      if (cancel_sent) {
	        LOGI("Sync: stopping (cancel requested; waiting for workers to exit).");
	      } else {
	        LOGI("Sync: stopping (will finish current file and then stop).");
	      }
	      return 0;
	    }
	    else {
	      try { n = std::max(1, std::stoi(args[1])); }
	      catch (...) { LOGE("usage: sync [count|all]"); return 2; }
	    }
	  }

	  if (!ensure_sync_directory_configured("sync")) {
	    return 2;
	  }
	  
	  bool expected_running = false;
	  if (!g_sync_running.compare_exchange_strong(expected_running, true,
	                                             std::memory_order_acq_rel)) {
	    LOGW("Sync already in progress. Use `sync stop` to cancel.");
	    return 0;
	  }

	  if (all) LOGI("Sync: ALL items from both slots (skip existing, keep names)...");
	  else     LOGI("Sync: latest " << n << " item(s) per slot (skip existing, keep names)...");

	  g_sync_abort.store(false, std::memory_order_release);

	  // Fire-and-forget worker so REPL stays responsive
	  try {
	    std::thread([&, all, n]{
	      struct SyncRunningReset {
		~SyncRunningReset() { g_sync_running.store(false, std::memory_order_release); }
	      } _sync_reset_guard;

	    if (all) g_sync_all.store(true, std::memory_order_relaxed);

	    // Arm tokens for exactly two callback invocations (slot1 + slot2)
	    g_sync_tokens.store(2, std::memory_order_relaxed);

	    // Reset active counter before we spawn the workers
	    g_sync_active.store(0, std::memory_order_relaxed);

	    // Kick both slots; same path as before
	    cb.OnNotifyRemoteTransferContentsListChanged(SDK::CrNotify_RemoteTransfer_Changed_Add,
							 SDK::CrSlotNumber_Slot1, all ? 0 : n);
	    cb.OnNotifyRemoteTransferContentsListChanged(SDK::CrNotify_RemoteTransfer_Changed_Add,
							 SDK::CrSlotNumber_Slot2, all ? 0 : n);

	    // wait briefly until at least one worker has started (or an abort/stop)
	    for (int i = 0; i < 40; ++i) { // ~1s total
	      if (g_sync_active.load(std::memory_order_relaxed) > 0 ||
	  	g_sync_abort.load(std::memory_order_acquire) ||
		  g_stop.load(std::memory_order_relaxed)) {
		break;
	      }
	      std::this_thread::sleep_for(std::chrono::milliseconds(25));
	    }
	    
	    // Background wait (so prompt remains free)
	    while (!g_stop.load(std::memory_order_relaxed) &&
		   g_sync_active.load(std::memory_order_relaxed) > 0) {
	      std::this_thread::sleep_for(std::chrono::milliseconds(50));
	    }

	    // Reset flag so future "sync N" behaves normally
	    g_sync_all.store(false, std::memory_order_relaxed);

	    // at the end of the detached sync thread
	    if (g_sync_abort.load(std::memory_order_acquire)) {
	      LOGI("Sync: stopped.");
	    } else {
	      LOGI("Sync: done.");
	    }
	    
	  }).detach();
	  } catch (...) {
	    g_sync_running.store(false, std::memory_order_release);
	    LOGE("Sync: failed to launch worker thread");
	    return 2;
	  }

	  // Return immediately; user can keep typing/issuing commands
	  return 0;
	}},
	{"exposure", [&](auto const& args)->int {
	  if (!handle) {
	    LOGE("exposure: camera handle unavailable");
	    return 2;
	  }
	  if (args.size() < 2) {
	    log_exposure_usage();
	    return 2;
	  }
	  std::string sub = to_lower_ascii(args[1]);
	  const ExposureSubcommand* entry = find_exposure_subcommand(sub);
	  if (!entry) {
	    log_exposure_usage();
	    return 2;
	  }
	  size_t extra = (args.size() > 2) ? (args.size() - 2) : 0;
	  if (extra < entry->min_args ||
	      (entry->max_args != kExposureUnlimitedArgs && extra > entry->max_args)) {
	    LOGE(entry->usage);
	    return 2;
	  }
	  return entry->handler(handle, verbose, args, 2);
	}},
	{"status", [&](auto const& args)->int {
	  (void)args;
	  if (!handle) {
	    LOGE("status: camera handle unavailable");
	    return 2;
	  }
	  StatusSnapshot snap;
	  bool got_any = collect_status_snapshot(handle, snap, verbose);
	  if (!got_any) {
	    LOGW("status: camera did not report detailed properties; showing defaults.");
	  }
	  std::string iso_display = snap.iso;
	  if (!snap.iso_actual.empty() && snap.iso_actual != snap.iso) {
	    if (!iso_display.empty() && iso_display != "--") iso_display += " [" + snap.iso_actual + "]";
	    else iso_display = snap.iso_actual;
	  }
	  std::string bracket_info;
	  if (!snap.focus_bracket_shots.empty()) {
	    bracket_info = "; Bracket: " + snap.focus_bracket_shots;
	    if (!snap.focus_bracket_range.empty()) {
	      bracket_info += " (range " + snap.focus_bracket_range + ")";
	    }
	  }
	  LOGI("Status:");
	  LOGI("  Body: " << snap.model << "  Lens: " << snap.lens << "  Serial: " << snap.serial);
	  LOGI("  Exposure: " << snap.f_number << "  " << snap.shutter << "  " << iso_display
	       << "; Mode: " << snap.exposure_program);
	  LOGI("  Focus: " << snap.focus_mode << "  Area: " << snap.focus_area
	       << "; Drive: " << snap.drive_mode << bracket_info);
	  LOGI("  Color: WB " << snap.white_balance << "; Silent: " << snap.silent_mode
	       << "; Shutter: " << snap.shutter_type);
	  LOGI("  Stabilization: Still " << snap.steady_still << " / Movie " << snap.steady_movie
	       << "; Movie mode: " << snap.movie_mode);
	  LOGI("  Video: Setting " << snap.movie_setting << "; Media: " << snap.movie_media
	       << "; Recording: " << snap.recording_state);
	  return 0;
	}},
	{"record", [&](auto const& args)->int {
	  if (args.size() < 2) {
	    LOGE("usage: record start|stop");
	    return 2;
	  }
	  std::string sub = args[1];
	  std::transform(sub.begin(), sub.end(), sub.begin(), [](unsigned char c){ return std::tolower(c); });
	  bool want_start = (sub == "start");
	  bool want_stop  = (sub == "stop");
	  if (!want_start && !want_stop) {
	    LOGE("usage: record start|stop");
	    return 2;
	  }

	  SDK::CrMovie_Recording_State initial_state = SDK::CrMovie_Recording_State_Not_Recording;
	  SDK::CrError state_err = SDK::CrError_None;
	  bool have_state = fetch_movie_recording_state(handle, initial_state, state_err);
	  if (!have_state && state_err != SDK::CrError_None) {
	    unsigned code = static_cast<unsigned>(state_err);
	    LOGW("Record: unable to query state: " << crsdk_err::error_to_name(state_err)
	         << " (0x" << std::hex << code << std::dec << ")");
	  }
	  if (have_state && verbose) {
	    LOGI("Record: current state " << movie_recording_state_to_string(initial_state));
	  }

	  if (want_start && have_state && initial_state == SDK::CrMovie_Recording_State_Recording) {
	    LOGI("Record: already recording.");
	    return 0;
	  }
	  if (want_stop && have_state && initial_state == SDK::CrMovie_Recording_State_Not_Recording) {
	    LOGI("Record: already stopped.");
	    return 0;
	  }

	  LOGI(want_start ? "Record: starting video..." : "Record: stopping video...");
	  bool press_ok = send_movie_record_button_press(handle, std::chrono::milliseconds(200), verbose);
	  if (!press_ok) {
	    return 2;
	  }

	  std::this_thread::sleep_for(std::chrono::milliseconds(500));

	  SDK::CrMovie_Recording_State final_state = SDK::CrMovie_Recording_State_Not_Recording;
	  SDK::CrError final_err = SDK::CrError_None;
	  if (fetch_movie_recording_state(handle, final_state, final_err)) {
	    LOGI("Record: camera state " << movie_recording_state_to_string(final_state));
	    if (want_start && final_state != SDK::CrMovie_Recording_State_Recording) {
	      LOGW("Record: camera did not report Recording state.");
	    }
	    if (want_stop && final_state == SDK::CrMovie_Recording_State_Recording) {
	      LOGW("Record: camera still reports Recording; retry stop if needed.");
	    }
	  } else if (final_err != SDK::CrError_None) {
	    unsigned code = static_cast<unsigned>(final_err);
	    LOGW("Record: unable to confirm state: " << crsdk_err::error_to_name(final_err)
	         << " (0x" << std::hex << code << std::dec << ")");
	  }

	  return 0;
	}},
        {"button", [&](auto const& args)->int {
          if (args.size() < 2) {
            LOGE("usage: button <dpad|playback|menu|shutter|movie> ...");
            return 2;
          }
          auto feature = args[1];
          std::transform(feature.begin(), feature.end(), feature.begin(),
                         [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
          if (feature == "dpad") {
            if (args.size() < 3) {
              LOGE("usage: button dpad left|right|up|down|center");
              return 2;
            }
            auto direction = args[2];
            std::transform(direction.begin(), direction.end(), direction.begin(),
                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            SDK::CrCameraButtonFunction button = SDK::CrCameraButtonFunction_LeftButton;
            const char* label = "dpad left";
            if (direction == "left") {
              button = SDK::CrCameraButtonFunction_LeftButton;
              label = "dpad left";
            } else if (direction == "right") {
              button = SDK::CrCameraButtonFunction_RightButton;
              label = "dpad right";
            } else if (direction == "up") {
              button = SDK::CrCameraButtonFunction_UpButton;
              label = "dpad up";
            } else if (direction == "down") {
              button = SDK::CrCameraButtonFunction_DownButton;
              label = "dpad down";
            } else if (direction == "center" || direction == "centre" || direction == "enter") {
              button = SDK::CrCameraButtonFunction_EnterButton;
              label = "dpad center";
            } else {
              LOGE("button: unknown direction '" << args[2] << "'; try 'button dpad left|right|up|down|center'");
              return 2;
            }
            if (!tap_camera_button(handle, button, label)) {
              return 2;
            }
            LOGI("button: tapped " << label << '.');
            return 0;
          } else if (feature == "playback") {
            if (!tap_camera_button(handle, SDK::CrCameraButtonFunction_PlaybackButton, "playback")) {
              return 2;
            }
            LOGI("button: tapped playback.");
            return 0;
          } else if (feature == "menu") {
            if (!tap_camera_button(handle, SDK::CrCameraButtonFunction_MenuButton, "menu")) {
              return 2;
            }
            LOGI("button: tapped menu.");
            return 0;
          } else if (feature == "shutter") {
            if (!trigger_full_shutter_press(handle, verbose, "button shutter")) {
              return 2;
            }
            LOGI("button: tapped shutter.");
            return 0;
          } else if (feature == "movie") {
            bool ok = send_movie_record_button_press(handle, std::chrono::milliseconds(200), verbose);
            if (!ok) return 2;
            LOGI("button: tapped movie.");
            return 0;
          } else {
            LOGE("button: unknown target '" << args[1] << "'; try 'button dpad ...', 'button playback', 'button menu', 'button shutter', or 'button movie'");
            return 2;
          }
        }},
	{"monitor", [&](auto const& args)->int {
	#ifdef SONSHELL_HEADLESS
	  (void)args;
	  LOGW("monitor command disabled in headless builds (rebuild without -DSONSHELL_HEADLESS=ON)");
	  return 2;
	#else
	  if (args.size() < 2) {
	    LOGE("usage: monitor start|stop");
	    return 2;
	  }
	  std::string sub = args[1];
	  std::transform(sub.begin(), sub.end(), sub.begin(), [](unsigned char c){ return std::tolower(c); });
	  if (sub == "start") {
	    bool ok = monitor_start(handle, verbose);
	    return ok ? 0 : 2;
	  }
	  if (sub == "stop") {
	    monitor_stop();
	    return 0;
	  }
	  LOGE("usage: monitor start|stop");
	  return 2;
	#endif
	}},
	{"power", [&](auto const& args)->int {
	  if (args.size() < 2) {
	    LOGE("usage: power off");
	    return 2;
	  }
	  std::string sub = args[1];
	  std::transform(sub.begin(), sub.end(), sub.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	  if (sub != "off") {
	    LOGE("power: unknown subcommand '" << sub << "'; try 'power off'.");
	    return 2;
	  }
	  if (!handle) {
	    LOGE("Camera handle not available; cannot power off.");
	    return 2;
	  }
	  LOGI("Sending power-off command to camera...");
	  auto err = SDK::SendCommand(handle, SDK::CrCommandId::CrCommandId_PowerOff, SDK::CrCommandParam_Down);
	  if (err != SDK::CrError_None) {
	    unsigned code = static_cast<unsigned>(err);
	    LOGE("Power-off command failed: " << crsdk_err::error_to_name(err) << " (0x" << std::hex << code << std::dec << ")");
	    return 2;
	  }
	  SDK::CrCameraPowerStatus status = SDK::CrCameraPowerStatus_PowerOn;
	  SDK::CrError status_err = SDK::CrError_None;
	  bool status_known = false;
	  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	  while (std::chrono::steady_clock::now() < deadline) {
	    if (fetch_camera_power_status(handle, status, status_err)) {
	      status_known = true;
	      if (status != SDK::CrCameraPowerStatus_PowerOn &&
	          status != SDK::CrCameraPowerStatus_TransitioningFromPowerOnToStandby) {
	        break;
	      }
	    } else if (status_err != SDK::CrError_None) {
	      break;
	    }
	    std::this_thread::sleep_for(std::chrono::milliseconds(200));
	  }
	  if (status_known) {
	    LOGI("Camera power status: " << camera_power_status_to_string(status)
	         << " (0x" << std::hex << static_cast<unsigned>(status) << std::dec << ")");
	    if (status == SDK::CrCameraPowerStatus_PowerOn) {
	      LOGW("Camera still reports PowerOn; enable 'Remote Power OFF/ON' and 'Network Standby' on the body to allow remote shutdown.");
	    }
	  } else if (status_err != SDK::CrError_None) {
	    LOGW("Could not read camera power status after power-off command: "
	         << crsdk_err::error_to_name(status_err)
	         << " (0x" << std::hex << static_cast<unsigned>(status_err) << std::dec << ")");
	  }
	  LOGI("Power-off command sent; waiting for camera to disconnect...");
	  return 0;
	}},
	{"quit", [&](auto const&)->int {
	  g_stop.store(true, std::memory_order_relaxed);   // <<< unify shutdown
	  return 99;
	}},
	{"exit", [&](auto const&)->int {
	  g_stop.store(true, std::memory_order_relaxed);   // <<< unify shutdown
	  return 99;
	}},
      };

      auto run_cli_command = [&](const std::vector<std::string>& args) -> int {
	if (args.empty()) return 0;
	auto it = cmd.find(args[0]);
	if (it == cmd.end()) {
	  LOGE("Unknown command: " << args[0]);
	  return 2;
	}
	std::lock_guard<std::mutex> exec_lk(g_command_exec_mutex);
	return it->second(args);
      };

      {
	std::lock_guard<std::mutex> lk(g_command_runner_mutex);
	g_command_runner = run_cli_command;
      }
      g_command_runner_cv.notify_all();

      while (!g_stop.load(std::memory_order_relaxed) && !g_reconnect.load(std::memory_order_relaxed)) {

        // Print logs that arrived just before we read; NO refresh here to avoid double prompt
        (void)drain_logs_and_refresh(nullptr);

	// if stop was requested while draining logs, bail *before* libedit can redraw the prompt
	if (g_stop.load(std::memory_order_relaxed) || g_reconnect.load(std::memory_order_relaxed)) break;

	int count = 0;
	errno = 0;
	const char* s = el_gets(el, &count);

	if (!s) {
	  if (g_stop.load()) break;
	  if (errno == EINTR) {
	    if (g_stop.load()) break;   // stop immediately after ^C
	    continue;                   // only continue for non-stop EINTRs
	  }
	  if (feof(stdin)) { g_stop.store(true); break; }
	  // Ctrl-D / EOF at empty prompt: el_gets() -> NULL, count==0, errno==0
	  if (count == 0 && errno == 0) { g_stop.store(true); break; }

	  // Fully drain wake pipe and clear the pending flag
	  if (g_wake_pipe[0] != -1) {
	    char buf[256];
	    while (true) {
	      ssize_t n = read(g_wake_pipe[0], buf, sizeof(buf));
	      if (n <= 0) break;
	    }
	  }
	  g_wake_pending.store(false, std::memory_order_relaxed);

	  // Print any queued logs, but DO NOT refresh here (to avoid duplicate prompts)
	  (void)drain_logs_and_refresh(nullptr);

	  // Go back to el_gets(); it will draw the prompt exactly once.
	  continue;
	}

	std::string line(s, count);
	if (!line.empty() && line.back() == '\n') line.pop_back();
	if (line.empty()) continue;

	history(hist, &ev, H_ENTER, line.c_str());

	auto args = tokenize(line);
	int rc = run_cli_command(args);
	if (rc == 99) break;  // already set g_stop above
	
	// Print logs produced by the command; let el_gets() render the next prompt once.
	drain_logs_and_refresh(nullptr);
      }

      // save historry
      history(hist, &ev, H_SAVE, histfile.c_str());
      history_end(hist);
      el_end(el);

      g_repl_active.store(false, std::memory_order_relaxed);
      {
	std::lock_guard<std::mutex> lk(g_command_runner_mutex);
	g_command_runner = nullptr;
      }
      g_command_runner_cv.notify_all();

      // Ensure the prompt line is cleared so shutdown logs start cleanly
      std::fputs("\r\033[K", stdout);
      std::fflush(stdout);

    });

    // Connected: wait until stop or disconnect signaled
    while (!g_stop.load() && !g_reconnect.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 1) Stop the REPL first so it cannot redraw a prompt during shutdown.
    if (inputThread.joinable()) {
      monitor_stop();
      // Nudge the REPL in case it’s waiting on poll(): send a wake byte.
      if (g_wake_pipe[1] != -1) { char x = 0; (void)!write(g_wake_pipe[1], &x, 1); }
      inputThread.join();
    }
    
    // 2) Now log and disconnect the camera; no prompt can appear anymore.
    if (verbose) LOGI( "Shutting down connection..." );
    disconnect_and_release(handle, created, enum_list);
    
    // 3) Join any download workers.
    for (auto &t : g_downloadThreads) {
      if (!t.joinable()) continue;
      if (g_force_close_requested.load(std::memory_order_relaxed)) {
        t.detach();
      } else {
        t.join();
      }
    }
    g_downloadThreads.clear();
    
    // 4) Close wake pipe at the very end.
    if (g_wake_pipe[0] != -1) { close(g_wake_pipe[0]); g_wake_pipe[0] = -1; }
    if (g_wake_pipe[1] != -1) { close(g_wake_pipe[1]); g_wake_pipe[1] = -1; }

    if (g_stop.load()) break;

    if (g_keepalive.count() == 0) {
      LOGE( "Disconnected and keepalive disabled; exiting." );
      break;
    }

    if (verbose) LOGI( "Disconnected; will retry in " << g_keepalive.count() << " ms..." );

    // Allow Ctrl-C during keepalive sleep after disconnects too.
    unblock_sigint_in_this_thread();
    interruptible_sleep(g_keepalive);
    block_sigint_in_this_thread();
  }

  maybe_log_force_close();
  LOGI( "Shutting down..." );
  monitor_stop();
  for (auto &t : g_downloadThreads) {
    if (!t.joinable()) continue;
    if (g_force_close_requested.load(std::memory_order_relaxed)) {
      t.detach();
    } else {
      t.join();
    }
  }
  g_stop.store(true, std::memory_order_relaxed);
  join_input_map_threads();
  cleanup_sdk();
  return 0;
}
