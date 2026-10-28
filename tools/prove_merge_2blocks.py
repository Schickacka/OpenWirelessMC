#!/usr/bin/env python3
import os
import sys
import struct
import hashlib

# Import merge_card_images and helpers from migrate_sd
sys.path.insert(0, os.path.dirname(__file__))
from migrate_sd import xor_checksum, merge_card_images, parse_card_saves

def make_blank_card() -> bytearray:
    card = bytearray(131072)
    card[0:2] = b"MC"
    for b in range(1, 16):
        d_off = b * 128
        card[d_off : d_off + 4] = b"\xA0\x00\x00\x00"
        card[d_off + 4 : d_off + 8] = b"\x00\x00\x00\x00"
        card[d_off + 8 : d_off + 10] = b"\xFF\xFF"
    for f in range(1024):
        fo = f * 128
        card[fo + 127] = xor_checksum(card[fo : fo + 127])
    return card

def main():
    print("=== PROVA A: Save de 2 blocos na fusao ===")
    
    # 1. Monta Card A com save de 1 bloco no Bloco 1
    card_a = make_blank_card()
    # Dir frame bloco 1 (offset 128 = 0x80)
    card_a[128:132] = b"\x51\x00\x00\x00"
    card_a[132:136] = struct.pack("<I", 8192)
    card_a[136:138] = b"\xFF\xFF"
    name_a = b"BASLUS-00421SAVE1"
    card_a[138:138+len(name_a)] = name_a
    # Data bloco 1 (offset 8192 = 0x2000)
    card_a[8192:8194] = b"SC"
    payload_a = bytes([0xAA]) * 8190
    card_a[8194:8194+len(payload_a)] = payload_a
    for f in range(64):
        fo = 8192 + f * 128
        card_a[fo + 127] = xor_checksum(card_a[fo : fo + 127])
    card_a[128 + 127] = xor_checksum(card_a[128 : 128 + 127])

    # 2. Monta Card B com save de 2 blocos nos Blocos 1 e 2
    card_b = make_blank_card()
    # Bloco 1 (primeiro bloco): estado 0x51, tamanho 16384, proximo = bloco 2
    dir1 = 1 * 128
    card_b[dir1 : dir1 + 4] = b"\x51\x00\x00\x00"
    card_b[dir1 + 4 : dir1 + 8] = struct.pack("<I", 16384)
    card_b[dir1 + 8 : dir1 + 10] = struct.pack("<H", 2)
    name_b = b"BASLUS-00421SAVE2BLKS"
    card_b[dir1 + 10 : dir1 + 10 + len(name_b)] = name_b
    card_b[dir1 + 127] = xor_checksum(card_b[dir1 : dir1 + 127])

    # Bloco 2 (bloco final): estado 0x53, tamanho 0, proximo = 0xFFFF
    dir2 = 2 * 128
    card_b[dir2 : dir2 + 4] = b"\x53\x00\x00\x00"
    card_b[dir2 + 4 : dir2 + 8] = b"\x00\x00\x00\x00"
    card_b[dir2 + 8 : dir2 + 10] = struct.pack("<H", 0xFFFF)
    card_b[dir2 + 10 : dir2 + 10 + len(name_b)] = name_b
    card_b[dir2 + 127] = xor_checksum(card_b[dir2 : dir2 + 127])

    # Dados do Bloco 1 de Card B (8192 B com SC no frame 0)
    b1_off = 1 * 8192
    card_b[b1_off : b1_off + 2] = b"SC"
    orig_payload_b1 = bytes([0xB1]) * 8190
    card_b[b1_off + 2 : b1_off + 8192] = orig_payload_b1
    for f in range(64):
        fo = b1_off + f * 128
        card_b[fo + 127] = xor_checksum(card_b[fo : fo + 127])

    # Dados do Bloco 2 de Card B (8192 B)
    b2_off = 2 * 8192
    orig_payload_b2 = bytes([0xB2]) * 8192
    card_b[b2_off : b2_off + 8192] = orig_payload_b2
    for f in range(64):
        fo = b2_off + f * 128
        card_b[fo + 127] = xor_checksum(card_b[fo : fo + 127])

    # 3. Executa a fusao
    fused = merge_card_images(bytes(card_a), bytes(card_b))
    assert fused is not None, "Falha na fusao!"

    # 4. Inspecao do cartao fundido
    # Bloco 1 de fused mantem o save original de Card A
    # O save de 2 blocos de Card B foi alocado nos blocos livres 2 e 3 de fused!
    f_dir2 = 2 * 128
    estado_b2 = fused[f_dir2 + 0x00]
    tam_b2 = struct.unpack("<I", fused[f_dir2 + 0x04 : f_dir2 + 0x08])[0]
    prox_b2 = struct.unpack("<H", fused[f_dir2 + 0x08 : f_dir2 + 0x0A])[0]

    f_dir3 = 3 * 128
    estado_b3 = fused[f_dir3 + 0x00]
    prox_b3 = struct.unpack("<H", fused[f_dir3 + 0x08 : f_dir3 + 0x0A])[0]

    # Payloads no cartao fundido
    fused_payload_b2 = fused[2 * 8192 : 3 * 8192]
    fused_payload_b3 = fused[3 * 8192 : 4 * 8192]

    orig_full_b1 = card_b[1 * 8192 : 2 * 8192]
    orig_full_b2 = card_b[2 * 8192 : 3 * 8192]

    print(f"1. Bloco 2 no cartao fundido (novo inicio do save):")
    print(f"   estado[0x00]  = 0x{estado_b2:02X} (esperado 0x51)")
    print(f"   tamanho[0x04] = {tam_b2} bytes (esperado 16384)")
    print(f"   proximo[0x08] = {prox_b2} (apontando para o bloco novo 3)")
    print()
    print(f"2. Bloco 3 no cartao fundido (novo bloco terminal):")
    print(f"   estado[0x00]  = 0x{estado_b3:02X} (esperado 0x53)")
    print(f"   proximo[0x08] = 0x{prox_b3:04X} (esperado 0xFFFF = terminal)")
    print()
    print(f"3. Comparacao do payload de cada bloco:")
    print(f"   Payload Bloco 2 fundido == Original Bloco 1 Card B: {fused_payload_b2 == orig_full_b1} ({len(fused_payload_b2)} bytes)")
    print(f"   Payload Bloco 3 fundido == Original Bloco 2 Card B: {fused_payload_b3 == orig_full_b2} ({len(fused_payload_b3)} bytes)")
    print(f"   SHA256 Bloco 2: {hashlib.sha256(fused_payload_b2).hexdigest()}")
    print(f"   SHA256 Bloco 3: {hashlib.sha256(fused_payload_b3).hexdigest()}")
    print()
    assert estado_b2 == 0x51
    assert prox_b2 == 3
    assert estado_b3 == 0x53
    assert prox_b3 == 0xFFFF
    assert fused_payload_b2 == orig_full_b1
    assert fused_payload_b3 == orig_full_b2
    print("Resultado: Fusao de save de 2 blocos validada com sucesso absoluto.")

if __name__ == "__main__":
    main()
