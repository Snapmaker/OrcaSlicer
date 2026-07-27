#include "DownloadManager.hpp"
#include "GUI_App.hpp"
#include "HttpServer.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/FileDecrypt.hpp"
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <vector>
#include <ctime>

namespace Slic3r { namespace GUI {

// ============================================================================
// Helper Functions
// ============================================================================

std::string DownloadManager::get_unique_file_path(const boost::filesystem::path& file_path)
{
    std::string original_path = file_path.string();
    BOOST_LOG_TRIVIAL(debug) << boost::format("DownloadManager::get_unique_file_path: Checking path '%1%'") % original_path;

    if (!boost::filesystem::exists(file_path)) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("DownloadManager::get_unique_file_path: File does not exist, returning original path '%1%'") % original_path;
        return original_path;
    }

    BOOST_LOG_TRIVIAL(debug) << boost::format("DownloadManager::get_unique_file_path: File exists, generating unique name");

    boost::filesystem::path parent_dir = file_path.parent_path();
    std::string filename = file_path.filename().string();
    std::string extension = file_path.extension().string();

    std::string name_without_ext;
    if (extension.empty()) {
        name_without_ext = filename;
    } else {
        name_without_ext = filename.substr(0, filename.size() - extension.size());
    }

    size_t version = 1;
    boost::filesystem::path unique_path;
    do {
        std::string new_filename;
        if (extension.empty()) {
            new_filename = name_without_ext + "(" + std::to_string(version) + ")";
        } else {
            new_filename = name_without_ext + "(" + std::to_string(version) + ")" + extension;
        }
        unique_path = parent_dir / new_filename;
        version++;
    } while (boost::filesystem::exists(unique_path) && version < 10000);

    if (version >= 10000) {
        BOOST_LOG_TRIVIAL(warning) << boost::format("DownloadManager::get_unique_file_path: Too many duplicate files for '%1%', using timestamp-based name")
            % original_path;
        std::string timestamp = std::to_string(std::time(nullptr));
        std::string new_filename;
        if (extension.empty()) {
            new_filename = name_without_ext + "_" + timestamp;
        } else {
            new_filename = name_without_ext + "_" + timestamp + extension;
        }
        unique_path = parent_dir / new_filename;
    }

    std::string result = unique_path.string();
    BOOST_LOG_TRIVIAL(debug) << boost::format("DownloadManager::get_unique_file_path: Final unique path: '%1%'") % result;
    return result;
}

// ============================================================================
// WCP Download Interface
// ============================================================================
size_t DownloadManager::start_wcp_download(const std::string& file_url,
                                             const std::string& file_name,
                                             std::shared_ptr<SSWCP_Instance> wcp_instance,
                                             bool use_original_event_id) {
    return start_wcp_download(file_url, file_name, wcp_instance, use_original_event_id, true, "");
}

size_t DownloadManager::start_wcp_download(const std::string& file_url,
                                             const std::string& file_name,
                                             std::shared_ptr<SSWCP_Instance> wcp_instance,
                                             bool use_original_event_id,
                                             bool need_decrypt,
                                             const std::string& sn) {

    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    size_t task_id = m_next_task_id++;

    auto downloadPath = wxGetApp().app_config->get("download_path");
    boost::filesystem::path dest_folder(downloadPath);
    boost::filesystem::create_directories(dest_folder);
    boost::filesystem::path dest_file = dest_folder / file_name;

    std::string dest_path = get_unique_file_path(dest_file);

    std::string actual_file_name = boost::filesystem::path(dest_path).filename().string();

    auto task = std::make_shared<DownloadTask>(task_id,
                                                file_url,
                                                actual_file_name,
                                                dest_path,
                                                wcp_instance,
                                                use_original_event_id,
                                                need_decrypt,
                                                sn);

    m_tasks[task_id] = task;

    if (m_active_downloads >= m_max_concurrent_downloads) {
        task->state = DownloadTaskState::Pending;
        m_pending_queue.push_back(task_id);
        return task_id;
    }

    task->state = DownloadTaskState::Downloading;
    m_active_downloads++;
    start_download_impl(task);
    return task_id;
}

// ============================================================================
// Internal Download Interface
// ============================================================================
size_t DownloadManager::start_internal_download(const std::string& file_url,
                                                 const std::string& file_name,
                                                 const std::string& dest_path,
                                                 DownloadCallbacks callbacks) {

    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    size_t task_id = m_next_task_id++;

    boost::filesystem::path dest_path_obj(dest_path);

    boost::filesystem::path dest_file_path;
    if (boost::filesystem::is_directory(dest_path_obj) || dest_path_obj.filename().empty()) {
        dest_file_path = dest_path_obj / file_name;
    } else {
        dest_file_path = dest_path_obj;
    }

    boost::filesystem::create_directories(dest_file_path.parent_path());

    std::string unique_dest_path = get_unique_file_path(dest_file_path);

    auto task = std::make_shared<DownloadTask>(task_id,
                                                file_url,
                                                file_name,
                                                unique_dest_path,
                                                std::move(callbacks));

    m_tasks[task_id] = task;

    if (m_active_downloads >= m_max_concurrent_downloads) {
        task->state = DownloadTaskState::Pending;
        m_pending_queue.push_back(task_id);
        return task_id;
    }

    task->state = DownloadTaskState::Downloading;
    m_active_downloads++;
    start_download_impl(task);

    return task_id;
}

size_t DownloadManager::start_internal_download(const std::string& file_url,
                                                 const std::string& file_name,
                                                 DownloadCallbacks callbacks) {
    auto downloadPath = wxGetApp().app_config->get("download_path");
    boost::filesystem::path dest_folder(downloadPath);
    boost::filesystem::create_directories(dest_folder);

    boost::filesystem::path dest_file = dest_folder / file_name;

    std::string dest_path = get_unique_file_path(dest_file);

    return start_internal_download(file_url, file_name, dest_path, std::move(callbacks));
}

size_t DownloadManager::start_internal_download(const std::string& file_url,
                                                 const std::string& file_name,
                                                 const std::string& dest_path,
                                                 DownloadCallbacks callbacks,
                                                 bool need_decrypt,
                                                 const std::string& sn) {

    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    size_t task_id = m_next_task_id++;

    boost::filesystem::path dest_path_obj(dest_path);
    boost::filesystem::create_directories(dest_path_obj);

    boost::filesystem::path dest_file_path = dest_path_obj / file_name;
    std::string unique_dest_path = get_unique_file_path(dest_file_path);

    auto task = std::make_shared<DownloadTask>(task_id,
                                                file_url,
                                                file_name,
                                                unique_dest_path,
                                                std::move(callbacks),
                                                need_decrypt,
                                                sn);

    m_tasks[task_id] = task;

    if (m_active_downloads >= m_max_concurrent_downloads) {
        task->state = DownloadTaskState::Pending;
        m_pending_queue.push_back(task_id);
        return task_id;
    }

    task->state = DownloadTaskState::Downloading;
    m_active_downloads++;
    start_download_impl(task);

    return task_id;
}

void DownloadManager::process_queue() {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    while (!m_pending_queue.empty() && m_active_downloads < m_max_concurrent_downloads) {
        size_t task_id = m_pending_queue.front();
        m_pending_queue.pop_front();
        auto it = m_tasks.find(task_id);
        if (it != m_tasks.end() && it->second->state == DownloadTaskState::Pending) {
            it->second->state = DownloadTaskState::Downloading;
            m_active_downloads++;
            start_download_impl(it->second);
        }
    }
}

void DownloadManager::start_download_impl(std::shared_ptr<DownloadTask> task) {

    wxGetApp().CallAfter([this, task]() {
        try {
            Http http = Http::get(task->file_url);
            http.timeout_max(0);

            http.on_progress([this, task](Http::Progress progress, bool& cancel) {
                {
                    std::lock_guard<std::mutex> lock(m_tasks_mutex);
                    if (m_tasks.find(task->task_id) == m_tasks.end()) {
                        cancel = true;
                        return;
                    }
                }

                if (task->state == DownloadTaskState::Canceled) {
                    cancel = true;
                    return;
                }

                int percent = 0;
                if (progress.dltotal > 0) {
                    percent = (int)(progress.dlnow * 100 / progress.dltotal);
                }

                task->percent = percent;

                std::lock_guard<std::mutex> lock(m_tasks_mutex);

                if (m_tasks.find(task->task_id) == m_tasks.end()) {
                    cancel = true;
                    return;
                }

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
                        std::lock_guard<std::mutex> lock(m_tasks_mutex);
                        if (m_tasks.find(task->task_id) != m_tasks.end() &&
                            task->state != DownloadTaskState::Canceled) {
                            send_progress_update(task, percent, progress.dlnow, progress.dltotal);
                        }
                    });
                }
            });

            http.on_complete([this, task](std::string body, unsigned status) {
                wxGetApp().CallAfter([this, task, body]() {
                    if (task->state == DownloadTaskState::Canceled) {
                        BOOST_LOG_TRIVIAL(debug) << "DownloadManager: Ignoring complete callback for canceled task " << task->task_id;
                        return;
                    }

                    try {
                        boost::nowide::ofstream file(task->dest_path, std::ios::binary);
                        if (!file.is_open()) {
                            std::string error_msg = "Failed to open file for writing: " + task->dest_path;
                            BOOST_LOG_TRIVIAL(error) << "DownloadManager: " << error_msg;
                            Slic3r::flush_logs();
                            send_error_update(task, error_msg);
                            cleanup_task(task->task_id);
                            m_active_downloads--;
                            process_queue();
                            return;
                        }

                        file.write(body.c_str(), body.size());
                        if (file.fail()) {
                            std::string error_msg = "Failed to write file: " + task->dest_path;
                            BOOST_LOG_TRIVIAL(error) << "DownloadManager: " << error_msg << ", body size: " << body.size();
                            Slic3r::flush_logs();
                            file.close();
                            send_error_update(task, error_msg);
                            cleanup_task(task->task_id);
                            m_active_downloads--;
                            process_queue();
                            return;
                        }
                        file.close();

                        std::string final_path = task->dest_path;

                        if (task->need_decrypt && !task->sn.empty()) {
                            const std::string enc_path = task->dest_path;
                            const std::string tmp_path = enc_path + ".tmp";

                            if (!decrypt_file_aes_cbc(enc_path, task->sn, Slic3r::PBKDF2_ITERATIONS, tmp_path)) {
                                send_error_update(task, "File decryption failed");
                                cleanup_task(task->task_id);
                                m_active_downloads--;
                                process_queue();
                                return;
                            }

                            try { boost::filesystem::remove(enc_path); } catch (...) {}
                            try { boost::filesystem::rename(tmp_path, enc_path); } catch (...) {}

                            task->decrypt_path = enc_path;
                        }

                        task->state = DownloadTaskState::Completed;
                        task->percent = 100;
                        send_complete_update(task, final_path);
                        cleanup_task(task->task_id);
                        m_active_downloads--;
                        process_queue();
                    } catch (std::exception& e) {
                        std::string error_msg = std::string("File write exception: ") + e.what();
                        BOOST_LOG_TRIVIAL(error) << "DownloadManager: " << error_msg;
                        Slic3r::flush_logs();
                        send_error_update(task, error_msg);
                        cleanup_task(task->task_id);
                        m_active_downloads--;
                        process_queue();
                    }
                });
            });

            http.on_error([this, task](std::string body, std::string error, unsigned status) {
                wxGetApp().CallAfter([this, task, error, status]() {
                    if (task->state == DownloadTaskState::Canceled) {
                        return;
                    }

                    std::string error_msg = boost::str(boost::format("HTTP error: %1% (status: %2%)") % error % status);
                    BOOST_LOG_TRIVIAL(error) << "DownloadManager: " << error_msg;
                    Slic3r::flush_logs();
                    task->state = DownloadTaskState::Error;
                    task->error_message = error;
                    send_error_update(task, error);
                    cleanup_task(task->task_id);
                    m_active_downloads--;
                    process_queue();
                });
            });

            task->http_object = http.perform();

        } catch (std::exception& e) {
            std::string error_msg = std::string("Download exception: ") + e.what();
            BOOST_LOG_TRIVIAL(error) << "DownloadManager: " << error_msg;
            task->state = DownloadTaskState::Error;
            task->error_message = e.what();
            send_error_update(task, e.what());
            cleanup_task(task->task_id);
            m_active_downloads--;
            process_queue();
        }
    });
}

bool DownloadManager::cancel_download(size_t task_id) {
    std::shared_ptr<SSWCP_Instance> wcp_to_destroy;

    {
        std::lock_guard<std::mutex> lock(m_tasks_mutex);

        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) {
            for (auto qit = m_pending_queue.begin(); qit != m_pending_queue.end(); ++qit) {
                if (*qit == task_id) {
                    m_pending_queue.erase(qit);
                    return true;
                }
            }
            return false;
        }

        auto task = it->second;
        if (task->state == DownloadTaskState::Downloading) {
            task->state = DownloadTaskState::Canceled;
            if (task->http_object) {
                task->http_object->cancel();
            }
            std::string partial_path = task->dest_path;
            m_tasks.erase(task_id);
            m_last_percent.erase(task_id);
            m_last_update.erase(task_id);
            m_active_downloads--;
            if (!partial_path.empty()) {
                boost::system::error_code ec;
                boost::filesystem::remove(partial_path, ec);
            }
        } else if (task->state == DownloadTaskState::Pending) {
            for (auto qit = m_pending_queue.begin(); qit != m_pending_queue.end(); ++qit) {
                if (*qit == task_id) {
                    m_pending_queue.erase(qit);
                    break;
                }
            }
            m_tasks.erase(task_id);
            return true;
        } else {
            return false;
        }
    }

    if (wcp_to_destroy) {
        wcp_to_destroy->finish_job();
    }

    process_queue();

    return true;
}

bool DownloadManager::pause_download(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end() && it->second->state == DownloadTaskState::Downloading) {
        it->second->state = DownloadTaskState::Paused;
        return true;
    }
    return false;
}

bool DownloadManager::resume_download(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end() && it->second->state == DownloadTaskState::Paused) {
        return false;
    }
    return false;
}

DownloadTaskState DownloadManager::get_task_state(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) {
        return it->second->state;
    }
    return DownloadTaskState::Error;
}

std::shared_ptr<DownloadTask> DownloadManager::get_task(size_t task_id) {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) {
        return it->second;
    }
    return nullptr;
}

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
    if (task->callbacks.on_progress && task->state != DownloadTaskState::Canceled) {
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
    if (task->callbacks.on_complete && task->state != DownloadTaskState::Canceled) {
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
    if (task->callbacks.on_error && task->state != DownloadTaskState::Canceled) {
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
