# local-tools/ — project-local binaries (NOT tracked by git)
#
# Drop a real `edbg` binary here and `test-radio-link.sh --flasher edbg`
# will find it: the script prepends this directory to PATH on startup,
# so no install, no sudo, no touching the rest of the system.
#
# Where to get edbg (Atmel EDBG programmer, ataradov/edbg):
#
#   sudo apt install build-essential libusb-1.0-0-dev libhidapi-dev
#   git clone https://github.com/ataradov/edbg.git /tmp/edbg-src
#   make -C /tmp/edbg-src
#   cp /tmp/edbg-src/edbg <this dir>/edbg
#   ./local-tools/edbg -l        # should list attached EDBG probes
#
# Then: ./test-radio-link.sh --flasher edbg --duration 30 --log-dir logs/run1
#
# Notes:
# - `edbg` needs USB access to the probes (same udev rules as openocd;
#   typically the `plugdev`/`dialout` group or the Zephyr udev rules).
# - This directory intentionally has no other contents: one tool, one job.
