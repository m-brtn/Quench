# Game UI

`quench_ui` is a game's interface: text in the game's own fonts, shapes and pictures, the comic pieces a HUD is put
together from, and widgets that take the app's input. Asked by the user on 2026-11-14 for a game with a HUD, an
inventory and radio lines shown as stickers with speech bubbles; built the same day. Dear ImGui stays what it was,
the tool for debug panels. All documentation: [index](README.md).

It draws through Dear ImGui's draw lists, which the app already renders on every backend, so the module has no
pipeline, shader or texture of its own and works wherever ImGui does. ImGui's windows and widgets are not used: a
game's interface has its own look, input rules and layout. The approach is the one the Outpost game's HUD kit took
inside the game; the code here is new.

| Header | What |
|---|---|
| `quench/ui/Canvas.h` | drawing: fonts, text, shapes, pictures, turning and scaling, a bar, a speech bubble, halftone dots, a sticker's backing |
| `quench/ui/Ui.h` | input: pressable areas, buttons, a pane, tips, slots with drag and drop, a touch stick and buttons, the focus of a gamepad or the keys, things that appear and go |

## Use

Link `quench_ui`. In `Game::ui`:

```cpp
#include <quench/ui/Ui.h>

ui::FontId m_font = ui::addFont("hud", {"fonts/Manrope-Bold.ttf", "fonts/Manrope-Bold-Cyrillic.ttf"});
ui::Ui m_ui; // a member: it keeps what is held, dragged and focused

void Game::ui(app::App& app) {
    ui::Canvas canvas = ui::Canvas::screen(app); // over the frame; sizes follow the window's height
    m_ui.begin(app, canvas, float(m_frameDt));
    const float u = canvas.unit();
    canvas.bar(ui::Rect::at({24 * u, 24 * u}, {210 * u, 14 * u}), health, ui::rgb(0xff4d5e));
    if (m_ui.button("restart", ui::Rect::at({24 * u, 60 * u}, {140 * u, 34 * u}), text("restart")).pressed) restart();
    m_ui.tip(text("restart.tip"));
    m_ui.end();
    if (!m_ui.pointerOverUi()) { /* the click is the game's */ }
}
```

- **Units.** Positions and sizes are window points, origin top left. `Canvas::unit()` is the window's height over a
  design height (720 by default, not below 0.6), so `24 * u` is 24 points on a 720-point window and grows with it.
  `Canvas::safe()` is the window without notches and rounded corners: lay out from its edges.
- **Colours** are sRGB with straight alpha (`ui::rgb(0x1ad9cc, 0.8f)`); `ui::fromLinear` converts a light's or a
  material's linear colour.
- **Fonts.** `ui::addFont(name, files)` names a font once (anywhere, also before the app runs): the first file is
  the base, the others are merged into it for the letters it lacks, so a Latin file with a Cyrillic subset beside
  it is one font. The files are read from the assets and added to ImGui's atlas the first time the font is drawn;
  any size then comes from the same font (`TextStyle::size`). A missing file is a warning and ImGui's default font.
- **Text.** `Canvas::text(at, style, text, align, wrap)`: `align` is the point of the text's box put at `at`
  (`{0.5, 0.5}`: centred); `TextStyle` has an ink outline and a shadow. Positions are rounded to whole points.
- **Turning.** `auto scope = canvas.transform(pivot, angle, scale, alpha);` turns, scales and fades everything
  drawn while `scope` lives (text and pictures too): it moves the vertices that were added. This is what makes a
  sticker sit at an angle and pop in (`ui::easeOutBack(m_ui.presence(id, shown))`).
- **Comic pieces.** `bubble` (a rounded body with an ink line and a tail to a point), `halftone` (dots that grow
  across a rectangle), `sticker` (a shape's white border and soft shadow, under the shape), `bar`.
- **Pictures.** `Canvas::image(app::imguiTexture(texture), rect)` for an engine texture; vector art is `polygon`
  and `polyline` (filled outlines may be concave, without holes).

## Input

- **A press** is a release over the widget that was pressed (`Press::pressed`), or the confirm button on the
  focused one. `Press::hovered`, `held` and `focused` are for drawing.
- **A click inside one frame counts.** The press and the release are read as the frame's edges
  (`app::Input::mousePressed`, `mouseReleased`), not from the button's level, so a tap on a trackpad whose two
  halves arrive in one frame presses a widget (since 0.20.2; before, such clicks were lost).
- **The pointer** is the mouse; on a touch screen the first finger arrives as the mouse (the app's rule). A tool
  window of ImGui's under the pointer takes the mouse first.
- **`pointerOverUi()`** says the pointer was over a widget or a pane in the last frame: the game then leaves that
  click alone.
- **The focus.** `setNavigation(true)` while a screen of widgets is open: the D-pad or the arrow keys move a focus
  to the nearest widget in that direction, South or Return presses it. It starts from the widget pressed last and
  shows once a key or a button was used; moving the pointer hides it again. Off, the module takes no button.
- **Slots.** `slot(index, rect, filled)` is one cell of a grid. A filled one's content is dragged with the pointer
  (after 6 design points of movement) and dropped on another: that slot's `SlotEvent::droppedFrom` names the
  source, once. With the focus, confirm picks the content up and confirm on another slot puts it down. The game
  owns the contents and draws them: in the slot, and at `dragPoint()` while `dragged()` is its index.
- **Tips.** `tip(text)` after a widget shows while the pointer rests on it or it has the focus; drawn in `end()`
  over everything, inside the safe area.
- **Under the thumbs.** `stick(id, zone, radius)` gives a finger that goes down inside the zone a stick under it
  and returns its deflection; `touchButton` is a round button any finger can hold. Both read every finger, so they
  work together; the mouse works them too.
- **Scenarios.** `Ui::point(id)` is the middle of a widget of the last frame (a slot's id is `slot:<index>`): a
  game answers `Game::scenarioPoint` with it, and a scenario clicks `@<name>`.

## What is built

`include/quench/ui/Canvas.h`, `Ui.h`, `src/ui/Canvas.cpp`, `Ui.cpp` (about 700 lines), the target `quench_ui` on
`quench_app`. Not headless: it needs ImGui's context, which the app makes.

The demo's section 18, `hud` ("Game UI", [Engine demo](demo.md#sections)): three meters, a radio line whose
speaker pops in as a sticker (a face drawn from ellipses and polygons, turned 7 degrees) with a bubble, halftone
dots and a turned tag, an inventory of 12 slots with five kinds of items, two buttons, tips, the touch stick.

## Verified

2026-11-14, on the Mac (M4 Pro), Release, offscreen: the demo's `hud` part in the three batches (SDL GPU's Metal
driver, the native Metal backend, Vulkan through MoltenVK), 30 checks each: a click presses a button; a finger's
drag moves an item to an empty slot, swaps two, and puts one back when let go over no slot; a tip shows over a
filled slot and not over an empty one; `pointerOverUi` follows the pointer; the gamepad's focus picks an item up
and puts it down a slot further; no ImGui id conflicts. Its capture (`demo_hud.png`) is the same on the three
backends (at most 1 level of difference on Vulkan).

Not verified: nobody has used it by hand (a real mouse, a finger, a gamepad); the stick's value is drawn but
checked by no number; no text other than Latin in the demo (the first game draws Cyrillic with the same calls);
a screen of two pixels a point (the font density path), a phone, a browser; no unit tests of its own (it needs
the app's ImGui context: it is tested through the demo).

## Limitations and open

- No layout engine: a game computes its rectangles. No scrolling, text input, sliders, lists or tabs yet.
- No pictures from files by themselves: a game loads a texture and passes its id.
- An ink outline draws the text nine times; long outlined texts cost accordingly.
- Text as meshes (letters with depth, for titles in the scene) is not here; planned with the first game's outro.
- One `Ui` per screen of widgets; ids are hashed strings and must be unique in a frame.
