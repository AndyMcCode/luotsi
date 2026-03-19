FROM python:3.11-slim

WORKDIR /app

RUN pip install --no-cache-dir \
    langchain-google-genai \
    langchain-core \
    pydantic \
    python-dotenv

COPY agent.py .
COPY .env .
ENTRYPOINT ["python", "agent.py"]
