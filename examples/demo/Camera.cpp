// The orbit camera and the controls every 3D section shares: mouse (left drag orbits, right or middle drag pans, the
// wheel zooms), touch (one finger orbits, two pan and pinch), gamepad (right stick orbits, left stick pans, the
// triggers zoom) and keyboard (arrows orbit, + and - zoom, while no window has the keyboard focus).

#include "DemoGame.h"

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace demo {

using namespace quench;
using app::Binding;
using app::GamepadAxis;

glm::vec3 OrbitCamera::eye() const {
    const float c = std::cos(pitch);
    return target + distance * glm::vec3(std::sin(yaw) * c, std::sin(pitch), std::cos(yaw) * c);
}

glm::mat4 OrbitCamera::view() const { return glm::lookAt(eye(), target, glm::vec3(0.0f, 1.0f, 0.0f)); }

float OrbitCamera::fovFor(float aspect) const {
    return std::max(fovY, 2.0f * std::atan(std::tan(glm::radians(32.0f)) / aspect));
}

void OrbitCamera::ray(glm::vec2 point, glm::vec2 window, glm::vec3& origin, glm::vec3& direction) const {
    const float aspect = window.x / std::max(window.y, 1.0f);
    const float t = std::tan(fovFor(aspect) * 0.5f);
    const glm::vec2 ndc(point.x / window.x * 2.0f - 1.0f, 1.0f - point.y / window.y * 2.0f);
    const glm::mat4 invView = glm::inverse(view());
    origin = glm::vec3(invView[3]);
    direction = glm::normalize(glm::vec3(invView * glm::vec4(ndc.x * t * aspect, ndc.y * t, -1.0f, 0.0f)));
}

std::optional<glm::vec2> OrbitCamera::project(const glm::vec3& world, glm::vec2 window) const {
    const float aspect = window.x / std::max(window.y, 1.0f);
    const float t = std::tan(fovFor(aspect) * 0.5f);
    const glm::vec4 v = view() * glm::vec4(world, 1.0f);
    if (v.z > -1e-4f) return std::nullopt;
    const glm::vec2 ndc(v.x / (-v.z * t * aspect), v.y / (-v.z * t));
    return glm::vec2((ndc.x + 1.0f) * 0.5f * window.x, (1.0f - ndc.y) * 0.5f * window.y);
}

void OrbitCamera::pan(glm::vec2 points, float windowHeight) {
    // A drag across the window's height moves the target by about the height of the view at the target.
    const float scale = 2.0f * distance * std::tan(fovY * 0.5f) / std::max(windowHeight, 1.0f);
    const glm::vec3 right(std::cos(yaw), 0.0f, -std::sin(yaw));
    const glm::vec3 forward(-std::sin(yaw), 0.0f, -std::cos(yaw));
    target += (-right * points.x + forward * points.y) * scale;
    const float reach = maxDistance;
    target.x = std::clamp(target.x, -reach, reach);
    target.z = std::clamp(target.z, -reach, reach);
}

void CameraControls::init(app::Input& input) {
    m_orbitDrag = action(input, "orbit_drag", {Binding::mouse(app::MouseButton::Left)});
    m_panDrag =
        action(input, "pan_drag", {Binding::mouse(app::MouseButton::Right), Binding::mouse(app::MouseButton::Middle)});
    m_orbitLeft =
        action(input, "orbit_left", {Binding::gamepadAxis(GamepadAxis::RightX, false), *Binding::key("Left")});
    m_orbitRight =
        action(input, "orbit_right", {Binding::gamepadAxis(GamepadAxis::RightX, true), *Binding::key("Right")});
    m_orbitUp = action(input, "orbit_up", {Binding::gamepadAxis(GamepadAxis::RightY, false), *Binding::key("Up")});
    m_orbitDown = action(input, "orbit_down", {Binding::gamepadAxis(GamepadAxis::RightY, true), *Binding::key("Down")});
    m_panLeft = action(input, "pan_left", {Binding::gamepadAxis(GamepadAxis::LeftX, false)});
    m_panRight = action(input, "pan_right", {Binding::gamepadAxis(GamepadAxis::LeftX, true)});
    m_panForward = action(input, "pan_forward", {Binding::gamepadAxis(GamepadAxis::LeftY, false)});
    m_panBack = action(input, "pan_back", {Binding::gamepadAxis(GamepadAxis::LeftY, true)});
    m_zoomIn =
        action(input, "zoom_in",
               {Binding::gamepadAxis(GamepadAxis::RightTrigger, true), *Binding::key("="), *Binding::key("Keypad +")});
    m_zoomOut =
        action(input, "zoom_out",
               {Binding::gamepadAxis(GamepadAxis::LeftTrigger, true), *Binding::key("-"), *Binding::key("Keypad -")});
}

void CameraControls::reset() {
    m_pressed = false;
    m_dragged = false;
    m_touchMoved = true;
}

void CameraControls::update(app::App& app, Section& section, float dt, bool sceneFocus,
                            const std::function<void(glm::vec2)>& click) {
    const app::Input& input = app.input();
    const bool uiCapturesMouse = input.uiCapturesMouse();
    OrbitCamera& camera = section.camera;
    const float height = float(app.windowHeight());
    glm::vec2 turn(0.0f), pan(0.0f);
    float zoom = 1.0f;
    m_touched = false;

    // Mouse: drag with the left button to orbit, the right or middle one to pan, the wheel; a press and release
    // without a drag is a click.
    if (!input.mouseFromTouch()) {
        const bool orbitPress = input.pressed(m_orbitDrag), panPress = input.pressed(m_panDrag);
        if (orbitPress || panPress) {
            m_pressed = true;
            m_panning = panPress && !orbitPress;
            m_pressPoint = input.mousePosition();
            m_dragged = false;
            m_touched = true;
        }
        const bool down = m_panning ? input.down(m_panDrag) : input.down(m_orbitDrag);
        if (down && m_pressed) {
            if (glm::length(input.mousePosition() - m_pressPoint) > 4.0f) m_dragged = true;
            if (m_dragged) (m_panning ? pan : turn) += input.mouseDelta() * (m_panning ? 1.0f : 0.008f);
        }
        if ((input.released(m_orbitDrag) || input.released(m_panDrag)) && m_pressed && !down) {
            if (!m_dragged) click(m_pressPoint);
            m_pressed = false;
        }
        if (!uiCapturesMouse && input.wheel().y != 0.0f) {
            zoom *= std::pow(0.9f, input.wheel().y);
            m_touched = true;
        }
    }

    // Fingers (not on a window): one drags the orbit, two pan by their midpoint and pinch; a short touch without a
    // drag is a tap.
    const std::span<const app::Finger> fingers = input.fingers();
    for (const app::Finger& f : fingers) {
        if (f.pressed) {
            m_touchStart = f.position;
            m_touchMoved = uiCapturesMouse;
            m_touchId = f.id;
            m_touched = true;
        }
        if (f.id == m_touchId && glm::length(f.position - m_touchStart) > 8.0f) m_touchMoved = true;
        if (f.released && f.id == m_touchId && !m_touchMoved && fingers.size() == 1) click(f.position);
    }
    if (!uiCapturesMouse) {
        if (section.freeCamera() && fingers.size() == 1 && !fingers[0].pressed) turn += fingers[0].delta * 0.008f;
        if (fingers.size() == 2) {
            const glm::vec2 now = fingers[0].position - fingers[1].position;
            const glm::vec2 before =
                (fingers[0].position - fingers[0].delta) - (fingers[1].position - fingers[1].delta);
            if (glm::length(now) > 1.0f && glm::length(before) > 1.0f) zoom *= glm::length(before) / glm::length(now);
            if (section.freeCamera() && !fingers[0].pressed && !fingers[1].pressed)
                pan += (fingers[0].delta + fingers[1].delta) * 0.5f;
        }
    }

    // Gamepad and keyboard while no window has the focus: the right stick and the arrows turn, the left stick pans,
    // the triggers and + - zoom.
    if (sceneFocus) {
        if (section.freeCamera()) {
            turn.x += input.axis(m_orbitLeft, m_orbitRight) * 2.0f * dt;
            turn.y += input.axis(m_orbitUp, m_orbitDown) * 1.5f * dt;
            pan += glm::vec2(input.axis(m_panLeft, m_panRight), -input.axis(m_panBack, m_panForward)) * height * dt;
        }
        zoom *= std::pow(2.0f, (input.value(m_zoomOut) - input.value(m_zoomIn)) * dt);
    }
    if (turn != glm::vec2(0.0f) || pan != glm::vec2(0.0f) || zoom != 1.0f) m_touched = true;

    camera.yaw -= turn.x;
    camera.pitch = std::clamp(camera.pitch + turn.y, glm::radians(-5.0f), glm::radians(85.0f));
    camera.distance = std::clamp(camera.distance * zoom, camera.minDistance, camera.maxDistance);
    if (pan != glm::vec2(0.0f)) camera.pan(pan, height);
}

} // namespace demo
