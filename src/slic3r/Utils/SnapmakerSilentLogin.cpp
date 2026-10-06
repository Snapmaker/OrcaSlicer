#include "SnapmakerSilentLogin.hpp"

namespace Slic3r {
namespace SMSilentLogin {

std::string skip_reason(const StartupInputs& in)
{
    // Order matters only for which reason the log names when several apply; the cheapest and
    // most permanent reasons first.
    if (!in.pref_enabled)
        return "turned off in Preferences";
    if (!in.signed_in_before)
        return "not signed in to Snapmaker on this computer";
    if (!in.is_editor)
        return "G-code viewer";
    if (in.hidden_instance)
        return "hidden instance";
    if (!in.main_window_ready)
        return "main window not ready";
    if (in.already_signed_in)
        return "already signed in";
    if (in.login_dialog_open)
        return "sign-in dialog already open";
    if (in.network_down)
        return "no network";
    return std::string();
}

Tick on_tick(int elapsed_ms, bool page_loaded, int since_load_ms)
{
    if (page_loaded && since_load_ms >= k_settle_ms)
        return Tick::NoSession;
    if (elapsed_ms >= k_overall_timeout_ms)
        // A page that loaded but kept re-navigating is still a page without a session; one that
        // never loaded at all is a network or service problem.
        return page_loaded ? Tick::NoSession : Tick::TimedOut;
    return Tick::Wait;
}

std::string token_from_url(const std::string& url)
{
    static const std::string key = "token=";
    size_t start = url.find(key);
    if (start == std::string::npos)
        return std::string();
    start += key.size();
    const size_t end = url.find('?', start);
    return end == std::string::npos ? url.substr(start) : url.substr(start, end - start);
}

const char* outcome_name(Outcome o)
{
    switch (o) {
    case Outcome::SignedIn:  return "signed in";
    case Outcome::NoSession: return "no session";
    case Outcome::TimedOut:  return "timed out";
    case Outcome::Failed:    return "failed";
    case Outcome::Cancelled: return "cancelled";
    case Outcome::Skipped:   return "skipped";
    }
    return "unknown";
}

SessionMarker session_marker_after(Outcome o)
{
    switch (o) {
    case Outcome::SignedIn:  return SessionMarker::Set;
    case Outcome::NoSession: return SessionMarker::Clear;
    default:                 return SessionMarker::Keep;
    }
}

std::string log_line(Outcome o, const std::string& detail)
{
    std::string line = std::string("Snapmaker silent login: ") + outcome_name(o);
    if (!detail.empty())
        line += " (" + detail + ")";
    return line;
}

} // namespace SMSilentLogin
} // namespace Slic3r
