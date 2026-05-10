"""Critterchron failure-triage analyzer (FAILURE_TRIAGE.md §3).

Soft-client over Stra2us — tails heartbeat queues, computes per-device
and fleet metrics, fires alerts, optionally writes `dump_now` KV
triggers to close the loop with §1's on-device snapshot path.

Run as a service:
    python3 -m tools.analyzer --config analyzer.yaml

See `analyzer.example.yaml` for the configuration shape.
"""
