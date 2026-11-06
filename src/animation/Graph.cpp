#include <quench/animation/Graph.h>

#include <quench/core/Json.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <set>

namespace quench::animation {

namespace {

using json::Value;

Error fail(std::string text) { return Error{std::move(text)}; }

int findState(const LayerDef& layer, std::string_view name) {
    for (std::size_t i = 0; i < layer.states.size(); ++i)
        if (layer.states[i].name == name) return int(i);
    return -1;
}

Result<int> parameterRef(const GraphDef& graph, const Value& v, std::string_view what,
                         std::initializer_list<ParameterType> types) {
    if (v.type != Value::Type::String) return fail(std::format("{}: a parameter name", what));
    const int index = graph.findParameter(v.string);
    if (index < 0) return fail(std::format("{}: no parameter '{}'", what, v.string));
    const ParameterType type = graph.parameters[std::size_t(index)].type;
    if (std::find(types.begin(), types.end(), type) == types.end())
        return fail(std::format("{}: parameter '{}' has the wrong type", what, v.string));
    return index;
}

Result<CompareOp> parseOp(const std::string& op) {
    if (op == ">") return CompareOp::Greater;
    if (op == ">=") return CompareOp::GreaterEqual;
    if (op == "<") return CompareOp::Less;
    if (op == "<=") return CompareOp::LessEqual;
    if (op == "==") return CompareOp::Equal;
    if (op == "!=") return CompareOp::NotEqual;
    return fail(std::format("unknown comparison '{}' (>, >=, <, <=, ==, !=)", op));
}

Result<StateDef> parseState(const GraphDef& graph, const Value& v, std::string_view where) {
    if (!v.isObject()) return fail(std::format("{}: a state is an object", where));
    StateDef s;
    s.name = v["name"].stringOr("");
    if (s.name.empty()) return fail(std::format("{}: a state without a name", where));
    const std::string what = std::format("{} state '{}'", where, s.name);
    s.clip = v["clip"].stringOr("");
    if (v.has("blend")) {
        const Value& blend = v["blend"];
        if (!s.clip.empty()) return fail(std::format("{}: a clip or a blend tree, not both", what));
        Result<int> p = parameterRef(graph, blend["parameter"], what + " blend", {ParameterType::Float});
        if (!p) return Error{p.error()};
        s.blendParameter = *p;
        if (!blend["clips"].isArray() || blend["clips"].array.empty())
            return fail(std::format("{}: blend.clips is a list of {{\"clip\", \"at\"}}", what));
        for (const Value& point : blend["clips"].array) {
            BlendPoint bp{point["clip"].stringOr(""), float(point["at"].numberOr(NAN))};
            if (bp.clip.empty() || !std::isfinite(bp.at))
                return fail(std::format("{}: a blend point needs \"clip\" and \"at\"", what));
            s.blend.push_back(std::move(bp));
        }
        std::ranges::stable_sort(s.blend, {}, &BlendPoint::at);
    }
    s.speed = float(v["speed"].numberOr(1.0));
    if (!std::isfinite(s.speed) || s.speed < 0.0f) return fail(std::format("{}: speed must be at least 0", what));
    if (v.has("speedParameter")) {
        Result<int> p = parameterRef(graph, v["speedParameter"], what + " speedParameter", {ParameterType::Float});
        if (!p) return Error{p.error()};
        s.speedParameter = *p;
    }
    if (v.has("loop")) {
        if (v["loop"].type != Value::Type::Bool) return fail(std::format("{}: loop is true or false", what));
        s.loop = v["loop"].boolean;
    }
    return s;
}

Result<TransitionDef> parseTransition(const GraphDef& graph, const LayerDef& layer, const Value& v,
                                      std::string_view where, std::size_t index) {
    const std::string what = std::format("{} transition {}", where, index);
    if (!v.isObject()) return fail(std::format("{}: an object", what));
    TransitionDef t;
    const std::string from = v["from"].stringOr("");
    if (from.empty()) return fail(std::format("{}: \"from\" is a state or \"*\"", what));
    if (from != "*") {
        t.from = findState(layer, from);
        if (t.from < 0) return fail(std::format("{}: no state '{}'", what, from));
    }
    const std::string to = v["to"].stringOr("");
    t.to = findState(layer, to);
    if (t.to < 0) return fail(std::format("{}: no state '{}'", what, to));
    if (t.from == t.to) return fail(std::format("{}: from and to are the same state", what));
    t.duration = float(v["duration"].numberOr(0.2));
    if (!std::isfinite(t.duration) || t.duration < 0.0f)
        return fail(std::format("{}: duration must be at least 0", what));
    if (v.has("trigger")) {
        Result<int> p = parameterRef(graph, v["trigger"], what + " trigger", {ParameterType::Trigger});
        if (!p) return Error{p.error()};
        t.trigger = *p;
    }
    if (v.has("exitTime")) {
        t.exitTime = float(v["exitTime"].numberOr(-1.0));
        if (!std::isfinite(t.exitTime) || t.exitTime < 0.0f)
            return fail(std::format("{}: exitTime is a normalized time of at least 0", what));
    }
    if (v.has("conditions")) {
        if (!v["conditions"].isArray()) return fail(std::format("{}: conditions is a list", what));
        for (const Value& c : v["conditions"].array) {
            Result<int> p =
                parameterRef(graph, c["parameter"], what + " condition", {ParameterType::Float, ParameterType::Bool});
            if (!p) return Error{p.error()};
            ConditionDef cond;
            cond.parameter = *p;
            Result<CompareOp> op = parseOp(c["op"].stringOr(""));
            if (!op) return Error{std::format("{}: {}", what, op.error())};
            cond.op = *op;
            const Value& value = c["value"];
            if (graph.parameters[std::size_t(*p)].type == ParameterType::Bool) {
                if (value.type != Value::Type::Bool)
                    return fail(std::format("{}: compare a bool with true or false", what));
                if (cond.op != CompareOp::Equal && cond.op != CompareOp::NotEqual)
                    return fail(std::format("{}: bools compare with == or !=", what));
                cond.value = value.boolean ? 1.0f : 0.0f;
            } else {
                if (!value.isNumber()) return fail(std::format("{}: compare a float with a number", what));
                cond.value = float(value.number);
            }
            t.conditions.push_back(cond);
        }
    }
    if (t.trigger < 0 && t.exitTime < 0.0f && t.conditions.empty())
        return fail(std::format("{}: needs a trigger, an exit time or conditions", what));
    return t;
}

} // namespace

int GraphDef::findParameter(std::string_view name) const {
    for (std::size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].name == name) return int(i);
    return -1;
}

std::vector<std::string> GraphDef::clipNames() const {
    std::set<std::string> names;
    for (const LayerDef& layer : layers)
        for (const StateDef& state : layer.states) {
            if (!state.clip.empty()) names.insert(state.clip);
            for (const BlendPoint& p : state.blend) names.insert(p.clip);
        }
    return {names.begin(), names.end()};
}

Result<GraphDef> parseGraph(std::string_view text) {
    Result<Value> doc = json::parse(std::string(text));
    if (!doc) return Error{doc.error()};
    const Value& root = *doc;
    if (!root.isObject()) return fail("an animation graph is a JSON object");
    GraphDef graph;
    if (root.has("parameters")) {
        if (!root["parameters"].isObject()) return fail("parameters: an object of name: initial value");
        for (const auto& [name, value] : root["parameters"].object) {
            ParameterDef p{name};
            if (value.isNumber()) {
                p.type = ParameterType::Float;
                p.initial = float(value.number);
            } else if (value.type == Value::Type::Bool) {
                p.type = ParameterType::Bool;
                p.initial = value.boolean ? 1.0f : 0.0f;
            } else if (value.type == Value::Type::String && value.string == "trigger") {
                p.type = ParameterType::Trigger;
            } else {
                return fail(std::format("parameter '{}': a number, true or false, or \"trigger\"", name));
            }
            graph.parameters.push_back(std::move(p));
        }
    }
    if (!root["layers"].isArray() || root["layers"].array.empty()) return fail("layers: a list of at least one layer");
    for (std::size_t l = 0; l < root["layers"].array.size(); ++l) {
        const Value& v = root["layers"].array[l];
        LayerDef layer;
        layer.name = v["name"].stringOr(std::format("layer{}", l));
        const std::string where = std::format("layer '{}'", layer.name);
        if (std::ranges::any_of(graph.layers, [&](const LayerDef& o) { return o.name == layer.name; }))
            return fail(std::format("two layers are named '{}'", layer.name));
        const std::string blend = v["blend"].stringOr("override");
        if (blend == "additive")
            layer.blend = LayerBlend::Additive;
        else if (blend != "override")
            return fail(std::format("{}: blend is \"override\" or \"additive\"", where));
        if (l == 0 && layer.blend == LayerBlend::Additive) return fail("the first layer cannot be additive");
        if (v.has("mask")) {
            if (!v["mask"].isArray()) return fail(std::format("{}: mask is a list of joint names", where));
            for (const Value& j : v["mask"].array) {
                if (j.type != Value::Type::String || j.string.empty())
                    return fail(std::format("{}: mask is a list of joint names", where));
                layer.mask.push_back(j.string);
            }
        }
        layer.weight = float(v["weight"].numberOr(1.0));
        if (!std::isfinite(layer.weight) || layer.weight < 0.0f || layer.weight > 1.0f)
            return fail(std::format("{}: weight is 0..1", where));
        if (v.has("weightParameter")) {
            Result<int> p =
                parameterRef(graph, v["weightParameter"], where + " weightParameter", {ParameterType::Float});
            if (!p) return Error{p.error()};
            layer.weightParameter = *p;
        }
        if (!v["states"].isArray() || v["states"].array.empty())
            return fail(std::format("{}: states is a list of at least one state", where));
        for (const Value& s : v["states"].array) {
            Result<StateDef> state = parseState(graph, s, where);
            if (!state) return Error{state.error()};
            if (findState(layer, state->name) >= 0)
                return fail(std::format("{}: two states are named '{}'", where, state->name));
            layer.states.push_back(std::move(*state));
        }
        const std::string start = v["start"].stringOr(layer.states[0].name);
        layer.start = findState(layer, start);
        if (layer.start < 0) return fail(std::format("{}: no start state '{}'", where, start));
        if (v.has("transitions")) {
            if (!v["transitions"].isArray()) return fail(std::format("{}: transitions is a list", where));
            for (std::size_t t = 0; t < v["transitions"].array.size(); ++t) {
                Result<TransitionDef> transition = parseTransition(graph, layer, v["transitions"].array[t], where, t);
                if (!transition) return Error{transition.error()};
                layer.transitions.push_back(std::move(*transition));
            }
        }
        graph.layers.push_back(std::move(layer));
    }
    return graph;
}

} // namespace quench::animation
