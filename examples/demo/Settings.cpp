#include "Settings.h"

#include <quench/core/Json.h>

#include <algorithm>
#include <format>

namespace demo {

using namespace quench;

const char* touchControlsName(TouchControls touch) {
    switch (touch) {
    case TouchControls::Auto: return "auto";
    case TouchControls::On: return "on";
    case TouchControls::Off: return "off";
    }
    return "auto";
}

Result<Settings> parseSettings(const std::string& text) {
    Result<json::Value> parsed = json::parse(text);
    if (!parsed) return Error{parsed.error()};
    const json::Value& v = *parsed;
    if (!v.isObject()) return Error{"not a JSON object"};
    if (!v["version"].isNumber()) return Error{"no version"};
    if (const double version = v["version"].number; version != Settings::kVersion)
        return Error{std::format("version {} (this demo reads {})", version, Settings::kVersion)};
    Settings s;
    s.section = v["section"].stringOr("");
    s.showUi = v["show_ui"].boolOr(true);
    s.uiScale = std::clamp(float(v["ui_scale"].numberOr(1.0)), 0.5f, 3.0f);
    const std::string touch = v["touch_controls"].stringOr("auto");
    s.touch = touch == "on" ? TouchControls::On : touch == "off" ? TouchControls::Off : TouchControls::Auto;
    s.hdr = v["hdr"].stringOr("");
    if (s.hdr != "off" && s.hdr != "on" && s.hdr != "auto") s.hdr.clear();
    s.present = v["present_mode"].stringOr("");
    if (s.present != "vsync" && s.present != "immediate" && s.present != "mailbox") s.present.clear();
    s.tourSeconds = std::clamp(float(v["tour_seconds"].numberOr(8.0)), 1.0f, 120.0f);
    s.adaptToHeat = v["adapt_to_heat"].boolOr(false);
    s.adaptToLoad = v["adapt_to_load"].boolOr(false);
    s.graphics = parseGraphics(v["graphics"]);
    return s;
}

std::string writeSettings(const Settings& s) {
    json::Value v = json::Value::makeObject();
    v.object["version"] = json::Value::makeNumber(Settings::kVersion);
    v.object["section"] = json::Value::makeString(s.section);
    v.object["show_ui"] = json::Value::makeBool(s.showUi);
    v.object["ui_scale"] = json::Value::makeNumber(double(s.uiScale));
    v.object["touch_controls"] = json::Value::makeString(touchControlsName(s.touch));
    v.object["hdr"] = json::Value::makeString(s.hdr);
    v.object["present_mode"] = json::Value::makeString(s.present);
    v.object["tour_seconds"] = json::Value::makeNumber(double(s.tourSeconds));
    v.object["adapt_to_heat"] = json::Value::makeBool(s.adaptToHeat);
    v.object["adapt_to_load"] = json::Value::makeBool(s.adaptToLoad);
    v.object["graphics"] = writeGraphics(s.graphics);
    return json::write(v) + "\n";
}

} // namespace demo
