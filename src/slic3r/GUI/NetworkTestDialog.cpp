#include "NetworkTestDialog.hpp"
#include "MainFrame.hpp"
#include "FirewallCheckDialog.hpp"
#include "I18N.hpp"

#include "libslic3r/Utils.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include <wx/regex.h>
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/log/trivial.hpp>
#include <chrono>
#include <vector>
#include <cstdio>
#include <memory>
#include <atomic>
#include <functional>
#include <mutex>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace Slic3r {
namespace GUI {


static wxString NA_STR = _L("N/A");

// Searchable tag for network test log entries in file logs.
#define NETWORK_TEST_LOG_TAG "[NetworkTest]"

// Upper bound for the TCP connect step. Without it a connect to an unreachable host blocks
// for the OS timeout (about 21 s on Windows) and cannot be interrupted.
static constexpr auto TCP_CONNECT_TIMEOUT = std::chrono::seconds(10);

// Lifetime model (Sentry EDGESLICER-2):
// The test worker threads used to run NetworkTestDialog member functions while holding a
// weak_ptr to a second, non-owning control block, so nothing kept the dialog alive. On close
// the threads were detached after a 1 s join timeout and the dialog was freed underneath a
// still-running test (e.g. a LAN TCP connect to an offline printer), which then crashed in
// update_status.
//
// Now every worker thread holds a shared_ptr to this runner and touches only the runner.
// The dialog is owned by wx / its creator as usual and is only ever dereferenced here on
// the main thread, after checking it has not been detached. close() (main thread, called
// from on_close and the dialog destructor) detaches the dialog under m_post_mutex, so no
// status can be posted afterwards and every running test winds down at its next check
// (HTTP and TCP connect are cancelled promptly). The runner has no wx windows, so it is
// fine for the last reference to be dropped on a worker thread. Closing never waits for a
// worker thread.
class NetworkTestRunner : public std::enable_shared_from_this<NetworkTestRunner>
{
public:
	explicit NetworkTestRunner(NetworkTestDialog* dialog) : m_dialog(dialog)
	{
		for (auto& flag : m_in_testing)
			flag.store(false);
	}

	// Main thread only. Idempotent.
	void close()
	{
		std::lock_guard<std::mutex> lock(m_post_mutex);
		m_closing.store(true);
		m_dialog = nullptr;
	}

	bool closing() const { return m_closing.load(); }

	bool is_testing(TestJob job) const { return m_in_testing[job].load(); }
	// Claims a job slot; false when that job is already running.
	bool begin(TestJob job) { return !m_in_testing[job].exchange(true); }
	void end(TestJob job) { m_in_testing[job].store(false); }

	// Guards against starting a second sequence run while one is in progress.
	std::atomic<bool> sequence_running{false};

	// Any thread. Silent no-op once closed.
	void update_status(int job_id, const wxString& info)
	{
		if (wxThread::IsMain()) {
			deliver(job_id, info);
			return;
		}
		// Hold the mutex across the post so close() cannot complete in between the
		// closing check and the post: once close() returns nothing new is queued.
		std::lock_guard<std::mutex> lock(m_post_mutex);
		if (m_closing.load())
			return;
		wxGetApp().CallAfter([self = shared_from_this(), job_id, info]() { self->deliver(job_id, info); });
	}

	void log_section_header(const wxString& title)
	{
		static const wxString sep("========================================");
		update_status(-1, wxEmptyString);
		update_status(-1, sep);
		update_status(-1, title);
		update_status(-1, sep);
		update_status(-1, wxEmptyString);
	}

	// Runs fn on a new thread that keeps this runner (never the dialog) alive. The thread
	// is not joined: it only touches the runner, so it may safely outlive the dialog.
	// Returns false if the thread could not be started.
	bool run_async(std::function<void(NetworkTestRunner&)> fn)
	{
		if (closing())
			return false;
		try {
			boost::thread worker([self = shared_from_this(), fn]() {
				try {
					fn(*self);
				} catch (const std::exception& e) {
					BOOST_LOG_TRIVIAL(error) << NETWORK_TEST_LOG_TAG << " test thread failed: " << e.what();
				} catch (...) {
					BOOST_LOG_TRIVIAL(error) << NETWORK_TEST_LOG_TAG << " test thread failed: unknown exception";
				}
			});
			worker.detach();
			return true;
		} catch (const std::exception& e) {
			BOOST_LOG_TRIVIAL(error) << NETWORK_TEST_LOG_TAG << " could not start test thread: " << e.what();
			return false;
		}
	}

	// Test bodies. They run on worker threads (never touch the dialog directly).
	void test_url(TestJob job, wxString name, wxString url);
	void test_telnet(TestJob job, wxString name, wxString server, int port);
	void test_ping(wxString server);

private:
	// Main thread only.
	void deliver(int job_id, const wxString& info)
	{
		if (m_closing.load() || m_dialog == nullptr)
			return;
		m_dialog->apply_status(job_id, info);
	}

	NetworkTestDialog* m_dialog; // main thread only; nulled by close()
	std::mutex         m_post_mutex;
	std::atomic<bool>  m_closing{false};
	std::atomic<bool>  m_in_testing[TEST_JOB_MAX];
};

NetworkTestDialog::NetworkTestDialog(wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style)
    : DPIDialog(parent,wxID_ANY,from_u8((boost::format(_utf8(L("Network Test")))).str()),wxDefaultPosition,
            wxSize(1000, 700),
            /*wxCAPTION*/wxDEFAULT_DIALOG_STYLE|wxMAXIMIZE_BOX|wxMINIMIZE_BOX|wxRESIZE_BORDER)
{
	m_runner = std::make_shared<NetworkTestRunner>(this);
    this->SetBackgroundColour(wxColour(255, 255, 255));

	this->SetSizeHints(wxDefaultSize, wxDefaultSize);

	wxBoxSizer* main_sizer;
	main_sizer = new wxBoxSizer(wxVERTICAL);

	wxBoxSizer* top_sizer = create_top_sizer(this);
	main_sizer->Add(top_sizer, 0, wxEXPAND, 5);

	wxBoxSizer* info_sizer = create_info_sizer(this);
	main_sizer->Add(info_sizer, 0, wxEXPAND, 5);

	wxBoxSizer* content_sizer = create_content_sizer(this);
	main_sizer->Add(content_sizer, 0, wxEXPAND, 5);

	wxBoxSizer* result_sizer = create_result_sizer(this);
	main_sizer->Add(result_sizer, 1, wxEXPAND, 5);

	set_default();

	init_bind();

	this->SetSizer(main_sizer);
	this->Layout();

	this->Centre(wxBOTH);
    wxGetApp().UpdateDlgDarkUI(this);
}

wxBoxSizer* NetworkTestDialog::create_top_sizer(wxWindow* parent)
{
	auto sizer = new wxBoxSizer(wxVERTICAL);

	auto line_sizer = new wxBoxSizer(wxHORIZONTAL);
	btn_start = new Button(this, _L("Start Test Multi-Thread"));
    btn_start->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
	line_sizer->Add(btn_start, 0, wxALL, 5);

	btn_start_sequence = new Button(this, _L("Start Test Single-Thread"));
    btn_start_sequence->SetStyle(ButtonStyle::Regular, ButtonType::Window);

	line_sizer->Add(btn_start_sequence, 0, wxALL, 5);

	btn_download_log = new Button(this, _L("Export Log"));
    btn_download_log->SetStyle(ButtonStyle::Regular, ButtonType::Window);
	line_sizer->Add(btn_download_log, 0, wxALL, 5);
	btn_download_log->Hide();

	btn_clear_log = new Button(this, _L("Clear Log"));
    btn_clear_log->SetStyle(ButtonStyle::Regular, ButtonType::Window);
	line_sizer->Add(btn_clear_log, 0, wxALL, 5);

	// Printers missing from the Device list are most often Windows Firewall dropping their
	// discovery broadcasts, which none of the tests here can see.
	if (FirewallCheckDialog::supported()) {
		auto* btn_firewall = new Button(this, _L("Check Windows Firewall..."));
		btn_firewall->SetStyle(ButtonStyle::Regular, ButtonType::Window);
		line_sizer->Add(btn_firewall, 0, wxALL, 5);
		btn_firewall->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { FirewallCheckDialog::show_modal(this); });
	}

	// Button handlers run on the main thread while the dialog (their parent) is alive.
	btn_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) { start_all_job(); });
	btn_start_sequence->Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) { start_all_job_sequence(); });
	btn_clear_log->Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
		if (txt_log)
			txt_log->Clear();
	});
	sizer->Add(line_sizer, 0, wxEXPAND, 5);
	return sizer;
}

wxBoxSizer* NetworkTestDialog::create_info_sizer(wxWindow* parent)
{
	auto sizer = new wxBoxSizer(wxVERTICAL);

	text_basic_info = new wxStaticText(this, wxID_ANY, _L("Basic Info"), wxDefaultPosition, wxDefaultSize, 0);
	text_basic_info->Wrap(-1);
	sizer->Add(text_basic_info, 0, wxALL, 5);

	wxBoxSizer* version_sizer = new wxBoxSizer(wxHORIZONTAL);
	text_version_title = new wxStaticText(this, wxID_ANY, _L("EdgeSlicer Version:"), wxDefaultPosition, wxDefaultSize, 0);
	text_version_title->Wrap(-1);
	version_sizer->Add(text_version_title, 0, wxALL, 5);

	wxString text_version = get_studio_version();
	text_version_val = new wxStaticText(this, wxID_ANY, text_version, wxDefaultPosition, wxDefaultSize, 0);
	text_version_val->Wrap(-1);
	version_sizer->Add(text_version_val, 0, wxALL, 5);
	sizer->Add(version_sizer, 1, wxEXPAND, 5);

	wxBoxSizer* sys_sizer = new wxBoxSizer(wxHORIZONTAL);

	txt_sys_info_title = new wxStaticText(this, wxID_ANY, _L("System Version:"), wxDefaultPosition, wxDefaultSize, 0);
	txt_sys_info_title->Wrap(-1);
	sys_sizer->Add(txt_sys_info_title, 0, wxALL, 5);

	txt_sys_info_value = new wxStaticText(this, wxID_ANY, get_os_info(), wxDefaultPosition, wxDefaultSize, 0);
	txt_sys_info_value->Wrap(-1);
	sys_sizer->Add(txt_sys_info_value, 0, wxALL, 5);

	sizer->Add(sys_sizer, 1, wxEXPAND, 5);

	wxBoxSizer* line_sizer = new wxBoxSizer(wxHORIZONTAL);
	txt_dns_info_title = new wxStaticText(this, wxID_ANY, _L("DNS Server:"), wxDefaultPosition, wxDefaultSize, 0);
	txt_dns_info_title->Wrap(-1);
	txt_dns_info_title->Hide();
	line_sizer->Add(txt_dns_info_title, 0, wxALL, 5);

	txt_dns_info_value = new wxStaticText(this, wxID_ANY, get_dns_info(), wxDefaultPosition, wxDefaultSize, 0);
	txt_dns_info_value->Hide();
	line_sizer->Add(txt_dns_info_value, 0, wxALL, 5);
	sizer->Add(line_sizer, 1, wxEXPAND, 5);

	return sizer;
}

wxBoxSizer* NetworkTestDialog::create_content_sizer(wxWindow* parent)
{
	auto sizer = new wxBoxSizer(wxVERTICAL);

	wxFlexGridSizer* grid_sizer;
	grid_sizer = new wxFlexGridSizer(0, 3, 0, 0);
	grid_sizer->SetFlexibleDirection(wxBOTH);
	grid_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

	btn_link = new Button(this, _L("Test EdgeSlicer(GitHub)"));
    btn_link->SetStyle(ButtonStyle::Regular, ButtonType::Window);
	grid_sizer->Add(btn_link, 0, wxEXPAND | wxALL, 5);

	text_link_title = new wxStaticText(this, wxID_ANY, _L("Test EdgeSlicer(GitHub):"), wxDefaultPosition, wxDefaultSize, 0);
	text_link_title->Wrap(-1);
	grid_sizer->Add(text_link_title, 0, wxALIGN_RIGHT | wxALL | wxALIGN_CENTER_VERTICAL, 5);

	text_link_val = new wxStaticText(this, wxID_ANY, _L("N/A"), wxDefaultPosition, wxDefaultSize, 0);
	text_link_val->Wrap(-1);
	grid_sizer->Add(text_link_val, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

	btn_bing = new Button(this, _L("Test bing.com"));
    btn_bing->SetStyle(ButtonStyle::Regular, ButtonType::Window);
	grid_sizer->Add(btn_bing, 0, wxEXPAND | wxALL, 5);

    text_bing_title = new wxStaticText(this, wxID_ANY, _L("Test bing.com:"), wxDefaultPosition, wxDefaultSize, 0);

	text_bing_title->Wrap(-1);
	grid_sizer->Add(text_bing_title, 0, wxALIGN_RIGHT | wxALL | wxALIGN_CENTER_VERTICAL, 5);

	text_bing_val = new wxStaticText(this, wxID_ANY, _L("N/A"), wxDefaultPosition, wxDefaultSize, 0);
	text_bing_val->Wrap(-1);
	grid_sizer->Add(text_bing_val, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

	// LAN Device Test
	btn_lan_mqtt = new Button(this, _L("Test LAN Device"));
    btn_lan_mqtt->SetStyle(ButtonStyle::Regular, ButtonType::Window);
	grid_sizer->Add(btn_lan_mqtt, 0, wxEXPAND | wxALL, 5);

	text_lan_mqtt_title = new wxStaticText(this, wxID_ANY, _L("Test LAN Device:"), wxDefaultPosition, wxDefaultSize, 0);
	text_lan_mqtt_title->Wrap(-1);
	grid_sizer->Add(text_lan_mqtt_title, 0, wxALIGN_RIGHT | wxALL | wxALIGN_CENTER_VERTICAL, 5);

	text_lan_mqtt_val = new wxStaticText(this, wxID_ANY, _L("N/A"), wxDefaultPosition, wxDefaultSize, 0);
	text_lan_mqtt_val->Wrap(-1);
	grid_sizer->Add(text_lan_mqtt_val, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

	// Cloud Server Test
	btn_cloud_mqtt = new Button(this, _L("Test Cloud Server"));
    btn_cloud_mqtt->SetStyle(ButtonStyle::Regular, ButtonType::Window);
	grid_sizer->Add(btn_cloud_mqtt, 0, wxEXPAND | wxALL, 5);

	text_cloud_mqtt_title = new wxStaticText(this, wxID_ANY, _L("Test Cloud Server:"), wxDefaultPosition, wxDefaultSize, 0);
	text_cloud_mqtt_title->Wrap(-1);
	grid_sizer->Add(text_cloud_mqtt_title, 0, wxALIGN_RIGHT | wxALL | wxALIGN_CENTER_VERTICAL, 5);

	text_cloud_mqtt_val = new wxStaticText(this, wxID_ANY, _L("N/A"), wxDefaultPosition, wxDefaultSize, 0);
	text_cloud_mqtt_val->Wrap(-1);
	grid_sizer->Add(text_cloud_mqtt_val, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

	// Login API Test
	btn_login_api = new Button(this, _L("Test Login API"));
    btn_login_api->SetStyle(ButtonStyle::Regular, ButtonType::Window);
	grid_sizer->Add(btn_login_api, 0, wxEXPAND | wxALL, 5);

	text_login_api_title = new wxStaticText(this, wxID_ANY, _L("Test Login API:"), wxDefaultPosition, wxDefaultSize, 0);
	text_login_api_title->Wrap(-1);
	grid_sizer->Add(text_login_api_title, 0, wxALIGN_RIGHT | wxALL | wxALIGN_CENTER_VERTICAL, 5);

	text_login_api_val = new wxStaticText(this, wxID_ANY, _L("N/A"), wxDefaultPosition, wxDefaultSize, 0);
	text_login_api_val->Wrap(-1);
	grid_sizer->Add(text_login_api_val, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

	// Upload API Test
	btn_upload_api = new Button(this, _L("Test Upload API"));
    btn_upload_api->SetStyle(ButtonStyle::Regular, ButtonType::Window);
	grid_sizer->Add(btn_upload_api, 0, wxEXPAND | wxALL, 5);

	text_upload_api_title = new wxStaticText(this, wxID_ANY, _L("Test Upload API:"), wxDefaultPosition, wxDefaultSize, 0);
	text_upload_api_title->Wrap(-1);
	grid_sizer->Add(text_upload_api_title, 0, wxALIGN_RIGHT | wxALL | wxALIGN_CENTER_VERTICAL, 5);

	text_upload_api_val = new wxStaticText(this, wxID_ANY, _L("N/A"), wxDefaultPosition, wxDefaultSize, 0);
	text_upload_api_val->Wrap(-1);
	grid_sizer->Add(text_upload_api_val, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

	sizer->Add(grid_sizer, 1, wxEXPAND, 5);

	btn_link->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { start_test_github_thread(); });
	btn_bing->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { start_test_bing_thread(); });
	btn_lan_mqtt->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { start_test_lan_mqtt_thread(); });
	btn_cloud_mqtt->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { start_test_cloud_mqtt_thread(); });
	btn_login_api->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { start_test_login_api_thread(); });
	btn_upload_api->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { start_test_upload_api_thread(); });

	return sizer;
}
wxBoxSizer* NetworkTestDialog::create_result_sizer(wxWindow* parent)
{
	auto sizer = new wxBoxSizer(wxVERTICAL);
	text_result = new wxStaticText(this, wxID_ANY, _L("Log Info"), wxDefaultPosition, wxDefaultSize, 0);
	text_result->Wrap(-1);
	sizer->Add(text_result, 0, wxALL, 5);

	txt_log = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE);
	sizer->Add(txt_log, 1, wxALL | wxEXPAND, 5);
	return sizer;
}

NetworkTestDialog::~NetworkTestDialog()
{
	// Detach from the runner: running tests keep only the runner alive and wind down on
	// their own; their status updates become no-ops. Never blocks.
	m_runner->close();
}

void NetworkTestDialog::init_bind()
{
	Bind(wxEVT_CLOSE_WINDOW, &NetworkTestDialog::on_close, this);
}

wxString NetworkTestDialog::get_os_info()
{
	int major = 0, minor = 0, micro = 0;
	wxGetOsVersion(&major, &minor, &micro);
	std::string os_version = (boost::format("%1%.%2%.%3%") % major % minor % micro).str();
	wxString text_sys_version = wxGetOsDescription() + wxString::Format("%d.%d.%d", major, minor, micro);
	return text_sys_version;
}


wxString NetworkTestDialog::get_dns_info()
{
	return NA_STR;
}

void NetworkTestDialog::start_all_job()
{
	log_section_header(_L("Network test started"));

	start_test_github_thread();
	start_test_bing_thread();
	start_test_lan_mqtt_thread();
	start_test_cloud_mqtt_thread();
	start_test_login_api_thread();
	start_test_upload_api_thread();
}

void NetworkTestDialog::start_all_job_sequence()
{
	if (m_runner->sequence_running.exchange(true)) {
		update_status(-1, "Sequence test already running, please wait...");
		return;
	}

	// 在序列测试开始前，先弹出输入框获取局域网设备IP
	wxTextEntryDialog dlg(this,
		_L("Please enter the LAN device IP address for testing (leave empty to skip):"),
		_L("LAN Device Test - Sequence Mode"),
		"192.168.1.1",
		wxOK | wxCANCEL);

	wxString device_ip;
	if (dlg.ShowModal() == wxID_OK) {
		device_ip = dlg.GetValue().Trim();
	}

	// Read everything that needs the app (config, region) here on the main thread; the
	// worker only touches the runner.
	const wxString cloud_server = get_cloud_server_address();
	const std::string region = wxGetApp().app_config->get("region");
	const bool china = (region == "Chinese Mainland" || region == "China");
	const wxString login_api_url = china ? "https://id.snapmaker.cn" : "https://id.snapmaker.com";
	const wxString upload_api_url = china ? "https://public.resource.snapmaker.cn" : "https://public.resource.snapmaker.com";

	const bool started = m_runner->run_async([device_ip, cloud_server, login_api_url, upload_api_url](NetworkTestRunner& r) {
		// Always release the sequence slot, whichever way the run ends.
		struct SequenceGuard {
			NetworkTestRunner& r;
			~SequenceGuard() { r.sequence_running.store(false); }
		} guard{r};

		r.log_section_header("Start sequence test (single-thread mode)");

		r.test_url(TEST_BING_JOB, "Bing", "http://www.bing.com");
		if (r.closing()) return;

		r.update_status(-1, "");
		r.test_url(TEST_ORCA_JOB, "EdgeSlicer(GitHub)", "https://github.com/aceRage/EdgeSlicer");
		if (r.closing()) return;

		// 如果用户输入了局域网设备IP，则进行测试
		if (!device_ip.IsEmpty()) {
			r.update_status(-1, "");
			r.test_telnet(TEST_LAN_MQTT_JOB, "LAN Device", device_ip, 1884);
			if (r.closing()) return;
		}

		// 测试云服务器
		if (!cloud_server.IsEmpty()) {
			r.update_status(-1, "");
			r.test_telnet(TEST_CLOUD_MQTT_JOB, "Cloud Server", cloud_server, 8883);
		}
		if (r.closing()) return;

		// 测试登录API
		r.update_status(-1, "");
		r.test_url(TEST_LOGIN_API_JOB, "Login API", login_api_url);
		if (r.closing()) return;

		// 测试上传API
		r.update_status(-1, "");
		r.test_url(TEST_UPLOAD_API_JOB, "Upload API", upload_api_url);
		if (r.closing()) return;

		r.log_section_header("Sequence test completed");
	});
	if (!started)
		m_runner->sequence_running.store(false);
}

void NetworkTestRunner::test_url(TestJob job, wxString name, wxString url)
{
	m_in_testing[job].store(true);

	update_status(-1, "");
	update_status(-1, "========================================");
	wxString info = "test " + name + " start...";
	update_status(job, info);

	Slic3r::Http http = Slic3r::Http::get(url.ToStdString());
	info = "[test " + name + "]: url=" + url;
    update_status(-1, info);
	update_status(-1, "");

    int result = -1;
	// The callbacks run synchronously on this worker thread inside perform_sync(), which
	// keeps the runner alive, so capturing `this` is safe.
	http.timeout_max(10)
		.on_progress([this](Http::Progress, bool& cancel) {
			// Abort promptly when the dialog closes instead of running to the timeout.
			if (closing())
				cancel = true;
		})
		.on_complete([this, &result, job](std::string body, unsigned status) {
			if (closing()) return;
			try {
				if (status == 200) {
					result = 0;
				}
				// Upload API: 403 is OK (HTTPS resource with permission check)
				else if (job == TEST_UPLOAD_API_JOB && status == 403) {
					result = 0;
				}
			}
			catch (...) {
				;
			}
		})
		.on_ip_resolve([this, name, job](std::string ip) {
			if (closing()) return;
			wxString ip_report = "test " + name + " ip resolved = " + wxString::FromUTF8(ip);
			update_status(job, ip_report);
		})
		.on_error([this, name, job](std::string body, std::string error, unsigned int status) {
		if (closing()) return;
		// Upload API: 403 is OK (HTTPS resource with permission check)
		if (job == TEST_UPLOAD_API_JOB && status == 403) {
			update_status(job, "test " + name + " ok (403 - access restricted, but server reachable)");
			return;
		}
		wxString info = wxString::Format("status=%u, body=", status) + wxString::FromUTF8(body) + ", error=" + wxString::FromUTF8(error);
        update_status(job, "test " + name + " failed");
        update_status(-1, info);
	}).perform_sync();

	if (result == 0) {
        update_status(job, "test " + name + " ok");
    }

	update_status(-1, "========================================");
	update_status(-1, "");
	m_in_testing[job].store(false);
}

void NetworkTestRunner::test_ping(wxString server)
{
	update_status(-1, "");
	update_status(-1, "Starting ping test to " + server + "...");

	try {
#ifdef _WIN32
		// Windows: ping -n 4 <server>
		wxString ping_cmd = "ping -n 4 " + server;
		// Windows 可以使用 wxExecute
		wxArrayString output;
		wxArrayString errors;
		long exec_flags = wxEXEC_SYNC | wxEXEC_NODISABLE | wxEXEC_HIDE_CONSOLE;
		long result = wxExecute(ping_cmd, output, errors, exec_flags);
#else
		// Linux/Mac: ping -c 4 <server>
		// 使用 popen() 而不是 wxExecute，因为 wxExecute 在 macOS 上不能在异步线程中使用
		std::string ping_cmd = "ping -c 4 " + server.ToStdString();
		wxArrayString output;
		long result = -1;
		
		FILE* pipe = popen(ping_cmd.c_str(), "r");
		if (pipe != nullptr) {
			char buffer[1024];
			while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
				// 转换为 wxString 并添加到 output
				wxString line = wxString::FromUTF8(buffer);
				line.Trim();  // 移除换行符
				if (!line.IsEmpty()) {
					output.Add(line);
				}
			}
			int pclose_result = pclose(pipe);
			// pclose 返回的是命令的退出状态，需要检查 WEXITSTATUS
			if (WIFEXITED(pclose_result)) {
				result = WEXITSTATUS(pclose_result);
			} else {
				result = -1;
			}
		}
#endif

		if (result == 0 && output.GetCount() > 0) {
			// 解析ping输出（不输出每一行，减少UI更新）
			bool found_rtt = false;
			wxString rtt_info;
			int received = 0;
			int sent = 4;
			
			// macOS需要收集所有time值来计算平均值
			std::vector<double> time_values;

			for (size_t i = 0; i < output.GetCount(); i++) {
				wxString line = output[i];

				// 完全不输出ping详细日志，只解析数据

#ifdef _WIN32
				// Windows: 通用解析（支持所有语言）

				// 策略1: 查找统计行（包含多个 "= 数字" 的模式）
				// 这一行通常是：数据包: 已发送 = 4，已接收 = 4，丢失 = 0
				// 或：Packets: Sent = 4, Received = 4, Lost = 0
				// 特征：一行中有至少3个 "= 数字" 模式
				if (received == 0 && line.Contains("=") && !line.Contains("TTL")) {
					// 提取所有 "= 数字" 模式
					wxArrayString numbers;
					size_t pos = 0;
					while (pos < line.Length()) {
						// 查找等号
						size_t eq_pos = line.find('=', pos);
						if (eq_pos == wxString::npos) break;

						// 跳过等号后的空格
						size_t num_start = eq_pos + 1;
						while (num_start < line.Length() &&
						       (line[num_start] == ' ' || line[num_start] == '\t')) {
							num_start++;
						}

						// 提取数字
						wxString num_str;
						size_t num_pos = num_start;
						while (num_pos < line.Length() && wxIsdigit(line[num_pos])) {
							num_str += line[num_pos];
							num_pos++;
						}

						if (!num_str.IsEmpty()) {
							numbers.Add(num_str);
						}

						pos = num_pos + 1;
					}

					// 统计行特征：至少有3个数字（sent, received, lost）
					// 第2个数字是received（已接收）
					if (numbers.GetCount() >= 3) {
						long val;
						if (numbers[1].ToLong(&val)) {
							received = val;
						}
					}
				}

				// 策略2: 查找平均RTT行（包含 "平均" 或 "Average" 或多个ms）
				// 格式：最短 = 42ms，最长 = 45ms，平均 = 43ms
				// 或：Minimum = 42ms, Maximum = 45ms, Average = 43ms
				if (!found_rtt) {
					// 检查是否是平均/Average行（包含多个ms的行）
					int ms_count = 0;
					size_t search_pos = 0;
					while ((search_pos = line.find("ms", search_pos)) != wxString::npos) {
						ms_count++;
						search_pos += 2;
					}

					// 如果一行中有3个ms，很可能是RTT统计行
					// 提取最后一个数字作为平均RTT（避免显示乱码）
					if (ms_count >= 3) {
						wxString avg_rtt;
						// 从后往前找最后一个 "数字ms" 模式
						for (int i = line.Length() - 1; i >= 1; i--) {
							if (line[i] == 's' && line[i-1] == 'm') {
								// 找到ms，往前提取数字
								wxString num_str;
								int j = i - 2;
								while (j >= 0 && (wxIsdigit(line[j]) || line[j] == '.')) {
									num_str = line[j] + num_str;
									j--;
								}
								if (!num_str.IsEmpty()) {
									avg_rtt = num_str;
									break;
								}
							}
						}

						if (!avg_rtt.IsEmpty()) {
							found_rtt = true;
							rtt_info = avg_rtt + "ms (average)";
						}
					}
				}
#else
				// macOS格式: "64 bytes from 13.35.32.150: icmp_seq=519 ttl=244 time=57.030 ms"
				// 需要从每行提取 time=XX.XXX ms 并收集所有值
				if (line.Contains("time=")) {
					int time_pos = line.Find("time=");
					if (time_pos != wxNOT_FOUND) {
						wxString after_time = line.Mid(time_pos + 5);  // 跳过 "time="
						wxString time_str;
						// 提取数字（可能包含小数点）
						for (size_t j = 0; j < after_time.Length(); j++) {
							wxChar c = after_time[j];
							if (wxIsdigit(c) || c == '.') {
								time_str += c;
							} else if (c == ' ' || c == 'm') {
								// 遇到空格或 'm' (ms) 停止
								break;
							}
						}
						if (!time_str.IsEmpty()) {
							double time_val;
							if (time_str.ToDouble(&time_val)) {
								time_values.push_back(time_val);
								found_rtt = true;
							}
						}
					}
				}
				
				// Linux格式: "rtt min/avg/max/mdev = 1.234/5.678/9.012/1.234 ms"
				if (!found_rtt && line.Contains("rtt") && line.Contains("avg")) {
					found_rtt = true;
					// 提取平均值
					int avg_pos = line.Find("avg");
					if (avg_pos != wxNOT_FOUND) {
						wxString after_avg = line.Mid(avg_pos + 3);
						wxString num_str;
						size_t i = 0;
						// 跳过空格和等号
						while (i < after_avg.Length() && (after_avg[i] == ' ' || after_avg[i] == '\t' || after_avg[i] == '=')) {
							i++;
						}
						// 提取数字直到遇到斜杠或空格
						while (i < after_avg.Length() && (wxIsdigit(after_avg[i]) || after_avg[i] == '.')) {
							num_str += after_avg[i];
							i++;
						}
						if (!num_str.IsEmpty()) {
							rtt_info = num_str + "ms (average)";
						} else {
							rtt_info = line;
						}
					} else {
						rtt_info = line;
					}
				}
				
				// 统计格式: "4 packets transmitted, 4 received" (Linux/macOS通用)
				if (line.Contains("packets transmitted") && line.Contains("received")) {
					int pos_received = line.Find(" received");
					if (pos_received != wxNOT_FOUND) {
						wxString before = line.Mid(0, pos_received);
						wxString num_str;
						for (int j = before.Length() - 1; j >= 0; j--) {
							if (wxIsdigit(before[j])) {
								num_str = before[j] + num_str;
							} else if (!num_str.IsEmpty()) {
								break;
							}
						}
						long val;
						if (!num_str.IsEmpty() && num_str.ToLong(&val)) {
							received = val;
						}
					}
				}
				
				// macOS 可能还有统计行: "round-trip min/avg/max/stddev = 1.234/5.678/9.012/1.234 ms"
				if (!found_rtt && line.Contains("round-trip") && line.Contains("avg")) {
					found_rtt = true;
					int avg_pos = line.Find("avg");
					if (avg_pos != wxNOT_FOUND) {
						wxString after_avg = line.Mid(avg_pos + 3);
						wxString num_str;
						size_t i = 0;
						while (i < after_avg.Length() && (after_avg[i] == ' ' || after_avg[i] == '\t' || after_avg[i] == '=')) {
							i++;
						}
						while (i < after_avg.Length() && (wxIsdigit(after_avg[i]) || after_avg[i] == '.')) {
							num_str += after_avg[i];
							i++;
							if (i < after_avg.Length() && after_avg[i] == '/') break;  // 遇到斜杠停止
						}
						if (!num_str.IsEmpty()) {
							rtt_info = num_str + "ms (average)";
						} else {
							rtt_info = line;
						}
					} else {
						rtt_info = line;
					}
				}
#endif
			}

			// 如果收集了多个time值（macOS），计算平均值
			if (!time_values.empty()) {
				double sum = 0.0;
				for (double val : time_values) {
					sum += val;
				}
				double avg = sum / time_values.size();
				rtt_info = wxString::Format("%.3fms (average from %zu samples)", avg, time_values.size());
			} else if (found_rtt && rtt_info.IsEmpty()) {
				rtt_info = "N/A";
			}

			// 计算丢包率
			int packet_loss = ((sent - received) * 100) / sent;

			// 一次性输出所有结果，减少UI更新次数
			wxString summary = "\n";

			// 检查是否成功解析
			bool parse_success = (found_rtt || received > 0);

			if (parse_success) {
				// 成功解析了ping结果
				if (found_rtt && !rtt_info.IsEmpty()) {
					summary += "[OK] Ping round-trip time: " + rtt_info + "\n";
				}
				summary += wxString::Format("Packet loss: %d%% (%d/%d received)\n", packet_loss, received, sent);

				if (received > 0) {
					summary += wxString::Format("[OK] Ping test successful (%d/%d packets)", received, sent);
				} else {
					summary += "[FAIL] Ping test failed - 100% packet loss";
				}
			} else {
				// 无法解析ping输出
				summary += "[WARN] Cannot parse ping output - unusual format\n";
				summary += "[INFO] TCP connection test can still verify network connectivity";
			}

			update_status(-1, summary);

		} else {
			wxString error_summary = "\n[FAIL] Ping command failed or timed out";
#ifdef _WIN32
			for (size_t i = 0; i < errors.GetCount(); i++) {
				error_summary += "\nError: " + errors[i];
			}
#else
			if (output.GetCount() > 0) {
				error_summary += "\nOutput:\n";
				for (size_t i = 0; i < output.GetCount() && i < 5; i++) {
					error_summary += "  " + output[i] + "\n";
				}
			} else {
				error_summary += "\nNo output from ping command";
			}
#endif
			update_status(-1, error_summary);
		}

	} catch (const std::exception& e) {
		update_status(-1, wxString("\nPing exception: ") + wxString(e.what()));
	} catch (...) {
		update_status(-1, "\nPing test failed: unknown error");
	}
}

void NetworkTestRunner::test_telnet(TestJob job, wxString name, wxString server, int port)
{
	m_in_testing[job].store(true);

	// 添加分隔空行
	update_status(-1, "");
	update_status(-1, "========================================");

	wxString info = "test " + name + " start...";
	update_status(job, info);

	try {
		info = "[test " + name + "]: server=" + server + wxString::Format(", port=%d", port);
		update_status(-1, info);
		update_status(-1, ""); // 空行

		// ============================================
		// 第一步: Ping测试 - 测量网络层RTT
		// ============================================
		update_status(-1, "--- Step 1: Network Layer Test (ICMP Ping) ---");
		test_ping(server);

		// The ping can take several seconds; skip the TCP step if the dialog closed meanwhile.
		if (closing()) {
			m_in_testing[job].store(false);
			return;
		}

		// 添加步骤间空行
		update_status(-1, "");

		// ============================================
		// 第二步: TCP连接测试 - 验证服务可用性
		// ============================================
		update_status(-1, "--- Step 2: Transport Layer Test (TCP Connection) ---");

		// 记录开始时间
		auto start_time = std::chrono::high_resolution_clock::now();

		boost::asio::io_context io_context;
		boost::asio::ip::tcp::socket socket(io_context);
		boost::asio::ip::tcp::resolver resolver(io_context);

		bool success = false;
		std::string error_msg;

		try {
			// 解析主机名
			auto resolve_start = std::chrono::high_resolution_clock::now();
			boost::asio::ip::tcp::resolver::results_type endpoints;

			try {
				endpoints = resolver.resolve(server.ToStdString(), std::to_string(port));
				auto resolve_end = std::chrono::high_resolution_clock::now();
				auto resolve_time = std::chrono::duration_cast<std::chrono::milliseconds>(resolve_end - resolve_start).count();

				update_status(-1, wxString::Format("DNS resolve time: %lld ms", resolve_time));
			} catch (const boost::system::system_error& e) {
				error_msg = (wxString("DNS resolve failed: ") + wxString(e.what())).ToStdString();
				throw;
			}

			// 连接到服务器
			auto connect_start = std::chrono::high_resolution_clock::now();
			boost::system::error_code ec;
			long long connect_time = 0;

			// 尝试连接到所有解析出的endpoint
			// Asynchronous connect (tries each resolved endpoint in turn) driven in short
			// slices, so it is bounded by TCP_CONNECT_TIMEOUT and cancelled as soon as the
			// dialog closes, instead of blocking for the OS connect timeout.
			bool connect_done = false;
			boost::asio::async_connect(socket, endpoints,
				[&connect_done, &ec](const boost::system::error_code& result, const boost::asio::ip::tcp::endpoint&) {
					ec = result;
					connect_done = true;
				});
			const auto connect_deadline = std::chrono::steady_clock::now() + TCP_CONNECT_TIMEOUT;
			bool timed_out = false;
			bool aborted = false;
			while (!connect_done) {
				io_context.run_for(std::chrono::milliseconds(100));
				if (connect_done)
					break;
				if (io_context.stopped()) {
					// No pending work but the handler has not run: should not happen.
					ec = boost::asio::error::operation_aborted;
					break;
				}
				if (!aborted && (closing() || std::chrono::steady_clock::now() >= connect_deadline)) {
					// Closing the socket makes the pending connect complete with operation_aborted.
					timed_out = !closing();
					aborted = true;
					boost::system::error_code ignored;
					socket.close(ignored);
				}
			}
			if (timed_out)
				ec = boost::asio::error::timed_out;

			if (closing()) {
				m_in_testing[job].store(false);
				return;
			}

			bool connected = !ec;
			if (connected) {
				auto connect_end = std::chrono::high_resolution_clock::now();
				connect_time = std::chrono::duration_cast<std::chrono::milliseconds>(connect_end - connect_start).count();

				update_status(job, "test " + name + " connected");
				update_status(-1, wxString::Format("[OK] TCP connection established in %lld ms", connect_time));
			}

			if (!connected) {
				error_msg = (wxString("Connection failed: ") + wxString(ec.message())).ToStdString();
				throw boost::system::system_error(ec);
			}

			// 计算总时间
			auto end_time = std::chrono::high_resolution_clock::now();
			auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

			update_status(-1, wxString::Format("Total test time: %lld ms", total_time));

			// Calculate connection quality grade based on RTT
			int grade;
			wxString grade_desc;
			wxString grade_icon;

			if (connect_time <= 50) {
				grade = 1;
				grade_desc = "Excellent";
				grade_icon = "[Level 1]";
			} else if (connect_time <= 100) {
				grade = 2;
				grade_desc = "Good";
				grade_icon = "[Level 2]";
			} else if (connect_time <= 400) {
				grade = 3;
				grade_desc = "Fair";
				grade_icon = "[Level 3]";
			} else {
				grade = 4;
				grade_desc = "Poor";
				grade_icon = "[Level 4]";
			}

			// 添加空行
			update_status(-1, "");
			update_status(-1, "--- Test Summary ---");
			update_status(-1, "[OK] Network Layer: Ping test completed (see RTT above)");
			update_status(-1, wxString::Format("[OK] Transport Layer: TCP port %d is open and accepting connections", port));
			update_status(-1, wxString::Format("Connection Quality: %s %s (RTT: %lld ms)", grade_icon, grade_desc, connect_time));
			update_status(job, wxString::Format("test %s ok (%s - %s, RTT: %lld ms)", name, grade_icon, grade_desc, connect_time));

			success = true;

			// 关闭连接
			socket.close(ec);

		} catch (const boost::system::system_error& e) {
			if (error_msg.empty()) {
				error_msg = e.what();
			}
			update_status(-1, "");
			update_status(-1, "[FAIL] TCP connection error: " + wxString::FromUTF8(error_msg));
			update_status(-1, "Connection Quality: [Level 5] Failed (timeout or unreachable)");
			update_status(job, "test " + name + " failed ([Level 5] - timeout or unreachable)");
		} catch (const std::exception& e) {
			update_status(-1, "");
			update_status(-1, wxString("Exception: ") + wxString(e.what()));
			update_status(-1, "Connection Quality: [Level 5] Failed (exception)");
			update_status(job, "test " + name + " failed ([Level 5] - exception)");
		}

	} catch (...) {
		update_status(-1, "");
		update_status(-1, "Connection Quality: [Level 5] Failed (unknown error)");
		update_status(job, "test " + name + " failed ([Level 5] - unknown error)");
	}

	update_status(-1, "========================================");
	update_status(-1, ""); // 测试结束后的空行
	m_in_testing[job].store(false);
}

// Main thread. Claims the job slot and runs fn on a worker thread that holds only the
// runner. The slot is released when fn returns (or throws), or right away if the thread
// could not be started.
static void launch_job(const std::shared_ptr<NetworkTestRunner>& runner, TestJob job, std::function<void(NetworkTestRunner&)> fn)
{
	if (!runner->begin(job))
		return; // already running
	const bool started = runner->run_async([job, fn](NetworkTestRunner& r) {
		struct JobGuard {
			NetworkTestRunner& r;
			TestJob job;
			~JobGuard() { r.end(job); }
		} guard{r, job};
		fn(r);
	});
	if (!started)
		runner->end(job);
}

void NetworkTestDialog::start_test_lan_mqtt_thread()
{
	if (m_runner->is_testing(TEST_LAN_MQTT_JOB)) {
		return;
	}

	// 弹出对话框让用户输入IP地址
	wxTextEntryDialog dlg(this,
		_L("Please enter the device IP address:"),
		_L("LAN Device Test"),
		"192.168.1.1",
		wxOK | wxCANCEL);

	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	wxString device_ip = dlg.GetValue().Trim();
	if (device_ip.IsEmpty()) {
		update_status(TEST_LAN_MQTT_JOB, "Invalid IP address");
		return;
	}

	launch_job(m_runner, TEST_LAN_MQTT_JOB, [device_ip](NetworkTestRunner& r) {
		// 测试局域网设备 - 端口默认1884
		r.test_telnet(TEST_LAN_MQTT_JOB, "LAN Device", device_ip, 1884);
	});
}

wxString NetworkTestDialog::get_cloud_server_address()
{
	auto app_config = wxGetApp().app_config;
	std::string region = app_config->get("region");
    if (region == "Chinese Mainland")
        return "a1su7rk2r6cmbq.ats.iot.cn-north-1.amazonaws.com.cn";
    else
        return "a1pr8yczi3n0se-ats.iot.us-west-1.amazonaws.com";
}

void NetworkTestDialog::start_test_cloud_mqtt_thread()
{
	if (m_runner->is_testing(TEST_CLOUD_MQTT_JOB)) {
		return;
	}

	wxString cloud_server = get_cloud_server_address();

	if (cloud_server.IsEmpty()) {
		update_status(TEST_CLOUD_MQTT_JOB, "Cloud server not configured");
		update_status(-1, "Please configure cloud server address in get_cloud_server_address()");
		return;
	}

	launch_job(m_runner, TEST_CLOUD_MQTT_JOB, [cloud_server](NetworkTestRunner& r) {
		// 测试云服务器 - 使用telnet方式，端口8883
		r.test_telnet(TEST_CLOUD_MQTT_JOB, "Cloud Server", cloud_server, 8883);
	});
}
void NetworkTestDialog::start_test_github_thread()
{
	launch_job(m_runner, TEST_ORCA_JOB, [](NetworkTestRunner& r) {
		r.test_url(TEST_ORCA_JOB, "EdgeSlicer(GitHub)", "https://github.com/aceRage/EdgeSlicer");
	});
}

void NetworkTestDialog::start_test_bing_thread()
{
	launch_job(m_runner, TEST_BING_JOB, [](NetworkTestRunner& r) {
		r.test_url(TEST_BING_JOB, "Bing", "http://www.bing.com");
	});
}

void NetworkTestDialog::start_test_login_api_thread()
{
	// Read the region on the main thread; the worker never touches the app.
	std::string region = wxGetApp().app_config->get("region");
	wxString login_api_url = (region == "Chinese Mainland") ? "https://id.snapmaker.cn" : "https://id.snapmaker.com";
	launch_job(m_runner, TEST_LOGIN_API_JOB, [login_api_url](NetworkTestRunner& r) {
		r.test_url(TEST_LOGIN_API_JOB, "Login API", login_api_url);
	});
}

void NetworkTestDialog::start_test_upload_api_thread()
{
	std::string region = wxGetApp().app_config->get("region");
	wxString upload_api_url = (region == "Chinese Mainland") ? "https://public.resource.snapmaker.cn" : "https://public.resource.snapmaker.com";
	launch_job(m_runner, TEST_UPLOAD_API_JOB, [upload_api_url](NetworkTestRunner& r) {
		r.test_url(TEST_UPLOAD_API_JOB, "Upload API", upload_api_url);
	});
}

void NetworkTestDialog::on_close(wxCloseEvent& event)
{
	// Stop delivering status and let running tests wind down on their own. Nothing is
	// joined here, so closing never freezes the UI; the threads keep only the runner alive.
	m_runner->close();
	event.Skip();
}


wxString NetworkTestDialog::get_studio_version()
{
	return wxString(Snapmaker_VERSION);
}

void NetworkTestDialog::set_default()
{
	text_version_val->SetLabelText(get_studio_version());
	txt_sys_info_value->SetLabelText(get_os_info());
	txt_dns_info_value->SetLabelText(get_dns_info());
	text_link_val->SetLabelText(NA_STR);
	text_bing_val->SetLabelText(NA_STR);
	text_lan_mqtt_val->SetLabelText(NA_STR);
	text_cloud_mqtt_val->SetLabelText(NA_STR);
	text_login_api_val->SetLabelText(NA_STR);
	text_upload_api_val->SetLabelText(NA_STR);
}


void NetworkTestDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    ;
}

void NetworkTestDialog::log_section_header(const wxString& title)
{
	m_runner->log_section_header(title);
}

void NetworkTestDialog::update_status(int job_id, wxString info)
{
	m_runner->update_status(job_id, info);
}

void NetworkTestDialog::apply_status(int job_id, const wxString& info)
{
	// Main thread only, reached through NetworkTestRunner while the dialog is attached.
	// Send log to MainFrame for file writing
	auto log_evt = new wxCommandEvent(EVT_NETWORK_TEST_LOG_UPDATE);
	log_evt->SetString(info);
	wxQueueEvent(wxGetApp().mainframe, log_evt);

	switch (job_id)
	{
		case TEST_ORCA_JOB:
		{
			text_link_val->SetLabelText(info);
			break;
		}
		case TEST_BING_JOB:
		{
			text_bing_val->SetLabelText(info);
			break;
		}
		case TEST_LAN_MQTT_JOB:
		{
			text_lan_mqtt_val->SetLabelText(info);
			break;
		}
		case TEST_CLOUD_MQTT_JOB:
		{
			text_cloud_mqtt_val->SetLabelText(info);
			break;
		}
		case TEST_LOGIN_API_JOB:
		{
			text_login_api_val->SetLabelText(info);
			break;
		}
		case TEST_UPLOAD_API_JOB:
		{
			text_upload_api_val->SetLabelText(info);
			break;
		}
		default:
			if (job_id >= TEST_JOB_MAX)
				BOOST_LOG_TRIVIAL(warning) << "[NetworkTest] unknown job_id=" << job_id << ", info=" << into_u8(info);
			break;
	}

	std::time_t t = std::time(0);
	std::tm* now_time = std::localtime(&t);
	std::stringstream buf;
	buf << std::put_time(now_time, "%a %b %d %H:%M:%S");
	wxString log_line = wxString(buf.str()) + ": " + info + "\n";
	if (txt_log) {
		txt_log->AppendText(log_line);
	}
}

} // namespace GUI
} // namespace Slic3r


