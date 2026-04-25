#!/usr/bin/env bash

# Script to build Altair for all supported Pico boards
# Usage: ./build_all_boards.sh


set -e  # Exit on error
set -o pipefail
BUILD_JOBS="${BUILD_JOBS:-6}"

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Timing helpers
now_ms() {
    python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
}

format_ms() {
    local ms="$1"
    python3 - "$ms" <<'PY'
import sys
ms = int(sys.argv[1])
print(f"{ms/1000:.3f}s")
PY
}

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Increment build version once at the start
VERSION_FILE="${SCRIPT_DIR}/build_version.txt"
if [ -f "$VERSION_FILE" ]; then
    CURRENT_VERSION=$(cat "$VERSION_FILE")
    NEW_VERSION=$((CURRENT_VERSION + 1))
    echo "$NEW_VERSION" > "$VERSION_FILE"
    echo -e "${GREEN}Build version incremented: ${CURRENT_VERSION} -> ${NEW_VERSION}${NC}"
else
    echo "1" > "$VERSION_FILE"
    NEW_VERSION=1
    echo -e "${GREEN}Build version initialized: ${NEW_VERSION}${NC}"
fi
echo ""

# Array of boards to test
# Note: Display 2.8 and large patch pool require RP2350 (520KB RAM) - pico/pico_w only have 264KB
BOARDS=("pico2" "pico2_w_rfs" "pico2_w_sd" "pico2_w_vt100_sd_bt" "pimoroni_pico_plus2_w_rp2350" "pimoroni_pico_plus2_w_rp2350_sd" "pimoroni_pico_plus2_w_rp2350_waveshare2_rfs" "pico2_w_display28_rfs" "pico2_w_waveshare2_rfs" "pico_w_rfs")

# Create tests directory (clean slate)
TESTS_DIR="${SCRIPT_DIR}/tests"
if [ -d "$TESTS_DIR" ]; then
    echo -e "${YELLOW}Cleaning old test artifacts...${NC}"
    rm -rf "$TESTS_DIR"
fi
mkdir -p "$TESTS_DIR"

echo -e "${BLUE}=================================${NC}"
echo -e "${BLUE}Altair 8800 Multi-Board Build Test${NC}"
echo -e "${BLUE}=================================${NC}"
echo -e "${BLUE}Build parallelism: ${BUILD_JOBS}${NC}"
echo ""

# Clean up old releases
echo -e "${YELLOW}Cleaning up old releases...${NC}"
rm -f "${SCRIPT_DIR}/Releases/"*.uf2
echo ""

# Track build results (using simple arrays as fallback for zsh compatibility)
BUILD_RESULTS=()
BUILD_TIMES=()
RM_TIMES=()
CONFIGURE_TIMES=()
COMPILE_TIMES=()
COPY_TIMES=()
TOTAL_BOARD_TIMES=()
TOTAL_START_MS=$(now_ms)

# Build for each board
BOARD_INDEX=0
for BOARD in "${BOARDS[@]}"; do
    echo -e "${YELLOW}Building for ${BOARD}...${NC}"

    BUILD_START_MS=$(now_ms)

    # Clean and build (skip version increment since we did it once at the start)
    RM_START_MS=$(now_ms)
    rm -rf build
    RM_END_MS=$(now_ms)
    RM_TIME_MS=$((RM_END_MS - RM_START_MS))

    # Set CMake options based on board
    # Remove display suffixes from PICO_BOARD
    CLEAN_BOARD="${BOARD//_inky/}"
    CLEAN_BOARD="${CLEAN_BOARD//_display28/}"
    CLEAN_BOARD="${CLEAN_BOARD//_waveshare35_sd/}"
    CLEAN_BOARD="${CLEAN_BOARD//_waveshare2/}"
    CLEAN_BOARD="${CLEAN_BOARD//_vt100/}"
    CLEAN_BOARD="${CLEAN_BOARD//_rfs/}"
    CLEAN_BOARD="${CLEAN_BOARD//_sd/}"
    CLEAN_BOARD="${CLEAN_BOARD//_bt/}"
    CMAKE_OPTS="-DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=${CLEAN_BOARD} -DSKIP_VERSION_INCREMENT=1 -DALTAIR_DEBUG=OFF"

    # Configure SD Card Support
    if [[ "$BOARD" == *"_sd" ]] || [[ "$BOARD" == *"_sd_"* ]] || [[ "$BOARD" == *"_waveshare35_sd" ]]; then
        CMAKE_OPTS="$CMAKE_OPTS -DSD_CARD_SUPPORT=ON"
    else
        CMAKE_OPTS="$CMAKE_OPTS -DSD_CARD_SUPPORT=OFF"
    fi

    # Add Waveshare 3.5" display support
    if [[ "$BOARD" == *"_waveshare35_sd" ]] || [[ "$BOARD" == *"_vt100"* ]]; then
        CMAKE_OPTS="$CMAKE_OPTS -DWAVESHARE_3_5_DISPLAY=ON"
    fi

    # Add VT100 display support
    if [[ "$BOARD" == *"_vt100"* ]]; then
        CMAKE_OPTS="$CMAKE_OPTS -DVT100_DISPLAY=ON"
    fi

    # Add Bluetooth keyboard support
    if [[ "$BOARD" == *"_bt" ]]; then
        CMAKE_OPTS="$CMAKE_OPTS -DBLUETOOTH_KEYBOARD_SUPPORT=ON"
    else
        CMAKE_OPTS="$CMAKE_OPTS -DBLUETOOTH_KEYBOARD_SUPPORT=OFF"
    fi

    # Add Waveshare 2" display support
    if [[ "$BOARD" == *"_waveshare2"* ]]; then
        CMAKE_OPTS="$CMAKE_OPTS -DWAVESHARE_2_DISPLAY=ON"
    fi

    # Add display support flags
    if [[ "$BOARD" == *"_inky" ]]; then
        CMAKE_OPTS="$CMAKE_OPTS -DINKY_SUPPORT=ON -DDISPLAY_ST7789_SUPPORT=OFF"
    elif [[ "$BOARD" == *"_display28"* ]] || [[ "$BOARD" == *"_waveshare2"* ]]; then
        CMAKE_OPTS="$CMAKE_OPTS -DINKY_SUPPORT=OFF -DDISPLAY_ST7789_SUPPORT=ON"
    else
        CMAKE_OPTS="$CMAKE_OPTS -DINKY_SUPPORT=OFF -DDISPLAY_ST7789_SUPPORT=OFF"
    fi

    # Add Remote FS support (Explicit _rfs suffix OR any pico_w variant)
    if [[ "$BOARD" == *"_rfs" ]] || [[ "$BOARD" == "pico_w" ]] || [[ "$BOARD" == "pico_w_inky" ]]; then
        # IP address is configured at runtime via flash storage (serial console prompt)
        CMAKE_OPTS="$CMAKE_OPTS -DREMOTE_FS_SUPPORT=ON -DRFS_SERVER_PORT=8085"
    fi

    CONFIGURE_START_MS=$(now_ms)
    if cmake -B build -G Ninja $CMAKE_OPTS; then
        CONFIGURE_END_MS=$(now_ms)
        CONFIGURE_TIME_MS=$((CONFIGURE_END_MS - CONFIGURE_START_MS))

        COMPILE_START_MS=$(now_ms)
        if cmake --build build --parallel "$BUILD_JOBS"; then
            COMPILE_END_MS=$(now_ms)
            COMPILE_TIME_MS=$((COMPILE_END_MS - COMPILE_START_MS))

            BUILD_END_MS=$(now_ms)
            BUILD_TIME_MS=$((BUILD_END_MS - BUILD_START_MS))
            BUILD_TIMES[$BOARD_INDEX]="${BOARD}:${BUILD_TIME_MS}"
            RM_TIMES[$BOARD_INDEX]="${BOARD}:${RM_TIME_MS}"
            CONFIGURE_TIMES[$BOARD_INDEX]="${BOARD}:${CONFIGURE_TIME_MS}"
            COMPILE_TIMES[$BOARD_INDEX]="${BOARD}:${COMPILE_TIME_MS}"

            # Copy artifacts to tests directory
            COPY_START_MS=$(now_ms)
            BOARD_DIR="${TESTS_DIR}/${BOARD}"
            mkdir -p "$BOARD_DIR"

            if [ -f "build/altair.uf2" ]; then
                cp build/altair.uf2 "${BOARD_DIR}/altair_${BOARD}.uf2"
                # Only keep UF2 for release builds
                # cp build/altair.elf "${BOARD_DIR}/altair_${BOARD}.elf"
                # cp build/altair.dis "${BOARD_DIR}/altair_${BOARD}.dis" 2>/dev/null || true

                # Get file size
                UF2_SIZE=$(du -h "${BOARD_DIR}/altair_${BOARD}.uf2" | cut -f1)
                COPY_END_MS=$(now_ms)
                COPY_TIME_MS=$((COPY_END_MS - COPY_START_MS))
                COPY_TIMES[$BOARD_INDEX]="${BOARD}:${COPY_TIME_MS}"
                TOTAL_BOARD_TIMES[$BOARD_INDEX]="${BOARD}:${BUILD_TIME_MS}"

                BUILD_RESULTS[$BOARD_INDEX]="${BOARD}:✓ SUCCESS ($(format_ms "$BUILD_TIME_MS"), ${UF2_SIZE})"
                echo -e "${GREEN}✓ Build successful for ${BOARD} ($(format_ms "$BUILD_TIME_MS"))${NC}"
                echo -e "${GREEN}  Clean: $(format_ms "$RM_TIME_MS") | Configure: $(format_ms "$CONFIGURE_TIME_MS") | Build: $(format_ms "$COMPILE_TIME_MS") | Copy: $(format_ms "$COPY_TIME_MS")${NC}"
                echo -e "${GREEN}  Artifacts saved to: ${BOARD_DIR}/${NC}"
            else
                COPY_END_MS=$(now_ms)
                COPY_TIME_MS=$((COPY_END_MS - COPY_START_MS))
                COPY_TIMES[$BOARD_INDEX]="${BOARD}:${COPY_TIME_MS}"
                TOTAL_BOARD_TIMES[$BOARD_INDEX]="${BOARD}:${BUILD_TIME_MS}"
                BUILD_RESULTS[$BOARD_INDEX]="${BOARD}:✗ FAILED - No UF2 generated ($(format_ms "$BUILD_TIME_MS"))"
                echo -e "${RED}✗ Build failed for ${BOARD} - No UF2 file generated${NC}"
                echo -e "${YELLOW}  Clean: $(format_ms "$RM_TIME_MS") | Configure: $(format_ms "$CONFIGURE_TIME_MS") | Build: $(format_ms "$COMPILE_TIME_MS") | Copy: $(format_ms "$COPY_TIME_MS")${NC}"
            fi
        else
            COMPILE_END_MS=$(now_ms)
            COMPILE_TIME_MS=$((COMPILE_END_MS - COMPILE_START_MS))
            BUILD_END_MS=$(now_ms)
            BUILD_TIME_MS=$((BUILD_END_MS - BUILD_START_MS))
            RM_TIMES[$BOARD_INDEX]="${BOARD}:${RM_TIME_MS}"
            CONFIGURE_TIMES[$BOARD_INDEX]="${BOARD}:${CONFIGURE_TIME_MS}"
            COMPILE_TIMES[$BOARD_INDEX]="${BOARD}:${COMPILE_TIME_MS}"
            COPY_TIMES[$BOARD_INDEX]="${BOARD}:0"
            TOTAL_BOARD_TIMES[$BOARD_INDEX]="${BOARD}:${BUILD_TIME_MS}"
            BUILD_RESULTS[$BOARD_INDEX]="${BOARD}:✗ FAILED ($(format_ms "$BUILD_TIME_MS"))"
            echo -e "${RED}✗ Build failed for ${BOARD}${NC}"
            echo -e "${YELLOW}  Clean: $(format_ms "$RM_TIME_MS") | Configure: $(format_ms "$CONFIGURE_TIME_MS") | Build: $(format_ms "$COMPILE_TIME_MS") | Copy: $(format_ms 0)${NC}"
        fi
    else
        CONFIGURE_END_MS=$(now_ms)
        CONFIGURE_TIME_MS=$((CONFIGURE_END_MS - CONFIGURE_START_MS))
        BUILD_END_MS=$(now_ms)
        BUILD_TIME_MS=$((BUILD_END_MS - BUILD_START_MS))
        RM_TIMES[$BOARD_INDEX]="${BOARD}:${RM_TIME_MS}"
        CONFIGURE_TIMES[$BOARD_INDEX]="${BOARD}:${CONFIGURE_TIME_MS}"
        COMPILE_TIMES[$BOARD_INDEX]="${BOARD}:0"
        COPY_TIMES[$BOARD_INDEX]="${BOARD}:0"
        TOTAL_BOARD_TIMES[$BOARD_INDEX]="${BOARD}:${BUILD_TIME_MS}"
        BUILD_RESULTS[$BOARD_INDEX]="${BOARD}:✗ CONFIGURE FAILED ($(format_ms "$BUILD_TIME_MS"))"
        echo -e "${RED}✗ Configure failed for ${BOARD}${NC}"
        echo -e "${YELLOW}  Clean: $(format_ms "$RM_TIME_MS") | Configure: $(format_ms "$CONFIGURE_TIME_MS") | Build: $(format_ms 0) | Copy: $(format_ms 0)${NC}"
    fi

    BOARD_INDEX=$((BOARD_INDEX + 1))
    echo ""
done

TOTAL_END_MS=$(now_ms)
TOTAL_TIME_MS=$((TOTAL_END_MS - TOTAL_START_MS))
TOTAL_TIME=$(python3 - "$TOTAL_TIME_MS" <<'PY'
import sys
ms = int(sys.argv[1])
print(f"{ms/1000:.3f}")
PY
)

# Generate summary report
REPORT_FILE="${TESTS_DIR}/build_report.txt"
{
    echo "================================="
    echo "Altair 8800 Build Test Report"
    echo "================================="
    echo "Date: $(date)"
    echo "Total Time: ${TOTAL_TIME}s"
    echo "Build parallelism: ${BUILD_JOBS}"
    echo ""
    echo "Build Results:"
    echo "---------------------------------"
    printf "%-40s %12s %12s %12s %12s %12s\n" "Board" "Clean" "Configure" "Build" "Copy" "Total"
    printf "%-40s %12s %12s %12s %12s %12s\n" "----------------------------------------" "------------" "------------" "------------" "------------" "------------"
    for i in "${!BUILD_RESULTS[@]}"; do
        BOARD_NAME="${BUILD_RESULTS[$i]%%:*}"
        RM_MS="${RM_TIMES[$i]#*:}"
        CONFIG_MS="${CONFIGURE_TIMES[$i]#*:}"
        COMPILE_MS="${COMPILE_TIMES[$i]#*:}"
        COPY_MS="${COPY_TIMES[$i]#*:}"
        TOTAL_MS="${TOTAL_BOARD_TIMES[$i]#*:}"
        printf "%-40s %12s %12s %12s %12s %12s\n" \
            "$BOARD_NAME" \
            "$(format_ms "$RM_MS")" \
            "$(format_ms "$CONFIG_MS")" \
            "$(format_ms "$COMPILE_MS")" \
            "$(format_ms "$COPY_MS")" \
            "$(format_ms "$TOTAL_MS")"
    done
    echo ""
    echo "Detailed Results:"
    echo "---------------------------------"
    for RESULT in "${BUILD_RESULTS[@]}"; do
        BOARD="${RESULT%%:*}"
        STATUS="${RESULT#*:}"
        echo "  ${BOARD}: ${STATUS}"
    done
    echo ""
    echo "Artifacts Location: ${TESTS_DIR}"
    echo ""
    # List all generated files
    echo "Generated Files:"
    echo "---------------------------------"
    find "$TESTS_DIR" -name "*.uf2" -o -name "*.elf" | sort
} > "$REPORT_FILE"

# Copy successfully built artifacts to Releases folder
RELEASES_DIR="${SCRIPT_DIR}/Releases"
mkdir -p "$RELEASES_DIR"
echo ""
echo -e "${YELLOW}Copying artifacts to Releases directory...${NC}"
find "$TESTS_DIR" -name "*.uf2" -exec cp {} "$RELEASES_DIR/" \;
echo -e "${GREEN}All UF2 artifacts copied to ${RELEASES_DIR}${NC}"
echo ""

# Print summary to console
echo -e "${BLUE}=================================${NC}"
echo -e "${BLUE}Build Summary${NC}"
echo -e "${BLUE}=================================${NC}"

SUCCESS_COUNT=0
FAIL_COUNT=0

for RESULT in "${BUILD_RESULTS[@]}"; do
    BOARD="${RESULT%%:*}"
    STATUS="${RESULT#*:}"
    if [[ $STATUS == *"SUCCESS"* ]]; then
        echo -e "  ${BOARD}: ${GREEN}${STATUS}${NC}"
        SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
    else
        echo -e "  ${BOARD}: ${RED}${STATUS}${NC}"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
done

echo ""
echo -e "${BLUE}Per-board timing breakdown:${NC}"
for i in "${!BUILD_RESULTS[@]}"; do
    BOARD_NAME="${BUILD_RESULTS[$i]%%:*}"
    RM_MS="${RM_TIMES[$i]#*:}"
    CONFIG_MS="${CONFIGURE_TIMES[$i]#*:}"
    COMPILE_MS="${COMPILE_TIMES[$i]#*:}"
    COPY_MS="${COPY_TIMES[$i]#*:}"
    TOTAL_MS="${TOTAL_BOARD_TIMES[$i]#*:}"
    echo -e "  ${BOARD_NAME}: clean $(format_ms "$RM_MS"), configure $(format_ms "$CONFIG_MS"), build $(format_ms "$COMPILE_MS"), copy $(format_ms "$COPY_MS"), total $(format_ms "$TOTAL_MS")"
done
echo ""
echo -e "${BLUE}Build parallelism used: ${BUILD_JOBS}${NC}"
echo -e "${BLUE}Total Time: ${TOTAL_TIME}s${NC}"
echo -e "${GREEN}Successful: ${SUCCESS_COUNT}${NC}"
echo -e "${RED}Failed: ${FAIL_COUNT}${NC}"
echo ""
echo -e "Report saved to: ${REPORT_FILE}"
echo -e "Test artifacts in: ${TESTS_DIR}"

# Exit with error if any builds failed
if [ $FAIL_COUNT -gt 0 ]; then
    exit 1
fi
