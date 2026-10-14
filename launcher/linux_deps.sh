# Sourced by launcher/prepare_python.sh on Linux (Python itself is checked
# there): the build tools, and the
# headers SDL3 needs when no SDL3 3.4 is installed (the game's CMake then
# builds SDL3 itself, and without X11 and audio headers that SDL3 would have
# no window and no sound). Returns non-zero after printing the one command
# that installs what is missing on this distribution.
linux_packages() {
    local id="" like=""
    if [ -r /etc/os-release ]; then
        id=$(. /etc/os-release; echo "${ID:-}")
        like=$(. /etc/os-release; echo "${ID_LIKE:-}")
    fi
    case " $id $like " in
    *" steamos "*)
        echo "SteamOS is read-only: set the game up inside a Distrobox container. In Desktop Mode, in Konsole:"
        echo "    distrobox create --name t3x --image ubuntu:24.04"
        echo "    distrobox enter t3x"
        echo "then, inside it, in this folder, run  ./\"Setup Tekken 3.sh\"  again: it gives the command to type there."
        ;;
    *" arch "*)
        echo "Install them with:"
        echo "    sudo pacman -S --needed base-devel cmake ninja python tk sdl3 libx11 libxext libxrandr libxcursor libxi libxkbcommon libpulse alsa-lib"
        ;;
    *" fedora "*|*" rhel "*)
        echo "Install them with:"
        echo "    sudo dnf install gcc gcc-c++ cmake ninja-build pkgconf-pkg-config python3 python3-tkinter" \
             "libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel libXtst-devel" \
             "libxkbcommon-devel wayland-devel libdecor-devel mesa-libGL-devel mesa-libEGL-devel mesa-libgbm-devel libdrm-devel" \
             "alsa-lib-devel pulseaudio-libs-devel dbus-devel systemd-devel"
        ;;
    *" debian "*|*" ubuntu "*)
        echo "Install them with:"
        echo "    sudo apt install build-essential cmake ninja-build pkg-config python3-venv python3-tk" \
             "libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev" \
             "libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libdrm-dev libgbm-dev libgl1-mesa-dev libegl1-mesa-dev" \
             "libgles2-mesa-dev libdbus-1-dev libudev-dev"
        ;;
    *)
        echo "Install them with your package manager: a C and C++ compiler, cmake, ninja, pkg-config, Python 3.10+ with venv"
        echo "(and Tk for the setup window), and the development files of X11 (Xext, Xrandr, Xcursor, Xi), xkbcommon,"
        echo "PulseAudio or ALSA and udev; or SDL3 3.4 or later. The README lists them for Ubuntu, Arch and Fedora."
        ;;
    esac
}

linux_deps() {
    local missing=() tool module
    for tool in cmake ninja pkg-config; do command -v "$tool" >/dev/null || missing+=("$tool"); done
    command -v cc >/dev/null || command -v gcc >/dev/null || command -v clang >/dev/null || missing+=("a C compiler")
    command -v c++ >/dev/null || command -v g++ >/dev/null || command -v clang++ >/dev/null || missing+=("a C++ compiler")
    if command -v pkg-config >/dev/null && ! pkg-config --exists 'sdl3 >= 3.4'; then
        for module in x11 xext xrandr xcursor xi xkbcommon libudev; do
            pkg-config --exists "$module" || missing+=("$module headers")
        done
        pkg-config --exists libpulse || pkg-config --exists alsa || missing+=("audio headers (PulseAudio or ALSA)")
    fi
    [ ${#missing[@]} -eq 0 ] && return 0
    echo "Missing: $(IFS=,; echo "${missing[*]}" | sed 's/,/, /g')."
    linux_packages
    return 1
}
