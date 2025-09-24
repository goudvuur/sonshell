#include <iostream>
#include <string>
#include <vector>
#include <unordered_set>
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
#include <sys/types.h>
#include <fcntl.h>

// Sony Camera Remote SDK headers
#include "CRSDK/CameraRemote_SDK.h"
#include "CRSDK/ICrCameraObjectInfo.h"
#include "CRSDK/IDeviceCallback.h"

#include "prop_names_generated.h" // provides crsdk_util::prop_code_to_name()
#include "error_names_generated.h"  // provides crsdk_err::error_to_name(), warning_to_name()

namespace SDK = SCRSDK;

// ---- graceful shutdown flag ----
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_shutting_down{false};

static void signal_handler(int signo) { (void)signo; g_stop.store(true, std::memory_order_relaxed); }
static void install_signal_handlers() {
  struct sigaction sa{}; sa.sa_handler = signal_handler; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr); sigaction(SIGTERM, &sa, nullptr);
#ifdef SIGHUP
  sigaction(SIGHUP, &sa, nullptr);
#endif
#ifdef SIGPIPE
  struct sigaction ign{}; ign.sa_handler = SIG_IGN; sigemptyset(&ign.sa_mask); ign.sa_flags = 0;
  sigaction(SIGPIPE, &ign, nullptr);
#endif
}

static CrInt32u ip_to_uint32(const std::string& ip) {
  unsigned int a=0,b=0,c=0,d=0;
  if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a,&b,&c,&d) != 4) return 0;
  return (CrInt32u)( (a) | (b<<8) | (c<<16) | (d<<24) );
}

static bool parse_mac(const std::string& macStr, unsigned char out[6]) {
  int vals[6];
  if (std::sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x",
		  &vals[0],&vals[1],&vals[2],&vals[3],&vals[4],&vals[5]) == 6) {
    for (int i=0;i<6;++i) out[i] = static_cast<unsigned char>(vals[i] & 0xFF);
    return true;
  }
  return false;
}

// Small helpers to persist fingerprint
static std::string get_cache_dir() {
  const char* home = std::getenv("HOME");
  std::string base = (home && *home) ? std::string(home) + "/.cache/sony_watch" : std::string("./.sony_watch_cache");
  // ensure directory exists
  struct stat st{};
  if (stat(base.c_str(), &st) != 0) {
    mkdir(base.c_str(), 0700);
  } else if (!S_ISDIR(st.st_mode)) {
    // try to recreate as dir
    ::remove(base.c_str());
    mkdir(base.c_str(), 0700);
  }
  return base;
}

static bool load_fingerprint(const std::string& path, std::vector<char>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 1024) { std::fclose(f); return false; }
  out.resize(sz);
  size_t rd = std::fread(out.data(), 1, (size_t)sz, f);
  std::fclose(f);
  return rd == (size_t)sz;
}

static bool save_fingerprint(const std::string& path, const char* data, size_t len) {
  if (!data || len == 0) return false;
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  size_t wr = std::fwrite(data, 1, len, f);
  std::fclose(f);
  return wr == len;
}

// ---- Quiet, focused device callback ----
class QuietCallback : public SDK::IDeviceCallback {
public:
  // Device handle (set after successful connect) so we can query properties
  SDK::CrDeviceHandle device_handle = 0;

  // Track latest seen values for a subset of interesting properties
  std::unordered_map<CrInt32u, CrInt64u> last_prop_vals;

private:

  void log_changed_properties_(const char* tag) {
    if (!device_handle) return;

    SDK::CrDeviceProperty* props = nullptr;
    CrInt32 nprop = 0;
    auto er = SDK::GetDeviceProperties(device_handle, &props, &nprop);
    if (er != SDK::CrError_None || nprop <= 0 || !props) return;

    for (int i = 0; i < nprop; ++i) {
      CrInt32u code = props[i].GetCode();
      CrInt64u val = props[i].GetCurrentValue();
      auto it = last_prop_vals.find(code);
      if (it == last_prop_vals.end() || it->second != val) {
	last_prop_vals[code] = val;
	if (verbose) {
	  const char* name = crsdk_util::prop_code_to_name(code);
	  std::cout << tag << ": " << name
		    << " (0x" << std::hex << code << std::dec
		    << ") changed to " << (long long)val << std::endl;
	}
      }
    }
    SDK::ReleaseDeviceProperties(device_handle, props);
  }

public:
  std::mutex mtx;
  std::condition_variable cv;
  bool contents_changed = false;
  bool verbose = false; // set by CLI

  // Connection handshake state
  std::condition_variable conn_cv;
  bool connected = false;
  bool conn_finished = false;
  CrInt32u last_error_code = 0;

  // Track last content change event
  CrInt32u last_contents_slot = 0;
  // Variables for download synchronization
  std::mutex dl_mtx;
  std::condition_variable dl_cv;
  bool dl_waiting = false;
  CrInt32u dl_notify_code = 0;
  CrInt32u dl_progress = 0;
  std::string last_downloaded_file;

  // de-dup spammy warn/error codes
  CrInt32u last_warning = 0;
  CrInt32u last_error = 0;

  void OnConnected(SDK::DeviceConnectionVersioin v) override {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    if (verbose) std::cout << "[CB] OnConnected: version=" << v << std::endl;
    {
      std::lock_guard<std::mutex> lk(mtx);
      connected = true;
      conn_finished = true;
    }
    conn_cv.notify_all();
  }
  void OnDisconnected(CrInt32u error) override {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    std::cout << "[CB] OnDisconnected: error=" << "0x" << std::hex << error << std::dec << " (" << crsdk_err::error_to_name(error) << ")" << std::endl;
    {
      std::lock_guard<std::mutex> lk(mtx);
      last_error_code = error;
      conn_finished = true;
    }
    conn_cv.notify_all();
  }
  void OnWarning(CrInt32u warning) override {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;

    if (warning != last_warning) {
      std::cout << "[CB] OnWarning: " << crsdk_err::warning_to_name(warning) << " (" << "0x" << std::hex << warning << std::dec << ")" << std::endl;
      last_warning = warning;
    }
  }
  void OnError(CrInt32u error) override {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    if (error != last_error) {
      std::cout << "[CB] OnError: " << crsdk_err::error_to_name(error) << " (" << "0x" << std::hex << error << std::dec << ")" << std::endl;
      last_error = error;
    }
    {
      std::lock_guard<std::mutex> lk(mtx);
      last_error_code = error;
      conn_finished = true; // treat as failed connect if it happens right after connect
    }
    conn_cv.notify_all();
  }
  void OnPropertyChanged() override {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    log_changed_properties_("[CB] OnPropertyChanged") ;
  }
  void OnLvPropertyChanged() override {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    log_changed_properties_("[CB] OnLvPropertyChanged");
  }
  void OnNotifyRemoteTransferContentsListChanged(CrInt32u notify, CrInt32u slotNumber, CrInt32u addSize) override {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    std::cout << "[CB] ContentsListChanged: notify=" << "0x" << std::hex << notify << std::dec
	      << " slot=" << slotNumber << " add=" << addSize << std::endl;
    if (notify == SDK::CrNotify_RemoteTransfer_Changed_Add) {
      std::lock_guard<std::mutex> lk(mtx);
      contents_changed = true;
      last_contents_slot = slotNumber;
      cv.notify_all();
    }
  }
  void OnNotifyContentsTransfer(CrInt32u notify, SDK::CrContentHandle contentHandle, CrChar* filename) override {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    if (verbose) {
      const char* file = filename ? filename : "(null)";
      std::cout << "[CB] ContentsTransfer: notify=" << "0x" << std::hex << notify << std::dec
		<< " handle=" << "0x" << std::hex << contentHandle << std::dec
		<< " file=" << file << std::endl;
    }
  }
  void OnNotifyRemoteTransferResult(CrInt32u notify, CrInt32u per, CrInt8u* /*data*/, CrInt64u size) override {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    if (verbose) {
      std::cout << "[CB] RemoteTransferResult: notify=" << "0x" << std::hex << notify << std::dec
		<< " per=" << per << " size=" << size << std::endl;
    }
  }
  void OnNotifyRemoteTransferResult(CrInt32u notify, CrInt32u per, CrChar* filename) override {
    if (dl_waiting) {
      std::unique_lock<std::mutex> lk(dl_mtx);
      if (filename) last_downloaded_file = filename;
      else last_downloaded_file.clear();
      dl_notify_code = notify;
      dl_progress = per;
      if (notify != SDK::CrNotify_RemoteTransfer_InProgress) {
	// Transfer finished (either success or failure)
	dl_waiting = false;
	dl_cv.notify_all();
      } else {
	if (g_shutting_down.load(std::memory_order_relaxed)) return;
        if (verbose) {
	  std::cout << "[CB] Download progress: " << per << "%" << std::endl;
	}
      }
    } else if (g_shutting_down.load(std::memory_order_relaxed)) return;
    if (verbose) {
      std::cout << "[CB] RemoteTransferResult: notify=" << "0x" << std::hex << notify << std::dec
		<< " per=" << per;
      if (filename) std::cout << " file=" << filename;
      std::cout << std::endl;
    }
  }
  void OnNotifyFTPTransferResult(CrInt32u notify, CrInt32u numOfSuccess, CrInt32u numOfFail) override {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    if (verbose) {
      std::cout << "[CB] FTPTransferResult: notify=" << "0x" << std::hex << notify << std::dec
		<< " success=" << numOfSuccess << " fail=" << numOfFail << std::endl;
    }
  }
  void OnNotifyRemoteFirmwareUpdateResult(CrInt32u notify, const void* param) override {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    if (verbose) {
      std::cout << "[CB] FWUpdateResult: notify=" << "0x" << std::hex << notify << std::dec
		<< " param=" << param << std::endl;
    }
  }
  void OnReceivePlaybackTimeCode(CrInt32u timeCode) override {
    if (verbose) std::cout << "[CB] PlaybackTimeCode: " << timeCode << std::endl;
  }
  void OnNotifyMonitorUpdated(CrInt32u type, CrInt32u frameNo) override {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    if (verbose) {
      std::cout << "[CB] MonitorUpdated: type=" << "0x" << std::hex << type << std::dec
		<< " frameNo=" << frameNo << std::endl;
    }
  }
};

// Utility: print new files when detected (uses folder handle + filename)
static void scan_and_report_new(SDK::CrDeviceHandle handle,
                                std::unordered_set<SDK::CrContentHandle>& seen) {
  SDK::CrMtpFolderInfo* folders = nullptr;
  CrInt32u numFolders = 0;
  if (SDK::GetDateFolderList(handle, &folders, &numFolders) != SDK::CrError_None || !folders) return;
  for (CrInt32u i = 0; i < numFolders; ++i) {
    SDK::CrContentHandle* contents = nullptr;
    CrInt32u numContents = 0;
    if (SDK::GetContentsHandleList(handle, folders[i].handle, &contents, &numContents) == SDK::CrError_None && contents) {
      for (CrInt32u j = 0; j < numContents; ++j) {
	auto h = contents[j];
	if (seen.find(h) == seen.end()) {
	  SDK::CrMtpContentsInfo info{};
	  if (SDK::GetContentsDetailInfo(handle, h, &info) == SDK::CrError_None) {
	    const char* fname = info.fileName ? info.fileName : "unknown";
	    std::cout << "[NEW] folder#" << (unsigned)folders[i].handle << "/" << fname << std::endl;
	  } else {
	    std::cout << "[NEW] handle=" << "0x" << std::hex << h << std::dec << std::endl;
	  }
	  seen.insert(h);
	}
      }
      SDK::ReleaseContentsHandleList(handle, contents);
    }
  }
  SDK::ReleaseDateFolderList(handle, folders);
}

int main(int argc, char** argv) {
  install_signal_handlers();

  std::string explicit_ip;
  std::string explicit_mac;
  std::string download_dir;
  bool verbose = false;

  // Simple CLI parsing
  for (int i=1;i<argc;i++) {
    std::string a = argv[i];
    if (a == "--ip" && i+1<argc) { explicit_ip = argv[++i]; }
    else if (a == "--mac" && i+1<argc) { explicit_mac = argv[++i]; }
    else if (a == "--dir" && i+1<argc) { download_dir = argv[++i]; }
    else if (a == "--verbose" || a == "-v") { verbose = true; }
  }

  std::cout << "[watch-quiet] Init SDK..." << std::endl;
  if (!SDK::Init()) { std::cerr << "Init failed\n"; return 1; }

  QuietCallback callback;
  callback.verbose = verbose;

  SDK::CrError err = SDK::CrError_None;
  const SDK::ICrCameraObjectInfo* selected = nullptr;
  SDK::ICrEnumCameraObjectInfo* enum_list = nullptr;
  SDK::ICrCameraObjectInfo* created = nullptr;
  SDK::CrDeviceHandle handle = 0;

  auto cleanup = [&](){
    g_shutting_down.store(true, std::memory_order_relaxed);
    std::cout << "[watch-quiet] Cleaning up..." << std::endl;
    if (handle) {
      SDK::Disconnect(handle);
      SDK::ReleaseDevice(handle);
      handle = 0;
    }
    if (created) { created->Release(); created = nullptr; }
    if (enum_list) { enum_list->Release(); enum_list = nullptr; }
    SDK::Release();
    std::cout << "[watch-quiet] Cleanup done." << std::endl;
  };

  // If --ip is given, skip enumeration entirely
  bool using_direct_ip = !explicit_ip.empty();
  if (!using_direct_ip) {
    if (verbose) std::cout << "[watch-quiet] Enumerating..." << std::endl;
    err = SDK::EnumCameraObjects(&enum_list, 3);
    if (err != SDK::CrError_None) {
      std::cerr << "[watch-quiet] EnumCameraObjects failed: 0x" << std::hex << err << std::dec << std::endl;
      cleanup();
      return 11;
    }
    if (!enum_list || enum_list->GetCount() == 0) {
      std::cerr << "[watch-quiet] No cameras found during enumeration. (Tip: use --ip <addr>)" << std::endl;
      cleanup();
      return 12;
    }
    auto count = enum_list->GetCount();
    for (CrInt32u i = 0; i < count; ++i) {
      auto info = enum_list->GetCameraObjectInfo(i);
      if (!info) continue;
      const char* connType = info->GetConnectionTypeName();
      if (connType && std::string(connType) == "IP") { selected = info; break; }
      if (!selected) selected = info;
    }
  } else {
    // build object info from explicit IP
    CrInt32u ipAddr = ip_to_uint32(explicit_ip);
    if (ipAddr == 0) { std::cerr << "Bad IP\n"; cleanup(); return 2; }
    unsigned char mac[6] = {0,0,0,0,0,0};
    if (!explicit_mac.empty() && !parse_mac(explicit_mac, mac)) {
      std::cerr << "WARN: bad MAC, ignoring: " << explicit_mac << std::endl;
    }
    SDK::CrCameraDeviceModelList model = SDK::CrCameraDeviceModel_ILCE_6700;
    err = SDK::CreateCameraObjectInfoEthernetConnection(&created, model, ipAddr, mac, 0);
    if (err != SDK::CrError_None || !created) { std::cerr << "CreateCameraObjectInfoEthernetConnection failed\n"; cleanup(); return 3; }
    selected = created;
  }

  if (!selected) {
    std::cerr << "[watch-quiet] No camera selected. Aborting." << std::endl;
    cleanup();
    return 13;
  }

  // Build fingerprint path
  std::string cache_dir = get_cache_dir();
  std::string fp_path = cache_dir + (using_direct_ip ? ("/fp_" + explicit_ip + ".bin") : "/fp_enumerated.bin");

  // Load cached fingerprint if available
  std::vector<char> fp_buf;
  CrChar* fp_ptr = nullptr;
  CrInt32u fp_len = 0;
  if (load_fingerprint(fp_path, fp_buf)) {
    fp_ptr = reinterpret_cast<CrChar*>(fp_buf.data());
    fp_len = static_cast<CrInt32u>(fp_buf.size());
    if (verbose) std::cout << "[watch-quiet] Using cached fingerprint: " << fp_path << " (" << fp_len << " bytes)" << std::endl;
  }

  // Connect (first attempt, possibly with fingerprint)
  if (using_direct_ip) {
    std::cout << "[watch-quiet] Connecting to " << explicit_ip << " ..." << std::endl;
  } else {
    std::cout << "[watch-quiet] Connecting to enumerated camera..." << std::endl;
  }
  err = SDK::Connect(const_cast<SDK::ICrCameraObjectInfo*>(selected), &callback, &handle,
		     SDK::CrSdkControlMode_RemoteTransfer, SDK::CrReconnecting_ON,
		     nullptr, nullptr, fp_ptr, fp_len);
  if (err != SDK::CrError_None || handle == 0) {
    std::cerr << "Connect failed: 0x" << std::hex << err << std::dec << " (" << crsdk_err::error_to_name(err) << ")\n";
    cleanup();
    return 4;
  }


  auto wait_for_handshake_with_grace = [&](int seconds, int grace_ms)->bool{
    std::unique_lock<std::mutex> lk(callback.mtx);
    bool ok = callback.conn_cv.wait_for(lk, std::chrono::seconds(seconds), [&]{
      return callback.connected || callback.conn_finished || g_stop.load();
    });
    if (!ok && !callback.connected && grace_ms > 0) {
      ok = callback.conn_cv.wait_for(lk, std::chrono::milliseconds(grace_ms), [&]{
	return callback.connected || callback.conn_finished || g_stop.load();
      });
    }
    return ok && callback.connected;
  };


  bool got = wait_for_handshake_with_grace((fp_ptr?5:12), 1500);
  if (!got || !callback.connected) {
    // 0x820a = pairing required / auth error; retry once WITHOUT fingerprint
    bool pairing_error = (callback.last_error_code == 0x820a);
    if (pairing_error && (fp_ptr != nullptr)) {
      std::cerr << "[watch-quiet] Pairing mismatch (0x820a). Retrying without cached fingerprint..." << std::endl;
      SDK::Disconnect(handle);
      SDK::ReleaseDevice(handle);
      handle = 0;
      callback.connected = false;
      callback.conn_finished = false;
      callback.last_error_code = 0;
      err = SDK::Connect(const_cast<SDK::ICrCameraObjectInfo*>(selected), &callback, &handle,
			 SDK::CrSdkControlMode_RemoteTransfer, SDK::CrReconnecting_ON,
			 nullptr, nullptr, nullptr, 0);
      if (err == SDK::CrError_None && handle != 0) {
	got = wait_for_handshake_with_grace(12, 2000);
      }
    }
  }

  if (!got || !callback.connected) {
    std::cerr << "Camera not available: ";
    if (callback.last_error_code) {
      std::cerr << "error=" << "0x" << std::hex << callback.last_error_code << std::dec << " (" << crsdk_err::error_to_name(callback.last_error_code) << ")\n";
    } else if (!got) {
      std::cerr << "timeout waiting for connection.\n";
    } else {
      std::cerr << "unknown connection failure.\n";
    }
    cleanup();
    return 5;
  }

  callback.device_handle = handle;
  std::cout << "[watch-quiet] Connected. Press Ctrl+C to stop." << std::endl;

  // Read current fingerprint from SDK and persist it for next runs
  {
    char new_fp[512] = {0};
    CrInt32u new_len = 0;
    SDK::GetFingerprint(const_cast<SDK::ICrCameraObjectInfo*>(selected), new_fp, &new_len);
    if (new_len > 0 && new_len <= sizeof(new_fp)) {
      if (save_fingerprint(fp_path, new_fp, new_len)) {
	if (verbose) std::cout << "[watch-quiet] Saved fingerprint to " << fp_path << " (" << new_len << " bytes)" << std::endl;
      } else {
	if (verbose) {
	  std::cout << "[watch-quiet] WARNING: could not write fingerprint file: " << fp_path << std::endl;
	}
      }
    } else {
      if (verbose) {
	std::cout << "[watch-quiet] Fingerprint not available/unchanged." << std::endl;
      }
    }
  }

  const bool isRemoteTransfer = true;

  // Index existing
  std::unordered_set<SDK::CrContentHandle> seen;
  if (!isRemoteTransfer) {
    SDK::CrMtpFolderInfo* folders = nullptr;
    CrInt32u numFolders = 0;
    if (SDK::GetDateFolderList(handle, &folders, &numFolders) == SDK::CrError_None && folders) {
      for (CrInt32u i = 0; i < numFolders; ++i) {
	SDK::CrContentHandle* contents = nullptr;
	CrInt32u numContents = 0;
	if (SDK::GetContentsHandleList(handle, folders[i].handle, &contents, &numContents) == SDK::CrError_None && contents) {
	  for (CrInt32u j = 0; j < numContents; ++j) seen.insert(contents[j]);
	  SDK::ReleaseContentsHandleList(handle, contents);
	}
      }
      SDK::ReleaseDateFolderList(handle, folders);
    }
  }

  // Event-driven wait with periodic wake (so signals end the loop promptly)
  while (!g_stop.load(std::memory_order_relaxed)) {
    std::unique_lock<std::mutex> lk(callback.mtx);
    callback.cv.wait_for(lk, std::chrono::milliseconds(500), [&]{ return callback.contents_changed || g_stop.load(); });
    if (g_stop.load()) break;
    bool changed = callback.contents_changed;
    CrInt32u changed_slot = callback.last_contents_slot;
    callback.contents_changed = false;
    lk.unlock();

    if (changed) {
      std::cout << "[watch-quiet] Contents list changed: ";
      if (isRemoteTransfer) {
	std::cout << "downloading latest image..." << std::endl;
	// Retrieve the latest image from camera
	SDK::CrSlotNumber slot = static_cast<SDK::CrSlotNumber>(changed_slot);
	if (slot != SDK::CrSlotNumber_Slot1 && slot != SDK::CrSlotNumber_Slot2) {
	  slot = SDK::CrSlotNumber_Slot1;
	}
	CrInt32u getCode = (slot == SDK::CrSlotNumber_Slot2 ? SDK::CrDevicePropertyCode::CrDeviceProperty_MediaSLOT2_ContentsInfoListUpdateTime
			    : SDK::CrDevicePropertyCode::CrDeviceProperty_MediaSLOT1_ContentsInfoListUpdateTime);
	SDK::CrDeviceProperty* propList = nullptr;
	CrInt32 nprop = 0; // must be CrInt32* for API
	SDK::CrError res = SDK::GetSelectDeviceProperties(handle, 1, &getCode, &propList, &nprop);
	if (res != SDK::CrError_None || nprop == 0) {
	  std::cerr << "Failed to get ContentsInfoListUpdateTime property (err=" << "0x" << std::hex << res << std::dec << ")" << std::endl;
	  if (propList) SDK::ReleaseDeviceProperties(handle, propList);
	} else {
	  CrInt64u updateTime = propList[0].GetCurrentValue();
	  SDK::ReleaseDeviceProperties(handle, propList);
	  SDK::CrCaptureDate updateDate(updateTime);
	  SDK::CrCaptureDate dummyDate{};
	  SDK::CrContentsInfo* list = nullptr;
	  CrInt32u count = 0;
	  SDK::CrError resList = SDK::GetRemoteTransferContentsInfoList(handle, slot, SDK::CrGetContentsInfoListType_All, &dummyDate, 0, &list, &count);
	  if (resList != SDK::CrError_None || count == 0) {
	    std::cerr << "Failed to get contents info list (err=" << "0x" << std::hex << resList << std::dec << ")" << std::endl;
	    if (list) SDK::ReleaseRemoteTransferContentsInfoList(handle, list);
	  } else {
	    SDK::CrContentsInfo targetContents{};
	    bool found = false;
	    for (CrInt32u i = 0; i < count; ++i) {
	      if (list[i].modificationDatetimeUTC == updateDate) {
		targetContents = list[i];
		found = true;
		break;
	      }
	    }
	    if (!found || targetContents.contentId == 0) {
	      std::cerr << "New content not found in list." << std::endl;
	    } else {
	      CrInt32u divisionSize = 0x5000000;
#if defined(__linux__)
	      divisionSize = 0x1000000;
#endif
	      for (CrInt32u fi = 0; fi < targetContents.filesNum; ++fi) {
		callback.dl_waiting = true;
		// If a download directory is provided, pass it to the SDK
		CrChar* path = nullptr;
		if (!download_dir.empty()) {
		  path = const_cast<CrChar*>(reinterpret_cast<const CrChar*>(download_dir.c_str()));
		}
		SDK::CrError dres = SDK::GetRemoteTransferContentsDataFile(handle, slot, targetContents.contentId, targetContents.files[fi].fileId,
									   divisionSize, path, nullptr);
		if (dres != SDK::CrError_None) {
		  std::cerr << "GetRemoteTransferContentsDataFile failed (err=" << "0x" << std::hex << dres << std::dec << ")" << std::endl;
		  callback.dl_waiting = false;
		  continue;
		}
		{
		  std::unique_lock<std::mutex> dlLock(callback.dl_mtx);
		  callback.dl_cv.wait(dlLock, [&]{ return !callback.dl_waiting; });
		}
		if (callback.dl_notify_code == SDK::CrNotify_RemoteTransfer_Result_OK) {
		  std::cout << "[SAVED] " << callback.last_downloaded_file << std::endl;
		} else {
		  std::cerr << "Download failed (notify=" << "0x" << std::hex << callback.dl_notify_code << std::dec << ")" << std::endl;
		}
	      }
	    }
	    SDK::ReleaseRemoteTransferContentsInfoList(handle, list);
	  }
	}
      } else {
	std::cout << "rescanning..." << std::endl;
	scan_and_report_new(handle, seen);
      }
    }
  }

  std::cout << "[watch-quiet] Stop requested. Disconnecting..." << std::endl;
  cleanup();
  return 0;
}
