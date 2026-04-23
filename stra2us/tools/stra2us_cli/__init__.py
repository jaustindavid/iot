"""Reference CLI for Stra2us KV catalogs.

See docs/catalog_spec.md in the stra2us repo for the catalog schema
and resolution contract. The CLI is a thin shell over three concerns:

    catalog.py  — YAML loader + schema validation (pydantic)
    client.py   — HMAC-signed HTTP client for the stra2us server
    config.py   — credential / host lookup (flag → env → .stra2us)
    cli.py      — argparse + verb dispatch
"""

__version__ = "0.1.0"
