#!/usr/bin/env bash
# Host-side (PC) tests for the logic that does not need the ESP32: storage, config parser, URL handling, HTML escaping.
# Needs g++ and a writable /sdcard directory (storage.cc mounts the card at /sdcard):
#   sudo mkdir -p /sdcard && sudo chown "$USER" /sdcard
set -e
cd "$(dirname "$0")"
S=../../src
CXX="g++ -std=c++14 -g -fsanitize=address,undefined -w -I stubs -I $S"
gcc -c blobs.c -o /tmp/owmc_blobs.o
$CXX -DCONFIG_FILE_PATH="\"/tmp/test_config.txt\"" test_main.cc common_stubs.cc $S/storage/storage.cc $S/storage/config.cc $S/web/percent_decode.cc $S/storage/ps1_empty_card.cc -o /tmp/owmc_t -lpthread
/tmp/owmc_t
$CXX -include stubs/bsd_compat.h -include freertos/FreeRTOS.h -include freertos/task.h test_web.cc $S/web/percent_decode.cc common_stubs.cc $S/storage/storage.cc $S/storage/ps1_empty_card.cc /tmp/owmc_blobs.o -o /tmp/owmc_tw -lpthread
/tmp/owmc_tw
