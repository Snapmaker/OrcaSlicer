#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>

namespace Slic3r
{
namespace GUI
{

/** @brief Holds the immutable values used to order a filament popup row. */
struct FilamentSortItem
{
    wxString    display_name;
    std::string vendor;
    std::string filament_product;
    size_t      original_index{0};
};

/** @brief Stores the optional vendor-specific TopN filament order. */
class FilamentTopNOrder
{
public:
    FilamentTopNOrder() = default;

    /** @brief Parses a TopN configuration stream and returns an empty order on invalid input. */
    static FilamentTopNOrder from_stream(std::istream &stream);

    /** @brief Returns the configured rank or the maximum value when no rank exists. */
    size_t rank(const std::string &vendor, const std::string &filament_product) const;

    /** @brief Reports whether the configuration contains at least one valid vendor order. */
    bool empty() const;

private:
    using Order  = std::vector<std::string>;
    using Orders = std::vector<std::pair<std::string, Order>>;

    explicit FilamentTopNOrder(Orders orders);

    Orders m_orders;
};

/** @brief Defines an overridable ordering for filament rows. */
class FilamentSorter
{
public:
    FilamentSorter() = default;
    virtual ~FilamentSorter() = default;

    FilamentSorter(const FilamentSorter &)            = delete;
    FilamentSorter &operator=(const FilamentSorter &) = delete;
    FilamentSorter(FilamentSorter &&)                 = delete;
    FilamentSorter &operator=(FilamentSorter &&)      = delete;

    /** @brief Returns whether @p left precedes @p right. */
    virtual bool less(const FilamentSortItem &left, const FilamentSortItem &right) const;

protected:
    /** @brief Applies the default display-name ordering. */
    bool less_by_name(const FilamentSortItem &left, const FilamentSortItem &right) const;
};

/** @brief Defines an overridable ordering for filament vendors. */
class FilamentVendorSorter
{
public:
    FilamentVendorSorter() = default;
    virtual ~FilamentVendorSorter() = default;

    FilamentVendorSorter(const FilamentVendorSorter &)            = delete;
    FilamentVendorSorter &operator=(const FilamentVendorSorter &) = delete;
    FilamentVendorSorter(FilamentVendorSorter &&)                 = delete;
    FilamentVendorSorter &operator=(FilamentVendorSorter &&)      = delete;

    /** @brief Returns whether vendor @p left precedes vendor @p right. */
    virtual bool less(const std::string &left, const std::string &right) const;
};

/** @brief Prioritizes Snapmaker and Generic vendors before lexical vendor order. */
class SystemFilamentVendorSorter final : public FilamentVendorSorter
{
public:
    bool less(const std::string &left, const std::string &right) const override;
};

/** @brief Applies Snapmaker TopN ordering before the default name ordering. */
class SystemFilamentSorter final : public FilamentSorter
{
public:
    /** @brief Creates a sorter using the supplied immutable TopN order. */
    explicit SystemFilamentSorter(FilamentTopNOrder topn_order);

    bool less(const FilamentSortItem &left, const FilamentSortItem &right) const override;

private:
    FilamentTopNOrder m_topn_order;
};

/** @brief Reports whether a vendor is Snapmaker, ignoring case. */
bool is_snapmaker_vendor(const std::string &vendor);

/** @brief Normalizes the known system vendor names to their canonical spelling. */
std::string canonical_vendor(const std::string &vendor);

} // namespace GUI
} // namespace Slic3r
