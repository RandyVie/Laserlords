#!/bin/sh
# Linux x86_64 (Steam Deck / desktop) via Docker: dist/LaserLords-linux-x86_64.tar.gz
set -e
cd "$(dirname "$0")/.."
docker build --platform linux/amd64 -f packaging/linux/Dockerfile -t laserlords-linux .
mkdir -p dist/linux
id=$(docker create --platform linux/amd64 laserlords-linux)
docker cp "$id:/build/laserlords" dist/linux/laserlords
docker rm "$id" >/dev/null
cp packaging/linux/laserlords.desktop packaging/icon_1024.png dist/linux/
tar -C dist -czf dist/LaserLords-linux-x86_64.tar.gz linux
echo "dist/LaserLords-linux-x86_64.tar.gz"
