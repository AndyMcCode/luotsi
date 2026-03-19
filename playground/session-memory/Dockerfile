FROM python:3.11-slim

WORKDIR /app

# No dependencies needed — pure Python JSON Lines storage

COPY session_memory.py .

ENTRYPOINT ["python", "-u", "session_memory.py"]
