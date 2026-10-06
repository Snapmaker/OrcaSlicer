#include "AccountProviders.hpp"

#include <algorithm>

namespace Slic3r {
namespace Accounts {

void Registry::add(Provider provider)
{
    if (provider.id.empty() || !provider.is_signed_in)
        return;
    auto it = std::find_if(m_providers.begin(), m_providers.end(), [&](const Provider& p) { return p.id == provider.id; });
    if (it != m_providers.end())
        *it = std::move(provider);
    else
        m_providers.push_back(std::move(provider));
}

const Provider* Registry::find(const std::string& id) const
{
    for (const Provider& p : m_providers)
        if (p.id == id)
            return &p;
    return nullptr;
}

const Provider* Registry::find_for_vendor(const std::string& vendor_id) const
{
    if (vendor_id.empty())
        return nullptr;
    for (const Provider& p : m_providers)
        if (std::find(p.vendor_ids.begin(), p.vendor_ids.end(), vendor_id) != p.vendor_ids.end())
            return &p;
    return nullptr;
}

Registry& registry()
{
    static Registry r;
    return r;
}

bool should_warn(const Inputs& in)
{
    if (in.signed_in)
        return false;
    return in.previously_signed_in || in.cloud_bound;
}

Status evaluate(const Registry& registry, const std::string& vendor_id, const std::function<bool(const std::string&)>& was_signed_in)
{
    Status st;
    st.provider = registry.find_for_vendor(vendor_id);
    if (st.provider == nullptr)
        return st;
    Inputs in;
    in.signed_in            = st.provider->is_signed_in();
    // The other two only matter when signed out; skip asking (the cloud-bound one looks at the device list).
    if (!in.signed_in) {
        in.previously_signed_in = was_signed_in ? was_signed_in(st.provider->id) : false;
        in.cloud_bound          = st.provider->selected_printer_cloud_bound ? st.provider->selected_printer_cloud_bound() : false;
    }
    st.signed_in            = in.signed_in;
    st.warn                 = should_warn(in);
    return st;
}

std::string signed_in_flag_key(const std::string& provider_id)
{
    return "account_signed_in_" + provider_id;
}

} // namespace Accounts
} // namespace Slic3r
