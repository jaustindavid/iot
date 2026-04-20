"""Round-trip test for ir_text. Encode every script in agents/, decode,
re-encode, and assert byte-exact equality. Also sanity-checks that the
decoded IR dict re-encodes identically (structural round trip).

Run: python3 hal/ir/test_ir_text.py
"""

from __future__ import annotations
import glob
import hashlib
import os
import sys
import traceback

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, "..", ".."))
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)

from compiler import CritCompiler              # noqa: E402
from hal.ir import ir_text                     # noqa: E402


def _stub_meta(script_path: str, src: str) -> dict:
    return {
        "name":            os.path.splitext(os.path.basename(script_path))[0],
        "src_sha256":      hashlib.sha256(src.encode("utf-8")).hexdigest(),
        "encoded_at":      "1970-01-01T00:00:00Z",
        "encoder_version": "test",
        "ir_version":      1,
        "source":          src,
    }


def _check_script(script_path: str) -> tuple[bool, str]:
    with open(script_path) as f:
        src = f.read()

    try:
        ir = CritCompiler().compile(script_path)
    except Exception as e:
        return False, f"compile failed: {e}"

    meta = _stub_meta(script_path, src)

    try:
        text1 = ir_text.encode(ir, meta)
    except Exception as e:
        return False, f"encode #1 failed: {e}\n{traceback.format_exc()}"

    try:
        ir2, meta2 = ir_text.decode(text1)
    except Exception as e:
        return False, f"decode failed: {e}\n{traceback.format_exc()}"

    try:
        text2 = ir_text.encode(ir2, meta2)
    except Exception as e:
        return False, f"encode #2 failed: {e}\n{traceback.format_exc()}"

    if text1 != text2:
        # Produce a readable diff hint.
        lines1 = text1.splitlines()
        lines2 = text2.splitlines()
        diff = []
        for i, (a, b) in enumerate(zip(lines1, lines2)):
            if a != b:
                diff.append(f"  line {i}: {a!r} != {b!r}")
                if len(diff) >= 5:
                    break
        if len(lines1) != len(lines2):
            diff.append(f"  line count: {len(lines1)} != {len(lines2)}")
        return False, "byte-exact round trip failed:\n" + "\n".join(diff)

    return True, f"ok ({len(text1)} bytes)"


def main() -> int:
    agents_dir = os.path.join(_REPO, "agents")
    scripts = sorted(glob.glob(os.path.join(agents_dir, "*.crit")))
    if not scripts:
        print(f"no scripts found in {agents_dir}", file=sys.stderr)
        return 1

    fails = 0
    for p in scripts:
        ok, msg = _check_script(p)
        flag = "PASS" if ok else "FAIL"
        print(f"[{flag}] {os.path.basename(p)}: {msg}")
        if not ok:
            fails += 1

    print()
    print(f"{len(scripts) - fails}/{len(scripts)} passed")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
