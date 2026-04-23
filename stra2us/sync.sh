#!/bin/sh

rsync -va ~/src/claude-sandbox/stra2us/* . --exclude=__pycache__ --exclude=venv --exclude=.pytest_cache
