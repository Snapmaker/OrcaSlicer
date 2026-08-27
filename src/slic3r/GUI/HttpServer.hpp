#ifndef slic3r_Http_App_hpp_
#define slic3r_Http_App_hpp_

#include <iostream>
#include <functional>
#include <mutex>
#include <stack>
#include <cstdint>
#include <map>
#include <sstream>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <string>
#include <set>
#include <memory>

#define LOCALHOST_PORT      13618
#define PAGE_HTTP_PORT      13619
#define LOCALHOST_URL       "http://127.0.0.1:"
#define WCP_DOWNLOAD_PREFIX "/wcp_download/"

namespace Slic3r { namespace GUI {

class session;

struct FileProgress
{
    std::string    url;
    std::uintmax_t bytes_sent;
    std::uintmax_t total_bytes;
    bool           failed;
};

class http_headers
{
    std::string method;
    std::string url;
    std::string version;

    std::map<std::string, std::string> headers;

    friend class session;
public:
    std::string get_url() { return url; }

    // Case-insensitive lookup with surrounding whitespace trimmed.
    std::string get_header(const std::string& name) const;

    int content_length()
    {
        auto request = headers.find("content-length");
        if (request != headers.end()) {
            std::stringstream ssLength(request->second);
            int               content_length;
            ssLength >> content_length;
            return content_length;
        }
        return 0;
    }

    void on_read_header(std::string line)
    {
        // std::cout << "header: " << line << std::endl;

        std::stringstream ssHeader(line);
        std::string       headerName;
        std::getline(ssHeader, headerName, ':');

        std::string value;
        std::getline(ssHeader, value);
        headers[headerName] = value;
    }

    void on_read_request_line(std::string line)
    {
        std::stringstream ssRequestLine(line);
        ssRequestLine >> method;
        ssRequestLine >> url;
        ssRequestLine >> version;

        std::cout << "request for resource: " << url << std::endl;
    }
};

class HttpServer
{
    boost::asio::ip::port_type port;

    // 添加辅助函数声明
    static bool is_port_available(boost::asio::ip::port_type port);
    boost::asio::ip::port_type find_available_port(boost::asio::ip::port_type start_port);

public:
    using FileProgressCallback = std::function<void(const FileProgress&)>;
    using StreamCompletion     = std::function<void(const boost::beast::error_code&, std::size_t)>;

    class Response
    {
    public:
        virtual ~Response()                                   = default;
        virtual void write_response(std::stringstream& ssOut) = 0;
        virtual bool write_response(boost::asio::ip::tcp::socket& socket, const FileProgressCallback& progress_callback,
                                    StreamCompletion completion);

        // Forwarded conditional request headers, used by ResponseFile to implement
        // 304 Not Modified revalidation (If-Modified-Since / If-None-Match).
        void set_conditional_headers(const std::string& if_modified_since, const std::string& if_none_match)
        {
            m_if_modified_since = if_modified_since;
            m_if_none_match     = if_none_match;
        }

    protected:
        std::string m_if_modified_since;
        std::string m_if_none_match;
    };

    class ResponseNotFound : public Response
    {
    public:
        ~ResponseNotFound() override = default;
        void write_response(std::stringstream& ssOut) override;
    };

    class ResponseRedirect : public Response
    {
        const std::string location_str;

    public:
        ResponseRedirect(const std::string& location) : location_str(location) {}
        ~ResponseRedirect() override = default;
        void write_response(std::stringstream& ssOut) override;
    };

    class ResponseFile : public Response
    {
        std::string file_path;
        bool        m_native_path = false;  // true if path is already in system encoding, skip UTF-8 conversion

    public:
        ResponseFile(const std::string& path, bool native_path = false) : file_path(path), m_native_path(native_path) {}
        ~ResponseFile() override = default;

        void write_response(std::stringstream& ssOut) override;
        bool write_response(boost::asio::ip::tcp::socket& socket, const FileProgressCallback& progress_callback,
                            StreamCompletion completion) override;

        bool ends_with(const std::string& str, const std::string& suffix)
        {
            if (str.length() >= suffix.length()) {
                return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
            }
            return false;
        };
    };

    HttpServer(boost::asio::ip::port_type port = LOCALHOST_PORT);
    ~HttpServer();  // 添加析构函数

    boost::thread m_http_server_thread;
    bool          start_http_server = false;
    
    // 添加自动健康检查相关成员
    boost::thread m_health_check_thread;
    bool          m_health_check_enabled = false;
    int           m_health_check_interval = 5000; // 5秒检查一次
    mutable std::mutex m_health_check_mutex;  // 保护健康检查相关变量
    std::condition_variable m_health_check_cv;  // 条件变量，用于精确控制间隔
    bool          m_restart_requested = false;  // 添加重启请求标志
    
    // 添加重启检查线程
    boost::thread m_restart_check_thread;
    bool          m_restart_check_enabled = false;

    bool is_started() { return start_http_server; }
    void start();
    void stop();
    void restart();  // 添加重启方法
    bool is_healthy();  // 添加健康检查方法
    void start_health_check();  // 启动健康检查
    void stop_health_check();   // 停止健康检查
    void set_health_check_interval(int interval_ms);  // 设置健康检查间隔
    int get_health_check_interval() const;  // 获取健康检查间隔
    bool is_health_check_enabled() const;   // 检查健康检查是否启用
    bool is_restart_requested() const;      // 检查是否有重启请求
    void start_restart_check();  // 启动重启检查
    void stop_restart_check();   // 停止重启检查
    void simulate_crash();       // 模拟服务器崩溃，用于测试重启机制
    void set_request_handler(const std::function<std::shared_ptr<Response>(const std::string&)>& m_request_handler);
    void set_file_progress_callback(FileProgressCallback callback);
    void setPort(boost::asio::ip::port_type new_port) { 
        if (!start_http_server) {  // 只有在服务器未启动时才允许修改端口
            port = new_port; 
        }
    }

    boost::asio::ip::port_type get_port() const { return port; }

    static std::string map_url_to_file_path(const std::string& url);

    static std::shared_ptr<Response> bbl_auth_handle_request(const std::string& url);

    static std::shared_ptr<Response> web_server_handle_request(const std::string& url);

private:
    class IOServer
    {
    public:
        HttpServer&                        server;
        boost::asio::io_service           io_service;
        boost::asio::ip::tcp::acceptor    acceptor;
        std::set<std::shared_ptr<session>> sessions;

        // 只声明构造函数，不在头文件中定义
        IOServer(HttpServer& server);

        void do_accept();
        void start(std::shared_ptr<session> session);
        void stop(std::shared_ptr<session> session);
        void stop_all();
    };
    friend class session;

    std::unique_ptr<IOServer> server_{nullptr};

    std::function<std::shared_ptr<Response>(const std::string&)> m_request_handler{&HttpServer::bbl_auth_handle_request};
    FileProgressCallback m_file_progress_callback;
    mutable std::mutex  m_file_progress_mutex;

    void notify_file_progress(const FileProgress& progress) const;
};

class session : public std::enable_shared_from_this<session>
{
    HttpServer::IOServer& server;
    boost::asio::ip::tcp::socket socket;

    boost::asio::streambuf buff;
    http_headers headers;

    void read_first_line();
    void read_next_line();
    void read_body();

public:
    session(HttpServer::IOServer& server, boost::asio::ip::tcp::socket socket) : server(server), socket(std::move(socket)) {}

    void start();
    void stop();
};

std::string url_get_param(const std::string& url, const std::string& key);

}};

#endif
