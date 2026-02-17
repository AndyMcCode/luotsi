FROM python:3.11-slim

WORKDIR /app

# Install dependencies from PyPI
RUN pip install --no-cache-dir odoo-mcp

# Copy config file
COPY odoo_config.json /app/odoo_config.json

# Set environment variable to use the config file
ENV ODOO_CONFIG_FILE=/app/odoo_config.json

# Entrypoint
CMD ["odoo-mcp"]
