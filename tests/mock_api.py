#!/usr/bin/env python3
"""A mock of the Anthropic messages API.

The tests of the drawing need an answer to draw. This server speaks the
stream of the Anthropic API and hands out the text it is given, so that the
port can be driven through a pty without a network.

    python3 tests/mock_api.py --port 8123 --text "Hello there."

and the port is pointed at it with `ANTHROPIC_BASE_URL=http://127.0.0.1:8123`.
"""

import argparse
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# The text of the next answer. The tests set it before they drive the port.
#
# When a tool is named, the first answer is the call of that tool and the
# answers that follow it are the text: the port asks for the tool again
# once the result of the tool is in the conversation.
ANSWER = {"text": "Hello there.", "tool": None, "delay": 0.0, "echo": False,
          "classify": None, "chunk_delay": 0.0}


def sse(event, data):
    """One event of the stream of the API."""
    return f"event: {event}\ndata: {json.dumps(data)}\n\n".encode()


class Handler(BaseHTTPRequestHandler):
    # The API answers with a stream of events, the way a server of the API
    # does: the body comes in pieces of the encoding of the chunks.
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def begin_stream(self):
        """Begins the answer of the API."""
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

    def write(self, data):
        """Writes a piece of the stream."""
        self.wfile.write(b"%x\r\n" % len(data) + data + b"\r\n")
        self.wfile.flush()

    def end_stream(self):
        """Ends the answer of the API."""
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def answer_with_tool(self, tool):
        """Answers with the call of a tool."""
        self.begin_stream()

        self.write(sse("message_start", {
            "type": "message_start",
            "message": {
                "id": "msg_mock",
                "type": "message",
                "role": "assistant",
                "model": "claude-sonnet-4-5-20250929",
                "content": [],
                "stop_reason": None,
                "stop_sequence": None,
                "usage": {"input_tokens": INPUT_TOKENS, "output_tokens": 1},
            },
        }))
        self.write(sse("content_block_start", {
            "type": "content_block_start", "index": 0,
            "content_block": {"type": "tool_use", "id": "toolu_1",
                              "name": tool["name"], "input": {}},
        }))

        # The arguments of the tool arrive in pieces, the way they arrive
        # from the API.
        partial = json.dumps(tool.get("input", {}))
        for piece in range(0, len(partial), 20):
            self.write(sse("content_block_delta", {
                "type": "content_block_delta", "index": 0,
                "delta": {"type": "input_json_delta", "partial_json": partial[piece:piece + 20]},
            }))

        self.write(sse("content_block_stop", {"type": "content_block_stop", "index": 0}))
        self.write(sse("message_delta", {
            "type": "message_delta",
            "delta": {"stop_reason": "tool_use", "stop_sequence": None},
            "usage": {"output_tokens": 20},
        }))
        self.write(sse("message_stop", {"type": "message_stop"}))
        self.end_stream()

    def echo_of(self, body):
        """The results of the tools the session sent, as text."""
        try:
            request = json.loads(body)
        except ValueError:
            return "the body was not json"

        answers = []
        for message in request.get("messages", []):
            content = message.get("content")
            if not isinstance(content, list):
                continue
            for block in content:
                if not isinstance(block, dict):
                    continue
                if block.get("type") != "tool_result":
                    continue
                text = block.get("content")
                if isinstance(text, str):
                    answers.append(text)
                elif isinstance(text, list):
                    for piece in text:
                        if isinstance(piece, dict) and piece.get("type") == "text":
                            answers.append(piece.get("text", ""))
        return " | ".join(answers) if answers else "nothing came back"

    def classify_of(self, body):
        """The answer of the classifier of the auto mode, when it is asked."""
        try:
            request = json.loads(body)
        except ValueError:
            return None

        tools = request.get("tools")
        if not isinstance(tools, list):
            return None
        for tool in tools:
            if isinstance(tool, dict) and tool.get("name") == "classify_result":
                return {"name": "classify_result",
                        "input": ANSWER.get("classify") or {
                            "thinking": "The action is ordinary.",
                            "shouldBlock": False,
                            "reason": "The action is safe."}}
        return None

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)

        # The classifier of the auto mode is asked with a request of its
        # own: the answer is the call of its tool.
        classification = self.classify_of(body)
        if classification is not None:
            self.answer_with_tool(classification)
            return

        # The tool is called once: the answers that follow it carry the
        # result of the tool in the conversation.
        tool = ANSWER.get("tool")
        if tool and b"tool_result" not in body:
            self.answer_with_tool(tool)
            return

        text = ANSWER["text"]
        if ANSWER.get("echo"):
            # The answer is what the session sent back: the results of the
            # tools, so that a test can see what the port asked.
            text = self.echo_of(body)
        self.begin_stream()

        self.write(sse("message_start", {
            "type": "message_start",
            "message": {
                "id": "msg_mock",
                "type": "message",
                "role": "assistant",
                "model": "claude-sonnet-4-5-20250929",
                "content": [],
                "stop_reason": None,
                "stop_sequence": None,
                "usage": {"input_tokens": INPUT_TOKENS, "output_tokens": 1},
            },
        }))
        self.write(sse("content_block_start", {
            "type": "content_block_start", "index": 0,
            "content_block": {"type": "text", "text": ""},
        }))

        # The answer takes a while, the way a real one does.
        delay = ANSWER.get("delay") or 0.0
        if delay:
            time.sleep(delay)

        # The text arrives in pieces, the way it arrives from the API.
        chunk_delay = ANSWER.get("chunk_delay") or 0.0
        for piece in text.split(" "):
            self.write(sse("content_block_delta", {
                "type": "content_block_delta", "index": 0,
                "delta": {"type": "text_delta", "text": piece + " "},
            }))
            # The pieces may come one after the other, so that the answer
            # streams while the test watches it.
            if chunk_delay:
                time.sleep(chunk_delay)

        self.write(sse("content_block_stop", {"type": "content_block_stop", "index": 0}))
        self.write(sse("message_delta", {
            "type": "message_delta",
            "delta": {"stop_reason": "end_turn", "stop_sequence": None},
            "usage": {"output_tokens": 20},
        }))
        self.write(sse("message_stop", {"type": "message_stop"}))
        self.end_stream()


INPUT_TOKENS = 10


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8123)
    parser.add_argument("--input-tokens", type=int, default=10,
                        help="the input tokens the answer reports, so that the context "
                             "of the session can be made large in a test")
    parser.add_argument("--text", default="Hello there.")
    parser.add_argument("--tool", default="",
                        help="the name of a tool the answer calls")
    parser.add_argument("--tool-input", default="{}",
                        help="the arguments of the tool")
    parser.add_argument("--delay", type=float, default=0.0,
                        help="the seconds the answer takes")
    parser.add_argument("--chunk-delay", type=float, default=0.0,
                        help="the seconds between the pieces of the answer")
    parser.add_argument("--echo", action="store_true",
                        help="the answer is what the session sent back")
    parser.add_argument("--classify-block", action="store_true",
                        help="the classifier of the auto mode blocks the action")
    args = parser.parse_args()
    global INPUT_TOKENS
    INPUT_TOKENS = args.input_tokens

    ANSWER["classify"] = {
        "thinking": "The action is ordinary.",
        "shouldBlock": bool(args.classify_block),
        "reason": "The action is safe." if not args.classify_block
                  else "The action leaves the folder of the session.",
    }
    ANSWER["text"] = args.text
    ANSWER["delay"] = args.delay
    ANSWER["chunk_delay"] = args.chunk_delay
    ANSWER["echo"] = args.echo
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    if args.tool:
        ANSWER["tool"] = {"name": args.tool, "input": json.loads(args.tool_input)}

    print(f"the mock API listens on http://127.0.0.1:{args.port}", flush=True)
    try:
        thread.join()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
