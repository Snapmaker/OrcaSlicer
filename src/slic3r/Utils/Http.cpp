#include "Http.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <thread>
#include <deque>
#include <sstream>
#include <exception>
#include <vector>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string.hpp>

#include <curl/curl.h>

#ifdef OPENSSL_CERT_OVERRIDE
#include <openssl/x509.h>
#endif

namespace fs = boost::filesystem;

namespace Slic3r {

// Private

struct CurlGlobalInit
{
    static std::unique_ptr<CurlGlobalInit> instance;
    std::string message;

	CurlGlobalInit()
    {
#ifdef OPENSSL_CERT_OVERRIDE // defined if SLIC3R_STATIC=ON

        // Look for a set of distro specific directories. Don't change the
        // order: https://bugzilla.redhat.com/show_bug.cgi?id=1053882
        static const char * CA_BUNDLES[] = {
            "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL 6
            "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu/Gentoo etc.
            "/usr/share/ssl/certs/ca-bundle.crt",
            "/usr/local/share/certs/ca-root-nss.crt", // FreeBSD
            "/etc/ssl/cert.pem",
            "/etc/ssl/ca-bundle.pem"              // OpenSUSE Tumbleweed
        };

        namespace fs = boost::filesystem;
        // Env var name for the OpenSSL CA bundle (SSL_CERT_FILE nomally)
        const char *const SSL_CA_FILE = X509_get_default_cert_file_env();
        const char * ssl_cafile = ::getenv(SSL_CA_FILE);

        if (!ssl_cafile)
            ssl_cafile = X509_get_default_cert_file();

        int replace = true;
        if (!ssl_cafile || !fs::exists(fs::path(ssl_cafile))) {
            const char * bundle = nullptr;
            for (const char * b : CA_BUNDLES) {
                if (fs::exists(fs::path(b))) {
                    ::setenv(SSL_CA_FILE, bundle = b, replace);
                    break;
                }
            }

            if (!bundle)
                message = "Unable to get system certificate.";
            else
                message = (boost::format("use system SSL certificate: %1%") % bundle).str();

             message += "\n" + (boost::format("To manually specify the system certificate store, "
                                                   "set the %1% environment variable to the correct CA and restart the application") % SSL_CA_FILE).str();
        }
#endif // OPENSSL_CERT_OVERRIDE

        if (CURLcode ec = ::curl_global_init(CURL_GLOBAL_DEFAULT)) {
            message += "CURL initialization failed. See the log for additional details.";
            BOOST_LOG_TRIVIAL(error) << ::curl_easy_strerror(ec);
        }
    }

	~CurlGlobalInit() { ::curl_global_cleanup(); }
};

std::unique_ptr<CurlGlobalInit> CurlGlobalInit::instance;

std::map<std::string, std::string> extra_headers;
std::mutex g_mutex;

struct form_file
{
    fs::ifstream                          ifs;
    boost::filesystem::ifstream::off_type init_offset;
    size_t                                content_length;

    form_file(fs::path const& p, const boost::filesystem::ifstream::off_type offset, const size_t content_length)
        : ifs(p, std::ios::in | std::ios::binary), init_offset(offset), content_length(content_length)
    {}
};

struct Http::priv
{
	enum {
		DEFAULT_TIMEOUT_CONNECT = 10,
        DEFAULT_TIMEOUT_MAX = 0,
		DEFAULT_SIZE_LIMIT = 1024 * 1024 * 1024,
	};

	::CURL *curl;
	::curl_httppost *form;
	::curl_httppost *form_end;
	::curl_mime* mime;
	::curl_slist *headerlist;
	// Used for reading the body
	std::string buffer;
	// Used for storing file streams added as multipart form parts
	// Using a deque here because unlike vector it doesn't ivalidate pointers on insertion
	std::deque<form_file> form_files;
	std::string postfields;
	// Whether a body was set at all, as opposed to merely being empty. An empty body still
	// has to reach curl as CURLOPT_POSTFIELDS: a POST with neither POSTFIELDS nor a form
	// makes curl read the body from CURLOPT_READFUNCTION instead, and that callback here is
	// form_file_read_cb, whose CURLOPT_READDATA is only ever set for a PUT.
	bool postfields_set { false };
	std::string error_buffer;    // Used for CURLOPT_ERRORBUFFER
	std::string url;             // As given to the constructor; the certificate policy is decided on it
	Http::TlsPolicy tls_policy { Http::TlsPolicy::Auto };
	bool ca_file_set { false };  // ca_file() named a CA bundle: verify against it alone
	bool no_revoke { false };    // ssl_revoke_best_effort(true): CURLSSLOPT_NO_REVOKE
    std::string headers;
	size_t limit;
	bool cancel;
    std::unique_ptr<form_file> putFile;

	std::thread io_thread;
	Http::CompleteFn completefn;
	Http::ErrorFn errorfn;
	Http::ProgressFn progressfn;
	Http::IPResolveFn ipresolvefn;
	Http::HeaderCallbackFn headerfn;
	Http::BodyFn bodyfn;
	size_t body_seen { 0 };        // bytes handed to bodyfn
	bool body_refused { false };   // bodyfn said stop (as opposed to the size limit)
	bool follow_location { true };

	priv(const std::string &url);
	~priv();

	static bool ca_file_supported(::CURL *curl);
	static size_t writecb(void *data, size_t size, size_t nmemb, void *userp);
	static int xfercb(void *userp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);
	static int xfercb_legacy(void *userp, double dltotal, double dlnow, double ultotal, double ulnow);
	static size_t form_file_read_cb(char *buffer, size_t size, size_t nitems, void *userp);
    static size_t headers_cb(char *buffer, size_t size, size_t nitems, void *userp);

	void set_timeout_connect(long timeout);
    void set_timeout_max(long timeout);
	void form_add_file(const char *name, const fs::path &path, const char* filename, boost::filesystem::ifstream::off_type offset, size_t length);
	/* mime */
	void mime_form_add_text(const char* name, const char* value);
	void mime_form_add_file(const char* name, const char* path);
	void set_post_body(const fs::path &path);
	void set_post_body(const std::string &body);
	void set_put_body(const fs::path &path);
	void set_del_body(const std::string& body);
    void set_range(const std::string &range);

	std::string curl_error(CURLcode curlcode);
	std::string body_size_error();
	void http_perform();
};

// add a dummy log callback
static int log_trace(CURL* handle, curl_infotype type,
	char* data, size_t size,
	void* userp)
{
	return 0;
}

// Domains that should bypass proxy and connect directly
// This helps when users have Shadowsocks/Clash/V2Ray proxy enabled
static const char* NOPROXY_DOMAINS = 
    "snapmaker.cn,"
    "snapmaker.com,"
    "*.snapmaker.cn,"
    "*.snapmaker.com,"
    "localhost,"
    "127.0.0.1,"
    "::1";

#ifndef _WIN32
// A PEM CA bundle for OpenSSL on Linux and macOS: $SSL_CERT_FILE when it names a file, otherwise
// the first system bundle that exists. Empty when none does; libcurl then falls back to its own
// build-time default (and on Linux to OpenSSL's default paths, which CurlGlobalInit points at).
static const std::string& system_ca_bundle()
{
    static const std::string bundle = []() -> std::string {
        boost::system::error_code ec;
        if (const char *env = ::getenv("SSL_CERT_FILE"); env != nullptr && *env != '\0' && fs::exists(fs::path(env), ec))
            return env;
        static const char *const candidates[] = {
            "/etc/ssl/cert.pem",                     // macOS, Alpine, Arch, Fedora (link)
            "/etc/ssl/certs/ca-certificates.crt",    // Debian/Ubuntu/Gentoo
            "/etc/pki/tls/certs/ca-bundle.crt",      // Fedora/RHEL
            "/usr/share/ssl/certs/ca-bundle.crt",
            "/usr/local/share/certs/ca-root-nss.crt", // FreeBSD
            "/etc/ssl/ca-bundle.pem",                // openSUSE
            "/usr/local/etc/openssl/cert.pem",       // Homebrew OpenSSL
            "/opt/homebrew/etc/openssl@3/cert.pem",
        };
        for (const char *c : candidates)
            if (fs::exists(fs::path(c), ec))
                return c;
        BOOST_LOG_TRIVIAL(warning) << "Http: no system CA bundle found; certificate checks rely on libcurl's built-in default";
        return {};
    }();
    return bundle;
}
#endif

// Sets the certificate checks of one easy handle. With `verify` off this is what every request
// did until 2026-09-22 (VERIFYPEER=0, VERIFYHOST=0).
static void apply_tls_options(::CURL *curl, bool verify, bool ca_file_set, bool no_revoke = false)
{
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify ? 1L : 0L);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify ? 2L : 0L);
    if (!verify)
        return;
    // NO_REVOKE only means something to Schannel; with our OpenSSL build it is a no-op.
    long ssl_options = no_revoke ? long(CURLSSLOPT_NO_REVOKE) : 0L;
#ifdef _WIN32
    // Our libcurl is built on OpenSSL on every platform (deps/CURL/CURL.cmake), and OpenSSL has no
    // CA store of its own on Windows: take the trusted roots from the Windows certificate store
    // (CURLSSLOPT_NATIVE_CA, libcurl >= 7.71; ours is 7.75) - unless ca_file() named the CA to
    // trust, which is then the only one (a printer's own CA must not be widened to every root).
    if (!ca_file_set)
        ssl_options |= long(CURLSSLOPT_NATIVE_CA);
    ::curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, ssl_options);
#else
    ::curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, ssl_options);
    if (!ca_file_set) {
        const std::string &bundle = system_ca_bundle();
        if (!bundle.empty())
            ::curl_easy_setopt(curl, CURLOPT_CAINFO, bundle.c_str());
    }
#endif
}

Http::priv::priv(const std::string &url)
	: curl(::curl_easy_init())
	, form(nullptr)
	, form_end(nullptr)
	, mime(nullptr)
	, headerlist(nullptr)
	, error_buffer(CURL_ERROR_SIZE + 1, '\0')
	, url(url)
	, limit(0)
	, cancel(false)
{
    Http::tls_global_init();

	if (curl == nullptr) {
		throw Slic3r::RuntimeError(std::string("Could not construct Curl object"));
	}

	set_timeout_connect(DEFAULT_TIMEOUT_CONNECT);
    set_timeout_max(DEFAULT_TIMEOUT_MAX);
	::curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, log_trace);
	::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());   // curl makes a copy internally
	::curl_easy_setopt(curl, CURLOPT_USERAGENT, SLIC3R_APP_NAME "/" SLIC3R_VERSION);
	::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, &error_buffer.front());
#ifdef __WINDOWS__
	::curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_MAX_TLSv1_2);
#endif
	::curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
	// The certificate checks are set in http_perform(), from `url` and tls_policy (see
	// apply_tls_options below). Until 2026-09-22 they were switched off here for every request.

	// Bypass proxy for Snapmaker domains - this fixes issues when users have
	// Shadowsocks/Clash/V2Ray or other proxy software enabled
	// The domains in NOPROXY_DOMAINS will connect directly without going through proxy
	::curl_easy_setopt(curl, CURLOPT_NOPROXY, NOPROXY_DOMAINS);

	// https://everything.curl.dev/http/post/expect100.html
	// remove the Expect: header, it will add a second delay to each request,
	// if the file is uploaded in packets, it will cause the upload time to be longer
	headerlist = curl_slist_append(headerlist, "Expect:");
}

Http::priv::~priv()
{
	::curl_easy_cleanup(curl);
	::curl_formfree(form);
	::curl_mime_free(mime);
	::curl_slist_free_all(headerlist);
}

bool Http::priv::ca_file_supported(::CURL *curl)
{
	//BBS support set ca file by default
	bool res = true;

	if (curl == nullptr) { return res; }

#if LIBCURL_VERSION_MAJOR >= 7 && LIBCURL_VERSION_MINOR >= 48
	::curl_tlssessioninfo *tls;
	if (::curl_easy_getinfo(curl, CURLINFO_TLS_SSL_PTR, &tls) == CURLE_OK) {
		if (tls->backend == CURLSSLBACKEND_SCHANNEL || tls->backend == CURLSSLBACKEND_DARWINSSL) {
			// With Windows and OS X native SSL support, cert files cannot be set
			res = false;
		}
	}
#endif

	return res;
}

size_t Http::priv::writecb(void *data, size_t size, size_t nmemb, void *userp)
{
	auto self = static_cast<priv*>(userp);
	const char *cdata = static_cast<char*>(data);
	const size_t realsize = size * nmemb;

	const size_t limit = self->limit > 0 ? self->limit : DEFAULT_SIZE_LIMIT;
	if (self->bodyfn) {
		if (self->body_seen + realsize > limit)
			return 0;
		self->body_seen += realsize;
		long status = 0;
		::curl_easy_getinfo(self->curl, CURLINFO_RESPONSE_CODE, &status);
		if (self->bodyfn(unsigned(status), cdata, realsize))
			return realsize;
		self->body_refused = true;
		return 0;
	}
	if (self->buffer.size() + realsize > limit) {
		// This makes curl_easy_perform return CURLE_WRITE_ERROR
		return 0;
	}

	self->buffer.append(cdata, realsize);

	return realsize;
}

int Http::priv::xfercb(void *userp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	auto self = static_cast<priv*>(userp);
	bool cb_cancel = false;

	if (self->progressfn) {
		double speed;
        curl_easy_getinfo(self->curl, CURLINFO_SPEED_UPLOAD, &speed);
		if (speed > 0.01)
			speed = speed;
		Progress progress(dltotal, dlnow, ultotal, ulnow, self->buffer, speed);
		self->progressfn(progress, cb_cancel);
	}

	if (cb_cancel) { self->cancel = true; }

	return self->cancel;
}

int Http::priv::xfercb_legacy(void *userp, double dltotal, double dlnow, double ultotal, double ulnow)
{
	return xfercb(userp, dltotal, dlnow, ultotal, ulnow);
}

size_t Http::priv::form_file_read_cb(char *buffer, size_t size, size_t nitems, void *userp)
{
    // This callback is installed on every request, but CURLOPT_READDATA is only set for a
    // PUT. Anything else that makes curl ask for a body (a POST with no CURLOPT_POSTFIELDS
    // and no form) arrives here with curl's own default read-data, which is not a form_file:
    // reading it faulted. Refuse the read rather than reinterpret whatever curl passed.
    auto f = reinterpret_cast<form_file*>(userp);
    if (f == nullptr)
        return CURL_READFUNC_ABORT;

	try {
	    size_t max_read_size = size * nitems;
        if (f->content_length == 0) {
			// Unlimited
            f->ifs.read(buffer, max_read_size);
        } else {
            unsigned long long read_size = f->ifs.tellg() - f->init_offset;
            if (read_size >= f->content_length) {
                return 0;
            }

            max_read_size = std::min(max_read_size, size_t(f->content_length - read_size));
            f->ifs.read(buffer, max_read_size);
        }
	} catch (const std::exception &) {
		return CURL_READFUNC_ABORT;
	}

	return f->ifs.gcount();
}

size_t Http::priv::headers_cb(char *buffer, size_t size, size_t nitems, void *userp)
{
	auto self = static_cast<priv*>(userp);

	if (self->headerfn) {
        self->headers.append(buffer, nitems * size);
		self->headerfn(self->headers);
	}
	return nitems * size;
}

void Http::priv::set_timeout_connect(long timeout)
{
	::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout);
}

void Http::priv::set_timeout_max(long timeout)
{
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
}

void Http::priv::form_add_file(const char *name, const fs::path &path, const char* filename, boost::filesystem::ifstream::off_type offset, size_t length)
{
	// We can't use CURLFORM_FILECONTENT, because curl doesn't support Unicode filenames on Windows
	// and so we use CURLFORM_STREAM with boost ifstream to read the file.

	if (filename == nullptr) {
		filename = path.string().c_str();
	}

	form_files.emplace_back(path, offset, length);
	auto &f = form_files.back();
    size_t size = length;
    if (length == 0) {
        f.ifs.seekg(0, std::ios::end);
        size = f.ifs.tellg() - offset;
    }
    f.ifs.seekg(offset);

	if (filename != nullptr) {
		::curl_formadd(&form, &form_end,
			CURLFORM_COPYNAME, name,
			CURLFORM_FILENAME, filename,
			CURLFORM_CONTENTTYPE, "application/octet-stream",
			CURLFORM_STREAM, static_cast<void*>(&f),
			CURLFORM_CONTENTSLENGTH, static_cast<long>(size),
			CURLFORM_END
		);
	}
}

void Http::priv::mime_form_add_text(const char* name, const char* value)
{
	if (!mime) {
		mime = curl_mime_init(curl);
	}

	curl_mimepart *part;
	part = curl_mime_addpart(mime);
	curl_mime_name(part, name);
	curl_mime_type(part, "multipart/form-data");
	curl_mime_data(part, value, CURL_ZERO_TERMINATED);
}

void Http::priv::mime_form_add_file(const char* name, const char* path)
{
	if (!mime) {
		mime = curl_mime_init(curl);
	}

	curl_mimepart* part;
	part = curl_mime_addpart(mime);
	curl_mime_name(part, "file");
	curl_mime_type(part, "multipart/form-data");
	curl_mime_filedata(part, path);
	// BBS specify filename after filedata
	curl_mime_filename(part, name);
}

//FIXME may throw! Is the caller aware of it?
void Http::priv::set_post_body(const fs::path &path)
{
	std::ifstream file(path.string());
	std::string file_content { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	postfields = std::move(file_content);
	postfields_set = true;
}

void Http::priv::set_post_body(const std::string &body)
{
	postfields = body;
	postfields_set = true;
}

void Http::priv::set_put_body(const fs::path &path)
{
	boost::system::error_code ec;
	boost::uintmax_t filesize = file_size(path, ec);
	if (!ec) {
        putFile = std::make_unique<form_file>(path, 0, 0);
		::curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
		::curl_easy_setopt(curl, CURLOPT_READDATA, (void *) (putFile.get()));
		::curl_easy_setopt(curl, CURLOPT_INFILESIZE, filesize);
	}
}

void Http::priv::set_del_body(const std::string& body)
{
	postfields = body;
	postfields_set = true;
}

void Http::priv::set_range(const std::string& range)
{
	::curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
}

std::string Http::priv::curl_error(CURLcode curlcode)
{
	return (boost::format("curl:%1%:\n%2%\n[Error %3%]")
		% ::curl_easy_strerror(curlcode)
		% error_buffer.c_str()
		% curlcode
	).str();
}

std::string Http::priv::body_size_error()
{
	return (boost::format("HTTP body data size exceeded limit (%1% bytes)") % limit).str();
}

void Http::priv::http_perform()
{
	const bool tls_verify = Http::tls_verify_for(url, tls_policy, ca_file_set);
	apply_tls_options(curl, tls_verify, ca_file_set, no_revoke);

	::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, follow_location ? 1L : 0L);
	::curl_easy_setopt(curl, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
	::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writecb);
	::curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void*>(this));
	::curl_easy_setopt(curl, CURLOPT_READFUNCTION, form_file_read_cb);
	//BBS set header functions
	::curl_easy_setopt(curl, CURLOPT_HEADERDATA, static_cast<void *>(this));
	::curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headers_cb);

	::curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#if LIBCURL_VERSION_MAJOR >= 7 && LIBCURL_VERSION_MINOR >= 32
	::curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfercb);
	::curl_easy_setopt(curl, CURLOPT_XFERINFODATA, static_cast<void*>(this));
#ifndef _WIN32
	(void)xfercb_legacy;   // prevent unused function warning
#endif
#else
	::curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, xfercb);
	::curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, static_cast<void*>(this));
#endif

	::curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

	if (headerlist != nullptr) {
		::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist);
	}

	if (form != nullptr) {
		::curl_easy_setopt(curl, CURLOPT_HTTPPOST, form);
	}

	if (mime != nullptr) {
		::curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
	}

	// postfields_set, not !postfields.empty(): an explicitly set empty body is still a body.
	// Without this a POST of "" left curl with no body source but CURLOPT_POST, so it read
	// from form_file_read_cb with unset read-data and crashed (RemoteHub's /hub/phone,
	// /hub/newlink and /hub/quit are all empty-bodied POSTs).
	if (postfields_set && form == nullptr && mime == nullptr) {
		::curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postfields.c_str());
		::curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, postfields.size());
	}

	CURLcode res = ::curl_easy_perform(curl);

    putFile.reset();

	if (res != CURLE_OK) {
		if (res == CURLE_ABORTED_BY_CALLBACK) {
			if (cancel) {
				// The abort comes from the request being cancelled programatically
				Progress dummyprogress(0, 0, 0, 0, std::string());
				bool cancel = true;
				if (progressfn) { progressfn(dummyprogress, cancel); }
			} else {
				// The abort comes from the CURLOPT_READFUNCTION callback, which means reading file failed
				if (errorfn) { errorfn(std::move(buffer), "Error reading file for file upload", 0); }
			}
		}
		else if (res == CURLE_WRITE_ERROR) {
			if (errorfn) { errorfn(std::move(buffer), body_refused ? std::string("The transfer was stopped") : body_size_error(), 0); }
		} else {
			if (tls_verify && (res == CURLE_PEER_FAILED_VERIFICATION || res == CURLE_SSL_CACERT_BADFILE))
				BOOST_LOG_TRIVIAL(warning) << "Http: TLS certificate check failed for host " << Http::url_host(url)
				                           << ": " << error_buffer.c_str();
			if (errorfn) { errorfn(std::move(buffer), curl_error(res), 0); }
		};
	} else {
		long http_status = 0;
		::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

		//BBS check success http status code
		if (http_status >= 200 && http_status < 300) {
			if (completefn) { completefn(std::move(buffer), http_status); }
			if (ipresolvefn) {
				char* ct;
				res = curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ct);
				if ((CURLE_OK == res) && ct) {
					ipresolvefn(ct);
				}
			}
		}
		//BBS check error http status code; a 3xx only ends up here when follow_redirects(false)
		else if (http_status >= 300) {
			if (errorfn) { errorfn(std::move(buffer), std::string(), http_status); }
		}
	}
}

Http::Http(const std::string &url) : p(new priv(url)) {

    std::lock_guard<std::mutex> l(g_mutex);
	for (auto it = extra_headers.begin(); it != extra_headers.end(); it++)
		this->header(it->first, it->second);
}


// Public

Http::Http(Http &&other) : p(std::move(other.p)) {}

Http::~Http()
{
    assert(! p || ! p->putFile);
	if (p && p->io_thread.joinable()) {
		p->io_thread.detach();
	}
}


Http& Http::timeout_connect(long timeout)
{
	if (timeout < 1) { timeout = priv::DEFAULT_TIMEOUT_CONNECT; }
	if (p) { p->set_timeout_connect(timeout); }
	return *this;
}

Http& Http::timeout_max(long timeout)
{
    if (timeout < 1) { timeout = priv::DEFAULT_TIMEOUT_MAX; }
    if (p) { p->set_timeout_max(timeout); }
    return *this;
}

Http& Http::http_version(long version)
{
	if (p) { ::curl_easy_setopt(p->curl, CURLOPT_HTTP_VERSION, version); }
	return *this;
}

bool Http::has_http2()
{
	const curl_version_info_data *ver = ::curl_version_info(CURLVERSION_NOW);
	return ver != nullptr && (ver->features & CURL_VERSION_HTTP2) != 0;
}

Http& Http::size_limit(size_t sizeLimit)
{
	if (p) { p->limit = sizeLimit; }
	return *this;
}

Http& Http::set_range(const std::string& range)
{
	if (p) { p->set_range(range); }
	return *this;
}

Http& Http::header(std::string name, const std::string &value)
{
	if (!p) { return * this; }

	if (name.size() > 0) {
		name.append(": ").append(value);
	} else {
		name.push_back(':');
	}
	p->headerlist = curl_slist_append(p->headerlist, name.c_str());
	return *this;
}

Http& Http::remove_header(std::string name)
{
	if (p) {
		name.push_back(':');
		p->headerlist = curl_slist_append(p->headerlist, name.c_str());
	}

	return *this;
}

Http& Http::clear_headers()
{
	if (p) {
		::curl_slist_free_all(p->headerlist);
		p->headerlist = nullptr;
		// Same default the constructor starts from: no "Expect: 100-continue" round trip.
		p->headerlist = curl_slist_append(p->headerlist, "Expect:");
	}

	return *this;
}

// Authorization by HTTP digest, based on RFC2617.
Http& Http::auth_digest(const std::string &user, const std::string &password)
{
	curl_easy_setopt(p->curl, CURLOPT_USERNAME, user.c_str());
	curl_easy_setopt(p->curl, CURLOPT_PASSWORD, password.c_str());
	curl_easy_setopt(p->curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);

	return *this;
}

Http& Http::auth_basic(const std::string &user, const std::string &password)
{
    curl_easy_setopt(p->curl, CURLOPT_USERNAME, user.c_str());
    curl_easy_setopt(p->curl, CURLOPT_PASSWORD, password.c_str());
    curl_easy_setopt(p->curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);

    return *this;
}

Http& Http::ca_file(const std::string &name)
{
	if (p && priv::ca_file_supported(p->curl)) {
		::curl_easy_setopt(p->curl, CURLOPT_CAINFO, name.c_str());
		p->ca_file_set = true;
	}

	return *this;
}

Http& Http::tls_policy(TlsPolicy policy)
{
	if (p) { p->tls_policy = policy; }
	return *this;
}

std::string Http::url_host(const std::string &url)
{
    const size_t scheme_end = url.find("://");
    const size_t start      = scheme_end == std::string::npos ? 0 : scheme_end + 3;
    const size_t end        = url.find_first_of("/?#", start);
    std::string  authority  = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (const size_t at = authority.rfind('@'); at != std::string::npos)
        authority.erase(0, at + 1);

    std::string host;
    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        host = authority.substr(1, close == std::string::npos ? std::string::npos : close - 1);
        if (const size_t zone = host.find('%'); zone != std::string::npos)
            host.erase(zone); // fe80::1%25eth0
    } else {
        host = authority.substr(0, authority.find(':'));
    }
    boost::algorithm::to_lower(host);
    while (!host.empty() && host.back() == '.')
        host.pop_back();
    return host;
}

// Dotted-quad IPv4 only (what URLs carry); false for anything else.
static bool parse_ipv4(const std::string &s, unsigned char out[4])
{
    int    part = 0;
    size_t i    = 0;
    while (part < 4) {
        size_t   digits = 0;
        unsigned value  = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 3) {
            value = value * 10 + unsigned(s[i] - '0');
            ++i, ++digits;
        }
        if (digits == 0 || value > 255)
            return false;
        out[part++] = (unsigned char) value;
        if (part < 4) {
            if (i >= s.size() || s[i] != '.')
                return false;
            ++i;
        }
    }
    return i == s.size();
}

static bool ipv4_is_private(const unsigned char a[4])
{
    return a[0] == 127                                  // loopback
        || a[0] == 10                                   // RFC 1918
        || (a[0] == 172 && (a[1] & 0xf0) == 16)         // RFC 1918 172.16/12
        || (a[0] == 192 && a[1] == 168)                 // RFC 1918
        || (a[0] == 169 && a[1] == 254)                 // link-local
        || (a[0] == 100 && (a[1] & 0xc0) == 64)         // CGNAT 100.64/10: Tailscale addresses
        || a[0] == 0;                                   // "this network"
}

// Expands an IPv6 literal (no brackets, no zone) into 8 groups; false when it is not one.
static bool parse_ipv6(const std::string &s, uint16_t out[8])
{
    auto parse_groups = [](const std::string &part, std::vector<uint16_t> &groups, bool allow_v4_tail) -> bool {
        if (part.empty())
            return true;
        size_t pos = 0;
        while (true) {
            const size_t colon = part.find(':', pos);
            const std::string g = part.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
            if (colon == std::string::npos && allow_v4_tail && g.find('.') != std::string::npos) {
                unsigned char v4[4];
                if (!parse_ipv4(g, v4))
                    return false;
                groups.push_back(uint16_t((v4[0] << 8) | v4[1]));
                groups.push_back(uint16_t((v4[2] << 8) | v4[3]));
                return true;
            }
            if (g.empty() || g.size() > 4 || g.find_first_not_of("0123456789abcdef") != std::string::npos)
                return false;
            groups.push_back(uint16_t(std::stoul(g, nullptr, 16)));
            if (colon == std::string::npos)
                return true;
            pos = colon + 1;
        }
    };
    std::vector<uint16_t> head, tail;
    const size_t gap = s.find("::");
    if (gap == std::string::npos) {
        if (!parse_groups(s, head, true) || head.size() != 8)
            return false;
    } else {
        if (s.find("::", gap + 1) != std::string::npos)
            return false;
        if (!parse_groups(s.substr(0, gap), head, false) || !parse_groups(s.substr(gap + 2), tail, true) ||
            head.size() + tail.size() > 7)
            return false;
    }
    std::fill(out, out + 8, uint16_t(0));
    std::copy(head.begin(), head.end(), out);
    std::copy(tail.begin(), tail.end(), out + 8 - tail.size());
    return true;
}

bool Http::tls_host_is_private(const std::string &host_in)
{
    std::string host = host_in;
    boost::algorithm::to_lower(host);
    while (!host.empty() && host.back() == '.')
        host.pop_back();
    if (host.empty())
        return true;
    if (host == "localhost" || boost::algorithm::ends_with(host, ".localhost"))
        return true;

    unsigned char v4[4];
    if (parse_ipv4(host, v4))
        return ipv4_is_private(v4);

    if (host.find(':') != std::string::npos) {
        uint16_t g[8];
        if (!parse_ipv6(host, g))
            return true; // not a host we understand; do not start failing requests on it
        const bool upper_zero = g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0 && g[4] == 0;
        if (upper_zero && g[5] == 0 && g[6] == 0 && (g[7] == 0 || g[7] == 1))
            return true;                                   // :: and ::1
        if (upper_zero && g[5] == 0xffff) {                // ::ffff:a.b.c.d
            const unsigned char m[4] = { (unsigned char) (g[6] >> 8), (unsigned char) g[6], (unsigned char) (g[7] >> 8), (unsigned char) g[7] };
            return ipv4_is_private(m);
        }
        return (g[0] & 0xfe00) == 0xfc00                   // unique local fc00::/7
            || (g[0] & 0xffc0) == 0xfe80;                  // link-local fe80::/10
    }

    // A single-label name ("octopi", "u1") only resolves on the local network.
    if (host.find('.') == std::string::npos)
        return true;

    // Names that only exist on a LAN. ts.net is Tailscale MagicDNS: those hosts are reached through
    // the WireGuard tunnel (already authenticated and encrypted end to end), and a device there
    // serves its own self-signed certificate unless Tailscale Serve fronts it.
    static const char *const private_suffixes[] = {
        ".local", ".lan", ".home", ".internal", ".intranet", ".localdomain", ".home.arpa", ".ts.net",
    };
    for (const char *suffix : private_suffixes)
        if (boost::algorithm::ends_with(host, suffix))
            return true;
    return false;
}

bool Http::tls_verify_for(const std::string &url, TlsPolicy policy, bool has_ca_file)
{
    std::string scheme = url.substr(0, url.find("://"));
    boost::algorithm::to_lower(scheme);
    if (scheme != "https" && scheme != "wss")
        return false; // no TLS at all (curl treats a URL without a scheme as http)
    if (has_ca_file)
        return true;  // an explicit CA file means "check this host against it"
    if (policy == TlsPolicy::PrintHost)
        return false;
    if (policy == TlsPolicy::Verify)
        return true;
    return !tls_host_is_private(url_host(url));
}

void Http::apply_tls_policy(void *curl_handle, const std::string &url, TlsPolicy policy)
{
    if (curl_handle != nullptr)
        apply_tls_options(static_cast<::CURL *>(curl_handle), tls_verify_for(url, policy), false);
}

Http& Http::form_clear() {
	if (p) {
        if (p->form) {
            ::curl_formfree(p->form);
            p->form     = nullptr;
            p->form_end = nullptr;
        }
		for (auto &f : p->form_files) {
			f.ifs.close();
		}
		p->form_files.clear();

	}
	return *this;
}

Http& Http::form_add(const std::string &name, const std::string &contents)
{
	if (p) {
		::curl_formadd(&p->form, &p->form_end,
			CURLFORM_COPYNAME, name.c_str(),
			CURLFORM_COPYCONTENTS, contents.c_str(),
			CURLFORM_END
		);
	}

	return *this;
}

Http& Http::form_add_file(const std::string &name, const fs::path &path, boost::filesystem::ifstream::off_type offset, size_t length)
{
	if (p) { p->form_add_file(name.c_str(), path.c_str(), nullptr, offset, length); }
	return *this;
}


Http& Http::mime_form_add_text(std::string &name, std::string &value)
{
	if (p) { p->mime_form_add_text(name.c_str(), value.c_str()); }
	return *this;
}

Http& Http::mime_form_add_file(std::string &name, const char* path)
{
	if (p) { p->mime_form_add_file(name.c_str(), path); }
	return *this;
}


Http& Http::form_add_file(const std::wstring& name, const fs::path& path, boost::filesystem::ifstream::off_type offset, size_t length)
{
	if (p) { p->form_add_file((char*)name.c_str(), path.c_str(), nullptr, offset, length); }
	return *this;
}

Http& Http::form_add_file(const std::string &name, const fs::path &path, const std::string &filename, boost::filesystem::ifstream::off_type offset, size_t length)
{
	if (p) { p->form_add_file(name.c_str(), path.c_str(), filename.c_str(), offset, length); }
	return *this;
}

// Recorded here and applied in http_perform() together with the other CURLOPT_SSL_OPTIONS bits
// (see apply_tls_options). It used to be compiled out entirely.
Http& Http::ssl_revoke_best_effort(bool set)
{
	if (p) { p->no_revoke = set; }
	return *this;
}

Http& Http::set_post_body(const fs::path &path)
{
	if (p) { p->set_post_body(path);}
	return *this;
}

Http& Http::set_post_body(const std::string &body)
{
	if (p) { p->set_post_body(body); }
	return *this;
}

Http& Http::set_put_body(const fs::path &path)
{
	if (p) { p->set_put_body(path);}
	return *this;
}

Http& Http::set_del_body(const std::string &body)
{
	if (p) { p->set_del_body(body); }
	return *this;
}

Http& Http::on_complete(CompleteFn fn)
{
	if (p) { p->completefn = std::move(fn); }
	return *this;
}

Http& Http::on_error(ErrorFn fn)
{
	if (p) { p->errorfn = std::move(fn); }
	return *this;
}

Http& Http::on_progress(ProgressFn fn)
{
	if (p) { p->progressfn = std::move(fn); }
	return *this;
}

Http& Http::on_ip_resolve(IPResolveFn fn)
{
	if (p) { p->ipresolvefn = std::move(fn); }
	return *this;
}

Http &Http::on_header_callback(HeaderCallbackFn fn)
{
	if (p) { p->headerfn = std::move(fn); }
	return *this;
}

Http &Http::on_body(BodyFn fn)
{
	if (p) { p->bodyfn = std::move(fn); }
	return *this;
}

Http &Http::follow_redirects(bool follow)
{
	if (p) { p->follow_location = follow; }
	return *this;
}

Http::Ptr Http::perform()
{
	auto self = std::make_shared<Http>(std::move(*this));

	if (self->p) {
		auto io_thread = std::thread([self](){
				self->p->http_perform();
			});
		self->p->io_thread = std::move(io_thread);
	}

	return self;
}

void Http::perform_sync()
{
	if (p) { p->http_perform(); }
}

void Http::cancel()
{
	if (p) { p->cancel = true; }
}

Http Http::get(std::string url)
{
    return Http{std::move(url)};
}

Http Http::post(std::string url)
{
	Http http{std::move(url)};
	curl_easy_setopt(http.p->curl, CURLOPT_POST, 1L);
	return http;
}

Http Http::put(std::string url)
{
	Http http{std::move(url)};
	curl_easy_setopt(http.p->curl, CURLOPT_UPLOAD, 1L);
	return http;
}

Http Http::put2(std::string url)
{
	Http http{ std::move(url) };
	curl_easy_setopt(http.p->curl, CURLOPT_CUSTOMREQUEST, "PUT");
	return http;
}

Http Http::patch(std::string url)
{
	Http http{ std::move(url) };
	curl_easy_setopt(http.p->curl, CURLOPT_CUSTOMREQUEST, "PATCH");
	return http;
}

Http Http::del(std::string url)
{
	Http http{ std::move(url) };
	curl_easy_setopt(http.p->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
	return http;
}

void Http::set_extra_headers(std::map<std::string, std::string> headers)
{
    std::lock_guard<std::mutex> l(g_mutex);
	extra_headers.swap(headers);
}

std::map<std::string, std::string> Http::get_extra_headers()
{
    std::lock_guard<std::mutex> l(g_mutex);
    return extra_headers;
}

bool Http::ca_file_supported()
{
	::CURL *curl = ::curl_easy_init();
	bool res = priv::ca_file_supported(curl);
	if (curl != nullptr) { ::curl_easy_cleanup(curl); }
    return res;
}

std::string Http::tls_global_init()
{
    if (!CurlGlobalInit::instance)
        CurlGlobalInit::instance = std::make_unique<CurlGlobalInit>();

    return CurlGlobalInit::instance->message;
}

std::string Http::tls_system_cert_store()
{
    std::string ret;

#ifdef OPENSSL_CERT_OVERRIDE
    // getenv() returns NULL when SSL_CERT_FILE is unset, and assigning NULL to a
    // std::string is undefined (a strlen(NULL) crash at startup). CurlGlobalInit only
    // sets the variable when OpenSSL's default cert file is missing; with the runtime's
    // OpenSSL 3 that file exists, so the variable stays unset. Report an empty store then.
    if (const char *cert_file = ::getenv(X509_get_default_cert_file_env()))
        ret = cert_file;
#endif

    return ret;
}

std::string Http::url_encode(const std::string &str)
{
	::CURL *curl = ::curl_easy_init();
	if (curl == nullptr) {
		return str;
	}
	char *ce = ::curl_easy_escape(curl, str.c_str(), str.length());
	std::string encoded = std::string(ce);

	::curl_free(ce);
	::curl_easy_cleanup(curl);

	return encoded;
}

std::string Http::url_decode(const std::string &str)
{
    ::CURL *curl = ::curl_easy_init();
    if (curl == nullptr) { return str; }
    int outlen = 0;
    char *ce = ::curl_easy_unescape(curl, str.c_str(), str.length(), &outlen);
    std::string dencoded = std::string(ce, outlen);

    ::curl_free(ce);
    ::curl_easy_cleanup(curl);

    return dencoded;
}

std::string Http::get_filename_from_url(const std::string &url)
{
    int end_pos = url.find_first_of('?');
	if (end_pos <= 0) return "";
	std::string path_url = url.substr(0, end_pos);
	int start_pos = path_url.find_last_of("/");
	if (start_pos < 0) return "";
	return path_url.substr(start_pos + 1, path_url.length() - start_pos - 1);
}

std::ostream& operator<<(std::ostream &os, const Http::Progress &progress)
{
	os << "Http::Progress("
		<< "dltotal = " << progress.dltotal
		<< ", dlnow = " << progress.dlnow
		<< ", ultotal = " << progress.ultotal
		<< ", ulnow = " << progress.ulnow
		<< ")";
	return os;
}

std::string Http::encode_url_path(const std::string& url)
{
    size_t protocol_end = url.find("://");
    if (protocol_end == std::string::npos) {
        return url;
    }

    // 找到域名结束的位置（第一个斜杠）
    size_t domain_end = url.find("/", protocol_end + 3);
    if (domain_end == std::string::npos) {
        return url;
    }

    // 分割 URL
    std::string protocol_domain = url.substr(0, domain_end);
    std::string path            = url.substr(domain_end);

    // 只对路径部分进行编码
    CURL*       curl = curl_easy_init();
    std::string encoded_path;
    if (curl) {
        char* encoded = curl_easy_escape(curl, path.c_str(), path.length());
        if (encoded) {
            encoded_path = encoded;
            curl_free(encoded);
        }
        curl_easy_cleanup(curl);
    }

    // 替换编码后的斜杠为实际的斜杠
    boost::replace_all(encoded_path, "%2F", "/");

    return protocol_domain + encoded_path;
}

}
