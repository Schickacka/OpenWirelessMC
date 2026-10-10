import urllib.request, re, json

url = 'https://raw.githubusercontent.com/stenzek/duckstation/master/data/resources/gamedb.yaml'
req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
try:
    print('Downloading gamedb.yaml from DuckStation...')
    content = urllib.request.urlopen(req, timeout=40).read().decode('utf-8', errors='ignore')
    print('Downloaded! Length:', len(content))
    matches = re.findall(r'^([A-Z]{4}-\d{3,5}):\s*\n\s*name:\s*\"?([^\n\"]+)\"?', content, flags=re.MULTILINE)
    print('Total DuckStation games parsed:', len(matches))
    db = {}
    for code, name in matches:
        clean_name = name.strip().strip('"').strip("'")
        db[code] = clean_name
    
    with open('games_duckstation.json', 'w', encoding='utf-8') as f:
        json.dump(db, f, indent=2, ensure_ascii=False)
    print('Saved to games_duckstation.json, count:', len(db))
    
    test_codes = ['SLUS-00747', 'SLUS-00842', 'SLUS-00860', 'SLUS-00875', 'SLUS-00884', 'SLUS-00922', 'SLUS-01334', 'SCUS-94183', 'SLUS-01363']
    for c in test_codes:
        print(' ', c, '->', db.get(c, 'NOT FOUND'))
except Exception as e:
    print('Error:', e)
