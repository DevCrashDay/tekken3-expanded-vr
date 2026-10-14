# Sourced by Setup Tekken 3.command and Build Android APK.command (macOS), and
# by Setup Tekken 3.sh (Linux): the tools the setup needs, then Python in
# .setup/venv with the import's packages. Sets $venv; returns non-zero after
# telling the player what to do.
if [ "$(uname)" = Darwin ]; then
    missing=()
    for tool in cmake ninja; do command -v "$tool" >/dev/null || missing+=("$tool"); done
    if [ ${#missing[@]} -gt 0 ]; then
        echo "Missing: ${missing[*]}. Install them with Homebrew (https://brew.sh):"
        echo "    brew install cmake ninja sdl3"
        read -r -p "Press Return to close." _; return 1
    fi
    xcode-select -p >/dev/null 2>&1 || { echo "No compiler: run  xcode-select --install  then open this again."; read -r -p "Press Return to close." _; return 1; }
    python_hint() { echo "Python 3.10 to 3.15 is needed:  brew install python@3.13"; }
    pycheck='import sys, pyexpat, venv'
else
    . launcher/linux_deps.sh
    linux_deps || { read -r -p "Press Return to close." _; return 1; }
    python_hint() { echo "Python 3.10 to 3.15, with venv, is needed."; linux_packages; }
    # Debian and Ubuntu split ensurepip (python3-venv) from venv itself.
    pycheck='import sys, pyexpat, venv, ensurepip'
fi

# Python 3.10 to 3.15 whose pip works (pip needs pyexpat):
# tools/requirements-import.txt pins Pillow and numpy wheels for those only.
# A venv made with a newer Python lacks them, so it is made again here.
venv=.setup/venv
if [ ! -x "$venv/bin/python" ] || ! "$venv/bin/python" -c 'import PIL, numpy' 2>/dev/null; then
    python=""
    for p in python3.13 python3.12 python3.14 python3.11 python3.10 python3.15 python3; do
        if command -v "$p" >/dev/null && "$p" -c "$pycheck; sys.exit(not (3, 10) <= sys.version_info[:2] <= (3, 15))" 2>/dev/null; then
            python=$p; break
        fi
    done
    if [ -z "$python" ]; then
        python_hint
        read -r -p "Press Return to close." _; return 1
    fi
    echo "Preparing Python ($("$python" --version)) in $venv..."
    rm -rf "$venv"
    # Exact versions, every file checked against its SHA-256, wheels only
    # (tools/update_requirements.py writes the lists).
    "$python" -m venv "$venv" \
        && "$venv/bin/python" -m pip install --quiet --require-hashes --only-binary=:all: -r tools/requirements-pip.txt \
        && "$venv/bin/python" -m pip install --quiet --require-hashes --only-binary=:all: -r tools/requirements-import.txt \
        || { echo "Could not install Pillow and numpy."; read -r -p "Press Return to close." _; return 1; }
fi
