#!/bin/sh
set -eu

sudo apt-get update
sudo apt-get install -y linux-image-amd64/stable-backports linux-headers-amd64/stable-backports
