#include "Test.h"

#include <quench/core/Json.h>

#include <string>

using namespace quench;

QUENCH_TEST(json_round_trip) {
    const std::string text =
        R"({"name": "rock", "mass": 12.5, "tags": ["a", "b"], "flags": [true, false], "nested": {"n": null}})";
    const Result<json::Value> doc = json::parse(text);
    CHECK(doc.ok(), "parse: %s", doc.error().c_str());
    if (!doc) return;
    CHECK((*doc)["name"].stringOr("") == "rock", "name");
    CHECK((*doc)["mass"].numberOr(0.0) == 12.5, "mass");
    CHECK((*doc)["tags"].array.size() == 2, "tags");
    CHECK((*doc)["nested"]["n"].isNull() && (*doc)["missing"]["x"].isNull(), "null lookups");
    const std::string written = json::write(*doc);
    const Result<json::Value> again = json::parse(written);
    CHECK(again.ok() && json::write(*again) == written, "second write differs:\n%s", written.c_str());
}

QUENCH_TEST(json_numbers) {
    json::Value v = json::Value::makeArray();
    v.array.push_back(json::Value::makeNumber(1639580.0));
    v.array.push_back(json::Value::makeNumber(0.1));
    v.array.push_back(json::Value::makeNumber(-3.0));
    const std::string out = json::write(v);
    CHECK(out == "[1639580, 0.1, -3]\n", "'%s'", out.c_str());
}

QUENCH_TEST(json_escapes) {
    const Result<json::Value> doc = json::parse(R"(["a\"b\\c\n", "\u00e9\u4e2d"])");
    CHECK(doc.ok(), "parse: %s", doc.error().c_str());
    if (!doc) return;
    CHECK(doc->array[0].string == "a\"b\\c\n", "escapes");
    CHECK(doc->array[1].string == "\xc3\xa9\xe4\xb8\xad", "\\u to UTF-8");
    const std::string out = json::write(*doc);
    CHECK(out.find(R"("a\"b\\c\n")") != std::string::npos, "written '%s'", out.c_str());
}

QUENCH_TEST(json_errors) {
    const Result<json::Value> missing = json::parse(R"({"a": })");
    CHECK(!missing && missing.error().find("offset 6") != std::string::npos, "error '%s'", missing.error().c_str());
    CHECK(!json::parse("[1, 2] x"), "trailing characters accepted");
    CHECK(!json::parse(R"("open)"), "unterminated string accepted");
    CHECK(!json::parse(""), "empty text accepted");
    const Result<json::Value> deep = json::parse(std::string(200, '[') + std::string(200, ']'));
    CHECK(!deep && deep.error().find("nesting too deep") != std::string::npos, "error '%s'", deep.error().c_str());
}

QUENCH_TEST(json_merge) {
    Result<json::Value> base = json::parse(R"({"a": 1, "o": {"x": 1, "y": 2}, "l": [1, 2]})");
    const Result<json::Value> overlay = json::parse(R"({"b": 2, "o": {"y": 3}, "l": [9]})");
    CHECK(base && overlay, "parse");
    if (!base || !overlay) return;
    json::merge(*base, *overlay);
    const json::Value& v = *base;
    CHECK(v["a"].numberOr(0) == 1 && v["b"].numberOr(0) == 2, "top-level members");
    CHECK(v["o"]["x"].numberOr(0) == 1 && v["o"]["y"].numberOr(0) == 3, "nested object merged");
    CHECK(v["l"].array.size() == 1, "array replaced, not merged");
}
