#!/bin/bash

# Script to create a symbolic link to the LWM2M models directory
# This allows the webapp to access the XML model files from src/main/resources/models

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
WEBAPP_DIR="$SCRIPT_DIR"
MODELS_SOURCE="../src/main/resources/models"
MODELS_LINK="$WEBAPP_DIR/models"

echo "Setting up LWM2M models link..."
echo "Source: $MODELS_SOURCE"
echo "Target: $MODELS_LINK"

# Remove existing link or directory if it exists
if [ -L "$MODELS_LINK" ]; then
    echo "Removing existing symbolic link..."
    rm "$MODELS_LINK"
elif [ -d "$MODELS_LINK" ]; then
    echo "Warning: A real directory exists at $MODELS_LINK"
    echo "Please remove it manually if you want to create a symlink"
    exit 1
fi

# Create symbolic link
if ln -s "$MODELS_SOURCE" "$MODELS_LINK"; then
    echo "✓ Symbolic link created successfully!"
    echo "You can now access LWM2M models from the webapp"
    
    # Count XML files
    XML_COUNT=$(find "$MODELS_LINK" -name "*.xml" 2>/dev/null | wc -l)
    echo "Found $XML_COUNT XML model files"
else
    echo "✗ Failed to create symbolic link"
    exit 1
fi
