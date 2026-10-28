#!/usr/bin/env bash
# Host-side (PC) tests for the logic that does not need the ESP32: storage, config parser, URL handling, HTML escaping.
set -e
cd "$(dirname "$0")"
S=../../src

# O suite cria e apaga cartoes em tests_sdcard; restos de uma execucao
# interrompida mudavam o resultado da seguinte.
rm -rf tests_sdcard

SAN_FLAGS=""
if gcc -fsanitize=address -xc /dev/null -o /dev/null 2>/dev/null; then
  SAN_FLAGS="-fsanitize=address,undefined"
fi

CXX="g++ -std=c++14 -g $SAN_FLAGS -w -I stubs -I $S"

OUT_T="./owmc_t"
OUT_TW="./owmc_tw"
if [ -d "/tmp" ] && [ -w "/tmp" ]; then
  OUT_T="/tmp/owmc_t"
  OUT_TW="/tmp/owmc_tw"
fi

$CXX -include stubs/bsd_compat.h -DCONFIG_FILE_PATH="\"test_config.txt\"" test_main.cc common_stubs.cc $S/storage/storage.cc $S/storage/config.cc $S/web/percent_decode.cc $S/storage/ps1_empty_card.cc $S/storage/game_lookup.cc $S/storage/parent_db.cc -o "$OUT_T" -lpthread
"$OUT_T"

# Guardas de tamanho de estrutura (nenhuma logica, so static_assert).
$CXX -include stubs/bsd_compat.h test_ram.cc -o "${OUT_T}_ram"
"${OUT_T}_ram"

$CXX -include stubs/bsd_compat.h -include freertos/FreeRTOS.h -include freertos/task.h test_web.cc common_stubs.cc $S/storage/storage.cc $S/storage/ps1_empty_card.cc $S/web/percent_decode.cc $S/storage/game_lookup.cc $S/storage/parent_db.cc -o "$OUT_TW" -lpthread
"$OUT_TW"
