#include "FilamentSort.hpp"

#include "nlohmann/json.hpp"

#include <limits>
#include <utility>

namespace Slic3r
{
namespace GUI
{

namespace
{

constexpr const char *g_snapmaker_vendor = "Snapmaker";
constexpr const char *g_generic_vendor   = "Generic";
constexpr int         g_allowlist_schema_version = 1;

/** @brief Converts a vendor identifier into the label used by system sorting. */
wxString vendor_label(const std::string &vendor)
{
    return vendor.empty() ? wxString::FromUTF8("System") : wxString::FromUTF8(vendor.c_str());
}

/** @brief Returns the fixed priority bucket for a system vendor. */
int vendor_rank(const std::string &vendor)
{
    const wxString label = vendor_label(vendor);
    if (label.CmpNoCase(wxString::FromUTF8(g_snapmaker_vendor)) == 0)
        return 0;
    if (label.CmpNoCase(wxString::FromUTF8(g_generic_vendor)) == 0)
        return 1;
    return 2;
}

/** @brief Compares display names and preserves the original order for ties. */
bool default_name_less(const FilamentSortItem &left, const FilamentSortItem &right)
{
    const int name_compare = left.display_name.CmpNoCase(right.display_name);
    if (name_compare != 0)
        return name_compare < 0;
    return left.original_index < right.original_index;
}

} // namespace

FilamentOrder FilamentOrder::from_stream(std::istream &stream)
{
    nlohmann::json root = nlohmann::json::parse(stream, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return FilamentOrder{};

    const auto schema_version = root.find("schema_version");
    const auto sections_node  = root.find("sections");
    if (schema_version == root.end() || !schema_version->is_number_integer() ||
        *schema_version != g_allowlist_schema_version || sections_node == root.end() || !sections_node->is_object())
        return FilamentOrder{};

    const auto order_node = sections_node->find("filament_order");
    if (order_node == sections_node->end() || !order_node->is_object())
        return FilamentOrder{};

    Orders orders;
    for (const auto &vendor_order : order_node->items())
    {
        if (vendor_order.key().empty() || !vendor_order.value().is_array() || vendor_order.value().empty())
            return FilamentOrder{};

        Order values;
        for (const auto &value : vendor_order.value())
        {
            if (!value.is_string() || value.get_ref<const std::string &>().empty())
                return FilamentOrder{};
            values.emplace_back(value.get_ref<const std::string &>());
        }
        orders.emplace_back(vendor_order.key(), std::move(values));
    }

    if (orders.empty())
        return FilamentOrder{};
    return FilamentOrder(std::move(orders));
}

FilamentOrder::FilamentOrder(Orders orders) : m_orders(std::move(orders))
{
}

size_t FilamentOrder::rank(const std::string &vendor, const std::string &filament_product) const
{
    for (const auto &vendor_order : m_orders)
    {
        if (wxString::FromUTF8(vendor_order.first.c_str()).CmpNoCase(wxString::FromUTF8(vendor.c_str())) != 0)
            continue;

        for (size_t index = 0; index < vendor_order.second.size(); ++index)
        {
            // Product names are authored both in the preset files and in filament_allow_list.json, so they match
            // case-insensitively like the vendor key above: a casing drift must not silently drop an entry.
            if (wxString::FromUTF8(vendor_order.second[index].c_str())
                    .CmpNoCase(wxString::FromUTF8(filament_product.c_str())) == 0)
                return index;
        }
        break;
    }
    return std::numeric_limits<size_t>::max();
}

bool FilamentOrder::empty() const
{
    return m_orders.empty();
}

bool FilamentSorter::less(const FilamentSortItem &left, const FilamentSortItem &right) const
{
    return less_by_name(left, right);
}

bool FilamentSorter::less_by_name(const FilamentSortItem &left, const FilamentSortItem &right) const
{
    return default_name_less(left, right);
}

bool FilamentVendorSorter::less(const std::string &left, const std::string &right) const
{
    return vendor_label(left).CmpNoCase(vendor_label(right)) < 0;
}

bool SystemFilamentVendorSorter::less(const std::string &left, const std::string &right) const
{
    const int left_rank  = vendor_rank(left);
    const int right_rank = vendor_rank(right);
    if (left_rank != right_rank)
        return left_rank < right_rank;
    return FilamentVendorSorter::less(left, right);
}

SystemFilamentSorter::SystemFilamentSorter(FilamentOrder filament_order)
    : m_filament_order(std::move(filament_order))
{
}

bool SystemFilamentSorter::less(const FilamentSortItem &left, const FilamentSortItem &right) const
{
    if (is_snapmaker_vendor(left.vendor) && is_snapmaker_vendor(right.vendor))
    {
        const size_t left_rank  = m_filament_order.rank(left.vendor, left.filament_product);
        const size_t right_rank = m_filament_order.rank(right.vendor, right.filament_product);
        if (left_rank != right_rank)
            return left_rank < right_rank;
    }

    return less_by_name(left, right);
}

bool is_snapmaker_vendor(const std::string &vendor)
{
    return wxString::FromUTF8(vendor.c_str()).CmpNoCase(wxString::FromUTF8(g_snapmaker_vendor)) == 0;
}

std::string canonical_vendor(const std::string &vendor)
{
    const wxString label = wxString::FromUTF8(vendor.c_str());
    if (label.CmpNoCase(wxString::FromUTF8(g_snapmaker_vendor)) == 0)
        return g_snapmaker_vendor;
    if (label.CmpNoCase(wxString::FromUTF8(g_generic_vendor)) == 0)
        return g_generic_vendor;
    return vendor;
}

} // namespace GUI
} // namespace Slic3r
