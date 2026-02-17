#!/bin/bash
set -e

echo "Building Playground Images..."

echo "Building Odoo Mock..."
docker build -t luotsi-playground-odoo ./odoo

echo "Building WhatsApp Mock..."
docker build -t luotsi-playground-whatsapp ./whatsapp

echo "Done! Images built successfully."
