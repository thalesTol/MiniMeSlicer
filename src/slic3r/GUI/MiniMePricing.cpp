#include "MiniMePricing.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "GLToolbar.hpp"
#include "Widgets/WebView.hpp"

#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/clipbrd.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <wx/webview.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace Slic3r::GUI::Pricing {

namespace {

namespace fs = boost::filesystem;

double r2(double v) { return std::round(v * 100.) / 100.; }

fs::path data_file() { return fs::path(Slic3r::data_dir()) / "minime_pricing.json"; }

// the artifact's own defaults
json default_config()
{
    return {{"machine_per_hour", 1.50}, {"margin_low", 3.}, {"margin_mid", 6.}, {"margin_high", 10.}, {"packaging", 2.00},
            {"shipping", 0.},           {"fee_type", "pct"}, {"fee_value", 0.}, {"scenario", "media"}, {"custom_margin", 0.},
            {"waste_pct", 0.},          {"artifact_url", ""}};
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

double price_per_kg(const json &f)
{
    const double kg = f.value("spool_kg", 0.);
    return kg > 0. ? f.value("paid", 0.) / kg : 0.;
}

std::string filament_label(const json &f)
{
    std::string out;
    for (const char *k : {"brand", "type", "color"}) {
        const std::string v = f.value(k, std::string());
        if (!v.empty())
            out += (out.empty() ? "" : " ") + v;
    }
    return out.empty() ? "(sem nome)" : out;
}

const json *find_filament(const json &data, const std::string &id)
{
    for (const json &f : data["filaments"])
        if (f.value("id", std::string()) == id)
            return &f;
    return nullptr;
}

std::string new_id()
{
    static int n = 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return "f" + std::to_string(ms) + "_" + std::to_string(++n);
}

double number(const json &v, const char *what)
{
    double d = 0.;
    if (v.is_number())
        d = v.get<double>();
    else if (v.is_string()) {
        std::string s = trim(v.get<std::string>());
        std::replace(s.begin(), s.end(), ',', '.');
        try {
            size_t used = 0;
            d = std::stod(s, &used);
            if (used != s.size())
                throw Error("");
        } catch (...) {
            throw Error(std::string("'") + what + "' is not a number");
        }
    } else
        throw Error(std::string("'") + what + "' is not a number");
    if (!std::isfinite(d) || d < 0.)
        throw Error(std::string("'") + what + "' must be 0 or more");
    return d;
}

// where the price of a filament slot comes from: the filament the user tied to it, a price typed for it, a filament of
// the list that matches the preset's brand and type, or a price set in the user's own preset
json slot_price(const json &data, const std::string &key, const std::string &preset_name)
{
    const json &slots = data["slots"];
    if (slots.contains(key)) {
        const json &s = slots[key];
        if (s.contains("filament"))
            if (const json *f = find_filament(data, s["filament"].get<std::string>()))
                return {{"price_kg", r2(price_per_kg(*f))}, {"source", "list"}, {"filament_id", (*f)["id"]}, {"label", filament_label(*f)}};
        if (s.contains("price_kg"))
            return {{"price_kg", s["price_kg"]}, {"source", "manual"}, {"label", "pre\303\247o digitado"}};
    }
    // the list's filament whose brand is in the preset's name, preferring one whose type words are there too
    const std::string preset = lower(preset_name);
    const json       *best   = nullptr;
    int               best_score = 0;
    static const std::vector<std::string> materials = {"pla", "petg", "pet", "abs", "asa", "tpu", "pa", "pc", "pva", "hips"};
    for (const json &f : data["filaments"]) {
        // the brand's first word ("Bambu Lab" -> "bambu", as in the preset "Bambu PLA Basic @BBL A1")
        std::istringstream brand_words(lower(f.value("brand", std::string())));
        std::string        brand;
        brand_words >> brand;
        if (brand.empty() || preset.find(brand) == std::string::npos)
            continue;
        std::istringstream words(lower(f.value("type", std::string())));
        std::vector<std::string> type_words;
        for (std::string w; words >> w;)
            type_words.push_back(w);
        // a material in the type must be the preset's (a PETG preset never takes a PLA price)
        if (!type_words.empty() && std::find(materials.begin(), materials.end(), type_words.front()) != materials.end() &&
            preset.find(type_words.front()) == std::string::npos)
            continue;
        int  score = 1;
        bool all   = true;
        for (const std::string &w : type_words)
            all = all && preset.find(w) != std::string::npos;
        if (all)
            ++score;
        if (score > best_score) {
            best       = &f;
            best_score = score;
        }
    }
    if (best != nullptr)
        return {{"price_kg", r2(price_per_kg(*best))}, {"source", "auto"}, {"filament_id", (*best)["id"]}, {"label", filament_label(*best)}};
    PresetBundle *pb = wxGetApp().preset_bundle;
    if (const Preset *p = pb->filaments.find_preset(preset_name, false)) {
        if (!p->is_system)
            if (const auto *cost = p->config.option<ConfigOptionFloats>("filament_cost"); cost != nullptr && !cost->values.empty() && cost->values.front() > 0.)
                return {{"price_kg", r2(cost->values.front())}, {"source", "preset"}, {"label", "pre\303\247o do perfil"}};
    }
    return {{"price_kg", 0.}, {"source", "none"}, {"label", "sem pre\303\247o"}};
}

} // namespace

std::string slot_key(int slot)
{
    PresetBundle            *pb      = wxGetApp().preset_bundle;
    const DynamicPrintConfig full    = pb->full_config();
    const auto              *colours = full.option<ConfigOptionStrings>("filament_colour");
    const size_t             i       = size_t(std::max(slot, 1) - 1);
    if (i >= pb->filament_presets.size())
        throw Error("there is no filament " + std::to_string(slot));
    const std::string colour = colours != nullptr && i < colours->values.size() ? lower(colours->values[i]) : "";
    return pb->filament_presets[i] + "|" + colour;
}

json load()
{
    json data;
    try {
        if (fs::exists(data_file())) {
            boost::nowide::ifstream in(data_file().string());
            data = json::parse(in, nullptr, false);
        }
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MiniMe pricing: cannot read " << data_file().string() << ": " << e.what();
    }
    if (!data.is_object())
        data = json::object();
    json config = default_config();
    if (data.contains("config") && data["config"].is_object())
        for (auto &[k, v] : data["config"].items())
            if (config.contains(k) && v.type() == config[k].type())
                config[k] = v;
            else if (config.contains(k) && v.is_number() && config[k].is_number())
                config[k] = v.get<double>();
    data["config"] = config;
    if (!data.contains("filaments") || !data["filaments"].is_array()) {
        // a first list: Bambu Lab's own filaments (prices of a 1 kg spool in Brazil; the Price window can look them up again)
        data["filaments"] = json::array({{{"id", "bambu_pla_basic"}, {"brand", "Bambu Lab"}, {"type", "PLA Basic"}, {"color", ""}, {"spool_kg", 1.}, {"paid", 150.}},
                                         {{"id", "bambu_pla_lite"}, {"brand", "Bambu Lab"}, {"type", "PLA Lite"}, {"color", ""}, {"spool_kg", 1.}, {"paid", 110.}}});
    }
    if (!data.contains("slots") || !data["slots"].is_object())
        data["slots"] = json::object();
    return data;
}

void save(const json &data)
{
    const fs::path path = data_file();
    const fs::path tmp  = path.string() + ".tmp";
    {
        boost::nowide::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        if (!out)
            throw Error("cannot write " + path.string());
        out << data.dump(2);
    }
    boost::system::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec)
        throw Error("cannot write " + path.string() + ": " + ec.message());
}

std::vector<int> used_plates()
{
    PartPlateList   &pl = wxGetApp().plater()->get_partplate_list();
    std::vector<int> out;
    for (int i = 0; i < pl.get_plate_count(); ++i)
        if (!pl.get_plate(i)->get_obj_and_inst_set().empty())
            out.push_back(i);
    return out;
}

bool plate_sliced(int plate)
{
    PartPlateList &pl = wxGetApp().plater()->get_partplate_list();
    if (plate < 0 || plate >= pl.get_plate_count())
        return false;
    PartPlate            *pp  = pl.get_plate(plate);
    GCodeProcessorResult *res = pp->get_slice_result();
    return res != nullptr && pp->is_slice_result_valid() && res->print_statistics.modes[0].time > 0.f;
}

json plates_usage(const json &data, const std::vector<int> &plates)
{
    if (plates.empty())
        throw Error("no plate has objects on it");
    std::string missing;
    for (int i : plates)
        if (!plate_sliced(i))
            missing += (missing.empty() ? "" : ", ") + std::to_string(i + 1);
    if (!missing.empty())
        throw Error("plate(s) " + missing + " (counting from 1) are not sliced, or changed since: slice them first");
    json   per_plate = json::array();
    json   filaments = json::array();
    double seconds = 0., grams = 0.;
    std::string project;
    for (int i : plates) {
        const json u = plate_usage(data, i);
        project      = u["project"].get<std::string>();
        seconds += u["seconds"].get<double>();
        grams += u["grams"].get<double>();
        per_plate.push_back({{"plate", u["plate"]}, {"seconds", u["seconds"]}, {"hours", u["hours"]}, {"grams", u["grams"]}});
        // the same MiniMe filament on several plates is one line
        for (const json &f : u["filaments"]) {
            auto it = std::find_if(filaments.begin(), filaments.end(), [&](const json &g) { return g["slot"] == f["slot"]; });
            if (it == filaments.end()) {
                filaments.push_back(f);
                continue;
            }
            (*it)["grams"]       = r2((*it)["grams"].get<double>() + f["grams"].get<double>());
            (*it)["purge_grams"] = r2((*it)["purge_grams"].get<double>() + f["purge_grams"].get<double>());
        }
    }
    std::sort(filaments.begin(), filaments.end(), [](const json &a, const json &b) { return a["slot"].get<int>() < b["slot"].get<int>(); });
    return {{"plate", -1}, {"all", true}, {"plates", per_plate}, {"project", project}, {"seconds", r2(seconds)},
            {"hours", seconds / 3600.}, {"grams", r2(grams)}, {"filaments", filaments}};
}

json plate_usage(const json &data, int plate)
{
    Plater        *p  = wxGetApp().plater();
    PartPlateList &pl = p->get_partplate_list();
    const int      idx = plate < 0 ? pl.get_curr_plate_index() : plate;
    if (idx < 0 || idx >= pl.get_plate_count())
        throw Error("there is no plate " + std::to_string(idx + 1));
    PartPlate            *pp  = pl.get_plate(idx);
    GCodeProcessorResult *res = pp->get_slice_result();
    if (res == nullptr || !pp->is_slice_result_valid() || res->print_statistics.modes[0].time <= 0.f)
        throw Error("plate " + std::to_string(idx + 1) + " is not sliced, or changed since it was: slice it first");
    const PrintEstimatedStatistics &ps   = res->print_statistics;
    PresetBundle                   *pb   = wxGetApp().preset_bundle;
    const DynamicPrintConfig        full = pb->full_config();
    const auto *colours = full.option<ConfigOptionStrings>("filament_colour");
    const auto *types   = full.option<ConfigOptionStrings>("filament_type");
    auto        vol_of  = [](const std::map<size_t, double> &m, size_t id) {
        auto it = m.find(id);
        return it == m.end() ? 0. : it->second;
    };
    json   filaments = json::array();
    double grams_all = 0.;
    for (const auto &[id, volume] : ps.total_volumes_per_extruder) {
        if (volume <= 0.)
            continue;
        const double density = id < res->filament_densities.size() && res->filament_densities[id] > 0.f ? res->filament_densities[id] : 1.24;
        const double grams   = volume * density / 1000.;
        const double purge   = (vol_of(ps.flush_per_filament, id) + vol_of(ps.wipe_tower_volumes_per_extruder, id) + vol_of(ps.purge_volumes_per_extruder, id)) * density / 1000.;
        const std::string preset = id < pb->filament_presets.size() ? pb->filament_presets[id] : "";
        const std::string colour = colours != nullptr && id < colours->values.size() ? colours->values[id] : "";
        const std::string key    = preset + "|" + lower(colour);
        json f = {{"slot", int(id) + 1}, {"key", key}, {"preset", preset}, {"colour", colour},
                  {"type", types != nullptr && id < types->values.size() ? types->values[id] : ""},
                  {"grams", r2(grams)}, {"purge_grams", r2(purge)}};
        f.update(slot_price(data, key, preset));
        filaments.push_back(f);
        grams_all += grams;
    }
    const double seconds = ps.modes[0].time;
    return {{"plate", idx + 1}, {"project", into_u8(p->get_project_name())}, {"seconds", r2(seconds)},
            {"hours", seconds / 3600.}, {"grams", r2(grams_all)}, {"filaments", filaments}};
}

json compute(const json &data, const json &usage, const json &overrides)
{
    json cfg = data["config"];
    for (auto &[k, v] : overrides.items())
        if (cfg.contains(k) && k != "artifact_url") {
            if (k == "fee_type" || k == "scenario")
                cfg[k] = lower(v.get<std::string>());
            else
                cfg[k] = number(v, k.c_str());
        }
    const std::string fee_type = cfg["fee_type"].get<std::string>();
    const std::string chosen   = cfg["scenario"].get<std::string>();
    if (fee_type != "pct" && fee_type != "brl")
        throw Error("'fee_type' is pct (a share of the price) or brl (a fixed amount)");
    if (chosen != "baixa" && chosen != "media" && chosen != "alta" && chosen != "personalizada")
        throw Error("'scenario' is baixa, media, alta or personalizada");

    const double hours = overrides.contains("hours") ? number(overrides["hours"], "hours") : usage.value("hours", 0.);
    if (hours <= 0.)
        throw Error("the print time is 0");

    json   filaments     = json::array();
    double filament_cost = 0., grams = 0.;
    bool   missing_price = false;
    for (json f : usage["filaments"]) {
        const std::string slot = std::to_string(f["slot"].get<int>());
        if (overrides.contains("prices") && overrides["prices"].contains(slot)) {
            f["price_kg"] = number(overrides["prices"][slot], "prices");
            f["source"]   = "manual";
            f["label"]    = "pre\303\247o desta conta";
        }
        const double cost = f["price_kg"].get<double>() / 1000. * f["grams"].get<double>();
        missing_price     = missing_price || f["price_kg"].get<double>() <= 0.;
        f["cost"]         = r2(cost);
        filament_cost += cost;
        grams += f["grams"].get<double>();
        filaments.push_back(f);
    }

    const double machine    = cfg["machine_per_hour"].get<double>() * hours;
    const double waste_pct  = cfg["waste_pct"].get<double>();
    const double production = (machine + filament_cost) * (1. + waste_pct / 100.);
    const double packaging  = cfg["packaging"].get<double>();
    const double shipping   = cfg["shipping"].get<double>();
    double       fee        = cfg["fee_value"].get<double>();
    if (fee_type == "pct" && fee >= 100.)
        fee = 99.;
    const double base = production + packaging + shipping;

    // the price and total cost for a profit
    auto price_for = [&](double profit, double &total_cost) {
        if (fee_type == "pct" && fee > 0.) {
            const double price = (base + profit) / (1. - fee / 100.);
            total_cost         = base + price * fee / 100.;
            return price;
        }
        total_cost = base + fee;
        return total_cost + profit;
    };

    struct Scenario { const char *key, *label; double margin; };
    std::vector<Scenario> scenarios = {{"baixa", "Baixa", cfg["margin_low"].get<double>()},
                                       {"media", "M\303\251dia", cfg["margin_mid"].get<double>()},
                                       {"alta", "Alta", cfg["margin_high"].get<double>()}};
    if (chosen == "personalizada")
        scenarios.push_back({"personalizada", "Personalizada", cfg["custom_margin"].get<double>()});
    json out_scenarios = json::array();
    for (const Scenario &s : scenarios) {
        const double profit = s.margin * hours;
        double       total  = 0.;
        const double price  = price_for(profit, total);
        out_scenarios.push_back({{"key", s.key}, {"label", s.label}, {"margin_per_hour", r2(s.margin)}, {"profit", r2(profit)},
                                 {"total_cost", r2(total)}, {"price", r2(price)}, {"chosen", chosen == s.key},
                                 {"profit_pct_of_price", price > 0. ? r2(profit / price * 100.) : 0.},
                                 {"price_per_gram", grams > 0. ? r2(price / grams) : 0.}});
    }
    double       break_even_cost = 0.;
    const double break_even      = price_for(0., break_even_cost);

    json j = {{"hours", hours},
              {"grams", r2(grams)},
              {"filaments", filaments},
              {"costs", {{"filament", r2(filament_cost)}, {"machine", r2(machine)}, {"waste", r2(production - machine - filament_cost)},
                         {"production", r2(production)}, {"packaging", r2(packaging)}, {"shipping", r2(shipping)},
                         {"fee_type", fee_type}, {"fee_value", r2(fee)}, {"base", r2(base)}}},
              {"scenarios", out_scenarios},
              {"chosen", chosen},
              {"break_even_price", r2(break_even)},
              {"config", cfg}};
    if (missing_price)
        j["warning"] = "a filament has no price (price 0): pick it from the filament list or type its price per kg";

    // is a price worth it?
    if (overrides.contains("sell_price") && !overrides["sell_price"].is_null() &&
        !(overrides["sell_price"].is_string() && trim(overrides["sell_price"].get<std::string>()).empty())) {
        const double price      = number(overrides["sell_price"], "sell_price");
        const double fee_amount = fee_type == "pct" ? price * fee / 100. : fee;
        const double net        = price - base - fee_amount;
        const double per_hour   = net / hours;
        std::string  verdict, text;
        if (net < 0.) {
            verdict = "loss";
            text    = "Preju\303\255zo: o pre\303\247o n\303\243o cobre os custos.";
        } else if (per_hour < cfg["margin_low"].get<double>()) {
            verdict = "below_low";
            text    = "Cobre os custos, mas o lucro por hora fica abaixo da sua margem baixa.";
        } else if (per_hour < cfg["margin_mid"].get<double>()) {
            verdict = "low";
            text    = "Lucro na faixa da margem baixa.";
        } else if (per_hour < cfg["margin_high"].get<double>()) {
            verdict = "mid";
            text    = "Lucro na faixa da margem m\303\251dia.";
        } else {
            verdict = "high";
            text    = "Lucro na margem alta ou acima: vale a pena.";
        }
        j["evaluation"] = {{"sell_price", r2(price)},  {"fee", r2(fee_amount)}, {"net_profit", r2(net)},
                           {"profit_per_hour", r2(per_hour)}, {"profit_pct_of_price", price > 0. ? r2(net / price * 100.) : 0.},
                           {"verdict", verdict}, {"verdict_text", text}};
    }
    return j;
}

json apply_change(json &data, const json &change)
{
    const std::string action = change.value("action", "");
    json              result = {{"action", action}};
    if (action == "set_config") {
        if (!change.contains("values") || !change["values"].is_object())
            throw Error("give 'values', e.g. {\"machine_per_hour\": 1.5}");
        json &cfg = data["config"];
        for (auto &[k, v] : change["values"].items()) {
            if (!cfg.contains(k))
                throw Error("unknown setting '" + k + "'");
            if (k == "fee_type") {
                const std::string t = lower(v.get<std::string>());
                if (t != "pct" && t != "brl")
                    throw Error("'fee_type' is pct or brl");
                cfg[k] = t;
            } else if (k == "scenario") {
                const std::string s = lower(v.get<std::string>());
                if (s != "baixa" && s != "media" && s != "alta" && s != "personalizada")
                    throw Error("'scenario' is baixa, media, alta or personalizada");
                cfg[k] = s;
            } else if (k == "artifact_url") {
                const std::string url = trim(v.get<std::string>());
                if (!url.empty() && url.rfind("https://", 0) != 0)
                    throw Error("'artifact_url' must start with https://");
                cfg[k] = url;
            } else {
                const double d = number(v, k.c_str());
                if ((k == "fee_value" && cfg["fee_type"] == "pct" && d >= 100.) || (k == "waste_pct" && d > 500.))
                    throw Error("'" + k + "' is too large");
                cfg[k] = d;
            }
        }
        result["config"] = cfg;
    } else if (action == "save_filament" || action == "import_filaments") {
        json list = action == "save_filament" ? json::array({change.value("filament", json::object())}) : change.value("filaments", json::array());
        if (!list.is_array() || list.empty())
            throw Error(action == "save_filament" ? "give 'filament'" : "give 'filaments', a list");
        int added = 0, updated = 0;
        json ids = json::array();
        for (const json &in : list) {
            if (!in.is_object())
                throw Error("each filament is {brand, type, color, spool_kg, paid}");
            json f = {{"brand", trim(in.value("brand", std::string()))}, {"type", trim(in.value("type", std::string()))},
                      {"color", trim(in.value("color", std::string()))},
                      {"spool_kg", number(in.value("spool_kg", json(1.)), "spool_kg")}, {"paid", number(in.value("paid", json(0.)), "paid")}};
            if (f["spool_kg"].get<double>() <= 0.)
                throw Error("'spool_kg' (the spool's weight in kg) must be more than 0");
            if (f["brand"].get<std::string>().empty() && f["type"].get<std::string>().empty())
                throw Error("give at least the brand or the type");
            // the one to change: by id, or (importing) the one with the same brand, type and colour
            json *target = nullptr;
            const std::string id = in.value("id", std::string());
            for (json &e : data["filaments"]) {
                if (!id.empty() ? e.value("id", std::string()) == id :
                                  action == "import_filaments" && lower(e.value("brand", std::string())) == lower(f["brand"].get<std::string>()) &&
                                      lower(e.value("type", std::string())) == lower(f["type"].get<std::string>()) &&
                                      lower(e.value("color", std::string())) == lower(f["color"].get<std::string>())) {
                    target = &e;
                    break;
                }
            }
            if (!id.empty() && target == nullptr)
                throw Error("there is no filament with id " + id);
            if (target != nullptr) {
                f["id"] = (*target)["id"];
                *target = f;
                ++updated;
            } else {
                f["id"] = new_id();
                data["filaments"].push_back(f);
                ++added;
            }
            ids.push_back(f["id"]);
        }
        result["added"]   = added;
        result["updated"] = updated;
        result["ids"]     = ids;
    } else if (action == "delete_filament") {
        const std::string id = change.value("id", std::string());
        json &list = data["filaments"];
        const size_t before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(), [&id](const json &f) { return f.value("id", std::string()) == id; }), list.end());
        if (list.size() == before)
            throw Error("there is no filament with id " + id);
        for (auto it = data["slots"].begin(); it != data["slots"].end();)
            if (it->contains("filament") && (*it)["filament"] == id)
                it = data["slots"].erase(it);
            else
                ++it;
        result["deleted"] = id;
    } else if (action == "assign") {
        std::string key = change.value("key", std::string());
        if (key.empty()) {
            if (!change.contains("slot") || !change["slot"].is_number_integer())
                throw Error("give 'slot', the MiniMe filament number");
            key = slot_key(change["slot"].get<int>());
        }
        const bool has_filament = change.contains("filament") && change["filament"].is_string() && !change["filament"].get<std::string>().empty();
        const bool has_price    = change.contains("price_kg") && !change["price_kg"].is_null() &&
                               !(change["price_kg"].is_string() && trim(change["price_kg"].get<std::string>()).empty());
        if (has_filament) {
            if (find_filament(data, change["filament"].get<std::string>()) == nullptr)
                throw Error("there is no filament with id " + change["filament"].get<std::string>());
            data["slots"][key] = {{"filament", change["filament"]}};
        } else if (has_price)
            data["slots"][key] = {{"price_kg", number(change["price_kg"], "price_kg")}};
        else
            data["slots"].erase(key);
        result["key"] = key;
    } else
        throw Error("'action' is set_config, save_filament, import_filaments, delete_filament or assign");
    save(data);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// the window

namespace {

// asks the Claude chat to look up today's prices on the web and save them with the pricing tool
void ask_claude_for_prices(const std::string &id)
{
    json data = load();
    std::string which = "every filament in my list";
    if (!id.empty()) {
        const json *f = find_filament(data, id);
        if (f == nullptr)
            return;
        which = "the filament with id " + id + " (" + filament_label(*f) + ", " + std::to_string(f->value("spool_kg", 1.)).substr(0, 4) + " kg spool)";
    }
    const std::string text =
        "Find today's price in Brazil of " + which + " in my MiniMe price list, and update it.\n"
        "1. Read the list with the pricing tool (action=get).\n"
        "2. For each one, search the web for that exact product (same brand, type and spool weight; the colour too if it changes the "
        "price), new, in R$: the brand's own Brazilian store first, then Mercado Livre, Amazon.com.br, Shopee and big 3D printing "
        "shops. Use the usual price of one spool (not kits, used items, or coupon or cart-only prices). If a store sells another "
        "spool weight, convert to the list's weight.\n"
        "3. Save each new price with pricing action=save_filament (same id; paid = the price of its spool).\n"
        "4. Then show me a table: filament, old price, new price, store with link. Keep the old price where you found nothing "
        "reliable, and say so.";
    if (MainFrame *frame = wxGetApp().mainframe)
        frame->ask_chat(text);
}

class PricingWindow : public wxDialog
{
public:
    explicit PricingWindow(wxWindow *parent)
        : wxDialog(parent, wxID_ANY, _L("Price") + wxString::FromUTF8(" \302\267 MiniMe"), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX | wxMINIMIZE_BOX)
    {
        SetBackgroundColour(wxGetApp().dark_mode() ? wxColour(18, 22, 31) : wxColour(247, 248, 251));
        SetSizer(new wxBoxSizer(wxVERTICAL));
        SetSize(FromDIP(wxSize(1000, 780)));
        SetMinSize(FromDIP(wxSize(560, 480)));
        CenterOnParent();

        wxString url = wxString::Format("file://%s/web/minime_price/index.html", from_u8(resources_dir()));
        url.Replace("\\", "/");
        m_browser = WebView::CreateWebView(this, url, "minime_price");
        if (m_browser == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "MiniMe pricing: cannot create the web view";
            return;
        }
        GetSizer()->Add(m_browser, 1, wxEXPAND);
        Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PricingWindow::on_message, this, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_NAVIGATING, [](wxWebViewEvent &evt) {
            const wxString target = evt.GetURL();
            if (target.StartsWith("file:") || target.StartsWith("about:") || target.StartsWith("data:"))
                return;
            evt.Veto();
            if (target.StartsWith("https://"))
                wxLaunchDefaultBrowser(target);
        }, m_browser->GetId());
        Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent &evt) {
            if (evt.GetURL().StartsWith("https://"))
                wxLaunchDefaultBrowser(evt.GetURL());
        }, m_browser->GetId());
        // the plate's slice changes while the window is open: follow it
        m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent &) { poll(); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &) {
            m_timer.Stop();
            Hide();
        });
        Bind(wxEVT_SHOW, [this](wxShowEvent &evt) {
            if (evt.IsShown()) {
                m_signature.clear();
                if (m_ready)
                    send_state();
                m_timer.Start(700);
            }
            evt.Skip();
        });
    }

    void send_state()
    {
        if (m_browser == nullptr || !m_ready)
            return;
        json state = {{"kind", "state"}, {"dark", wxGetApp().dark_mode()}};
        try {
            json data = load();
            json filaments = json::array();
            for (json f : data["filaments"]) {
                f["price_kg"] = r2(price_per_kg(f));
                f["label"]    = filament_label(f);
                filaments.push_back(f);
            }
            state["config"]    = data["config"];
            state["filaments"] = filaments;
            Plater *p          = wxGetApp().plater();
            state["slicing"]   = p->is_background_process_slicing();
            state["plate"]     = p->get_partplate_list().get_curr_plate_index() + 1;
            state["all"]       = m_all;
            try {
                json usage;
                if (m_all) {
                    const std::vector<int> plates = used_plates();
                    json unsliced = json::array();
                    for (int i : plates)
                        if (!plate_sliced(i))
                            unsliced.push_back(i + 1);
                    state["used_plates"] = plates.size();
                    state["unsliced"]    = unsliced;
                    usage                = plates_usage(data, plates);
                } else
                    usage = plate_usage(data);
                state["usage"]   = usage;
                json ov          = json::object();
                if (!m_sell_price.empty())
                    ov["sell_price"] = m_sell_price;
                state["result"] = compute(data, usage, ov);
            } catch (const Error &e) {
                state["usage_error"] = e.what();
            }
        } catch (const std::exception &e) {
            state["error"] = e.what();
        }
        WebView::RunScript(m_browser, wxString::FromUTF8("window.mm && window.mm.recv(" + state.dump() + ");"));
    }

private:
    void poll()
    {
        if (!IsShown())
            return;
        Plater        *p  = wxGetApp().plater();
        PartPlateList &pl = p->get_partplate_list();
        std::string    sig = std::to_string(pl.get_curr_plate_index()) + (m_all ? "a" : "") + (p->is_background_process_slicing() ? "s" : "");
        for (int i = 0; i < pl.get_plate_count(); ++i) {
            if (!m_all && i != pl.get_curr_plate_index())
                continue;
            PartPlate *plate = pl.get_plate(i);
            sig += "|" + std::to_string(plate->get_obj_and_inst_set().size()) + (plate->is_slice_result_valid() ? "v" : "x");
            if (GCodeProcessorResult *res = plate->get_slice_result())
                sig += std::to_string(res->print_statistics.modes[0].time);
        }
        sig += std::to_string(wxGetApp().preset_bundle->filament_presets.size());
        if (sig != m_signature) {
            m_signature = sig;
            send_state();
        }
    }

    void on_message(wxWebViewEvent &evt)
    {
        json msg = json::parse(evt.GetString().ToUTF8().data(), nullptr, false);
        if (!msg.is_object())
            return;
        const std::string cmd = msg.value("cmd", "");
        if (msg.contains("sell_price") && msg["sell_price"].is_string())
            m_sell_price = msg["sell_price"].get<std::string>();
        if (cmd == "ready") {
            m_ready = true;
            send_state();
        } else if (cmd == "refresh") {
            send_state();
        } else if (cmd == "change") {
            try {
                json data = load();
                apply_change(data, msg.value("change", json::object()));
            } catch (const std::exception &e) {
                const json err = {{"kind", "toast"}, {"text", e.what()}, {"error", true}};
                WebView::RunScript(m_browser, wxString::FromUTF8("window.mm && window.mm.recv(" + err.dump() + ");"));
            }
            send_state();
        } else if (cmd == "scope") {
            m_all = msg.value("all", false);
            m_signature.clear();
            send_state();
        } else if (cmd == "slice") {
            Plater *p = wxGetApp().plater();
            if (m_all)
                wxPostEvent(p, SimpleEvent(EVT_GLTOOLBAR_SLICE_ALL));
            else if (!p->get_partplate_list().get_curr_plate()->get_obj_and_inst_set().empty())
                wxPostEvent(p, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
        } else if (cmd == "find_prices") {
            ask_claude_for_prices(msg.value("id", std::string()));
        } else if (cmd == "open_url") {
            const std::string url = msg.value("url", "");
            if (url.rfind("https://", 0) == 0)
                wxLaunchDefaultBrowser(wxString::FromUTF8(url));
        } else if (cmd == "copy") {
            if (wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(msg.value("text", ""))));
                wxTheClipboard->Close();
            }
        } else if (cmd == "close") {
            m_timer.Stop();
            Hide();
        }
    }

    wxWebView  *m_browser{nullptr};
    bool        m_ready{false};
    bool        m_all{false};
    wxTimer     m_timer;
    std::string m_signature;
    std::string m_sell_price;
};

PricingWindow *s_window = nullptr;

} // namespace

void show_window()
{
    if (s_window == nullptr)
        s_window = new PricingWindow(wxGetApp().mainframe);
    s_window->Show();
    s_window->Raise();
}

void refresh_window()
{
    if (s_window != nullptr && s_window->IsShown())
        s_window->send_state();
}

} // namespace Slic3r::GUI::Pricing
