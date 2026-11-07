#ifndef slic3r_MiniMePricing_hpp_
#define slic3r_MiniMePricing_hpp_

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

// SliceOfMe: what to charge for a print. The same calculation as the user's pricing artifact ("Precificação"):
//   production = machine cost per hour x hours + filament cost (+ losses %)
//   base       = production + packaging + shipping
//   per scenario (low / medium / high / custom margin per hour): profit = margin x hours
//     channel fee in %:  price = (base + profit) / (1 - fee%)   total cost = base + price x fee%
//     channel fee in R$: total cost = base + fee                 price = total cost + profit
// The hours and the grams of every filament come from the sliced plate; the price of a filament comes from the user's
// filament list (brand, type, colour, spool weight, price paid). Everything is kept in <data dir>/minime_pricing.json
// and used by the pricing window and by the Claude chat's price tools alike. All functions run on the GUI thread.
namespace Slic3r::GUI::Pricing {

using json = nlohmann::json;

struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// the saved settings and filament list, with defaults for whatever is missing
json load();
void save(const json &data);

// the sliced plate's hours and filaments with their prices (plate < 0: the current one); throws Error when the plate
// has no valid slice
json plate_usage(const json &data, int plate = -1);

// the whole calculation. 'usage' as plate_usage gives it; 'overrides' may change any config value for this calculation
// only, give 'hours', per filament 'prices' ({"1": price per kg}) and a 'sell_price' to judge
json compute(const json &data, const json &usage, const json &overrides);

// changes the data (and saves it): set_config, save_filament, delete_filament, import_filaments, assign. Returns what
// changed. Throws Error for bad input.
json apply_change(json &data, const json &change);

// the key that ties a MiniMe filament slot (1-based) to a filament of the list
std::string slot_key(int slot);

// the pricing window
void show_window();
// tells an open pricing window that the data changed
void refresh_window();

} // namespace Slic3r::GUI::Pricing

#endif // slic3r_MiniMePricing_hpp_
