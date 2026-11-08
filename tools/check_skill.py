#!/usr/bin/env python3
"""Checks that the agent skill .agents/skills/quench-game still names what the engine has.

    python3 tools/check_skill.py [SKILL_DIR]

A light check of the skill's Markdown files against this checkout, run by `tools/check.sh step new-game`:
  - engine paths in inline code (docs/, include/quench/, cmake/, tools/, templates/, examples/), in
    ${quench_SOURCE_DIR}/... and in #include <quench/...> exist, and so do the files the skill links;
  - command line flags (--name) are flags of the app (src/app/Options.cpp), of tools/new_game.py, of
    tools/tracy_capture.py, of the image compare tool, or of the external tools the skill runs (EXTERNAL_FLAGS);
  - the first words of ```scenario blocks and the lowercase names in the "Scenarios" bullet are known to the scenario
    runner (src/app/Scenario.cpp, src/app/AppScenario.cpp);
  - quench_* CMake functions and targets are defined in CMakeLists.txt or cmake/;
  - engine C++ names (after quench namespaces in code blocks; in inline code, camelCase or PascalCase names alone
    and names after ::, . or ->) are words in include/quench/ or in the shader library (shaders/quench/).
It cannot tell whether the text is still right: a change of behaviour needs the skill read and updated by hand.
Standard library only. Exit code 0: every name found; 1: a list of what is missing.
"""

import re
import sys
from pathlib import Path

ENGINE_DIR = Path(__file__).resolve().parents[1]
DEFAULT_SKILL = ENGINE_DIR / ".agents" / "skills" / "quench-game"
ENGINE_PREFIXES = ("docs/", "include/quench/", "cmake/", "tools/", "templates/", "examples/")
EXTERNAL_FLAGS = {"--preset", "--build", "--console", "--terminate-running-process"}
# Namespaces and prefixes whose names are not the engine's.
FOREIGN = ("std::", "glm::", "JPH::", "ImGui", "SDL_")
QUENCH_NAMESPACES = (r"(?:quench|render|app|audio|assets|animation|navigation|weather|fs|log|gpu|PhysicsLayers|"
                     r"JobPriority)")


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def split_markdown(text: str):
    """Returns the inline code spans outside fences and the fenced blocks as (info, body)."""
    blocks, prose, fence, info, body = [], [], None, "", []
    for line in text.splitlines():
        stripped = line.strip()
        if fence is None and stripped.startswith("```"):
            fence, info, body = "```", stripped[3:].strip(), []
        elif fence is not None and stripped.startswith("```"):
            blocks.append((info, "\n".join(body)))
            fence = None
        elif fence is not None:
            body.append(line)
        else:
            prose.append(line)
    prose_text = "\n".join(prose)
    return re.findall(r"`([^`\n]+)`", prose_text), blocks, prose_text


def main() -> int:
    skill = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else DEFAULT_SKILL
    files = sorted(skill.glob("*.md"))
    if not (skill / "SKILL.md").is_file():
        print(f"check_skill: no SKILL.md in {skill}")
        return 1

    app_flags = read(ENGINE_DIR / "src/app/Options.cpp") + read(ENGINE_DIR / "tools/new_game.py") + read(
        ENGINE_DIR / "tools/tracy_capture.py") + read(ENGINE_DIR / "tools/imagecompare/ImageCompare.cpp")
    runner = read(ENGINE_DIR / "src/app/Scenario.cpp") + read(ENGINE_DIR / "src/app/AppScenario.cpp")
    commands = set(re.findall(r'op == "([a-z_]+)"', runner))
    cmake_text = read(ENGINE_DIR / "CMakeLists.txt") + "".join(read(p) for p in (ENGINE_DIR / "cmake").glob("*.cmake"))
    cmake_names = set(re.findall(r"(?:function|add_library|add_executable)\((quench_[a-z_]+)", cmake_text))
    header_words = set()
    headers = list((ENGINE_DIR / "include/quench").rglob("*.h")) + list((ENGINE_DIR / "shaders/quench").rglob("*.h*"))
    for header in headers:
        header_words.update(re.findall(r"\b[A-Za-z_]\w*\b", read(header)))

    missing = []

    def need(ok: bool, what: str, where: Path) -> None:
        if not ok:
            missing.append(f"{where.name}: {what}")

    for path in files:
        spans, blocks, prose = split_markdown(read(path))
        code = "\n".join(body for _, body in blocks)

        for target in re.findall(r"\]\(([^)#\s]+)", prose):
            if "://" not in target:
                need((path.parent / target).exists(), f"linked file {target}", path)
        engine_paths = [s for s in spans if s.startswith(ENGINE_PREFIXES)]
        engine_paths += re.findall(r"\$\{quench_SOURCE_DIR\}/([^\s)\"]+)", code)
        for p in engine_paths:
            p = p.rstrip(".,;:")
            if not re.search(r"[<>*]", p):
                need((ENGINE_DIR / p).exists(), f"engine path {p}", path)
        for header in re.findall(r"#include <(quench/[^>]+)>", code):
            need((ENGINE_DIR / "include" / header).exists() or (ENGINE_DIR / "shaders" / header).exists(),
                 f"header <{header}>", path)

        for flag in set(re.findall(r"(?<![\w-])--[a-z][a-z0-9-]*", " ".join(spans) + "\n" + code)):
            known = flag in EXTERNAL_FLAGS or re.search(re.escape(flag) + r"(?![a-z0-9-])", app_flags)
            need(bool(known), f"flag {flag}", path)

        for info, body in blocks:
            if info == "scenario":
                for line in body.splitlines():
                    words = line.split("#", 1)[0].split()
                    if words:
                        need(words[0] in commands, f"scenario command '{words[0]}'", path)
        bullet = re.search(r"^- \*\*Scenarios\*\*.*?(?=^- \*\*|\Z)", prose, re.S | re.M)
        if bullet:
            for name in re.findall(r"`([a-z_]+:?)[A-Z]*`", bullet.group(0)):
                need(f'"{name}' in runner, f"scenario command or value '{name}'", path)

        for name in set(re.findall(r"\bquench_[a-z_]+\b", " ".join(spans) + "\n" + code)):
            need(name in cmake_names, f"CMake function or target {name}", path)

        cpp_names = re.findall(QUENCH_NAMESPACES + r"::(\w+)", code)
        for span in spans:
            if re.fullmatch(r"[a-z]+[A-Z]\w*|[A-Z][a-z]+[A-Z]\w*", span):
                cpp_names.append(span)
            elif "::" in span or re.search(r"\w\(", span):
                if not any(f in span for f in FOREIGN) and "/" not in span:
                    cpp_names += re.findall(r"(?:::|\.|->)([A-Za-z_]\w*)", span)
                    cpp_names += re.findall(r"^([A-Za-z_]\w*)", span)
        for name in set(cpp_names) - {"quench"}:
            # m_ members are the game's; snake_case calls are CMake's (quench_* is checked above).
            if not name.startswith("m_") and not re.fullmatch(r"[a-z0-9]+(_[a-z0-9]+)+", name):
                need(name in header_words, f"engine name {name}", path)

    if missing:
        print("check_skill: the skill names what this checkout does not have:")
        for line in sorted(set(missing)):
            print(f"  {line}")
        return 1
    print(f"check_skill: {skill.relative_to(ENGINE_DIR) if skill.is_relative_to(ENGINE_DIR) else skill}: "
          f"{len(files)} files, every path and name found")
    return 0


if __name__ == "__main__":
    sys.exit(main())
