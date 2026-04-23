import sys
import os
import json
import logging
import asyncio
import threading
import dotenv
from typing import Dict, Any, List

from pydantic import BaseModel, Field
from langchain_google_genai import ChatGoogleGenerativeAI
from langchain_core.messages import HumanMessage, SystemMessage

# Pydantic schema for the Planner
class Plan(BaseModel):
    """A series of step-by-step instructions to fulfill the user's request."""
    steps: List[str] = Field(description="The sequential steps to execute.")

class ToolCall(BaseModel):
    """Determines whether to call a tool or read a resource to solve an execution aim."""
    action_type: str = Field(description="One of: 'tool_call', 'resource_read', or 'none'. Use 'tool_call' for tools/call, 'resource_read' for resources/read, 'none' if no external call needed.", default="none")
    tool_name: str = Field(description="The exact name of the tool to invoke (for tool_call).", default="")
    tool_arguments: dict = Field(description="The JSON dictionary of parameters matching the tool's inputSchema (for tool_call).", default={})
    resource_uri: str = Field(description="The URI to read (for resource_read), e.g. 'odoo://search/res.partner/[]'.", default="")
    reasoning_or_answer: str = Field(description="If action_type is 'none', provide the finding or plain text answer. Otherwise explain why you chose this action.")

logging.basicConfig(level=logging.INFO, stream=sys.stderr, 
                    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger("LuotsiAgent")

class MCPLangChainAgent:
    def __init__(self):
        self.llm = ChatGoogleGenerativeAI(
            model="gemini-2.5-flash",
            temperature=0.0
        )
        self.msg_id = 1
        self.tools_manifest = []
        self.resources_manifest = []
        self.templates_manifest = []
        
        # Threading infrastructure for synchronous tool calls
        self.pending_rpc_calls = {}

    def log(self, msg):
        logger.info(msg)

    def write_rpc(self, method: str, params: dict = None, request_id: int = None, delegated_role: str = None):
        """Write a JSON-RPC request to stdout to be routed by Luotsi."""
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None: msg["params"] = params
        if request_id is not None: msg["id"] = request_id
        if delegated_role: msg["__luotsi_role__"] = delegated_role
        
        json_str = json.dumps(msg, sort_keys=True)
        self.log(f"Sending via Luotsi Bus: {json_str}")
        print(json_str, flush=True)

    def dispatch_mcp_tool_sync(self, name: str, arguments: dict, override_method: str = "tools/call", delegated_role: str = None, timeout: float = 30.0) -> str:
        """Invoked by LangChain Executor. Sends tool call to Luotsi and waits for reply."""
        self.msg_id += 1
        req_id = self.msg_id
        
        event = threading.Event()
        response_bucket = {}
        self.pending_rpc_calls[req_id] = (event, response_bucket)
        
        params = {"name": name, "arguments": arguments} if override_method == "tools/call" else arguments
        
        self.write_rpc(override_method, params, req_id, delegated_role=delegated_role)
        
        self.log(f"Executor blocking on: {override_method} (req_id: {req_id})")
        event.wait(timeout=timeout)
        
        self.pending_rpc_calls.pop(req_id, None)
        
        # Parse MCP typical content returns
        if "content" in response_bucket:
            content = response_bucket["content"]
            if isinstance(content, list) and len(content) > 0 and "text" in content[0]:
                return str(content[0]["text"])
            return str(content)
        elif bool(response_bucket):
            return response_bucket # Return the whole response if it's not a content string
        else:
            return "Error: Tool execution timed out or failed."

    def process_user_message(self, user_msg: str, reply_id: int, delegated_role: str = None, session_id: str = None):
        """The core Plan-and-Execute agent logic."""
        self.log(f"--- Starting Plan-and-Execute Run for: '{user_msg}' ---")
        
        # 0. Proactive RAG (Memory Recall)
        self.log("Phase 0: Proactive RAG (Memory Recall)")
        memory_context = ""
        try:
            recall_args = {"query": user_msg}
            if session_id:
                recall_args["session_id"] = session_id
                
            recall_result = self.dispatch_mcp_tool_sync(
                "session_memory__get_recent_history", 
                recall_args, 
                delegated_role=delegated_role,
                timeout=10.0
            )
            
            if "RECENT HISTORY" in str(recall_result):
                memory_context = str(recall_result)
                self.log(f"Successfully recalled memory context (len={len(memory_context)})")
            else:
                self.log("No relevant memory context found.")
        except Exception as e:
            self.log(f"Memory recall failed (ignoring): {e}")

        # 1. PLAN
        self.log("Phase 1: Planning")
        planner = self.llm.with_structured_output(Plan)
        
        tool_descriptions = "\n".join([f"- {t['name']}: {t.get('description', 'No description')}" for t in self.tools_manifest])
        resource_descriptions = "\n".join([f"- {r.get('name', r.get('uri', ''))}: {r.get('description', 'No description')} (URI: {r.get('uri', 'N/A')})" for r in self.resources_manifest])
        template_descriptions = "\n".join([f"- {t.get('name', '')}: {t.get('description', 'No description')} (URI Template: {t.get('uriTemplate', 'N/A')})" for t in self.templates_manifest])
        memory_block = f"\nRECENT CONVERSATION CONTEXT (Use this to answer the user if applicable):\n{memory_context}\n" if memory_context else ""
        prompt = f"""
You are an expert planner. Create a step-by-step plan to answer the user's request.
{memory_block}
Available Tools (invoked via tools/call):
{tool_descriptions if tool_descriptions else '(none)'}

Available Resources (read via resources/read with a URI):
{resource_descriptions if resource_descriptions else '(none)'}

Available Resource Templates (read via resources/read, fill in the {{placeholders}} in the URI):
{template_descriptions if template_descriptions else '(none)'}

IMPORTANT: To query Odoo data you can either use 'execute_method' tool OR use a resource template like 'odoo://search/{{model_name}}/{{domain}}' via resources/read.
For example to list customers: resources/read with URI 'odoo://search/res.partner/[]' or 'odoo://search/res.partner/[["is_company","=",true]]'.
CRITICAL INSTRUCTION: If the RECENT CONVERSATION CONTEXT fully and specifically answers the User Request (e.g., it mentions the exact product brand or stock level requested), your plan should be a single step: 'Provide the answer directly to the user using the recent conversation context.' 

However, if the request asks for specific details (like a brand, price, or availability) that are NOT explicitly found in the context, you MUST plan the necessary Odoo tool/resource calls to get the live data. Do not guess or assume general information is sufficient for specific queries.

FINAL STEP REQUIREMENT: Your FINAL planned step must be to invoke the `cs_agent__reply` tool. 

CRITICAL: This step must explicitly instruct the executor to evaluate whether the previous actions were successful.

The final message MUST truthfully reflect the findings (e.g., "I've added the contact" vs "I was unable to add the contact because...").

User Request: {user_msg}
"""
        try:
            plan = planner.invoke([HumanMessage(content=prompt)])
            self.log(f"Generated Plan with {len(plan.steps)} steps:")
            for i, step in enumerate(plan.steps):
                self.log(f"  Step {i+1}: {step}")
        except Exception as e:
            self.log(f"Planner failed: {e}")
            self._send_reply("I encountered an internal error while planning.", reply_id)
            return

        # 2. EXECUTE
        self.log("Phase 2: Executing")
        scratchpad = []
        
        for i, step in enumerate(plan.steps):
            self.log(f"Executing Step {i+1}: {step}")
            
            exec_prompt = f"""
You are an execution engine. Your ONLY job is to complete the Aim below.
Aim: {step}

{memory_block}
Context of previous findings: {json.dumps(scratchpad)}

Available Tools (action_type='tool_call'):
{json.dumps([{ 'name': t['name'], 'description': t.get('description',''), 'schema': t.get('inputSchema', {}) } for t in self.tools_manifest], indent=2)}

Available Resource Templates (action_type='resource_read', fill in placeholders in the URI):
{json.dumps([{ 'name': t.get('name',''), 'description': t.get('description',''), 'uriTemplate': t.get('uriTemplate','') } for t in self.templates_manifest], indent=2)}

Available Resources (action_type='resource_read', use the URI directly):
{json.dumps([{ 'name': r.get('name',''), 'description': r.get('description',''), 'uri': r.get('uri','') } for r in self.resources_manifest], indent=2)}

Important Tool Instructions:
- If invoking 'cs_agent__reply', you MUST supply the original user message in the 'customer_message' argument so the agent has context. The user asked: "{user_msg}"
- You MUST also include the relevant findings from previous steps in the 'message' argument. 
- Use the recent conversation context to decide if you need to be apologizing for previous errors or referencing prior facts: {memory_block}

Decide how to satisfy the Aim:
- Use action_type='tool_call' and populate tool_name + tool_arguments to call a tool.
- Use action_type='resource_read' and populate resource_uri to read a resource (e.g. 'odoo://search/res.partner/[]').
- Use action_type='none' and populate reasoning_or_answer to provide a direct answer.
"""
            try:
                executor_llm = self.llm.with_structured_output(ToolCall)
                action = executor_llm.invoke([
                    SystemMessage(content="You are a strict execution environment. Analyze the aim, evaluate available context, and determine if an external tool invocation or resource read is required. IMPORTANT: If you encounter data fields with the value '[VALUE_OMITTED_DUE_TO_SIZE: ... bytes]', it means the field was too large for the context window. Ignore that specific field and proceed with the rest of the available data to satisfy the Aim."),
                    HumanMessage(content=exec_prompt)
                ])
                
                if action.action_type == "tool_call" and action.tool_name:
                    self.log(f"LLM decided to call tool: {action.tool_name} with args {action.tool_arguments}")
                    result = self.dispatch_mcp_tool_sync(action.tool_name, action.tool_arguments, delegated_role=delegated_role)
                    scratchpad.append({f"Step {i+1} Tool Result ({action.tool_name})": result})
                elif action.action_type == "resource_read" and action.resource_uri:
                    self.log(f"LLM decided to read resource: {action.resource_uri}")
                    result = self.dispatch_mcp_tool_sync("resources/read", {"uri": action.resource_uri}, override_method="resources/read", delegated_role=delegated_role)
                    scratchpad.append({f"Step {i+1} Resource ({action.resource_uri})": result})
                else:
                    self.log(f"LLM provided direct answer: {action.reasoning_or_answer}")
                    scratchpad.append({f"Step {i+1} Thought": action.reasoning_or_answer})
                    
            except Exception as e:
                self.log(f"Executor step failed: {e}")

        # 3. SYNTHESIZE
        self.log("Phase 3: Synthesizing Final Response")
        # Proactively detect errors in the scratchpad to prevent LLM hallucinations
        errors = [v for step in scratchpad for k, v in step.items() if "Error:" in str(v)]
        error_context = "\nCRITICAL: One or more steps failed. You MUST explain this failure to the user and NOT claim success.\n" + "\n".join(errors) if errors else ""
        
        synth_prompt = f"""
User Request: {user_msg}
{memory_block}

Execution Scratchpad (Steps taken and findings):
{json.dumps(scratchpad, indent=2)}
{error_context}

You are the final output layer. Compare the Execution Scratchpad results with any generated `cs_agent__reply`.
CRITICAL: If the scratchpad contains a "Tool Error" or "Error:", you MUST prioritize explaining that failure to the user.
Only output the `cs_agent__reply` content if it accurately reflects the tool results.
If the tool output claims success but the scratchpad shows a failure, ignore the tool output and generate your own apologetic response.
"""
        try:
            final_response = self.llm.invoke([HumanMessage(content=synth_prompt)])
            self._send_reply(final_response.content, reply_id)
            self.log("Final response emitted successfully.")
        except Exception as e:
            self.log(f"Synthesizer failed: {e}")
            self._send_reply("I encountered an error synthesizing the final response.", reply_id)

    def _send_reply(self, text, reply_id):
        reply = {
            "jsonrpc": "2.0",
            "result": {"status": "processed", "reply": text}
        }
        if reply_id is not None: reply["id"] = reply_id
        print(json.dumps(reply, sort_keys=True), flush=True)

    def read_loop(self):
        """Background thread that reads from stdin and fires callbacks/events."""
        self.log("Starting Stdin Background Thread...")
        for line in sys.stdin:
            line = line.strip()
            if not line: continue
            try:
                data = json.loads(line)
            except json.JSONDecodeError:
                continue
                
            # Handle Handshake & RPC Calls (result or error)
            if "id" in data and ("result" in data or "error" in data):
                req_id = data["id"]
                
                if req_id in self.pending_rpc_calls:
                    event, bucket = self.pending_rpc_calls[req_id]
                    if "result" in data:
                        bucket.update(data["result"])
                    elif "error" in data:
                        err = data["error"]
                        self.log(f"⚠️  Core returned error for req {req_id}: {err.get('message', err)}")
                        # Surface error as a tool result so the agent can react
                        bucket["error"] = err.get("message", "Unknown error from Luotsi Core")
                        bucket["isError"] = True
                    event.set()
                    continue

                if "result" not in data:
                    self.log(f"⚠️  Received response with no result (likely error) for untracked ID: {data}")
                    continue

                res = data["result"]
                if "protocolVersion" in res:
                    pass
                elif "tools" in res:
                    self.tools_manifest = res["tools"]
                    self.log(f"Successfully discovered {len(self.tools_manifest)} tools.")
                elif "resources" in res:
                    self.resources_manifest = res["resources"]
                    self.log(f"Successfully discovered {len(self.resources_manifest)} resources.")
                elif "resourceTemplates" in res:
                    self.templates_manifest = res["resourceTemplates"]
                    self.log(f"Successfully discovered {len(self.templates_manifest)} resource templates.")
            
            # Handle Incoming Users
            elif "method" in data and data["method"] == "messaging.incoming":
                user_msg = data.get("params", {}).get("body", "")
                session_id = data.get("params", {}).get("from", "")
                reply_id = data.get("id")
                delegated_role = data.get("__luotsi_role__")
                
                # Fire the sequence in a NEW thread so we don't block the stdin reader!!!
                exec_thread = threading.Thread(target=self.process_user_message, args=(user_msg, reply_id, delegated_role, session_id), daemon=True)
                exec_thread.start()

    async def run(self):
        """Entrypoint."""
        self.log("Initializing Gemini 2.5 Flash Agent...")
        
        # Start background reader thread
        reader_thread = threading.Thread(target=self.read_loop, daemon=True)
        reader_thread.start()
        
        # 0.5) Authenticate with Luotsi Core Policy Engine
        secret_key = os.environ.get("LUOTSI_SECRET_KEY", "super_secret_admin")
        self.log(f"Authenticating with Luotsi Hub using key: {secret_key}")
        auth_result = self.dispatch_mcp_tool_sync("luotsi/authenticate", {
            "secret_key": secret_key
        }, override_method="luotsi/authenticate")
        
        if isinstance(auth_result, dict) and "authenticated" in auth_result:
             self.log(f"Authentication Successful! Assigned Policy Role: {auth_result.get('role')}")
        else:
             self.log(f"Authentication Failed or Unrecognized Response: {auth_result}")
             sys.exit(1)
            
        self.log("Dispatching MCP Initialization request...")
        
        # 1. Initialize
        init_result = self.dispatch_mcp_tool_sync("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "luotsi-agent", "version": "1.0.0"}
        }, override_method="initialize")
        
        self.log("MCP Handshake successful! Emitting notifications/initialized.")
        self.write_rpc("notifications/initialized")
        
        # 2. Tools List
        self.log("Dispatching tool discovery request...")
        tools_result = self.dispatch_mcp_tool_sync("tools/list", {}, override_method="tools/list")
        if isinstance(tools_result, dict) and "tools" in tools_result:
             self.tools_manifest = tools_result["tools"]
        
        # 3. Resources List
        self.log("Dispatching resource discovery request...")
        res_result = self.dispatch_mcp_tool_sync("resources/list", {}, override_method="resources/list")
        if isinstance(res_result, dict) and "resources" in res_result:
            self.resources_manifest = res_result["resources"]
        
        # 4. Resource Templates List
        self.log("Dispatching resource templates discovery request...")
        tpl_result = self.dispatch_mcp_tool_sync("resources/templates/list", {}, override_method="resources/templates/list")
        if isinstance(tpl_result, dict) and "resourceTemplates" in tpl_result:
            self.templates_manifest = tpl_result["resourceTemplates"]
        
        self.log(f"=== Discovery Summary ===")
        self.log(f"  Tools:     {len(self.tools_manifest)} discovered")
        for t in self.tools_manifest:
            self.log(f"    - {t.get('name', '?')}")
        self.log(f"  Resources: {len(self.resources_manifest)} discovered")
        for r in self.resources_manifest:
            self.log(f"    - {r.get('name', r.get('uri', '?'))}")
        self.log(f"  Templates: {len(self.templates_manifest)} discovered")
        for t in self.templates_manifest:
            self.log(f"    - {t.get('name', '?')}: {t.get('uriTemplate', '?')}")
        self.log(f"==========================")
        
        # Keep main thread alive
        while True:
            await asyncio.sleep(1)

if __name__ == "__main__":
    dotenv.load_dotenv()
    if not os.getenv("GOOGLE_API_KEY"):
        logger.error("GOOGLE_API_KEY environment variable is missing!")
        sys.exit(1)
    
    agent = MCPLangChainAgent()
    asyncio.run(agent.run())
