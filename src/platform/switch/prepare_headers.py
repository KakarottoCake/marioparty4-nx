import os
import shutil
import sys

src_inc = os.path.abspath("../../../include")
dst_inc = os.path.abspath("build/include")

os.makedirs(dst_inc, exist_ok=True)

# Copy directories
for item in ["game", "dolphin", "REL", "data_num", "msm"]:
    src_path = os.path.join(src_inc, item)
    dst_path = os.path.join(dst_inc, item)
    if os.path.exists(src_path):
        if os.path.exists(dst_path):
            try:
                os.rmdir(dst_path)
            except Exception:
                try:
                    shutil.rmtree(dst_path)
                except Exception:
                    pass
        
        try:
            if sys.platform == "win32":
                import subprocess
                subprocess.check_call(f'mklink /j "{dst_path}" "{src_path}"', shell=True)
            else:
                os.symlink(src_path, dst_path)
        except Exception:
            shutil.copytree(src_path, dst_path)

# Copy non-standard files from the root of include/
ignored_files = {
    "stdint.h", "stddef.h", "stdlib.h", "string.h", 
    "math.h", "ctype.h", "float.h", "stdarg.h", "stdio.h"
}

for item in os.listdir(src_inc):
    src_path = os.path.join(src_inc, item)
    if os.path.isfile(src_path):
        if item.endswith(".h") and item not in ignored_files:
            shutil.copy2(src_path, os.path.join(dst_inc, item))

print("Switch port include directories and headers prepared successfully.")
