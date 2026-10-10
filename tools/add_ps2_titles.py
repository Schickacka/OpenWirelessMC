import urllib.request
import re
import os

print("Downloading PCSX2 GameIndex.yaml...")
url = "https://raw.githubusercontent.com/PCSX2/pcsx2/master/bin/resources/GameIndex.yaml"
req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
content = urllib.request.urlopen(req, timeout=30).read().decode("utf-8", errors="ignore")

# Find serials and names
# Pattern in GameIndex.yaml:
# SLUS-20002:
#   name: ...
matches = re.findall(r'^([A-Za-z]{4}-\d{5}):\s*\n\s*name:\s*(.+)$', content, re.MULTILINE)
print(f"Total PS2 serial entries found: {len(matches)}")

ps2_dict = {}
for serial, name in matches:
    clean_name = name.strip().strip('"').strip("'")
    clean_serial = serial.upper()
    if clean_serial not in ps2_dict:
        ps2_dict[clean_serial] = clean_name

print(f"Unique PS2 games: {len(ps2_dict)}")

# Read existing titles.db (PS1 games)
ps1_dict = {}
if os.path.exists("titles.db"):
    with open("titles.db", "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line:
                k, v = line.split("=", 1)
                ps1_dict[k.strip().upper()] = v.strip()

print(f"Existing PS1 games in titles.db: {len(ps1_dict)}")

# Merge: PS1 + PS2
merged = dict(ps1_dict)
merged.update(ps2_dict)
print(f"Total Combined PS1 + PS2 games: {len(merged)}")

# Write to titles_complete.db
with open("titles.db", "w", encoding="utf-8") as f:
    f.write("# OpenWirelessMC Global Game Database (PS1 & PS2)\n")
    for k in sorted(merged.keys()):
        f.write(f"{k}={merged[k]}\n")

new_size = os.path.getsize("titles.db")
print(f"New titles.db size: {new_size} bytes ({new_size / 1024:.1f} KB)")
