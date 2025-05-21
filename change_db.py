import os

LOAD_STATEMENT = (
    "statement ok\n"
    "LOAD '/Users/holanda/Documents/Projects/ducklake/duckdb/build/release/extension/sqlite_scanner/sqlite_scanner.duckdb_extension'\n"
)

def process_file(file_path):
    with open(file_path, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    # Find where to insert the LOAD statement (after all initial comments starting with "#")
    insert_idx = 0
    while insert_idx < len(lines) and lines[insert_idx].startswith('#'):
        insert_idx += 1

    # Check if LOAD already exists
    if any("LOAD" in line and "sqlite_scanner" in line for line in lines):
        return  # Skip if already processed

    # Insert LOAD statement
    lines = lines[:insert_idx] + [LOAD_STATEMENT + "\n"] + lines[insert_idx:]

    # Replace ducklake:__TEST_DIR__ with ducklake:sqlite:__TEST_DIR__ in ATTACH lines
    lines = [line.replace("ducklake:__TEST_DIR__", "ducklake:sqlite:__TEST_DIR__") for line in lines]

    # Overwrite the file
    with open(file_path, 'w', encoding='utf-8') as f:
        f.writelines(lines)
    print(f"Updated: {file_path}")

def walk_directory(root_folder):
    for dirpath, _, filenames in os.walk(root_folder):
        for filename in filenames:
            if filename.endswith('.test'):
                full_path = os.path.join(dirpath, filename)
                process_file(full_path)

if __name__ == "__main__":
    folder_to_process = "./test/sql"  # Replace with the actual path
    walk_directory(folder_to_process)