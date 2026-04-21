import argparse
import random
import sys
import time
from compiler import CritCompiler
from engine import CritterEngine
from renderer import CritterRenderer

# Shared font data
MEGAFONT_5X6 = {
    "0": [0x70, 0x98, 0x98, 0x98, 0x98, 0x70], "1": [0x30, 0x70, 0x30, 0x30, 0x30, 0x78],
    "2": [0xF0, 0x18, 0x70, 0xC0, 0xC0, 0xF8], "3": [0xF8, 0x18, 0x70, 0x18, 0x98, 0x70],
    "4": [0x80, 0x98, 0xF8, 0x18, 0x18, 0x18], "5": [0xF8, 0xC0, 0xF0, 0x18, 0x98, 0x70],
    "6": [0x70, 0xC0, 0xF0, 0xC8, 0xC8, 0x70], "7": [0xF8, 0x18, 0x30, 0x60, 0x60, 0x60],
    "8": [0x70, 0x98, 0x70, 0x98, 0x98, 0x70], "9": [0x70, 0x98, 0x78, 0x18, 0x98, 0x70],
}

def print_health_report(engine, start_time):
    """Outputs diagnostic metrics to the terminal."""
    duration = time.time() - start_time
    expected_duration = (engine.tick_count * engine.ir["simulation"]["tick_rate"]) / 1000.0
    drift = duration - expected_duration

    print("\n" + "="*30)
    print(" CRITTERCHRON HEALTH REPORT")
    print("="*30)
    print(f"Total Ticks:    {engine.tick_count}")
    print(f"Convergence:    {engine.health_metrics['convergences'] / engine.tick_count * 100:.1f}%")
    
    total_intended = engine.health_metrics.get('total_intended', 0)
    lit_rate = (engine.health_metrics.get('total_lit_intended', 0) / total_intended * 100) if total_intended > 0 else 0.0
    print(f"Lit Rate:       {lit_rate:.1f}%")
    
    print(f"Glitched (RBW): {engine.health_metrics['glitches']}")
    print(f"Failed Seeks:   {engine.health_metrics['failed_seeks']}")
    print(f"Step Contests:  {engine.health_metrics['step_contests']}")
    print(f"Wall Clock:     {duration:.2f}s")
    print(f"Sim Drift:      {drift:+.2f}s")
    print("="*30 + "\n")

def main():
    parser = argparse.ArgumentParser(description="CritterChron Simulator")
    parser.add_argument("file", help="Path to the .crit agent file")
    parser.add_argument("--ticks", type=int, default=0, help="Stop after N ticks (0 = infinite)")
    parser.add_argument("--tick-rate", type=int, default=None, help="Override tick rate in ms (ignores value in .crit file)")
    parser.add_argument("--headless", action="store_true", help="Run without Pygame window")
    parser.add_argument("--seed", type=int, default=None,
                        help="Seed the RNG for deterministic runs. Required for HAL smoke-test parity.")
    parser.add_argument("--dump-state", metavar="PATH", default=None,
                        help="Write tick-by-tick tile and agent state to PATH (JSON Lines). "
                             "Use with --seed for a reproducible reference trace.")
    parser.add_argument("--dump-fake-time", metavar="YYYY-MM-DDTHH:MM",
                        help="Freeze sync_time() to this wall-clock instant. "
                             "Pair with --dump-state so the oracle layer is stable across runs.")
    parser.add_argument("--width", type=int, default=16,
                        help="Grid width in tiles (default 16). Scripts that use "
                             "max_x resolve against this value.")
    parser.add_argument("--height", type=int, default=16,
                        help="Grid height in tiles (default 16). Scripts that use "
                             "max_y resolve against this value.")
    parser.add_argument("--trace", action="store_true",
                        help="Print one line per instruction dispatched by any "
                             "agent (tick, name#id, pos, state, pc, opcode). "
                             "Firehose; pipe to grep for a specific agent.")
    parser.add_argument("--night", action="store_true",
                        help="Force night mode on. Engine resolves colors "
                             "through the script's `night:` palette (if "
                             "present) instead of the day palette. Blobs "
                             "without a night palette are unaffected.")
    args = parser.parse_args()

    if args.seed is not None:
        random.seed(args.seed)

    # 1. Compile
    compiler = CritCompiler(width=args.width, height=args.height)
    try:
        ir = compiler.compile(args.file)
    except Exception as e:
        print(f"Compilation Failed: {e}")
        sys.exit(1)

    if args.tick_rate is not None:
        ir["simulation"]["tick_rate"] = args.tick_rate

    # 2. Initialize Engine & Renderer
    engine = CritterEngine(ir, width=args.width, height=args.height)
    engine.trace = args.trace
    engine.night_mode = args.night
    renderer = None if args.headless else CritterRenderer(engine)

    dump_file = open(args.dump_state, "w") if args.dump_state else None
    frozen_now = None
    if args.dump_fake_time:
        import datetime as _dt
        frozen_now = _dt.datetime.strptime(args.dump_fake_time, "%Y-%m-%dT%H:%M")

    def do_sync_time():
        if frozen_now is not None:
            engine.sync_time_at(MEGAFONT_5X6, frozen_now)
        else:
            engine.sync_time(MEGAFONT_5X6)

    start_time = time.time()
    last_tick = time.time()
    running = True

    try:
        while running:
            if args.headless:
                do_sync_time()
                engine.tick()
                if dump_file is not None:
                    dump_file.write(engine.dump_state_jsonl())
                if args.ticks > 0 and engine.tick_count >= args.ticks:
                    running = False
            else:
                now = time.time()
                tick_rate_sec = ir["simulation"]["tick_rate"] / 1000.0

                # Tick logic
                if now - last_tick >= tick_rate_sec:
                    do_sync_time()
                    if renderer:
                        renderer.run_tick()
                    else:
                        engine.tick()
                    if dump_file is not None:
                        dump_file.write(engine.dump_state_jsonl())
                    last_tick = now

                    # Check termination
                    if args.ticks > 0 and engine.tick_count >= args.ticks:
                        running = False

                if renderer:
                    renderer.draw()
                    # Check for Pygame quit
                    import pygame
                    for event in pygame.event.get():
                        if event.type == pygame.QUIT: running = False

    except KeyboardInterrupt:
        pass

    if dump_file is not None:
        dump_file.close()

    # 3. Final Report
    print_health_report(engine, start_time)

if __name__ == "__main__":
    main()
