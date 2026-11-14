#pragma once

// A game's interface, drawn (docs/ui.md): text in the game's own fonts, shapes, pictures, and the comic pieces a HUD
// is put together from (a bar, a speech bubble, a sticker's border, halftone dots), each of which can be turned and
// scaled about a point. The widgets that take input are in Ui.h.
//
// It draws through Dear ImGui's draw lists, which the app already renders on every backend, so a canvas needs no
// pipeline, shader or texture of its own; ImGui's windows are not used. Positions and sizes are window points
// (origin top left). Colours are what a designer writes: sRGB with straight alpha, 0..1 (ui::rgb for hex).

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ImDrawList;
struct ImFont;

namespace quench::app {
class App;
}

namespace quench::ui {

using Color = glm::vec4; // sRGB, straight alpha, 0..1

// 0xRRGGBB and an opacity.
constexpr Color rgb(std::uint32_t hex, float alpha = 1.0f) {
    return {float((hex >> 16) & 0xff) / 255.0f, float((hex >> 8) & 0xff) / 255.0f, float(hex & 0xff) / 255.0f, alpha};
}
// The sRGB colour of a linear one (a light's or a material's colour shown in the interface).
Color fromLinear(const glm::vec3& linear, float alpha = 1.0f);

struct Rect {
    glm::vec2 min{0.0f}, max{0.0f};

    static Rect at(glm::vec2 topLeft, glm::vec2 size) { return {topLeft, topLeft + size}; }
    static Rect around(glm::vec2 center, glm::vec2 size) { return {center - size * 0.5f, center + size * 0.5f}; }
    glm::vec2 size() const { return max - min; }
    glm::vec2 center() const { return (min + max) * 0.5f; }
    bool contains(glm::vec2 p) const { return p.x >= min.x && p.y >= min.y && p.x < max.x && p.y < max.y; }
    Rect grown(float by) const { return {min - glm::vec2(by), max + glm::vec2(by)}; }
};

// A font of the game's: files of its assets, the first the base, the others merged into it for the letters it lacks
// (a Cyrillic subset beside a Latin one). Added to ImGui's atlas the first time it is drawn with.
using FontId = std::uint32_t;
FontId addFont(std::string name, std::initializer_list<std::string_view> assetPaths);
// The font of an id; ImGui's default vector font where its files could not be read (logged once).
ImFont* font(FontId id);

struct TextStyle {
    FontId font = 0;
    float size = 16.0f; // window points
    Color color{1.0f};
    // An ink line around the letters (window points; 0: none) and a shadow under them (its offset; zero: none).
    float outline = 0.0f;
    Color outlineColor{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 shadow{0.0f};
    Color shadowColor{0.0f, 0.0f, 0.0f, 0.75f};
    float lineHeight = 1.25f; // of the size, between wrapped lines
};

class Canvas {
public:
    // Onto a draw list of ImGui's. `unit`: window points per design point, for sizes that follow the window.
    Canvas(ImDrawList* list, glm::vec2 window, Rect safe, float unit);
    // Over everything else of the frame (ImGui's foreground list), the window's size and safe area, a unit of
    // windowHeight / designHeight (not below minUnit). Call it in Game::ui.
    static Canvas screen(const app::App& app, float designHeight = 720.0f, float minUnit = 0.6f);

    ImDrawList* list() const { return m_list; }
    glm::vec2 window() const { return m_window; }
    Rect safe() const { return m_safe; } // the window without notches and rounded corners
    float unit() const { return m_unit; }
    float u(float designPoints) const { return designPoints * m_unit; }

    // ---- shapes
    void rect(const Rect& r, const Color& color, float radius = 0.0f);
    void frame(const Rect& r, const Color& color, float thickness, float radius = 0.0f);
    void circle(glm::vec2 center, float radius, const Color& color);
    void ring(glm::vec2 center, float radius, const Color& color, float thickness);
    void line(glm::vec2 a, glm::vec2 b, const Color& color, float thickness);
    // A filled outline, convex or not (no holes, no self-crossing); closed by itself.
    void polygon(std::span<const glm::vec2> points, const Color& color);
    void polyline(std::span<const glm::vec2> points, const Color& color, float thickness, bool closed);
    // A picture: an engine texture's id (app::imguiTexture) over `r`, tinted.
    void image(std::uint64_t texture, const Rect& r, const Color& tint = Color(1.0f), glm::vec2 uvMin = {0.0f, 0.0f},
               glm::vec2 uvMax = {1.0f, 1.0f});

    // ---- text
    // The size of a text (wrapped to `wrap` window points when above 0).
    glm::vec2 measure(const TextStyle& style, std::string_view text, float wrap = 0.0f) const;
    // Draws it with the point `align` of its box at `at` ((0, 0): its top left, (0.5, 0.5): its middle). Returns
    // its size.
    glm::vec2 text(glm::vec2 at, const TextStyle& style, std::string_view text, glm::vec2 align = {0.0f, 0.0f},
                   float wrap = 0.0f);

    // ---- turning and scaling
    // What is drawn while the returned object lives is turned by `angle` (radians, clockwise on the screen) and
    // scaled by `scale` about `pivot`, and its opacity multiplied by `alpha`. Scopes nest.
    class Scope {
    public:
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        friend class Canvas;
        Scope(Canvas& canvas, glm::vec2 pivot, float angle, glm::vec2 scale, float alpha);
        Canvas& m_canvas;
        glm::vec2 m_pivot;
        float m_angle;
        glm::vec2 m_scale;
        float m_alpha;
        int m_firstVertex;
    };
    [[nodiscard]] Scope transform(glm::vec2 pivot, float angle, float scale = 1.0f, float alpha = 1.0f);
    [[nodiscard]] Scope transform(glm::vec2 pivot, float angle, glm::vec2 scale, float alpha = 1.0f);

    // ---- the pieces of a HUD
    // A bar: a well and `fill` (0..1) of it from the left.
    void bar(const Rect& r, float fill, const Color& color, const Color& well = rgb(0x000000, 0.65f),
             float radius = 3.0f);
    // A speech bubble: a rounded body with an ink line and a tail from its edge to `tail` (a point outside it).
    void bubble(const Rect& body, glm::vec2 tail, const Color& fill, const Color& ink, float radius, float inkWidth);
    // Halftone dots over `r`: a grid of `cell` window points whose dots grow from `from` to `to` (0..1 of the cell)
    // along x.
    void halftone(const Rect& r, const Color& color, float cell, float from, float to);
    // A sticker's backing: `points` grown by `border` in white (or `paper`), with a soft shadow under it, as if the
    // shape were cut out and stuck on. Draw the shape itself after it.
    void sticker(std::span<const glm::vec2> points, float border, const Color& paper = Color(1.0f),
                 const Color& shadow = rgb(0x000000, 0.45f));

private:
    ImDrawList* m_list;
    glm::vec2 m_window;
    Rect m_safe;
    float m_unit;
};

// Easing for things that pop in: 0 at t = 0, past 1 on the way (an overshoot of about 10 %), 1 at t = 1.
float easeOutBack(float t);
float easeOutCubic(float t);

} // namespace quench::ui
