from router import request
from models.ollama import process_chat

@request("chat")
def handle(payload):
    text = payload["text"]

    return {
        "reply": process_chat(text)
    }