import json
import re

log_path = "/Users/shreyjain/.gemini/antigravity/brain/83f1aaf5-0cac-423e-85a6-1e6b7b06cc3d/.system_generated/logs/transcript.jsonl"
target_files = [
    "/Users/shreyjain/Downloads/exchange-engine/include/OrderBook.h",
    "/Users/shreyjain/Downloads/exchange-engine/include/Market.h",
    "/Users/shreyjain/Downloads/exchange-engine/include/Server.h",
    "/Users/shreyjain/Downloads/exchange-engine/include/Database.h",
    "/Users/shreyjain/Downloads/exchange-engine/src/main.cpp",
    "/Users/shreyjain/Downloads/exchange-engine/tests/test_orderbook.cpp"
]

files_restored = set()

# Process backwards to get the most recent view_file response
with open(log_path, 'r') as f:
    lines = f.readlines()

for line in reversed(lines):
    try:
        entry = json.loads(line)
    except:
        continue
    
    if entry.get("type") == "ACTION_RESULT" and "tool_calls" in entry:
        for call in entry["tool_calls"]:
            output = call.get("call_result", {}).get("output", "")
            if "The following code has been modified" in output:
                # Output might have 'File Path: `file:///path/to/file`'
                match = re.search(r"File Path: `file://([^`]+)`", output)
                if match:
                    filepath = match.group(1)
                    if filepath in target_files and filepath not in files_restored:
                        print(f"Restoring {filepath}")
                        # Extract lines
                        out_lines = output.split('\n')
                        content_lines = []
                        start_parsing = False
                        for l in out_lines:
                            if l.startswith("The following code has been modified"):
                                start_parsing = True
                                continue
                            if l.startswith("The above content shows the entire"):
                                break
                            if start_parsing:
                                # Remove <line_number>: prefix
                                l_stripped = re.sub(r'^\d+:\s?', '', l)
                                content_lines.append(l_stripped)
                        
                        with open(filepath, 'w') as out_f:
                            out_f.write('\n'.join(content_lines))
                        files_restored.add(filepath)

print("Done. Restored:", files_restored)
