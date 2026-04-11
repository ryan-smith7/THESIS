#!/bin/bash

# Navigate to the DT7 directory
cd ~/csse4011/repo/prac2/task7

# Bring down the Docker Compose setup
docker compose down

# Bring up the Docker Compose setup and watch for changes
docker compose up -d

# Open the specified URL in the default web browser
open "http://localhost:3000/d/gui-prac2/thesis-sensors?orgId=1&from=now-15m&to=now&timezone=browser&refresh=5s"

# Run the main.py script with the specified Python executable
/opt/anaconda3/bin/python /Users/ryan/csse4011/repo/prac2/task7/app/main.py
