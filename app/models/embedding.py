from sentence_transformers import SentenceTransformer

@request("embedding")
def handle(payload):
    model = SentenceTransformer("all-MiniLM-L6-v2") # "multilingual-e5-small"

    text = payload["text"]

    vector = model.encode(text)

    return {
        "vector": vector.tolist()
    }
