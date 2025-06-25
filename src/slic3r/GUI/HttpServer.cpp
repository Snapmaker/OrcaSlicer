#include "HttpServer.hpp"
#include <boost/log/trivial.hpp>
#include "GUI_App.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

namespace Slic3r { namespace GUI {

std::string url_get_param(const std::string& url, const std::string& key)
{
    size_t start = url.find(key);
    if (start == std::string::npos)
        return "";
    size_t eq = url.find('=', start);
    if (eq == std::string::npos)
        return "";
    std::string key_str = url.substr(start, eq - start);
    if (key_str != key)
        return "";
    start += key.size() + 1;
    size_t end = url.find('&', start);
    if (end == std::string::npos)
        end = url.length(); // Last param
    std::string result = url.substr(start, end - start);
    return result;
}

void session::start() { read_first_line(); }

void session::stop()
{
    boost::system::error_code ignored_ec;
    socket.shutdown(boost::asio::socket_base::shutdown_both, ignored_ec);
    socket.close(ignored_ec);
}

void session::read_first_line()
{
    auto self(shared_from_this());

    async_read_until(socket, buff, '\r', [this, self](const boost::beast::error_code& e, std::size_t s) {
        if (!e) {
            std::string  line, ignore;
            std::istream stream{&buff};
            std::getline(stream, line, '\r');
            std::getline(stream, ignore, '\n');
            headers.on_read_request_line(line);
            read_next_line();
        } else if (e != boost::asio::error::operation_aborted) {
            server.stop(self);
        }
    });
}

void session::read_body()
{
    auto self(shared_from_this());

    int                                nbuffer = 1000;
    std::shared_ptr<std::vector<char>> bufptr  = std::make_shared<std::vector<char>>(nbuffer);
    async_read(socket, boost::asio::buffer(*bufptr, nbuffer),
               [this, self](const boost::beast::error_code& e, std::size_t s) { server.stop(self); });
}

void session::read_next_line()
{
    auto self(shared_from_this());

    if (headers.method == "OPTIONS") {
        // 构造OPTIONS响应（允许跨域）
        std::stringstream ssOut;
        ssOut << "HTTP/1.1 200 OK\r\n";
        ssOut << "Access-Control-Allow-Origin: *\r\n";                            // 允许所有源
        ssOut << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";          // 允许的方法
        ssOut << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"; // 允许的请求头
        ssOut << "Content-Length: 0\r\n";                                         // 无响应体
        ssOut << "\r\n";                                                          // 头和主体之间的空行（必须）

        // 异步发送响应
        async_write(socket, boost::asio::buffer(ssOut.str()), [this, self](const boost::beast::error_code& e, std::size_t s) {
            std::cout << "OPTIONS预检请求已处理" << std::endl;
            server.stop(self); // 关闭连接
        });
        return; // 提前返回，避免后续逻辑
    }

    async_read_until(socket, buff, '\r', [this, self](const boost::beast::error_code& e, std::size_t s) {
        if (!e) {
            std::string  line, ignore;
            std::istream stream{&buff};
            std::getline(stream, line, '\r');
            std::getline(stream, ignore, '\n');
            headers.on_read_header(line);

            if (line.length() == 0) {
                if (headers.content_length() == 0) {
                    std::cout << "Request received: " << headers.method << " " << headers.get_url();
                    if (headers.method == "OPTIONS") {
                        // Ignore http OPTIONS
                        server.stop(self);
                        return;
                    }

                    const std::string url_str = Http::url_decode(headers.get_url());
                    const auto        resp    = server.server.m_request_handler(url_str);
                    std::stringstream ssOut;
                    resp->write_response(ssOut);
                    std::shared_ptr<std::string> str = std::make_shared<std::string>(ssOut.str());
                    async_write(socket, boost::asio::buffer(str->c_str(), str->length()),
                                [this, self, str](const boost::beast::error_code& e, std::size_t s) {
                                    std::cout << "done" << std::endl;
                                    server.stop(self);
                                });
                } else {
                    read_body();
                }
            } else {
                read_next_line();
            }
        } else if (e != boost::asio::error::operation_aborted) {
            server.stop(self);
        }
    });
}

void HttpServer::IOServer::do_accept()
{
    acceptor.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
        if (!acceptor.is_open()) {
            return;
        }

        if (!ec) {
            const auto ss = std::make_shared<session>(*this, std::move(socket));
            start(ss);
        }

        do_accept();
    });
}

void HttpServer::IOServer::start(std::shared_ptr<session> session)
{
    sessions.insert(session);
    session->start();
}

void HttpServer::IOServer::stop(std::shared_ptr<session> session)
{
    sessions.erase(session);
    session->stop();
}

void HttpServer::IOServer::stop_all()
{
    for (auto s : sessions) {
        s->stop();
    }
    sessions.clear();
}

HttpServer::IOServer::IOServer(HttpServer& server) : server(server), acceptor(io_service)
{
    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), server.port);
    acceptor.open(endpoint.protocol());
    acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint);
}

HttpServer::HttpServer(boost::asio::ip::port_type port) : port(port) {}

bool HttpServer::is_port_available(boost::asio::ip::port_type port)
{
    try {
        boost::asio::io_service        io_service;
        boost::asio::ip::tcp::acceptor acceptor(io_service);
        boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), port);

        acceptor.open(endpoint.protocol());
        acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.close();
        return true;
    } catch (const boost::system::system_error&) {
        return false;
    }
}

boost::asio::ip::port_type HttpServer::find_available_port(boost::asio::ip::port_type start_port)
{
    // 尝试从起始端口开始查找可用端口
    for (boost::asio::ip::port_type p = start_port; p < start_port + 1000; ++p) {
        if (is_port_available(p)) {
            return p;
        }
    }
    throw std::runtime_error("No available ports found");
}

void HttpServer::start()
{
    BOOST_LOG_TRIVIAL(info) << "start_http_service...";

    try {
        // 如果指定端口不可用，查找下一个可用端口
        if (!is_port_available(port)) {
            auto new_port = find_available_port(port + 1);
            BOOST_LOG_TRIVIAL(info) << "Original port " << port << " is in use, switching to port " << new_port;
            port = new_port;
        }

        start_http_server    = true;
        m_http_server_thread = create_thread([this] {
            try {
                set_current_thread_name("http_server");
                server_ = std::make_unique<IOServer>(*this);
                server_->acceptor.listen();
                server_->do_accept();
                server_->io_service.run();
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "HTTP server error: " << e.what();
                start_http_server = false;
            }
        });

        // 等待服务器启动
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!start_http_server) {
            throw std::runtime_error("Failed to start HTTP server");
        }

        BOOST_LOG_TRIVIAL(info) << "HTTP server started successfully on port " << port;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to start HTTP server: " << e.what();
        start_http_server = false;
        throw;
    }
}

void HttpServer::stop()
{
    start_http_server = false;
    if (server_) {
        server_->acceptor.close();
        server_->stop_all();
        server_->io_service.stop();
    }
    if (m_http_server_thread.joinable())
        m_http_server_thread.join();
    server_.reset();
}

void HttpServer::set_request_handler(const std::function<std::shared_ptr<Response>(const std::string&)>& request_handler)
{
    this->m_request_handler = request_handler;
}

std::shared_ptr<HttpServer::Response> HttpServer::bbl_auth_handle_request(const std::string& url)
{
    BOOST_LOG_TRIVIAL(info) << "thirdparty_login: get_response";

    if (boost::contains(url, "access_token")) {
        std::string   redirect_url           = url_get_param(url, "redirect_url");
        std::string   access_token           = url_get_param(url, "access_token");
        std::string   refresh_token          = url_get_param(url, "refresh_token");
        std::string   expires_in_str         = url_get_param(url, "expires_in");
        std::string   refresh_expires_in_str = url_get_param(url, "refresh_expires_in");
        NetworkAgent* agent                  = wxGetApp().getAgent();

        unsigned int http_code;
        std::string  http_body;
        int          result = agent->get_my_profile(access_token, &http_code, &http_body);
        if (result == 0) {
            std::string user_id;
            std::string user_name;
            std::string user_account;
            std::string user_avatar;
            try {
                json user_j = json::parse(http_body);
                if (user_j.contains("uidStr"))
                    user_id = user_j["uidStr"].get<std::string>();
                if (user_j.contains("name"))
                    user_name = user_j["name"].get<std::string>();
                if (user_j.contains("avatar"))
                    user_avatar = user_j["avatar"].get<std::string>();
                if (user_j.contains("account"))
                    user_account = user_j["account"].get<std::string>();
            } catch (...) {
                ;
            }
            json j;
            j["data"]["refresh_token"]      = refresh_token;
            j["data"]["token"]              = access_token;
            j["data"]["expires_in"]         = expires_in_str;
            j["data"]["refresh_expires_in"] = refresh_expires_in_str;
            j["data"]["user"]["uid"]        = user_id;
            j["data"]["user"]["name"]       = user_name;
            j["data"]["user"]["account"]    = user_account;
            j["data"]["user"]["avatar"]     = user_avatar;
            agent->change_user(j.dump());
            if (agent->is_user_login()) {
                wxGetApp().request_user_login(1);
            }
            GUI::wxGetApp().CallAfter([] { wxGetApp().ShowUserLogin(false); });
            std::string location_str = (boost::format("%1%?result=success") % redirect_url).str();
            return std::make_shared<ResponseRedirect>(location_str);
        } else {
            std::string error_str    = "get_user_profile_error_" + std::to_string(result);
            std::string location_str = (boost::format("%1%?result=fail&error=%2%") % redirect_url % error_str).str();
            return std::make_shared<ResponseRedirect>(location_str);
        }
    } else {
        return std::make_shared<ResponseNotFound>();
    }
}

std::shared_ptr<HttpServer::Response> HttpServer::web_server_handle_request(const std::string& url)
{
    BOOST_LOG_TRIVIAL(info) << "Handling file request for URL: " << url;

    std::string file_path = map_url_to_file_path(url);

    BOOST_LOG_TRIVIAL(info) << "Handling file_path request for URL: " << file_path;
    return std::make_shared<ResponseFile>(file_path);
}

std::string HttpServer::map_url_to_file_path(const std::string& url)
{
    if (url.find("..") != std::string::npos) {
        return "";
    }

    std::string trimmed_url = url;

    size_t question_mark = trimmed_url.find('?');
    if (question_mark != std::string::npos) {
        trimmed_url = trimmed_url.substr(0, question_mark);
    }

    if (trimmed_url == "/") {
        trimmed_url = "/flutter_web/index.html"; // 默认首页
    } else if (trimmed_url.substr(0, 11) == "/localfile/") {
        auto real_path = trimmed_url.substr(11);
        return std::string(wxString(real_path).ToUTF8());
    }
    auto data_web_path = boost::filesystem::path(data_dir()) / "web";
    if (!boost::filesystem::exists(data_web_path / "flutter_web")) {
        auto source_path = boost::filesystem::path(resources_dir()) / "web" / "flutter_web";
        auto target_path = data_web_path / "flutter_web";
        copy_directory_recursively(source_path, target_path);
    }

    if (trimmed_url.find("flutter_web") == std::string::npos) {
        return std::string(wxString(resources_dir() + trimmed_url).ToUTF8());
    } else {
        return std::string(wxString(data_dir() + trimmed_url).ToUTF8());
    }
}

void HttpServer::ResponseRedirect::write_response(std::stringstream& ssOut)
{
    const std::string sHTML          = "<html><body><p>redirect to url </p></body></html>";
    size_t            content_length = sHTML.size(); // 字节长度（与字符数相同，因无多字节字符）

    ssOut << "HTTP/1.1 302 Found\r\n";
    ssOut << "Location: " << location_str << "\r\n";
    ssOut << "Content-Type: text/html\r\n";
    ssOut << "Content-Length: " << content_length << "\r\n"; // 正确计算长度
    ssOut << "Access-Control-Allow-Origin: *\r\n";           // CORS头
    ssOut << "\r\n";                                         // 头和主体之间的空行（必须）
    ssOut << sHTML;                                          // 响应体（长度必须匹配）
}

void HttpServer::ResponseNotFound::write_response(std::stringstream& ssOut)
{
    const std::string sHTML          = "<html><body><h1>404 Not Found</h1><p>There's nothing here.</p></body></html>";
    size_t            content_length = sHTML.size(); // 字节长度

    ssOut << "HTTP/1.1 404 Not Found\r\n";
    ssOut << "Content-Type: text/html\r\n";
    ssOut << "Content-Length: " << content_length << "\r\n"; // 正确计算长度
    ssOut << "Access-Control-Allow-Origin: *\r\n";           // CORS头
    ssOut << "\r\n";                                         // 头和主体之间的空行（必须）
    ssOut << sHTML;                                          // 响应体（长度必须匹配）
}

void HttpServer::ResponseFile::write_response(std::stringstream& ssOut)
{
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        ResponseNotFound notFoundResponse;
        notFoundResponse.write_response(ssOut);
        return;
    }

    // 读取文件内容并计算长度（关键：使用字节长度）
    std::ostringstream fileStream;
    fileStream << file.rdbuf();
    std::string fileContent    = fileStream.str();
    size_t      content_length = fileContent.size(); // 字节长度，非字符数

    // 确定Content-Type（保持原有逻辑）
    std::string content_type = "application/octet-stream";
    if (ends_with(file_path, ".html"))
        content_type = "text/html";
    else if (ends_with(file_path, ".css"))
        content_type = "text/css";
    else if (ends_with(file_path, ".js"))
        content_type = "text/javascript";
    else if (ends_with(file_path, ".png"))
        content_type = "image/png";
    else if (ends_with(file_path, ".jpg"))
        content_type = "image/jpeg";
    else if (ends_with(file_path, ".gif"))
        content_type = "image/gif";
    else if (ends_with(file_path, ".svg"))
        content_type = "image/svg+xml";
    else if (ends_with(file_path, ".ttf"))
        content_type = "application/x-font-ttf";
    else if (ends_with(file_path, ".json"))
        content_type = "application/json";

    // 构造响应头（严格使用\r\n，头结束后空行）
    ssOut << "HTTP/1.1 200 OK\r\n";
    ssOut << "Content-Type: " << content_type << "\r\n";
    ssOut << "Content-Length: " << content_length << "\r\n"; // 必须与实际内容长度一致
    ssOut << "Access-Control-Allow-Origin: *\r\n";           // CORS头
    ssOut << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    ssOut << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
    ssOut << "\r\n";      // 头和主体之间的空行（必须）
    ssOut << fileContent; // 响应体（长度必须与Content-Length一致）
}

}} // namespace Slic3r::GUI