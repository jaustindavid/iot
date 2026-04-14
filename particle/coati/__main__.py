"""
CLI entry point: python -m coati <command> <script.coati> [options]

Commands:
  run     Run live in terminal (default)
  step    One tick at a time
  fast    No rendering, run to convergence
  parse   Parse and dump IR as JSON
"""
from __future__ import annotations
import argparse
import json
import sys
from dataclasses import asdict
from .parser import parse_file, ParseError
from .engine import CoatiEngine
from .simulator import run_live, run_step, run_fast
from .codegen import Codegen


def _json_default(obj):
    """Handle non-serializable types in IR."""
    if isinstance(obj, set):
        return list(obj)
    if hasattr(obj, "__dataclass_fields__"):
        return asdict(obj)
    return str(obj)


def main():
    parser = argparse.ArgumentParser(
        prog="coati",
        description="Coati — behavior language for grid particle simulations"
    )
    parser.add_argument("command", choices=["run", "step", "fast", "parse", "codegen"],
                        help="Simulation mode / codegen")
    parser.add_argument("script", help="Path to .coati script file")
    parser.add_argument("--speed", type=float, default=1.0,
                        help="Simulation speed multiplier (for 'run' mode)")
    parser.add_argument("--max-ticks", type=int, default=5000,
                        help="Max ticks before stopping (for 'fast' mode)")
    parser.add_argument("--time", type=str, default=None,
                        help="Static time HH:MM (for 'fast' mode, e.g. '12:34')")
    parser.add_argument("--output-dir", type=str, default=".",
                        help="Output directory for generated files (codegen mode)")
    parser.add_argument("--grid_width", type=int, default=None,
                        help="Override grid width (default: script value or 32)")
    parser.add_argument("--grid_height", type=int, default=None,
                        help="Override grid height (default: script value or 8)")

    args = parser.parse_args()

    # Parse script
    try:
        ir = parse_file(args.script, grid_width=args.grid_width, grid_height=args.grid_height)
    except ParseError as e:
        print(f"Parse error: {e}", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError:
        print(f"File not found: {args.script}", file=sys.stderr)
        sys.exit(1)

    if args.command == "parse":
        print(json.dumps(asdict(ir), indent=2, default=_json_default))
        return

    if args.command == "codegen":
        import os
        header, source = Codegen().generate(ir)
        out_dir = args.output_dir
        os.makedirs(out_dir, exist_ok=True)
        h_path = os.path.join(out_dir, "CoatiEngine.h")
        cpp_path = os.path.join(out_dir, "CoatiEngine.cpp")
        with open(h_path, "w") as f:
            f.write(header)
        with open(cpp_path, "w") as f:
            f.write(source)
        lm_info = ", ".join(f"{lm.name}({len(lm.cells)}cells)" for lm in ir.landmarks)
        rules = len(ir.behavior.rules)
        count = ir.agents.count
        mode = "pool" if ir.agents.pool_mode else "fixed"
        print(f"Generated: {h_path}")
        print(f"           {cpp_path}")
        print(f"  Grid:    {ir.grid.width}x{ir.grid.height}  Agents: {count} ({mode})")
        print(f"  Rules:   {rules}  Landmarks: {lm_info}")
        return

    # Build engine
    engine = CoatiEngine(ir)

    if args.command == "run":
        run_live(engine, ir, speed=args.speed)
    elif args.command == "step":
        run_step(engine, ir)
    elif args.command == "fast":
        static_time = None
        if args.time:
            parts = args.time.split(":")
            static_time = (int(parts[0]), int(parts[1]))
        run_fast(engine, ir, max_ticks=args.max_ticks, static_time=static_time)


if __name__ == "__main__":
    main()
