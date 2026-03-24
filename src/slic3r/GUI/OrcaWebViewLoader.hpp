#pragma once

#include <wx/string.h>
#include <wx/webview.h>

namespace Slic3r {
namespace GUI {

/**
 * 统一配置：本地 Web 加载参数
 */
struct OrcaWebLoadConfig {
    wxString root_path;        ///< 本地 Web 根目录
    wxString entry_url;        ///< 入口 URL：须为目录形式 orca://app/web/flutter_web/（勿用 index.html，见 LoadLocalHtml 注释）
    wxString user_assets_dir;   ///< 用户资源目录（temp/orca_user_assets）
    wxString route_path;       ///< 路由路径（如 /bridge）
    wxString route_params;     ///< 路由参数（如 locale=zh-cn&dark_mode=1）
    bool     use_debug_server = false;
    wxString debug_server_url; ///< Debug 模式下的远程服务器 URL
};

/**
 * OrcaWebViewLoader - 统一加载本地 HTML 的封装
 *
 * 封装 commit 13842c45ec、6dcad01f8e、945dda4049、545c06a6e1、1736556468 中的加载逻辑，
 * 提供跨平台统一的 HTML 加载接口。
 */
class OrcaWebViewLoader {
public:
    /**
     * 解析 Web 根路径：优先用户路径，否则使用 resources
     * @param user_web_path 用户自定义路径；空时仅用 resources_dir()/web/flutter_web
     * @return 完整配置
     */
    static OrcaWebLoadConfig ResolveConfig(const wxString& user_web_path = "D:\\snapmaker\\web");

    /**
     * 从 app_config 动态生成 route_params（locale、dark_mode）
     */
    static wxString BuildRouteParamsFromApp();

    /**
     * 按 path 创建预置配置（查询参数 path 为无前导斜杠片段，见 FlutterPathQueryValue）：
     * 1→home，2→deviceControl，4→preUpload，5→preUploadAndPrint（预打印）
     */
    static OrcaWebLoadConfig CreateConfigForPage(int path);

    /**
     * Flutter orcaRouter 从 URL 查询参数 path 读取路由片段（无前导 '/'，如 home、deviceControl）；
     * 嵌入 URL 时避免 path=/foo 中的裸斜杠破坏查询解析。对应 GoRoute 仍为 /home、/deviceControl 等。
     */
    static wxString FlutterPathQueryValue(const wxString& page_id);

    /**
     * 创建并返回 user_assets 目录路径
     */
    static wxString EnsureUserAssetsDir();

    /**
     * 加载本地 HTML 到 WebView
     *
     * - Windows：返回待加载的完整 URL，调用方需在首次 EVT_SIZE 时 LoadURL
     * - 非 Windows：直接 SetPage(html, baseUrl)，返回空
     *
     * @param webview 目标 WebView
     * @param config  加载配置
     * @return Windows 下返回完整 URL；非 Windows 返回空（已加载）
     */
    static wxString LoadLocalHtml(wxWebView* webview, const OrcaWebLoadConfig& config);

    /**
     * 构建带 query 的完整入口 URL
     */
    static wxString BuildFullEntryUrl(const OrcaWebLoadConfig& config);

    /**
     * 构建 SetPage 的 baseUrl（非 Windows 用）
     */
    static wxString BuildBaseUrl(const OrcaWebLoadConfig& config);

    /**
     * 从 root_path 读取 index.html 内容
     */
    static wxString ReadIndexHtml(const OrcaWebLoadConfig& config);

    /**
     * 直接加载 HTML 内容到 WebView（适用于已有 HTML 字符串的场景）
     * @param webview 目标 WebView
     * @param html    HTML 内容
     * @param base_url 基准 URL（如 orca://app/），用于解析相对路径
     */
    static void LoadHtml(wxWebView* webview, const wxString& html, const wxString& base_url);
};

} // namespace GUI
} // namespace Slic3r
