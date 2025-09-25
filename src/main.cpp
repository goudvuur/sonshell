#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <csignal>
#include <chrono>
#include <sys/stat.h>
#include <thread>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <algorithm>
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

#include "CRSDK/CameraRemote_SDK.h"
#include "CRSDK/ICrCameraObjectInfo.h"
#include "CRSDK/IDeviceCallback.h"
#include "CRSDK/CrDeviceProperty.h"
#include "CRSDK/CrTypes.h"

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
static std::atomic<bool> g_boot_pull_only_missing{false};
static std::atomic<int>  g_boot_tokens{0};   // how many callbacks are marked as boot-spawned
static std::atomic<int>  g_boot_active{0};   // how many boot-spawned workers are still running

// ----------------------------
// Logging
// ----------------------------
enum class LogLevel { Info, Warn, Error, Debug };

struct LogItem {
  LogLevel level;
  std::string text;
};

static std::mutex        g_log_mtx;
static std::condition_variable g_log_cv;
static std::deque<LogItem> g_log_q;

// Enqueue a message from any thread.
static inline void log_enqueue(LogLevel lvl, std::string msg) {
  if (!g_repl_active.load(std::memory_order_relaxed)) {
    std::ostream& os = (lvl == LogLevel::Error) ? std::cerr : std::cout;
    os << msg << std::endl;
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

// Drain everything that's queued and repaint the prompt.
// Call ONLY from the input thread that owns `el`.
static inline bool drain_logs_and_refresh(EditLine* el_or_null) {
  std::deque<LogItem> local;
  {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    local.swap(g_log_q);
  }

  if (local.empty()) return false;

  // clear current line once
  std::fputs("\r\033[K", stdout);  // CR + clear-to-end-of-line

  for (auto& it : local) {
    std::ostream& os = (it.level == LogLevel::Error) ? std::cerr : std::cout;
    os << it.text << '\n';
  }
  std::cout.flush();
  std::cerr.flush();

  if (el_or_null) el_set(el_or_null, EL_REFRESH, 0);
  return true;
}


// Optional: blocking wait-with-timeout to reduce busy looping
static inline void wait_for_logs_or_timeout(int ms) {
  std::unique_lock<std::mutex> lk(g_log_mtx);
  if (g_log_q.empty()) {
    g_log_cv.wait_for(lk, std::chrono::milliseconds(ms));
  }
}

// poll()-based getchar so logs can wake the REPL
static int my_getc(EditLine*, char* c) {
  struct pollfd fds[2];
  int nfds = 1;
  fds[0].fd = STDIN_FILENO;   fds[0].events = POLLIN; fds[0].revents = 0;
  fds[1].fd = -1;             fds[1].events = POLLIN; fds[1].revents = 0;
  if (g_wake_pipe[0] != -1) { fds[1].fd = g_wake_pipe[0]; nfds = 2; }

  for (;;) {
    int r = poll(fds, nfds, -1);
    if (r < 0) {
      if (errno == EINTR) continue;   // try again
      return -1;                      // error
    }

    // Wake pipe: drain a chunk and signal "no char, try again"
    if (nfds == 2 && (fds[1].revents & POLLIN)) {
      char buf[256];
      (void)read(g_wake_pipe[0], buf, sizeof(buf));
      return -1;                      // wake (no char)
    }

    // Real input
    if (fds[0].revents & POLLIN) {
      ssize_t n = read(STDIN_FILENO, c, 1);
      if (n == 1) return 1;           // <-- IMPORTANT: return COUNT = 1
      if (n == 0)  return 0;          // EOF
      if (errno == EINTR) continue;
      return -1;                      // error
    }
  }
}

// ----------------------------
// Signals
// ----------------------------
static void signal_handler(int) {
  g_stop.store(true, std::memory_order_relaxed);
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
// Post-download command
// ----------------------------
static void run_post_cmd(const std::string &path, const std::string &file) {
  if (path.empty()) return;

  pid_t pid = fork();
  if (pid == 0) {
    // child
    execl(path.c_str(), path.c_str(), file.c_str(), (char *)nullptr);
    // If execl fails
    _exit(127);
  }
  // parent: we don't wait (SIGCHLD is ignored)
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
    LOGI( "[CB] OnDisconnected: 0x" << std::hex << error << std::dec << " (" << crsdk_err::error_to_name(error) << ")" );
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
      if (it == last_prop_vals.end() || it->second != val) {
	last_prop_vals[code] = val;
	if (verbose) {
	  const char *name = crsdk_util::prop_code_to_name(code);
	  LOGI( tag << ": " << name << " (0x" << std::hex << code << std::dec << ") -> " << (long long)val );
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

  void OnNotifyRemoteTransferContentsListChanged(CrInt32u notify, CrInt32u slotNumber, CrInt32u addSize) override {
    if (g_shutting_down.load()) return;
    if (g_stop.load(std::memory_order_relaxed)) return;
    
    LOGI( "[CB] ContentsListChanged: notify=0x" << std::hex << notify << std::dec << " slot=" << slotNumber << " add=" << addSize );
    if (notify != SDK::CrNotify_RemoteTransfer_Changed_Add) return;

    // Was this invocation triggered by boot-pull?
    bool is_boot = false;
    for (int tok = g_boot_tokens.load(std::memory_order_relaxed); tok > 0; ) {
      if (g_boot_tokens.compare_exchange_weak(tok, tok - 1, std::memory_order_relaxed)) {
	is_boot = true;
	break;
      }
    }
    
    try {
      g_downloadThreads.emplace_back([this, slotNumber, addSize, is_boot]() {
	if (is_boot) g_boot_active.fetch_add(1, std::memory_order_relaxed);
	auto _boot_guard = std::unique_ptr<void, void(*)(void*)>{nullptr, [](void*) {
	  if (g_boot_active.load(std::memory_order_relaxed) > 0)
	    g_boot_active.fetch_sub(1, std::memory_order_relaxed);
	}};
	SDK::CrDeviceHandle handle = this->device_handle;
	if (!handle) return;
	SDK::CrSlotNumber slot = (slotNumber == SDK::CrSlotNumber_Slot2) ? SDK::CrSlotNumber_Slot2 : SDK::CrSlotNumber_Slot1;

	// Robust fetch: date list -> latest day -> contents for that day
	SDK::CrContentsInfo *list = nullptr; CrInt32u count = 0; SDK::CrError resList= SDK::CrError_None;
	const int max_attempts = 10;
	for (int attempt = 1; attempt <= max_attempts; ++attempt) {
	  if (g_stop.load(std::memory_order_relaxed)) {
	    if (list) SDK::ReleaseRemoteTransferContentsInfoList(handle, list);
	    return;
	  }
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
	    resList = SDK::GetRemoteTransferContentsInfoList(handle, slot, SDK::CrGetContentsInfoListType_Range_Day, &latest, 0, &list, &count);
	    SDK::ReleaseRemoteTransferCapturedDateList(handle, dateList);
	    if (resList == SDK::CrError_None && list && count > 0) break;
	    if (list) { SDK::ReleaseRemoteTransferContentsInfoList(handle, list); list = nullptr; count = 0; }
	  } else {
	    if (dateList) SDK::ReleaseRemoteTransferCapturedDateList(handle, dateList);
	  }
	  std::this_thread::sleep_for(std::chrono::milliseconds(150 * attempt));
	}
	if (resList != SDK::CrError_None) {
	  LOGE("[ERROR] Failed to get remote contents info list (err=0x" << std::hex << resList << std::dec << ")");
	  if (list) SDK::ReleaseRemoteTransferContentsInfoList(handle, list);
	  return;
	}
	if (!list || count == 0) {
	  LOGI("[INFO] No contents found for latest day (slot=" << (int)slot << ")");
	  if (list) SDK::ReleaseRemoteTransferContentsInfoList(handle, list);
	  return;
	}

	// pick newest N = addSize (or 1)
	CrInt32u want = (addSize > 0 ? addSize : 1);
	std::vector<CrInt32u> idx(count);
	for (CrInt32u i = 0; i < count; ++i) idx[i] = i;
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

	for (CrInt32u k = 0; k < want; ++k) {
	  if (g_stop.load(std::memory_order_relaxed)) break;
	  const SDK::CrContentsInfo &target = list[idx[k]];
	  if (target.contentId == 0) continue;

	  for (CrInt32u fi = 0; fi < target.filesNum; ++fi) {
	    if (g_stop.load(std::memory_order_relaxed)) break;
	    dl_waiting = true;
	    CrInt32u fileId = target.files[fi].fileId;

	    // determine original filename
	    std::string orig = basename_from_path(target.files[fi].filePath);
	    if (orig.empty()) {
	      std::ostringstream o; o << "content_" << (unsigned long long)target.contentId << "_file_" << fileId;
	      orig = o.str();
	    }

	    // Boot-only behavior: skip if exists; otherwise keep original name.
	    // Normal (post-boot) behavior: use unique_name() to add numeric suffixes.
	    std::string targetPath = join_path(g_download_dir, orig);
	    std::string finalName;
	    if (is_boot) {
	      if (std::filesystem::exists(targetPath)) {
		LOGI("[SKIP] already present: " << orig);
		dl_waiting = false;
		continue;
	      }
	      finalName = orig;                         // keep original name during boot-pull
	    } else {
	      finalName = unique_name(g_download_dir, orig); // normal suffixing after boot
	    }
	    
	    CrChar *saveDir = g_download_dir.empty() ? nullptr : const_cast<CrChar *>(reinterpret_cast<const CrChar *>(g_download_dir.c_str()));
	    if (g_stop.load(std::memory_order_relaxed)) {
	      dl_waiting = false;
	      break;
	    }
	    SDK::CrError dres = SDK::GetRemoteTransferContentsDataFile(handle, slot, target.contentId, fileId, 0x1000000, saveDir, const_cast<CrChar *>(finalName.c_str()));
	    if (dres != SDK::CrError_None) {
	      LOGE( "[ERROR] GetRemoteTransferContentsDataFile failed (0x" << std::hex << dres << std::dec << ")" );
	      dl_waiting = false;
	      continue;
	    }
	    {
	      std::unique_lock<std::mutex> lk(dl_mtx);
	      dl_cv.wait(lk, [&] { return !dl_waiting || g_stop.load(); });
	    }

	    if (g_stop.load(std::memory_order_relaxed)) break;
	    
	    if (dl_notify_code == SDK::CrNotify_RemoteTransfer_Result_OK) {
	      std::string saved = last_downloaded_file;
	      std::string base = saved;
	      size_t pos = base.find_last_of("/\\"); if (pos != std::string::npos) base = base.substr(pos + 1);
	      long long sizeB = 0; struct stat st{}; if (::stat(saved.c_str(), &st) == 0) sizeB = (long long)st.st_size;
	      LOGI( "[PHOTO] " << base << " (" << sizeB << " bytes)" );

	      // Run post-download hook if provided
	      if (!g_post_cmd.empty()) {
		run_post_cmd(g_post_cmd, saved);
	      }
	    } else if (!g_stop.load()) {
	      LOGE( "[ERROR] Download failed (notify=0x" << std::hex << dl_notify_code << std::dec << ")" );
	    }
	  }
	}

	SDK::ReleaseRemoteTransferContentsInfoList(handle, list);
      });
    } catch (...) {
      LOGE( "[ERROR] Failed to create download thread" );
    }
  }

  void OnNotifyContentsTransfer(CrInt32u, SDK::CrContentHandle, CrChar *) override {}

  void OnNotifyRemoteTransferResult(CrInt32u notify, CrInt32u per, CrChar *filename) override {
    std::unique_lock<std::mutex> lk(dl_mtx);
    if (filename) last_downloaded_file = filename; else last_downloaded_file.clear();
    dl_notify_code = notify; dl_progress = per;
    if (notify != SDK::CrNotify_RemoteTransfer_InProgress) {
      dl_waiting = false;
      dl_cv.notify_all();
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
static bool try_connect_once(const std::string &explicit_ip,
			     const std::string &explicit_mac,
			     const std::string &download_dir,
			     bool verbose,
			     QuietCallback &cb,
			     SDK::CrDeviceHandle &handle,
			     const SDK::ICrCameraObjectInfo *&selected,
			     SDK::ICrEnumCameraObjectInfo *&enum_list,
			     SDK::ICrCameraObjectInfo *&created) {
  cb.verbose = verbose;
  SDK::CrError err = SDK::CrError_None;
  selected = nullptr; enum_list = nullptr; created = nullptr; handle = 0;

  const bool using_direct_ip = !explicit_ip.empty();
  if (!using_direct_ip) {
    LOGI( "Searching for cameras..." );
    err = SDK::EnumCameraObjects(&enum_list, 1);
    if (err != SDK::CrError_None || !enum_list || enum_list->GetCount() == 0) {
      LOGE( "No cameras found (EnumCameraObjects)" );
      return false;
    }
    selected = enum_list->GetCameraObjectInfo(0);
  }
  else {
    LOGI( "Connecting with camera at " << explicit_ip << "..." );
    CrInt32u ipAddr = ip_to_uint32(explicit_ip);
    if (!ipAddr) {
      LOGE( "Bad IP" ); return false;
    }
    unsigned char mac[6] = {0, 0, 0, 0, 0, 0};
    if (!explicit_mac.empty() && !parse_mac(explicit_mac, mac)) {
      LOGE( "WARN bad MAC, ignoring: " << explicit_mac );
    }
    SDK::CrCameraDeviceModelList model = SDK::CrCameraDeviceModel_ILCE_6700;
    err = SDK::CreateCameraObjectInfoEthernetConnection(&created, model, ipAddr, mac, 0);
    if (err != SDK::CrError_None || !created) {
      LOGE( "CreateCameraObjectInfoEthernetConnection failed" );
      return false;
    }
    selected = created;
  }

  // fingerprint
  std::string fp_path = get_cache_dir() + "/fp_enumerated.bin";
  std::vector<char> fp_buf; CrChar *fp_ptr = nullptr; CrInt32u fp_len = 0;
  if (load_fingerprint(fp_path, fp_buf)) { fp_ptr = (CrChar *)fp_buf.data(); fp_len = (CrInt32u)fp_buf.size(); }

  err = SDK::Connect(const_cast<SDK::ICrCameraObjectInfo *>(selected), &cb, &handle,
		     SDK::CrSdkControlMode_RemoteTransfer, SDK::CrReconnecting_ON,
		     nullptr, nullptr, fp_ptr, fp_len);
  if (err != SDK::CrError_None || !handle) {
    LOGE( "Connect failed: 0x" << std::hex << err << std::dec );
    return false;
  }

  // wait handshake
  {
    std::unique_lock<std::mutex> lk(cb.mtx);
    cb.conn_cv.wait_for(lk, std::chrono::seconds(12), [&] { return cb.connected || cb.conn_finished || g_stop.load(); });
  }
  if (!cb.connected) {
    std::ostringstream _m;
    _m << "Camera not available";
    if (cb.last_error_code) _m << " error=0x" << std::hex << cb.last_error_code << std::dec;
    LOGE( _m.str() );
    
    return false;
  }

  cb.device_handle = handle;
  LOGI( "Connected. Ctrl+C to stop." );

  // persist fingerprint
  {
    char newfp[512] = {0}; CrInt32u nlen = 0;
    SDK::GetFingerprint(const_cast<SDK::ICrCameraObjectInfo *>(selected), newfp, &nlen);
    if (nlen > 0 && nlen <= sizeof(newfp)) save_fingerprint(fp_path, newfp, nlen);
  }

  (void)download_dir; // already assigned to global outside
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
  "shoot", "focus", "quit", "exit"
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

struct Context {
  SDK::CrDeviceHandle handle = 0;
  // add whatever shared state you need
};

int main(int argc, char **argv) {
  install_signal_handlers();
  block_sigint_in_this_thread();

  std::string explicit_ip, explicit_mac, download_dir;
  bool verbose = false;
  int boot_pull = 0;  // 0 = disabled

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--ip" && i + 1 < argc) explicit_ip = argv[++i];
    else if (a == "--mac" && i + 1 < argc) explicit_mac = argv[++i];
    else if (a == "--dir" && i + 1 < argc) download_dir = argv[++i];
    else if (a == "--verbose" || a == "-v") verbose = true;
    else if (a == "--cmd" && i + 1 < argc) g_post_cmd = argv[++i];
    else if (a == "--keepalive" && i + 1 < argc) {
      long long ms = std::atoll(argv[++i]);
      if (ms < 0) ms = 0;
      g_keepalive = std::chrono::milliseconds(ms);
    }
    else if (a == "--boot-pull" && i + 1 < argc) {
      boot_pull = std::max(0, std::atoi(argv[++i]));
    }
  }

  if (!SDK::Init()) {
    LOGE( "Init failed" );
    return 1;
  }
  g_download_dir = download_dir;

  auto cleanup_sdk = []() {
    g_shutting_down.store(true);
    SDK::Release();
  };

  QuietCallback cb;

  // Main connect loop (keepalive-aware)
  while (!g_stop.load()) {
    SDK::CrDeviceHandle handle = 0;
    const SDK::ICrCameraObjectInfo *selected = nullptr;
    SDK::ICrEnumCameraObjectInfo *enum_list = nullptr;
    SDK::ICrCameraObjectInfo *created = nullptr;
    cb.connected = false;
    cb.conn_finished = false;
    cb.last_error_code = 0;
    g_reconnect.store(false);

    bool ok = try_connect_once(explicit_ip, explicit_mac, download_dir, verbose, cb,
			       handle, selected, enum_list, created);
    if (!ok) {
      disconnect_and_release(handle, created, enum_list);
      if (g_keepalive.count() == 0) {
	LOGE( "Exiting (no keepalive)" );
	cleanup_sdk();
	return 2;
      }
      LOGI( "Retrying in " << g_keepalive.count() << " ms..." );

      // Allow Ctrl-C to work while we wait to retry.
      unblock_sigint_in_this_thread();
      interruptible_sleep(g_keepalive);
      block_sigint_in_this_thread();
      
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

    // trigger the boot download after connect
    // trigger the boot download after connect (non-blocking)
    if (boot_pull > 0) {
      // Snapshot how many download threads exist *before* we trigger boot-pull
      const size_t start_threads = g_downloadThreads.size();

      std::thread([&, start_threads, boot_pull]() {
	LOGI("Boot-pull (async): latest " << boot_pull << " item(s) per slot (skip existing)...");
	g_boot_pull_only_missing.store(true, std::memory_order_relaxed);

	// Arm exactly two boot tokens for the two callback invocations
	g_boot_tokens.store(2, std::memory_order_relaxed);
	
	// Kick off both slots (reuses the same code path as live notifications)
	cb.OnNotifyRemoteTransferContentsListChanged(SDK::CrNotify_RemoteTransfer_Changed_Add,
						     SDK::CrSlotNumber_Slot1, boot_pull);
	cb.OnNotifyRemoteTransferContentsListChanged(SDK::CrNotify_RemoteTransfer_Changed_Add,
						     SDK::CrSlotNumber_Slot2, boot_pull);

	// Wait until all boot workers are done OR user pressed Ctrl-C
	while (!g_stop.load(std::memory_order_relaxed) &&
	       g_boot_active.load(std::memory_order_relaxed) > 0) {
	  std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	g_boot_pull_only_missing.store(false, std::memory_order_relaxed);
	LOGI("Boot-pull (async): done.");
      }).detach();
    }

    inputThread = std::thread([handle]() {
      
      unblock_sigint_in_this_thread();
      
      // history setup
      History* hist = history_init();
      HistEvent ev{};
      history(hist, &ev, H_SETSIZE, 1000);

      // line editor setup
      EditLine* el = el_init("sonshell", stdin, stdout, stderr);
      // Replace EL_SIGNAL with our getchar (signal handling is fine to keep too)
      el_set(el, EL_GETCFN, my_getc);
      el_set(el, EL_PROMPT, &prompt);
      el_set(el, EL_EDITOR, "emacs");  // or "vi"
      el_set(el, EL_HIST, history, hist);
      el_set(el, EL_SIGNAL, 1); // let libedit cooperate with signals

      // bind tab to our completion function
      el_set(el, EL_ADDFN, "my-complete", "Complete commands", &complete);
      el_set(el, EL_BIND, "\t", "my-complete", NULL);

      g_repl_active.store(true, std::memory_order_relaxed);

      // First flush of any queued messages that arrived between connect and REPL start (no refresh)
      (void)drain_logs_and_refresh(nullptr);

      // bind commands to code
      Context ctx;
      using Handler = std::function<int(const std::vector<std::string>&)>;
      std::unordered_map<std::string, Handler> cmd{
	{"shoot", [&](auto const& args)->int {
	  //if (args.size() < 2) { std::cerr << "usage: connect <host>\n"; return 2; }
	  ctx.handle = handle;

	  SDK::SendCommand(handle, SDK::CrCommandId::CrCommandId_Release, SDK::CrCommandParam_Down);
	  std::this_thread::sleep_for(std::chrono::milliseconds(500));
	  SDK::SendCommand(handle, SDK::CrCommandId::CrCommandId_Release, SDK::CrCommandParam_Up);
	  LOGI( "Shutter triggered." );

	  return 0;
	}},
	{"focus", [&](auto const& args)->int {
	  //if (args.size() < 2) { std::cerr << "usage: capture start|stop\n"; return 2; }
	  //if (args[1] == "start") { std::cout << "Capture started\n"; }
	  //else if (args[1] == "stop") { std::cout << "Capture stopped\n"; }
	  //else { std::cerr << "unknown capture subcommand\n"; return 2; }

	  ctx.handle = handle;
	  SDK::CrDeviceProperty prop;
	  prop.SetCode(SDK::CrDevicePropertyCode::CrDeviceProperty_S1);
	  prop.SetCurrentValue(SDK::CrLockIndicator::CrLockIndicator_Locked);
	  prop.SetValueType(SDK::CrDataType::CrDataType_UInt16);
	  SDK::SetDeviceProperty(handle, &prop);
	  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	  prop.SetCurrentValue(SDK::CrLockIndicator::CrLockIndicator_Unlocked);
	  SDK::SetDeviceProperty(handle, &prop);
	  LOGI( "Autofocus activated." );

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

      while (!g_stop.load(std::memory_order_relaxed)) {

        // Print logs that arrived just before we read; NO refresh here to avoid double prompt
        (void)drain_logs_and_refresh(nullptr);

	int count = 0;
	errno = 0;
	const char* s = el_gets(el, &count);

	if (!s) {
	  if (g_stop.load()) break;
	  if (errno == EINTR) continue;
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
	auto it = cmd.find(args[0]);
	if (it == cmd.end()) {
	  LOGE("Unknown command: " << args[0]);
	  continue;
	}
	int rc = it->second(args);
	if (rc == 99) break;  // already set g_stop above

	// print logs produced by the command, then repaint prompt
	drain_logs_and_refresh(el);
      }

      history_end(hist);
      el_end(el);

      g_repl_active.store(false, std::memory_order_relaxed);

      // makes sure the next shutdown line is printed on the next line, not next to the prompt
      LOGI("");

    });

    // Connected: wait until stop or disconnect signaled
    while (!g_stop.load() && !g_reconnect.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOGI( "Shutting down connection..." );
    disconnect_and_release(handle, created, enum_list);

    for (auto &t : g_downloadThreads) {
      if (t.joinable()) t.join();
    }
    g_downloadThreads.clear();

    if (inputThread.joinable()) {
      inputThread.join();
    }

    if (g_wake_pipe[0] != -1) { close(g_wake_pipe[0]); g_wake_pipe[0] = -1; }
    if (g_wake_pipe[1] != -1) { close(g_wake_pipe[1]); g_wake_pipe[1] = -1; }

    if (g_stop.load()) break;

    if (g_keepalive.count() == 0) {
      LOGE( "Disconnected and keepalive disabled; exiting." );
      break;
    }

    LOGI( "Disconnected; will retry in " << g_keepalive.count() << " ms..." );

    // Allow Ctrl-C during keepalive sleep after disconnects too.
    unblock_sigint_in_this_thread();
    interruptible_sleep(g_keepalive);
    block_sigint_in_this_thread();
  }

  LOGI( "Shutting down..." );
  for (auto &t : g_downloadThreads) {
    if (t.joinable()) t.join();
  }
  cleanup_sdk();
  return 0;
}
