#include "MQTT.hpp"
#include <thread>
#include <boost/log/trivial.hpp>

// Constructor: Initialize MQTT client with server address and client ID
// @param server_address: Address of the MQTT broker
// @param client_id: Unique identifier for this client
// @param clean_session: Whether to start with a clean session
MqttClient::MqttClient(const std::string& server_address, const std::string& client_id, bool clean_session)
    : server_address_(server_address)
    , client_id_(client_id)
    , client_(std::make_unique<mqtt::async_client>(server_address_, client_id_))
    , connOpts_()
    , subListener_("Subscription")
    , connected_(false)              // std::atomic<bool> 可以从 bool 直接构造
    , is_reconnecting(false)
    , connection_failure_callback_(nullptr)
{
    // Configure connection options
    // 写死false
    connOpts_.set_clean_session(false);
    connOpts_.set_keep_alive_interval(30);
    connOpts_.set_connect_timeout(10);
    // 设置自动重连参数
    connOpts_.set_automatic_reconnect(std::chrono::seconds(2), std::chrono::seconds(30));
    client_->set_callback(*this);
}

// Establish connection to the MQTT broker
// @return: true if connection successful, false otherwise
bool MqttClient::Connect()
{
    if (connected_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(warning) << "Already connected to MQTT server";
        return true;
    }

    try {
        BOOST_LOG_TRIVIAL(info) << "Connecting to the MQTT server: " << server_address_;
        
        // 添加用户上下文来标识这是连接操作
        const char* context             = "connection";
        mqtt::token_ptr conntok = client_->connect(connOpts_, (void*) (context), *this);
        if (!conntok->wait_for(std::chrono::seconds(6))) {
            BOOST_LOG_TRIVIAL(error) << "Connection timeout";
            connected_.store(false, std::memory_order_release);
            return false;
        }

        // 简单检查连接状态
        if (!client_->is_connected()) {
            BOOST_LOG_TRIVIAL(error) << "Connection failed - client not connected";
            connected_.store(false, std::memory_order_release);
            return false;
        }

        connected_.store(true, std::memory_order_release);
        BOOST_LOG_TRIVIAL(info) << "Successfully connected to MQTT server";
        return true;
    }
    catch (const mqtt::exception& exc) {
        connected_.store(false, std::memory_order_release);
        BOOST_LOG_TRIVIAL(error) << "Failed to connect to MQTT server: " << exc.what();
        return false;
    }
}

// Disconnect from the MQTT broker
// @return: true if disconnection successful, false otherwise
bool MqttClient::Disconnect()
{
    if (!connected_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(warning) << "MQTT client already disconnected";
        return true;  // 已经断开就返回成功
    }

    try {
        BOOST_LOG_TRIVIAL(info) << "Disconnecting from MQTT server...";
        
        // 清理连接
        try {
            auto disctok = client_->disconnect();
            // 设置5秒超时
            if (!disctok->wait_for(std::chrono::seconds(5))) {
                BOOST_LOG_TRIVIAL(error) << "MQTT disconnect timeout";
            }
        } catch (const mqtt::exception& exc) {
            BOOST_LOG_TRIVIAL(error) << "Error during disconnect: " << exc.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Unexpected error during disconnect";
        }

        connected_.store(false, std::memory_order_release);
        BOOST_LOG_TRIVIAL(info) << "Successfully disconnected from MQTT server";
        return true;
    }
    catch (const mqtt::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "Error disconnecting from MQTT server: " << exc.what();
        // 即使发生异常，也要标记为断开状态
        connected_.store(false, std::memory_order_release);
        return false;
    }
    catch (const std::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "Unexpected error while disconnecting: " << exc.what();
        connected_.store(false, std::memory_order_release);
        return false;
    }
}

// Subscribe to a specific MQTT topic
// @param topic: The topic to subscribe to
// @param qos: Quality of Service level (0, 1, or 2)
// @return: true if subscription successful, false otherwise
bool MqttClient::Subscribe(const std::string& topic, int qos)
{
    if (!CheckConnected()) {
        BOOST_LOG_TRIVIAL(error) << "Cannot subscribe: client not connected";
        return false;
    }

    try {
        BOOST_LOG_TRIVIAL(info) << "Subscribing to MQTT topic '" << topic << "' with QoS " << qos;
        mqtt::token_ptr subtok = client_->subscribe(topic, qos, nullptr, subListener_);
        if (!subtok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "Subscribe timeout for topic: " << topic;
            return false;
        }
        add_topic_to_resubscribe(topic, qos);
        return true;
    } catch (const mqtt::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "Error subscribing to topic '" << topic << "': " << exc.what();
        return false;
    }
}

// Unsubscribe from a specific MQTT topic
// @param topic: The topic to unsubscribe from
// @return: true if unsubscription successful, false otherwise
bool MqttClient::Unsubscribe(const std::string& topic)
{
    if (!CheckConnected()) {
        BOOST_LOG_TRIVIAL(error) << "Cannot unsubscribe: client not connected";
        return false;
    }

    try {
        BOOST_LOG_TRIVIAL(info) << "Unsubscribing from MQTT topic '" << topic << "'";
        mqtt::token_ptr unsubtok = client_->unsubscribe(topic);
        if (!unsubtok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "Unsubscribe timeout for topic: " << topic;
            return false;
        }
        remove_topic_from_resubscribe(topic);
        return true;
    } catch (const mqtt::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "Error unsubscribing from topic '" << topic << "': " << exc.what();
        return false;
    }
}

// Publish a message to a specific MQTT topic
// @param topic: The topic to publish to
// @param payload: The message content
// @param qos: Quality of Service level (0, 1, or 2)
// @return: true if publish successful, false otherwise
bool MqttClient::Publish(const std::string& topic, const std::string& payload, int qos)
{
    if (!CheckConnected()) {
        return false;
    }

    mqtt::message_ptr pubmsg = mqtt::make_message(topic, payload);
    pubmsg->set_qos(qos);

    try {
        BOOST_LOG_TRIVIAL(debug) << "Publishing message to topic '" << topic << "' with QoS " << qos;
        mqtt::token_ptr pubtok = client_->publish(pubmsg);
        /*if (!pubtok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "Publish timeout for topic: " << topic;
            return false;
        }*/
        return true;
    } catch (const mqtt::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "Error publishing to topic '" << topic << "': " << exc.what();
        return false;
    }
}

// Set callback function for handling incoming messages
// @param callback: Function to be called when a message arrives
void MqttClient::SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload)> callback)
{
    message_callback_ = callback;
}

// Check if the client is currently connected
// @return: true if connected, false otherwise
bool MqttClient::CheckConnected()
{
    if (!connected_.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(error) << "MQTT client is not connected to server";
        return false;
    }

    // 带超时的连接状态检查
    auto check_future = std::async(std::launch::async, [this]() {
        return client_->is_connected();
    });
    
    if (check_future.wait_for(std::chrono::seconds(3)) == std::future_status::timeout) {
        BOOST_LOG_TRIVIAL(error) << "Connection status check timeout";
        connected_.store(false, std::memory_order_release);
        return false;
    }

    if (!check_future.get()) {
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
    BOOST_LOG_TRIVIAL(warning) << "MQTT connection lost";
    if (!cause.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Cause: " << cause;
    }

    connected_.store(false, std::memory_order_release);
    
    if (!is_reconnecting.load(std::memory_order_acquire)) {
        is_reconnecting.store(true, std::memory_order_release);
        // 获取weak_ptr用于后续检查
        std::weak_ptr<MqttClient> weak_self = shared_from_this();

        // 启动异步检查任务，并忽略返回值
        std::thread([weak_self]() {
            // 等待10秒
            std::this_thread::sleep_for(std::chrono::seconds(20));

            // 尝试获取shared_ptr，检查对象是否还存在
            if (auto self = weak_self.lock()) {
                // 对象仍然存在，检查连接状态
                if (!self->connected_.load(std::memory_order_acquire)) {
                    BOOST_LOG_TRIVIAL(error) << "MQTT connection not restored after 10 seconds";
                    // 如果设置了失败回调，则调用
                    if (self->connection_failure_callback_) {
                        self->connection_failure_callback_();
                    }
                } else {
                    BOOST_LOG_TRIVIAL(info) << "MQTT connection successfully restored";
                }
            } else {
                BOOST_LOG_TRIVIAL(info) << "MQTT client object no longer exists";
            }
        }).detach(); // 分离线程，让它在后台运行

        BOOST_LOG_TRIVIAL(info) << "Waiting for automatic reconnection...";
    }

    
}

// Callback when a message arrives
// @param msg: Pointer to the received message
void MqttClient::message_arrived(mqtt::const_message_ptr msg)
{
    if (message_callback_) {
        message_callback_(msg->get_topic(), msg->to_string());
    }
}

// Callback when message delivery is complete
// @param token: Delivery token containing message details
void MqttClient::delivery_complete(mqtt::delivery_token_ptr token)
{
    BOOST_LOG_TRIVIAL(debug) << "Message delivery complete for token: " << (token ? token->get_message_id() : -1);
}

// Callback for operation failure
// @param tok: Token containing operation details
void MqttClient::on_failure(const mqtt::token& tok)
{
    // 检查是否是连接操作失败
    if (tok.get_user_context() && 
        std::string(static_cast<const char*>(tok.get_user_context())) == "connection") {
        BOOST_LOG_TRIVIAL(error) << "MQTT connection attempt failed";
        if (tok.get_reason_code() != 0) {
            BOOST_LOG_TRIVIAL(error) << "Reason code: " << tok.get_reason_code();
        }
        // 这里可以触发连接失败的处理逻辑
        connected_.store(false, std::memory_order_release);
        
        //// 调用连接失败回调
        //if (connection_failure_callback_) {
        //    connection_failure_callback_();
        //}
    } else {
        // 其他操作失败的处理
        BOOST_LOG_TRIVIAL(error) << "Operation failed for token: " << tok.get_message_id();
        if (tok.get_reason_code() != 0) {
            BOOST_LOG_TRIVIAL(error) << "Reason code: " << tok.get_reason_code();
        }
    }
}

// Callback for operation success
// @param tok: Token containing operation details
void MqttClient::on_success(const mqtt::token& tok)
{
    BOOST_LOG_TRIVIAL(debug) << "Operation successful for token: " << tok.get_message_id();
    auto top = tok.get_topics();
    if (top && !top->empty()) {
        BOOST_LOG_TRIVIAL(debug) << "Token topic: '" << (*top)[0] << "'";
    }
}

// 添加连接成功的回调
void MqttClient::connected(const std::string& cause)
{
    BOOST_LOG_TRIVIAL(info) << "MQTT connection established";
    connected_.store(true, std::memory_order_release);
    is_reconnecting.store(false, std::memory_order_release);
    
    // 连接成功后重新订阅之前的主题
    /*resubscribe_topics();*/
}

void MqttClient::resubscribe_topics() {
    if (topics_to_resubscribe_.empty()) {
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "Resubscribing to " << topics_to_resubscribe_.size() << " topics";
    
    for (const auto& topic_pair : topics_to_resubscribe_) {
        try {
            auto tok = client_->subscribe(topic_pair.first, topic_pair.second, nullptr,  subListener_);
            if (!tok->wait_for(std::chrono::seconds(5))) {
                BOOST_LOG_TRIVIAL(error) << "Subscribe timeout for topic: " << topic_pair.first;
                continue;
            }
            if (!tok->is_complete() || tok->get_return_code() != 0) {
                BOOST_LOG_TRIVIAL(error) << "Failed to resubscribe to topic: " << topic_pair.first;
            }
        } catch (const mqtt::exception& exc) {
            BOOST_LOG_TRIVIAL(error) << "Error resubscribing to topic " << topic_pair.first 
                                   << ": " << exc.what();
        }
    }
}

void MqttClient::add_topic_to_resubscribe(const std::string& topic, int qos) {
    topics_to_resubscribe_[topic] = qos;
    BOOST_LOG_TRIVIAL(debug) << "Added topic to resubscribe list: " << topic;
}

void MqttClient::remove_topic_from_resubscribe(const std::string& topic) {
    auto it = topics_to_resubscribe_.find(topic);
    if (it != topics_to_resubscribe_.end()) {
        topics_to_resubscribe_.erase(it);
        BOOST_LOG_TRIVIAL(debug) << "Removed topic from resubscribe list: " << topic;
    }
}
