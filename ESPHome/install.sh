#!/bin/sh

# Create a fresh environment
python3.13 -m venv esphome_2026

# Activate it
source esphome_2026/bin/activate  # Mac/Linux
# .\esphome_env\Scripts\activate  # Windows

# Install fresh
pip install esphome
