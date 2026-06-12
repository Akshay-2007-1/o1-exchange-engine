import json
import re

log_path = "/Users/shreyjain/.gemini/antigravity/brain/83f1aaf5-0cac-423e-85a6-1e6b7b06cc3d/.system_generated/logs/transcript.jsonl"
with open(log_path, 'r') as f:
    data = f.read()

# Match File Path: `file:///Users/shreyjain/Downloads/exchange-engine/include/OrderBook.h`
matches = re.finditer(r"File Path: `file://([^`]+)`\nTotal Lines: \d+\nTotal Bytes: \d+\nShowing lines \d+ to \d+\nThe following code has been modified.*?\n(1:.*?)\nThe above content shows the entire", data, re.DOTALL)

restored = set()
for match in matches:
    filepath = match.group(1)
    content = match.group(2)
    # The regex might match multiple times, let's keep the last one by overwriting
    
    # Process content
    out_lines = []
    for line in content.split('\n'):
        # strip "123: "
        l_stripped = re.sub(r'^\d+:\s?', '', line)
        out_lines.append(l_stripped)
        
    with open(filepath, 'w') as out_f:
        out_f.write('\n'.join(out_lines))
    restored.add(filepath)
    
print("Restored:", restored)
