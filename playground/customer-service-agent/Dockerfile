FROM python:3.11-slim

WORKDIR /app

# Customer Service Agent might need langchain or other deps
# (Mocked for now so it's simple)
RUN pip install --no-cache-dir langchain-google-genai pydantic python-dotenv

COPY cs_agent.py .
COPY .env .
ENTRYPOINT ["python", "cs_agent.py"]
