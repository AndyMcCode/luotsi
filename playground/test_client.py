
import subprocess
import json
import sys

def run_command(command, input_data=None):
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=0  # Unbuffered
    )

    if input_data:
        # Send input
        process.stdin.write(input_data + "\n")
        process.stdin.flush()

    # Read output line by line
    try:
        while True:
            line = process.stdout.readline()
            if not line:
                break
            print(f"Server: {line.strip()}")
            # Simple logic to proceed after receiving specific messages if needed
            # For this test, we might just want to send a sequence of commands
            
    except KeyboardInterrupt:
        process.terminate()


    # Command to run the docker container
    cmd = ["docker", "run", "-i", "--rm", "odoo-mcp"]
    
    process = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=sys.stderr,
        text=True
    )

    # 1. Initialize
    init_request = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": "2024-11-05", # Check version compatibility
            "capabilities": {},
            "clientInfo": {"name": "test-client", "version": "0.1"}
        }
    }
    
    print(f"Client: {json.dumps(init_request)}")
    process.stdin.write(json.dumps(init_request) + "\n")
    process.stdin.flush()

    # Read init response
    response_line = process.stdout.readline()
    print(f"Server: {response_line.strip()}")
    
    # 2. Initialized Notification
    init_notif = {
        "jsonrpc": "2.0",
        "method": "notifications/initialized"
    }
    print(f"Client: {json.dumps(init_notif)}")
    process.stdin.write(json.dumps(init_notif) + "\n")
    process.stdin.flush()

    # 3. List Tools
    tools_request = {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "tools/list"
    }
    print(f"Client: {json.dumps(tools_request)}")
    process.stdin.write(json.dumps(tools_request) + "\n")
    process.stdin.flush()

    # Read tools response
    # Note: Server might send log messages or other notifications, so we might need a loop
    # but for a simple test, we expect the response soon.
    
    while True:
        line = process.stdout.readline()
        if not line:
            break
        print(f"Server: {line.strip()}")
        try:
            msg = json.loads(line)
            if msg.get("id") == 2:
                print("Tools list received!")
                
                # 4. Call Tool (execute_method)
                tool_call = {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "tools/call",
                    "params": {
                        "name": "execute_method",
                        "arguments": {
                            "model": "res.partner",
                            "method": "search_read",
                            "args": [[], ["name", "email"]],
                            "kwargs": {"limit": 1}
                        }
                    }
                }
                print(f"Client: {json.dumps(tool_call)}")
                process.stdin.write(json.dumps(tool_call) + "\n")
                process.stdin.flush()
                
            if msg.get("id") == 3:
                print("Search result received!")
                result = msg.get("result", {}).get("structuredContent", {}).get("result", {}).get("result", [])
                print(f"Result: {result}")
                
                # 5. Call Tool: Create Partner
                print("\nCreating new partner...")
                create_call = {
                    "jsonrpc": "2.0",
                    "id": 4,
                    "method": "tools/call",
                    "params": {
                        "name": "execute_method",
                        "arguments": {
                            "model": "res.partner",
                            "method": "create",
                            "args": [{"name": "Test Partner MCP", "email": "test@example.com"}],
                        }
                    }
                }
                print(f"Client: {json.dumps(create_call)}")
                process.stdin.write(json.dumps(create_call) + "\n")
                process.stdin.flush()
                
            if msg.get("id") == 4:
                print("Create result received!")
                content = msg.get("result", {}).get("structuredContent", {}).get("result", {})
                new_id = content.get("result")
                print(f"Created Partner ID: {new_id}")
                
                if new_id:
                     # 6. Call Tool: Unlink Partner (Cleanup)
                    print("\nCleaning up (Unlink)...")
                    unlink_call = {
                        "jsonrpc": "2.0",
                        "id": 5,
                        "method": "tools/call",
                        "params": {
                            "name": "execute_method",
                            "arguments": {
                                "model": "res.partner",
                                "method": "unlink",
                                "args": [[new_id]],
                            }
                        }
                    }
                    print(f"Client: {json.dumps(unlink_call)}")
                    process.stdin.write(json.dumps(unlink_call) + "\n")
                    process.stdin.flush()
            
            if msg.get("id") == 5:
                print("Unlink result received!")
                print(f"Result: {msg}")

                # 7. List Resources
                print("\nListing Resources...")
                resources_request = {
                    "jsonrpc": "2.0",
                    "id": 6,
                    "method": "resources/list"
                }
                print(f"Client: {json.dumps(resources_request)}")
                process.stdin.write(json.dumps(resources_request) + "\n")
                process.stdin.flush()

            if msg.get("id") == 6:
                print("Resources list received!")
                print(f"Server: {line.strip()}")
                
                # 8. List Prompts
                print("\nListing Prompts...")
                prompts_request = {
                    "jsonrpc": "2.0",
                    "id": 7,
                    "method": "prompts/list"
                }
                print(f"Client: {json.dumps(prompts_request)}")
                process.stdin.write(json.dumps(prompts_request) + "\n")
                process.stdin.flush()

            if msg.get("id") == 7:
                print("Prompts list received!")
                print(f"Server: {line.strip()}")
                
                # 9. Read Resource (odoo://models)
                print("\nReading 'odoo://models'...")
                read_resource_request = {
                    "jsonrpc": "2.0",
                    "id": 8,
                    "method": "resources/read",
                    "params": {
                        "uri": "odoo://models"
                    }
                }
                print(f"Client: {json.dumps(read_resource_request)}")
                process.stdin.write(json.dumps(read_resource_request) + "\n")
                process.stdin.flush()

            if msg.get("id") == 8:
                print("Resource content received!")
                # The content might be huge, so let's just print a preview
                try:
                    content_list = msg.get("result", {}).get("contents", [])
                    if content_list:
                         text_content = content_list[0].get("text", "")
                         models = json.loads(text_content)
                         print(f"Total Models Found: {len(models)}")
                         print(f"First 5 Models: {models[:5]}")
                    else:
                        print(f"Server: {line.strip()}")
                except Exception as e:
                    print(f"Error parsing resource content: {e}")
                    print(f"Server: {line.strip()}")
                
                # 10. Call Tool: Get Product Categories
                print("\nFetching Product Categories...")
                cat_call = {
                    "jsonrpc": "2.0",
                    "id": 9,
                    "method": "tools/call",
                    "params": {
                        "name": "execute_method",
                        "arguments": {
                            "model": "product.category",
                            "method": "search_read",
                            "args": [[], ["name", "parent_id"]],
                            "kwargs": {"limit": 5}
                        }
                    }
                }
                print(f"Client: {json.dumps(cat_call)}")
                process.stdin.write(json.dumps(cat_call) + "\n")
                process.stdin.flush()

            if msg.get("id") == 9:
                print("Product Categories received!")
                result = msg.get("result", {}).get("structuredContent", {}).get("result", {}).get("result", [])
                print(f"Result: {result}")
                break
                
        except json.JSONDecodeError:
            pass
            
    # Terminate
    process.terminate()

if __name__ == "__main__":
    run_interactive_test()
