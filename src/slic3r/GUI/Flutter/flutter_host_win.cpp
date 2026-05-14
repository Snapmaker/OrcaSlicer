// Windows implementation: FlutterEngineHost + FlutterViewHost
// Uses Flutter Windows desktop embedding C API (flutter_windows.h)
// Manually implements StandardMethodCodec encode/decode to match Dart's MethodChannel.
#include <windows.h>
#include <flutter_windows.h>
#include "flutter_host.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cerrno>

// ── StandardMethodCodec helpers ────────────────────────────────────────

// Encodes an unsigned integer as a 7-bit varint.
static void writeVarint(std::vector<uint8_t>& buf, uint32_t value) {
    while (value >= 0x80) {
        buf.push_back((value & 0x7F) | 0x80);
        value >>= 7;
    }
    buf.push_back(value & 0x7F);
}

// Reads a 7-bit varint from data; returns {value, bytes_consumed}.
static std::pair<uint32_t, size_t> readVarint(const uint8_t* data, size_t size) {
    uint32_t value = 0;
    size_t shift = 0;
    size_t pos = 0;
    while (pos < size) {
        uint8_t byte = data[pos++];
        value |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
        if (shift >= 32) return {0, pos};
    }
    return {value, pos};
}

// Encodes a string as: [0x07, varint(len), bytes].
static void writeString(std::vector<uint8_t>& buf, const std::string& s) {
    buf.push_back(0x07);
    writeVarint(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

// Decodes a string value from data at offset. On success sets *out and returns
// bytes consumed (including type byte). On failure returns 0.
static size_t readString(const uint8_t* data, size_t size, std::string* out) {
    if (size < 2 || data[0] != 0x07) return 0;
    auto [len, vpos] = readVarint(data + 1, size - 1);
    size_t start = 1 + vpos;
    if (start + len > size) return 0;
    out->assign(reinterpret_cast<const char*>(data + start), len);
    return start + len;
}

// Writes a StandardMessageCodec value, auto-detecting int vs string so
// that Dart's typed invokeMethod<T> receives the correct wire type.
static void writeValue(std::vector<uint8_t>& buf, const std::string& s) {
    if (!s.empty()) {
        errno = 0;
        char* end = nullptr;
        long long val = std::strtoll(s.c_str(), &end, 10);
        if (end && *end == '\0' && errno != ERANGE) {
            if (val >= INT32_MIN && val <= INT32_MAX) {
                buf.push_back(0x03); // int32
                int32_t v32 = static_cast<int32_t>(val);
                uint32_t bits;
                std::memcpy(&bits, &v32, sizeof(bits));
                buf.push_back(bits & 0xFF);
                buf.push_back((bits >> 8) & 0xFF);
                buf.push_back((bits >> 16) & 0xFF);
                buf.push_back((bits >> 24) & 0xFF);
                return;
            }
            buf.push_back(0x04); // int64
            int64_t v64 = val;
            uint64_t bits;
            std::memcpy(&bits, &v64, sizeof(bits));
            for (int i = 0; i < 8; ++i)
                buf.push_back((bits >> (i * 8)) & 0xFF);
            return;
        }
        {
            errno = 0;
            char* fend = nullptr;
            double dval = std::strtod(s.c_str(), &fend);
            if (fend && *fend == '\0' && errno != ERANGE) {
                buf.push_back(0x06); // float64
                uint64_t bits;
                std::memcpy(&bits, &dval, sizeof(bits));
                for (int i = 0; i < 8; ++i)
                    buf.push_back((bits >> (i * 8)) & 0xFF);
                return;
            }
        }
    }
    // fall through: encode as string
    buf.push_back(0x07);
    writeVarint(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

// Builds a success envelope: [0x00, encoded_result].
static std::vector<uint8_t> encodeSuccess(const std::string& result) {
    std::vector<uint8_t> buf;
    buf.push_back(0x00); // success
    writeValue(buf, result);
    return buf;
}

// Builds an error envelope: [0x01, error_code, error_message, null].
static std::vector<uint8_t> encodeError(const std::string& code,
                                         const std::string& message) {
    std::vector<uint8_t> buf;
    buf.push_back(0x01); // error
    writeString(buf, code);
    writeString(buf, message);
    buf.push_back(0x00); // null details
    return buf;
}

// Builds a method-call message: [method_value, args_value].
static std::vector<uint8_t> encodeMethodCall(const std::string& method,
                                             const std::string& args) {
    std::vector<uint8_t> buf;
    writeValue(buf, method);
    writeValue(buf, args);
    return buf;
}

// Decodes one StandardMessageCodec value, converts to string. Returns bytes consumed.
static size_t decodeValue(const uint8_t* data, size_t size, std::string* out) {
    if (size < 1) return 0;
    switch (data[0]) {
        case 0x00: // null
            *out = "";
            return 1;
        case 0x01: // true
            *out = "true";
            return 1;
        case 0x02: // false
            *out = "false";
            return 1;
        case 0x03: { // int32 (4 bytes)
            if (size < 5) return 0;
            uint32_t bits = data[1] | (static_cast<uint32_t>(data[2]) << 8)
                          | (static_cast<uint32_t>(data[3]) << 16)
                          | (static_cast<uint32_t>(data[4]) << 24);
            int32_t val;
            std::memcpy(&val, &bits, sizeof(val));
            *out = std::to_string(val);
            return 5;
        }
        case 0x04: { // int64 (8 bytes)
            if (size < 9) return 0;
            uint64_t bits = 0;
            for (int i = 0; i < 8; ++i)
                bits |= static_cast<uint64_t>(data[1 + i]) << (i * 8);
            int64_t val;
            std::memcpy(&val, &bits, sizeof(val));
            *out = std::to_string(val);
            return 9;
        }
        case 0x06: { // float64 (8 bytes, IEEE 754)
            if (size < 9) return 0;
            uint64_t bits = 0;
            for (int i = 0; i < 8; ++i)
                bits |= static_cast<uint64_t>(data[1 + i]) << (i * 8);
            double dbl;
            std::memcpy(&dbl, &bits, sizeof(dbl));
            *out = std::to_string(dbl);
            return 9;
        }
        case 0x07: // string
            return readString(data, size, out);
        default:
            return 0;
    }
}

// Decodes a method-call message. Returns {method_name, args}.
static std::pair<std::string, std::string>
    decodeMethodCall(const uint8_t* data, size_t size) {
    if (!data || size < 1) return {"", ""};
    size_t off = 0;
    std::string method, args;
    size_t n = decodeValue(data + off, size - off, &method);
    if (!n) return {"", ""};
    off += n;
    if (!decodeValue(data + off, size - off, &args)) return {"", ""};
    return {method, args};
}

// ── FlutterViewHostWin ─────────────────────────────────────────────────

class FlutterViewHostWin : public FlutterViewHost {
    FlutterDesktopViewControllerRef m_controller = nullptr;
    FlutterDesktopEngineRef        m_engine = nullptr;
    FlutterDesktopMessengerRef     m_messenger = nullptr;
    std::string                    m_channel;
    MethodCallHandler              m_handler;

    static void messageCallback(FlutterDesktopMessengerRef messenger,
                                 const FlutterDesktopMessage* msg,
                                 void* userData) {
        auto* self = static_cast<FlutterViewHostWin*>(userData);
        if (!msg) return;

        auto [method, args] = decodeMethodCall(msg->message, msg->message_size);

        if (!self->m_handler) {
            // No handler yet — respond "not implemented" (empty bytes)
            if (messenger && msg->response_handle) {
                FlutterDesktopMessengerSendResponse(
                    messenger, msg->response_handle, nullptr, 0);
            }
            return;
        }

        auto replied = std::make_shared<bool>(false);

        Reply reply = [messenger, response_handle = msg->response_handle, replied]
                      (const std::string& result) {
            if (*replied) return;
            *replied = true;
            if (messenger && response_handle) {
                auto encoded = encodeSuccess(result);
                FlutterDesktopMessengerSendResponse(
                    messenger, response_handle, encoded.data(), encoded.size());
            }
        };

        try {
            self->m_handler(method, args, reply);
        } catch (const std::exception& e) {
            if (!*replied) {
                *replied = true;
                if (messenger && msg->response_handle) {
                    auto encoded = encodeError("EXCEPTION", e.what());
                    FlutterDesktopMessengerSendResponse(
                        messenger, msg->response_handle, encoded.data(), encoded.size());
                }
            }
        } catch (...) {
            if (!*replied) {
                *replied = true;
                if (messenger && msg->response_handle) {
                    auto encoded = encodeError("EXCEPTION", "unknown error");
                    FlutterDesktopMessengerSendResponse(
                        messenger, msg->response_handle, encoded.data(), encoded.size());
                }
            }
        }
        // If handler stored the reply for later but never called it and
        // never threw, respond so the Dart future doesn't hang.
        if (!*replied) {
            *replied = true;
            if (messenger && msg->response_handle) {
                FlutterDesktopMessengerSendResponse(
                    messenger, msg->response_handle, nullptr, 0);
            }
        }
    }

public:
    FlutterViewHostWin(FlutterDesktopViewControllerRef ctrl,
                       FlutterDesktopEngineRef engine,
                       const std::string& channelName)
        : m_controller(ctrl), m_engine(engine), m_channel(channelName) {

        m_messenger = FlutterDesktopEngineGetMessenger(engine);
        if (m_messenger) {
            FlutterDesktopMessengerSetCallback(
                m_messenger, m_channel.c_str(), messageCallback, this);
        }
    }

    ~FlutterViewHostWin() override {
        if (m_messenger) {
            FlutterDesktopMessengerSetCallback(
                m_messenger, m_channel.c_str(), nullptr, nullptr);
        }
        if (m_controller) {
            FlutterDesktopViewControllerDestroy(m_controller);
        }
        // m_engine is owned by m_controller; destroying the controller
        // also destroys the engine on Windows.
    }

    void resize(int width, int height) override {
        if (!m_controller) return;
        FlutterDesktopViewRef view =
            FlutterDesktopViewControllerGetView(m_controller);
        HWND hwnd = FlutterDesktopViewGetHWND(view);
        if (!hwnd) return;
        if (width <= 0 || height <= 0) return;
        SetWindowPos(hwnd, nullptr,
                     0, 0, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void embedInto(void* parentView) override {
        FlutterDesktopViewRef view =
            FlutterDesktopViewControllerGetView(m_controller);
        HWND childHwnd = FlutterDesktopViewGetHWND(view);
        HWND parentHwnd = static_cast<HWND>(parentView);

        if (!childHwnd || !parentHwnd) return;

        // The Flutter engine creates the view as WS_CHILD already (see
        // FlutterWindow::InitializeChild in flutter_window.cc).  No style
        // manipulation needed — just change the parent.
        SetParent(childHwnd, parentHwnd);

        RECT rect;
        GetClientRect(parentHwnd, &rect);
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top;
        if (w <= 1) w = 800;
        if (h <= 1) h = 600;
        SetWindowPos(childHwnd, nullptr,
                     0, 0, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void invokeMethod(const std::string& method,
                      const std::string& arguments) override {
        if (!m_messenger) return;
        auto encoded = encodeMethodCall(method, arguments);
        FlutterDesktopMessengerSend(
            m_messenger, m_channel.c_str(),
            encoded.data(), encoded.size());
    }

    void focus() override {
        FlutterDesktopViewRef view =
            FlutterDesktopViewControllerGetView(m_controller);
        HWND hwnd = FlutterDesktopViewGetHWND(view);
        if (hwnd) ::SetFocus(hwnd);
    }

#ifdef __WXMSW__
    void* nativeHandle() const override {
        FlutterDesktopViewRef view =
            FlutterDesktopViewControllerGetView(m_controller);
        return (void*)FlutterDesktopViewGetHWND(view);
    }
#endif

    void setMethodCallHandler(MethodCallHandler h) override {
        m_handler = std::move(h);
    }
};

// ── FlutterEngineHostWin ───────────────────────────────────────────────

class FlutterEngineHostWin : public FlutterEngineHost {
    bool m_started = false;

public:
    ~FlutterEngineHostWin() override { stop(); }

    bool start() override {
        m_started = true;
        return true;
    }

    void stop() override {
        m_started = false;
    }

    std::unique_ptr<FlutterViewHost> createView(
        const std::string& entrypoint,
        const std::string& channelName) override {

        // Resolve paths relative to the executable, not the CWD.
        wchar_t exe_path[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        std::wstring exe_dir;
        if (len > 0 && len < MAX_PATH) {
            std::wstring full(exe_path, len);
            auto pos = full.rfind(L'\\');
            if (pos != std::wstring::npos)
                exe_dir = full.substr(0, pos);
        }
        if (exe_dir.empty()) return nullptr;

        std::wstring assets = exe_dir + L"\\data\\flutter_assets";
        std::wstring icu = exe_dir + L"\\data\\icudtl.dat";
        std::wstring aot = exe_dir + L"\\flutter_app.dll";

        FlutterDesktopEngineProperties props = {};
        props.assets_path = assets.c_str();
        props.icu_data_path = icu.c_str();
        props.aot_library_path = aot.c_str();

        FlutterDesktopEngineRef engine =
            FlutterDesktopEngineCreate(&props);
        if (!engine) return nullptr;

        const char* ep = entrypoint.empty() ? nullptr : entrypoint.c_str();
        if (!FlutterDesktopEngineRun(engine, ep)) {
            FlutterDesktopEngineDestroy(engine);
            return nullptr;
        }

        FlutterDesktopViewControllerRef controller =
            FlutterDesktopViewControllerCreate(800, 600, engine);
        if (!controller) {
            FlutterDesktopEngineDestroy(engine);
            return nullptr;
        }

        auto* view = new FlutterViewHostWin(controller, engine, channelName);
        return std::unique_ptr<FlutterViewHost>(view);
    }
};

// ── Factory ────────────────────────────────────────────────────────────

std::unique_ptr<FlutterEngineHost> createFlutterEngine(
    const std::string&, const std::string&) {
    return std::make_unique<FlutterEngineHostWin>();
}
