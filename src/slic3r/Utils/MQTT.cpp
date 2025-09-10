#include "MQTT.hpp"
#include <thread>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <fstream>

// Constructor: Initialize MQTT client with server address and client ID
// @param server_address: Address of the MQTT broker
// @param client_id: Unique identifier for this client
// @param clean_session: Whether to start with a clean session
MqttClient::MqttClient(const std::string& server_address, const std::string& client_id, const std::string& username, const std::string& password,  bool clean_session)
    : server_address_(server_address)
    , client_id_(client_id)
    , client_(std::make_unique<mqtt::async_client>(server_address_, client_id_))
    , connOpts_()
    , subListener_("Subscription")
    , connected_(false)              // std::atomic<bool> 可以从 bool 直接构造
    , is_reconnecting(false)
    , connection_failure_callback_(nullptr)
    , pending_reconnect_checks{0}  // 添加计数器
{
    BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] 初始化MQTT连接 server_address: " << server_address << ", client_id: " << client_id;

    // Configure connection options
    // 写死false
    connOpts_.set_clean_session(false);
    connOpts_.set_keep_alive_interval(30);
    connOpts_.set_connect_timeout(10);
    // 设置自动重连参数
    connOpts_.set_automatic_reconnect(std::chrono::seconds(2), std::chrono::seconds(30));
    client_->set_callback(*this);

    // 设置认证信息
    if (!username.empty()) {
        connOpts_.set_user_name(username);
        if (!password.empty()) {
            connOpts_.set_password(password);
        }
    }
}

// SSL/TLS构造函数实现
MqttClient::MqttClient(const std::string& server_address, 
                      const std::string& client_id,
                      const std::string& ca_content,        
                      const std::string& cert_content,
                      const std::string& key_content,
                      const std::string& username,
                      const std::string& password,
                      bool clean_session)
    : MqttClient(server_address, client_id, username, password, clean_session)
{
    BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] 初始化MQTT SSL连接 server_address: " << server_address << ", client_id: " << client_id
                            << ", ca_content: " << ca_content << ", cert_content: " << cert_content << ", username: " << username
                            << ", password: " << password;
    
    try {
        // 创建临时文件
        boost::filesystem::path temp_dir = boost::filesystem::temp_directory_path();

        // CA证书临时文件
        boost::filesystem::path ca_path = temp_dir / ("ca_" + client_id + std::to_string(int64_t(this)) + ".pem");
        if (!ca_content.empty()) {
            std::ofstream ca_file(ca_path.string());
            ca_file << ca_content;
            ca_file.close();
            BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] CA证书已写入临时文件: " << ca_path;
        }

        // 客户端证书临时文件
        boost::filesystem::path cert_path = temp_dir / ("cert_" + client_id + std::to_string(int64_t(this)) + ".pem");
        if (!cert_content.empty()) {
            std::ofstream cert_file(cert_path.string());
            cert_file << cert_content;
            cert_file.close();
            BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] 客户端证书已写入临时文件: " << cert_path;
        }

        // 私钥临时文件
        boost::filesystem::path key_path = temp_dir / ("key_" + client_id + std::to_string(int64_t(this)) + ".pem");
        if (!key_content.empty()) {
            std::ofstream key_file(key_path.string());
            key_file << key_content;
            key_file.close();
            BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] 私钥已写入临时文件: " << key_path;
        }

        // 配置SSL/TLS
        mqtt::ssl_options ssl_opts;
        
        // 设置SSL验证选项
        ssl_opts.set_verify(false);
        ssl_opts.set_enable_server_cert_auth(true);
        
        // 设置证书文件路径
        if (!ca_content.empty()) {
            ssl_opts.set_trust_store(ca_path.string());
        }
        if (!cert_content.empty() && !key_content.empty()) {
            ssl_opts.set_key_store(cert_path.string());
            ssl_opts.set_private_key(key_path.string());
        }

        ssl_opts.set_ssl_version(MQTT_SSL_VERSION_TLS_1_2);
        
        // 设置SSL选项
        connOpts_.set_ssl(ssl_opts);
        
        // 保存临时文件路径以便后续清理
        temp_ca_path_ = ca_path;
        temp_cert_path_ = cert_path;
        temp_key_path_ = key_path;

    } catch (const std::exception& e) {
        // 清理临时文件
        cleanup_temp_files();
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT SSL初始化失败: " << e.what();
        throw;
    }
}

// Establish connection to the MQTT broker
// @return: true if connection successful, false otherwise
bool MqttClient::Connect(std::string& msg)
{
    if (connected_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] " << client_id_ 
                                   << " Already connected to MQTT server " << server_address_;
        msg = "success";
        return true;
    }

    try {
        // 添加详细的连接参数日志
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] 正在连接MQTT服务器: " << server_address_;
        
        // 如果是SSL连接，添加SSL配置信息日志
        try {
            auto ssl_opts = connOpts_.get_ssl_options();
            BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] SSL配置信息:"
                << "\n - 服务器地址: " << server_address_
                << "\n - 客户端ID: " << client_id_
                << "\n - CA证书长度: " << (ssl_opts.get_trust_store().empty() ? 0 : ssl_opts.get_trust_store().length())
                << "\n - 客户端证书长度: " << (ssl_opts.get_key_store().empty() ? 0 : ssl_opts.get_key_store().length())
                << "\n - 私钥长度: " << (ssl_opts.get_private_key().empty() ? 0 : ssl_opts.get_private_key().length());
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] 非SSL连接";
        }

        const char* context = "connection";
        mqtt::token_ptr conntok = client_->connect(connOpts_, (void*)(context), *this);
        
        if (!conntok->wait_for(std::chrono::seconds(20))) {
            auto rc = conntok->get_return_code();
            auto reason = conntok->get_reason_code();
            
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Connection timeout. Return code: " << rc 
                                    << ", Reason code: " << static_cast<int>(reason)
                                    << ", Server: " << server_address_;

            if (rc == MQTTASYNC_FAILURE) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Connection failed - MQTTASYNC_FAILURE";
                msg = "MQTTASYNC_FAILURE";
            } else if (rc == MQTTASYNC_DISCONNECTED) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Connection failed - MQTTASYNC_DISCONNECTED";
                msg = "MQTTASYNC_DISCONNECTED";
            }
            
            connected_.store(false, std::memory_order_release);
            return false;
        }

        if (!client_->is_connected()) {
            auto rc = conntok->get_return_code();
            auto reason = conntok->get_reason_code();
            
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Connection failed. Return code: " << rc 
                                    << ", Reason code: " << static_cast<int>(reason)
                                    << ", Server: " << server_address_;

            msg = "connetion failed, Return code: " + std::to_string(rc) + " Reason code: " + std::to_string(reason);
            
            connected_.store(false, std::memory_order_release);
            return false;
        }

        connected_.store(true, std::memory_order_release);
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] Successfully connected to MQTT server";
        msg = "success";
        return true;
    }
    catch (const mqtt::exception& exc) {
        connected_.store(false, std::memory_order_release);
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT exception during connect: " << exc.what()
                                << ", Return code: " << exc.get_return_code()
                                << ", Message: " << exc.get_message();

        msg = std::string(exc.what()) + ";" + exc.get_reason_code_str() + ";" + exc.get_message(); 

        return false;
    }
    catch (const std::exception& e) {
        connected_.store(false, std::memory_order_release);
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] General exception during connect: " << e.what();
        msg = std::string(e.what());
        return false;
    }
}

// Disconnect from the MQTT broker
// @return: true if disconnection successful, false otherwise
bool MqttClient::Disconnect(std::string& msg)
{
    if (!connected_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] MQTT client already disconnected";
        msg = "success";
        return true;  // 已经断开就返回成功
    }

    try {
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] Disconnecting from MQTT server...";
        
        // 清理连接
        try {
            auto disctok = client_->disconnect();
            // 设置5秒超时
            if (!disctok->wait_for(std::chrono::seconds(5))) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT disconnect timeout";
            }
        } catch (const mqtt::exception& exc) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Error during disconnect: " << exc.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Unexpected error during disconnect";
        }

        connected_.store(false, std::memory_order_release);
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] Successfully disconnected from MQTT server";
        msg = "success";
        return true;
    }
    catch (const mqtt::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Error disconnecting from MQTT server: " << exc.what();
        // 即使发生异常，也要标记为断开状态
        connected_.store(false, std::memory_order_release);
        msg = "success";
        return true;
    }
    catch (const std::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Unexpected error while disconnecting: " << exc.what();
        connected_.store(false, std::memory_order_release);
        msg = "success";
        return true;
    }
}

// Subscribe to a specific MQTT topic
// @param topic: The topic to subscribe to
// @param qos: Quality of Service level (0, 1, or 2)
// @return: true if subscription successful, false otherwise
bool MqttClient::Subscribe(const std::string& topic, int qos, std::string& msg)
{
    if (!CheckConnected()) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Cannot subscribe: client not connected";

        msg = "client not connected";
        return false;
    }

    try {
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] Subscribing to MQTT topic '" << topic << "' with QoS " << qos;
        mqtt::token_ptr subtok = client_->subscribe(topic, qos, nullptr, subListener_);
        if (!subtok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Subscribe timeout for topic: " << topic;
            msg = "subscribe timeout";
            return false;
        }
        add_topic_to_resubscribe(topic, qos);
        msg = "success";
        return true;
    } catch (const mqtt::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Error subscribing to topic '" << topic << "': " << exc.what();
        msg = "Error: " + std::string(exc.what());
        return false;
    }
}

// Unsubscribe from a specific MQTT topic
// @param topic: The topic to unsubscribe from
// @return: true if unsubscription successful, false otherwise
bool MqttClient::Unsubscribe(const std::string& topic, std::string& msg)
{
    if (!CheckConnected()) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Cannot unsubscribe: client not connected";
        msg = "client not connect"; 
        return false;
    }

    try {
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] Unsubscribing from MQTT topic '" << topic << "'";
        mqtt::token_ptr unsubtok = client_->unsubscribe(topic);
        if (!unsubtok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Unsubscribe timeout for topic: " << topic;
            msg = "Unsubscribe timeout for topic";
            return false;
        }
        remove_topic_from_resubscribe(topic);
        msg = "success";
        return true;
    } catch (const mqtt::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Error unsubscribing from topic '" << topic << "': " << exc.what();
        msg = "Error unsubscribing from topic: " + std::string(exc.what());
        return false;
    }
}

// Publish a message to a specific MQTT topic
// @param topic: The topic to publish to
// @param payload: The message content
// @param qos: Quality of Service level (0, 1, or 2)
// @return: true if publish successful, false otherwise
bool MqttClient::Publish(const std::string& topic, const std::string& payload, int qos, std::string& msg)
{
    if (!CheckConnected()) {
        msg = "client not connect";
        return false;
    }

    mqtt::message_ptr pubmsg = mqtt::make_message(topic, payload);
    pubmsg->set_qos(qos);

    try {
        BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Publishing message to topic '" << topic << "' with QoS " << qos;
        mqtt::token_ptr pubtok = client_->publish(pubmsg);
        /*if (!pubtok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "Publish timeout for topic: " << topic;
            return false;
        }*/
        msg = "success";
        return true;
    } catch (const mqtt::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Error publishing to topic '" << topic << "': " << exc.what();
        msg = "error: " + std::string(exc.what());
        return false;
    }
}

// Set callback function for handling incoming messages
// @param callback: Function to be called when a message arrives
void MqttClient::SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload)> callback)
{
    message_callback1_ = nullptr;
    message_callback_ = callback;
}

void MqttClient::SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload, void* this_)> callback)
{
    message_callback_  = nullptr;
    message_callback1_ = callback;
}

// Check if the client is currently connected
// @return: true if connected, false otherwise
bool MqttClient::CheckConnected()
{
    if (!connected_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT client is not connected to server";
        return false;
    }

    // 检查客户端指针是否有效
    if (!client_) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT client pointer is null";
        connected_.store(false, std::memory_order_release);
        return false;
    }

    // 带超时的连接状态检查
    auto check_future = std::async(std::launch::async, [this]() {
        try {
            return client_->is_connected();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Exception during connection check: " << e.what();
            return false;
        }
    });
    
    if (check_future.wait_for(std::chrono::seconds(3)) == std::future_status::timeout) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Connection status check timeout";
        connected_.store(false, std::memory_order_release);
        return false;
    }

    try {
        if (!check_future.get()) {
            connected_.store(false, std::memory_order_release);
            return false;
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Exception getting connection status: " << e.what();
        connected_.store(false, std::memory_order_release);
        return false;
    }

    return true;
}

// Callback when connection is lost
// Implements automatic reconnection with retry logic
// @param cause: Reason for connection loss
void MqttClient::connection_lost(const std::string& cause)
{
    BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] MQTT connection lost";
    if (!cause.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] Cause: " << cause;
    }

    connected_.store(false, std::memory_order_release);
    
    if (!is_reconnecting.load(std::memory_order_acquire)) {
        is_reconnecting.store(true, std::memory_order_release);
        pending_reconnect_checks.fetch_add(1, std::memory_order_release);
        
        // 获取weak_ptr用于后续检查
        try {
            std::weak_ptr<MqttClient> weak_self = shared_from_this();

            // 启动异步检查任务
            std::thread([weak_self]() {
                // 等待20秒
                std::this_thread::sleep_for(std::chrono::seconds(20));

                if (auto self = weak_self.lock()) {
                    // 检查对象是否仍然有效
                    if (self->client_) {
                        int remaining = self->pending_reconnect_checks.fetch_sub(1, std::memory_order_acq_rel);

                        // 只有当这是最后一个检查线程时才执行检查
                        if (remaining <= 1) {
                            if (!self->connected_.load(std::memory_order_acquire)) {
                                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT connection not restored after 20 seconds";
                                std::string dc_msg = "";
                                self->Disconnect(dc_msg);
                                if (self->connection_failure_callback_) {
                                    self->connection_failure_callback_();
                                }
                            }
                            // 重置重连标志
                            self->is_reconnecting.store(false, std::memory_order_release);
                        }
                    } else {
                        // 客户端已被销毁，减少计数
                        self->pending_reconnect_checks.fetch_sub(1, std::memory_order_acq_rel);
                    }
                }
            }).detach();

            BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] Waiting for automatic reconnection...";
        }
        catch (std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Failed to start reconnection check: " << e.what();
            // 重置状态
            is_reconnecting.store(false, std::memory_order_release);
            pending_reconnect_checks.fetch_sub(1, std::memory_order_release);
        }
       
    } else {
        // 如果已经在重连中，只增加计数
        pending_reconnect_checks.fetch_add(1, std::memory_order_release);
    }
}

// Callback when a message arrives
// @param msg: Pointer to the received message
void MqttClient::message_arrived(mqtt::const_message_ptr msg)
{
    if (message_callback_) {
        message_callback_(msg->get_topic(), msg->to_string());
    }

    if (message_callback1_) {
        message_callback1_(msg->get_topic(), msg->to_string(), this);
    }
}

// Callback when message delivery is complete
// @param token: Delivery token containing message details
void MqttClient::delivery_complete(mqtt::delivery_token_ptr token)
{
    BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Message delivery complete for token: " << (token ? token->get_message_id() : -1);
}

// Callback for operation failure
// @param tok: Token containing operation details
void MqttClient::on_failure(const mqtt::token& tok)
{
    // 检查是否是连接操作失败
    if (tok.get_user_context() && 
        std::string(static_cast<const char*>(tok.get_user_context())) == "connection") {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT connection attempt failed";
        if (tok.get_reason_code() != 0) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Reason code: " << tok.get_reason_code();
        }
        // 这里可以触发连接失败的处理逻辑
        connected_.store(false, std::memory_order_release);
        
        //// 调用连接失败回调
        //if (connection_failure_callback_) {
        //    connection_failure_callback_();
        //}
    } else {
        // 其他操作失败的处理
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Operation failed for token: " << tok.get_message_id();
        if (tok.get_reason_code() != 0) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Reason code: " << tok.get_reason_code();
        }
    }
}

// Callback for operation success
// @param tok: Token containing operation details
void MqttClient::on_success(const mqtt::token& tok)
{
    BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Operation successful for token: " << tok.get_message_id();
    auto top = tok.get_topics();
    if (top && !top->empty()) {
        BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Token topic: '" << (*top)[0] << "'";
    }
}

// 添加连接成功的回调
void MqttClient::connected(const std::string& cause)
{
    BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] MQTT connection established";
    connected_.store(true, std::memory_order_release);
    is_reconnecting.store(false, std::memory_order_release);
    pending_reconnect_checks.store(0, std::memory_order_release);
    
    // 连接成功后重新订阅之前的主题
    /*resubscribe_topics();*/
}

void MqttClient::resubscribe_topics() {
    if (topics_to_resubscribe_.empty()) {
        return;
    }

    BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] Resubscribing to " << topics_to_resubscribe_.size() << " topics";
    
    for (const auto& topic_pair : topics_to_resubscribe_) {
        try {
            auto tok = client_->subscribe(topic_pair.first, topic_pair.second, nullptr,  subListener_);
            if (!tok->wait_for(std::chrono::seconds(5))) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Subscribe timeout for topic: " << topic_pair.first;
                continue;
            }
            if (!tok->is_complete() || tok->get_return_code() != 0) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Failed to resubscribe to topic: " << topic_pair.first;
            }
        } catch (const mqtt::exception& exc) {
            BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] Error resubscribing to topic " << topic_pair.first 
                                   << ": " << exc.what();
        }
    }
}

void MqttClient::add_topic_to_resubscribe(const std::string& topic, int qos) {
    topics_to_resubscribe_[topic] = qos;
    BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Added topic to resubscribe list: " << topic;
}

void MqttClient::remove_topic_from_resubscribe(const std::string& topic) {
    auto it = topics_to_resubscribe_.find(topic);
    if (it != topics_to_resubscribe_.end()) {
        topics_to_resubscribe_.erase(it);
        BOOST_LOG_TRIVIAL(debug) << "[MQTT_INFO] Removed topic from resubscribe list: " << topic;
    }
}

// 添加析构函数实现
MqttClient::~MqttClient()
{
    try {
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] 释放MQTT客户端资源...";
        
        // 先标记为断开状态，防止新的操作
        connected_.store(false, std::memory_order_release);
        is_reconnecting.store(false, std::memory_order_release);
        
        // 等待所有重连检查完成
        while (pending_reconnect_checks.load(std::memory_order_acquire) > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // 如果客户端仍然连接，先断开连接
        if (client_ && client_->is_connected()) {
            try {
                BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] 在析构函数中断开MQTT连接";
                
                // 使用阻塞方式断开，确保完全断开
                auto disctok = client_->disconnect();
                if (disctok) {
                    disctok->wait_for(std::chrono::seconds(5));
                }
            } 
            catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT断开连接时出错: " << e.what();
            }
        }
        
        // 清除所有订阅主题
        topics_to_resubscribe_.clear();
        
        // 确保回调不被调用
        message_callback_ = nullptr;
        message_callback1_           = nullptr;
        connection_failure_callback_ = nullptr;
        
        // 最后重置客户端指针
        client_.reset();
        
        // 清理临时文件
        cleanup_temp_files();
        
        BOOST_LOG_TRIVIAL(warning) << "[MQTT_INFO] MQTT客户端资源已释放";
    }
    catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT客户端析构时发生异常: " << e.what();
    }
    catch (...) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] MQTT客户端析构时发生未知异常";
    }
}

// 添加清理临时文件的方法
void MqttClient::cleanup_temp_files()
{
    try {
        if (!temp_ca_path_.empty() && boost::filesystem::exists(temp_ca_path_)) {
            boost::filesystem::remove(temp_ca_path_);
        }
        if (!temp_cert_path_.empty() && boost::filesystem::exists(temp_cert_path_)) {
            boost::filesystem::remove(temp_cert_path_);
        }
        if (!temp_key_path_.empty() && boost::filesystem::exists(temp_key_path_)) {
            boost::filesystem::remove(temp_key_path_);
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[MQTT_INFO] 清理临时证书文件失败: " << e.what();
    }
}
