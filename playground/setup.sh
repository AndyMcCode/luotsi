# Navigate to the script's directory to ensure relative paths work
cd "$(dirname "$0")"

echo "Building Playground Images..."

echo "Building Odoo Mock..."
docker build -t luotsi-playground-odoo ./odoo

echo "Building WhatsApp Mock..."
docker build -t luotsi-playground-whatsapp ./whatsapp

echo "Building Session Memory..."
docker build -t luotsi-session-memory ./session-memory

echo "Building Memory MCP..."
docker build -t luotsi-memory-mcp ./memory-mcp

echo "Building CS Agent..."
docker build -t luotsi-cs-agent ./customer-service-agent

echo "Building Master Mock..."
docker build -t luotsi-master-mock ./langchain-agent

echo "Building Dummy MCP..."
docker build -t luotsi-dummy-mcp ./dummy-mcp

echo "Done! All images built successfully."
