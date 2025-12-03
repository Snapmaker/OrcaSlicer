// Why?
#define _WIN32_WINNT 0x0502
// The standard Windows includes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include <wchar.h>
#include <shlobj.h>
#include "sentry.h"

#ifdef SLIC3R_GUI
extern "C" {
// Let the NVIDIA and AMD know we want to use their graphics card
// on a dual graphics card system.
__declspec(dllexport) DWORD NvOptimusEnablement                  = 0x00000000;
__declspec(dllexport) int   AmdPowerXpressRequestHighPerformance = 0;
}
#endif /* SLIC3R_GUI */

#include <stdlib.h>
#include <stdio.h>

#ifdef SLIC3R_GUI
#include <GL/GL.h>
#endif /* SLIC3R_GUI */

#include <string>
#include <vector>

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <stdio.h>

static sentry_value_t on_crash_callback(const sentry_ucontext_t* uctx, sentry_value_t event, void* closure)
{
    (void) uctx;
    (void) closure;

    // tell the backend to retain the event
    return event;
}

#ifdef SLIC3R_GUI
class OpenGLVersionCheck
{
public:
    std::string version;
    std::string glsl_version;
    std::string vendor;
    std::string renderer;

    HINSTANCE hOpenGL = nullptr;
    bool      success = false;

    bool load_opengl_dll()
    {
        MSG      msg     = {0};
        WNDCLASS wc      = {0};
        wc.lpfnWndProc   = OpenGLVersionCheck::supports_opengl2_wndproc;
        wc.hInstance     = (HINSTANCE) GetModuleHandle(nullptr);
        wc.hbrBackground = (HBRUSH) (COLOR_BACKGROUND);
        wc.lpszClassName = L"Snapmaker_Orca_opengl_version_check";
        wc.style         = CS_OWNDC;
        if (RegisterClass(&wc)) {
            HWND hwnd = CreateWindowW(wc.lpszClassName, L"Snapmaker_Orca_opengl_version_check", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, 0, 0,
                                      wc.hInstance, (LPVOID) this);
            if (hwnd) {
                message_pump_exit = false;
                while (GetMessage(&msg, NULL, 0, 0) > 0 && !message_pump_exit)
                    DispatchMessage(&msg);
            }
        }
        return this->success;
    }

    void unload_opengl_dll()
    {
        if (this->hOpenGL) {
            BOOL released = FreeLibrary(this->hOpenGL);
            if (released)
                printf("System OpenGL library released\n");
            else
                printf("System OpenGL library NOT released\n");
            this->hOpenGL = nullptr;
        }
    }

    bool is_version_greater_or_equal_to(unsigned int major, unsigned int minor) const
    {
        // printf("is_version_greater_or_equal_to, version: %s\n", version.c_str());
        std::vector<std::string> tokens;
        boost::split(tokens, version, boost::is_any_of(" "), boost::token_compress_on);
        if (tokens.empty())
            return false;

        std::vector<std::string> numbers;
        boost::split(numbers, tokens[0], boost::is_any_of("."), boost::token_compress_on);

        unsigned int gl_major = 0;
        unsigned int gl_minor = 0;
        if (numbers.size() > 0)
            gl_major = ::atoi(numbers[0].c_str());
        if (numbers.size() > 1)
            gl_minor = ::atoi(numbers[1].c_str());
        // printf("Major: %d, minor: %d\n", gl_major, gl_minor);
        if (gl_major < major)
            return false;
        else if (gl_major > major)
            return true;
        else
            return gl_minor >= minor;
    }

protected:
    static bool message_pump_exit;

    void check(HWND hWnd)
    {
        hOpenGL = LoadLibraryExW(L"opengl32.dll", nullptr, 0);
        if (hOpenGL == nullptr) {
            printf("Failed loading the system opengl32.dll\n");
            return;
        }

        typedef HGLRC(WINAPI * Func_wglCreateContext)(HDC);
        typedef BOOL(WINAPI * Func_wglMakeCurrent)(HDC, HGLRC);
        typedef BOOL(WINAPI * Func_wglDeleteContext)(HGLRC);
        typedef GLubyte*(WINAPI * Func_glGetString)(GLenum);

        Func_wglCreateContext wglCreateContext = (Func_wglCreateContext) GetProcAddress(hOpenGL, "wglCreateContext");
        Func_wglMakeCurrent   wglMakeCurrent   = (Func_wglMakeCurrent) GetProcAddress(hOpenGL, "wglMakeCurrent");
        Func_wglDeleteContext wglDeleteContext = (Func_wglDeleteContext) GetProcAddress(hOpenGL, "wglDeleteContext");
        Func_glGetString      glGetString      = (Func_glGetString) GetProcAddress(hOpenGL, "glGetString");

        if (wglCreateContext == nullptr || wglMakeCurrent == nullptr || wglDeleteContext == nullptr || glGetString == nullptr) {
            printf("Failed loading the system opengl32.dll: The library is invalid.\n");
            return;
        }

        PIXELFORMATDESCRIPTOR pfd = {sizeof(PIXELFORMATDESCRIPTOR),
                                     1,
                                     PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                     PFD_TYPE_RGBA, // The kind of framebuffer. RGBA or palette.
                                     32,            // Color depth of the framebuffer.
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     24, // Number of bits for the depthbuffer
                                     8,  // Number of bits for the stencilbuffer
                                     0,  // Number of Aux buffers in the framebuffer.
                                     PFD_MAIN_PLANE,
                                     0,
                                     0,
                                     0,
                                     0};

        HDC ourWindowHandleToDeviceContext = ::GetDC(hWnd);
        // Gdi32.dll
        int letWindowsChooseThisPixelFormat = ::ChoosePixelFormat(ourWindowHandleToDeviceContext, &pfd);
        // Gdi32.dll
        SetPixelFormat(ourWindowHandleToDeviceContext, letWindowsChooseThisPixelFormat, &pfd);
        // Opengl32.dll
        HGLRC glcontext = wglCreateContext(ourWindowHandleToDeviceContext);
        wglMakeCurrent(ourWindowHandleToDeviceContext, glcontext);
        // Opengl32.dll
        const char* data = (const char*) glGetString(GL_VERSION);
        if (data != nullptr)
            this->version = data;
        // printf("check -version: %s\n", version.c_str());
        data = (const char*) glGetString(0x8B8C); // GL_SHADING_LANGUAGE_VERSION
        if (data != nullptr)
            this->glsl_version = data;
        data = (const char*) glGetString(GL_VENDOR);
        if (data != nullptr)
            this->vendor = data;
        data = (const char*) glGetString(GL_RENDERER);
        if (data != nullptr)
            this->renderer = data;
        // Opengl32.dll
        wglDeleteContext(glcontext);
        ::ReleaseDC(hWnd, ourWindowHandleToDeviceContext);
        this->success = true;
    }

    static LRESULT CALLBACK supports_opengl2_wndproc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message) {
        case WM_CREATE: {
            CREATESTRUCT*       pCreate  = reinterpret_cast<CREATESTRUCT*>(lParam);
            OpenGLVersionCheck* ogl_data = reinterpret_cast<OpenGLVersionCheck*>(pCreate->lpCreateParams);
            ogl_data->check(hWnd);
            DestroyWindow(hWnd);
            return 0;
        }
        case WM_NCDESTROY: message_pump_exit = true; return 0;
        default: return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
};

bool OpenGLVersionCheck::message_pump_exit = false;
#endif /* SLIC3R_GUI */

extern "C" {
typedef int(__stdcall* Slic3rMainFunc)(int argc, wchar_t** argv);
Slic3rMainFunc Snapmaker_Orca_main = nullptr;
}

void initSentry()
{
    sentry_options_t* options = sentry_options_new();
    {
#ifdef WIN32
        std::string dsn = std::string("https://c74b617c2aedc291444d3a238d23e780@o4508125599563776.ingest.us.sentry.io/4510425163956224");

        sentry_options_set_dsn(options, dsn.c_str());

        wchar_t exeDir[MAX_PATH];
        ::GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
        std::wstring wsExeDir(exeDir);
        int          nPos     = wsExeDir.find_last_of('\\');
        std::wstring wsDmpDir = wsExeDir.substr(0, nPos + 1);

        std::wstring handlerDir = wsDmpDir + L"crashpad_handler.exe";
        wsDmpDir += L"dump";

        auto wstringTostring = [](std::wstring wTmpStr) -> std::string {
            std::string resStr = std::string();
            int         len    = WideCharToMultiByte(CP_UTF8, 0, wTmpStr.c_str(), -1, nullptr, 0, nullptr, nullptr);

            if (len <= 0)
                return std::string();
            std::string desStr(len, 0);

            WideCharToMultiByte(CP_UTF8, 0, wTmpStr.c_str(), -1, &desStr[0], len, nullptr, nullptr);

            resStr = desStr;

            return resStr;
        };

        std::string desDir = wstringTostring(handlerDir);
        if (!desDir.empty())
            sentry_options_set_handler_path(options, desDir.c_str());
        desDir = wstringTostring(wsDmpDir);
        desDir                        = wstringTostring(wsDmpDir);
        wchar_t appDataPath[MAX_PATH] = {0};
        auto    hr                    = SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appDataPath);
        char*   path                  = new char[MAX_PATH];
        size_t  pathLength;
        wcstombs_s(&pathLength, path, MAX_PATH, appDataPath, MAX_PATH);
        std::string filePath = path;
        std::string appName  = "\\" + std::string("Snapmaker_Orca\\");
        filePath             = filePath + appName;

        if (!filePath.empty())
            sentry_options_set_database_path(options, filePath.c_str());
#endif
        std::string softVersion = "snapmaker_orca_2.2.0_beta2";
        // Snapmaker_VERSION
        sentry_options_set_release(options, softVersion.c_str());

#if defined(_DEBUG) || !defined(NDEBUG)
        sentry_options_set_debug(options, 1);
#else
        sentry_options_set_debug(options, 0);
#endif
        // release version environment(Testing/production/development/Staging)
        sentry_options_set_environment(options, "develop");
        sentry_options_set_auto_session_tracking(options, false);
        sentry_options_set_symbolize_stacktraces(options, true);
        sentry_options_set_on_crash(options, on_crash_callback, NULL);
        // Enable before_send hook for filtering sensitive data
        sentry_options_set_before_send(options, NULL, NULL);

        // Ensure all events and crashes are captured (sample rate 100%)
        sentry_options_set_sample_rate(options, 1.0);        // Capture 100% of events
        sentry_options_set_traces_sample_rate(options, 1.0); // Capture 100% of traces

        sentry_init(options);
        sentry_start_session();

        DWORD processID = GetCurrentProcessId();
        sentry_set_tag("PID", std::to_string(processID).c_str());

        auto pcName = boost::asio::ip::host_name();
        // auto macAddress = getMacAddress();

        sentry_set_tag("computer_name", pcName.c_str());
        // sentry_set_tag("mac_address", macAddress.c_str());
    }
}

extern "C" {
#ifdef SLIC3R_WRAPPER_NOCONSOLE
int APIENTRY wWinMain(HINSTANCE /* hInstance */, HINSTANCE /* hPrevInstance */, PWSTR /* lpCmdLine */, int /* nCmdShow */)
{
    int       argc;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
#else
int wmain(int argc, wchar_t** argv)
{
#endif
    // Allow the asserts to open message box, such message box allows to ignore the assert and continue with the application.
    // Without this call, the seemingly same message box is being opened by the abort() function, but that is too late and
    // the application will be killed even if "Ignore" button is pressed.
    _set_error_mode(_OUT_TO_MSGBOX);

#if defined(_DEBUG) || !defined(NDEBUG)
    SetUnhandledExceptionFilter((LPTOP_LEVEL_EXCEPTION_FILTER) ExceptionCrashHandler);
#else
    // Initialize Sentry for crash reporting in Release builds
    initSentry();
#endif

    std::vector<wchar_t*> argv_extended;
    argv_extended.emplace_back(argv[0]);

#ifdef SLIC3R_WRAPPER_GCODEVIEWER
    wchar_t gcodeviewer_param[] = L"--gcodeviewer";
    argv_extended.emplace_back(gcodeviewer_param);
#endif /* SLIC3R_WRAPPER_GCODEVIEWER */

#ifdef SLIC3R_GUI
    // Here one may push some additional parameters based on the wrapper type.
    bool force_mesa = false;
#endif /* SLIC3R_GUI */
    for (int i = 1; i < argc; ++i) {
#ifdef SLIC3R_GUI
        if (wcscmp(argv[i], L"--sw-renderer") == 0)
            force_mesa = true;
        else if (wcscmp(argv[i], L"--no-sw-renderer") == 0)
            force_mesa = false;
#endif /* SLIC3R_GUI */
        argv_extended.emplace_back(argv[i]);
    }
    argv_extended.emplace_back(nullptr);

#ifdef SLIC3R_GUI
    OpenGLVersionCheck opengl_version_check;
    bool               load_mesa =
        // Forced from the command line.
        force_mesa ||
        // Try to load the default OpenGL driver and test its context version.
        !opengl_version_check.load_opengl_dll() || !opengl_version_check.is_version_greater_or_equal_to(2, 0);
#endif /* SLIC3R_GUI */

    wchar_t path_to_exe[MAX_PATH + 1] = {0};
    ::GetModuleFileNameW(nullptr, path_to_exe, MAX_PATH);
    wchar_t drive[_MAX_DRIVE];
    wchar_t dir[_MAX_DIR];
    wchar_t fname[_MAX_FNAME];
    wchar_t ext[_MAX_EXT];
    _wsplitpath(path_to_exe, drive, dir, fname, ext);
    _wmakepath(path_to_exe, drive, dir, nullptr, nullptr);

#ifdef SLIC3R_GUI
    // https://wiki.qt.io/Cross_compiling_Mesa_for_Windows
    // http://download.qt.io/development_releases/prebuilt/llvmpipe/windows/
    if (load_mesa) {
        opengl_version_check.unload_opengl_dll();
        wchar_t path_to_mesa[MAX_PATH + 1] = {0};
        wcscpy(path_to_mesa, path_to_exe);
        wcscat(path_to_mesa, L"mesa\\opengl32.dll");
        printf("Loading MESA OpenGL library: %S\n", path_to_mesa);
        HINSTANCE hInstance_OpenGL = LoadLibraryExW(path_to_mesa, nullptr, 0);
        if (hInstance_OpenGL == nullptr) {
            printf("MESA OpenGL library was not loaded\n");
        } else
            printf("MESA OpenGL library was loaded sucessfully\n");
    }
#endif /* SLIC3R_GUI */

    wchar_t path_to_slic3r[MAX_PATH + 1] = {0};
    wcscpy(path_to_slic3r, path_to_exe);
    wcscat(path_to_slic3r, L"Snapmaker_Orca.dll");
    //	printf("Loading Slic3r library: %S\n", path_to_slic3r);
    HINSTANCE hInstance_Slic3r = LoadLibraryExW(path_to_slic3r, nullptr, 0);
    if (hInstance_Slic3r == nullptr) {
        printf("Snapmaker_Orca.dll was not loaded, error=%d\n", GetLastError());
        sentry_close();
        return -1;
    }

    // resolve function address here
    Snapmaker_Orca_main = (Slic3rMainFunc)
        GetProcAddress(hInstance_Slic3r,
#ifdef _WIN64
                       // there is just a single calling conversion, therefore no mangling of the function name.
                       "Snapmaker_Orca_main"
#else // stdcall calling convention declaration
                       "_bambustu_main@8"
#endif
        );
    if (Snapmaker_Orca_main == nullptr) {
        printf("could not locate the function Snapmaker_Orca_main in Snapmaker_Orca.dll\n");
        sentry_close();
        return -1;
    }

    // argc minus the trailing nullptr of the argv
    auto res = Snapmaker_Orca_main((int) argv_extended.size() - 1, argv_extended.data());
    sentry_close();
    return res;
}
}
