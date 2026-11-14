#include <quench/ui/Canvas.h>

#include <quench/app/App.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Log.h>

#include <glm/geometric.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace quench::ui {

namespace {

ImU32 pack(const Color& c) {
    const auto channel = [](float v) { return ImU32(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return channel(c.r) | (channel(c.g) << 8) | (channel(c.b) << 16) | (channel(c.a) << 24);
}

ImVec2 im(glm::vec2 v) { return {v.x, v.y}; }

// The fonts a game named, and for each ImGui context the font it became there (a test may make another context).
struct FontEntry {
    std::string name;
    std::vector<std::string> paths;
    ImGuiContext* context = nullptr;
    ImFont* font = nullptr;
};
std::vector<FontEntry>& fonts() {
    static std::vector<FontEntry> entries;
    return entries;
}

ImFont* loadFile(const std::string& path, ImFont* into) {
    Result<fs::Bytes> bytes = fs::readAsset(path);
    if (!bytes || bytes->empty()) {
        log::warning("ui", "no font {}: {}", path, bytes ? std::string("an empty file") : bytes.error());
        return nullptr;
    }
    // ImGui's atlas owns the copy.
    void* data = IM_ALLOC(bytes->size());
    std::memcpy(data, bytes->data(), bytes->size());
    ImFontConfig config;
    config.FontDataOwnedByAtlas = true;
    if (into) {
        config.MergeMode = true;
        config.DstFont = into;
    }
    std::snprintf(config.Name, sizeof(config.Name), "%s", path.c_str());
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, int(bytes->size()), 0.0f, &config);
}

// ImGui bakes a font at the window's density only while that font is pushed; a canvas hands its fonts to the draw
// list without pushing them, so they are told the density here (else a screen of two pixels a point gets blurred
// letters).
ImFont* atDensity(ImFont* f) {
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if (f && context && context->FontRasterizerDensity > 0.0f)
        f->CurrentRasterizerDensity = context->FontRasterizerDensity;
    return f;
}

// The corners of a rounded rectangle, clockwise from the top left, `segments` points a corner.
void roundedPath(const Rect& r, float radius, int segments, std::vector<glm::vec2>& out) {
    radius = std::min(radius, std::min(r.size().x, r.size().y) * 0.5f);
    const glm::vec2 centers[4] = {{r.min.x + radius, r.min.y + radius},
                                  {r.max.x - radius, r.min.y + radius},
                                  {r.max.x - radius, r.max.y - radius},
                                  {r.min.x + radius, r.max.y - radius}};
    for (int corner = 0; corner < 4; ++corner)
        for (int i = 0; i <= segments; ++i) {
            const float a = 3.14159265f * (1.0f + 0.5f * (float(corner) + float(i) / float(segments)));
            out.push_back(centers[corner] + glm::vec2(std::cos(a), std::sin(a)) * radius);
        }
}

} // namespace

Color fromLinear(const glm::vec3& linear, float alpha) {
    const auto channel = [](float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    };
    return {channel(linear.r), channel(linear.g), channel(linear.b), alpha};
}

FontId addFont(std::string name, std::initializer_list<std::string_view> assetPaths) {
    std::vector<FontEntry>& all = fonts();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i].name == name) return FontId(i + 1);
    FontEntry entry;
    entry.name = std::move(name);
    for (const std::string_view path : assetPaths) entry.paths.emplace_back(path);
    all.push_back(std::move(entry));
    return FontId(all.size());
}

ImFont* font(FontId id) {
    std::vector<FontEntry>& all = fonts();
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (id == 0 || id > all.size()) return atDensity(ImGui::GetFont());
    FontEntry& entry = all[id - 1];
    if (entry.context != context || !entry.font) {
        entry.context = context;
        entry.font = nullptr;
        for (const std::string& path : entry.paths) {
            ImFont* loaded = loadFile(path, entry.font);
            if (!entry.font) entry.font = loaded;
        }
        if (!entry.font) {
            log::error("ui", "the font '{}' has no file that could be read: ImGui's default font instead", entry.name);
            entry.font = ImGui::GetIO().Fonts->AddFontDefaultVector();
        }
    }
    return atDensity(entry.font);
}

Canvas::Canvas(ImDrawList* list, glm::vec2 window, Rect safe, float unit)
    : m_list(list), m_window(window), m_safe(safe), m_unit(unit) {}

Canvas Canvas::screen(const app::App& app, float designHeight, float minUnit) {
    const glm::vec2 window(float(app.windowWidth()), float(app.windowHeight()));
    const app::WindowRect safe = app.safeArea();
    const Rect area{{float(safe.x), float(safe.y)}, {float(safe.x + safe.width), float(safe.y + safe.height)}};
    return Canvas(ImGui::GetForegroundDrawList(), window, area, std::max(window.y / designHeight, minUnit));
}

void Canvas::rect(const Rect& r, const Color& color, float radius) {
    m_list->AddRectFilled(im(r.min), im(r.max), pack(color), radius);
}

void Canvas::frame(const Rect& r, const Color& color, float thickness, float radius) {
    m_list->AddRect(im(r.min), im(r.max), pack(color), radius, 0, thickness);
}

void Canvas::circle(glm::vec2 center, float radius, const Color& color) {
    m_list->AddCircleFilled(im(center), radius, pack(color), 0);
}

void Canvas::ring(glm::vec2 center, float radius, const Color& color, float thickness) {
    m_list->AddCircle(im(center), radius, pack(color), 0, thickness);
}

void Canvas::line(glm::vec2 a, glm::vec2 b, const Color& color, float thickness) {
    m_list->AddLine(im(a), im(b), pack(color), thickness);
}

void Canvas::polygon(std::span<const glm::vec2> points, const Color& color) {
    if (points.size() < 3) return;
    static_assert(sizeof(glm::vec2) == sizeof(ImVec2));
    m_list->AddConcavePolyFilled(reinterpret_cast<const ImVec2*>(points.data()), int(points.size()), pack(color));
}

void Canvas::polyline(std::span<const glm::vec2> points, const Color& color, float thickness, bool closed) {
    if (points.size() < 2) return;
    m_list->AddPolyline(reinterpret_cast<const ImVec2*>(points.data()), int(points.size()), pack(color),
                        closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
}

void Canvas::image(std::uint64_t texture, const Rect& r, const Color& tint, glm::vec2 uvMin, glm::vec2 uvMax) {
    m_list->AddImage(ImTextureRef(ImTextureID(texture)), im(r.min), im(r.max), im(uvMin), im(uvMax), pack(tint));
}

glm::vec2 Canvas::measure(const TextStyle& style, std::string_view text, float wrap) const {
    if (text.empty()) return glm::vec2(0.0f);
    ImFont* f = font(style.font);
    const ImVec2 size = f->CalcTextSizeA(style.size, FLT_MAX, wrap, text.data(), text.data() + text.size());
    return {size.x, size.y};
}

glm::vec2 Canvas::text(glm::vec2 at, const TextStyle& style, std::string_view text, glm::vec2 align, float wrap) {
    if (text.empty()) return glm::vec2(0.0f);
    ImFont* f = font(style.font);
    const glm::vec2 size = measure(style, text, wrap);
    // Whole pixels: letters between two pixels blur.
    const glm::vec2 origin = glm::round(at - size * align);
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto draw = [&](glm::vec2 offset, const Color& color) {
        m_list->AddText(f, style.size, im(origin + offset), pack(color), begin, end, wrap);
    };
    if (style.shadow != glm::vec2(0.0f)) draw(style.shadow, style.shadowColor);
    if (style.outline > 0.0f) {
        // The ink line: the text again at eight points around it.
        for (int i = 0; i < 8; ++i) {
            const float a = 6.2831853f * float(i) / 8.0f;
            draw(glm::vec2(std::cos(a), std::sin(a)) * style.outline, style.outlineColor);
        }
    }
    draw(glm::vec2(0.0f), style.color);
    return size;
}

Canvas::Scope::Scope(Canvas& canvas, glm::vec2 pivot, float angle, glm::vec2 scale, float alpha)
    : m_canvas(canvas), m_pivot(pivot), m_angle(angle), m_scale(scale), m_alpha(alpha),
      m_firstVertex(canvas.m_list->VtxBuffer.Size) {}

Canvas::Scope::~Scope() {
    // Everything drawn since the scope began: its vertices are moved in place.
    ImVector<ImDrawVert>& vertices = m_canvas.m_list->VtxBuffer;
    const float c = std::cos(m_angle), s = std::sin(m_angle);
    for (int i = m_firstVertex; i < vertices.Size; ++i) {
        ImDrawVert& v = vertices[i];
        const glm::vec2 p = (glm::vec2(v.pos.x, v.pos.y) - m_pivot) * m_scale;
        v.pos = ImVec2(m_pivot.x + p.x * c - p.y * s, m_pivot.y + p.x * s + p.y * c);
        if (m_alpha < 1.0f) {
            const ImU32 alpha = ImU32(float(v.col >> IM_COL32_A_SHIFT) * std::clamp(m_alpha, 0.0f, 1.0f));
            v.col = (v.col & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
        }
    }
}

Canvas::Scope Canvas::transform(glm::vec2 pivot, float angle, float scale, float alpha) {
    return Scope(*this, pivot, angle, glm::vec2(scale), alpha);
}

Canvas::Scope Canvas::transform(glm::vec2 pivot, float angle, glm::vec2 scale, float alpha) {
    return Scope(*this, pivot, angle, scale, alpha);
}

void Canvas::bar(const Rect& r, float fill, const Color& color, const Color& well, float radius) {
    rect(r.grown(2.0f), well, radius + 1.0f);
    fill = std::clamp(fill, 0.0f, 1.0f);
    if (fill <= 0.0f) return;
    rect({r.min, {r.min.x + r.size().x * fill, r.max.y}}, color, radius);
}

void Canvas::bubble(const Rect& body, glm::vec2 tail, const Color& fill, const Color& ink, float radius,
                    float inkWidth) {
    // The outline of the body with the tail let into the side that faces its point.
    std::vector<glm::vec2> outline;
    roundedPath(body, radius, 6, outline);
    const glm::vec2 center = body.center();
    const glm::vec2 toTail = tail - center;
    const glm::vec2 half = body.size() * 0.5f;
    const float width = std::min(body.size().x, body.size().y) * 0.28f;
    // Where the line from the centre to the tail leaves the body, and the tail's two roots beside it on that side.
    const bool vertical = std::abs(toTail.y) * half.x > std::abs(toTail.x) * half.y;
    glm::vec2 a, b;
    if (vertical) {
        const float y = toTail.y > 0.0f ? body.max.y : body.min.y;
        const float x = std::clamp(center.x + toTail.x * (half.y / std::max(std::abs(toTail.y), 1.0e-3f)),
                                   body.min.x + radius + width * 0.5f, body.max.x - radius - width * 0.5f);
        a = {x - width * 0.5f, y};
        b = {x + width * 0.5f, y};
    } else {
        const float x = toTail.x > 0.0f ? body.max.x : body.min.x;
        const float y = std::clamp(center.y + toTail.y * (half.x / std::max(std::abs(toTail.x), 1.0e-3f)),
                                   body.min.y + radius + width * 0.5f, body.max.y - radius - width * 0.5f);
        a = {x, y - width * 0.5f};
        b = {x, y + width * 0.5f};
    }
    const glm::vec2 tailPoints[3] = {a, tail, b};
    // The ink first (the shapes grown by its width), then the fill over it.
    if (inkWidth > 0.0f && ink.a > 0.0f) {
        rect(body.grown(inkWidth), ink, radius + inkWidth);
        const glm::vec2 out = glm::normalize(tail - (a + b) * 0.5f) * inkWidth * 2.2f;
        const glm::vec2 side = glm::normalize(b - a) * inkWidth * 1.2f;
        const glm::vec2 inkTail[3] = {a - side, tail + out, b + side};
        polygon(inkTail, ink);
    }
    polygon(outline, fill);
    polygon(tailPoints, fill);
}

void Canvas::halftone(const Rect& r, const Color& color, float cell, float from, float to) {
    if (cell <= 1.0f) return;
    const int columns = int(std::ceil(r.size().x / cell)), rows = int(std::ceil(r.size().y / cell));
    m_list->PushClipRect(im(r.min), im(r.max), true);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < columns; ++x) {
            const float t = columns > 1 ? float(x) / float(columns - 1) : 0.0f;
            const float radius = cell * 0.5f * (from + (to - from) * t);
            if (radius < 0.3f) continue;
            // Every other row is shifted by half a cell.
            const glm::vec2 center =
                r.min + glm::vec2((float(x) + (y % 2 ? 1.0f : 0.5f)) * cell, (float(y) + 0.5f) * cell);
            m_list->AddCircleFilled(im(center), radius, pack(color), 8);
        }
    m_list->PopClipRect();
}

void Canvas::sticker(std::span<const glm::vec2> points, float border, const Color& paper, const Color& shadow) {
    if (points.size() < 3) return;
    // The shape grown by the border: a thick closed line around it, and the shape itself.
    std::vector<glm::vec2> moved(points.begin(), points.end());
    if (shadow.a > 0.0f) {
        const glm::vec2 offset(border * 0.35f, border * 0.6f);
        for (glm::vec2& p : moved) p += offset;
        polyline(moved, shadow, border * 2.0f, true);
        polygon(moved, shadow);
    }
    polyline(points, paper, border * 2.0f, true);
    polygon(points, paper);
}

float easeOutBack(float t) {
    t = std::clamp(t, 0.0f, 1.0f) - 1.0f;
    constexpr float kOvershoot = 1.70158f;
    return 1.0f + (kOvershoot + 1.0f) * t * t * t + kOvershoot * t * t;
}

float easeOutCubic(float t) {
    t = 1.0f - std::clamp(t, 0.0f, 1.0f);
    return 1.0f - t * t * t;
}

} // namespace quench::ui
