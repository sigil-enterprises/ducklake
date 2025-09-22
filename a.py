import os

def process_file(filepath):
    with open(filepath, "r", encoding="utf-8") as f:
        lines = f.readlines()

    new_lines = []
    inserted = False
    has_data_path = any("test-env DATA_PATH" in line for line in lines)

    for line in lines:
        new_lines.append(line)

        # Insert DATA_PATH only once, right after DUCKLAKE_CONNECTION
        if (line.strip().startswith("test-env DUCKLAKE_CONNECTION") 
            and not inserted 
            and not has_data_path):
            new_lines.append("\ntest-env DATA_PATH __TEST_DIR__\n")
            inserted = True

    content = "".join(new_lines)

    # Only do replacements if we inserted the DATA_PATH line
    if inserted:
        updated_lines = []
        for line in content.splitlines(True):  # keep line endings
            if line.strip().startswith("test-env DUCKLAKE_CONNECTION"):
                updated_lines.append(line)  # leave untouched
            else:
                updated_lines.append(line.replace("__TEST_DIR__/", "${DATA_PATH}/"))
        content = "".join(updated_lines)

    with open(filepath, "w", encoding="utf-8") as f:
        f.write(content)


def process_folder(root_folder):
    for root, _, files in os.walk(root_folder):
        for filename in files:
            filepath = os.path.join(root, filename)
            process_file(filepath)


if __name__ == "__main__":
    folder = "./test/sql"  # change this to your folder
    process_folder(folder)
