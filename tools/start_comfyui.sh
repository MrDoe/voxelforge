#!/bin/bash
# Start ComfyUI server for texture generation
# Usage: ./start_comfyui.sh [--background]
cd ~/comfyui
PYTHON=/home/christoph/code/bark-venv/bin/python
if [ "$1" = "--background" ]; then
    nohup $PYTHON main.py --listen 127.0.0.1 --port 8188 > /tmp/opencode/comfyui.log 2>&1 &
    echo "ComfyUI started in background (PID $!)"
    echo "Logs: /tmp/opencode/comfyui.log"
    echo "GUI: http://127.0.0.1:8188"
else
    $PYTHON main.py --listen 127.0.0.1 --port 8188
fi
