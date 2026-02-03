#include "DownloadManager.hpp"
#include "GUI_App.hpp"
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <vector>

namespace Slic3r { namespace GUI {

// ============================================================================
// WCP Download Interface (for Web-to-PC communication)
// ============================================================================
size_t DownloadManager::start_wcp_download(const std::string& file_url,
                                             const std::string& file_name,
                                             std::shared_ptr<SSWCP_Instance> wcp_instance,
                                             bool use_original_event_id) {
    
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    size_t task_id = m_next_task_id++;
    
    auto downloadPath = wxGetApp().app_config->get("download_path");
    boost::filesystem::path dest_folder(downloadPath);
    boost::filesystem::create_directories(dest_folder);
    boost::filesystem::path dest_file = dest_folder / file_name;
    std::string dest_path = dest_file.string();
    
    auto task = std::make_shared<DownloadTask>(task_id, 
                                                file_url, 
                                                file_name, 
                                                dest_path, 
                                                wcp_instance,
                                                use_original_event_id);

    task->state = DownloadTaskState::Downloading;
    
    m_tasks[task_id] = task;
    start_download_impl(task);
    return task_id;
}

// ============================================================================
// Internal Download Interface (for PC internal use)
// ============================================================================
size_t DownloadManager::start_internal_download(const std::string& file_url,
                                                  const std::string& file_name,
                                                  const std::string& dest_path,
                                                  DownloadCallbacks callbacks) {
    
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    size_t task_id = m_next_task_id++;
    
    boost::filesystem::path dest_file_path(dest_path);
    boost::filesystem::create_directories(dest_file_path.parent_path());

    auto task = std::make_shared<DownloadTask>(task_id, 
                                                file_url, 
                                                file_name, 
                                                dest_path, 
                                                std::move(callbacks));

    task->state = DownloadTaskState::Downloading;
    
    m_tasks[task_id] = task;
    start_download_impl(task);
    
    return task_id;
}

size_t DownloadManager::start_internal_download(const std::string& file_url,
                                                 const std::string& file_name,
                                                 DownloadCallbacks callbacks) {
    // Get default download path
    auto downloadPath = wxGetApp().app_config->get("download_path");
    boost::filesystem::path dest_folder(downloadPath);
    boost::filesystem::create_directories(dest_folder);
    
    boost::filesystem::path dest_file = dest_folder / file_name;
    std::string dest_path = dest_file.string();
    
    return start_internal_download(file_url, file_name, dest_path, std::move(callbacks));
}

void DownloadManager::start_download_impl(std::shared_ptr<DownloadTask> task) {
    
    wxGetApp().CallAfter([this, task]() {
        try {
            // Step 1: Create Http object
            Http http = Http::get(task->file_url);
            
            http.timeout_max(0);

            // Step 2: Set progress callback
            http.on_progress([this, task](Http::Progress progress, bool& cancel) {
                if (task->state == DownloadTaskState::Canceled) {
                    cancel = true;
                    return;
                }
                
                int percent = 0;
                if (progress.dltotal > 0) {
                    percent = (int)(progress.dlnow * 100 / progress.dltotal);
                }
                
                task->percent = percent;
                
                // Throttle progress updates: update every 5% or every second
                std::lock_guard<std::mutex> lock(m_tasks_mutex);
                auto& last_pct = m_last_percent[task->task_id];
                auto& last_upd = m_last_update[task->task_id];
                
                auto now = std::chrono::steady_clock::now();
                bool should_update = false;
                
                if (percent - last_pct >= 5) {
                    should_update = true;
                    last_pct = percent;
                } else if (now - last_upd >= std::chrono::seconds(1)) {
                    should_update = true;
                }
                
                if (should_update) {
                    last_upd = now;
                    wxGetApp().CallAfter([this, task, percent, progress]() {
                        send_progress_update(task, percent, progress.dlnow, progress.dltotal);
                    });
                }
            });
            
            // Step 3: Set complete callback
            http.on_complete([this, task](std::string body, unsigned status) {
                wxGetApp().CallAfter([this, task, body]() {
                    try {
                        // Save file
                        boost::nowide::ofstream file(task->dest_path, std::ios::binary);
                        if (!file.is_open()) {
                            send_error_update(task, "Failed to open file for writing");
                            cleanup_task(task->task_id);
                            return;
                        }
                        
                        file.write(body.c_str(), body.size());
                        file.close();
                        
                        task->state = DownloadTaskState::Completed;
                        task->percent = 100;
                        send_complete_update(task, task->dest_path);
                        cleanup_task(task->task_id);
                    } catch (std::exception& e) {
                        send_error_update(task, e.what());
                        cleanup_task(task->task_id);
                    }
                });
            });
            
            // Step 4: Set error callback
            http.on_error([this, task](std::string body, std::string error, unsigned status) {
                wxGetApp().CallAfter([this, task, error, status]() {
                    task->state = DownloadTaskState::Error;
                    task->error_message = error;
                    send_error_update(task, error);
                    cleanup_task(task->task_id);
                });
            });
            
            // Step 5: Start download and save Http::Ptr for cancellation
            task->http_object = http.perform();
            
        } catch (std::exception& e) {
            task->state = DownloadTaskState::Error;
            task->error_message = e.what();
            send_error_update(task, e.what());
            cleanup_task(task->task_id);
        }
    });
}

//this function not currently in use
bool DownloadManager::cancel_download(size_t task_id) {
    std::shared_ptr<SSWCP_Instance> wcp_to_destroy;
    
    {
        std::lock_guard<std::mutex> lock(m_tasks_mutex);
        
        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) {
            return false;
        }
        
        auto task = it->second;
        if (task->state == DownloadTaskState::Downloading) {
            task->state = DownloadTaskState::Canceled;
            if (task->http_object) {
                task->http_object->cancel();
            }
            
            // Only for WCP downloads
            if (task->is_wcp_download()) {
                wcp_to_destroy = task->wcp_instance.lock();
            } else {
                // For internal downloads, call error callback if available
                if (task->callbacks.on_error) {
                    wxGetApp().CallAfter([task]() {
                        if (task->callbacks.on_error) {
                            task->callbacks.on_error(task->task_id, "Download canceled");
                        }
                    });
                }
            }
            
            cleanup_task(task_id);
        } else {
            return false;
        }
    }

    if (wcp_to_destroy) {
        wcp_to_destroy->finish_job();
    }
    
    return true;
}

// this function not currently in use
bool DownloadManager::pause_download(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end() && it->second->state == DownloadTaskState::Downloading) {
        it->second->state = DownloadTaskState::Paused;        
        return true;
    }
    return false;
}

// this function not currently in use
bool DownloadManager::resume_download(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end() && it->second->state == DownloadTaskState::Paused) {
        return false; 
    }
    return false;
}

// this function not currently in use
DownloadTaskState DownloadManager::get_task_state(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) {
        return it->second->state;
    }
    return DownloadTaskState::Error;
}

// this function not currently in use
std::shared_ptr<DownloadTask> DownloadManager::get_task(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) {
        return it->second;
    }
    return nullptr;
}

// this function not currently in use
std::vector<std::shared_ptr<DownloadTask>> DownloadManager::get_all_tasks() {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    std::vector<std::shared_ptr<DownloadTask>> result;
    result.reserve(m_tasks.size());
    for (const auto& pair : m_tasks) {
        result.push_back(pair.second);
    }
    return result;
}

// ============================================================================
// Progress/Complete/Error Update Handlers
// ============================================================================
void DownloadManager::send_progress_update(std::shared_ptr<DownloadTask> task, 
                                               int percent, 
                                               size_t downloaded, 
                                               size_t total) {
    if (task->is_wcp_download()) {
        send_wcp_progress_update(task, percent, downloaded, total);
    } else {
        call_internal_progress_callback(task, percent, downloaded, total);
    }
}

void DownloadManager::send_wcp_progress_update(std::shared_ptr<DownloadTask> task, 
                                                  int percent, 
                                                  size_t downloaded, 
                                                  size_t total) {    
    if (!task->use_original_event_id) {
        return;
    }
    
    if (auto wcp = task->wcp_instance.lock()) {
        json progress_data;
        progress_data["task_id"] = task->task_id;
        progress_data["percent"] = percent;
        progress_data["downloaded"] = downloaded;
        progress_data["total"] = total;
        progress_data["state"] = "downloading";
        
        wcp->m_res_data = progress_data;
        wcp->m_status = 0;
        wcp->m_msg = "Download progress";
                
        json header;
        if (task->use_original_event_id) {
            header["event_id"] = wcp->m_event_id;
        } else {
            header["event_id"] = wcp->m_event_id + "_progress";
        }

        header["command"] = "download_progress";
        wcp->m_header = header;
        
        wcp->send_to_js();
    }
}

void DownloadManager::call_internal_progress_callback(std::shared_ptr<DownloadTask> task,
                                                      int percent,
                                                      size_t downloaded,
                                                      size_t total) {    
    if (task->callbacks.on_progress) {
        task->callbacks.on_progress(task->task_id, percent, downloaded, total);
    }
}

void DownloadManager::send_complete_update(std::shared_ptr<DownloadTask> task, 
                                               const std::string& file_path) {
    if (task->is_wcp_download()) {
        send_wcp_complete_update(task, file_path);
    } else {
        call_internal_complete_callback(task, file_path);
    }
}

void DownloadManager::send_wcp_complete_update(std::shared_ptr<DownloadTask> task, 
                                                 const std::string& file_path) {
    
    if (!task->use_original_event_id) {
        return;
    }

    if (auto wcp = task->wcp_instance.lock()) {
        json complete_data;
        complete_data["task_id"] = task->task_id;
        complete_data["file_path"] = file_path;
        complete_data["file_name"] = task->file_name;
        complete_data["percent"] = 100;
        complete_data["state"] = "completed";
        
        wcp->m_res_data = complete_data;
        wcp->m_status = 0;
        wcp->m_msg = "Download completed";
        
        wcp->send_to_js();
        wcp->finish_job();
    }
}

void DownloadManager::call_internal_complete_callback(std::shared_ptr<DownloadTask> task,
                                                       const std::string& file_path) {
    if (task->callbacks.on_complete) {
        task->callbacks.on_complete(task->task_id, file_path);
    }
}

void DownloadManager::send_error_update(std::shared_ptr<DownloadTask> task, 
                                           const std::string& error) {
    if (task->is_wcp_download()) {
        send_wcp_error_update(task, error);
    } else {
        call_internal_error_callback(task, error);
    }
}

void DownloadManager::send_wcp_error_update(std::shared_ptr<DownloadTask> task, 
                                              const std::string& error) {
    
    if (!task->use_original_event_id) {
        return;
    }
    
    if (auto wcp = task->wcp_instance.lock()) {
        json error_data;
        error_data["task_id"] = task->task_id;
        error_data["error"] = error;
        error_data["state"] = "error";
        
        wcp->m_res_data = error_data;
        wcp->m_status = -1;
        wcp->m_msg = error;

        wcp->send_to_js();
        wcp->finish_job();
    }
}

void DownloadManager::call_internal_error_callback(std::shared_ptr<DownloadTask> task,
                                                    const std::string& error) {
    if (task->callbacks.on_error) {
        task->callbacks.on_error(task->task_id, error);
    }
}

void DownloadManager::cleanup_task(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    m_tasks.erase(task_id);
    m_last_percent.erase(task_id);
    m_last_update.erase(task_id);
}

}} // namespace Slic3r::GUI

