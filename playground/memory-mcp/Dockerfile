FROM python:3.11-slim

WORKDIR /app

# Install dependencies for chroma
RUN pip install --no-cache-dir chromadb

COPY memory_mcp.py .

ENTRYPOINT ["python", "-u", "memory_mcp.py"]
