"""
Pre-build script: sets PROJECT_SRC_DIR per environment.
Usage in platformio.ini:
    extra_scripts = pre:scripts/select_src.py
    custom_src_dir = cam/src
"""
Import("env")
import os

src = env.GetProjectOption("custom_src_dir", "src")
full = os.path.join(env.subst("$PROJECT_DIR"), src)
env.Replace(PROJECT_SRC_DIR=full)
print("  SRC_DIR -> %s" % src)
