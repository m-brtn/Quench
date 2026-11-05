#!/usr/bin/env python3
"""Creates a new game from templates/game.

    python3 tools/new_game.py "My Game" ~/Projects/my-game [--id my_game] [--bundle-id com.example.my-game]
                              [--organization "My Studio"] [--engine-commit SHA] [--no-check]

The template's names are replaced (display name, target and executable id, C++ class, bundle id, organization) and
the engine is pinned to a commit of this checkout (HEAD by default). Then, unless --no-check is given, the game is
configured and built in Debug against this engine checkout (FETCHCONTENT_SOURCE_DIR_QUENCH) and started once with
--quit-after 1: the check is only that the game builds, starts and quits cleanly. Its own gates (ctest with the smoke
scenario, a capture looked at) come after that, as described in the new game's AGENTS.md. Standard library only.
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

ENGINE_DIR = Path(__file__).resolve().parents[1]
TEMPLATE_DIR = ENGINE_DIR / "templates" / "game"
BINARY_SUFFIXES = {".png"}


def derive_id(name: str) -> str:
    game_id = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    if game_id and game_id[0].isdigit():
        game_id = "game_" + game_id
    return game_id


def class_name(game_id: str) -> str:
    return "".join(part.capitalize() for part in game_id.split("_") if part)


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(ENGINE_DIR), *args], check=True, capture_output=True,
                          text=True).stdout.strip()


def engine_commit(requested: Optional[str]) -> str:
    commit = git("rev-parse", "--verify", (requested or "HEAD") + "^{commit}")
    if not requested and git("status", "--porcelain"):
        print("note: the engine checkout has uncommitted changes; the pin is HEAD, the check builds the working tree")
    if not git("branch", "-r", "--contains", commit):
        print(f"note: engine commit {commit[:12]} is not on a remote branch yet: the game builds only with "
              "-DFETCHCONTENT_SOURCE_DIR_QUENCH until it is pushed")
    return commit


def create(dest: Path, replacements: list) -> None:
    for source in sorted(TEMPLATE_DIR.rglob("*")):
        target = dest / source.relative_to(TEMPLATE_DIR)
        if source.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.suffix in BINARY_SUFFIXES:
            shutil.copyfile(source, target)
            continue
        text = source.read_text(encoding="utf-8")
        for old, new in replacements:
            text = text.replace(old, new)
        target.write_text(text, encoding="utf-8")


def check(dest: Path, game_id: str) -> None:
    def run(command: list) -> None:
        print("+", " ".join(command), flush=True)
        subprocess.run(command, cwd=dest, check=True)

    run(["cmake", "--preset", "debug", f"-DFETCHCONTENT_SOURCE_DIR_QUENCH={ENGINE_DIR}"])
    run(["cmake", "--build", "--preset", "debug"])
    run([str(dest / "build" / "debug" / game_id), "--quit-after", "1"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("name", help="display name, for example \"My Game\"")
    parser.add_argument("dest", type=Path, help="directory of the new game (must not exist or be empty)")
    parser.add_argument("--id", help="CMake target and executable name (default: from the name, my_game)")
    parser.add_argument("--bundle-id", help="iOS bundle id (default: com.example.<id with dashes>)")
    parser.add_argument("--organization", default="",
                        help="organization of the user directory (SDL_GetPrefPath); default: none")
    parser.add_argument("--engine-commit", help="engine commit to pin (default: HEAD of this checkout)")
    parser.add_argument("--no-check", action="store_true", help="do not build and start the new game")
    args = parser.parse_args()

    game_id = args.id or derive_id(args.name)
    if not re.fullmatch(r"[a-z][a-z0-9_]*", game_id):
        parser.error(f"id '{game_id}': lowercase letters, digits and underscores, starting with a letter")
    if game_id.startswith("quench"):
        parser.error(f"id '{game_id}': the quench prefix belongs to the engine's targets")
    if '"' in args.name or "\\" in args.name or '"' in args.organization or "\\" in args.organization:
        parser.error("the name and the organization cannot contain quotes or backslashes")
    bundle_id = args.bundle_id or "com.example." + game_id.replace("_", "-")
    dest = args.dest.expanduser().resolve()
    if dest.exists() and any(dest.iterdir()):
        parser.error(f"{dest} exists and is not empty")

    commit = engine_commit(args.engine_commit)
    replacements = [
        ("QUENCH_ENGINE_COMMIT", commit),
        ("org.example.template-game", bundle_id),
        ('desc.organization = "Example";', f'desc.organization = "{args.organization}";'),
        ("Template Game", args.name),
        ("TemplateGame", class_name(game_id)),
        ("TEMPLATE_GAME", game_id.upper()),
        ("template_game", game_id),
    ]
    create(dest, replacements)
    print(f"created {dest}: {args.name} ({game_id}, {bundle_id}), engine {commit[:12]}")

    if not args.no_check:
        try:
            check(dest, game_id)
        except (subprocess.CalledProcessError, FileNotFoundError) as error:
            print(f"check failed: {error}", file=sys.stderr)
            return 1
        print(f"check passed: {game_id} builds, starts and quits")
    print(f"next: cd {dest} && git init, then follow AGENTS.md (gates: debug and release, ctest, a capture)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
