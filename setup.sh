#!/bin/bash
# Purpose: Interactive UEFI Secure Boot MOK generation and module signing.
#
clear

echo "======================================================================"
echo "     Driver Acer Nitro V16 Secure Boot Setup (qapkyy_qy)              "
echo "======================================================================"
echo ""

if [ ! -f "qapkyy_qy.c" ] || [ ! -f "Makefile" ]; then
    echo "ERROR: Please run this script from the directory containing qapkyy_qy.c and Makefile."
    exit 1
fi

KEY_DIR="$HOME/module-signing"
echo "[Step 1/5] Creating Secure Boot MOK key directory..."
mkdir -p "$KEY_DIR"
echo "Directory created/verified at: $KEY_DIR"
echo ""

PRIV_KEY="$KEY_DIR/MOK.priv"
DER_KEY="$KEY_DIR/MOK.der"

if [ -f "$PRIV_KEY" ] && [ -f "$DER_KEY" ]; then
    echo "[Step 2/5] Existing Machine Owner Key (MOK) detected. Skipping generation."
    echo ""
else
    echo "[Step 2/5] Generating cryptographic MOK pair (RSA:2048, Valid: 100 years)..."
    openssl req -new -x509 -newkey rsa:2048 -keyout "$PRIV_KEY" -outform DER -out "$DER_KEY" \
        -nodes -days 36500 -subj "/CN=QapkyKey/"
    echo "Keys generated successfully under $KEY_DIR"
    echo ""
fi

echo "[Step 3/5] Building the qapkyy_qy kernel driver..."
make clean
make
echo "Driver binary (qapkyy_qy.ko) compiled successfully."
echo ""

echo "[Step 4/5] Locating kernel signing tool and signing the module..."
SIGN_FILE="/lib/modules/$(uname -r)/build/scripts/sign-file"

if [ ! -f "$SIGN_FILE" ]; then
    SIGN_FILE=$(find /usr/src/linux-headers-$(uname -r)/scripts/ -name "sign-file" 2>/dev/null | head -n 1)
fi

if [ -z "$SIGN_FILE" ] || [ ! -f "$SIGN_FILE" ]; then
    echo "CRITICAL: Kernel 'sign-file' script not found. Please ensure linux-headers/linux-devel packages are installed."
    exit 1
fi

echo "Using signing utility at: $SIGN_FILE"
sudo "$SIGN_FILE" sha256 "$PRIV_KEY" "$DER_KEY" "qapkyy_qy.ko"
echo "Success: qapkyy_qy.ko has been signed with QapkyKey MOK."
echo ""

echo "[Step 5/5] Launching automated systemd and driver deployment..."
sudo make install
echo "Systemd services, binary nodes, permissions, and module configs deployed successfully."
echo ""

echo "======================================================================"
echo "                    INTERACTIVE UEFI MOK ENROLLMENT                  "
echo "======================================================================"
echo "You will now be prompted by 'mokutil' to create a temporary password."
echo "IMPORTANT: Remember this password! You must type it right after rebooting your machine."
echo ""
read -p "Press [Enter] to initiate UEFI key enrollment..."

sudo mokutil --import "$DER_KEY"

echo ""
echo "======================================================================"
echo "                         SETUP PHASING COMPLETE                       "
echo "======================================================================"
echo "To complete the enrollment and activate your driver, you MUST REBOOT now."
echo ""
echo "Upon rebooting, your screen will display a blue UEFI MOK Manager menu:"
echo "  1. Select 'Enroll MOK'"
echo "  2. Select 'View key 0' (Optional: verify it says 'QapkyKey')"
echo "  3. Select 'Continue'"
echo "  4. Select 'Yes' to confirm enrollment"
echo "  5. Enter the Temporary Password you typed a moment ago"
echo "  6. Select 'Reboot'"
echo ""
echo "Once your system boots back up, your driver and systemd controllers will handle everything automatically!"
echo ""

read -p "Would you like to reboot your laptop now? (y/N): " choice
if [[ "$choice" =~ ^[Yy]$ ]]; then
    echo "Rebooting system..."
    sudo reboot
else
    echo "Please remember to reboot your laptop manually to finish loading the signed driver."
fi
