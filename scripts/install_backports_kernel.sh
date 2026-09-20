#!/bin/sh
set -eu

pkexec apt-get update
pkexec apt-get install -y linux-image-amd64/stable-backports linux-headers-amd64/stable-backports
