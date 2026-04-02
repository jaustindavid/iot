#!/bin/bash
# Start script for the Stra2us IoT Backend

echo "Starting Stra2us IoT Backend..."
if [ ! -d "venv" ]; then
    echo "Virtual environment not found! Please run 'python3 -m venv venv && source venv/bin/activate && pip install -r requirements.txt' first."
    exit 1
fi

source venv/bin/activate
uvicorn src.main:app --reload
