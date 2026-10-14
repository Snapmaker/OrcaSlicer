#include "FilamentSort.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
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
constexpr const char *g_undefined_vendor = "(Undefined)";
constexpr int         g_allowlist_schema_version = 1;

/** @brief Removes leading and trailing whitespace. */
std::string trimmed(std::string value)
{
    const auto is_space = [](unsigned char character) { return std::isspace(character) != 0; };
    const auto first    = std::find_if_not(value.begin(), value.end(), is_space);
    const auto last     = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    if (first >= last)
        return std::string();

    return std::string(first, last);
}

/** @brief Compares two strings case-insensitively over ASCII letters; every other byte must match exactly. */
bool ascii_iequal(const std::string &left, const std::string &right)
{
    if (left.size() != right.size())
        return false;

    for (size_t index = 0; index < left.size(); ++index)
    {
        const unsigned char lhs = static_cast<unsigned char>(left[index]);
        const unsigned char rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs))
            return false;
    }
    return true;
}

/** @brief Converts a vendor identifier into the label used by system sorting. */
wxString vendor_label(const std::string &vendor)
{
    return vendor.empty() ? wxString::FromUTF8("System") : wxString::FromUTF8(vendor.c_str());
}

/** @brief Returns the fixed priority bucket for a system vendor. */
int vendor_rank(const std::string &vendor)
{
    // Case-sensitive, like the vendor ordering below. Known vendors reach this point already spelled
    // canonically (PlaterFilamentComboBox calls canonical_vendor() before sorting), so a profile that
    // writes "snapmaker" is still ranked as Snapmaker; only unknown vendors keep their own spelling.
    const wxString label = vendor_label(vendor);
    if (label.Cmp(wxString::FromUTF8(g_snapmaker_vendor)) == 0)
        return 0;
    if (label.Cmp(wxString::FromUTF8(g_generic_vendor)) == 0)
        return 1;
    return 2;
}

/** @brief Compares display names by code point and preserves the original order for ties. */
bool default_name_less(const FilamentSortItem &left, const FilamentSortItem &right)
{
    // Case-sensitive, like the upstream Bambu collation: names that differ only in case are ordered
    // by code point instead of being treated as equal.
    const int name_compare = left.display_name.Cmp(right.display_name);
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

FilamentOrder FilamentOrder::from_file(const std::filesystem::path &path, std::string *error)
{
    std::ifstream stream(path);
    if (!stream)
    {
        if (error != nullptr)
            *error = "cannot be opened";
        return FilamentOrder{};
    }

    const FilamentOrder order = from_stream(stream);
    if (order.empty() && error != nullptr)
        *error = "has an invalid or empty configuration";
    return order;
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
    // Case-sensitive, like the upstream Bambu vendor table: the vendor axis orders by code point.
    return vendor_label(left).Cmp(vendor_label(right)) < 0;
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
    // Narrow contract: the caller orders rows by vendor first (PlaterFilamentComboBox::sort_system_rows
    // compares vendors and only falls back to this sorter when they are equivalent), so both sides
    // carry the same vendor here and the Snapmaker gate is symmetric. Passing two different vendors to
    // one call would mix the configured rank with the name order and stop being a strict weak ordering,
    // which std::stable_sort requires.
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
    // Deliberately case-insensitive, unlike the ordering above: this gate activates the configured order,
    // and a spelling drift must not silently disable it.
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

std::string filament_product_key(const std::string &preset_name, const std::string &vendor)
{
    std::string product = trimmed(preset_name);
    std::string owner   = trimmed(vendor);

    // An unset vendor, or the schema placeholder, groups the preset by its own leading word.
    if (owner.empty() || owner == g_undefined_vendor)
        owner = product.substr(0, product.find_first_of(" \t"));

    if (!owner.empty() && product.size() > owner.size() && ascii_iequal(product.substr(0, owner.size()), owner) &&
        product[owner.size()] == ' ')
        product = product.substr(owner.size() + 1);

    // Printer variants share one product name, e.g. "PLA Matte @BBL X1C" is the "PLA Matte" entry.
    const size_t printer_suffix = product.find(" @");
    if (printer_suffix != std::string::npos)
        product = product.substr(0, printer_suffix);

    return trimmed(product);
}

std::filesystem::path choose_allow_list_copy(const std::filesystem::path &user_copy,
                                             const std::filesystem::path &shipped_copy,
                                             bool                         user_copy_exists)
{
    return user_copy_exists ? user_copy : shipped_copy;
}

} // namespace GUI
} // namespace Slic3r
