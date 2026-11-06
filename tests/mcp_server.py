#!/usr/bin/env python3
"""A tiny MCP server, used to check that the port talks to one.

It speaks JSON-RPC over its standard input and output, and offers one tool,
one resource and one prompt.
"""

import json
import sys


def send(message):
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


def handle(request):
    method = request.get("method")
    params = request.get("params", {})

    if method == "initialize":
        return {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}, "resources": {}, "prompts": {}},
            "serverInfo": {"name": "test-server", "version": "1.0"},
        }
    if method == "tools/list":
        return {
            "tools": [
                {
                    "name": "echo",
                    "description": "Echoes the text it is given.",
                    "inputSchema": {
                        "type": "object",
                        "properties": {"text": {"type": "string"}},
                        "required": ["text"],
                    },
                }
            ]
        }
    if method == "tools/call":
        arguments = params.get("arguments", {})
        return {
            "content": [{"type": "text", "text": "echo: " + str(arguments.get("text", ""))}],
            "isError": False,
        }
    if method == "resources/list":
        return {
            "resources": [
                {
                    "uri": "test://hello",
                    "name": "hello",
                    "description": "A greeting.",
                    "mimeType": "text/plain",
                }
            ]
        }
    if method == "resources/read":
        return {"contents": [{"uri": "test://hello", "mimeType": "text/plain", "text": "hello world"}]}
    if method == "prompts/list":
        return {
            "prompts": [
                {
                    "name": "greet",
                    "description": "Greets someone.",
                    "arguments": [{"name": "who", "description": "Who to greet.", "required": True}],
                }
            ]
        }
    if method == "prompts/get":
        arguments = params.get("arguments", {})
        return {
            "messages": [
                {
                    "role": "user",
                    "content": {"type": "text", "text": "Hello, " + str(arguments.get("who", "there"))},
                }
            ]
        }
    return None


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except ValueError:
            continue

        identifier = request.get("id")
        if identifier is None:
            # A notification; the server has nothing to say about it.
            continue

        result = handle(request)
        if result is None:
            send({"jsonrpc": "2.0", "id": identifier, "error": {"code": -32601, "message": "Unknown method"}})
            continue
        send({"jsonrpc": "2.0", "id": identifier, "result": result})


if __name__ == "__main__":
    main()
