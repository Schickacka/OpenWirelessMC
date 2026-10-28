#!/usr/bin/env python3
"""
tools/migrate_sd.py - Migração segura de layout do cartão SD para OpenWirelessMC 1.5.
Normativo: Seção 18 do dossiê OWMC.
Apenas Python 3 stdlib.
"""

import os
import sys
import shutil
import hashlib
import json
import re
import argparse
import tempfile
import time
import urllib.request
import urllib.error
import urllib.parse
from datetime import datetime
from collections import Counter
import struct

CARD_SIZE_BYTES = 131072
FRAME_SIZE = 128
BLOCK_SIZE = 8192
NUM_BLOCKS = 16
NUM_FRAMES = 1024

SANITY_FILES = [
    "config.txt",
    "titles.db",
    "games",
    "games/",
    "default.mc",
    "multidisc.txt",
    "Game2Folder.ini",
    "MemoryCards",
    "MemoryCards/",
]

def xor_checksum(frame: bytes) -> int:
    chk = 0
    for b in frame[:127]:
        chk ^= b
    return chk

def validate_card_frames(data: bytes) -> bool:
    if len(data) != CARD_SIZE_BYTES:
        return False
    if data[0:2] != b"MC":
        return False
    for f_idx in range(NUM_FRAMES):
        f = data[f_idx * FRAME_SIZE : (f_idx + 1) * FRAME_SIZE]
        if f[127] != xor_checksum(f):
            return False
    return True

class SaveEntry:
    def __init__(self, block_idx: int, chain: list, state: int, next_block: int, size: int, name: bytes, raw_payload: bytes):
        self.block_idx = block_idx
        self.chain = chain
        self.state = state
        self.next_block = next_block
        self.size = size
        self.name = name
        self.raw_payload = raw_payload
        self.save_id = name.strip(b"\x00") if name.strip(b"\x00") else raw_payload[:128].hex()

def parse_card_saves(data: bytes):
    if not validate_card_frames(data):
        return None

    used_blocks = set()
    saves = []

    for b in range(1, NUM_BLOCKS):
        dir_off = b * FRAME_SIZE
        dir_frame = data[dir_off : dir_off + FRAME_SIZE]
        state_byte = dir_frame[0]

        if state_byte == 0x51:
            size = struct.unpack("<I", dir_frame[4:8])[0]
            next_b = struct.unpack("<H", dir_frame[8:10])[0]
            name = bytes(dir_frame[10:74])

            chain = [b]
            cur = b
            cur_next = next_b

            while cur_next != 0xFFFF:
                if cur_next < 1 or cur_next >= NUM_BLOCKS or cur_next in chain:
                    return None
                chain.append(cur_next)
                cur = cur_next
                next_dir = data[cur * FRAME_SIZE : (cur + 1) * FRAME_SIZE]
                next_state = next_dir[0]
                cur_next = struct.unpack("<H", next_dir[8:10])[0]
                if cur_next == 0xFFFF:
                    if next_state != 0x53:
                        return None
                else:
                    if next_state != 0x52:
                        return None

            for blk in chain:
                if blk in used_blocks:
                    return None
                used_blocks.add(blk)

            payload_parts = [data[blk * BLOCK_SIZE : (blk + 1) * BLOCK_SIZE] for blk in chain]
            full_payload = b"".join(payload_parts)

            entry = SaveEntry(b, chain, state_byte, next_b, size, name, full_payload)
            saves.append((entry, chain))

        elif state_byte in (0x52, 0x53):
            pass
        elif state_byte in (0xA0, 0xA1, 0xA2, 0xA3):
            pass
        else:
            pass

    return saves

def merge_card_images(data_a: bytes, data_b: bytes) -> bytes:
    saves_a = parse_card_saves(data_a)
    saves_b = parse_card_saves(data_b)
    if saves_a is None or saves_b is None:
        return None

    a_by_id = {s.save_id: (s, chain) for s, chain in saves_a}
    b_to_add = []

    for s_b, chain_b in saves_b:
        if s_b.save_id in a_by_id:
            s_a, _ = a_by_id[s_b.save_id]
            if s_a.raw_payload == s_b.raw_payload:
                continue
            else:
                return None
        else:
            b_to_add.append((s_b, chain_b))

    if not b_to_add:
        return bytes(data_a)

    used_blocks_a = set()
    for _, chain in saves_a:
        for blk in chain:
            used_blocks_a.add(blk)

    free_blocks = [blk for blk in range(1, NUM_BLOCKS) if blk not in used_blocks_a]
    total_blocks_needed = sum(len(chain) for _, chain in b_to_add)
    if len(free_blocks) < total_blocks_needed:
        return None

    out = bytearray(data_a)
    free_idx = 0

    for s_b, chain_b in b_to_add:
        n_blks = len(chain_b)
        new_chain = free_blocks[free_idx : free_idx + n_blks]
        free_idx += n_blks

        for i, old_blk in enumerate(chain_b):
            new_blk = new_chain[i]
            src_off = old_blk * BLOCK_SIZE
            dst_off = new_blk * BLOCK_SIZE
            out[dst_off : dst_off + BLOCK_SIZE] = data_b[src_off : src_off + BLOCK_SIZE]

            src_dir_off = old_blk * FRAME_SIZE
            dst_dir_off = new_blk * FRAME_SIZE
            dir_entry = bytearray(data_b[src_dir_off : src_dir_off + FRAME_SIZE])

            if n_blks == 1:
                state = 0x51
                next_b = 0xFFFF
            elif i == 0:
                state = 0x51
                next_b = new_chain[1]
            elif i == n_blks - 1:
                state = 0x53
                next_b = 0xFFFF
            else:
                state = 0x52
                next_b = new_chain[i + 1]

            dir_entry[0x00] = state
            dir_entry[0x01:0x04] = b"\x00\x00\x00"
            dir_entry[0x08:0x0A] = struct.pack("<H", next_b)
            dir_entry[127] = xor_checksum(bytes(dir_entry[:127]))

            out[dst_dir_off : dst_dir_off + FRAME_SIZE] = dir_entry

    verified = parse_card_saves(bytes(out))
    if verified is None:
        return None

    v_by_id = {s.save_id: s for s, _ in verified}
    for s_a, _ in saves_a:
        if s_a.save_id not in v_by_id or v_by_id[s_a.save_id].raw_payload != s_a.raw_payload:
            return None
    for s_b, _ in b_to_add:
        if s_b.save_id not in v_by_id or v_by_id[s_b.save_id].raw_payload != s_b.raw_payload:
            return None

    return bytes(out)

class LocalTransport:
    def __init__(self, root_dir: str):
        self.root_dir = os.path.abspath(root_dir)

    def sanity_check(self) -> bool:
        if not os.path.isdir(self.root_dir):
            return False
        for f in SANITY_FILES:
            if os.path.exists(os.path.join(self.root_dir, f)):
                return True
        return False

    def check_safe_path(self, rel_path: str):
        norm = os.path.normpath(os.path.join(self.root_dir, rel_path))
        if not norm.startswith(self.root_dir):
            raise PermissionError(f"Caminho tenta escapar do root: {rel_path}")
        return norm

    def exists(self, rel_path: str) -> bool:
        p = self.check_safe_path(rel_path)
        return os.path.exists(p)

    def is_dir(self, rel_path: str) -> bool:
        p = self.check_safe_path(rel_path)
        return os.path.isdir(p)

    def get_size(self, rel_path: str) -> int:
        p = self.check_safe_path(rel_path)
        return os.path.getsize(p)

    def read_bytes(self, rel_path: str) -> bytes:
        p = self.check_safe_path(rel_path)
        with open(p, "rb") as f:
            return f.read()

    def write_bytes(self, rel_path: str, data: bytes):
        p = self.check_safe_path(rel_path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
            f.flush()
            try:
                os.fsync(f.fileno())
            except OSError:
                pass

    def move(self, src_rel: str, dst_rel: str):
        src_p = self.check_safe_path(src_rel)
        dst_p = self.check_safe_path(dst_rel)
        os.makedirs(os.path.dirname(dst_p), exist_ok=True)
        os.replace(src_p, dst_p)

    def delete_file(self, rel_path: str):
        p = self.check_safe_path(rel_path)
        if os.path.isfile(p):
            os.remove(p)

    def rmdir_empty(self, rel_path: str) -> bool:
        p = self.check_safe_path(rel_path)
        if os.path.isdir(p):
            try:
                os.rmdir(p)
                return True
            except OSError:
                return False
        return False

    def list_dir(self, rel_path: str):
        p = self.check_safe_path(rel_path)
        if not os.path.exists(p) or not os.path.isdir(p):
            return []
        entries = []
        for e in os.scandir(p):
            entries.append({"name": e.name, "is_dir": e.is_dir(), "size": e.stat().st_size if not e.is_dir() else 0})
        return entries

    def walk_all_files(self):
        res = []
        for root, dirs, files in os.walk(self.root_dir):
            for f in files:
                full = os.path.join(root, f)
                rel = os.path.relpath(full, self.root_dir).replace("\\", "/")
                res.append(rel)
        return res

class HttpTransport:
    def __init__(self, base_url: str):
        self.base_url = base_url.rstrip("/")

    def _req(self, endpoint: str, data=None, method=None):
        url = f"{self.base_url}/{endpoint.lstrip('/')}"
        req = urllib.request.Request(url, data=data, method=method)
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                return resp.getcode(), resp.read()
        except urllib.error.HTTPError as e:
            if e.code == 409 and (data is not None or method in ("POST", "DELETE")):
                # Transferência ocupada: aguarda 2s e repete uma vez
                time.sleep(2)
                try:
                    req_retry = urllib.request.Request(url, data=data, method=method)
                    with urllib.request.urlopen(req_retry, timeout=10) as resp2:
                        return resp2.getcode(), resp2.read()
                except urllib.error.HTTPError as e2:
                    return e2.code, e2.read()
            return e.code, e.read()

    def unload_mounted_card(self):
        self._req("/api/unload", data=b"", method="POST")

    def sanity_check(self) -> bool:
        code, body = self._req("/api/status", method="GET")
        if code == 200:
            return True
        code, body = self._req("/api/fs/list?path=", method="GET")
        return code == 200

    def exists(self, rel_path: str) -> bool:
        dir_name = os.path.dirname(rel_path).replace("\\", "/")
        base_name = os.path.basename(rel_path)
        entries = self.list_dir(dir_name)
        return any(e["name"] == base_name for e in entries)

    def is_dir(self, rel_path: str) -> bool:
        dir_name = os.path.dirname(rel_path).replace("\\", "/")
        base_name = os.path.basename(rel_path)
        entries = self.list_dir(dir_name)
        for e in entries:
            if e["name"] == base_name and e.get("is_dir"):
                return True
        return False

    def get_size(self, rel_path: str) -> int:
        dir_name = os.path.dirname(rel_path).replace("\\", "/")
        base_name = os.path.basename(rel_path)
        entries = self.list_dir(dir_name)
        for e in entries:
            if e["name"] == base_name:
                return e.get("size", 0)
        return 0

    def read_bytes(self, rel_path: str) -> bytes:
        code, body = self._req(rel_path, method="GET")
        if code != 200:
            raise IOError(f"HTTP GET {rel_path} falhou com código {code}")
        return body

    def write_bytes(self, rel_path: str, data: bytes):
        endpoint = f"/api/fs/put?path={urllib.parse.quote(rel_path)}&overwrite=1"
        code, resp = self._req(endpoint, data=data, method="POST")
        if code not in (200, 201):
            raise IOError(f"HTTP PUT {rel_path} falhou com código {code}: {resp}")

    def move(self, src_rel: str, dst_rel: str):
        data = self.read_bytes(src_rel)
        src_sha = hashlib.sha256(data).hexdigest()
        self.write_bytes(dst_rel, data)
        data_back = self.read_bytes(dst_rel)
        dst_sha = hashlib.sha256(data_back).hexdigest()
        if src_sha != dst_sha:
            raise IOError(f"Falha de integridade SHA256 ao mover {src_rel} -> {dst_rel}")
        self.delete_file(src_rel)

    def delete_file(self, rel_path: str):
        endpoint = f"/api/fs/delete?path={urllib.parse.quote(rel_path)}"
        code, resp = self._req(endpoint, data=b"", method="POST")
        if code not in (200, 204):
            raise IOError(f"HTTP DELETE {rel_path} falhou com código {code}: {resp}")

    def rmdir_empty(self, rel_path: str) -> bool:
        endpoint = f"/api/fs/delete?path={urllib.parse.quote(rel_path)}"
        code, resp = self._req(endpoint, data=b"", method="POST")
        return code == 200

    def list_dir(self, rel_path: str):
        endpoint = f"/api/fs/list?path={urllib.parse.quote(rel_path)}"
        code, body = self._req(endpoint, method="GET")
        if code != 200:
            return []
        try:
            data = json.loads(body.decode("utf-8"))
            if isinstance(data, dict):
                return data.get("entries", [])
            elif isinstance(data, list):
                return data
            return []
        except Exception:
            return []

    def walk_all_files(self):
        result = []
        def _recurse(d):
            entries = self.list_dir(d)
            for e in entries:
                sub = f"{d}/{e['name']}".lstrip("/")
                if e.get("dir", False) or e.get("is_dir", False):
                    _recurse(sub)
                else:
                    result.append(sub)
        _recurse("")
        return result

    def get_legacy_detected(self) -> bool:
        code, body = self._req("/api/status", method="GET")
        if code == 200:
            try:
                st = json.loads(body.decode("utf-8"))
                legacy = st.get("legacy", {})
                return legacy.get("detected", False)
            except Exception:
                pass
        return False

GAMEID_REGEX = re.compile(r"^([A-Za-z]{4})[-_]?([0-9]{3})\.?([0-9]{2})$")

def normalize_game_id(stem: str):
    m = GAMEID_REGEX.match(stem)
    if m:
        prefix = m.group(1).upper()
        num1 = m.group(2)
        num2 = m.group(3)
        return f"{prefix}-{num1}{num2}"
    return None

def parse_card_stem(stem: str):
    if normalize_game_id(stem):
        return stem, 1, False, None

    m = re.match(r"^(.*?)-([0-9]+)$", stem)
    if m:
        base = m.group(1)
        num = int(m.group(2))
        if num == 0:
            return base, 1, False, "CANAL_ZERO"
        elif num > 254:
            return base, None, False, "CANAL_ALTO"
        else:
            return base, num, True, None
    return stem, 1, False, None

def load_parent_db(path: str):
    db = {}
    if not os.path.isfile(path):
        return db
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            if "=" in line:
                k, v = line.split("=", 1)
                db[k.strip().upper()] = v.strip().upper()
    return db

def load_ini_keys(content_str: str):
    res = {}
    cur_sec = ""
    for line in content_str.splitlines():
        line = line.split(";")[0].split("#")[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            cur_sec = line[1:-1].strip()
        elif "=" in line:
            k, v = line.split("=", 1)
            res[(cur_sec, k.strip())] = v.strip()
    return res

def insert_keys_into_ini(ini_content: str, target_sec: str, new_keys_dict: dict) -> str:
    lines = ini_content.splitlines()
    sec_tag = f"[{target_sec}]"
    sec_idx = -1
    for i, line in enumerate(lines):
        if line.strip() == sec_tag:
            sec_idx = i
            break

    add_lines = [f"{k} = {v}" for k, v in new_keys_dict.items()]
    if sec_idx != -1:
        next_sec_idx = len(lines)
        for i in range(sec_idx + 1, len(lines)):
            if lines[i].strip().startswith("[") and lines[i].strip().endswith("]"):
                next_sec_idx = i
                break
        lines[next_sec_idx:next_sec_idx] = add_lines
    else:
        if lines and lines[-1].strip():
            lines.append("")
        lines.append(sec_tag)
        lines.extend(add_lines)

    return "\n".join(lines) + "\n"

class MigrationPlanner:
    def __init__(self, transport, parent_db, merge_policy="keep"):
        self.t = transport
        self.parent_db = parent_db
        self.merge_policy = merge_policy
        self.plan = []
        self.game2folder = {}
        self.folder_max_channels = {}
        self.allocated_channels = {}
        self.allocated_sources = {}

    def init_game2folder(self):
        ini_path = ".sd2psx/Game2Folder.ini"
        if self.t.exists(ini_path):
            try:
                data = self.t.read_bytes(ini_path).decode("utf-8", errors="ignore")
                for (sec, k), v in load_ini_keys(data).items():
                    if sec in ("", "PS1"):
                        self.game2folder[k] = v
            except Exception:
                pass

    def get_max_channels(self, folder: str) -> int:
        if folder in self.folder_max_channels:
            return self.folder_max_channels[folder]
        ini_path = f"MemoryCards/PS1/{folder}/{folder}.ini"
        max_ch = 8
        if self.t.exists(ini_path):
            try:
                data = self.t.read_bytes(ini_path).decode("utf-8", errors="ignore")
                keys = load_ini_keys(data)
                for (sec, k), v in keys.items():
                    if k.lower() == "maxchannels":
                        max_ch = int(v)
            except Exception:
                pass
        self.folder_max_channels[folder] = max_ch
        return max_ch

    def scan(self, timestamp_str: str):
        self.init_game2folder()
        all_files = sorted(self.t.walk_all_files())
        quarantine_root = f".sd2psx/migrate-trash/{timestamp_str}"

        # 1. default.mc (Regra 2 do §18.1: nunca sobrescreve)
        if "default.mc" in all_files:
            boot_target = "MemoryCards/PS1/BOOT/BootCard-1.mcd"
            if self.t.exists(boot_target):
                src_b = self.t.read_bytes("default.mc")
                dst_b = self.t.read_bytes(boot_target)
                if hashlib.sha256(src_b).digest() == hashlib.sha256(dst_b).digest():
                    self.plan.append({
                        "action": "DUPLICATA",
                        "src": "default.mc",
                        "dst": f"{quarantine_root}/default.mc",
                        "note": "DUPLICATA"
                    })
                else:
                    self.plan.append({
                        "action": "QUARENTENA",
                        "src": "default.mc",
                        "dst": f"{quarantine_root}/default.mc",
                        "note": "CONFLITO"
                    })
            else:
                self.plan.append({
                    "action": "MOVE",
                    "src": "default.mc",
                    "dst": boot_target,
                    "note": "BOOT_CARD"
                })

        # 2. Varredura de cartões
        card_exts = (".mc", ".mcd", ".mcr")
        card_candidates = []

        for rel in all_files:
            if rel in ("default.mc", "multidisc.txt", "Game2Folder.ini", "config.txt", "titles.db"):
                continue
            if rel.startswith(".sd2psx/") or rel.startswith("MemoryCards/") or rel.startswith("System Volume Information"):
                continue

            parts = rel.split("/")
            if len(parts) > 1 and parts[0] != "games":
                continue

            fname = parts[-1]
            stem, ext = os.path.splitext(fname)
            ext_lower = ext.lower()
            sz = self.t.get_size(rel)

            if ext_lower not in card_exts or sz != CARD_SIZE_BYTES:
                self.plan.append({
                    "action": "QUARENTENA",
                    "src": rel,
                    "dst": f"{quarantine_root}/{rel}",
                    "note": "ERRO_EXT_OU_TAMANHO"
                })
                continue

            base_stem, channel, is_explicit, note = parse_card_stem(stem)
            norm_id = normalize_game_id(base_stem)

            if norm_id:
                pai = self.parent_db.get(norm_id, norm_id)
                folder = self.game2folder.get(pai, pai)
                if not re.match(r"^[A-Za-z0-9._-]+$", folder) or len(folder) > 31:
                    self.plan.append({
                        "action": "QUARENTENA",
                        "src": rel,
                        "dst": f"{quarantine_root}/{rel}",
                        "note": "PASTA_INVALIDA"
                    })
                    continue
                card_name = None
            else:
                folder = "UserCards"
                card_name = base_stem.replace("_", " ").replace("-", " ").strip()

            card_candidates.append({
                "rel": rel,
                "folder": folder,
                "channel": channel,
                "is_explicit": is_explicit,
                "card_name": card_name,
                "note": note
            })

        # Política de fusão (--merge-policy merge)
        if self.merge_policy == "merge":
            by_folder = {}
            for c in card_candidates:
                by_folder.setdefault(c["folder"], []).append(c)

            merged_candidates = []
            for fld, items in by_folder.items():
                if len(items) >= 2 and fld != "UserCards":
                    c_a = items[0]
                    c_b = items[1]
                    data_a = self.t.read_bytes(c_a["rel"])
                    data_b = self.t.read_bytes(c_b["rel"])
                    fused = merge_card_images(data_a, data_b)
                    if fused is not None:
                        target_m = f"MemoryCards/PS1/{fld}/{fld}-1.mcd"
                        self.plan.append({
                            "action": "MERGE_WRITE",
                            "src": c_a["rel"],
                            "src_b": c_b["rel"],
                            "dst": target_m,
                            "fused_data": fused,
                            "folder": fld,
                            "channel": 1,
                            "note": "MERGE_OK"
                        })
                        self.plan.append({
                            "action": "QUARENTENA",
                            "src": c_a["rel"],
                            "dst": f"{quarantine_root}/{c_a['rel']}",
                            "note": "ORIGINAL_A_QUARENTENA"
                        })
                        self.plan.append({
                            "action": "QUARENTENA",
                            "src": c_b["rel"],
                            "dst": f"{quarantine_root}/{c_b['rel']}",
                            "note": "ORIGINAL_B_QUARENTENA"
                        })
                        self.allocated_channels.setdefault(fld, set()).add(1)
                        for it in items[2:]:
                            merged_candidates.append(it)
                        continue
                for it in items:
                    merged_candidates.append(it)
            card_candidates = merged_candidates

        # Duas passadas determinísticas
        pass1_cards = [c for c in card_candidates if c["is_explicit"]]
        pass2_cards = [c for c in card_candidates if not c["is_explicit"]]

        pass1_cards.sort(key=lambda c: (c["folder"], c["channel"], c["rel"]))
        pass2_cards.sort(key=lambda c: (c["folder"], c["rel"]))

        # PASSADA 1: Canais explícitos 1..254
        for c in pass1_cards:
            folder = c["folder"]
            rel = c["rel"]
            ch = c["channel"]
            note = c["note"]
            card_name = c["card_name"]

            if folder not in self.allocated_channels:
                self.allocated_channels[folder] = set()

            max_ch = self.get_max_channels(folder)
            if ch > max_ch:
                note = "CANAL_ALTO"

            target_rel = f"MemoryCards/PS1/{folder}/{folder}-{ch}.mcd"
            if ch in self.allocated_channels[folder] or self.t.exists(target_rel):
                is_dupe = False
                src_bytes = self.t.read_bytes(rel)
                src_hash = hashlib.sha256(src_bytes).digest()

                if self.t.exists(target_rel):
                    dst_bytes = self.t.read_bytes(target_rel)
                    if src_hash == hashlib.sha256(dst_bytes).digest():
                        is_dupe = True
                elif (folder, ch) in self.allocated_sources:
                    prev_src = self.allocated_sources[(folder, ch)]
                    prev_bytes = self.t.read_bytes(prev_src)
                    if src_hash == hashlib.sha256(prev_bytes).digest():
                        is_dupe = True

                if is_dupe:
                    self.plan.append({
                        "action": "DUPLICATA",
                        "src": rel,
                        "dst": f"{quarantine_root}/{rel}",
                        "note": "DUPLICATA"
                    })
                    continue
                else:
                    ch_next = ch + 1
                    found_free = False
                    while ch_next <= max_ch:
                        cand_rel = f"MemoryCards/PS1/{folder}/{folder}-{ch_next}.mcd"
                        if ch_next not in self.allocated_channels[folder] and not self.t.exists(cand_rel):
                            ch = ch_next
                            target_rel = cand_rel
                            found_free = True
                            break
                        ch_next += 1

                    if not found_free:
                        self.plan.append({
                            "action": "QUARENTENA",
                            "src": rel,
                            "dst": f"{quarantine_root}/{rel}",
                            "note": "SEM_CANAL"
                        })
                        continue

            self.allocated_channels[folder].add(ch)
            self.allocated_sources[(folder, ch)] = rel
            self.plan.append({
                "action": "MOVE",
                "src": rel,
                "dst": target_rel,
                "folder": folder,
                "channel": ch,
                "card_name": card_name,
                "note": note or "OK"
            })

        # PASSADA 2: Sem canal explícito / -0 / >254
        for c in pass2_cards:
            folder = c["folder"]
            rel = c["rel"]
            card_name = c["card_name"]
            note = c["note"]

            if folder not in self.allocated_channels:
                self.allocated_channels[folder] = set()

            max_ch = self.get_max_channels(folder)
            desired_ch = c["channel"] if c["channel"] is not None else 1

            assigned_ch = desired_ch
            target_rel = f"MemoryCards/PS1/{folder}/{folder}-{assigned_ch}.mcd"

            if assigned_ch in self.allocated_channels[folder] or self.t.exists(target_rel):
                is_dupe = False
                src_bytes = self.t.read_bytes(rel)
                src_hash = hashlib.sha256(src_bytes).digest()

                if self.t.exists(target_rel):
                    dst_bytes = self.t.read_bytes(target_rel)
                    if src_hash == hashlib.sha256(dst_bytes).digest():
                        is_dupe = True
                elif (folder, assigned_ch) in self.allocated_sources:
                    prev_src = self.allocated_sources[(folder, assigned_ch)]
                    prev_bytes = self.t.read_bytes(prev_src)
                    if src_hash == hashlib.sha256(prev_bytes).digest():
                        is_dupe = True

                if is_dupe:
                    self.plan.append({
                        "action": "DUPLICATA",
                        "src": rel,
                        "dst": f"{quarantine_root}/{rel}",
                        "note": "DUPLICATA"
                    })
                    continue
                else:
                    found_ch = None
                    scan_ch = 1
                    while scan_ch <= max_ch:
                        cand_rel = f"MemoryCards/PS1/{folder}/{folder}-{scan_ch}.mcd"
                        if scan_ch not in self.allocated_channels[folder] and not self.t.exists(cand_rel):
                            found_ch = scan_ch
                            break
                        scan_ch += 1

                    if found_ch is None:
                        found_ch = max(self.allocated_channels[folder], default=max_ch) + 1
                        note = "CANAL_ALTO"

                    assigned_ch = found_ch
                    target_rel = f"MemoryCards/PS1/{folder}/{folder}-{assigned_ch}.mcd"

            self.allocated_channels[folder].add(assigned_ch)
            self.allocated_sources[(folder, assigned_ch)] = rel
            self.plan.append({
                "action": "MOVE",
                "src": rel,
                "dst": target_rel,
                "folder": folder,
                "channel": assigned_ch,
                "card_name": card_name,
                "note": note or "OK"
            })

        # 3. Game2Folder.ini da raiz (§18.5)
        if self.t.exists("Game2Folder.ini"):
            self.plan.append({
                "action": "GAME2FOLDER",
                "src": "Game2Folder.ini",
                "dst": ".sd2psx/Game2Folder.ini",
                "note": "GAME2FOLDER"
            })

        # 4. multidisc.txt (§18.4)
        if self.t.exists("multidisc.txt"):
            self.plan.append({
                "action": "MULTIDISC",
                "src": "multidisc.txt",
                "dst": "multidisc.txt.bak",
                "note": "MULTIDISC"
            })

    def execute(self, is_apply: bool, log_path: str = None, root_str: str = ""):
        if not self.plan:
            print("Nada a migrar: árvore já está no layout 1.5.")
            return {"OK": 0}, "Nada a migrar"

        resumo = Counter()
        log_lines = []

        now_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        log_lines.append(f"# OWMC Migration Manifest - {now_str}")
        log_lines.append(f"# Root: {root_str}")
        log_lines.append(f"# Active Parent DB pairs: {len(self.parent_db)}")
        log_lines.append("")

        for item in self.plan:
            action = item["action"]
            src = item["src"]
            dst = item["dst"]
            note = item["note"]

            resumo[note] += 1
            line = f"{note:<12} | {src} -> {dst}"
            log_lines.append(line)

            if not is_apply:
                continue

            if action in ("MOVE", "DUPLICATA", "QUARENTENA"):
                self.t.move(src, dst)

                folder = item.get("folder")
                ch = item.get("channel")
                card_name = item.get("card_name")

                if folder:
                    ini_rel = f"MemoryCards/PS1/{folder}/{folder}.ini"
                    ini_content = ""
                    if self.t.exists(ini_rel):
                        ini_content = self.t.read_bytes(ini_rel).decode("utf-8", errors="ignore")

                    modified = False
                    max_ch = self.get_max_channels(folder)
                    if ch and ch > max_ch:
                        if "[Settings]" not in ini_content:
                            ini_content = f"[Settings]\nMaxChannels = {ch}\n\n" + ini_content
                        else:
                            if re.search(r"MaxChannels\s*=\s*[0-9]+", ini_content):
                                ini_content = re.sub(r"MaxChannels\s*=\s*[0-9]+", f"MaxChannels = {ch}", ini_content)
                            else:
                                ini_content = ini_content.replace("[Settings]", f"[Settings]\nMaxChannels = {ch}")
                        self.folder_max_channels[folder] = ch
                        modified = True

                    if card_name and ch:
                        ch_entry = f"{ch} = {card_name}"
                        if "[ChannelName]" not in ini_content:
                            ini_content += f"\n[ChannelName]\n{ch_entry}\n"
                        else:
                            if re.search(rf"^\s*{ch}\s*=.*$", ini_content, re.MULTILINE):
                                ini_content = re.sub(rf"^\s*{ch}\s*=.*$", ch_entry, ini_content, flags=re.MULTILINE)
                            else:
                                ini_content = ini_content.replace("[ChannelName]", f"[ChannelName]\n{ch_entry}")
                        modified = True

                    if modified:
                        self.t.write_bytes(ini_rel, ini_content.encode("utf-8"))

            elif action == "MERGE_WRITE":
                fused_data = item["fused_data"]
                dst = item["dst"]
                self.t.write_bytes(dst, fused_data)

            elif action == "MULTIDISC":
                raw = self.t.read_bytes(src).decode("utf-8", errors="ignore")
                g2f_rel = ".sd2psx/Game2Folder.ini"
                g2f_content = ""
                if self.t.exists(g2f_rel):
                    g2f_content = self.t.read_bytes(g2f_rel).decode("utf-8", errors="ignore")

                new_keys = {}
                for l in raw.splitlines():
                    l = l.split("#")[0].strip()
                    if not l:
                        continue
                    if "=" in l:
                        child, parent = l.split("=", 1)
                        child = child.strip().upper()
                        parent = parent.strip().upper()
                        if child in self.parent_db:
                            if self.parent_db[child] == parent:
                                resumo["INERTE"] += 1
                            else:
                                resumo["CONFLITO"] += 1
                        else:
                            # Resolve pasta do PAI (§18.4)
                            parent_norm = normalize_game_id(parent) or parent
                            root_parent = self.parent_db.get(parent_norm, parent_norm)
                            resolved_folder = self.game2folder.get(root_parent, root_parent)
                            if f"{child}=" not in g2f_content and f"{child} =" not in g2f_content:
                                new_keys[child] = resolved_folder

                if new_keys:
                    g2f_content = insert_keys_into_ini(g2f_content, "PS1", new_keys)
                    self.t.write_bytes(g2f_rel, g2f_content.encode("utf-8"))

                bak_dst = "multidisc.txt.bak"
                idx = 1
                while self.t.exists(bak_dst):
                    bak_dst = f"multidisc.txt.bak.{idx}"
                    idx += 1
                self.t.move(src, bak_dst)

            elif action == "GAME2FOLDER":
                g2f_dst = ".sd2psx/Game2Folder.ini"
                raw_src = self.t.read_bytes(src).decode("utf-8", errors="ignore")
                if not self.t.exists(g2f_dst):
                    self.t.move(src, g2f_dst)
                else:
                    raw_dst = self.t.read_bytes(g2f_dst).decode("utf-8", errors="ignore")
                    src_keys = load_ini_keys(raw_src)
                    dst_keys = load_ini_keys(raw_dst)
                    to_insert_by_sec = {}
                    for (sec, k), v in src_keys.items():
                        target_sec = sec if sec else "PS1"
                        if (target_sec, k) not in dst_keys and ("", k) not in dst_keys and ("PS1", k) not in dst_keys:
                            if target_sec not in to_insert_by_sec:
                                to_insert_by_sec[target_sec] = {}
                            to_insert_by_sec[target_sec][k] = v
                        elif dst_keys.get((target_sec, k), dst_keys.get(("PS1", k))) != v:
                            resumo["CONFLITO"] += 1

                    for s, keys in to_insert_by_sec.items():
                        raw_dst = insert_keys_into_ini(raw_dst, s, keys)

                    self.t.write_bytes(g2f_dst, raw_dst.encode("utf-8"))

                    bak_dst = "Game2Folder.ini.bak"
                    idx = 1
                    while self.t.exists(bak_dst):
                        bak_dst = f"Game2Folder.ini.bak.{idx}"
                        idx += 1
                    self.t.move(src, bak_dst)

        log_content = "\n".join(log_lines) + "\n"
        if is_apply:
            ts_file = datetime.now().strftime("%Y%m%d-%H%M%S")
            manifest_path = f".sd2psx/migrate-{ts_file}.log"
            try:
                self.t.write_bytes(manifest_path, log_content.encode("utf-8"))
            except Exception:
                pass

        if log_path:
            with open(log_path, "w", encoding="utf-8") as f:
                f.write(log_content)
                f.flush()
                try:
                    os.fsync(f.fileno())
                except OSError:
                    pass

        return resumo, log_content

def run_self_test():
    tmpdir = tempfile.mkdtemp()
    try:
        empty_card = bytearray(CARD_SIZE_BYTES)
        empty_card[0:2] = b"MC"
        empty_card[127] = xor_checksum(empty_card[0:128])
        for f in range(1, NUM_FRAMES):
            off = f * FRAME_SIZE
            empty_card[off : off + FRAME_SIZE] = b"\xFF" * FRAME_SIZE

        saves = parse_card_saves(bytes(empty_card))
        assert saves is not None, "Falha ao validar cartão em branco"
        assert len(saves) == 0, f"Esperado 0 saves, obteve {len(saves)}"

        card_a = bytearray(empty_card)
        b1_off = 1 * BLOCK_SIZE
        card_a[b1_off] = 0x51
        card_a[b1_off + 1] = 0xFF
        payload_a = (b"SAVE_A_DATA" * 10).ljust(128, b"\0")
        card_a[b1_off + 0x80 : b1_off + 0x80 + 128] = payload_a
        for f in range(BLOCK_SIZE // FRAME_SIZE):
            f_off = b1_off + f * FRAME_SIZE
            card_a[f_off + 127] = xor_checksum(card_a[f_off : f_off + FRAME_SIZE])

        card_b = bytearray(empty_card)
        payload_b = (b"SAVE_B_DATA" * 10).ljust(128, b"\0")
        for i, blk in enumerate([1, 2, 3]):
            b_off = blk * BLOCK_SIZE
            if i == 0:
                card_b[b_off] = 0xA0
                card_b[b_off + 1] = 2
            elif i == 1:
                card_b[b_off] = 0x52
                card_b[b_off + 1] = 3
            else:
                card_b[b_off] = 0x53
                card_b[b_off + 1] = 0xFF
            card_b[b_off + 0x80 : b_off + 0x80 + 128] = payload_b
            for f in range(BLOCK_SIZE // FRAME_SIZE):
                f_off = b_off + f * FRAME_SIZE
                card_b[f_off + 127] = xor_checksum(card_b[f_off : f_off + FRAME_SIZE])

        saves_a = parse_card_saves(bytes(card_a))
        saves_b = parse_card_saves(bytes(card_b))
        assert len(saves_a) == 1, "Card A deve ter 1 save"
        assert len(saves_b) == 1, "Card B deve ter 1 save"

        out_bytes = merge_card_images(bytes(card_a), bytes(card_b))
        assert out_bytes is not None, "Fusão falhou"

        saves_out = parse_card_saves(out_bytes)
        assert len(saves_out) == 2, f"Esperado 2 saves após fusão, obteve {len(saves_out)}"
        assert validate_card_frames(out_bytes) is True, "Frames de saída inválidos"

        print("Self-test passed: 4 assertions verified cleanly.")
        return 0
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

def main():
    parser = argparse.ArgumentParser(description="OWMC SD Card Migration Tool")
    parser.add_argument("--root", help="Caminho local ou URL do OWMC")

    mode_group = parser.add_mutually_exclusive_group()
    mode_group.add_argument("--dry-run", action="store_true", help="Apenas simula a migração (default)")
    mode_group.add_argument("--apply", action="store_true", help="Executa a migração")

    parser.add_argument("--verify", action="store_true", help="Verifica se não há itens pendentes")
    parser.add_argument("--backup", help="Diretório de destino para backup completo")
    parser.add_argument("--parent-db", default="sdcard_assets/multidisc.txt", help="Caminho para multidisc.txt compilado")
    parser.add_argument("--merge-policy", choices=["keep", "merge"], default="keep", help="Política para múltiplos cartões")
    parser.add_argument("--log", help="Caminho para salvar manifesto localmente")
    parser.add_argument("--json", action="store_true", help="Saída em formato JSON")
    parser.add_argument("--yes", action="store_true", help="Confirmação não-interativa")
    parser.add_argument("--merge-cards", nargs=2, metavar=("A", "B"), help="Funde dois cartões")
    parser.add_argument("-o", "--output", help="Arquivo de saída para --merge-cards")
    parser.add_argument("--self-test", action="store_true", help="Roda testes unitários internos da fusão de cartões")

    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    if args.merge_cards:
        if not args.output:
            print("Erro: --output é obrigatório com --merge-cards", file=sys.stderr)
            return 1
        card_a_path, card_b_path = args.merge_cards
        if not os.path.exists(card_a_path) or not os.path.exists(card_b_path):
            print("Erro: Cartão de entrada inexistente", file=sys.stderr)
            return 1
        if os.path.exists(args.output):
            print(f"Erro de segurança: arquivo de saída já existe: {args.output}", file=sys.stderr)
            return 1

        with open(card_a_path, "rb") as fa, open(card_b_path, "rb") as fb:
            data_a = fa.read()
            data_b = fb.read()

        out = merge_card_images(data_a, data_b)
        if out is None:
            print("Erro: Falha na fusão dos cartões (conflito ou falta de blocos)", file=sys.stderr)
            return 1

        with open(args.output, "wb") as fo:
            fo.write(out)

        print(f"Fusão concluída com sucesso em {args.output}")
        return 0

    if not args.root:
        print("Erro de segurança: --root é obrigatório", file=sys.stderr)
        return 3

    if args.root.startswith("http://") or args.root.startswith("https://"):
        transport = HttpTransport(args.root)
    else:
        transport = LocalTransport(args.root)

    if not transport.sanity_check():
        print(f"Erro de segurança (§18.1): Checagem de sanidade falhou para --root {args.root}", file=sys.stderr)
        return 3

    if not os.path.isfile(args.parent_db):
        alt_db = os.path.join(os.path.dirname(__file__), "..", "sdcard_assets", "multidisc.txt")
        if os.path.isfile(alt_db):
            args.parent_db = alt_db
        else:
            print(f"Erro: parent-db {args.parent_db} não encontrado!", file=sys.stderr)
            return 1

    parent_db = load_parent_db(args.parent_db)
    print(f"Parent DB carregado com {len(parent_db)} pares ativos.")

    if isinstance(transport, HttpTransport) and args.apply:
        transport.unload_mounted_card()

    if args.backup:
        print(f"Criando backup da árvore em {args.backup}...")
        os.makedirs(args.backup, exist_ok=True)
        for rel in transport.walk_all_files():
            dst = os.path.join(args.backup, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(transport.read_bytes(rel))
        print("Backup concluído com sucesso.")

    ts_now = datetime.now().strftime("%Y%m%d-%H%M%S")
    planner = MigrationPlanner(transport, parent_db, args.merge_policy)
    planner.scan(ts_now)

    if args.verify:
        if len(planner.plan) > 0:
            print(f"Verificação encontrou {len(planner.plan)} itens pendentes de migração:")
            for p in planner.plan:
                print(f"  {p['note']}: {p['src']} -> {p['dst']}")
            return 2
        if isinstance(transport, HttpTransport) and transport.get_legacy_detected():
            print("Verificação falhou: legacy.detected é true no dispositivo Wi-Fi (§18.2.1 passo 6)")
            return 2
        print("Verificação concluída: zero pendências.")
        return 0

    is_apply = args.apply
    if is_apply:
        if not args.yes:
            if sys.stdin.isatty():
                ans = input("Deseja aplicar as alterações de migração no SD? [s/N]: ").strip().lower()
                if ans not in ("s", "sim", "y", "yes"):
                    print("Operação cancelada pelo usuário.")
                    return 0
            else:
                print("Erro de segurança (§18.1): --apply exige --yes quando executado sem terminal interativo.", file=sys.stderr)
                return 3

    resumo, manifest = planner.execute(is_apply, args.log, args.root)

    if args.json:
        out_dict = {
            "root": args.root,
            "apply": is_apply,
            "resumo": dict(resumo),
            "plan": [{k: v for k, v in p.items() if k != "fused_data"} for p in planner.plan]
        }
        print(json.dumps(out_dict, indent=2))
    else:
        print("\n" + manifest)
        print("Resumo:")
        for k, v in resumo.items():
            if v > 0:
                print(f"  {k:<20}: {v}")

    return 0

if __name__ == "__main__":
    sys.exit(main())
