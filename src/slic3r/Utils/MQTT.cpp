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
    , client_(server_address_, client_id_)
    , connOpts_()
    , subListener_("Subscription")
    , connected_(false)
{
    // Configure connection options
    connOpts_.set_clean_session(clean_session);
    connOpts_.set_keep_alive_interval(20);  // Keep-alive ping every 20 seconds
    client_.set_callback(*this);
}

// Establish connection to the MQTT broker
// @return: true if connection successful, false otherwise
bool MqttClient::Connect()
{
    if (connected_) {
        BOOST_LOG_TRIVIAL(warning) << "Already connected to MQTT server";
        return true;
    }

    try {
        // 先确保之前的连接已完全断开
        if (client_.is_connected()) {
            try {
                client_.disconnect()->wait();
            } catch (...) {
                // 忽略断开时的错误
            }
        }

        BOOST_LOG_TRIVIAL(info) << "Connecting to the MQTT server: " << server_address_;
        
        // 设置连接超时
        connOpts_.set_connect_timeout(5);  // 5秒连接超时
        
        // 使用同步连接，等待连接结果
        mqtt::token_ptr conntok = client_.connect(connOpts_, nullptr, *this);
        if (!conntok->wait_for(std::chrono::seconds(6))) {  // 给额外1秒缓冲
            BOOST_LOG_TRIVIAL(error) << "Connection timeout";
            connected_ = false;
            return false;
        }

        // 验证连接状态
        if (!client_.is_connected()) {
            BOOST_LOG_TRIVIAL(error) << "Connection failed - client not connected";
            connected_ = false;
            return false;
        }

        connected_ = true;
        BOOST_LOG_TRIVIAL(info) << "Successfully connected to MQTT server";
        return true;
    }
    catch (const mqtt::exception& exc) {
        connected_ = false;
        BOOST_LOG_TRIVIAL(error) << "Failed to connect to MQTT server '" << server_address_ << "': " << exc.what();
        
        // 确保清理任何可能的残留连接
        try {
            client_.disconnect()->wait();
        } catch (...) {}
        
        return false;
    }
    catch (const std::exception& exc) {
        connected_ = false;
        BOOST_LOG_TRIVIAL(error) << "Unexpected error while connecting: " << exc.what();
        return false;
    }
}

// Disconnect from the MQTT broker
// @return: true if disconnection successful, false otherwise
bool MqttClient::Disconnect()
{
    if (!connected_ && !client_.is_connected()) {
        BOOST_LOG_TRIVIAL(warning) << "MQTT client already disconnected";
        return true;  // 已经断开就返回成功
    }

    try {
        BOOST_LOG_TRIVIAL(info) << "Disconnecting from MQTT server...";
        
        // 设置断开连接超时
        mqtt::token_ptr disctok = client_.disconnect();
        if (!disctok->wait_for(std::chrono::seconds(5))) {
            BOOST_LOG_TRIVIAL(error) << "Disconnect timeout";
            // 即使超时，我们也标记为已断开
            connected_ = false;
            return false;
        }

        connected_ = false;
        BOOST_LOG_TRIVIAL(info) << "Successfully disconnected from MQTT server";
        return true;
    }
    catch (const mqtt::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "Error disconnecting from MQTT server: " << exc.what();
        // 即使发生异常，也要标记为断开状态
        connected_ = false;
        return false;
    }
    catch (const std::exception& exc) {
        BOOST_LOG_TRIVIAL(error) << "Unexpected error while disconnecting: " << exc.what();
        connected_ = false;
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
        return false;
    }

    try {
        BOOST_LOG_TRIVIAL(info) << "Subscribing to MQTT topic '" << topic << "' with QoS " << qos;
        mqtt::token_ptr subtok = client_.subscribe(topic, qos, nullptr, subListener_);
        subtok->wait_for(std::chrono::seconds(10));
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
        return false;
    }

    try {
        BOOST_LOG_TRIVIAL(info) << "Unsubscribing from MQTT topic '" << topic << "'";
        mqtt::token_ptr unsubtok = client_.unsubscribe(topic);
        unsubtok->wait();
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
        BOOST_LOG_TRIVIAL(debug) << "Publishing message to topic '" << topic << "' with QoS " << qos << ": " << payload;
        mqtt::token_ptr pubtok = client_.publish(pubmsg);
        pubtok->wait_for(std::chrono::seconds(10));
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
    if (!connected_) {
        BOOST_LOG_TRIVIAL(error) << "MQTT client is not connected to server";
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

    // 确保更新连接状态
    connected_ = false;
    
    // 清理连接
    try {
        client_.disconnect()->wait();
    } catch (...) {}
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
    BOOST_LOG_TRIVIAL(error) << "Operation failed for token: " << tok.get_message_id();
    if (tok.get_reason_code() != 0) {
        BOOST_LOG_TRIVIAL(error) << "Reason code: " << tok.get_reason_code();
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
