#!/usr/bin/env python3
"""
tools/amalgamate.py — Keylight C++ SDK single-header amalgamator.

Usage:
    python3 tools/amalgamate.py

Outputs:
    keylight_single.hpp  in the repository root.

Algorithm:
  1. Walk the include graph starting from include/keylight/keylight.hpp.
  2. Inline each local header exactly once (visited set); topological order —
     a header is emitted after every header it includes.
  3. Strip #pragma once and include-guard lines (#ifndef/#define/#endif) from
     inlined content.
  4. Skip #include directives that reference already-visited headers, and skip
     transport/httplib.hpp entirely (it requires OpenSSL).
  5. Hoist all system includes (#include <...>) to the top, de-duplicated and
     sorted, so they appear before any code.
  6. Emit a single #pragma once + generated-file banner at the very top.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Optional

# ── Configuration ────────────────────────────────────────────────────────────

REPO_ROOT   = Path(__file__).resolve().parent.parent
INCLUDE_DIR = REPO_ROOT / "include"
ENTRY_POINT = INCLUDE_DIR / "keylight" / "keylight.hpp"
OUTPUT_FILE = REPO_ROOT / "keylight_single.hpp"

# Headers to exclude unconditionally (relative paths within the include tree).
# httplib.hpp requires OpenSSL and is intentionally opt-in.
EXCLUDE_HEADERS = {
    "keylight/transport/httplib.hpp",
    "transport/httplib.hpp",
}

# ── Regexes ─────────────────────────────────────────────────────────────────

# Local include:  #include "foo.hpp"  or  #include "keylight/foo.hpp"
RE_LOCAL = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
# System include: #include <foo>
RE_SYSTEM = re.compile(r'^\s*#\s*include\s+<([^>]+)>')
# pragma once
RE_PRAGMA_ONCE = re.compile(r'^\s*#\s*pragma\s+once\s*$')
# Include-guard lines (conservative: only #ifndef / #define at top level,
# and the matching #endif at the very end of a file).
# We strip these because the amalgamation has its own single #pragma once.
RE_GUARD_IFNDEF  = re.compile(r'^\s*#\s*ifndef\s+\w+\s*$')
RE_GUARD_DEFINE  = re.compile(r'^\s*#\s*define\s+\w+\s*$')
RE_ENDIF         = re.compile(r'^\s*#\s*endif\s*(?://.*)?$')

# ── State ───────────────────────────────────────────────────────────────────

visited:        set  = set()    # canonical absolute paths already emitted
system_includes: list = []      # ordered list of system include tokens (de-duped)
system_seen:     set  = set()   # tracks which system includes we've collected
body_chunks:    list  = []      # ordered text chunks for the body


def resolve_local(current_file: Path, token: str) -> Optional[Path]:
    """
    Resolve a local include token to an absolute path.

    Resolution order (matches the CMakeLists.txt include directories):
      1. Relative to INCLUDE_DIR   — handles "keylight/foo.hpp" from anywhere
      2. Relative to the same dir as current_file — handles "foo.hpp" used
         inside include/keylight/ files
    """
    # Try from INCLUDE_DIR first (the CMake -I include path)
    candidate = INCLUDE_DIR / token
    if candidate.exists():
        return candidate.resolve()

    # Try relative to the current file's directory
    candidate = (current_file.parent / token).resolve()
    if candidate.exists():
        return candidate

    return None


def is_excluded(token: str) -> bool:
    """Return True if this local include token should be skipped."""
    return token in EXCLUDE_HEADERS or token.replace("\\", "/") in EXCLUDE_HEADERS


def strip_include_guards(lines: list[str]) -> list[str]:
    """
    Remove include-guard boilerplate from a file's lines.

    Strategy: scan for the classic triple:
        #ifndef FOO_HPP
        #define FOO_HPP
        ...
        #endif  // or #endif /* ... */
    at the outermost level (first two lines after any blank/comment prefix,
    and the last non-blank line).  Also unconditionally remove #pragma once.

    This is intentionally conservative: only strip guards that look like
    standard include guards (single-identifier form), not arbitrary #ifdefs
    in the body.
    """
    result = []
    n = len(lines)
    # Detect guard: find first two non-blank lines
    first_code = [i for i, l in enumerate(lines) if l.strip()]
    if len(first_code) >= 2:
        i0, i1 = first_code[0], first_code[1]
        if RE_GUARD_IFNDEF.match(lines[i0]) and RE_GUARD_DEFINE.match(lines[i1]):
            # Find the last non-blank line
            last_code = [i for i, l in enumerate(lines) if l.strip()]
            if last_code and RE_ENDIF.match(lines[last_code[-1]]):
                # Strip them
                skip = {i0, i1, last_code[-1]}
                for i, line in enumerate(lines):
                    if i in skip:
                        continue
                    if RE_PRAGMA_ONCE.match(line):
                        continue
                    result.append(line)
                return result

    # No classic guard found — just strip #pragma once lines
    for line in lines:
        if RE_PRAGMA_ONCE.match(line):
            continue
        result.append(line)
    return result


def process_file(path: Path) -> None:
    """
    Recursively process a header file:
      - Collect system includes into system_includes (de-duped).
      - Recursively inline local includes (topological).
      - Append body content (minus guards/pragmas) to body_chunks.
    """
    canon = path.resolve()
    if canon in visited:
        return
    visited.add(canon)

    try:
        text = canon.read_text(encoding="utf-8")
    except OSError as e:
        print(f"ERROR: cannot read {canon}: {e}", file=sys.stderr)
        sys.exit(1)

    lines = text.splitlines(keepends=True)

    # First pass: collect system includes and recurse into local includes,
    # but don't emit the include lines themselves.
    # Build the list of "output lines" for this file.
    output_lines: list[str] = []

    for line in lines:
        # Check for system include
        m = RE_SYSTEM.match(line)
        if m:
            token = m.group(1)
            if token not in system_seen:
                system_seen.add(token)
                system_includes.append(token)
            # Don't emit the include line — system includes are hoisted
            continue

        # Check for local include
        m = RE_LOCAL.match(line)
        if m:
            token = m.group(1)
            if is_excluded(token):
                # Emit a commented-out note so readers understand the omission
                output_lines.append(
                    f"// #include \"{token}\"  -- excluded from single-header (opt-in only)\n"
                )
                continue

            resolved = resolve_local(path, token)
            if resolved is None:
                print(
                    f"WARNING: cannot resolve local include '{token}' from {path}",
                    file=sys.stderr,
                )
                output_lines.append(line)  # keep original line if unresolvable
                continue

            # Recurse first (topological: dependency before dependent)
            process_file(resolved)
            # Don't emit the include line — the content is inlined
            continue

        # All other lines pass through
        output_lines.append(line)

    # Strip include guards / pragma once from output lines
    clean_lines = strip_include_guards(output_lines)

    # Suppress files that become empty (or only whitespace) after stripping
    content = "".join(clean_lines).strip()
    if not content:
        return

    # Emit a file-separation comment so the amalgamation is navigable
    rel = canon.relative_to(REPO_ROOT)
    separator = (
        f"\n// {'─' * 74}\n"
        f"// {rel}\n"
        f"// {'─' * 74}\n\n"
    )
    body_chunks.append(separator)
    body_chunks.append("".join(clean_lines))


def main() -> None:
    process_file(ENTRY_POINT)

    # Build the final output
    banner = (
        "// =============================================================================\n"
        "// keylight_single.hpp — Keylight C++ SDK  (single-header amalgamation)\n"
        "//\n"
        "// AUTO-GENERATED — DO NOT EDIT\n"
        "// Regenerate with:  python3 tools/amalgamate.py\n"
        "//\n"
        "// Include this single file in your project instead of the split headers.\n"
        "// To use the cpp-httplib transport, define KEYLIGHT_BUILD_HTTPLIB_TRANSPORT\n"
        "// and add third_party/ to your include path before including this file.\n"
        "//\n"
        "// SPDX-License-Identifier: MIT\n"
        "// =============================================================================\n"
        "#pragma once\n"
        "\n"
    )

    # Sort system includes for determinism
    sorted_sys = sorted(system_includes)
    sys_block = "".join(f"#include <{s}>\n" for s in sorted_sys) + "\n"

    output = banner + sys_block + "".join(body_chunks)

    # Normalise line endings to LF
    output = output.replace("\r\n", "\n").replace("\r", "\n")

    OUTPUT_FILE.write_text(output, encoding="utf-8")
    print(f"Generated: {OUTPUT_FILE}")
    print(f"  System includes hoisted: {len(sorted_sys)}")
    print(f"  Headers inlined:         {len(visited)}")


if __name__ == "__main__":
    main()
