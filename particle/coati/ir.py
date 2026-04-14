"""
Intermediate Representation for .coati scripts.

These dataclasses are the canonical IR that the parser produces
and the engine/simulator consume. They can be serialized to JSON
for inspection or (future) transmission to a device.
"""
from __future__ import annotations
from dataclasses import dataclass, field
from typing import Any


# ── Grid ────────────────────────────────────────────────────────

@dataclass
class GridIR:
    width: int
    height: int
    wiring: str = "zigzag"


# ── Layers ──────────────────────────────────────────────────────

@dataclass
class LayerIR:
    name: str
    type: str          # "bool", "int", "float"
    default: Any       # False, 0, 0.0


# ── Landmarks ───────────────────────────────────────────────────

@dataclass
class LandmarkIR:
    name: str
    cells: list[tuple[int, int]]
    color: tuple[int, int, int] | None = None


# ── Terms ───────────────────────────────────────────────────────

@dataclass
class TermExpr:
    """A boolean expression over layers at a grid cell."""
    op: str                    # "and", "or", "not", "eq"
    args: list[TermExpr] = field(default_factory=list)
    layer: str | None = None   # for "eq" nodes
    value: Any = None          # for "eq" nodes

@dataclass
class TermIR:
    name: str
    expr: TermExpr


# ── Agents ──────────────────────────────────────────────────────

@dataclass
class PropertyIR:
    name: str
    type: str          # "bool", "int", "float"
    default: Any

@dataclass
class AgentsIR:
    type_name: str
    count: int
    starts: list[tuple[int, int]]
    properties: list[PropertyIR]
    pool_mode: bool = False         # True = "up to N" (agents spawn/despawn)
    spawn_point: tuple[int, int] | None = None  # where new agents appear


# ── Spawning ────────────────────────────────────────────────────

@dataclass
class SpawningIR:
    every_ticks: int = 10
    condition: ConditionNode | None = None  # when to spawn (e.g. "missing exist")


# ── Pathfinding ─────────────────────────────────────────────────

@dataclass
class PenaltyIR:
    condition: str     # "lit", "occupied"
    cost: float

@dataclass
class PathfindingIR:
    algorithm: str = "astar"
    heuristic: str = "euclidean"
    max_nodes: int = 256
    orthogonal_cost: float = 1.0
    diagonal_cost: float = 1.414
    penalties: list[PenaltyIR] = field(default_factory=list)


# ── Behavior ────────────────────────────────────────────────────

@dataclass
class ConditionNode:
    """A boolean condition in the behavior tree."""
    op: str
    # For binary/nary ops ("and", "or"): args is a list of children
    args: list[ConditionNode] = field(default_factory=list)
    # For leaf ops ("prop_true", "prop_false", "gt", "lt", "eq",
    #   "at_landmark", "landmark_free", "standing_on_term",
    #   "cell_is_lit", "term_exists", "term_empty",
    #   "at_claimed", "at_any_landmark", "near_lit", "on_lit"):
    prop: str | None = None
    value: Any = None
    name: str | None = None     # landmark or term name

@dataclass
class ActionNode:
    """A single action in the behavior tree."""
    action: str
    # Optional parameters depending on action type:
    prop: str | None = None
    value: Any = None
    landmark: str | None = None
    term: str | None = None
    source: str | None = None   # "board", "dumpster"
    # For wander:
    avoid: list[str] | None = None
    chance: int | None = None
    forced_condition: str | None = None

@dataclass
class IfNode:
    """An if/else block within a rule body."""
    condition: ConditionNode
    then_body: list[ActionNode | IfNode]
    else_body: list[ActionNode | IfNode] = field(default_factory=list)

@dataclass
class BehaviorRule:
    """A top-level 'when' block."""
    condition: ConditionNode
    body: list[ActionNode | IfNode]

@dataclass
class BehaviorIR:
    pathfind_limit: int = 1
    rules: list[BehaviorRule] = field(default_factory=list)
    tick_actions: list[ActionNode] = field(default_factory=list)  # "each tick:" block


# ── Rendering ───────────────────────────────────────────────────

@dataclass
class ColorExpr:
    r: int
    g: int
    b: int
    modulation: str = "solid"      # "solid", "pulse", "shimmer"
    mod_period_ms: int = 0
    multiply_layer: str | None = None  # e.g. "fade"

@dataclass
class RenderRule:
    selector: str          # e.g. "board_pixel", "agent_carrying_not_washed", "agent_when"
    agent_index: int | None = None   # None = all agents
    color: ColorExpr = field(default_factory=lambda: ColorExpr(0, 0, 0))
    condition: "ConditionNode | None" = None  # only set for "when <cond>: agent" rules

@dataclass
class RenderingIR:
    physics_hz: int = 10
    display_hz: int = 60
    interpolated: bool = True
    rules: list[RenderRule] = field(default_factory=list)


# ── Goal ────────────────────────────────────────────────────────

@dataclass
class WobblyTimeIR:
    min_offset: int = 120
    max_offset: int = 300
    fast_rate: float = 1.3
    slow_rate: float = 0.7

@dataclass
class GoalIR:
    type: str = "clock"
    font: str = "megafont"
    font_size: tuple[int, int] = (5, 6)
    layout: str = "horizontal"
    time_config: WobblyTimeIR | None = None


# ── Collision ───────────────────────────────────────────────────

@dataclass
class CollisionIR:
    body: list[ActionNode | IfNode] = field(default_factory=list)


# ── Top-level IR ────────────────────────────────────────────────

@dataclass
class CoatiIR:
    grid: GridIR = field(default_factory=lambda: GridIR(32, 8))
    layers: list[LayerIR] = field(default_factory=list)
    landmarks: list[LandmarkIR] = field(default_factory=list)
    terms: list[TermIR] = field(default_factory=list)
    agents: AgentsIR = field(default_factory=lambda: AgentsIR("agent", 1, [(0, 0)], []))
    spawning: SpawningIR | None = None
    pathfinding: PathfindingIR = field(default_factory=PathfindingIR)
    behavior: BehaviorIR = field(default_factory=BehaviorIR)
    rendering: RenderingIR = field(default_factory=RenderingIR)
    goal: GoalIR = field(default_factory=GoalIR)
    collision: CollisionIR = field(default_factory=CollisionIR)
