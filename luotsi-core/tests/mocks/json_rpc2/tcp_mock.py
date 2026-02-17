import asyncio
import json

async def handle_client(reader, writer):
    addr = writer.get_extra_info('peername')
    print(f"Connection from {addr}")

    try:
        while True:
            data = await reader.readline()
            if not data:
                break
            
            line = data.decode().strip()
            print(f"Received: {line}")

            try:
                rpc = json.loads(line)
                if rpc.get("method") == "luotsi.forward":
                    params = rpc.get("params")
                    source = params.get("source_id")
                    target = params.get("target_id")
                    payload = params.get("payload")

                    # Swap source/target for reply
                    reply = {
                        "jsonrpc": "2.0",
                        "method": "luotsi.forward",
                        "params": {
                            "source_id": target,
                            "target_id": source,
                            "payload": {
                                "method": "chat.reply",
                                "text": f"Echo: {payload.get('text', '')}"
                            }
                        }
                    }
                    writer.write((json.dumps(reply) + "\n").encode())
                    await writer.drain()
            except json.JSONDecodeError:
                print("Failed to decode JSON")
    except Exception as e:
        print(f"Error: {e}")
    finally:
        writer.close()
        await writer.wait_closed()
        print(f"Closed {addr}")

async def main():
    server = await asyncio.start_server(handle_client, '127.0.0.1', 8888)
    addr = server.sockets[0].getsockname()
    print(f"Serving on {addr}")

    async with server:
        await server.serve_forever()

if __name__ == "__main__":
    asyncio.run(main())
