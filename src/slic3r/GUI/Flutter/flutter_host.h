#pragma once
#include <string>
#include <functional>
#include <memory>

// =========================================================================
// Two-layer architecture:
//   FlutterEngineHost — one per process, owns the FlutterEngine lifecycle
//   FlutterViewHost   — one per panel, owns a ViewController + MethodChannel
// =========================================================================

// ── View layer (one per visible panel) ─────────────────────────────────

class FlutterViewHost {
public:
    using Reply = std::function<void(const std::string& result)>;
    using MethodCallHandler = std::function<void(
        const std::string& method,
        const std::string& arguments,
        Reply reply)>;

    virtual ~FlutterViewHost() = default;

    virtual void embedInto(void* parentView) = 0;

    virtual void resize(int width, int height) = 0;

    virtual void invokeMethod(const std::string& method,
                              const std::string& arguments) = 0;

    virtual void setMethodCallHandler(MethodCallHandler handler) = 0;

    virtual void focus() = 0;

#ifdef __WXMSW__
    virtual void* nativeHandle() const { return nullptr; }
#endif
};

// ── Engine layer (one per process, creates views) ──────────────────────

class FlutterEngineHost {
public:
    virtual ~FlutterEngineHost() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;

    virtual std::unique_ptr<FlutterViewHost> createView(
        const std::string& entrypoint,
        const std::string& channelName) = 0;
};

// Factory
std::unique_ptr<FlutterEngineHost> createFlutterEngine(
    const std::string& assetsPath,
    const std::string& icuDataPath);
