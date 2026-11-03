#include "Exception.hpp"
#include "PrintBase.hpp"

#include <tbb/global_control.h>

#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>

#include "I18N.hpp"

//! macro used to mark string used at localization,
//! return same string
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r
{

void PrintTryCancel::operator()()
{
    m_print->throw_if_canceled();
}

void PrintBase::check_memory_guard()
{
    using namespace std::chrono;
    const int64_t now  = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    int64_t       last = m_last_mem_check_ns.load(std::memory_order_relaxed);
    // Throttle to one sample per interval; the compare-exchange makes a
    // single thread take each sample.
    if (now - last < duration_cast<nanoseconds>(MEM_GUARD_SAMPLE_INTERVAL).count() ||
        ! m_last_mem_check_ns.compare_exchange_strong(last, now, std::memory_order_relaxed))
        return;
    // The guard already acted during this slice.
    if (m_memory_guard_acknowledged.load(std::memory_order_relaxed))
        return;

    const AvailableMemory mem          = get_available_memory();
    const bool            low_physical = mem.physical != 0 && mem.physical < MEM_GUARD_THRESHOLD;
    const bool            low_commit   = mem.commit != 0 && mem.commit < MEM_GUARD_THRESHOLD;
    if (! low_physical && ! low_commit) {
        m_low_mem_since_ns.store(0, std::memory_order_relaxed);
        return;
    }

    int64_t since = m_low_mem_since_ns.load(std::memory_order_relaxed);
    if (since == 0) {
        since = now;
        m_low_mem_since_ns.store(now, std::memory_order_relaxed);
    }
    const bool critical = (mem.physical != 0 && mem.physical < MEM_GUARD_CRITICAL) ||
                          (mem.commit != 0 && mem.commit < MEM_GUARD_CRITICAL);
    const milliseconds sustain = critical ? milliseconds(0) : low_commit ? MEM_GUARD_SUSTAIN_COMMIT : MEM_GUARD_SUSTAIN_PHYSICAL;
    if (now - since < duration_cast<nanoseconds>(sustain).count())
        return;

    // Claim the action for this slice; another thread may have taken the
    // same low sample.
    bool expected = false;
    if (! m_memory_guard_acknowledged.compare_exchange_strong(expected, true, std::memory_order_relaxed))
        return;

    BOOST_LOG_TRIVIAL(warning) << "Memory guard: low memory during slicing, continuing on a single thread. physical available "
        << (mem.physical >> 20) << " MB, commit available " << (mem.commit >> 20) << " MB, low for "
        << (now - since) / 1000000 << " ms" << (critical ? " (critical)" : "") << log_memory_info(true);

    {
        // TBB lets running workers finish their current task, then keeps
        // them out, so the rest of the slice runs on one thread.
        std::lock_guard<std::mutex> lock(m_memory_guard_throttle_mutex);
        m_memory_guard_throttle = std::make_shared<tbb::global_control>(tbb::global_control::max_allowed_parallelism, 1);
    }
    m_memory_guard_callback();
}

bool PrintBase::memory_guard_throttled() const
{
    std::lock_guard<std::mutex> lock(m_memory_guard_throttle_mutex);
    return m_memory_guard_throttle != nullptr;
}

void PrintBase::release_memory_guard_throttle()
{
    std::lock_guard<std::mutex> lock(m_memory_guard_throttle_mutex);
    if (m_memory_guard_throttle) {
        m_memory_guard_throttle.reset();
        BOOST_LOG_TRIVIAL(warning) << "Memory guard: back to full parallelism";
    }
}

size_t PrintStateBase::g_last_timestamp = 0;

// Update "scale", "input_filename", "input_filename_base" placeholders from the current m_objects.
void PrintBase::update_object_placeholders(DynamicConfig &config, const std::string &default_ext) const
{
    // get the first input file name
    std::string input_file;
    std::vector<std::string> v_scale;
    int num_objects = 0;
    int num_instances = 0;
	for (const ModelObject *model_object : m_model.objects) {
		ModelInstance *printable = nullptr;
		for (ModelInstance *model_instance : model_object->instances)
			if (model_instance->is_printable()) {
				printable = model_instance;
				++ num_instances;
			}
		if (printable) {
            ++ num_objects;
	        // CHECK_ME -> Is the following correct ?
			v_scale.push_back("x:" + boost::lexical_cast<std::string>(printable->get_scaling_factor(X) * 100) +
				"% y:" + boost::lexical_cast<std::string>(printable->get_scaling_factor(Y) * 100) +
				"% z:" + boost::lexical_cast<std::string>(printable->get_scaling_factor(Z) * 100) + "%");
	        if (input_file.empty())
	            input_file = model_object->name.empty() ? model_object->input_file : model_object->name;
	    }
    }

    config.set_key_value("num_objects", new ConfigOptionInt(num_objects));
    config.set_key_value("num_instances", new ConfigOptionInt(num_instances));

    config.set_key_value("scale", new ConfigOptionStrings(v_scale));
    if (! input_file.empty()) {
        // get basename with and without suffix
        const std::string input_filename = boost::filesystem::path(input_file).filename().string();
        const std::string input_filename_base = input_filename.substr(0, input_filename.find_last_of("."));
        config.set_key_value("input_filename", new ConfigOptionString(input_filename_base + default_ext));
        config.set_key_value("input_filename_base", new ConfigOptionString(input_filename_base));
    }
}

// Generate an output file name based on the format template, default extension, and template parameters
// (timestamps, object placeholders derived from the model, current placeholder prameters, print statistics - config_override)
std::string PrintBase::output_filename(const std::string &format, const std::string &default_ext, const std::string &filename_base, const DynamicConfig *config_override) const
{
    DynamicConfig cfg;
    if (config_override != nullptr)
    	cfg = *config_override;
    cfg.set_key_value("version", new ConfigOptionString(std::string(Snapmaker_VERSION)));
    PlaceholderParser::update_timestamp(cfg);
    PlaceholderParser::update_user_name(cfg);
    this->update_object_placeholders(cfg, default_ext);
    if (! filename_base.empty()) {
		cfg.set_key_value("input_filename", new ConfigOptionString(filename_base + default_ext));
		cfg.set_key_value("input_filename_base", new ConfigOptionString(filename_base));
    }
    try {
		boost::filesystem::path filename = format.empty() ?
			cfg.opt_string("input_filename_base") + default_ext :
			this->placeholder_parser().process(format, 0, &cfg);
        if (filename.extension().empty())
            filename.replace_extension(default_ext);
        return filename.string();
    } catch (std::runtime_error &err) {
        throw Slic3r::PlaceholderParserError(L("Failed processing of the filename_format template.") + "\n" + err.what());
    }
}

std::string PrintBase::output_filepath(const std::string &path, const std::string &filename_base) const
{
    // if we were supplied no path, generate an automatic one based on our first object's input file
    if (path.empty())
        // get the first input file name
        return (boost::filesystem::path(m_model.propose_export_file_name_and_path()).parent_path() / this->output_filename(filename_base)).make_preferred().string();

    // if we were supplied a directory, use it and append our automatically generated filename
    boost::filesystem::path p(path);
    if (boost::filesystem::is_directory(p))
        return (p / this->output_filename(filename_base)).make_preferred().string();

    // if we were supplied a file which is not a directory, use it
    return path;
}

//BBS: move set_status from hpp to cpp
void  PrintBase::set_status(int percent, const std::string &message, unsigned int flags, int warning_step) const
{
	if (m_status_callback)
        m_status_callback(SlicingStatus(percent, message, flags, warning_step));
    else
        BOOST_LOG_TRIVIAL(debug) <<boost::format("Percent %1%: %2%\n")%percent %message.c_str();
}

void PrintBase::status_update_warnings(int step, PrintStateBase::WarningLevel  warning_level,
    const std::string &message, const PrintObjectBase* print_object, PrintStateBase::SlicingNotificationType message_id)
{
    if (this->m_status_callback) {
        auto status = print_object ? SlicingStatus(*print_object, step, message, message_id, warning_level) : SlicingStatus(*this, step, message, message_id, warning_level);
        m_status_callback(status);
    }
    else if (! message.empty())
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Print warning: %1%\n")% message.c_str();
}

//BBS: add PrintObject id into slicing status
void PrintBase::status_update_warnings(int step, PrintStateBase::WarningLevel warning_level,
    const std::string& message, PrintObjectBase &object, PrintStateBase::SlicingNotificationType message_id)
{
    //BBS: add object it into slicing status
    if (this->m_status_callback) {
        m_status_callback(SlicingStatus(object, step, message, message_id, warning_level));
    }
    else if (!message.empty())
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", PrintObject warning: %1%\n")% message.c_str();
}


std::mutex& PrintObjectBase::state_mutex(PrintBase *print)
{
	return print->state_mutex();
}

std::function<void()> PrintObjectBase::cancel_callback(PrintBase *print)
{
	return print->cancel_callback();
}

void PrintObjectBase::status_update_warnings(PrintBase *print, int step, PrintStateBase::WarningLevel warning_level,
    const std::string &message, PrintStateBase::SlicingNotificationType message_id)
{
    print->status_update_warnings(step, warning_level, message, this, message_id);
}

} // namespace Slic3r
