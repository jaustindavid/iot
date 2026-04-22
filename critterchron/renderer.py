import pygame
import time
import random
from engine import CritterEngine

# Baseline 16x16 window was 640x640 at cell_size=40. For non-square grids we
# still want a reasonable window footprint, so cap both dimensions and pick
# the largest cell size that fits.
MAX_SCREEN_W = 1280
MAX_SCREEN_H = 720
FPS = 50

class CritterRenderer:
    def __init__(self, engine):
        pygame.init()
        self.engine = engine
        self.width = engine.width
        self.height = engine.height
        self.cell_size = max(8, min(40, MAX_SCREEN_W // self.width, MAX_SCREEN_H // self.height))
        self.screen_width = self.width * self.cell_size
        self.screen_height = self.height * self.cell_size
        self.screen = pygame.display.set_mode((self.screen_width, self.screen_height))
        pygame.display.set_caption("CritterChron Simulator")
        self.clock = pygame.time.Clock()

        # Luma Fading: Track previous grid states to ramp brightness
        self.tile_alphas = [[0.0 for _ in range(self.height)] for _ in range(self.width)]
        self.last_tick_time = time.time()

    def _get_rainbow_color(self):
        """Generates a cycling RGB color for glitched agents."""
        t = time.time() * 10
        r = int(127 + 127 * pygame.math.sin(t))
        g = int(127 + 127 * pygame.math.sin(t + 2))
        b = int(127 + 127 * pygame.math.sin(t + 4))
        return (r, g, b)

    def draw(self):
        self.screen.fill((10, 10, 10)) # Dark background
        now = time.time()
        
        # Calculate progress through the current tick
        tick_duration = self.engine.ir["simulation"]["tick_rate"] / 1000.0
        elapsed = now - self.last_tick_time
        tick_progress = min(1.0, elapsed / tick_duration)

        # 0.5. Render Landmarks as background LED tiles
        for name, lmark in self.engine.ir.get("landmarks", {}).items():
            pts = lmark.get("points", [])
            cname = lmark.get("color")
            color = self.engine._resolve_color(cname, default=(100, 100, 100))
            for pt in pts:
                rect = (pt[0] * self.cell_size, pt[1] * self.cell_size, self.cell_size - 1, self.cell_size - 1)
                pygame.draw.rect(self.screen, color, rect)

        # Render grid pixels
        for x in range(self.width):
            for y in range(self.height):
                # Base color
                tile = self.engine.grid[x][y]
                target_alpha = 1.0 if tile.state else 0.0
                
                # Step alpha
                alpha_step = 1.0 / (tick_duration * FPS)
                if self.tile_alphas[x][y] < target_alpha:
                    self.tile_alphas[x][y] = min(target_alpha, self.tile_alphas[x][y] + alpha_step)
                elif self.tile_alphas[x][y] > target_alpha:
                    self.tile_alphas[x][y] = max(target_alpha, self.tile_alphas[x][y] - alpha_step)
                
                # Start with grid color (lit-tile RGB, faded by alpha)
                tile_r = tile.color[0] * self.tile_alphas[x][y]
                tile_g = tile.color[1] * self.tile_alphas[x][y]
                tile_b = tile.color[2] * self.tile_alphas[x][y]

                # Additively composite any marker ramp on top. Each
                # marker contributes `count * rgb_floats` to the tile
                # channel sum (pre-clamp). Markers stack additively
                # with each other and with the lit color — e.g. a lit
                # tile at (r,g,b) with a 5-unit trail at (1.0,0.5,0.0)
                # reads as (r+5, g+2.5, b). Clamp happens below after
                # the agent composite.
                for name, spec in self.engine._markers.items():
                    c = tile.count[spec["index"]]
                    if c:
                        # Night overrides the per-unit ramp via
                        # engine.marker_ramp(), which falls through to the
                        # day ramp when no night entry is declared for
                        # this marker. Decay K/T isn't consulted here —
                        # this is the visual coefficient only.
                        rr, gg, bb = self.engine.marker_ramp(name)
                        tile_r += rr * c
                        tile_g += gg * c
                        tile_b += bb * c

                # Agent dominates — pick the highest-presence agent on this tile
                presence = 0.0
                agent_color = None
                for agent in self.engine.agents:
                    p1 = agent.prev_pos
                    p2 = agent.pos

                    # Fraction of the step animation completed. For a single-tick
                    # step (step_duration == 1) this equals tick_progress. For a
                    # slow step (step_duration > 1) it spans the whole window so
                    # the agent smears continuously from p1 to p2.
                    dur = max(1, agent.step_duration)
                    lerp_frac = (dur - agent.remaining_ticks + tick_progress) / dur
                    lerp_frac = max(0.0, min(1.0, lerp_frac))

                    if tuple(p1) == (x, y) and tuple(p2) == (x, y):
                        p = 1.0
                    elif tuple(p1) == (x, y):
                        p = 1.0 - lerp_frac
                    elif tuple(p2) == (x, y):
                        p = lerp_frac
                    else:
                        continue

                    if p > presence:
                        presence = p
                        agent_color = self.engine._agent_color(agent)

                if agent_color is not None:
                    r = agent_color[0] * presence + tile_r * (1.0 - presence)
                    g = agent_color[1] * presence + tile_g * (1.0 - presence)
                    b = agent_color[2] * presence + tile_b * (1.0 - presence)
                else:
                    r, g, b = tile_r, tile_g, tile_b
                
                # Clamp colors
                r = min(255, max(0, int(r)))
                g = min(255, max(0, int(g)))
                b = min(255, max(0, int(b)))
                
                if r > 0 or g > 0 or b > 0:
                    rect = (x * self.cell_size, y * self.cell_size, self.cell_size - 1, self.cell_size - 1)
                    pygame.draw.rect(self.screen, (r, g, b), rect)

                # Optional: Subtle guide for 'intended' layer
                if tile.intended:
                    inset = max(2, self.cell_size // 3)
                    size = max(2, self.cell_size - 2 * inset)
                    guide_rect = (x * self.cell_size + inset, y * self.cell_size + inset, size, size)
                    pygame.draw.rect(self.screen, (40, 40, 40), guide_rect, 1)

        pygame.display.flip()

    def run_tick(self):
        """Triggers the engine and resets the smoothing timer."""
        self.engine.tick()
        self.last_tick_time = time.time()

if __name__ == "__main__":
    # Test stub
    from compiler import CritCompiler
    import sys
    
    if len(sys.argv) < 2:
        print("Usage: python renderer.py <file.crit>")
        sys.exit(1)

    compiler = CritCompiler()
    ir = compiler.compile(sys.argv[1])
    engine = CritterEngine(ir)
    renderer = CritterRenderer(engine)

    # Initial time sync
    from SPEC_DATA import MEGAFONT_5X6 # Assume font is in a helper file
    engine.sync_time(MEGAFONT_5X6)

    running = True
    last_sim_tick = time.time()
    
    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

        # Simulation Heartbeat
        now = time.time()
        tick_rate_sec = ir["simulation"]["tick_rate"] / 1000.0
        if now - last_sim_tick >= tick_rate_sec:
            renderer.run_tick()
            last_sim_tick = now
            # Sync time every second
            engine.sync_time(MEGAFONT_5X6)

        renderer.draw()
        renderer.clock.tick(FPS)

    pygame.quit()
