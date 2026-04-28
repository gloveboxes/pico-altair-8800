# Script to generate build version header
# This script is executed at build time to increment the build number

# Read current build version
file(READ ${VERSION_FILE} BUILD_VERSION)
string(STRIP "${BUILD_VERSION}" BUILD_VERSION)

# Check if we should increment (don't increment if SKIP_VERSION_INCREMENT is set)
if(NOT DEFINED SKIP_VERSION_INCREMENT)
    # Increment build version
    math(EXPR NEW_VERSION "${BUILD_VERSION} + 1")
    
    # Write new version back to file
    file(WRITE ${VERSION_FILE} "${NEW_VERSION}")
    
    message(STATUS "Build version incremented: ${BUILD_VERSION} -> ${NEW_VERSION}")
else()
    set(NEW_VERSION "${BUILD_VERSION}")
    message(STATUS "Using existing build version: ${NEW_VERSION}")
endif()

# Get current date and time
string(TIMESTAMP BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP BUILD_TIME "%H:%M:%S")
string(TIMESTAMP BUILD_DATETIME "%Y-%m-%d %H:%M:%S")

# Generate the header file
configure_file(${TEMPLATE_FILE} ${OUTPUT_FILE} @ONLY)
