#!/usr/bin/env bash

# Script to build Altair for all supported Pico boards
# Usage: ./build_all_boards.sh

set -e  # Exit on error

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

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
BOARDS=("pico" "pico_w" "pico2" "pico2_w" "pimoroni_pico_plus2_w_rp2350" "pico2_w_inky" "pico2_display28" "pico2_w_display28")

# Create tests directory
TESTS_DIR="${SCRIPT_DIR}/tests"
mkdir -p "$TESTS_DIR"

echo -e "${BLUE}=================================${NC}"
echo -e "${BLUE}Altair 8800 Multi-Board Build Test${NC}"
echo -e "${BLUE}=================================${NC}"
echo ""

# Track build results (using simple arrays as fallback for zsh compatibility)
BUILD_RESULTS=()
BUILD_TIMES=()
TOTAL_START=$(date +%s)

# Build for each board
BOARD_INDEX=0
for BOARD in "${BOARDS[@]}"; do
    echo -e "${YELLOW}Building for ${BOARD}...${NC}"
    
    BUILD_START=$(date +%s)
    
    # Clean and build (skip version increment since we did it once at the start)
    rm -rf build
    
    # Set CMake options based on board
    # Remove display suffixes from PICO_BOARD
    CLEAN_BOARD="${BOARD//_inky/}"
    CLEAN_BOARD="${CLEAN_BOARD//_display28/}"
    CMAKE_OPTS="-DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=${CLEAN_BOARD} -DSKIP_VERSION_INCREMENT=1"
    
    # Add display support flags
    if [[ "$BOARD" == *"_inky" ]]; then
        CMAKE_OPTS="$CMAKE_OPTS -DINKY_SUPPORT=ON -DDISPLAY_2_8_SUPPORT=OFF"
    elif [[ "$BOARD" == *"_display28" ]]; then
        CMAKE_OPTS="$CMAKE_OPTS -DINKY_SUPPORT=OFF -DDISPLAY_2_8_SUPPORT=ON"
    else
        CMAKE_OPTS="$CMAKE_OPTS -DINKY_SUPPORT=OFF -DDISPLAY_2_8_SUPPORT=OFF"
    fi
    
    if cmake -B build $CMAKE_OPTS && \
       cmake --build build -- -j; then
        
        BUILD_END=$(date +%s)
        BUILD_TIME=$((BUILD_END - BUILD_START))
        BUILD_TIMES[$BOARD_INDEX]="${BOARD}:${BUILD_TIME}"
        
        # Copy artifacts to tests directory
        BOARD_DIR="${TESTS_DIR}/${BOARD}"
        mkdir -p "$BOARD_DIR"
        
        if [ -f "build/altair.uf2" ]; then
            cp build/altair.uf2 "${BOARD_DIR}/altair_${BOARD}.uf2"
            cp build/altair.elf "${BOARD_DIR}/altair_${BOARD}.elf"
            cp build/altair.dis "${BOARD_DIR}/altair_${BOARD}.dis" 2>/dev/null || true
            
            # Get file size
            UF2_SIZE=$(du -h "${BOARD_DIR}/altair_${BOARD}.uf2" | cut -f1)
            
            BUILD_RESULTS[$BOARD_INDEX]="${BOARD}:✓ SUCCESS (${BUILD_TIME}s, ${UF2_SIZE})"
            echo -e "${GREEN}✓ Build successful for ${BOARD} (${BUILD_TIME}s)${NC}"
            echo -e "${GREEN}  Artifacts saved to: ${BOARD_DIR}/${NC}"
        else
            BUILD_RESULTS[$BOARD_INDEX]="${BOARD}:✗ FAILED - No UF2 generated"
            echo -e "${RED}✗ Build failed for ${BOARD} - No UF2 file generated${NC}"
        fi
    else
        BUILD_END=$(date +%s)
        BUILD_TIME=$((BUILD_END - BUILD_START))
        BUILD_RESULTS[$BOARD_INDEX]="${BOARD}:✗ FAILED (${BUILD_TIME}s)"
        echo -e "${RED}✗ Build failed for ${BOARD}${NC}"
    fi
    
    BOARD_INDEX=$((BOARD_INDEX + 1))
    echo ""
done

TOTAL_END=$(date +%s)
# Generate summary report
REPORT_FILE="${TESTS_DIR}/build_report.txt"
{
    echo "================================="
    echo "Altair 8800 Build Test Report"
    echo "================================="
    echo "Date: $(date)"
    echo "Total Time: ${TOTAL_TIME}s"
    echo ""
    echo "Build Results:"
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
