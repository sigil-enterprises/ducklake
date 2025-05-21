import os
import sys

if len(sys.argv) != 3:
    print("Usage: python script.py <dependency_path> <dbms>")
    sys.exit(1)

dependency_path = sys.argv[1]
dbms = sys.argv[2]

# Automatically resolve folder_to_process as ../test/sql relative to this script
script_dir = os.path.dirname(os.path.abspath(__file__))
folder_to_process = os.path.abspath(os.path.join(script_dir, "..", "test", "sql"))

LOAD_STATEMENT = (
    "\nstatement ok\n"
    f"LOAD '{dependency_path}'"
)

def process_file(file_path):
    with open(file_path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    insert_idx = 0
    while insert_idx < len(lines) and lines[insert_idx].startswith('#'):
        insert_idx += 1
    if any("LOAD" in line and dependency_path in line for line in lines):
        return
    lines = lines[:insert_idx] + [LOAD_STATEMENT + "\n"] + lines[insert_idx:]
    lines = [line.replace("ducklake:__TEST_DIR__", f"ducklake:{dbms}:__TEST_DIR__") for line in lines]
    with open(file_path, 'w', encoding='utf-8') as f:
        f.writelines(lines)
    print(f"Updated: {file_path}")

def walk_directory(root_folder):
    for dirpath, _, filenames in os.walk(root_folder):
        for filename in filenames:
            if filename.endswith('.test'):
                full_path = os.path.join(dirpath, filename)
                process_file(full_path)

walk_directory(folder_to_process)
