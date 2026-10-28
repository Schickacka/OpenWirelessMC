#!/usr/bin/env python3
"""
tools/test_migrate_sd.py - Testes de aceitação do script de migração tools/migrate_sd.py
Normativo: Seção 18.9 do dossiê OWMC.
18 casos executados em diretório temporário, estilo assert puro.
"""

import os
import sys
import shutil
import tempfile
import hashlib
import subprocess
import threading
import json
import struct
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler

SCRIPT_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), "migrate_sd.py"))
PARENT_DB_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "sdcard_assets", "multidisc.txt"))
PYTHON_EXE = sys.executable

def make_blank_card(pattern_byte=0x00) -> bytes:
    card = bytearray(131072)
    card[0:2] = b"MC"

    # Frames 1..15: Directory entries
    for b in range(1, 16):
        d_off = b * 128
        card[d_off : d_off + 4] = b"\xA0\x00\x00\x00"
        card[d_off + 4 : d_off + 8] = b"\x00\x00\x00\x00"
        card[d_off + 8 : d_off + 10] = b"\xFF\xFF"

    if pattern_byte != 0x00:
        d_off = 1 * 128
        card[d_off : d_off + 4] = b"\x51\x00\x00\x00"
        card[d_off + 4 : d_off + 8] = struct.pack("<I", 8192)
        card[d_off + 8 : d_off + 10] = b"\xFF\xFF"
        name = f"BASLUS-00421-{pattern_byte}".encode("ascii")
        card[d_off + 10 : d_off + 10 + len(name)] = name

        b1_off = 8192
        card[b1_off : b1_off + 2] = b"SC"
        card[b1_off + 2 : b1_off + 8192] = bytes([pattern_byte]) * 8190

    for f in range(1024):
        fo = f * 128
        f_chk = 0
        for b in card[fo : fo + 127]:
            f_chk ^= b
        card[fo + 127] = f_chk

    return bytes(card)

def hash_tree(dir_path: str):
    hashes = {}
    for root, dirs, files in os.walk(dir_path):
        for f in sorted(files):
            p = os.path.join(root, f)
            rel = os.path.relpath(p, dir_path).replace("\\", "/")
            with open(p, "rb") as fp:
                hashes[rel] = hashlib.sha256(fp.read()).hexdigest()
    return hashes

# ----------------- 18 Casos de Teste -----------------

def test_1_dry_run_zero_byte_change():
    td = tempfile.mkdtemp()
    try:
        os.makedirs(os.path.join(td, "games", "testgame"), exist_ok=True)
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("wifi=1")
        with open(os.path.join(td, "default.mc"), "wb") as f: f.write(make_blank_card(1))
        with open(os.path.join(td, "multidisc.txt"), "w") as f: f.write("SLUS-00592=SLUS-00421\n")
        with open(os.path.join(td, "Game2Folder.ini"), "w") as f: f.write("[PS1]\nSLUS-00421=SLUS-00421\n")
        with open(os.path.join(td, "games", "testgame", "SLUS-00421-1.mc"), "wb") as f: f.write(make_blank_card(2))

        before = hash_tree(td)
        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--dry-run", "--parent-db", PARENT_DB_PATH]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert res.returncode == 0, f"dry-run falhou: {res.stderr.decode()}"
        after = hash_tree(td)
        assert before == after, "Erro: dry-run modificou arquivos no disco!"
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_2_apply_re2_unified_and_verify():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        with open(os.path.join(td, "SLUS-00421.mc"), "wb") as f: f.write(make_blank_card(1))
        with open(os.path.join(td, "SLUS-00592.mc"), "wb") as f: f.write(make_blank_card(2))

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert res.returncode == 0, f"apply falhou: {res.stderr.decode()}"

        re2_dir = os.path.join(td, "MemoryCards", "PS1", "SLUS-00421")
        assert os.path.isdir(re2_dir), "Pasta SLUS-00421 não foi criada!"
        files = os.listdir(re2_dir)
        assert "SLUS-00421-1.mcd" in files, "SLUS-00421-1.mcd não encontrado"
        assert "SLUS-00421-2.mcd" in files, "SLUS-00421-2.mcd não encontrado"

        cmd_v = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--verify", "--parent-db", PARENT_DB_PATH]
        res_v = subprocess.run(cmd_v, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert res_v.returncode == 0, f"verify falhou após apply: {res_v.stderr.decode()}"
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_3_second_apply_nothing_to_migrate():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        with open(os.path.join(td, "SLUS-00421.mc"), "wb") as f: f.write(make_blank_card(1))
        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        subprocess.run(cmd, check=True)

        tree1 = hash_tree(td)
        cmd2 = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        res2 = subprocess.run(cmd2, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert res2.returncode == 0
        tree2 = hash_tree(td)
        assert tree1 == tree2, "Segunda execução de apply alterou arquivos!"
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_4_verify_before_apply_returns_2():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        with open(os.path.join(td, "SLUS-00421.mc"), "wb") as f: f.write(make_blank_card(1))
        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--verify", "--parent-db", PARENT_DB_PATH]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert res.returncode == 2, f"verify antes de apply deve retornar 2, retornou {res.returncode}"
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_5_multidisc_compiled_and_custom():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        os.makedirs(os.path.join(td, ".sd2psx"), exist_ok=True)
        # Se SLUS-00421 tem override para RE2-MEU
        with open(os.path.join(td, ".sd2psx", "Game2Folder.ini"), "w") as f:
            f.write("[PS1]\nSLUS-00421 = RE2-MEU\n")
        with open(os.path.join(td, "multidisc.txt"), "w") as f:
            f.write("SLUS-00592=SLUS-00421\nCUSTOM-00001=SLUS-00421\n")
        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        subprocess.run(cmd, check=True)

        assert os.path.isfile(os.path.join(td, "multidisc.txt.bak")), "multidisc.txt.bak não criado"
        assert not os.path.isfile(os.path.join(td, "multidisc.txt")), "multidisc.txt original não deveria existir"
        g2f = os.path.join(td, ".sd2psx", "Game2Folder.ini")
        assert os.path.isfile(g2f), ".sd2psx/Game2Folder.ini não criado"
        with open(g2f, "r") as f:
            c = f.read()
            # Item 3: grava FILHO = <pasta resolvida do PAI> (RE2-MEU)
            assert "CUSTOM-00001 = RE2-MEU" in c, f"Chave custom deve apontar para pasta resolvida do PAI: {c}"
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_6_collision_different_content_picks_next_channel():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        c1 = make_blank_card(0x11)
        c2 = make_blank_card(0x22)
        with open(os.path.join(td, "SLUS-00421-1.mc"), "wb") as f: f.write(c1)
        with open(os.path.join(td, "SLUS-00592-1.mc"), "wb") as f: f.write(c2)

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        subprocess.run(cmd, check=True)

        f1 = os.path.join(td, "MemoryCards", "PS1", "SLUS-00421", "SLUS-00421-1.mcd")
        f2 = os.path.join(td, "MemoryCards", "PS1", "SLUS-00421", "SLUS-00421-2.mcd")
        assert os.path.isfile(f1) and os.path.isfile(f2), "Canais 1 e 2 devem existir"
        with open(f1, "rb") as fp: assert fp.read() == c1
        with open(f2, "rb") as fp: assert fp.read() == c2
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_7_exact_duplicate_sent_to_quarantine():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        c1 = make_blank_card(0x33)
        with open(os.path.join(td, "SLUS-00421-1.mc"), "wb") as f: f.write(c1)
        with open(os.path.join(td, "SLUS-00592-1.mc"), "wb") as f: f.write(c1)

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        subprocess.run(cmd, check=True)

        f1 = os.path.join(td, "MemoryCards", "PS1", "SLUS-00421", "SLUS-00421-1.mcd")
        assert os.path.isfile(f1)
        assert not os.path.isfile(os.path.join(td, "SLUS-00592-1.mc"))
        trash = os.path.join(td, ".sd2psx", "migrate-trash")
        assert os.path.isdir(trash), "Quarentena não criada"
        quarantine_files = []
        for r, d, fs in os.walk(trash):
            for file in fs: quarantine_files.append(file)
        assert "SLUS-00592-1.mc" in quarantine_files, "Duplicata não foi para quarentena"
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_8_empty_root_fails_sanity_code_3():
    td = tempfile.mkdtemp()
    try:
        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--parent-db", PARENT_DB_PATH]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert res.returncode == 3, f"root vazio deve retornar código 3, retornou {res.returncode}"
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_9_usercards_renaming():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        with open(os.path.join(td, "memcard_do_primo.mc"), "wb") as f: f.write(make_blank_card(0x44))
        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        subprocess.run(cmd, check=True)

        card = os.path.join(td, "MemoryCards", "PS1", "UserCards", "UserCards-1.mcd")
        assert os.path.isfile(card), "UserCards-1.mcd não criado"
        ini = os.path.join(td, "MemoryCards", "PS1", "UserCards", "UserCards.ini")
        assert os.path.isfile(ini), "UserCards.ini não criado"
        with open(ini, "r") as f:
            c = f.read()
            assert "[ChannelName]" in c and "1 = memcard do primo" in c, "Nome do canal incorreto no INI"
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_10_corrupt_size_file_sent_to_quarantine():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        with open(os.path.join(td, "bad.mc"), "wb") as f: f.write(b"X" * 8192)
        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)

        # Resumo deve reportar ERRO_EXT_OU_TAMANHO
        stdout = res.stdout.decode()
        assert "ERRO_EXT_OU_TAMANHO" in stdout, "Resumo perdeu ERRO_EXT_OU_TAMANHO"

        assert not os.path.exists(os.path.join(td, "MemoryCards", "PS1", "UserCards", "UserCards-1.mcd"))
        trash = os.path.join(td, ".sd2psx", "migrate-trash")
        assert any("bad.mc" in f for _, _, files in os.walk(trash) for f in files)
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_11_apply_without_yes_non_tty_code_3():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        with open(os.path.join(td, "SLUS-00421.mc"), "wb") as f: f.write(make_blank_card(1))
        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--parent-db", PARENT_DB_PATH]
        res = subprocess.run(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert res.returncode == 3, f"Esperado código 3, obteve {res.returncode}"
    finally:
        shutil.rmtree(td, ignore_errors=True)

def write_save_to_card(card: bytearray, start_block: int, num_blocks: int, name: str, pattern: bytes):
    chain = [start_block + i for i in range(num_blocks)]
    for i, blk in enumerate(chain):
        d_off = blk * 128
        state = 0x51 if num_blocks == 1 else (0x51 if i == 0 else (0x53 if i == num_blocks - 1 else 0x52))
        next_b = 0xFFFF if (num_blocks == 1 or i == num_blocks - 1) else chain[i + 1]
        card[d_off : d_off + 4] = struct.pack("<I", state)
        card[d_off + 4 : d_off + 8] = struct.pack("<I", num_blocks * 8192 if i == 0 else 0)
        card[d_off + 8 : d_off + 10] = struct.pack("<H", next_b)
        n_bytes = name.encode("ascii")
        card[d_off + 10 : d_off + 10 + len(n_bytes)] = n_bytes

        data_off = blk * 8192
        card[data_off : data_off + 2] = b"SC"
        card[data_off + 2 : data_off + 8192] = (pattern * 4096)[:8190]
        for f in range(64):
            fo = data_off + f * 128
            f_chk = 0
            for b in card[fo : fo + 127]: f_chk ^= b
            card[fo + 127] = f_chk

    for blk in chain:
        d_off = blk * 128
        f_chk = 0
        for b in card[d_off : d_off + 127]: f_chk ^= b
        card[d_off + 127] = f_chk

def test_12_merge_cards_synthetic_success():
    td = tempfile.mkdtemp()
    try:
        p_a = os.path.join(td, "A.mcd")
        p_b = os.path.join(td, "B.mcd")
        p_out = os.path.join(td, "OUT.mcd")

        card_a = bytearray(make_blank_card())
        write_save_to_card(card_a, start_block=1, num_blocks=1, name="BASLUS-00421SAVE1", pattern=b"AAA")

        card_b = bytearray(make_blank_card())
        write_save_to_card(card_b, start_block=1, num_blocks=1, name="BASLUS-00421SAVE2", pattern=b"BBB")

        with open(p_a, "wb") as f: f.write(card_a)
        with open(p_b, "wb") as f: f.write(card_b)

        hash_a_before = hashlib.sha256(card_a).hexdigest()
        hash_b_before = hashlib.sha256(card_b).hexdigest()

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--merge-cards", p_a, p_b, "-o", p_out]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert res.returncode == 0, f"--merge-cards falhou: {res.stderr.decode()}"

        assert os.path.isfile(p_out), "OUT.mcd não foi gerado"
        with open(p_a, "rb") as f: assert hashlib.sha256(f.read()).hexdigest() == hash_a_before
        with open(p_b, "rb") as f: assert hashlib.sha256(f.read()).hexdigest() == hash_b_before
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_13_merge_cards_conflict_rejected():
    td = tempfile.mkdtemp()
    try:
        p_a = os.path.join(td, "A.mcd")
        p_b = os.path.join(td, "B.mcd")
        p_out = os.path.join(td, "OUT.mcd")

        card_a = bytearray(make_blank_card())
        write_save_to_card(card_a, start_block=1, num_blocks=1, name="BASLUS-00421SAVE1", pattern=b"AAA")

        card_b = bytearray(make_blank_card())
        write_save_to_card(card_b, start_block=1, num_blocks=1, name="BASLUS-00421SAVE1", pattern=b"CONFLICT_DIFF")

        with open(p_a, "wb") as f: f.write(card_a)
        with open(p_b, "wb") as f: f.write(card_b)

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--merge-cards", p_a, p_b, "-o", p_out]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert res.returncode == 1, "Conflito de ID com payload diferente deve abortar"
        assert not os.path.exists(p_out), "OUT não deve existir em caso de conflito"
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_14_merge_cards_out_exists_refused():
    td = tempfile.mkdtemp()
    try:
        p_a = os.path.join(td, "A.mcd")
        p_b = os.path.join(td, "B.mcd")
        p_out = os.path.join(td, "OUT.mcd")
        with open(p_a, "wb") as f: f.write(make_blank_card())
        with open(p_b, "wb") as f: f.write(make_blank_card())
        with open(p_out, "wb") as f: f.write(b"existing")

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--merge-cards", p_a, p_b, "-o", p_out]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert res.returncode == 1, "OUT existente deve ser recusado"
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_15_three_cards_exact_channels_preserved():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        c1 = make_blank_card(1)
        c2 = make_blank_card(2)
        c3 = make_blank_card(3)
        with open(os.path.join(td, "SLUS-00421-1.mc"), "wb") as f: f.write(c1)
        with open(os.path.join(td, "SLUS-00421-2.mc"), "wb") as f: f.write(c2)
        with open(os.path.join(td, "SLUS-00421-3.mc"), "wb") as f: f.write(c3)

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        subprocess.run(cmd, check=True)

        re2_dir = os.path.join(td, "MemoryCards", "PS1", "SLUS-00421")
        for i, exp_c in enumerate([c1, c2, c3], 1):
            target = os.path.join(re2_dir, f"SLUS-00421-{i}.mcd")
            assert os.path.isfile(target), f"SLUS-00421-{i}.mcd não encontrado"
            with open(target, "rb") as f: assert f.read() == exp_c
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_16_channel_zero_and_non_numeric_suffix():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        # Item 2 do prompt: com SLUS-00421-0/-1/-2/-3 os explícitos 1,2,3 são preservados e -0 vai para 4!
        with open(os.path.join(td, "SLUS-00421-0.mc"), "wb") as f: f.write(make_blank_card(0x10))
        with open(os.path.join(td, "SLUS-00421-1.mc"), "wb") as f: f.write(make_blank_card(0x11))
        with open(os.path.join(td, "SLUS-00421-2.mc"), "wb") as f: f.write(make_blank_card(0x12))
        with open(os.path.join(td, "SLUS-00421-3.mc"), "wb") as f: f.write(make_blank_card(0x13))
        with open(os.path.join(td, "SLUS-00421-final.mc"), "wb") as f: f.write(make_blank_card(0x20))

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        subprocess.run(cmd, check=True)

        re2_dir = os.path.join(td, "MemoryCards", "PS1", "SLUS-00421")
        assert os.path.isfile(os.path.join(re2_dir, "SLUS-00421-1.mcd")), "SLUS-00421-1 deve estar no canal 1"
        assert os.path.isfile(os.path.join(re2_dir, "SLUS-00421-2.mcd")), "SLUS-00421-2 deve estar no canal 2"
        assert os.path.isfile(os.path.join(re2_dir, "SLUS-00421-3.mcd")), "SLUS-00421-3 deve estar no canal 3"
        assert os.path.isfile(os.path.join(re2_dir, "SLUS-00421-4.mcd")), "SLUS-00421-0 não deve roubar canal 1; deve ir para canal 4"

        # -final vai para UserCards
        f_user = os.path.join(td, "MemoryCards", "PS1", "UserCards", "UserCards-1.mcd")
        assert os.path.isfile(f_user), "SLUS-00421-final deve migrar para UserCards"
    finally:
        shutil.rmtree(td, ignore_errors=True)

def test_17_channel_9_updates_max_channels_in_ini():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        with open(os.path.join(td, "SLUS-00421-9.mc"), "wb") as f: f.write(make_blank_card(9))

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        subprocess.run(cmd, check=True)

        target = os.path.join(td, "MemoryCards", "PS1", "SLUS-00421", "SLUS-00421-9.mcd")
        assert os.path.isfile(target), "SLUS-00421-9.mcd deve permanecer no canal 9"
        ini = os.path.join(td, "MemoryCards", "PS1", "SLUS-00421", "SLUS-00421.ini")
        assert os.path.isfile(ini), "INI não criado"
        with open(ini, "r") as f:
            c = f.read()
            assert "MaxChannels = 9" in c or "MaxChannels=9" in c, "MaxChannels não foi atualizado para 9"
    finally:
        shutil.rmtree(td, ignore_errors=True)

class FakeOwmcHandler(BaseHTTPRequestHandler):
    root_dir = ""

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        qs = urllib.parse.parse_qs(parsed.query)

        if parsed.path == "/api/unload":
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
            return

        elif parsed.path == "/api/fs/put":
            rel_path = qs.get("path", [""])[0]
            length = int(self.headers.get("Content-Length", 0))
            data = self.rfile.read(length)
            full_path = os.path.join(self.root_dir, rel_path)
            os.makedirs(os.path.dirname(full_path), exist_ok=True)
            with open(full_path, "wb") as f:
                f.write(data)
            self.send_response(201)
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
            return

        elif parsed.path == "/api/fs/delete":
            rel_path = qs.get("path", [""])[0]
            full_path = os.path.join(self.root_dir, rel_path)
            if os.path.isfile(full_path):
                os.remove(full_path)
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"status":"ok"}')
            elif os.path.isdir(full_path):
                try:
                    os.rmdir(full_path)
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(b'{"status":"ok"}')
                except OSError:
                    self.send_response(400)
                    self.end_headers()
                    self.wfile.write(b'{"status":"error","message":"Directory not empty"}')
            else:
                self.send_response(404)
                self.end_headers()
            return

        self.send_response(404)
        self.end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        qs = urllib.parse.parse_qs(parsed.query)

        if parsed.path == "/api/status":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            has_legacy = os.path.isdir(os.path.join(self.root_dir, "games")) or os.path.isfile(os.path.join(self.root_dir, "default.mc"))
            resp = json.dumps({"version":"1.5.0", "legacy":{"detected": has_legacy}}).encode("utf-8")
            self.wfile.write(resp)
            return

        elif parsed.path == "/api/fs/list":
            rel_path = qs.get("path", [""])[0]
            full_path = os.path.join(self.root_dir, rel_path) if rel_path else self.root_dir
            if not os.path.isdir(full_path):
                self.send_response(404)
                self.end_headers()
                return
            entries = []
            for e in os.scandir(full_path):
                entries.append({"name": e.name, "is_dir": e.is_dir(), "size": e.stat().st_size if not e.is_dir() else 0})
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"entries": entries}).encode("utf-8"))
            return

        else:
            rel_path = parsed.path.lstrip("/")
            full_path = os.path.join(self.root_dir, rel_path)
            if os.path.isfile(full_path):
                with open(full_path, "rb") as f:
                    data = f.read()
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            else:
                self.send_response(404)
                self.end_headers()

    def log_message(self, format, *args):
        pass

def test_18_http_transport_full_migration():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("wifi=1")
        with open(os.path.join(td, "default.mc"), "wb") as f: f.write(make_blank_card(1))
        with open(os.path.join(td, "SLUS-00421.mc"), "wb") as f: f.write(make_blank_card(2))

        FakeOwmcHandler.root_dir = td
        server = HTTPServer(("127.0.0.1", 0), FakeOwmcHandler)
        port = server.server_port
        t = threading.Thread(target=server.serve_forever, daemon=True)
        t.start()

        url = f"http://127.0.0.1:{port}"
        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", url, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        server.shutdown()
        server.server_close()

        assert res.returncode == 0, f"HTTP migration falhou: {res.stderr.decode()}"

        assert os.path.isfile(os.path.join(td, "MemoryCards", "PS1", "BOOT", "BootCard-1.mcd")), "BootCard-1.mcd não criado via HTTP"
        assert os.path.isfile(os.path.join(td, "MemoryCards", "PS1", "SLUS-00421", "SLUS-00421-1.mcd")), "SLUS-00421-1.mcd não criado via HTTP"
        assert not os.path.isfile(os.path.join(td, "default.mc")), "default.mc não foi removido após migração HTTP"
        assert not os.path.isfile(os.path.join(td, "SLUS-00421.mc")), "SLUS-00421.mc não foi removido após migração HTTP"
    finally:
        shutil.rmtree(td, ignore_errors=True)

# Teste Adicional 19: default.mc em conflito vai para quarentena (Regra 2 do §18.1)
def test_19_default_mc_conflict_quarantine():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        boot_dir = os.path.join(td, "MemoryCards", "PS1", "BOOT")
        os.makedirs(boot_dir, exist_ok=True)
        boot_path = os.path.join(boot_dir, "BootCard-1.mcd")
        # Já existe BootCard-1.mcd com conteúdo X
        with open(boot_path, "wb") as f: f.write(make_blank_card(0xAA))
        # default.mc tem conteúdo diferente Y
        with open(os.path.join(td, "default.mc"), "wb") as f: f.write(make_blank_card(0xBB))

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        assert "CONFLITO" in res.stdout.decode()

        # BootCard-1.mcd original NÃO pode ser sobrescrito
        with open(boot_path, "rb") as f:
            assert f.read() == make_blank_card(0xAA), "BootCard-1.mcd foi indevidamente sobrescrito!"

        # default.mc deve ir para quarentena
        trash = os.path.join(td, ".sd2psx", "migrate-trash")
        assert any("default.mc" in f for _, _, files in os.walk(trash) for f in files)
    finally:
        shutil.rmtree(td, ignore_errors=True)

# Teste Adicional 20: merge-policy merge funde dois cartões do mesmo jogo em canal 1
def test_20_merge_policy_merge_flag():
    td = tempfile.mkdtemp()
    try:
        with open(os.path.join(td, "config.txt"), "w") as f: f.write("ok")
        # Cria card A com save 1
        card_a = bytearray(make_blank_card())
        write_save_to_card(card_a, start_block=1, num_blocks=1, name="SAVE_A", pattern=b"A_DATA")

        # Cria card B com save 2
        card_b = bytearray(make_blank_card())
        write_save_to_card(card_b, start_block=1, num_blocks=1, name="SAVE_B", pattern=b"B_DATA")

        with open(os.path.join(td, "SLUS-00421-1.mc"), "wb") as f: f.write(card_a)
        with open(os.path.join(td, "SLUS-00421-2.mc"), "wb") as f: f.write(card_b)

        cmd = [PYTHON_EXE, SCRIPT_PATH, "--root", td, "--merge-policy", "merge", "--apply", "--yes", "--parent-db", PARENT_DB_PATH]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        assert "MERGE_OK" in res.stdout.decode()

        # Destino final é SLUS-00421-1.mcd com ambos os saves
        final_card = os.path.join(td, "MemoryCards", "PS1", "SLUS-00421", "SLUS-00421-1.mcd")
        assert os.path.isfile(final_card)
        with open(final_card, "rb") as f:
            data = f.read()
            assert b"SAVE_A" in data and b"SAVE_B" in data, "Cartão fundido não contém os dois saves"

        # Originais foram para quarentena
        trash = os.path.join(td, ".sd2psx", "migrate-trash")
        assert any("SLUS-00421-1.mc" in f for _, _, files in os.walk(trash) for f in files)
        assert any("SLUS-00421-2.mc" in f for _, _, files in os.walk(trash) for f in files)
    finally:
        shutil.rmtree(td, ignore_errors=True)

# ----------------- Runner Principal -----------------

def main():
    print("Iniciando bateria completa de testes de aceitação do migrate_sd.py...")
    tests = [
        ("Test 1: Dry-run zero byte change", test_1_dry_run_zero_byte_change),
        ("Test 2: Apply RE2 unified & verify code 0", test_2_apply_re2_unified_and_verify),
        ("Test 3: Second apply nothing to migrate", test_3_second_apply_nothing_to_migrate),
        ("Test 4: Verify before apply returns 2", test_4_verify_before_apply_returns_2),
        ("Test 5: Multidisc compiled vs custom", test_5_multidisc_compiled_and_custom),
        ("Test 6: Collision different content picks next channel", test_6_collision_different_content_picks_next_channel),
        ("Test 7: Duplicate sent to quarantine", test_7_exact_duplicate_sent_to_quarantine),
        ("Test 8: Empty root fails sanity code 3", test_8_empty_root_fails_sanity_code_3),
        ("Test 9: UserCards renaming and INI update", test_9_usercards_renaming),
        ("Test 10: Corrupt size file sent to quarantine", test_10_corrupt_size_file_sent_to_quarantine),
        ("Test 11: Apply without --yes non-tty code 3", test_11_apply_without_yes_non_tty_code_3),
        ("Test 12: Merge cards synthetic success", test_12_merge_cards_synthetic_success),
        ("Test 13: Merge cards conflict rejected", test_13_merge_cards_conflict_rejected),
        ("Test 14: Merge cards OUT exists refused", test_14_merge_cards_out_exists_refused),
        ("Test 15: Three cards exact channels preserved", test_15_three_cards_exact_channels_preserved),
        ("Test 16: Channel zero and non-numeric suffix", test_16_channel_zero_and_non_numeric_suffix),
        ("Test 17: Channel 9 updates MaxChannels in INI", test_17_channel_9_updates_max_channels_in_ini),
        ("Test 18: Fake HTTP transport full migration", test_18_http_transport_full_migration),
        ("Test 19: default.mc conflict quarantine (D2/§18.1)", test_19_default_mc_conflict_quarantine),
        ("Test 20: --merge-policy merge implementation", test_20_merge_policy_merge_flag),
    ]

    passed = 0
    for name, fn in tests:
        print(f"  Executando {name}...", end=" ", flush=True)
        fn()
        print("PASS")
        passed += 1

    print(f"\nTodos os {passed} testes de aceitação passaram com sucesso absoluto.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
