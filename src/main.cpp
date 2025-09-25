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

// ----------------------------
// Signals
// ----------------------------
static void signal_handler(int) { g_stop.store(true, std::memory_order_relaxed); }

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

// ----------------------------
// FS helpers
// ----------------------------
static bool file_exists(const std::string &p) {
    struct stat st{}; return ::stat(p.c_str(), &st) == 0;
}

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
    if (!file_exists(target)) return base;
    std::string name = base, ext;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && dot + 1 < base.size()) {
        name = base.substr(0, dot);
        ext = base.substr(dot);
    }
    for (int i = 1; i < 1000000; ++i) {
        std::ostringstream o; o << name << "_" << i << ext;
        if (!file_exists(join_path(dir, o.str()))) return o.str();
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
        if (verbose) std::cout << "[CB] OnConnected v=" << v << std::endl;
        {
            std::lock_guard<std::mutex> lk(mtx); connected = true; conn_finished = true;
        }
        conn_cv.notify_all();
    }

    void OnDisconnected(CrInt32u error) override {
        if (g_shutting_down.load()) return;
        std::cout << "[CB] OnDisconnected: 0x" << std::hex << error << std::dec
                  << " (" << crsdk_err::error_to_name(error) << ")" << std::endl;
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
            std::cout << "[CB] OnWarning: " << crsdk_err::warning_to_name(w)
                      << " (0x" << std::hex << w << std::dec << ")" << std::endl;
        }
    }

    void OnError(CrInt32u e) override {
        if (g_shutting_down.load()) return;
        std::cout << "[CB] OnError: " << crsdk_err::error_to_name(e)
                  << " (0x" << std::hex << e << std::dec << ")" << std::endl;
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
                    std::cout << tag << ": " << name << " (0x" << std::hex << code << std::dec
                              << ") -> " << (long long)val << std::endl;
                }
            }
        }
        SDK::ReleaseDeviceProperties(device_handle, props);
    }

public:
    void OnPropertyChanged() override { if (!g_shutting_down.load()) log_changed_properties_("[CB] OnPropertyChanged"); }
    void OnLvPropertyChanged() override { if (!g_shutting_down.load()) log_changed_properties_("[CB] OnLvPropertyChanged"); }

    void OnNotifyRemoteTransferContentsListChanged(CrInt32u notify, CrInt32u slotNumber, CrInt32u addSize) override {
        if (g_shutting_down.load()) return;
        std::cout << "[CB] ContentsListChanged: notify=0x" << std::hex << notify << std::dec
                  << " slot=" << slotNumber << " add=" << addSize << std::endl;
        if (notify != SDK::CrNotify_RemoteTransfer_Changed_Add) return;

        try {
            g_downloadThreads.emplace_back([this, slotNumber, addSize]() {
                SDK::CrDeviceHandle handle = this->device_handle;
                if (!handle) return;
                SDK::CrSlotNumber slot = (slotNumber == SDK::CrSlotNumber_Slot2) ? SDK::CrSlotNumber_Slot2 : SDK::CrSlotNumber_Slot1;

                // Robust fetch: date list -> latest day -> contents for that day
                SDK::CrContentsInfo *list = nullptr; CrInt32u count = 0; SDK::CrError resList = SDK::CrError_None;
                const int max_attempts = 10;
                for (int attempt = 1; attempt <= max_attempts; ++attempt) {
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
                if (resList != SDK::CrError_None || !list || count == 0) {
                    std::cerr << "[ERROR] Failed to get remote contents info list (err=0x" << std::hex << resList << std::dec << ")" << std::endl;
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
                    const SDK::CrContentsInfo &target = list[idx[k]];
                    if (target.contentId == 0) continue;

                    for (CrInt32u fi = 0; fi < target.filesNum; ++fi) {
                        dl_waiting = true;
                        CrInt32u fileId = target.files[fi].fileId;

                        // determine original filename
                        std::string orig = basename_from_path(target.files[fi].filePath);
                        if (orig.empty()) {
                            std::ostringstream o; o << "content_" << (unsigned long long)target.contentId << "_file_" << fileId;
                            orig = o.str();
                        }
                        std::string finalName = unique_name(g_download_dir, orig);
                        CrChar *saveDir = g_download_dir.empty() ? nullptr : const_cast<CrChar *>(reinterpret_cast<const CrChar *>(g_download_dir.c_str()));
                        SDK::CrError dres = SDK::GetRemoteTransferContentsDataFile(handle, slot, target.contentId, fileId, 0x1000000, saveDir, const_cast<CrChar *>(finalName.c_str()));
                        if (dres != SDK::CrError_None) {
                            std::cerr << "[ERROR] GetRemoteTransferContentsDataFile failed (0x" << std::hex << dres << std::dec << ")" << std::endl;
                            dl_waiting = false;
                            continue;
                        }
                        {
                            std::unique_lock<std::mutex> lk(dl_mtx);
                            dl_cv.wait(lk, [&] { return !dl_waiting || g_stop.load(); });
                        }
                        if (dl_notify_code == SDK::CrNotify_RemoteTransfer_Result_OK) {
                            std::string saved = last_downloaded_file;
                            std::string base = saved;
                            size_t pos = base.find_last_of("/\\"); if (pos != std::string::npos) base = base.substr(pos + 1);
                            long long sizeB = 0; struct stat st{}; if (::stat(saved.c_str(), &st) == 0) sizeB = (long long)st.st_size;
                            std::cout << "[PHOTO] " << base << " (" << sizeB << " bytes)" << std::endl;

                            // Run post-download hook if provided
                            if (!g_post_cmd.empty()) {
                                run_post_cmd(g_post_cmd, saved);
                            }
                        } else if (!g_stop.load()) {
                            std::cerr << "[ERROR] Download failed (notify=0x" << std::hex << dl_notify_code << std::dec << ")" << std::endl;
                        }
                    }
                }

                SDK::ReleaseRemoteTransferContentsInfoList(handle, list);
            });
        } catch (...) {
            std::cerr << "[ERROR] Failed to create download thread" << std::endl;
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
        std::cout << "Searching for cameras..." << std::endl;
        err = SDK::EnumCameraObjects(&enum_list, 1);
        if (err != SDK::CrError_None || !enum_list || enum_list->GetCount() == 0) {
            std::cerr << "No cameras found (EnumCameraObjects)" << std::endl;
            return false;
        }
        selected = enum_list->GetCameraObjectInfo(0);
    } else {
        std::cout << "Connecting with camera at " << explicit_ip << "..." << std::endl;
        CrInt32u ipAddr = ip_to_uint32(explicit_ip);
        if (!ipAddr) {
            std::cerr << "Bad IP" << std::endl; return false;
        }
        unsigned char mac[6] = {0, 0, 0, 0, 0, 0};
        if (!explicit_mac.empty() && !parse_mac(explicit_mac, mac)) {
            std::cerr << "WARN bad MAC, ignoring: " << explicit_mac << std::endl;
        }
        SDK::CrCameraDeviceModelList model = SDK::CrCameraDeviceModel_ILCE_6700;
        err = SDK::CreateCameraObjectInfoEthernetConnection(&created, model, ipAddr, mac, 0);
        if (err != SDK::CrError_None || !created) {
            std::cerr << "CreateCameraObjectInfoEthernetConnection failed" << std::endl;
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
        std::cerr << "Connect failed: 0x" << std::hex << err << std::dec << std::endl;
        return false;
    }

    // wait handshake
    {
        std::unique_lock<std::mutex> lk(cb.mtx);
        cb.conn_cv.wait_for(lk, std::chrono::seconds(12), [&] { return cb.connected || cb.conn_finished || g_stop.load(); });
    }
    if (!cb.connected) {
        std::cerr << "Camera not available";
        if (cb.last_error_code) std::cerr << " error=0x" << std::hex << cb.last_error_code << std::dec;
        std::cerr << std::endl;
        return false;
    }

    cb.device_handle = handle;
    std::cout << "Connected. Ctrl+C to stop." << std::endl;

    // Start a background thread for interactive shutter trigger
    std::thread shutterThread([&]() {
        std::string line;
        // Loop until program stop is requested
        while (!g_stop.load(std::memory_order_relaxed)) {
            // Wait for user to press Enter (std::getline blocks until a newline)
            if (!std::getline(std::cin, line)) {
                // If input stream closed or error, exit the thread loop
                break;
            }
            // On any Enter key press (ignore content of input line), send shutter command
            SDK::SendCommand(handle, SDK::CrCommandId_Release, SDK::CrCommandParam_Down);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            SDK::SendCommand(handle, SDK::CrCommandId_Release, SDK::CrCommandParam_Up);
            std::cout << "Shutter triggered." << std::endl;
        }
    });
    // Detach the thread so it runs independently
    shutterThread.detach();

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

int main(int argc, char **argv) {
    install_signal_handlers();

    std::string explicit_ip, explicit_mac, download_dir;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--ip" && i + 1 < argc) explicit_ip = argv[++i];
        else if (a == "--mac" && i + 1 < argc) explicit_mac = argv[++i];
        else if (a == "--dir" && i + 1 < argc) download_dir = argv[++i];
        else if (a == "--verbose" || a == "-v") verbose = true;
        else if (a == "--cmd" && i + 1 < argc) g_post_cmd = argv[++i];
        else if (a == "--keepalive" && i + 1 < argc) {
            long long ms = std::atoll(argv[++i]);
            if (ms < 0) ms = 0; g_keepalive = std::chrono::milliseconds(ms);
        }
    }

    if (!SDK::Init()) { std::cerr << "Init failed" << std::endl; return 1; }
    g_download_dir = download_dir;

    auto cleanup_sdk = []() {
        g_shutting_down.store(true);
        SDK::Release();
    };

    QuietCallback cb;

    // Main connect loop (keepalive-aware)
    while (!g_stop.load()) {
        SDK::CrDeviceHandle handle = 0; const SDK::ICrCameraObjectInfo *selected = nullptr;
        SDK::ICrEnumCameraObjectInfo *enum_list = nullptr; SDK::ICrCameraObjectInfo *created = nullptr;
        cb.connected = false; cb.conn_finished = false; cb.last_error_code = 0; g_reconnect.store(false);

        bool ok = try_connect_once(explicit_ip, explicit_mac, download_dir, verbose, cb,
                                   handle, selected, enum_list, created);
        if (!ok) {
            disconnect_and_release(handle, created, enum_list);
            if (g_keepalive.count() == 0) {
                std::cerr << "Exiting (no keepalive)" << std::endl;
                cleanup_sdk();
                return 2;
            }
            std::cout << "Retrying in " << g_keepalive.count() << " ms..." << std::endl;
            std::this_thread::sleep_for(g_keepalive);
            continue;
        }

        // Connected: wait until stop or disconnect signaled
        while (!g_stop.load() && !g_reconnect.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "Shutting down connection..." << std::endl;
        disconnect_and_release(handle, created, enum_list);

        for (auto &t : g_downloadThreads) { if (t.joinable()) t.join(); }
        g_downloadThreads.clear();

        if (g_stop.load()) break; // Ctrl+C or external stop

        // If we should reconnect but keepalive is 0, we exit
        if (g_keepalive.count() == 0) {
            std::cerr << "Disconnected and keepalive disabled; exiting." << std::endl;
            break;
        }

        std::cout << "Disconnected; will retry in " << g_keepalive.count() << " ms..." << std::endl;
        std::this_thread::sleep_for(g_keepalive);
    }

    std::cout << "Shutting down..." << std::endl;
    for (auto &t : g_downloadThreads) { if (t.joinable()) t.join(); }
    cleanup_sdk();
    return 0;
}
