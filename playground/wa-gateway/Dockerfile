FROM python:3.11-slim

WORKDIR /app

RUN pip install --no-cache-dir requests

COPY . .

# Expose port 8090 for the host-side cloudflared to reach us
EXPOSE 8090

CMD ["python", "wa_gateway.py"]
