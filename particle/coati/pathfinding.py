"""
A* pathfinding, parameterized by the IR's pathfinding config.
"""
from __future__ import annotations
import heapq
import math
from .ir import PathfindingIR


def euclidean(a: tuple[int, int], b: tuple[int, int]) -> float:
    return math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2)


def find_path(
    start: tuple[int, int],
    dest: tuple[int, int],
    grid_w: int,
    grid_h: int,
    self_index: int,
    agent_positions: list[tuple[int, int]],
    lit_cells: set[tuple[int, int]],
    config: PathfindingIR,
) -> list[tuple[int, int]]:
    """
    A* from start to dest. Returns list of points to walk (not including start).
    Falls back to best frontier node if dest is unreachable within max_nodes.
    """
    if start == dest:
        return []

    # Cost and tracking arrays as dicts (sparse — fine for 32x8 grids)
    g_cost: dict[tuple[int, int], float] = {start: 0.0}
    parent: dict[tuple[int, int], tuple[int, int]] = {}
    visited: set[tuple[int, int]] = set()

    # Priority queue: (f_cost, tiebreaker, point)
    counter = 0
    pq: list[tuple[float, int, tuple[int, int]]] = []
    heapq.heappush(pq, (euclidean(start, dest), counter, start))

    best_frontier = start
    best_h = euclidean(start, dest)
    nodes_expanded = 0

    agent_set = set(agent_positions)

    while pq and nodes_expanded < config.max_nodes:
        _, _, curr = heapq.heappop(pq)

        if curr in visited:
            continue
        visited.add(curr)
        nodes_expanded += 1

        if curr == dest:
            best_frontier = dest
            break

        h = euclidean(curr, dest)
        if h < best_h:
            best_h = h
            best_frontier = curr

        cx, cy = curr
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                nx, ny = cx + dx, cy + dy
                if nx < 0 or nx >= grid_w or ny < 0 or ny >= grid_h:
                    continue

                npt = (nx, ny)

                # Base step cost
                if dx == 0 or dy == 0:
                    step = config.orthogonal_cost
                else:
                    step = config.diagonal_cost

                # Penalties
                penalty = 0.0
                is_dest = (npt == dest)
                if not is_dest:
                    if npt in lit_cells:
                        for p in config.penalties:
                            if p.condition == "lit":
                                penalty += p.cost
                    if npt in agent_set:
                        for p in config.penalties:
                            if p.condition == "occupied":
                                penalty += p.cost

                nc = g_cost[curr] + step + penalty
                if nc < g_cost.get(npt, float("inf")):
                    g_cost[npt] = nc
                    parent[npt] = curr
                    counter += 1
                    heapq.heappush(pq, (nc + euclidean(npt, dest), counter, npt))

    # Reconstruct path
    target = dest if dest in g_cost and g_cost[dest] < float("inf") else best_frontier
    if target == start:
        return []

    path = []
    cur = target
    safety = 0
    while cur != start and safety < 512:
        path.append(cur)
        cur = parent.get(cur, start)
        safety += 1
    path.reverse()
    return path
