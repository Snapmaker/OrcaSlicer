// macOS implementation: FlutterEngineHost + FlutterViewHost
#import <Cocoa/Cocoa.h>
#import <FlutterMacOS/FlutterMacOS.h>
#include "flutter_host.h"
#include <string>
#include <cstdlib>
#include <vector>

// Shared helpers
static id decodeArg(const std::string& s) {
    if (s.empty()) return nil;
    char* end = nullptr;
    long val = strtol(s.c_str(), &end, 10);
    if (end && *end == '\0') return @(val);
    return [NSString stringWithUTF8String:s.c_str()];
}

static std::string encodeArg(id arg) {
    if (!arg) return "";
    if ([arg isKindOfClass:[NSString class]])
        return [arg UTF8String];
    return [[arg description] UTF8String];
}

// ── FlutterViewHostMacOS ──────────────────────────────────────────────

class FlutterViewHostMacOS : public FlutterViewHost {
    FlutterViewController* controller = nil;
    FlutterMethodChannel* channel = nil;
    MethodCallHandler handler;

public:
    FlutterViewHostMacOS(FlutterEngine* engine, const std::string& channelName) {
        controller = [[FlutterViewController alloc] initWithEngine:engine
                                                            nibName:nil bundle:nil];
        controller.mouseTrackingMode = kFlutterMouseTrackingModeInActiveApp;
        controller.view.wantsLayer = YES;
        engine.viewController = controller;
        channel = [FlutterMethodChannel
            methodChannelWithName:[NSString stringWithUTF8String:channelName.c_str()]
                 binaryMessenger:engine.binaryMessenger];
    }

    ~FlutterViewHostMacOS() override {
        // controller and channel are owned by engine, don't release here
    }

    void embedInto(void* parentView) override {
        NSView* fv = controller.view;
        NSView* cv = (__bridge NSView*)parentView;
        if (!fv || !cv) return;
        fv.frame = cv.bounds;
        fv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [cv addSubview:fv];
    }

    void invokeMethod(const std::string& method,
                      const std::string& arguments) override {
        if (!channel) return;
        [channel invokeMethod:[NSString stringWithUTF8String:method.c_str()]
                    arguments:decodeArg(arguments)];
    }

    void setMethodCallHandler(MethodCallHandler h) override {
        handler = h;
        [channel setMethodCallHandler:^(
            FlutterMethodCall* call, FlutterResult fResult) {
            if (handler) {
                std::string args = encodeArg(call.arguments);
                FlutterViewHost::Reply reply = [fResult](const std::string& out) {
                    fResult(decodeArg(out));
                };
                handler([call.method UTF8String], args, reply);
            } else {
                fResult(FlutterMethodNotImplemented);
            }
        }];
    }
};

// ── FlutterEngineHostMacOS ────────────────────────────────────────────

class FlutterEngineHostMacOS : public FlutterEngineHost {
    FlutterDartProject* project = nil;
    std::vector<FlutterEngine*> engines;

public:
    ~FlutterEngineHostMacOS() override { stop(); }

    bool start() override {
        project = [[FlutterDartProject alloc] initWithPrecompiledDartBundle:nil];
        return true;
    }

    void stop() override {
        for (auto* e : engines) [e shutDownEngine];
        engines.clear();
        project = nil;
    }

    std::unique_ptr<FlutterViewHost> createView(
        const std::string& entrypoint,
        const std::string& channelName) override {

        FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"embed"
                                                             project:project
                                              allowHeadlessExecution:YES];

        NSString* nsEntry = nil;
        if (!entrypoint.empty() && entrypoint != "main")
            nsEntry = [NSString stringWithUTF8String:entrypoint.c_str()];

        if (![engine runWithEntrypoint:nsEntry]) {
            return nullptr;
        }

        auto* view = new FlutterViewHostMacOS(engine, channelName);
        engines.push_back(engine);
        return std::unique_ptr<FlutterViewHost>(view);
    }
};

std::unique_ptr<FlutterEngineHost> createFlutterEngine(
    const std::string&, const std::string&) {
    return std::make_unique<FlutterEngineHostMacOS>();
}
