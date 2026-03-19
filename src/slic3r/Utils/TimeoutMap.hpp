#ifndef slic3r_TimeoutMap_hpp_
#define slic3r_TimeoutMap_hpp_

#include <map>
#include <mutex>
#include <chrono>
#include <memory>
#include <thread>
#include <functional>
#include <type_traits>

namespace Slic3r {

// Check if type is a shared_ptr
template<typename T>
struct is_shared_ptr : std::false_type {};

template<typename T>
struct is_shared_ptr<std::shared_ptr<T>> : std::true_type {};

// Check if type has an on_timeout method
template<typename T, typename = void>
struct has_on_timeout : std::false_type {};

template<typename T>
struct has_on_timeout<T, std::void_t<decltype(std::declval<T>().on_timeout())>> : std::true_type {};

template<typename T, typename = void>
struct has_timeout_callback : std::false_type {};

template<typename T>
struct has_timeout_callback<T, 
    std::void_t<decltype(std::declval<T>().timeout_cb)>> : std::true_type {};

// A thread-safe map container that automatically removes entries after a timeout period
template<typename K, typename V>
class TimeoutMap {
public:
    // Clock type used for timing
    using clock_type = std::chrono::steady_clock;
    using time_point = std::chrono::time_point<clock_type>;
    
    // Special timeout value indicating item should never expire
    static constexpr std::chrono::milliseconds INFINITE_TIMEOUT{std::chrono::milliseconds::max()};

    // Container for map items with timeout information
    struct Item {
        V value;                // The stored value
        time_point expire_time; // When this item expires
        bool never_expire;      // Flag indicating if item should never expire
        
        // Constructor initializes item with value and timeout duration
        Item(V v, std::chrono::milliseconds timeout) 
            : value(std::move(v))
            , expire_time(clock_type::now() + timeout)
            , never_expire(timeout == INFINITE_TIMEOUT)
        {}
    };

    // Constructor starts background thread for checking timeouts
    TimeoutMap(std::chrono::milliseconds default_timeout = std::chrono::milliseconds(30000))
        : m_default_timeout(default_timeout)
        , m_running(true) {
        m_check_thread = std::thread([this]() { check_timeouts(); });
    }

    // Destructor ensures background thread is stopped and joined
    ~TimeoutMap() {
        m_running = false;
        if (m_check_thread.joinable()) {
            m_check_thread.join();
        }
    }

    // Add an item that never expires
    bool add_infinite(const K& key, V value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_items[key] = std::make_shared<Item>(std::move(value), INFINITE_TIMEOUT);
        return true;
    }

    // Add an item with optional custom timeout
    bool add(const K& key, V value, std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (timeout.count() == 0) timeout = m_default_timeout;
        m_items[key] = std::make_shared<Item>(std::move(value), timeout);
        return true;
    }

    // Remove an item by key
    void remove(const K& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_items.erase(key);
    }

    // Get value by key, returns nullptr if expired or not found
    std::shared_ptr<V> get(const K& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_items.find(key);
        if (it != m_items.end()) {
            if (it->second->expire_time > std::chrono::steady_clock::now() || it->second->never_expire) {
                return std::make_shared<V>(it->second->value);
            } else {
                m_items.erase(it);
            }
        }
        return nullptr;
    }

    // Get and remove value, useful for one-time callbacks
    std::shared_ptr<V> get_and_remove(const K& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_items.find(key);
        if (it != m_items.end()) {
            if (it->second->expire_time > std::chrono::steady_clock::now()) {
                auto value = std::make_shared<V>(it->second->value);
                m_items.erase(it);
                return value;
            } else {
                m_items.erase(it);
            }
        }
        return nullptr;
    }

    // Check if key exists and has not expired
    bool exists(const K& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_items.find(key);
        if (it != m_items.end()) {
            if (it->second->expire_time > std::chrono::steady_clock::now()) {
                return true;
            } else {
                m_items.erase(it);
            }
        }
        return false;
    }

    // Update timeout for an existing item
    bool update_timeout(const K& key, std::chrono::milliseconds timeout) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_items.find(key);
        if (it != m_items.end()) {
            it->second->expire_time = std::chrono::steady_clock::now() + timeout;
            return true;
        }
        return false;
    }

    // Clear all items from the map
    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_items.clear();
    }

    // Get current number of items
    size_t size() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.size();
    }

    // Iterator support
    using iterator = typename std::map<K, std::shared_ptr<Item>>::iterator;
    using const_iterator = typename std::map<K, std::shared_ptr<Item>>::const_iterator;

    // Iterator access methods
    iterator begin() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.begin();
    }

    iterator end() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.end();
    }

    const_iterator begin() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.begin();
    }

    const_iterator end() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.end();
    }

    const_iterator cbegin() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.cbegin();
    }

    const_iterator cend() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.cend();
    }

    // Get a snapshot of current state
    std::vector<std::pair<K, V>> get_snapshot() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::pair<K, V>> snapshot;
        snapshot.reserve(m_items.size());
        for (const auto& [key, item] : m_items) {
            snapshot.emplace_back(key, item->value);
        }
        return snapshot;
    }

private:
    // Background thread function to check for expired items
    void check_timeouts() {
        while (m_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            std::lock_guard<std::mutex> lock(m_mutex);
            auto now = clock_type::now();
            
            for (auto it = m_items.begin(); it != m_items.end();) {
                if (!it->second->never_expire && it->second->expire_time <= now) {
                    // Handle timeout callbacks for different types
                    if constexpr (has_timeout_callback<V>::value) {
                        if (it->second->value.timeout_cb) {
                            it->second->value.timeout_cb();
                        }
                    }
                    else if constexpr (has_on_timeout<V>::value) {
                        it->second->value.on_timeout();
                    }
                    else if constexpr (is_shared_ptr<V>::value && 
                                     has_on_timeout<typename V::element_type>::value) {
                        if (it->second->value) {
                            it->second->value->on_timeout();
                        }
                    }
                    
                    it = m_items.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    // Member variables
    std::map<K, std::shared_ptr<Item>> m_items;        // Storage for timed items
    mutable std::mutex m_mutex;                        // Mutex for thread safety
    std::chrono::milliseconds m_default_timeout;       // Default timeout duration
    std::thread m_check_thread;                        // Background check thread
    bool m_running;                                    // Thread control flag
};

}
#endif