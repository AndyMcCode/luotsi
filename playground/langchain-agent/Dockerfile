FROM python:3.11-slim

WORKDIR /app

# Install dependencies required by the script
RUN pip install --no-cache-dir langchain-google-genai mcp langchain-core

# Copy the agent script
COPY agent.py /app/agent.py

# Unbuffered output is critical for stdio communication in Luotsi
ENV PYTHONUNBUFFERED=1

# Execute the agent
CMD ["python", "/app/agent.py"]
