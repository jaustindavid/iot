#!/bin/sh

rsync -va ~/src/claude-sandbox/critterchron/* . --exclude=__pycache__ --exclude=venv
