// Implementation of Moonraker printer host communication
#include "MoonRaker.hpp"
#include "MQTT.hpp"

#include <algorithm>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/convert.hpp>
#include <curl/curl.h>

#include <wx/progdlg.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "Bonjour.hpp"
#include "slic3r/GUI/BonjourDialog.hpp"
#include "slic3r/GUI/WebPreprintDialog.hpp"
#include "slic3r/GUI/SSWCP.hpp"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
#endif

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;


namespace Slic3r {

namespace {

#ifdef WIN32
// Helper function to extract host and port from URL
std::string get_host_from_url(const std::string& url_in)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始解析URL获取主机信息: " << url_in;
    
    std::string url = url_in;
    // Add http:// if there is no scheme
    size_t double_slash = url.find("//");
    if (double_slash == std::string::npos) {
        url = "http://" + url;
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 添加默认协议前缀，修改后的URL: " << url;
    }
    
    std::string out  = url;
    CURLU*      hurl = curl_url();
    if (hurl) {
        // Parse the input URL
        CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, url.c_str(), 0);
        if (rc == CURLUE_OK) {
            // Extract host and port
            char* host;
            rc = curl_url_get(hurl, CURLUPART_HOST, &host, 0);
            if (rc == CURLUE_OK) {
                char* port;
                rc = curl_url_get(hurl, CURLUPART_PORT, &port, 0);
                if (rc == CURLUE_OK && port != nullptr) {
                    out = std::string(host) + ":" + port;
                    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 成功提取主机和端口: " << out;
                    curl_free(port);
                } else {
                    out = host;
                    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 成功提取主机（无端口）: " << out;
                    curl_free(host);
                }
            } else
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 从URL获取主机失败: " << url;
        } else
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 解析URL失败: " << url;
        curl_url_cleanup(hurl);
    } else
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 分配curl_url失败";
    
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] URL解析完成，返回结果: " << out;
    return out;
}

// Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
std::string substitute_host(const std::string& orig_addr, std::string sub_addr)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始替换主机地址，原地址: " << orig_addr << ", 新地址: " << sub_addr;
    
    // put ipv6 into [] brackets
    if (sub_addr.find(':') != std::string::npos && sub_addr.at(0) != '[') {
        sub_addr = "[" + sub_addr + "]";
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 检测到IPv6地址，添加方括号: " << sub_addr;
    }

#if 0
    //URI = scheme ":"["//"[userinfo "@"] host [":" port]] path["?" query]["#" fragment]
    std::string final_addr = orig_addr;
    //  http
    size_t double_dash = orig_addr.find("//");
    size_t host_start = (double_dash == std::string::npos ? 0 : double_dash + 2);
    // userinfo
    size_t at = orig_addr.find("@");
    host_start = (at != std::string::npos && at > host_start ? at + 1 : host_start);
    // end of host, could be port(:), subpath(/) (could be query(?) or fragment(#)?)
    // or it will be ']' if address is ipv6 )
    size_t potencial_host_end = orig_addr.find_first_of(":/", host_start); 
    // if there are more ':' it must be ipv6
    if (potencial_host_end != std::string::npos && orig_addr[potencial_host_end] == ':' && orig_addr.rfind(':') != potencial_host_end) {
        size_t ipv6_end = orig_addr.find(']', host_start);
        // DK: Uncomment and replace orig_addr.length() if we want to allow subpath after ipv6 without [] parentheses.
        potencial_host_end = (ipv6_end != std::string::npos ? ipv6_end + 1 : orig_addr.length()); //orig_addr.find('/', host_start));
    }
    size_t host_end = (potencial_host_end != std::string::npos ? potencial_host_end : orig_addr.length());
    // now host_start and host_end should mark where to put resolved addr
    // check host_start. if its nonsense, lets just use original addr (or  resolved addr?)
    if (host_start >= orig_addr.length()) {
        return final_addr;
    }
    final_addr.replace(host_start, host_end - host_start, sub_addr);
    return final_addr;
#else
    // Using the new CURL API for handling URL. https://everything.curl.dev/libcurl/url
    // If anything fails, return the input unchanged.
    std::string out  = orig_addr;
    CURLU*      hurl = curl_url();
    if (hurl) {
        // Parse the input URL.
        CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, orig_addr.c_str(), 0);
        if (rc == CURLUE_OK) {
            // Replace the address.
            rc = curl_url_set(hurl, CURLUPART_HOST, sub_addr.c_str(), 0);
            if (rc == CURLUE_OK) {
                // Extract a string fromt the CURL URL handle.
                char* url;
                rc = curl_url_get(hurl, CURLUPART_URL, &url, 0);
                if (rc == CURLUE_OK) {
                    out = url;
                    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 主机地址替换成功: " << out;
                    curl_free(url);
                } else
                    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 替换后提取URL失败";
            } else
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 替换主机失败，主机: " << sub_addr << ", URL: " << orig_addr;
        } else
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 解析原始URL失败: " << orig_addr;
        curl_url_cleanup(hurl);
    } else
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 分配curl_url失败";
    return out;
#endif
}
#endif // WIN32

// Helper function to URL encode a string
std::string escape_string(const std::string& unescaped)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始URL编码字符串，长度: " << unescaped.size();
    
    std::string ret_val;
    CURL*       curl = curl_easy_init();
    if (curl) {
        char* decoded = curl_easy_escape(curl, unescaped.c_str(), unescaped.size());
        if (decoded) {
            ret_val = std::string(decoded);
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] URL编码成功";
            curl_free(decoded);
        } else {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] URL编码失败";
        }
        curl_easy_cleanup(curl);
    } else {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 初始化CURL失败";
    }
    return ret_val;
}

// Helper function to URL encode each element of a filesystem path
std::string escape_path_by_element(const boost::filesystem::path& path)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始路径元素编码: " << path.string();
    
    std::string             ret_val = escape_string(path.filename().string());
    boost::filesystem::path parent(path.parent_path());
    while (!parent.empty() &&
           parent.string() != "/") // "/" check is for case "/file.gcode" was inserted. Then boost takes "/" as parent_path.
    {
        ret_val = escape_string(parent.filename().string()) + "/" + ret_val;
        parent  = parent.parent_path();
    }
    
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 路径编码完成: " << ret_val;
    return ret_val;
}
} // namespace

Moonraker::Moonraker(DynamicPrintConfig* config)
    : m_host(config->opt_string("print_host"))
    , m_apikey(config->opt_string("printhost_apikey"))
    , m_cafile(config->opt_string("printhost_cafile"))
    , m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 初始化Moonraker实例，主机: " << m_host;
}

// Return the name of this print host type
const char* Moonraker::get_name() const { return "Moonraker"; }

#ifdef WIN32
bool Moonraker::test_with_resolved_ip(wxString& msg) const
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始使用解析的IP进行测试";
    
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure
    const char* name = get_name();
    bool        res  = true;
    // Msg contains ip string.
    auto url = substitute_host(make_url("api/version"), GUI::into_u8(msg));
    msg.Clear();

    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << boost::format("%1%: 获取版本信息，URL: %2%") % name % url;

    std::string host = get_host_from_url(m_host);
    auto        http = Http::get(url); // std::move(url));
    // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
    // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
    // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
    // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy
    // is used (issue #9734). Also when allow_ip_resolve = 0, this is not needed, but it should not break anything if it stays.
    // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    http.header("Host", host);
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] " << boost::format("%1%: 获取版本信息出错，URL: %2%, 错误: %3%, HTTP状态: %4%, 响应体: `%5%`") % name % url %
                                            error % status % body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << boost::format("%1%: 获取版本信息成功: %2%") % name % body;

            try {
                std::stringstream ss(body);
                pt::ptree         ptree;
                pt::read_json(ss, ptree);

                if (!ptree.get_optional<std::string>("api")) {
                    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 响应中未找到API字段";
                    res = false;
                    return;
                }

                const auto text = ptree.get_optional<std::string>("text");
                res             = validate_version_text(text);
                if (!res) {
                    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 版本文本验证失败";
                    msg = GUI::format_wxstr(_L("Mismatched type of print host: %s"), (text ? *text : name));
                } else {
                    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 版本验证成功";
                }
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 解析服务器响应失败: " << e.what();
                res = false;
                msg = "Could not parse server response.";
            }
        })
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .perform_sync();

    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] IP测试完成，结果: " << (res ? "成功" : "失败");
    return res;
}
#endif // WIN32

bool Moonraker::get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, json& response) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始获取机器信息，目标数量: " << targets.size();
    
    bool res = true;

    auto url = make_url("printer/objects/query");
    auto http = Http::post(std::move(url));

    for (const auto pair : targets) {
        std::string value = "";
        for (size_t i = 0; i < pair.second.size(); ++i) {
            if (i != 0) {
                value += ",";
            }
            value += pair.second[i];
        }
        http.form_add(pair.first, value);
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 添加查询参数: " << pair.first << " = " << value;
    }

    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 获取机器信息失败，错误: " << error << ", HTTP状态: " << status;
            res = false;
            try{
                response = json::parse(body);
            } catch (std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 解析错误响应失败: " << e.what();
            }
        })
        .on_complete([&](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 获取机器信息成功";
            try {
                response = json::parse(body);
            } catch (std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 解析机器信息响应失败: " << e.what();
            }
        })
        .perform_sync();

    return res;
}

bool Moonraker::send_gcodes(const std::vector<std::string>& codes, std::string& extraInfo)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始发送G代码，数量: " << codes.size();
    
    bool res = true;
    std::string param = "?script=";
    for (const auto code : codes) {
        param += "\n";
        param += code;
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 添加G代码: " << code;
    }
    auto url = make_url("printer/gcode/script");
    auto http = Http::post(std::move(url));

    http.form_add("script", param)
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送G代码失败，错误: " << error << ", HTTP状态: " << status;
            res = false;    
        })
        .on_complete([&](std::string body, unsigned){
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] G代码发送成功";
            res = true;
        })
        .perform_sync();

    return res;
}

bool Moonraker::test(wxString& msg) const
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始连接测试";
    
    //// test
    //MqttClient* mqttClient = new MqttClient("mqtt://172.18.0.9:1883", "OrcaClient", false);
    //mqttClient->SetMessageCallback([](const std::string& topic, const std::string& payload) {
    //    int a = 0;
    //    int b = 0;
    //});
    //mqttClient->Connect();
    //mqttClient->Subscribe("9BD4E436F33D0B56/response", 2);
    //mqttClient->Subscribe("9BD4E436F33D0B56/status", 2);
    //mqttClient->Publish("9BD4E436F33D0B56/request", "{\"jsonrpc\": \"2.0\",\"method\": \"server.info\",\"id\": 9546}", 1);
    
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure
    const char* name = get_name();

    bool res = true;
    auto url = make_url("printer/info");

    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << boost::format("%1%: 获取版本信息，URL: %2%") % name % url;
    // Here we do not have to add custom "Host" header - the url contains host filled by user and libCurl will set the header by itself.
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] " << boost::format("%1%: 获取版本信息出错: %2%, HTTP状态: %3%, 响应体: `%4%`") % name % error % status %
                                            body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << boost::format("%1%: 获取版本信息成功: %2%") % name % body;

            //try {
            //    std::stringstream ss(body);
            //    pt::ptree         ptree;
            //    pt::read_json(ss, ptree);

            //    if (!ptree.get_optional<std::string>("api")) {
            //        res = false;
            //        return;
            //    }

            //    const auto text = ptree.get_optional<std::string>("text");
            //    // test
            //    //res             = validate_version_text(text);

            //    res = true;
            //    if (!res) {
            //        msg = GUI::format_wxstr(_L("Mismatched type of print host: %s"), (text ? *text : name));
            //    }
            //} catch (const std::exception&) {
            //    res = false;
            //    msg = "Could not parse server response";
            //}
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .on_ip_resolve([&](std::string address) {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Remember resolved address to be reused at successive REST API call.
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] IP解析成功: " << address;
            msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();

    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 连接测试完成，结果: " << (res ? "成功" : "失败");
    return res;
}

// Return success message for connection test
wxString Moonraker::get_test_ok_msg() const { return _(L("Connection to Moonraker works correctly.")); }

// Return formatted error message for failed connection test
wxString Moonraker::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s\n\n%s", _L("Could not connect to Moonraker"), msg,
                             _L("Note: Moonraker version at least 1.1.0 is required."));
}

// Upload a file to the printer
bool Moonraker::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始文件上传，源文件: " << upload_data.source_path.string() 
                           << ", 目标路径: " << upload_data.upload_path.string();
    
    if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 上传后需要开始打印，显示预打印对话框";
        
        // 创建 promise 和 future 用于线程同步
        std::promise<bool> promise;
        auto               future = promise.get_future();

        // 使用 CallAfter 切换到主线程
        wxTheApp->CallAfter([this, &promise, upload_data]() {
            GUI::WebPreprintDialog dialog;

            dialog.set_gcode_file_name(upload_data.source_path.string());
            dialog.set_display_file_name(upload_data.upload_path.string());
            bool res = dialog.run();

            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 预打印对话框结果: " << (res ? "确认" : "取消");
            // 设置结果
            promise.set_value(res);
        });

        // 等待主线程完成
        bool flag = future.get();

        if (!flag) {
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 用户取消了打印，终止上传";
            Http::Progress progress(0, 0, 0, 0, "");
            bool           cancel = true;
            prorgess_fn(progress, cancel);
            return false;
        }
    }

#ifndef WIN32
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 使用主机名上传（非Windows）";
    return upload_inner_with_host(std::move(upload_data), prorgess_fn, error_fn, info_fn);
#else
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Windows平台，开始IP解析流程";
    std::string host = get_host_from_url(m_host);

    // decide what to do based on m_host - resolve hostname or upload to ip
    std::vector<boost::asio::ip::address> resolved_addr;
    boost::system::error_code             ec;
    boost::asio::ip::address              host_ip = boost::asio::ip::make_address(host, ec);
    if (!ec) {
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 主机已是IP地址: " << host_ip.to_string();
        resolved_addr.push_back(host_ip);
    } else if (GUI::get_app_config()->get_bool("allow_ip_resolve") && boost::algorithm::ends_with(host, ".local")) {
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始解析.local域名: " << host;
        Bonjour("Moonraker")
            .set_hostname(host)
            .set_retries(5) // number of rounds of queries send
            .set_timeout(1) // after each timeout, if there is any answer, the resolving will stop
            .on_resolve([&ra = resolved_addr](const std::vector<BonjourReply>& replies) {
                for (const auto& rpl : replies) {
                    boost::asio::ip::address ip(rpl.ip);
                    ra.emplace_back(ip);
                    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 解析到IP地址: " << rpl.ip;
                }
            })
            .resolve_sync();
    }

    // Handle different resolution scenarios
    if (resolved_addr.empty()) {
        // no resolved addresses - try system resolving
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 无法解析主机名 " << m_host
                                 << " 到IP地址，使用系统解析进行上传";
        return upload_inner_with_host(std::move(upload_data), prorgess_fn, error_fn, info_fn);
    } else if (resolved_addr.size() == 1) {
        // one address resolved - upload there
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 解析到单个地址，开始上传: " << resolved_addr.front().to_string();
        return upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn, error_fn, info_fn, resolved_addr.front());
    } else if (resolved_addr.size() == 2 && resolved_addr[0].is_v4() != resolved_addr[1].is_v4()) {
        // there are just 2 addresses and 1 is ip_v4 and other is ip_v6
        // try sending to both. (Then if both fail, show both error msg after second try)
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 解析到IPv4和IPv6地址，依次尝试上传";
        wxString error_message;
        if (!upload_inner_with_resolved_ip(
                std::move(upload_data), prorgess_fn,
                [&msg = error_message, resolved_addr](wxString error) { 
                    BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 第一个IP上传失败: " << resolved_addr.front().to_string();
                    msg = GUI::format_wxstr("%1%: %2%", resolved_addr.front(), error); 
                },
                info_fn, resolved_addr.front()) &&
            !upload_inner_with_resolved_ip(
                std::move(upload_data), prorgess_fn,
                [&msg = error_message, resolved_addr](wxString error) {
                    BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 第二个IP上传失败: " << resolved_addr.back().to_string();
                    msg += GUI::format_wxstr("\n%1%: %2%", resolved_addr.back(), error);
                },
                info_fn, resolved_addr.back())) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 所有IP地址上传均失败";
            error_fn(error_message);
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 至少一个IP地址上传成功";
        return true;
    } else {
        // There are multiple addresses - user needs to choose which to use.
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 解析到多个地址（" << resolved_addr.size() << "个），需要用户选择";
        size_t       selected_index = resolved_addr.size();
        IPListDialog dialog(nullptr, boost::nowide::widen(m_host), resolved_addr, selected_index);
        if (dialog.ShowModal() == wxID_OK && selected_index < resolved_addr.size()) {
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 用户选择IP地址: " << resolved_addr[selected_index].to_string();
            return upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn, error_fn, info_fn, resolved_addr[selected_index]);
        } else {
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 用户取消IP选择";
        }
    }
    return false;
#endif // WIN32
}

#ifdef WIN32
// Upload file using resolved IP address
bool Moonraker::upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 使用解析的IP地址上传: " << resolved_addr.to_string();
    
    info_fn(L"resolve", boost::nowide::widen(resolved_addr.to_string()));

    // If test fails, test_msg_or_host_ip contains the error message.
    // Otherwise on Windows it contains the resolved IP address of the host.
    // Test_msg already contains resolved ip and will be cleared on start of test().
    wxString test_msg_or_host_ip = GUI::from_u8(resolved_addr.to_string());
    if (/* !test_with_resolved_ip(test_msg_or_host_ip)*/ false) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] IP测试失败";
        error_fn(std::move(test_msg_or_host_ip));
        return false;
    }

    const char* name               = get_name();
    const auto  upload_filename    = upload_data.upload_path.filename();
    const auto  upload_parent_path = upload_data.upload_path.parent_path();
    std::string url                = substitute_host(make_url("server/files/upload"), resolved_addr.to_string());
    bool        result             = true;

    info_fn(L"resolve", boost::nowide::widen(url));

    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << boost::format("%1%: 上传文件 %2% 到 %3%, 文件名: %4%, 路径: %5%, 打印: %6%") % name %
                                   upload_data.source_path % url % upload_filename.string() % upload_parent_path.string() %
                                   (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "是" : "否");

    std::string host = get_host_from_url(m_host);
    auto        http = Http::post(url); // std::move(url));
    // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
    // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
    // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
    // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy
    // is used (issue #9734). https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    http.header("Host", host);
    set_auth(http);
    http.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        .form_add("path", upload_parent_path.string()) // XXX: slashes on windows ???
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())

        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << boost::format("%1%: 文件上传成功: HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] " << boost::format("%1%: 上传文件到 %2% 失败: %3%, HTTP %4%, 响应体: `%5%`") % name % url % error %
                                            status % body;

            // 尝试8080端口
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 尝试使用8080端口重新上传";
            url = substitute_host(make_url_8080("server/files/upload"), GUI::into_u8(test_msg_or_host_ip));

            auto http_8080 = Http::post(std::move(url));
            http_8080.header("host", host);
            set_auth(http_8080);

            http_8080.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
                .form_add("path", upload_parent_path.string()) // XXX: slashes on windows ???
                .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
                .on_complete([&](std::string body, unsigned status) {
                    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << boost::format("%1%: 8080端口文件上传成功: HTTP %2%: %3%") % name % status % body;
                })
                .on_error([&](std::string body, std::string error, unsigned status) {
                    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] " << boost::format("%1%: 8080端口上传失败: %2%, HTTP %3%, 响应体: `%4%`") % name % error %
                                                    status % body;

                    error_fn(format_error(body, error, status));
                    result = false;
                })
                .on_progress([&](Http::Progress progress, bool& cancel) {
                    prorgess_fn(std::move(progress), cancel);
                    if (cancel) {
                        // Upload was canceled
                        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << name << ": 8080端口上传被取消";
                        result = false;
                    }
                })
#ifdef WIN32
                .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
                .perform_sync();
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << name << ": 上传被取消";
                result = false;
            }
        })
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .perform_sync();

    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] IP地址上传完成，结果: " << (result ? "成功" : "失败");
    return result;
}
#endif // WIN32

// Upload file using hostname
bool Moonraker::upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 使用主机名上传文件";
    
    const char* name = get_name();

    const auto upload_filename    = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();

    // If test fails, test_msg_or_host_ip contains the error message.
    // Otherwise on Windows it contains the resolved IP address of the host.
    wxString test_msg_or_host_ip;
    
    /*
    if (!test(test_msg_or_host_ip)) {
        error_fn(std::move(test_msg_or_host_ip));
        return false;
    }
     */

    std::string url;
    bool        res = true;

#ifdef WIN32
    // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
    if (m_host.find("https://") == 0 || test_msg_or_host_ip.empty() || !GUI::get_app_config()->get_bool("allow_ip_resolve"))
#endif // _WIN32
    {
        // If https is entered we assume signed ceritificate is being used
        // IP resolving will not happen - it could resolve into address not being specified in cert
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 使用HTTPS或禁用IP解析，直接使用主机名";
        url = make_url("server/files/upload");
    }
#ifdef WIN32
    else {
        // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
        // Curl uses easy_getinfo to get ip address of last successful transaction.
        // If it got the address use it instead of the stored in "host" variable.
        // This new address returns in "test_msg_or_host_ip" variable.
        // Solves troubles of uploades failing with name address.
        // in original address (m_host) replace host for resolved ip
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 使用IP解析workaround";
        info_fn(L"resolve", test_msg_or_host_ip);
        url = substitute_host(make_url("server/files/upload"), GUI::into_u8(test_msg_or_host_ip));
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] IP解析后的上传地址: " << url;
    }
#endif // _WIN32

    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << boost::format("%1%: 上传文件 %2% 到 %3%, 文件名: %4%, 路径: %5%, 打印: %6%") % name %
                                   upload_data.source_path % url % upload_filename.string() % upload_parent_path.string() %
                                   (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "是" : "否");

    auto http = Http::post(std::move(url));
#ifdef WIN32
    // "Host" header is necessary here. In the workaround above (two mDNS..) we have got IP address from test connection and subsituted it
    // into "url" variable. And when creating Http object above, libcurl automatically includes "Host" header from address it got. Thus "Host"
    // is set to the resolved IP instead of host filled by user. We need to change it back. Not changing the host would work on the most cases
    // (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy is used (issue #9734). Also when allow_ip_resolve =
    // 0, this is not needed, but it should not break anything if it stays. https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    std::string host = get_host_from_url(m_host);
    http.header("Host", host);
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 设置Host头: " << host;
#endif // _WIN32
    set_auth(http);
    http.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        .form_add("path", upload_parent_path.string()) // XXX: slashes on windows ???
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << boost::format("%1%: 文件上传成功: HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] " << boost::format("%1%: 上传文件失败: %2%, HTTP %3%, 响应体: `%4%`") % name % error % status %
                                            body;

            // 尝试8080端口
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 尝试使用8080端口重新上传";
            url = make_url_8080("server/files/upload");
            
            auto http_8080 = Http::post(std::move(url));
            set_auth(http_8080);

            http_8080.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
                .form_add("path", upload_parent_path.string()) // XXX: slashes on windows ???
                .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
                .on_complete([&](std::string body, unsigned status) {
                    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << boost::format("%1%: 8080端口文件上传成功: HTTP %2%: %3%") % name % status % body;
                })
                .on_error([&](std::string body, std::string error, unsigned status) {
                    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] " << boost::format("%1%: 8080端口上传失败: %2%, HTTP %3%, 响应体: `%4%`") % name % error %
                                                    status % body;

                    error_fn(format_error(body, error, status));
                    res = false;
                })
                .on_progress([&](Http::Progress progress, bool& cancel) {
                    prorgess_fn(std::move(progress), cancel);
                    if (cancel) {
                        // Upload was canceled
                        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << name << ": 8080端口上传被取消";
                        res = false;
                    }
                })
#ifdef WIN32
                .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
                .perform_sync();
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] " << name << ": 上传被取消";
                res = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 主机名上传完成，结果: " << (res ? "成功" : "失败");
    return res;
}

// Validate version text to confirm printer type
bool Moonraker::validate_version_text(const boost::optional<std::string>& version_text) const
{
    bool result = version_text ? boost::starts_with(*version_text, "Moonraker") : true;
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 版本文本验证: " << (version_text ? *version_text : "空") 
                            << ", 结果: " << (result ? "通过" : "失败");
    return result;
}

// Set authentication headers for HTTP requests
void Moonraker::set_auth(Http& http) const
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 设置认证头，API密钥长度: " << m_apikey.length();
    http.header("X-Api-Key", m_apikey);

    if (!m_cafile.empty()) {
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 设置CA文件: " << m_cafile;
        http.ca_file(m_cafile);
    }
}

std::string Moonraker::make_url_8080(const std::string& path) const
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 构建8080端口URL，路径: " << path;
    
    size_t pos = m_host.find(":");
    std::string target_host = m_host;
    if (pos != std::string::npos && pos != std::string("http:").length() - 1 && std::string("https:").length() - 1) {
        target_host = target_host.substr(0, pos);
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 移除端口号，目标主机: " << target_host;
    }
    
    std::string result;
    if (target_host.find("http://") == 0 || target_host.find("https://") == 0) {
        if (target_host.back() == '/') {
            target_host = target_host.substr(0, target_host.length() - 1);
            result = (boost::format("%1%:8080/%2%") % target_host % path).str();
        } else {
            result = (boost::format("%1%:8080/%2%") % target_host % path).str();
        }
    } else {
        result = (boost::format("http://%1%:8080/%2%") % target_host % path).str();
    }
    
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 8080端口URL构建完成: " << result;
    return result;
}

// Construct full URL for API endpoint
std::string Moonraker::make_url(const std::string& path) const
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0) {
        if (m_host.back() == '/') {
            return (boost::format("%1%%2%") % m_host % path).str();
        } else {
            return (boost::format("%1%/%2%") % m_host % path).str();
        }
    } else {
        if (m_host.find(":1884") != std::string::npos) {
            std::string http_host = m_host.substr(0, m_host.find(":1884"));
            return (boost::format("http://%1%/%2%") % http_host % path).str();
        }
        if (m_host.find(":8883") != std::string::npos) {
            std::string http_host = m_host.substr(0, m_host.find(":8883"));
            return (boost::format("http://%1%/%2%") % http_host % path).str();
        }
        return (boost::format("http://%1%/%2%") % m_host % path).str();
    }
}

// Basic connect implementation
bool Moonraker::connect(wxString& msg, const nlohmann::json& params) {
    return test(msg);
}








// Moonraker_mqtt

std::shared_ptr<MqttClient> Moonraker_Mqtt::m_mqtt_client = nullptr;
std::shared_ptr<MqttClient> Moonraker_Mqtt::m_mqtt_client_tls = nullptr;
TimeoutMap<int64_t, Moonraker_Mqtt::RequestCallback> Moonraker_Mqtt::m_request_cb_map;
std::function<void(const nlohmann::json&)> Moonraker_Mqtt::m_status_cb = nullptr;
std::string Moonraker_Mqtt::m_response_topic = "/response";
std::string Moonraker_Mqtt::m_status_topic = "/status";
std::string Moonraker_Mqtt::m_notification_topic = "/notification";
std::string Moonraker_Mqtt::m_request_topic = "/request";
std::string Moonraker_Mqtt::m_sn = "";
std::mutex Moonraker_Mqtt::m_sn_mtx;
std::string Moonraker_Mqtt::m_auth_topic = "/config/response";
std::string Moonraker_Mqtt::m_auth_req_topic = "/config/request";
nlohmann::json Moonraker_Mqtt::m_auth_info = nlohmann::json::object();


Moonraker_Mqtt::Moonraker_Mqtt(DynamicPrintConfig* config, bool change_engine) : Moonraker(config) {
    std::string host_info = config->option<ConfigOptionString>("print_host")->value;

    if (change_engine) {
        // 获取本地IP
        std::string local_ip;
        try {
            #ifdef _WIN32
                SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
                if (sock == INVALID_SOCKET) {
                    throw std::runtime_error("Failed to create socket");
                }
            #else
                int sock = socket(AF_INET, SOCK_DGRAM, 0);
                if (sock < 0) {
                    throw std::runtime_error("Failed to create socket");
                }
            #endif
            
            // 连接到一个外部地址（这里使用Google的DNS服务器）
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(53);
            inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);
            
            if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                #ifdef _WIN32
                    closesocket(sock);
                #else
                    close(sock);
                #endif
                throw std::runtime_error("Failed to connect");
            }
            
            // 获取本地地址
            struct sockaddr_in local_addr;
            socklen_t len = sizeof(local_addr);
            if (getsockname(sock, (struct sockaddr*)&local_addr, &len) < 0) {
                #ifdef _WIN32
                    closesocket(sock);
                #else
                    close(sock);
                #endif
                throw std::runtime_error("Failed to get local address");
            }
            
            // 转换为字符串
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &local_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
            local_ip = ip_str;
            
            #ifdef _WIN32
                closesocket(sock);
            #else
                close(sock);
            #endif

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Error getting local IP: " << e.what();
            local_ip = "0.0.0.0"; // 失败时使用默认IP
        }
        m_mqtt_client.reset(new MqttClient("mqtt://" + host_info, local_ip, true));
        m_mqtt_client_tls.reset();
        BOOST_LOG_TRIVIAL(error) << "本地ip" << local_ip;
    }
    
}

// set auth info
void Moonraker_Mqtt::set_auth_info(const nlohmann::json& info) {
    m_auth_info = info;
}

nlohmann::json Moonraker_Mqtt::get_auth_info() {
    json authinfo;
    authinfo["user"] = m_user_name;
    authinfo["password"] = m_password;
    authinfo["ca"]       = m_ca;
    authinfo["cert"]     = m_cert;
    authinfo["key"]      = m_key;
    authinfo["clientid"] = m_client_id;
    authinfo["port"]     = m_port;

    return authinfo;
}

// Ask for TLS info
bool Moonraker_Mqtt::ask_for_tls_info(const nlohmann::json& cn_params)
{
    if (!m_mqtt_client) {
        return false;
    }

    if(m_mqtt_client->CheckConnected()) {
        m_mqtt_client->Disconnect();
    }

    m_sn_mtx.lock();
    m_sn = "";
    m_sn_mtx.unlock();

    bool is_connect = m_mqtt_client->Connect();
    if(!is_connect || !cn_params.count("code") || cn_params["code"].get<std::string>() == "") {
        return false;
    }

    std::string auth_code  = cn_params["code"].get<std::string>();

    bool response_subscribed = m_mqtt_client->Subscribe(auth_code + m_auth_topic, 1);
    if (!response_subscribed) {
        return false;
    }
    m_mqtt_client->SetMessageCallback([this](const std::string& topic, const std::string& payload) {
        this->on_mqtt_message_arrived(topic, payload);
    });

    json body;
    body["jsonrpc"] = "2.0";
    body["method"] = "server.request_key";
    json params;
    std::string clientid;
    try {
        clientid = m_mqtt_client->get_client_id();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Error getting local IP: " << e.what();
        clientid = "0.0.0.0"; // 失败时使用默认IP
    }
    if(clientid == "0.0.0.0") {
        return false;
    }

    params["clientid"] = clientid;
    body["params"] = params;

    int64_t seq_id = m_seq_generator.generate_seq_id();

    // 同步等待
    std::promise<bool> auth_promise;
    std::future<bool> auth_future = auth_promise.get_future();

    auto callback = [this, &auth_promise](const nlohmann::json& res) {
        try{
            json result = res;
            std::string state = result["state"].get<std::string>();
            if (state != "success") {
                auth_promise.set_value(false);
                return;
            }

            // sn
            m_sn_mtx.lock();
            m_sn = result["sn"].get<std::string>();
            m_sn_mtx.unlock();

            
            m_client_id = result["clientid"].get<std::string>();
            m_user_name = result["username"].get<std::string>();
            m_password = result["password"].get<std::string>();
            m_ca = result["ca"].get<std::string>();
            m_cert = result["cert"].get<std::string>();
            m_key = result["key"].get<std::string>();
            m_port = result["port"].get<int>();

            auth_promise.set_value(true);
        }
        catch(std::exception& e){
            auth_promise.set_value(false);
        }
    };

    auto timeout_callback = [this, &auth_promise]() {
        // 待调整
        auth_promise.set_value(false);
    };

    if (!add_response_target(seq_id, callback, timeout_callback, std::chrono::seconds(60))) {
            return false;
    }
    body["id"] = seq_id;

    if(!m_mqtt_client->Publish(auth_code + m_auth_req_topic, body.dump(), 1)){
        return false;
    }

    // 等待response
    auto status = auth_future.wait_for(std::chrono::seconds(70));
    if(status == std::future_status::timeout){
        return false;
    }

    return auth_future.get();
}

// Connect to MQTT broker
bool Moonraker_Mqtt::connect(wxString& msg, const nlohmann::json& params) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始MQTT连接流程";
    
    // 在创建新连接前检查参数
    if(!params.count("ca") || !params.count("cert") 
    || !params.count("key") || !params.count("port") || !params.count("clientid") || !params.count("sn")){
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 缺少TLS参数，尝试获取认证信息";
        bool flag = ask_for_tls_info(params);
        if (!flag) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 获取TLS认证信息失败";
            return false;
        }
    }else{
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 使用提供的TLS参数";
        m_user_name = params.count("user")     ? params["user"].get<std::string>() : "";
        m_password = params.count("password") ? params["password"].get<std::string>() : "";
        m_ca = params["ca"].get<std::string>();
        m_cert = params["cert"].get<std::string>();
        m_key = params["key"].get<std::string>();
        m_port = params["port"].get<int>();
        m_client_id = params["clientid"].get<std::string>();

        m_sn_mtx.lock();
        m_sn = params["sn"].get<std::string>();
        m_sn_mtx.unlock();
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 设置SN: " << m_sn;

        if (m_ca == "" || m_cert == "" || m_key == "") {
            bool flag = ask_for_tls_info(params);
            if (!flag) {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 获取TLS认证信息失败";
                return false;
            }
        }
    }

    // 验证证书格式
    if (!m_ca.empty()) {
        if (m_ca.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] CA证书格式不正确";
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] CA证书格式验证通过";
    }

    if (!m_cert.empty()) {
        if (m_cert.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 客户端证书格式不正确";
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 客户端证书格式验证通过";
    }

    if (!m_key.empty()) {
        if (m_key.find("-----BEGIN PRIVATE KEY-----") == std::string::npos 
            && m_key.find("-----BEGIN RSA PRIVATE KEY-----") == std::string::npos) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 私钥格式不正确";
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 私钥格式验证通过";
    }

    // 检查端口号
    if (m_port <= 0 || m_port > 65535) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 无效的端口号: " << m_port;
        return false;
    }

    // 记录连接参数（不记录敏感信息的具体内容）
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] MQTTS连接参数:"
        << "\n - 主机: " << m_host
        << "\n - 端口: " << m_port
        << "\n - 客户端ID: " << m_client_id
        << "\n - CA证书是否存在: " << (!m_ca.empty() ? "是" : "否")
        << "\n - 客户端证书是否存在: " << (!m_cert.empty() ? "是" : "否")
        << "\n - 私钥是否存在: " << (!m_key.empty() ? "是" : "否");

    // 在创建新连接前，确保旧连接已断开
    if (m_mqtt_client) {
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 断开旧的MQTT客户端连接";
        m_mqtt_client->Disconnect();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        m_mqtt_client.reset();
    }

    if (m_mqtt_client_tls) {
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 断开旧的MQTTS客户端连接";
        m_mqtt_client_tls->Disconnect();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        m_mqtt_client_tls.reset();
    }

    // 创建新的 MQTTS 连接
    std::string host_ip = m_host;
    size_t      pos     = host_ip.find(":");
    if (pos != std::string::npos) {
        host_ip = host_ip.substr(0, pos);
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 提取主机IP: " << host_ip;
    }
    
    std::string mqtts_url = "mqtts://" + host_ip + ":" + std::to_string(m_port);
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 创建MQTTS客户端，URL: " << mqtts_url << ", 客户端ID: " << m_client_id;
    
    m_mqtt_client_tls.reset(new MqttClient(mqtts_url, m_client_id, m_ca, m_cert, m_key));

    if (!m_mqtt_client_tls) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] MQTT客户端创建失败";
        return false;
    }

    bool is_connect = m_mqtt_client_tls->Connect();
    if (!is_connect) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] MQTT连接失败";
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] MQTTS连接成功";

    m_sn_mtx.lock();
    std::string tmp_sn = m_sn;
    m_sn_mtx.unlock();

    if(tmp_sn == ""){
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] SN为空，无法订阅主题";
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    bool notification_subscribed = m_mqtt_client_tls->Subscribe(tmp_sn + m_notification_topic, 1);
    bool response_subscribed = m_mqtt_client_tls->Subscribe(tmp_sn + m_response_topic, 1);
    
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 订阅主题结果 - 通知主题: " << (notification_subscribed ? "成功" : "失败")
                           << ", 响应主题: " << (response_subscribed ? "成功" : "失败");
    
    m_mqtt_client_tls->SetMessageCallback([this](const std::string& topic, const std::string& payload) {
        this->on_mqtt_tls_message_arrived(topic, payload);
    });

    bool result = is_connect && notification_subscribed && response_subscribed;
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] MQTT连接流程完成，结果: " << (result ? "成功" : "失败");
    return result;
}

// Disconnect from MQTT broker
bool Moonraker_Mqtt::disconnect(wxString& msg, const nlohmann::json& params) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始断开MQTT连接";
    
    if (!m_mqtt_client_tls) {
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] MQTTS客户端不存在，无需断开";
        return false;
    }

    bool flag = m_mqtt_client_tls->Disconnect();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] MQTTS断开连接结果: " << (flag ? "成功" : "失败");
    
    m_sn_mtx.lock();
    m_sn = "";
    m_sn_mtx.unlock();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 重置SN";

    m_mqtt_client_tls.reset();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] MQTTS客户端已重置";

    return flag;
}

// Subscribe to printer status updates
void Moonraker_Mqtt::async_subscribe_machine_info(std::function<void(const nlohmann::json&)> callback)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始订阅机器状态信息";
    
    std::string main_layer = "+";
    
    m_sn_mtx.lock();
    main_layer = m_sn;
    m_sn_mtx.unlock();
    
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 使用SN主题: " << main_layer;

    bool res = m_mqtt_client_tls ? m_mqtt_client_tls->Subscribe(main_layer + m_status_topic, 1) : false;

    if (!res) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 订阅状态主题失败";
        if (m_status_cb) {
            callback(json::value_t::null);
        }
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 成功订阅状态主题: " << main_layer + m_status_topic;
    m_status_cb = callback;
    callback(json::object());
}

// start print job
void Moonraker_Mqtt::async_start_print_job(const std::string& filename, std::function<void(const nlohmann::json&)> cb)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始启动打印任务，文件名: " << filename;
    
    std::string method = "printer.print.start";
    json        params = json::object();
    params["filename"] = filename;

    if (!send_to_request(method, params, true, cb,
                         [cb]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 启动打印任务超时";
                             json res;
                             res["error"] = "timeout";
                             cb(res);
                         }) &&
        cb) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送启动打印请求失败";
        cb(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_pause_print_job(std::function<void(const nlohmann::json&)> cb) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始暂停打印任务";
    
    std::string method =  "printer.print.pause";
    json        params  = json::object();

    if (!send_to_request(method, params, true, cb,
                         [cb]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 暂停打印任务超时";
                             json res;
                             res["error"] = "timeout";
                             cb(res);
                         }) &&
        cb) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送暂停打印请求失败";
        cb(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_resume_print_job(std::function<void(const nlohmann::json&)> cb) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始恢复打印任务";
    
    std::string method = "printer.print.resume";
    json        params = json::object();

    if (!send_to_request(method, params, true, cb,
                         [cb]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 恢复打印任务超时";
                             json res;
                             res["error"] = "timeout";
                             cb(res);
                         }) &&
        cb) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送恢复打印请求失败";
        cb(json::value_t::null);
    }
}

void Moonraker_Mqtt::test_async_wcp_mqtt_moonraker(const nlohmann::json& mqtt_request_params, std::function<void(const nlohmann::json&)> cb) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始测试MQTT Moonraker异步请求";
    
    int64_t id = -1;

    if (!mqtt_request_params.count("id") || !mqtt_request_params[id].is_number()) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 测试请求缺少有效ID";
        cb(json::value_t::null);
        return;
    }

    id = mqtt_request_params["id"].get<int64_t>();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 测试请求ID: " << id;

    if (id == -1) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 无效的请求ID";
        cb(json::value_t::null);
        return;
    }

    if (!add_response_target(id, cb, [cb]() {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 测试请求超时";
            json res;
            res["error"] = "timeout";
            cb(res);
        })){
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 添加响应目标失败";
        cb(json::value_t::null);
    }

    if (m_mqtt_client_tls) {
        std::string main_layer = "+";

        if (wait_for_sn()) {
            m_sn_mtx.lock();
            main_layer = m_sn;
            m_sn_mtx.unlock();

            if (main_layer == "+" || main_layer == "") {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] SN无效，删除响应目标";
                delete_response_target(id);
                cb(json::value_t::null);
                return;
            }

            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 发布测试请求到主题: " << main_layer + m_request_topic;
            bool res = m_mqtt_client_tls->Publish(main_layer + m_request_topic, mqtt_request_params.dump(), 1);
            if (!res) {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发布测试请求失败";
                delete_response_target(id);
            } else {
                BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 测试请求发布成功";
            }
            return;
        }
    }
}

void Moonraker_Mqtt::async_cancel_print_job(std::function<void(const nlohmann::json&)> cb)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始取消打印任务";
    
    std::string method = "printer.print.cancel";
    json        params = json::object();

    if (!send_to_request(method, params, true, cb,
                         [cb]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 取消打印任务超时";
                             json res;
                             res["error"] = "timeout";
                             cb(res);
                         }) &&
        cb) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送取消打印请求失败";
        cb(json::value_t::null);
    }
}

// Get printer info
void Moonraker_Mqtt::async_get_printer_info(std::function<void(const nlohmann::json& response)> callback) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始获取打印机信息";
    
    std::string method = "printer.info";
    json        params = json::object();

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 获取打印机信息超时";
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送获取打印机信息请求失败";
        callback(json::value_t::null);
    }
}

// Send G-code commands to printer
void Moonraker_Mqtt::async_send_gcodes(const std::vector<std::string>& scripts, std::function<void(const nlohmann::json&)> callback)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始发送G代码，数量: " << scripts.size();
    
    std::string method = "printer.gcode.script";

    std::string str_scripts = "";
    for (size_t i = 0; i < scripts.size(); ++i) {
        if (i != 0) {
            str_scripts += "\n";
        }
        str_scripts += scripts[i];
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 添加G代码: " << scripts[i];
    }

    json params;
    params["script"] = str_scripts;

    if (!send_to_request(method, params, true, callback, [callback](){
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 发送G代码超时";
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送G代码请求失败";
        callback(json::value_t::null);
    }
}

// Unsubscribe from printer status updates
void Moonraker_Mqtt::async_unsubscribe_machine_info(std::function<void(const nlohmann::json&)> callback)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始取消订阅机器状态信息";
    
    std::string main_layer = "+";
    
    m_sn_mtx.lock();
    main_layer = m_sn;
    m_sn_mtx.unlock();

    bool res = m_mqtt_client_tls ? m_mqtt_client_tls->Unsubscribe(main_layer + m_status_topic) : false;

    if (!res) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 取消订阅状态主题失败";
        if (callback) {
            callback(json::value_t::null);
        }
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 成功取消订阅状态主题: " << main_layer + m_status_topic;
    m_status_cb = nullptr;
    callback(json::object());
}

// Set filters for printer status subscription
void Moonraker_Mqtt::async_set_machine_subscribe_filter(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& targets,
    std::function<void(const nlohmann::json& response)> callback)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始设置机器订阅过滤器，目标数量: " << targets.size();
    
    std::string method = "printer.objects.subscribe";

    json params;
    params["objects"] = json::object();

    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].second.size() == 0) {
            params["objects"][targets[i].first] = json::value_t::null;
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 添加过滤器（全部）: " << targets[i].first;
        } else {
            params["objects"][targets[i].first] = json::array();

            for (const auto& key : targets[i].second) {
                params["objects"][targets[i].first].push_back(key);
            }
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 添加过滤器: " << targets[i].first 
                                    << ", 字段数量: " << targets[i].second.size();
        }
    }

    if (!send_to_request(method, params, true, callback, [callback](){
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 设置订阅过滤器超时";
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送设置订阅过滤器请求失败";
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_machine_files_roots(std::function<void(const nlohmann::json& response)> callback) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始获取文件系统根目录";
    
    std::string method = "server.files.roots";

    json params = json::object();

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 获取文件系统根目录超时";
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送获取文件系统根目录请求失败";
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_machine_files_metadata(const std::string& filename, std::function<void(const nlohmann::json& response)> callback)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始获取文件元数据，文件名: " << filename;
    
    std::string method = "server.files.metadata";

    json params;
    params["filename"] = filename;

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 获取文件元数据超时";
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送获取文件元数据请求失败";
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_machine_files_thumbnails(const std::string& filename, std::function<void(const nlohmann::json& response)> callback) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始获取文件缩略图，文件名: " << filename;
    
    std::string method = "server.files.thumbnails";

    json params;
    params["filename"] = filename;

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 获取文件缩略图超时";
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送获取文件缩略图请求失败";
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_machine_files_directory(const std::string& path, bool extend, std::function<void(const nlohmann::json& response)> callback) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始获取目录内容，路径: " << path << ", 扩展信息: " << extend;
    
    std::string method = "server.files.get_directory";

    json params;
    params["path"] = path;
    params["extended"] = extend;

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 获取目录内容超时";
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送获取目录内容请求失败";
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_camera_start(const std::string& domain, std::function<void(const nlohmann::json& response)> callback) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始启动摄像头监控，域名: " << domain;
    
    std::string method = "server.camera.start_monitor";

    json params;
    params["domain"]     = domain;

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 启动摄像头监控超时";
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送启动摄像头监控请求失败";
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_canmera_stop(const std::string& domain, std::function<void(const nlohmann::json& response)> callback) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始停止摄像头监控，域名: " << domain;
    
    std::string method = "server.camera.stop_monitor";

    json params;
    params["domain"] = domain;

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 停止摄像头监控超时";
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送停止摄像头监控请求失败";
        callback(json::value_t::null);
    }
}

// Query printer information
void Moonraker_Mqtt::async_get_machine_info(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& targets,
    std::function<void(const nlohmann::json& response)> callback)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始查询打印机信息，目标数量: " << targets.size();
    
    std::string method = "printer.objects.query";

    json params;
    params["objects"] = json::object();

    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].second.size() == 0) {
            params["objects"][targets[i].first] = json::value_t::null;
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 查询对象（全部属性）: " << targets[i].first;
        } else {
            params["objects"][targets[i].first] = json::array();

            for (const auto& key : targets[i].second) {
                params["objects"][targets[i].first].push_back(key);
            }
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 查询对象: " << targets[i].first 
                                    << ", 属性数量: " << targets[i].second.size();
        }
    }

    if (!send_to_request(method, params, true, callback, [callback](){
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 查询打印机信息超时";
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送查询打印机信息请求失败";
        callback(json::value_t::null);
    }
}

// Get system info of the machine
void Moonraker_Mqtt::async_get_system_info(std::function<void(const nlohmann::json& response)> callback)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始获取系统信息";
    
    std::string method = "machine.system_info";
    json params = json::object();

    if (!send_to_request(method, params, true, callback, [callback](){
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 获取系统信息超时";
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送获取系统信息请求失败";
        callback(json::value_t::null);
    }
}

// Get list of available printer objects
void Moonraker_Mqtt::async_get_machine_objects(std::function<void(const nlohmann::json& response)> callback)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 开始获取可用打印机对象列表";
    
    std::string method = "printer.objects.list";
    json params = json::object();

    if (!send_to_request(method, params, true, callback, [callback](){
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 获取打印机对象列表超时";
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发送获取打印机对象列表请求失败";
        callback(json::value_t::null);
    }
}

// Send request to printer via MQTT
bool Moonraker_Mqtt::send_to_request(
    const std::string& method,
    const json& params,
    bool need_response,
    std::function<void(const nlohmann::json& response)> callback,
    std::function<void()> timeout_callback)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 发送请求，方法: " << method << ", 需要响应: " << need_response;
    
    json body;
    body["jsonrpc"] = "2.0";
    body["method"] = method;
    body["params"] = params;

    int64_t seq_id = m_seq_generator.generate_seq_id();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 生成序列ID: " << seq_id;

    if (need_response) {
        if (!add_response_target(seq_id, callback, timeout_callback)) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 添加响应目标失败";
            return false;
        }
        body["id"] = seq_id;
    }

    if (m_mqtt_client_tls) {
        std::string main_layer = "+";
    
        m_sn_mtx.lock();
        main_layer = m_sn;
        m_sn_mtx.unlock();

        if (main_layer == "+" || main_layer == "") {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] SN无效，删除响应目标";
            delete_response_target(seq_id);
            return false;
        }

        std::string topic = main_layer + m_request_topic;
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 发布到主题: " << topic;
        
        bool res = m_mqtt_client_tls->Publish(topic, body.dump(), 1);
        if (!res) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 发布请求失败，方法: " << method;
            delete_response_target(seq_id);
        } else {
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 请求发布成功，方法: " << method;
        }
        return res;

        
    }
    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] MQTTS客户端不可用";
    return false;
}

// Register callback for response to a request
bool Moonraker_Mqtt::add_response_target(
    int64_t id,
    std::function<void(const nlohmann::json&)> callback,
    std::function<void()> timeout_callback,
    std::chrono::milliseconds timeout)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 注册响应回调，ID: " << id << ", 超时: " << timeout.count() << "ms";
    
    return m_request_cb_map.add(
        id,
        RequestCallback(std::move(callback), std::move(timeout_callback)),
        timeout
    );
}

// Remove registered callback
void Moonraker_Mqtt::delete_response_target(int64_t id) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 删除响应目标，ID: " << id;
    m_request_cb_map.remove(id);
}

bool Moonraker_Mqtt::check_sn_arrived() {
    bool result = wait_for_sn();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 检查SN到达状态: " << (result ? "已到达" : "未到达");
    return result;
}

// Get and remove callback for a request
std::function<void(const json&)> Moonraker_Mqtt::get_request_callback(int64_t id)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 获取并移除请求回调，ID: " << id;
    
    auto request_cb = m_request_cb_map.get_and_remove(id);
    return request_cb ? request_cb->success_cb : nullptr;
}

// Handle incoming MQTTs messages
void Moonraker_Mqtt::on_mqtt_tls_message_arrived(const std::string& topic, const std::string& payload) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 收到MQTTS消息，主题: " << topic << ", 载荷长度: " << payload.length();
    
    try {
        if (topic.find(m_response_topic) != std::string::npos) {
            size_t pos = topic.find("/response");

            if (pos != std::string::npos) {
                std::string sn = topic.substr(0, pos);
                BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 从响应主题提取SN: " << sn;

                m_sn_mtx.lock();
                m_sn = sn;
                m_sn_mtx.unlock();
            }

            on_response_arrived(payload);
        } else if (topic.find(m_status_topic) != std::string::npos) {
            size_t pos = topic.find("/status");

            if (pos != std::string::npos) {
                std::string sn = topic.substr(0, pos);
                BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 从状态主题提取SN: " << sn;

                m_sn_mtx.lock();
                m_sn = sn;
                m_sn_mtx.unlock();
            }

            on_status_arrived(payload);
        } else if (topic.find(m_notification_topic) != std::string::npos) {
            
            size_t pos = topic.find("/notification");

            if (pos != std::string::npos) {
                std::string sn = topic.substr(0, pos);
                BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 从通知主题提取SN: " << sn;

                m_sn_mtx.lock();
                m_sn = sn;
                m_sn_mtx.unlock();
            }
            

            on_notification_arrived(payload);
        }
        else {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 收到未知主题的消息: " << topic;
            return;
        }
    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 处理MQTTS消息异常: " << e.what();
    }
}

// Handle incoming MQTT messages
void Moonraker_Mqtt::on_mqtt_message_arrived(const std::string& topic, const std::string& payload)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 收到MQTT消息，主题: " << topic << ", 载荷长度: " << payload.length();
    
    try {
        if (topic.find(m_auth_topic) != std::string::npos) {
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 处理认证响应消息";
            on_auth_arrived(payload);
        } else {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 收到未知主题的MQTT消息: " << topic;
            return;
        }
    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 处理MQTT消息异常: " << e.what();
    }
}

// Handle auth messages
void Moonraker_Mqtt::on_auth_arrived(const std::string& payload) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 处理认证到达消息";
    
    try {
        json body = json::parse(payload);

        if(!body.count("id")){
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 认证响应缺少ID字段";
            return;
        }

        int64_t id = body["id"].get<int64_t>();
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 认证响应ID: " << id;

        auto cb = get_request_callback(id);
        delete_response_target(id);

        if (!cb) {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 未找到对应的认证回调";
            return;
        }

        json res = body["result"];
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 认证响应处理完成";
        cb(res);

    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 处理认证响应异常: " << e.what();
    }
}

// Handle response messages
void Moonraker_Mqtt::on_response_arrived(const std::string& payload)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 处理响应到达消息";
    
    try {
        json body = json::parse(payload);

        if (!body.count("id")) {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 响应消息缺少ID字段";
            return;
        }

        int64_t id = -1;
        id         = body["id"].get<int64_t>();
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 响应ID: " << id;

        auto cb = get_request_callback(id);
        delete_response_target(id);

        if (!cb) {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 未找到对应的响应回调，ID: " << id;
            return;
        }

        json res;

        if (/*id.find("mqtt") != std::string::npos*/ id == 20252025) {
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 特殊MQTT请求，直接返回原始响应";
            cb(body);
        } else {
            if (!body.count("result")) {
                if (body.count("error")) {
                    json error   = body["error"];
                    res["error"] = error;
                    BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 响应包含错误信息";
                }
            } else {
                res["data"] = body["result"];
                BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 响应包含结果数据";
                // test: 如果有method，则进行传递
            }
            res["method"] = "";
            if (body.count("method")) {
                res["method"] = body["method"];
                BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 响应包含方法: " << body["method"].get<std::string>();
            }
            cb(res);
        }
        

    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 处理响应消息异常: " << e.what();
    }
}

// Handle status update messages
void Moonraker_Mqtt::on_status_arrived(const std::string& payload)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 处理状态更新消息";
    
    try {
        json body = json::parse(payload);

        json data;
        if (body.count("params")) {
            data["data"] = body["params"];
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 状态更新包含参数";
        } else if (body.count("result") && body["result"].count("status")) {
            data["data"] = body["result"]["status"];
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 状态更新包含结果状态";
        } else {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 状态更新消息格式无效";
            return;
        }

        // test: 如果有method，则进行传递
        data["method"] = "";
        if (body.count("method")){
            data["method"] = body["method"];
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 状态更新包含方法: " << body["method"].get<std::string>();
        }

        if (!m_status_cb) {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 状态回调未设置";
            return;
        }

        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 调用状态回调";
        m_status_cb(data);

    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 处理状态更新异常: " << e.what();
    }
}

// Handle notification messages
void Moonraker_Mqtt::on_notification_arrived(const std::string& payload)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 处理通知消息，载荷长度: " << payload.length();
    
    try {
        // TODO: 实现通知消息处理逻辑
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 通知消息处理功能待实现";
    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] 处理通知消息异常: " << e.what();
    }
}

// 添加一个辅助函数来等待SN
bool Moonraker_Mqtt::wait_for_sn(int timeout_seconds)
{
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 等待SN，超时时间: " << timeout_seconds << "秒";
    
    using namespace std::chrono;
    auto start = steady_clock::now();

    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_sn_mtx);
            if (!m_sn.empty()) {
                BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] SN已获取: " << m_sn;
                return true;
            }
        }

        auto now = steady_clock::now();
        if (duration_cast<seconds>(now - start).count() >= timeout_seconds) {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] 等待SN超时";
            return false;
        }

        std::this_thread::sleep_for(milliseconds(100));
    }
}

void Moonraker_Mqtt::set_connection_lost(std::function<void()> callback) {
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 设置连接丢失回调";
    
    if (m_mqtt_client_tls)
        m_mqtt_client_tls->SetConnectionFailureCallback(callback);
}

std::string Moonraker_Mqtt::get_sn() {
    std::string res = "";
    m_sn_mtx.lock();
    res = m_sn;
    m_sn_mtx.unlock();

    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] 获取SN: " << res;
    return res;
}

} // namespace Slic3r
